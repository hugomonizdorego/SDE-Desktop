// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Liria's dialogs, drawn in the SDE theme like the rest of the window:
// questions before an operation (delete for good, empty the Trash, an item
// whose destination exists), Properties (name, type, location, size
// counted in the background, dates, permissions, the application it opens
// with), Open with (the applications for the type, made the default when
// asked), Liria's own sign-in for network places (the keyring offered only
// when one runs; a warning for FTP and WebDAV without TLS), GVfs's
// questions (an unknown SSH host key) and Connect to Server.
#define FL_INTERNALS 1
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <gio/gio.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

#include "desktop_model.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "location.h"
#include "menu.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "x11.h"

namespace sde {
namespace liria {

namespace {

// Text wrapped to |width| pixels (the font must be set).
std::vector<std::string> wrap(const std::string &text, int width) {
  std::vector<std::string> out;
  for (const auto &para : split(text, '\n', true)) {
    std::string line;
    for (const auto &word : split(para, ' ')) {
      std::string next = line.empty() ? word : line + " " + word;
      if (!line.empty() && text_width(next) > width) {
        out.push_back(line);
        line = word;
      } else {
        line = next;
      }
    }
    out.push_back(line);
  }
  return out;
}

void (*g_prev_before_map)(Fl_Window *, ::Window) = nullptr;
Fl_Window *g_mapping = nullptr;
::Window g_transient = 0;

void dialog_before_map(Fl_Window *win, ::Window xid) {
  if (g_prev_before_map) g_prev_before_map(win, xid);
  if (win != g_mapping) return;
  if (g_transient) XSetTransientForHint(fl_display, xid, g_transient);
  Atom type = XInternAtom(fl_display, "_NET_WM_WINDOW_TYPE", False);
  Atom dialog = XInternAtom(fl_display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  XChangeProperty(fl_display, xid, type, XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char *>(&dialog), 1);
}

// A rounded text field.
class Field : public Fl_Input {
 public:
  Field(int x, int y, int w, int h, bool secret = false) : Fl_Input(x, y, w, h) {
    if (secret) type(FL_SECRET_INPUT);
    box(FL_NO_BOX);
  }
  std::string placeholder;
  void draw() override {
    const Theme &t = active_theme();
    bool focused = Fl::focus() == this;
    {
      Painter p(window()->w(), window()->h());
      double r = t.radius_for(h());
      p.fill_rounded(x(), y(), w(), h(), r, t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, focused ? t.accent : t.border, focused ? Sd(1.6) : 1.0);
    }
    color(t.surface.fl());
    textcolor(t.text.fl());
    selection_color(t.accent.fl());
    cursor_color(t.accent.fl());
    textfont(FL_HELVETICA);
    textsize(t.font_size);
    drawtext(x() + S(10), y() + S(3), w() - S(20), h() - S(6));
    if (size() == 0 && !focused && !placeholder.empty()) {
      text_font(t.font_size);
      draw_text(placeholder, x() + S(11), y(), w() - S(22), h(), t.text_dim);
    }
  }
  int handle(int e) override {
    if (e == FL_FOCUS || e == FL_UNFOCUS) redraw();
    int ox = x(), ow = w();
    resize(ox + S(10), y(), ow - S(20), h());
    int r = Fl_Input::handle(e);
    resize(ox, y(), ow, h());
    return r;
  }
};

// The base of Liria's dialogs: a heading, a body drawn by |paint| and
// |text|, and buttons; Escape answers 0, Enter the default button.
class Dialog : public BufferedWindow {
 public:
  Dialog(MainWindow *parent, int w, int h, const std::string &title)
      : BufferedWindow(w, h), parent_(parent) {
    copy_label(title.c_str());
    xclass("liria");
    color(active_theme().surface.fl());
    callback([](Fl_Widget *w, void *) { static_cast<Dialog *>(w)->finish(0); });
  }
  std::string heading, body;
  std::vector<std::string> icons;
  std::function<void(Painter &)> paint;
  std::function<void()> text;
  int result = 0, default_result = 1;
  bool done = false;

  void finish(int r) {
    result = r;
    done = true;
    hide();
  }
  FlatButton *button(const std::string &label, int r, bool primary = false) {
    auto *b = new FlatButton(0, 0, 10, 10, label, primary);
    b->copy_label(label.c_str());
    b->user_data(reinterpret_cast<void *>(intptr_t(r)));
    b->callback([](Fl_Widget *w, void *d) {
      static_cast<Dialog *>(w->window())->finish(int(reinterpret_cast<intptr_t>(d)));
    }, reinterpret_cast<void *>(intptr_t(r)));
    buttons_.push_back(b);
    return b;
  }
  // Puts the buttons in a row at the bottom right.
  void place_buttons() {
    const Theme &t = active_theme();
    int bx = w() - S(20), by = h() - S(20) - S(34);
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it) {
      FlatButton *b = *it;
      text_font(t.font_size, true);
      int bw = std::max(S(96), text_width(b->label() ? b->label() : "") + S(36));
      bx -= bw;
      b->resize(bx, by, bw, S(34));
      bx -= S(8);
    }
  }
  int run() {
    end();
    apply_stock_style(this);
    place_buttons();
    // Over the window it belongs to.
    if (parent_ && parent_->shown()) {
      position(parent_->x() + (parent_->w() - w()) / 2, parent_->y() + std::max(S(40), (parent_->h() - h()) / 3));
      g_transient = fl_xid(parent_);
    } else {
      int sx, sy, sw, sh;
      Fl::screen_work_area(sx, sy, sw, sh);
      position(sx + (sw - w()) / 2, sy + (sh - h()) / 3);
      g_transient = 0;
    }
    set_modal();
    g_prev_before_map = Fl_X::before_map;
    Fl_X::before_map = dialog_before_map;
    g_mapping = this;
    show();
    Fl_X::before_map = g_prev_before_map;
    g_mapping = nullptr;
    if (!buttons_.empty() && !focus_set_) buttons_.back()->take_focus();
    while (!done && shown()) Fl::wait();
    return result;
  }
  void focus_later(Fl_Widget *w) {
    focus_set_ = true;
    Fl::add_timeout(0.0, [](void *d) { static_cast<Fl_Widget *>(d)->take_focus(); }, w);
  }
  void draw() override {
    const Theme &t = active_theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      if (!icons.empty()) draw_icon(p, icons, S(22), S(22), S(40));
      if (paint) paint(p);
    }
    int tx = icons.empty() ? S(24) : S(78);
    int tw = w() - tx - S(24);
    int ty = S(22);
    if (!heading.empty()) {
      text_font(t.font_size + 3, true);
      for (const auto &l : wrap(heading, tw)) {
        draw_text(l, tx, ty, tw, S(28), t.text);
        ty += S(28);
      }
      ty += S(6);
    }
    if (!body.empty()) {
      text_font(t.font_size);
      for (const auto &l : wrap(body, tw)) {
        draw_text(l, tx, ty, tw, S(22), t.text_dim);
        ty += S(22);
      }
    }
    body_end = ty;
    if (text) text();
    draw_children();
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD || e == FL_SHORTCUT) {
      int key = Fl::event_key();
      if (key == FL_Escape) {
        finish(0);
        return 1;
      }
      if ((key == FL_Enter || key == FL_KP_Enter) && !dynamic_cast<Fl_Choice *>(Fl::focus())) {
        if (FlatButton *b = dynamic_cast<FlatButton *>(Fl::focus())) {
          b->do_callback();
          return 1;
        }
        finish(default_result);
        return 1;
      }
    }
    return BufferedWindow::handle(e);
  }
  int body_end = 0;
  int text_height(const std::string &s, int width, int line_h) {
    text_font(active_theme().font_size);
    return int(wrap(s, width).size()) * line_h;
  }

 private:
  MainWindow *parent_;
  std::vector<FlatButton *> buttons_;
  bool focus_set_ = false;
};

std::string date_text(time_t t) {
  if (t <= 0) return "";
  char buf[64];
  struct tm tm {};
  localtime_r(&t, &tm);
  strftime(buf, sizeof buf, "%x %X", &tm);
  return buf;
}

std::string mode_text(mode_t m, bool dir) {
  auto part = [&](int r, int w, int x) {
    bool cr = m & r, cw = m & w, cx = m & x;
    if (dir) {
      if (cr && cw && cx) return std::string(_("Create and delete files"));
      if (cr && cx) return std::string(_("Access files"));
      if (cr) return std::string(_("List files only"));
      return std::string(_("None"));
    }
    if (cr && cw) return std::string(_("Read and write"));
    if (cr) return std::string(_("Read-only"));
    return std::string(_("None"));
  };
  return part(S_IRUSR, S_IWUSR, S_IXUSR) + "\n" + part(S_IRGRP, S_IWGRP, S_IXGRP) + "\n" + part(S_IROTH, S_IWOTH, S_IXOTH);
}

}  // namespace

// ------------------------------------------------------------------ confirm

bool confirm(MainWindow *parent, const std::string &title, const std::string &text, const std::string &yes,
             bool destructive) {
  text_font(active_theme().font_size);
  Dialog d(parent, S(480), S(200), title);
  d.heading = title;
  d.body = text;
  d.icons = destructive ? std::vector<std::string>{"dialog-warning"} : std::vector<std::string>{"dialog-question"};
  int lines = int(wrap(text, S(480) - S(78) - S(24)).size());
  d.size(S(480), S(22) + S(34) + lines * S(22) + S(24) + S(34) + S(20) + S(8));
  d.begin();
  d.button(_("Cancel"), 0);
  d.button(yes, 1, true);
  d.default_result = 1;
  return d.run() == 1;
}

// ------------------------------------------------------------------ clashes

Answer ask_clash(MainWindow *parent, const Clash &c, bool &all, Transfer kind) {
  std::string name = basename_of(c.dst);
  bool folders = c.src_dir && c.dst_dir;
  std::string title = folders ? format(_("This folder already has a folder named “%s”"), name.c_str())
                              : format(_("This folder already has an item named “%s”"), name.c_str());
  Dialog d(parent, S(560), S(340), _("Replace or skip"));
  d.heading = title;
  d.body = folders ? _("Merge puts the items of both folders together; items with the same name are asked about one by one.")
                   : _("Replace it with the one you are copying or moving, keep both, or skip it?");
  d.icons = {"dialog-question"};
  time_t now = time(nullptr);
  d.text = [&d, &c, now] {
    const Theme &t = active_theme();
    int y = d.body_end + S(10);
    auto row = [&](const std::string &label, bool dir, long long size, time_t mtime) {
      text_font(t.font_size, true);
      draw_text(label, S(78), y, S(200), S(22), t.text);
      text_font(t.font_size);
      std::string detail = (dir ? std::string(_("Folder")) : chooser::size_text(size)) + " · " +
                           chooser::time_text(mtime, now);
      draw_text(detail, S(78), y + S(20), d.w() - S(100), S(22), t.text_dim);
      y += S(48);
    };
    row(_("The new one"), c.src_dir, c.src_size, c.src_mtime);
    row(_("The one there"), c.dst_dir, c.dst_size, c.dst_mtime);
  };
  d.begin();
  auto *every = new Fl_Check_Button(S(74), S(340) - S(20) - S(34) - S(34), S(360), S(28), _("Do this for all items"));
  every->labelsize(active_theme().font_size);
  d.button(_("Cancel"), 0);
  d.button(_("Skip"), 3);
  d.button(_("Keep both"), 2);
  if (folders && kind != Transfer::Link) d.button(_("Merge"), 4, true);
  d.button(_("Replace"), 1, !folders);
  d.default_result = folders ? 4 : 1;
  int r = d.run();
  all = every->value() != 0;
  switch (r) {
    case 1: return Answer::Replace;
    case 2: return Answer::KeepBoth;
    case 3: return Answer::Skip;
    case 4: return Answer::Merge;
    default: return Answer::Cancel;
  }
}

// ------------------------------------------------------------------ properties

namespace {

struct SizeCount {
  std::atomic<bool> done{false}, stop{false};
  std::atomic<long long> bytes{0};
  std::atomic<long> files{0}, folders{0};
};

struct Ticker {
  Fl_Window *d;
  std::shared_ptr<SizeCount> c;
};

void props_tick(void *p) {
  auto *tk = static_cast<Ticker *>(p);
  tk->d->redraw();
  if (!tk->c->done) Fl::repeat_timeout(0.25, props_tick, p);
}

void count_tree(const std::string &path, SizeCount &c, bool top) {
  if (c.stop) return;
  struct stat st {};
  if (lstat(path.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    if (!top) c.folders++;
    for (const auto &n : list_dir(path)) {
      if (n == "." || n == "..") continue;
      count_tree(path == "/" ? "/" + n : path + "/" + n, c, false);
      if (c.stop) return;
    }
  } else {
    c.files++;
    if (S_ISREG(st.st_mode)) c.bytes += st.st_size;
  }
}

}  // namespace

void show_properties(MainWindow *parent, const std::vector<Item> &items) {
  if (items.empty()) return;
  const Item &e = items[0];
  bool single = items.size() == 1;
  App &app = App::get();
  std::string title = single ? format(_("%s Properties"), e.display.c_str()) : std::string(_("Properties"));
  auto counter = std::make_shared<SizeCount>();
  std::vector<std::string> paths;
  for (const auto &i : items) paths.push_back(i.path);
  bool any_dir = std::any_of(items.begin(), items.end(), [](const Item &i) { return i.dir; });
  bool local = std::all_of(items.begin(), items.end(), [](const Item &i) { return is_local(i.path); });
  if (local) {
    std::thread([counter, paths] {
      for (const auto &p : paths) count_tree(p, *counter, true);
      counter->done = true;
    }).detach();
  } else {
    counter->done = true;
  }
  struct stat st {};
  bool have_stat = single && local && lstat(e.path.c_str(), &st) == 0;
  struct stat target {};
  if (have_stat && S_ISLNK(st.st_mode)) stat(e.path.c_str(), &target);
  std::vector<std::pair<std::string, std::string>> rows;
  if (single) {
    rows.push_back({_("Type"), parent->type_text(e.mime)});
    if (parent->tab().location == kTrash) {
      rows.push_back({_("Original location"), e.original.empty() ? std::string(_("Unknown")) : display_path(chooser::parent_dir(e.original))});
      rows.push_back({_("Date deleted"), date_text(e.mtime)});
    } else {
      rows.push_back({_("Location"), display_path(chooser::parent_dir(e.path))});
    }
  } else {
    rows.push_back({_("Items"), format(_("%d items"), int(items.size()))});
    rows.push_back({_("Location"), display_path(chooser::parent_dir(e.path))});
  }
  int size_row = int(rows.size());
  rows.push_back({_("Size"), ""});
  if (any_dir) rows.push_back({_("Contains"), ""});
  if (have_stat) {
    if (S_ISLNK(st.st_mode)) {
      std::vector<char> buf(4096);
      ssize_t n = readlink(e.path.c_str(), buf.data(), buf.size() - 1);
      if (n > 0) rows.push_back({_("Link to"), std::string(buf.data(), size_t(n))});
    }
    rows.push_back({_("Modified"), date_text(st.st_mtime)});
    rows.push_back({_("Accessed"), date_text(st.st_atime)});
    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);
    rows.push_back({_("Owner"), pw ? pw->pw_name : std::to_string(st.st_uid)});
    rows.push_back({_("Group"), gr ? gr->gr_name : std::to_string(st.st_gid)});
  }
  int count_row = any_dir ? size_row + 1 : -1;
  std::string opens;
  std::string def;
  if (single && !e.dir) {
    def = app.apps.default_for(e.mime);
    if (const MimeApp *a = app.apps.find(def)) opens = a->name;
    else opens = _("No application");
  }
  int extra = have_stat ? S(120) : 0;
  if (!opens.empty()) extra += S(48);
  int H = S(110) + int(rows.size()) * S(30) + extra + S(70);
  Dialog d(parent, S(500), H, title);
  d.heading = single ? e.display : format(_("%d items"), int(items.size()));
  d.icons = single ? parent->icon_names(e) : std::vector<std::string>{"edit-copy", "folder"};
  int perm_y = S(84) + int(rows.size()) * S(30) + S(10);
  d.paint = [&](Painter &p) {
    const Theme &t = active_theme();
    p.line(S(24), S(76) + 0.5, d.w() - S(24), S(76) + 0.5, t.border);
    if (have_stat) p.line(S(24), perm_y - S(6) + 0.5, d.w() - S(24), perm_y - S(6) + 0.5, t.border);
  };
  d.text = [&] {
    const Theme &t = active_theme();
    // The size, as far as it is counted.
    std::string size = chooser::size_text(counter->bytes) + format(_(" (%s bytes)"), thousands(counter->bytes).c_str());
    if (!counter->done) size = format(_("%s, counting…"), chooser::size_text(counter->bytes).c_str());
    if (!local) size = single && !e.dir ? chooser::size_text(e.size) : std::string(_("Unknown"));
    rows[size_t(size_row)].second = size;
    if (count_row >= 0)
      rows[size_t(count_row)].second = format(_("%s files, %s folders"), thousands(counter->files).c_str(),
                                              thousands(counter->folders).c_str());
    int y = S(86);
    for (const auto &r : rows) {
      text_font(t.font_size);
      draw_text(r.first, S(24), y, S(140), S(28), t.text_dim);
      draw_text(r.second, S(170), y, d.w() - S(194), S(28), t.text);
      y += S(30);
    }
    if (have_stat) {
      int py = perm_y;
      text_font(t.font_size, true);
      draw_text(_("Permissions"), S(24), py, S(200), S(26), t.text);
      py += S(28);
      std::vector<std::string> who = {_("Owner"), _("Group"), _("Others")};
      std::vector<std::string> what = split(mode_text(st.st_mode, S_ISDIR(st.st_mode)), '\n');
      for (size_t i = 0; i < who.size() && i < what.size(); ++i) {
        text_font(t.font_size);
        draw_text(who[i], S(24), py, S(140), S(26), t.text_dim);
        draw_text(what[i], S(170), py, d.w() - S(194), S(26), t.text);
        py += S(26);
      }
    }
    if (!opens.empty()) {
      text_font(t.font_size);
      draw_text(_("Opens with"), S(24), d.h() - S(20) - S(34) - S(46), S(140), S(30), t.text_dim);
      draw_text(opens, S(170), d.h() - S(20) - S(34) - S(46), d.w() - S(300), S(30), t.text);
    }
  };
  d.begin();
  Fl_Check_Button *exec = nullptr;
  if (have_stat && S_ISREG(st.st_mode) && st.st_uid == getuid()) {
    exec = new Fl_Check_Button(S(166), perm_y + S(28) + 3 * S(26), S(300), S(26), _("Allow running it as a program"));
    exec->labelsize(active_theme().font_size);
    exec->value((st.st_mode & S_IXUSR) != 0);
  }
  if (!opens.empty()) {
    Item copy = e;
    auto *change = new FlatButton(d.w() - S(24) - S(110), d.h() - S(20) - S(34) - S(46), S(110), S(30), _("Change…"));
    change->copy_label(_("Change…"));
    change->callback([](Fl_Widget *w, void *data) {
      auto *dlg = static_cast<Dialog *>(w->window());
      dlg->finish(5);
      (void)data;
    });
  }
  d.button(_("Close"), 1, true);
  // The size is counted while the dialog is open.
  Ticker ticker{&d, counter};
  Fl::add_timeout(0.2, props_tick, &ticker);
  int r = d.run();
  Fl::remove_timeout(props_tick, &ticker);
  counter->stop = true;
  if (exec && ((st.st_mode & S_IXUSR) != 0) != (exec->value() != 0)) {
    mode_t m = st.st_mode & 07777;
    if (exec->value()) m |= (m & S_IRUSR ? S_IXUSR : 0) | (m & S_IRGRP ? S_IXGRP : 0) | (m & S_IROTH ? S_IXOTH : 0);
    else m &= ~mode_t(S_IXUSR | S_IXGRP | S_IXOTH);
    if (chmod(e.path.c_str(), m) != 0) parent->say(_("The permissions could not be changed."), true);
  }
  if (r == 5) show_open_with(parent, e);
}

// ------------------------------------------------------------------ open with

namespace {

class AppList : public Fl_Widget {
 public:
  AppList(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  std::vector<const MimeApp *> apps;
  int current = 0, top = 0;
  int row_h() const { return S(40); }
  void draw() override {
    const Theme &t = active_theme();
    fl_push_clip(x(), y(), w(), h());
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), t.radius_small, t.surface_alt);
      int rows = h() / row_h();
      for (int i = 0; i < rows && top + i < int(apps.size()); ++i) {
        int k = top + i, ry = y() + i * row_h();
        if (k == current) p.fill_rounded(x() + S(4), ry + S(2), w() - S(8), row_h() - S(4), t.radius_small, t.accent.alpha(0.18));
        std::vector<std::string> names = {apps[size_t(k)]->icon, "application-x-executable"};
        draw_icon(p, names, x() + S(12), ry + (row_h() - S(24)) / 2.0, S(24));
      }
    }
    int rows = h() / row_h();
    for (int i = 0; i < rows && top + i < int(apps.size()); ++i) {
      int k = top + i, ry = y() + i * row_h();
      text_font(t.font_size);
      draw_text(apps[size_t(k)]->name, x() + S(46), ry, w() - S(56), row_h(), t.text);
    }
    fl_pop_clip();
  }
  int handle(int e) override {
    int rows = std::max(1, h() / row_h());
    switch (e) {
      case FL_PUSH: {
        take_focus();
        int k = top + (Fl::event_y() - y()) / row_h();
        if (k >= 0 && k < int(apps.size())) {
          current = k;
          redraw();
          if (Fl::event_clicks()) do_callback();
        }
        return 1;
      }
      case FL_MOUSEWHEEL:
        top = std::max(0, std::min(int(apps.size()) - rows, top + Fl::event_dy() * 2));
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        return 1;
      case FL_KEYBOARD: {
        int key = Fl::event_key();
        if (key == FL_Up || key == FL_Down) {
          current = std::max(0, std::min(int(apps.size()) - 1, current + (key == FL_Down ? 1 : -1)));
          if (current < top) top = current;
          if (current >= top + rows) top = current - rows + 1;
          redraw();
          return 1;
        }
        return 0;
      }
    }
    return Fl_Widget::handle(e);
  }
};

}  // namespace

void show_open_with(MainWindow *parent, const Item &item) {
  App &app = App::get();
  std::vector<const MimeApp *> apps = app.apps.for_type(item.mime);
  // Every other application after the ones for the type.
  std::vector<const MimeApp *> others;
  for (const auto &a : app.apps.all())
    if (!a.hidden && std::find(apps.begin(), apps.end(), &a) == apps.end()) others.push_back(&a);
  std::sort(others.begin(), others.end(), [](const MimeApp *a, const MimeApp *b) { return a->name < b->name; });
  Dialog d(parent, S(520), S(520), _("Open with"));
  d.heading = format(_("How do you want to open “%s”?"), item.display.c_str());
  d.body = apps.empty() ? std::string(_("No application says it opens this type. These are all the applications."))
                        : format(_("Applications for %s"), parent->type_text(item.mime).c_str());
  d.icons = parent->icon_names(item);
  d.begin();
  auto *list = new AppList(S(24), S(110), S(520) - S(48), S(520) - S(110) - S(20) - S(34) - S(52));
  list->apps = apps.empty() ? others : apps;
  if (!apps.empty()) list->apps.insert(list->apps.end(), others.begin(), others.end());
  list->callback([](Fl_Widget *w, void *) { static_cast<Dialog *>(w->window())->finish(1); });
  auto *always = new Fl_Check_Button(S(20), S(520) - S(20) - S(34) - S(40), S(400), S(28),
                                     _("Always use this application for this type"));
  always->labelsize(active_theme().font_size);
  d.button(_("Cancel"), 0);
  d.button(_("Open"), 1, true);
  d.focus_later(list);
  if (d.run() != 1 || list->apps.empty()) return;
  const MimeApp *chosen = list->apps[size_t(std::max(0, std::min(int(list->apps.size()) - 1, list->current)))];
  if (always->value()) {
    std::string error;
    if (!app.apps.set_default(item.mime, chosen->id, &error)) parent->say(error, true);
  }
  if (!app.apps.launch(chosen->id, {item.path})) parent->say(_("The application could not be started."), true);
}

// ------------------------------------------------------------------ sign in

void show_sign_in(MainWindow *parent, const std::string &message, const std::string &user_in,
                  const std::string &domain_in, int flags, bool keyring, const std::string &location,
                  std::function<void(const SignIn &)> reply) {
  bool need_user = flags & G_ASK_PASSWORD_NEED_USERNAME, need_domain = flags & G_ASK_PASSWORD_NEED_DOMAIN;
  bool anonymous = flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED, saving = flags & G_ASK_PASSWORD_SAVING_SUPPORTED;
  std::string scheme = lower(location.substr(0, location.find(':')));
  bool plain = is_plain_text_scheme(scheme);
  // DOMAIN\user typed as the user is split as in Windows.
  std::string user = user_in, domain = domain_in;
  if (user.empty() || user == "guest" || user == "anonymous") user = user_name();
  int H = S(150) + (need_user ? S(70) : 0) + (need_domain ? S(70) : 0) + S(70) + (anonymous ? S(40) : 0) + S(40) +
          (plain ? S(48) : 0) + S(70);
  Dialog d(parent, S(500), H, _("Sign in"));
  d.heading = location.empty() ? std::string(_("Sign in")) : format(_("Sign in to %s"), location.c_str());
  d.body = message;
  d.icons = {"dialog-password", "network-server"};
  d.begin();
  int y = S(140);
  Field *ufield = nullptr, *dfield = nullptr;
  auto label_at = [&](int ly, const std::string &text) {
    std::string copy = text;
    (void)copy;
    (void)ly;
  };
  std::vector<std::pair<int, std::string>> labels;
  if (need_user) {
    labels.push_back({y, _("User name")});
    ufield = new Field(S(24), y + S(24), S(452), S(36));
    ufield->value(user.c_str());
    y += S(70);
  }
  if (need_domain) {
    labels.push_back({y, _("Domain")});
    dfield = new Field(S(24), y + S(24), S(452), S(36));
    dfield->value(domain.c_str());
    y += S(70);
  }
  labels.push_back({y, _("Password")});
  auto *pfield = new Field(S(24), y + S(24), S(352), S(36), true);
  auto *show = new FlatButton(S(384), y + S(24), S(92), S(36), _("Show"));
  show->copy_label(_("Show"));
  show->callback([](Fl_Widget *, void *f) {
    auto *field = static_cast<Field *>(f);
    field->type(field->input_type() == FL_SECRET_INPUT ? FL_NORMAL_INPUT : FL_SECRET_INPUT);
    field->redraw();
  }, pfield);
  y += S(70);
  Fl_Check_Button *guest = nullptr;
  if (anonymous) {
    guest = new Fl_Check_Button(S(20), y, S(440), S(30), _("Sign in as a guest"));
    guest->labelsize(active_theme().font_size);
    y += S(40);
  }
  Fl_Check_Button *remember = nullptr;
  std::string note;
  if (keyring && saving) {
    remember = new Fl_Check_Button(S(20), y, S(440), S(30), _("Remember the password in the keyring"));
    remember->labelsize(active_theme().font_size);
  } else {
    note = _("Passwords can be remembered when a keyring runs (such as GNOME Keyring or KeePassXC).");
  }
  int note_y = y;
  y += S(40);
  if (plain) note += std::string(note.empty() ? "" : "\n") +
                     _("This server sends the password and the files unencrypted: prefer SFTP, FTPS or WebDAV over HTTPS.");
  d.text = [&labels, &note, note_y, &d] {
    const Theme &t = active_theme();
    for (const auto &l : labels) {
      text_font(t.font_size);
      draw_text(l.second, S(24), l.first, S(300), S(22), t.text);
    }
    if (!note.empty()) {
      text_font(t.font_size - 1);
      int ny = note_y + (d.child(0) ? 0 : 0);
      for (const auto &line : wrap(note, d.w() - S(48))) {
        draw_text(line, S(24), ny + S(6), d.w() - S(48), S(20), t.text_dim);
        ny += S(20);
      }
    }
  };
  (void)label_at;
  d.button(_("Cancel"), 0);
  d.button(_("Sign in"), 1, true);
  d.focus_later(need_user && user.empty() && ufield ? static_cast<Fl_Widget *>(ufield) : pfield);
  SignIn s;
  s.ok = d.run() == 1;
  if (s.ok) {
    s.anonymous = guest && guest->value();
    s.user = ufield ? ufield->value() : user;
    s.domain = dfield ? dfield->value() : domain;
    size_t bs = s.user.find('\\');
    if (bs != std::string::npos && s.domain.empty()) {
      s.domain = s.user.substr(0, bs);
      s.user = s.user.substr(bs + 1);
    }
    s.password = pfield->value();
    s.remember = remember && remember->value();
  }
  // The password leaves the field.
  std::string blank(strlen(pfield->value()), ' ');
  pfield->value(blank.c_str());
  reply(s);
}

void show_question(MainWindow *parent, const std::string &message, const std::vector<std::string> &choices,
                   std::function<void(int)> reply) {
  text_font(active_theme().font_size);
  int lines = int(wrap(message, S(560) - S(78) - S(24)).size());
  Dialog d(parent, S(560), S(22) + lines * S(22) + S(40) + S(34) + S(30), _("A question from the server"));
  d.body = message;
  d.icons = {"dialog-question"};
  d.begin();
  for (size_t i = 0; i < choices.size(); ++i) d.button(choices[i], int(i) + 1, i == 0 && choices.size() == 1);
  d.default_result = 0;
  int r = d.run();
  reply(r - 1);
}

MountQuestions mount_questions(MainWindow *parent, const std::string &location) {
  MountQuestions q;
  auto asks = std::make_shared<int>(0);
  q.password = [parent, location, asks](const std::string &message, const std::string &user, const std::string &domain,
                                        int flags, bool keyring, std::function<void(const SignIn &)> reply) {
    std::string text = message;
    if (++*asks > 1) text = _("The user name or the password was not accepted. Try again.") + std::string("\n") + message;
    // The dialog runs outside GLib's dispatch: GIO waits for the reply.
    struct Ask {
      MainWindow *parent;
      std::string text, user, domain, location;
      int flags;
      bool keyring;
      std::function<void(const SignIn &)> reply;
    };
    auto *a = new Ask{parent, text, user, domain, location, flags, keyring, reply};
    Fl::add_timeout(0.0, [](void *d) {
      auto *ask = static_cast<Ask *>(d);
      auto &ws = App::get().windows;
      MainWindow *p = std::find(ws.begin(), ws.end(), ask->parent) != ws.end() ? ask->parent : nullptr;
      show_sign_in(p, ask->text, ask->user, ask->domain, ask->flags, ask->keyring, ask->location, ask->reply);
      delete ask;
    }, a);
  };
  q.question = [parent](const std::string &message, const std::vector<std::string> &choices,
                        std::function<void(int)> reply) {
    struct Ask {
      MainWindow *parent;
      std::string message;
      std::vector<std::string> choices;
      std::function<void(int)> reply;
    };
    auto *a = new Ask{parent, message, choices, reply};
    Fl::add_timeout(0.0, [](void *d) {
      auto *ask = static_cast<Ask *>(d);
      auto &ws = App::get().windows;
      MainWindow *p = std::find(ws.begin(), ws.end(), ask->parent) != ws.end() ? ask->parent : nullptr;
      show_question(p, ask->message, ask->choices, ask->reply);
      delete ask;
    }, a);
  };
  return q;
}

// ------------------------------------------------------------------ connect to server

void show_connect(MainWindow *parent) {
  App &app = App::get();
  Dialog d(parent, S(560), S(470), _("Connect to Server"));
  d.heading = _("Connect to a server");
  d.body = _("Type the address of a share or a server, or choose a recent one.");
  d.icons = {"network-server", "folder-remote"};
  d.begin();
  struct Proto {
    const char *scheme, *example;
  };
  static const Proto protos[] = {{"smb", "smb://server/share"},  {"sftp", "sftp://user@server/"},
                                 {"davs", "davs://server/path"}, {"dav", "dav://server/path"},
                                 {"ftp", "ftp://server/"},       {"nfs", "nfs://server/export"},
                                 {"afp", "afp://server/volume"}};
  auto *choice = new Fl_Choice(S(24), S(126), S(250), S(34));
  for (const auto &pr : protos) {
    std::string label = scheme_label(pr.scheme);
    if (!Drives::scheme_supported(pr.scheme)) label += " " + std::string(_("(not installed)"));
    // Fl_Choice parses "/" and "&" in labels.
    std::string escaped;
    for (char c : label) {
      if (c == '/' || c == '&' || c == '\\' || c == '_') escaped += '\\';
      escaped += c;
    }
    choice->add(escaped.c_str());
  }
  choice->value(0);
  auto *field = new Field(S(24), S(178), S(512), S(36));
  field->placeholder = protos[0].example;
  choice->callback([](Fl_Widget *w, void *f) {
    auto *c = static_cast<Fl_Choice *>(w);
    auto *fd = static_cast<Field *>(f);
    fd->placeholder = protos[std::max(0, c->value())].example;
    std::string v = fd->value();
    size_t sep = v.find("://");
    if (!v.empty() && sep != std::string::npos)
      v = std::string(protos[std::max(0, c->value())].scheme) + v.substr(sep);
    fd->value(v.c_str());
    fd->redraw();
  }, field);
  std::vector<std::string> recent = app.servers.items();
  d.text = [&recent, &d] {
    const Theme &t = active_theme();
    text_font(t.font_size, true);
    draw_text(_("Recent servers"), S(24), S(228), S(300), S(24), t.text);
    text_font(t.font_size);
    if (recent.empty()) draw_text(_("None yet."), S(24), S(254), S(400), S(24), t.text_dim);
    (void)d;
  };
  int ry = S(256);
  for (size_t i = 0; i < recent.size() && i < 4; ++i) {
    auto *b = new FlatButton(S(24), ry, S(512), S(32), recent[i]);
    b->copy_label(recent[i].c_str());
    b->callback([](Fl_Widget *w, void *f) {
      static_cast<Field *>(f)->value(w->label());
      static_cast<Field *>(f)->take_focus();
    }, field);
    ry += S(36);
  }
  d.button(_("Cancel"), 0);
  d.button(_("Connect"), 1, true);
  d.focus_later(field);
  if (d.run() != 1) return;
  std::string typed = trim(field->value());
  if (typed.empty()) return;
  if (typed.find("://") == std::string::npos && typed.rfind("\\\\", 0) != 0)
    typed = std::string(protos[std::max(0, choice->value())].scheme) + "://" + typed;
  parent->go_typed(typed);
}

void MainWindow::connect_to_server() { show_connect(this); }

}  // namespace liria
}  // namespace sde
