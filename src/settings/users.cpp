// SPDX-License-Identifier: GPL-3.0-or-later
// Control Center > Users & Avatar (sde-settings --page users).
//
// The account's picture in a circle, its real name (AccountsService
// User.RealName, changed with SetRealName: allowed for one's own account),
// the user name and the account type (Administrator for members of the sudo
// group, else Standard). "Change avatar": the stock pictures of
// /usr/share/Avatar, "Choose a picture..." (PNG, JPEG or SVG up to 10 MB,
// cropped to a square) and "Remove avatar" (back to the initials). The work
// is done by the shared backend (src/common/avatar.h) on a worker thread;
// the panel and the lock screen follow through _SDE_AVATAR_SERIAL.
#include <FL/Fl.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "avatar.h"
#include "avatar_view.h"
#include "i18n.h"
#include "settings.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

avatar::Account g_account;
bool g_account_loaded = false;
avatar::Picture g_picture;
bool g_busy = false;           // a change is being made
std::string g_status;          // its outcome
bool g_status_error = false;
std::string g_name_draft;      // the Real name field while it is edited
bool g_name_edited = false;
bool g_lookup_running = false, g_lookup_again = false;

// Without AccountsService's answer yet: the login and GECOS names.
const avatar::Account &local_account() {
  static avatar::Account a;
  static bool made = false;
  if (!made) {
    made = true;
    if (struct passwd *pw = getpwuid(getuid())) {
      a.user = pw->pw_name ? pw->pw_name : "";
      std::string g = pw->pw_gecos ? pw->pw_gecos : "";
      a.real_name = trim(g.substr(0, g.find(',')));
      if (a.real_name == a.user) a.real_name.clear();
    }
    if (a.user.empty()) a.user = env("USER", "user");
  }
  return a;
}

void refresh_account_pages() {
  refresh_if_shown("home");
  refresh_if_shown("users");
}

void set_status(const std::string &text, bool error) {
  g_status = text;
  g_status_error = error;
}

std::string pretty_name(const std::string &path) {
  std::string n = basename_of(path);
  size_t dot = n.rfind('.');
  if (dot != std::string::npos) n.erase(dot);
  for (char &c : n)
    if (c == '-' || c == '_') c = ' ';
  if (!n.empty()) n[0] = char(toupper((unsigned char)n[0]));
  return n;
}

void finished(const avatar::Result &r, const std::string &done_text) {
  g_busy = false;
  if (!r.ok)
    set_status(r.error.empty() ? std::string(_("The change did not work.")) : r.error, true);
  else
    set_status(r.note.empty() ? done_text : r.note, !r.note.empty());
  reload_account();
  refresh_account_pages();
}

void apply_picture(const std::string &path) {
  if (g_busy) return;
  g_busy = true;
  set_status(_("Changing your picture..."), false);
  refresh_account_pages();
  avatar::set_avatar_async(path, [](const avatar::Result &r) { finished(r, _("Your picture was changed.")); });
}

void remove_picture() {
  if (g_busy) return;
  g_busy = true;
  set_status(_("Removing your picture..."), false);
  refresh_account_pages();
  avatar::clear_avatar_async([](const avatar::Result &r) {
    finished(r, _("Your picture was removed: your initials are shown instead."));
  });
}

void save_name() {
  if (g_busy) return;
  std::string name = trim(g_name_draft);
  std::string why;
  if (!avatar::valid_real_name(name, &why)) {
    set_status(why, true);
    refresh_if_shown("users");
    return;
  }
  g_busy = true;
  set_status(_("Changing your name..."), false);
  refresh_if_shown("users");
  avatar::set_real_name_async(name, [](const avatar::Result &r) {
    g_name_edited = false;
    finished(r, _("Your name was changed."));
  });
}

// ------------------------------------------------------------- widgets

class AvatarCircle : public Fl_Widget {
 public:
  AvatarCircle(int x, int y, int d) : Fl_Widget(x, y, d, d) {}
  void draw() override {
    const Theme &t = settings_theme();
    cairo_surface_t *pic = g_picture.at(w());
    {
      Painter p(window()->w(), window()->h());
      p.circle(x() + w() / 2.0, y() + h() / 2.0, w() / 2.0 + Sd(2), t.border);
      avatar::paint_circle(p, pic, x(), y(), w(), t.accent);
    }
    if (!pic) avatar::draw_initials(settings_account().display_name(), x(), y(), w(), t.on_accent);
  }
};

// One stock picture: click to use it.
class StockChoice : public Fl_Widget {
 public:
  StockChoice(int x, int y, int d, std::string path)
      : Fl_Widget(x, y, d, d), path_(std::move(path)), name_(pretty_name(path_)) {
    tooltip(name_.c_str());
    pic_.set_path(path_);
    if (g_busy) deactivate();
  }
  bool current() const { return avatar::same_picture(path_, g_picture.path()); }
  void draw() override {
    const Theme &t = settings_theme();
    int inset = S(5), d = w() - 2 * inset;
    bool on = current();
    Painter p(window()->w(), window()->h());
    double cx = x() + w() / 2.0, cy = y() + h() / 2.0;
    if (on || hot_ || Fl::focus() == this) {
      cairo_t *cr = p.cr();
      cairo_new_path(cr);
      cairo_arc(cr, cx, cy, w() / 2.0 - Sd(1.5), 0, 2 * M_PI);
      p.set(on ? t.accent : t.text_dim.alpha(0.6));
      cairo_set_line_width(cr, on ? Sd(3) : Sd(1.5));
      cairo_stroke(cr);
    }
    avatar::paint_circle(p, pic_.at(d), x() + inset, y() + inset, d, t.surface_alt);
    if (!active_r()) p.circle(cx, cy, d / 2.0, t.surface.alpha(0.5));
    if (on) {
      double bx = x() + w() - Sd(12), by = y() + Sd(12);
      p.circle(bx, by, Sd(9), t.accent);
      p.line(bx - Sd(4), by, bx - Sd(1), by + Sd(3), t.on_accent, Sd(2));
      p.line(bx - Sd(1), by + Sd(3), bx + Sd(4), by - Sd(3), t.on_accent, Sd(2));
    }
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        if (active_r()) take_focus();
        return 1;
      case FL_RELEASE:
        if (active_r() && Fl::event_inside(this)) apply_picture(path_);
        return 1;
      case FL_ENTER:
      case FL_LEAVE:
        hot_ = e == FL_ENTER;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          apply_picture(path_);
          return 1;
        }
        return 0;
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  std::string path_, name_;
  avatar::Picture pic_;
  bool hot_ = false;
};

// A line of text in the accent (done) or error color.
class StatusLine : public Fl_Widget {
 public:
  StatusLine(int x, int y, int w, int h, std::string text, bool error)
      : Fl_Widget(x, y, w, h), text_(std::move(text)), error_(error) {}
  void draw() override {
    const Theme &t = settings_theme();
    text_font(t.font_size);
    fl_color((error_ ? t.error : t.accent_text).fl());
    fl_draw(text_.c_str(), x(), y(), w(), h(),
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  }

 private:
  std::string text_;
  bool error_;
};

class NameInput : public Fl_Input {
 public:
  NameInput(int x, int y, int w, int h) : Fl_Input(x, y, w, h) {
    const Theme &t = settings_theme();
    box(FL_BORDER_BOX);
    color(t.surface_alt.fl());
    textcolor(t.text.fl());
    cursor_color(t.accent.fl());
    selection_color(t.accent.fl());
    textsize(t.font_size + 1);
    when(FL_WHEN_CHANGED);
    tooltip(_("Real name"));
    callback([](Fl_Widget *w, void *) {
      g_name_draft = static_cast<Fl_Input *>(w)->value();
      g_name_edited = true;
    });
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
      Fl::add_timeout(0, [](void *) { save_name(); });
      return 1;
    }
    return Fl_Input::handle(e);
  }
};

void reload_later(void *) { reload_account(); }

// _SDE_AVATAR_SERIAL: another program (the Start menu) changed the picture.
int g_avatar_atom_handler(int) {
  if (!fl_xevent || fl_xevent->type != PropertyNotify) return 0;
  static Atom serial = XInternAtom(fl_display, avatar::kSerialAtom, False);
  if (fl_xevent->xproperty.window == RootWindow(fl_display, fl_screen) &&
      fl_xevent->xproperty.atom == serial) {
    Fl::remove_timeout(reload_later);
    Fl::add_timeout(0.15, reload_later);
  }
  return 0;
}

}  // namespace

const avatar::Account &settings_account() { return g_account_loaded ? g_account : local_account(); }
avatar::Picture &settings_avatar() { return g_picture; }

void reload_account() {
  if (g_lookup_running) {
    g_lookup_again = true;
    return;
  }
  g_lookup_running = true;
  avatar::lookup_async([](const avatar::Account &a, const std::string &path) {
    g_lookup_running = false;
    bool changed = !g_account_loaded || a.user != g_account.user ||
                   a.real_name != g_account.real_name || a.admin != g_account.admin ||
                   a.accounts_service != g_account.accounts_service;
    g_account = a;
    g_account_loaded = true;
    if (g_picture.set_path(path) || g_picture.check()) changed = true;
    if (!g_name_edited) g_name_draft = a.real_name;
    if (changed && !g_busy) refresh_account_pages();
    if (g_lookup_again) {
      g_lookup_again = false;
      reload_account();
    }
  });
}

void watch_account() {
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  Fl::add_handler(g_avatar_atom_handler);
  g_name_draft = local_account().real_name;
  reload_account();
}

void choose_avatar_picture() {
  if (g_busy) return;
  static const char *filter = "Pictures (*.{png,jpg,jpeg,svg,PNG,JPG,JPEG,SVG})";
  std::string dir = user_dir("PICTURES");
  if (dir.empty() || !dir_exists(dir)) dir = home_dir();
  const char *f = fl_file_chooser(_("Choose a picture"), filter, dir.c_str());
  if (!f || !*f) return;
  apply_picture(f);
}

void build_users_page(Page *page) {
  const Theme &t = settings_theme();
  const avatar::Account &a = settings_account();
  page->heading(_("Users & Avatar"), _("Your picture, your name and your account"));

  // Who: the picture, the name and the kind of account.
  page->begin_card();
  {
    int x = page->left() + S(20), y = page->cursor() + S(4), d = S(96);
    new AvatarCircle(x, y, d);
    int tx = x + d + S(22), tw = page->width() - (tx - page->left()) - S(20);
    new Label(tx, y + S(8), tw, S(34), a.display_name(), Label::TITLE);
    std::string kind = a.admin ? _("Administrator") : _("Standard account");
    new Label(tx, y + S(44), tw, S(22), a.user + "  ·  " + kind, Label::DIM);
    if (!g_status.empty()) {
      int sh = std::max(S(24), Label::measure(g_status, tw, Label::NORMAL) + S(4));
      new StatusLine(tx, y + S(70), tw, sh, g_status, g_status_error);
      page->advance(std::max(d + S(8), S(70) + sh + S(4)));
    } else {
      page->advance(d + S(8));
    }
  }
  page->end_card();

  // Change avatar.
  page->anchor("avatar");
  page->begin_card(_("Change avatar"));
  {
    int x = page->left() + S(20), avail = page->width() - S(40);
    std::vector<std::string> stock = avatar::stock_avatars();
    if (stock.empty()) {
      page->note(format(_("No pictures are installed in %s."), avatar::stock_dir().c_str()));
    } else {
      int cell = S(76), gap = S(10);
      int cols = std::max(1, (avail + gap) / (cell + gap));
      int y = page->cursor();
      for (size_t i = 0; i < stock.size(); ++i)
        new StockChoice(x + int(i % size_t(cols)) * (cell + gap), y + int(i / size_t(cols)) * (cell + gap),
                        cell, stock[i]);
      int rows = int((stock.size() + size_t(cols) - 1) / size_t(cols));
      page->advance(rows * (cell + gap) + S(4));
    }
    int bw = std::min(S(210), (avail - S(10)) / 2), y = page->cursor();
    auto *choose = new FlatButton(x, y, bw, S(34), _("Choose a picture…"), true);
    choose->callback([](Fl_Widget *, void *) { Fl::add_timeout(0, [](void *) { choose_avatar_picture(); }); });
    auto *remove = new FlatButton(x + bw + S(10), y, bw, S(34), _("Remove avatar"));
    remove->callback([](Fl_Widget *, void *) { remove_picture(); });
    if (g_picture.empty()) remove->deactivate();
    if (g_busy) {
      choose->deactivate();
      remove->deactivate();
    }
    page->advance(S(44));
    page->note(_("PNG, JPEG or SVG pictures of up to 10 MB. The middle square is kept and saved as "
                 "a 256 x 256 picture for SDE (~/.face) and the login screen."));
  }
  page->end_card();

  // The account.
  page->begin_card(_("Account"));
  page->row(
      _("Real name"), S(320), S(34),
      [&a](int x, int y, int w, int h) {
        int bw = S(92);
        auto *in = new NameInput(x, y, w - bw - S(8), h);
        in->value((g_name_edited ? g_name_draft : a.real_name).c_str());
        auto *save = new FlatButton(x + w - bw, y, bw, h, _("Save"), true);
        save->callback([](Fl_Widget *, void *) { save_name(); });
        if (!a.accounts_service || g_busy) {
          in->deactivate();
          save->deactivate();
        }
        return in;
      },
      a.accounts_service ? _("Shown in the Start menu, on the lock screen and when you log in.")
                         : _("Changing the name needs AccountsService (package accountsservice)."));
  page->row(_("User name"), S(220), S(30), [&a](int x, int y, int w, int h) {
    return new Label(x, y, w, h, a.user, Label::BOLD);
  });
  page->row(
      _("Account type"), S(220), S(30),
      [&a](int x, int y, int w, int h) {
        return new Label(x, y, w, h, a.admin ? _("Administrator") : _("Standard"), Label::BOLD);
      },
      a.admin ? _("A member of the sudo group: can install software and change the system.")
              : _("Not in the sudo group: an administrator's password is needed for system changes."));
  page->end_card();
  (void)t;
  page->finish();
}

}  // namespace sde
