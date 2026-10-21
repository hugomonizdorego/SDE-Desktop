// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Taca main window. Without an archive it shows a drop zone and the
// recent archives; with one, an "archive card" on the left (identity,
// compression and the actions), a breadcrumb and search on top and the
// contents in a list or a grid. Archive work runs on a worker thread and
// shows a progress card that can be cancelled.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>

#include "config.h"
#include "i18n.h"
#include "internal.h"
#include "spawn.h"
#include "taca.h"
#include "util.h"
#include "x11.h"

using namespace sde;

namespace taca {

TacaWindow *TacaWindow::instance_ = nullptr;

namespace {

enum Action {
  ExtractAll, ExtractSelected, AddFiles, AddFolder, RenameItem, DeleteItems, TestArchive, Properties,
  OpenOther, NewArchive, CloseArchive, ActionCount
};

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool inside(int mx, int my) const { return mx >= x && mx < x + w && my >= y && my < y + h; }
};

std::string recent_file() { return state_home() + "/sde/taca-recent"; }

std::string fmt_count(size_t n, const char *one, const char *many) { return format(n == 1 ? one : many, n); }

}  // namespace

// ----------------------------------------------------------------- recent

std::vector<std::string> TacaWindow::recent() {
  std::vector<std::string> out;
  std::string text;
  if (!read_file(recent_file(), text)) return out;
  for (const auto &l : split(text, '\n'))
    if (!l.empty() && file_exists(l) && out.size() < 8) out.push_back(l);
  return out;
}

void TacaWindow::remember(const std::string &path) {
  char real[4096];
  std::string p = realpath(path.c_str(), real) ? real : path;
  auto list = recent();
  list.erase(std::remove(list.begin(), list.end(), p), list.end());
  list.insert(list.begin(), p);
  if (list.size() > 8) list.resize(8);
  std::string text;
  for (const auto &l : list) text += l + "\n";
  make_dirs(state_home() + "/sde");
  write_file_atomic(recent_file(), text);
}

// ------------------------------------------------------------------ setup

TacaWindow::TacaWindow(int w, int h) : BufferedWindow(w, h, _("Taca")) {
  instance_ = this;
  theme_ = active_theme();
  begin();
  view_ = new FileView(this, 0, 0, w, h);
  search_ = new Fl_Input(0, 0, S(200), S(26));
  search_->when(FL_WHEN_CHANGED);
  search_->callback([](Fl_Widget *, void *p) { static_cast<TacaWindow *>(p)->refresh_view(); }, this);
  mode_ = new Chips(0, 0, S(150), S(30), {_("List"), _("Icons")});
  mode_->callback([](Fl_Widget *w, void *p) {
    auto *self = static_cast<TacaWindow *>(p);
    self->view_->set_grid(static_cast<Chips *>(w)->value() == 1);
    Config cfg;
    cfg.load();
    cfg.set("taca", "view", self->view_->grid() ? "icons" : "list");
    cfg.save();
  }, this);
  cancel_ = new FlatButton(0, 0, S(96), S(32), _("Cancel"));
  cancel_->callback([](Fl_Widget *, void *p) {
    auto *self = static_cast<TacaWindow *>(p);
    if (self->job_) self->job_->cancel();
  }, this);
  open_btn_ = new FlatButton(0, 0, S(170), S(38), _("Open Archive…"), true);
  open_btn_->callback([](Fl_Widget *, void *p) { static_cast<TacaWindow *>(p)->action_open(); }, this);
  new_btn_ = new FlatButton(0, 0, S(170), S(38), _("New Archive…"));
  new_btn_->callback([](Fl_Widget *, void *p) { static_cast<TacaWindow *>(p)->action_new(); }, this);
  end();
  Config cfg;
  cfg.load();
  bool grid = cfg.get("taca", "view", "list") == "icons";
  view_->set_grid(grid);
  mode_->value(grid ? 1 : 0);
  resizable(nullptr);
  size_range(S(640), S(440));
  xclass("Taca");
  recent_ = recent();
  apply_theme();
  layout();
  Fl::add_handler(x_event_handler);
  Fl::add_system_handler(system_handler, this);
  // Temporary copies from earlier sessions (opened files and drags).
  std::string cache = detail::cache_dir();
  for (const auto &n : list_dir(cache)) {
    struct stat st;
    std::string p = cache + "/" + n;
    if ((starts_with(n, ".taca-") || starts_with(n, "drag-") || starts_with(n, "open-")) && lstat(p.c_str(), &st) == 0 &&
        time(nullptr) - st.st_mtime > 24 * 3600)
      detail::remove_tree(p);
  }
}

TacaWindow::~TacaWindow() {
  Fl::remove_handler(x_event_handler);
  Fl::remove_system_handler(system_handler);
  Fl::remove_timeout(tick_cb, this);
  Fl::remove_timeout(toast_cb, this);
  job_.reset();
  instance_ = nullptr;
}

void TacaWindow::hide() {
  if (job_) {
    if (message(this, _("Stop the current task?"), format(_("“%s” is still running."), job_->title().c_str()),
                {_("Keep Running"), _("Stop and Close")}, 0, true) != 1)
      return;
    job_->cancel();
    job_.reset();
  }
  BufferedWindow::hide();
}

void TacaWindow::apply_theme() {
  Config cfg;
  cfg.load();
  double scale = ui_scale();
  theme_ = Theme::from_config(cfg);
  set_ui_scale(scale);
  theme_.scale = scale;
  theme_.apply_to_fltk();
  style_input(search_, theme_);
  color(theme_.background.fl());
  redraw();
}

int TacaWindow::x_event_handler(int) {
  if (!instance_ || !fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL"))
    instance_->apply_theme();
  return 0;
}

// Answers the drop target's request for our dragged files as text/uri-list
// (stock FLTK 1.3 only offers plain text, which file managers ignore).
int TacaWindow::system_handler(void *event, void *data) {
  auto *self = static_cast<TacaWindow *>(data);
  XEvent *xe = static_cast<XEvent *>(event);
  if (!self || xe->type != SelectionRequest || self->drag_uris_.empty()) return 0;
  const XSelectionRequestEvent &req = xe->xselectionrequest;
  if (req.selection != x11::atom("XdndSelection")) return 0;
  Atom uri = x11::atom("text/uri-list"), targets = x11::atom("TARGETS"), utf8 = x11::atom("UTF8_STRING");
  Atom plain = x11::atom("text/plain"), plain8 = x11::atom("text/plain;charset=utf-8");
  XSelectionEvent reply{};
  reply.type = SelectionNotify;
  reply.display = req.display;
  reply.requestor = req.requestor;
  reply.selection = req.selection;
  reply.target = req.target;
  reply.time = req.time;
  reply.property = req.property ? req.property : req.target;
  if (req.target == targets) {
    Atom list[] = {uri, utf8, XA_STRING, plain, plain8};
    XChangeProperty(fl_display, req.requestor, reply.property, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char *>(list), 5);
  } else if (req.target == uri || req.target == utf8 || req.target == XA_STRING || req.target == plain ||
             req.target == plain8) {
    XChangeProperty(fl_display, req.requestor, reply.property, req.target, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(self->drag_uris_.data()), int(self->drag_uris_.size()));
  } else {
    reply.property = None;
  }
  XSendEvent(fl_display, req.requestor, False, 0, reinterpret_cast<XEvent *>(&reply));
  XFlush(fl_display);
  return 1;
}

// ----------------------------------------------------------------- layout

void TacaWindow::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  layout();
}

void TacaWindow::layout() {
  bool archive = has_archive();
  sidebar_w_ = archive ? (w() >= S(900) ? S(284) : S(236)) : 0;
  header_h_ = archive ? S(64) : 0;
  int footer = S(34);
  if (archive) {
    view_->resize(sidebar_w_, header_h_, w() - sidebar_w_, h() - header_h_ - footer);
    view_->show();
    int right = w() - S(20), cy = header_h_ / 2;
    mode_->resize(right - S(150), cy - S(15), S(150), S(30));
    mode_->show();
    int sw = std::max(S(140), std::min(S(260), (w() - sidebar_w_) / 3));
    search_->resize(right - S(150) - S(16) - sw + S(34), cy - S(13), sw - S(44), S(26));
    search_->show();
    open_btn_->hide();
    new_btn_->hide();
  } else {
    view_->hide();
    mode_->hide();
    search_->hide();
    int zw = std::min(S(640), w() - S(64)), zx = (w() - zw) / 2, zy = std::max(S(40), h() / 2 - S(250));
    int by = zy + S(186);
    open_btn_->resize(w() / 2 - S(178), by, S(170), S(38));
    new_btn_->resize(w() / 2 + S(8), by, S(170), S(38));
    open_btn_->show();
    new_btn_->show();
    (void)zx;
  }
  if (job_) {
    int cw = std::min(S(560), w() - sidebar_w_ - S(40));
    int cx = sidebar_w_ + (w() - sidebar_w_ - cw) / 2, cy = h() - S(34) - S(96);
    cancel_->resize(cx + cw - S(112), cy + S(22), S(96), S(32));
    cancel_->show();
  } else {
    cancel_->hide();
  }
  redraw();
}

// ------------------------------------------------------------------ jobs

void TacaWindow::run_job(const std::string &title, Job::Work work, Job::Done done) {
  if (job_) {
    toast(_("Please wait for the current task to finish"));
    return;
  }
  job_.reset(new Job(title, std::move(work), [this, done](const Result &r) {
    std::unique_ptr<Job> finished = std::move(job_);
    Fl::remove_timeout(tick_cb, this);
    layout();
    if (done) done(r);
  }));
  Fl::add_timeout(0.1, tick_cb, this);
  layout();
  if (Fl::focus() == nullptr) Fl::focus(cancel_);
}

void TacaWindow::tick_cb(void *p) {
  auto *self = static_cast<TacaWindow *>(p);
  self->redraw();
  if (self->job_) Fl::repeat_timeout(0.1, tick_cb, p);
}

bool TacaWindow::password_retry(const Result &r, std::function<void()> again, const std::string &name) {
  if (!r.need_password && !r.wrong_password) return false;
  std::string pw;
  std::string shown = !name.empty() ? name : info_.path.empty() ? std::string(_("archive")) : info_.path;
  if (ask_password(this, shown, r.wrong_password, pw)) {
    password_ = pw;
    again();
  }
  return true;
}

void TacaWindow::show_result(const std::string &what, const Result &r) {
  if (r.cancelled) {
    toast(_("Cancelled"));
    return;
  }
  if (!r.ok) {
    message(this, what, r.error, {_("OK")}, 0, true);
    return;
  }
  if (!r.warnings.empty()) {
    std::string text;
    size_t shown = 0;
    for (const auto &w : r.warnings) {
      if (++shown > 8) {
        text += format(_("… and %zu more"), r.warnings.size() - 8);
        break;
      }
      text += w + "\n";
    }
    message(this, _("Some items were skipped"), text, {_("OK")}, 0, true);
  }
}

// --------------------------------------------------------------- archive

void TacaWindow::open_archive(const std::string &path, bool keep_password) {
  if (!file_exists(path)) {
    message(this, _("Cannot open the archive"), format(_("“%s” does not exist."), path.c_str()), {_("OK")}, 0, true);
    return;
  }
  if (info_.path != path && !keep_password) password_.clear();
  std::string pw = password_;
  auto result = std::make_shared<ArchiveInfo>();
  run_job(format(_("Opening “%s”"), basename_of(path).c_str()),
          [path, pw, result](Progress &p) { return list_archive(path, pw, *result, &p); },
          [this, path, result](const Result &r) {
            if (!r.ok) {
              if (password_retry(r, [this, path] { open_archive(path, true); }, path)) return;
              if (!r.cancelled)
                message(this, _("Cannot open the archive"),
                        format("%s\n\n%s", basename_of(path).c_str(), r.error.c_str()), {_("OK")}, 0, true);
              return;
            }
            std::string folder_path = info_.path == path && folder_ > 0 && folder_ < int(tree_.size())
                                          ? tree_.node(folder_).path
                                          : "";
            info_ = std::move(*result);
            tree_.build(info_.entries);
            folder_ = std::max(0, tree_.find(folder_path));
            remember(path);
            recent_ = recent();
            copy_label(format("%s — Taca", basename_of(path).c_str()).c_str());
            search_->value("");
            layout();
            refresh_view(false);
            Fl::focus(view_);
            if (info_.unsafe)
              toast(fmt_count(info_.unsafe, _("%zu unsafe name will be skipped when extracting"),
                              _("%zu unsafe names will be skipped when extracting")));
          });
}

void TacaWindow::reload() { open_archive(info_.path); }

void TacaWindow::close_archive() {
  info_ = ArchiveInfo{};
  tree_.clear();
  folder_ = 0;
  password_.clear();
  recent_ = recent();
  copy_label(_("Taca"));
  layout();
}

bool TacaWindow::searching() const { return search_ && search_->size() > 0 && has_archive(); }

void TacaWindow::refresh_view(bool keep_selection) {
  if (!has_archive()) return;
  std::vector<int> items = searching() ? tree_.search(folder_, search_->value(), sort_, descending_)
                                       : tree_.list(folder_, sort_, descending_);
  view_->set_items(items, keep_selection);
  redraw();
}

void TacaWindow::set_sort(SortKey key) {
  if (sort_ == key) descending_ = !descending_;
  else {
    sort_ = key;
    descending_ = key == SortKey::Size || key == SortKey::Modified || key == SortKey::Packed;
  }
  refresh_view(true);
}

void TacaWindow::selection_changed() { redraw(); }

std::string TacaWindow::current_folder() const {
  return folder_ > 0 && folder_ < int(tree_.size()) ? tree_.node(folder_).path : "";
}

std::vector<std::string> TacaWindow::selected_paths() const {
  std::vector<std::string> out;
  for (int n : view_->selection()) out.push_back(tree_.node(n).path);
  return out;
}

void TacaWindow::enter(int node) {
  if (node < 0 || node >= int(tree_.size())) return;
  if (tree_.node(node).dir) {
    folder_ = node;
    search_->value("");
    refresh_view(false);
    return;
  }
  open_member(node);
}

void TacaWindow::go_up() {
  if (searching()) {
    search_->value("");
    refresh_view();
    return;
  }
  if (folder_ <= 0) return;
  int from = folder_;
  folder_ = std::max(0, tree_.node(folder_).parent);
  refresh_view(false);
  (void)from;
}

// ---------------------------------------------------------------- actions

void TacaWindow::action_open() {
  std::string start = has_archive() ? info_.path.substr(0, info_.path.rfind('/')) : home_dir();
  auto files = choose_files(_("Open an Archive"), start, false, true);
  if (!files.empty()) open_archive(files[0]);
}

void TacaWindow::action_new(const std::vector<std::string> &given) {
  std::vector<std::string> files = given;
  if (files.empty()) {
    files = choose_files(_("Choose Files to Pack"), home_dir(), false, false);
    if (files.empty()) return;
  }
  CreateChoice c;
  if (!ask_create(this, files, c)) return;
  std::string path = c.path;
  CreateOptions o = c.options;
  run_job(format(_("Creating “%s”"), basename_of(path).c_str()),
          [path, files, o](Progress &p) { return create_archive(path, files, o, &p); },
          [this, path, o](const Result &r) {
            if (!r.ok) {
              show_result(_("Could not create the archive"), r);
              return;
            }
            password_ = o.password;
            if (o.volume_size > 0) {
              toast(_("The split archive was created"));
              return;
            }
            open_archive(path, true);  // keeps the password just set
          });
}

void TacaWindow::action_extract(bool selected_only) {
  if (!has_archive()) return;
  auto sel = selected_paths();
  ExtractChoice c;
  c.dest = info_.path.substr(0, info_.path.rfind('/'));
  if (c.dest.empty()) c.dest = "/";
  if (access(c.dest.c_str(), W_OK) != 0) c.dest = home_dir();
  c.selected_only = selected_only && !sel.empty();
  if (!ask_extract(this, info_.path, sel.size(), c)) return;
  ExtractOptions o;
  o.dest = c.dest;
  o.keep_structure = c.keep_structure;
  o.overwrite = c.overwrite;
  o.password = password_;
  int64_t total = 0;
  if (c.selected_only) {
    o.only = sel;
    o.strip_prefix = current_folder();
    for (int n : view_->selection()) total += std::max<int64_t>(0, tree_.node(n).size);
  } else {
    total = info_.total_size;
  }
  o.total_hint = total;
  std::string path = info_.path;
  bool open_after = c.open_after;
  std::function<void(ExtractOptions)> start;
  start = [this, path, open_after](ExtractOptions o) {
    run_job(format(_("Extracting “%s”"), basename_of(path).c_str()),
            [path, o](Progress &p) { return extract_archive(path, o, &p); },
            [this, o, open_after](const Result &r) mutable {
              if (password_retry(r, [this, o, open_after]() mutable {
                    ExtractOptions again = o;
                    again.password = password_;
                    std::string path = info_.path;
                    run_job(_("Extracting"), [path, again](Progress &p) { return extract_archive(path, again, &p); },
                            [this, again, open_after](const Result &r2) {
                              if (password_retry(r2, [this] { action_extract(false); })) return;
                              show_result(_("Extraction failed"), r2);
                              if (!r2.ok) return;
                              toast(format(_("Extracted to %s"), again.dest.c_str()));
                              if (open_after)
                                open_uri(r2.written.size() == 1 && dir_exists(r2.written[0]) ? r2.written[0] : again.dest);
                            });
                  }))
                return;
              show_result(_("Extraction failed"), r);
              if (r.ok) {
                toast(format(_("Extracted to %s"), o.dest.c_str()));
                if (open_after) open_uri(r.written.size() == 1 && dir_exists(r.written[0]) ? r.written[0] : o.dest);
              }
            });
  };
  start(o);
}

void TacaWindow::action_add(bool folders) {
  if (!has_archive()) return;
  auto files = choose_files(folders ? _("Add Folders") : _("Add Files"), home_dir(), folders, false);
  if (!files.empty()) add_paths(files);
}

void TacaWindow::add_paths(const std::vector<std::string> &paths) {
  if (!has_archive() || paths.empty()) return;
  Capabilities caps = capabilities(info_.format);
  if (!caps.modify) {
    message(this, _("This archive cannot be changed"),
            caps.note.empty() ? format(_("Taca can read %s archives but not change them. Extract it and create a new archive instead."),
                                       format_info(info_.format).label)
                              : caps.note,
            {_("OK")}, 0, true);
    return;
  }
  std::string path = info_.path, folder = current_folder(), pw = password_;
  run_job(fmt_count(paths.size(), _("Adding %zu item"), _("Adding %zu items")),
          [path, paths, folder, pw](Progress &p) { return add_to_archive(path, paths, folder, pw, &p); },
          [this, paths](const Result &r) {
            if (password_retry(r, [this, paths] { add_paths(paths); })) return;
            show_result(_("Could not add the files"), r);
            if (r.ok) toast(fmt_count(paths.size(), _("%zu item added"), _("%zu items added")));
            reload();
          });
}

void TacaWindow::action_delete() {
  auto sel = selected_paths();
  if (sel.empty()) return;
  Capabilities caps = capabilities(info_.format);
  if (!caps.modify) {
    message(this, _("This archive cannot be changed"), caps.note.empty() ? _("Archives of this format are read-only in Taca.") : caps.note,
            {_("OK")}, 0, true);
    return;
  }
  std::string q = sel.size() == 1 ? format(_("Delete “%s” from the archive?"), basename_of(sel[0]).c_str())
                                  : format(_("Delete %zu items from the archive?"), sel.size());
  if (message(this, q, _("This changes the archive file and cannot be undone."), {_("Cancel"), _("Delete")}, 1, true) != 1)
    return;
  std::string path = info_.path, pw = password_;
  run_job(_("Deleting"), [path, sel, pw](Progress &p) { return delete_from_archive(path, sel, pw, &p); },
          [this](const Result &r) {
            if (password_retry(r, [this] { action_delete(); })) return;
            show_result(_("Could not delete"), r);
            if (r.ok) toast(_("Deleted"));
            reload();
          });
}

void TacaWindow::action_rename() {
  auto sel = selected_paths();
  if (sel.size() != 1) {
    toast(_("Select one item to rename"));
    return;
  }
  if (!capabilities(info_.format).modify) {
    toast(_("Archives of this format cannot be changed"));
    return;
  }
  std::string from = sel[0], name;
  if (!ask_name(this, _("Rename"), basename_of(from), name) || name == basename_of(from)) return;
  size_t slash = from.rfind('/');
  std::string to = slash == std::string::npos ? name : from.substr(0, slash + 1) + name;
  if (tree_.find(to) >= 0) {
    message(this, _("The name is taken"), format(_("“%s” already exists here."), name.c_str()), {_("OK")}, 0, true);
    return;
  }
  std::string path = info_.path, pw = password_;
  run_job(_("Renaming"), [path, from, to, pw](Progress &p) { return rename_in_archive(path, from, to, pw, &p); },
          [this](const Result &r) {
            show_result(_("Could not rename"), r);
            reload();
          });
}

void TacaWindow::action_test() {
  if (!has_archive()) return;
  std::string path = info_.path, pw = password_;
  run_job(_("Testing integrity"), [path, pw](Progress &p) { return test_archive(path, pw, &p); },
          [this](const Result &r) {
            if (password_retry(r, [this] { action_test(); })) return;
            if (r.cancelled) return;
            if (r.ok)
              message(this, _("The archive is intact"),
                      format(_("Every file in “%s” was read and checked without errors."), basename_of(info_.path).c_str()),
                      {_("OK")});
            else
              message(this, _("The archive is damaged"), r.error, {_("OK")}, 0, true);
          });
}

void TacaWindow::action_properties() {
  if (has_archive()) show_properties(this, info_, password_.empty() ? "" : _("Entered for this session"));
}

void TacaWindow::open_member(int node) {
  const Node &n = tree_.node(node);
  std::string dir = detail::make_temp_dir(detail::cache_dir());
  if (dir.empty()) return;
  std::string tmp = dir + "/open";
  std::rename(dir.c_str(), (detail::cache_dir() + "/open-" + basename_of(dir).substr(6)).c_str());
  dir = detail::cache_dir() + "/open-" + basename_of(dir).substr(6);
  ExtractOptions o;
  o.dest = dir;
  o.only = {n.path};
  o.strip_prefix = n.path.find('/') == std::string::npos ? "" : n.path.substr(0, n.path.rfind('/'));
  o.password = password_;
  o.total_hint = n.size;
  std::string path = info_.path, file = dir + "/" + n.name;
  run_job(format(_("Opening “%s”"), n.name.c_str()), [path, o](Progress &p) { return extract_archive(path, o, &p); },
          [this, node, file](const Result &r) {
            if (password_retry(r, [this, node] { open_member(node); })) return;
            if (!r.ok) {
              show_result(_("Could not open the file"), r);
              return;
            }
            if (!file_exists(file) || !open_uri(file)) toast(_("No application can open this file"));
          });
  (void)tmp;
}

void TacaWindow::context_menu(int node) {
  if (!has_archive()) return;
  auto sel = view_->selection();
  bool one = sel.size() == 1, any = !sel.empty();
  bool can = capabilities(info_.format).modify;
  struct Item {
    const char *text;
    long id;
    int flags;
  } items[] = {{_("Open"), 1, (one ? 0 : FL_MENU_INACTIVE)},
               {_("Extract Selected…"), 2, (any ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER},
               {_("Rename…"), 3, one && can ? 0 : FL_MENU_INACTIVE},
               {_("Delete from Archive"), 4, (any && can ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER},
               {_("Add Files Here…"), 5, can ? 0 : FL_MENU_INACTIVE},
               {_("Select All"), 6, 0}};
  Fl_Menu_Item menu[7];
  memset(menu, 0, sizeof menu);
  for (int i = 0; i < 6; ++i) {
    menu[i].text = items[i].text;
    menu[i].user_data_ = reinterpret_cast<void *>(items[i].id);
    menu[i].flags = items[i].flags;
    menu[i].labelsize(theme_.font_size);
    menu[i].labelcolor(theme_.text.fl());
  }
  const Fl_Menu_Item *picked = menu->popup(Fl::event_x(), Fl::event_y());
  if (!picked) return;
  switch (reinterpret_cast<long>(picked->user_data())) {
    case 1: enter(sel[0]); break;
    case 2: action_extract(true); break;
    case 3: action_rename(); break;
    case 4: action_delete(); break;
    case 5: action_add(false); break;
    case 6: view_->select_all(); break;
  }
  (void)node;
}

// Dragging the selection out: the items are extracted to a private cache
// folder and offered to the drop target as a text/uri-list.
void TacaWindow::start_drag() {
  auto sel = view_->selection();
  if (sel.empty() || job_) return;
  int64_t total = 0;
  for (int n : sel) total += std::max<int64_t>(0, tree_.node(n).size);
  if (total > 512ll * 1000 * 1000) {
    toast(_("Large selection: use Extract Selected instead of dragging"));
    return;
  }
  std::string base = detail::make_temp_dir(detail::cache_dir());
  if (base.empty()) return;
  std::string dir = detail::cache_dir() + "/drag-" + basename_of(base).substr(6);
  std::rename(base.c_str(), dir.c_str());
  ExtractOptions o;
  o.dest = dir;
  o.only = selected_paths();
  o.strip_prefix = current_folder();
  o.password = password_;
  fl_cursor(FL_CURSOR_WAIT);
  Fl::flush();
  Result r = extract_archive(info_.path, o, nullptr);
  fl_cursor(FL_CURSOR_DEFAULT);
  if (!r.ok || r.written.empty()) {
    detail::remove_tree(dir);
    toast(r.need_password || r.wrong_password ? _("Extract the files once to enter the password, then drag them")
                                               : _("Could not prepare the files for dragging"));
    return;
  }
  drag_uris_.clear();
  for (const auto &w : r.written) drag_uris_ += path_to_uri(w) + "\r\n";
  Fl::copy(drag_uris_.c_str(), int(drag_uris_.size()), 0);
  dragging_out_ = true;
  Fl::dnd();
  dragging_out_ = false;
}

// --------------------------------------------------------------- drawing

namespace {

struct SidebarRow {
  Action action;
  Rect r;
  const char *label;
  icon::Kind glyph;
  bool small = false;  // an icon-only button in the bottom row
};

// Short windows get a compact archive card.
bool compact_card(int h) { return h < S(700); }
int card_height(int h) { return compact_card(h) ? S(164) : S(268); }

std::vector<SidebarRow> sidebar_rows(int w, int h, int top) {
  std::vector<SidebarRow> rows;
  int x = S(16), rw = w - S(32), rh = S(35);
  rows.push_back({ExtractAll, {x, top, rw, S(42)}, _("Extract All…"), icon::Extract});
  int y = top + S(52);
  const std::pair<Action, std::pair<const char *, icon::Kind>> list[] = {
      {ExtractSelected, {_("Extract Selected…"), icon::Extract}},
      {AddFiles, {_("Add Files…"), icon::Add}},
      {AddFolder, {_("Add Folder…"), icon::AddFolder}},
      {RenameItem, {_("Rename…"), icon::Rename}},
      {DeleteItems, {_("Delete"), icon::Trash}},
      {TestArchive, {_("Test Integrity"), icon::Shield}},
      {Properties, {_("Properties"), icon::Info}},
  };
  std::vector<SidebarRow> overflow;
  for (const auto &l : list) {
    if (y + rh > h - S(96)) {
      overflow.push_back({l.first, {}, l.second.first, l.second.second, true});
      continue;
    }
    rows.push_back({l.first, {x, y, rw, rh}, l.second.first, l.second.second});
    y += rh + S(2);
  }
  // Bottom row: Open and New, then whatever did not fit above as icons.
  int by = h - S(34) - S(52), gap = S(6), icon_w = S(36);
  int iw = int(overflow.size()) * (icon_w + gap);
  int half = (rw - iw - gap) / 2;
  bool labels = half >= S(84);
  if (!labels) half = icon_w;
  rows.push_back({OpenOther, {x, by, half, S(36)}, _("Open…"), icon::Open, !labels});
  rows.push_back({NewArchive, {x + half + gap, by, half, S(36)}, _("New…"), icon::Plus, !labels});
  int ox = x + 2 * (half + gap);
  for (auto &o : overflow) {
    o.r = {ox, by, icon_w, S(36)};
    rows.push_back(o);
    ox += icon_w + gap;
  }
  return rows;
}

int sidebar_top(int h) { return card_height(h) + S(32); }

}  // namespace

int TacaWindow::sidebar_action_at(int mx, int my) const {
  if (!has_archive() || mx >= sidebar_w_) return -1;
  for (const auto &row : sidebar_rows(sidebar_w_, h(), sidebar_top(h())))
    if (row.r.inside(mx, my)) return int(row.action);
  // The close "x" on the card.
  if (mx >= sidebar_w_ - S(44) && mx < sidebar_w_ - S(12) && my >= S(18) && my < S(50)) return CloseArchive;
  return -1;
}

bool action_enabled(const TacaWindow &win, int a, size_t selected) {
  if (win.busy()) return false;
  bool can = capabilities(win.info().format).modify;
  switch (a) {
    case ExtractSelected: return selected > 0;
    case AddFiles:
    case AddFolder: return can;
    case RenameItem: return can && selected == 1;
    case DeleteItems: return can && selected > 0;
    default: return true;
  }
}

void TacaWindow::run_sidebar_action(int a) {
  if (job_ || !action_enabled(*this, a, view_->selection().size())) return;
  switch (a) {
    case ExtractAll: action_extract(false); break;
    case ExtractSelected: action_extract(true); break;
    case AddFiles: action_add(false); break;
    case AddFolder: action_add(true); break;
    case RenameItem: action_rename(); break;
    case DeleteItems: action_delete(); break;
    case TestArchive: action_test(); break;
    case Properties: action_properties(); break;
    case OpenOther: action_open(); break;
    case NewArchive: action_new(); break;
    case CloseArchive: close_archive(); break;
  }
}

void TacaWindow::draw_sidebar(Painter &p) {
  int sw = sidebar_w_;
  p.fill_rect(0, 0, sw, h(), theme_.surface_alt);
  p.line(sw - 0.5, 0, sw - 0.5, h(), theme_.border.alpha(0.8));
  // Identity card.
  int cx = S(16), cw = sw - S(32);
  bool compact = compact_card(h());
  p.fill_rounded(cx, S(16), cw, card_height(h()), S(16), theme_.surface);
  p.stroke_rounded(cx + 0.5, S(16) + 0.5, cw - 1, card_height(h()) - 1, S(16), theme_.border.alpha(0.6));
  icon::badge(p, cx + S(16), S(32), S(56), theme_.accent);
  icon::draw(p, icon::Close, sw - S(40), S(26), S(18), hover_action_ == CloseArchive ? theme_.text : theme_.text_dim);
  bool locked = info_.encrypted || info_.header_encrypted;
  if (compact) {
    if (locked) {
      text_font(theme_.font_size - S(1), true);
      icon::draw(p, icon::Lock, cx + S(84) + text_width(format_info(info_.format).label) + S(8), S(39), S(16),
                 theme_.accent_text);
    }
  } else {
    // Compression meter.
    double saved = info_.total_size > 0 ? 1 - double(info_.archive_size) / double(info_.total_size) : -1;
    int my = S(206), mx = cx + S(16), mw = cw - S(32);
    p.fill_rounded(mx, my, mw, S(8), S(4), theme_.border.alpha(0.6));
    if (saved > 0) p.fill_rounded(mx, my, std::max(double(S(8)), mw * std::min(1.0, saved)), S(8), S(4), theme_.accent);
    if (locked) {
      p.fill_rounded(mx, S(242), S(112), S(26), S(13), theme_.accent.alpha(0.16));
      icon::draw(p, icon::Lock, mx + S(8), S(247), S(16), theme_.accent_text);
    }
  }
  // Actions.
  size_t selected = view_->selection().size();
  for (const auto &row : sidebar_rows(sw, h(), sidebar_top(h()))) {
    bool on = action_enabled(*this, row.action, selected) && !job_;
    bool hover = hover_action_ == int(row.action) && on;
    if (row.action == ExtractAll) {
      p.fill_gradient(row.r.x, row.r.y, row.r.w, row.r.h, row.r.h / 2.0, theme_.accent.mix(Color::hex(0xFFFFFF), hover ? 0.18 : 0.08),
                      theme_.accent.mix(Color::hex(0x000000), 0.08));
      icon::draw(p, row.glyph, row.r.x + S(16), row.r.y + (row.r.h - S(20)) / 2.0, S(20), theme_.on_accent);
      continue;
    }
    if (row.action == OpenOther || row.action == NewArchive || row.small) {
      p.fill_rounded(row.r.x, row.r.y, row.r.w, row.r.h, S(10), hover ? theme_.hover : theme_.surface);
      p.stroke_rounded(row.r.x + 0.5, row.r.y + 0.5, row.r.w - 1, row.r.h - 1, S(10), theme_.border);
      double ix = row.small ? row.r.x + (row.r.w - S(18)) / 2.0 : row.r.x + S(10);
      Color c = on || row.action == OpenOther || row.action == NewArchive ? theme_.text_dim : theme_.text_dim.alpha(0.4);
      if (on && row.action == DeleteItems) c = Color::hex(0xD14B3C);
      icon::draw(p, row.glyph, ix, row.r.y + (row.r.h - S(18)) / 2.0, S(18), c);
      continue;
    }
    if (hover) p.fill_rounded(row.r.x, row.r.y, row.r.w, row.r.h, S(10), theme_.hover);
    Color c = on ? (row.action == DeleteItems ? Color::hex(0xD14B3C) : theme_.text) : theme_.text_dim.alpha(0.5);
    icon::draw(p, row.glyph, row.r.x + S(12), row.r.y + (row.r.h - S(20)) / 2.0, S(20), c);
  }
}

void TacaWindow::draw_sidebar_text() {
  int sw = sidebar_w_, cx = S(16), cw = sw - S(32), tx = cx + S(16);
  text_font(theme_.font_size - S(1), true);
  draw_text(format_info(info_.format).label, cx + S(84), S(36), cw - S(84) - S(30), S(22), theme_.accent_text);
  text_font(theme_.font_size - S(2));
  draw_text(describe_format(info_.format, info_.path), cx + S(84), S(58), cw - S(96), S(22), theme_.text_dim);
  // Name: up to two lines.
  text_font(theme_.font_size + S(3), true);
  std::string name = basename_of(info_.path);
  int nw = cw - S(32);
  std::string l1 = name, l2;
  if (text_width(name) > nw) {
    size_t cut = name.size();
    while (cut > 1 && text_width(name.substr(0, cut)) > nw) --cut;
    while (cut > 1 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
    l1 = name.substr(0, cut);
    l2 = name.substr(cut);
  }
  draw_text(l1, tx, S(100), nw, S(26), theme_.text);
  if (!l2.empty()) draw_text(l2, tx, S(124), nw, S(26), theme_.text);
  text_font(theme_.font_size - S(1));
  int sy = l2.empty() ? S(132) : S(152);
  draw_text(format("%s · %s", fmt_count(info_.files, _("%zu file"), _("%zu files")).c_str(),
                   fmt_count(info_.folders, _("%zu folder"), _("%zu folders")).c_str()),
            tx, sy, nw, S(22), theme_.text_dim);
  double saved = info_.total_size > 0 ? 1 - double(info_.archive_size) / double(info_.total_size) : -1;
  if (!compact_card(h())) {
    draw_text(format("%s → %s", human_size(info_.total_size).c_str(), human_size(info_.archive_size).c_str()), tx,
              S(178), nw, S(22), theme_.text);
    text_font(theme_.font_size - S(2));
    std::string ratio = info_.total_size <= 0 ? std::string(_("Empty archive"))
                        : saved > 0.005 ? format(_("%d%% smaller than the original"), int(std::lround(saved * 100)))
                                        : std::string(_("Stored without compression"));
    draw_text(ratio, tx, S(216), nw, S(22), theme_.text_dim);
    if (info_.encrypted || info_.header_encrypted) {
      text_font(theme_.font_size - S(2), true);
      draw_text(_("Encrypted"), tx + S(28), S(242), S(80), S(26), theme_.accent_text);
    }
  } else if (l2.empty() && saved > 0.005) {
    text_font(theme_.font_size - S(2));
    draw_text(format(_("%s · %d%% smaller"), human_size(info_.archive_size).c_str(), int(std::lround(saved * 100))),
              tx, S(154), nw, S(22), theme_.text_dim);
  }
  size_t selected = view_->selection().size();
  for (const auto &row : sidebar_rows(sw, h(), sidebar_top(h()))) {
    if (row.small) continue;
    bool on = action_enabled(*this, row.action, selected) && !job_;
    std::string label = row.label;
    if (row.action == ExtractSelected && selected) label = format(_("Extract %zu Selected…"), selected);
    if (row.action == ExtractAll) {
      text_font(theme_.font_size, true);
      draw_text(label, row.r.x + S(46), row.r.y, row.r.w - S(56), row.r.h, theme_.on_accent);
    } else if (row.action == OpenOther || row.action == NewArchive) {
      text_font(theme_.font_size - S(1));
      draw_text(label, row.r.x + S(34), row.r.y, row.r.w - S(38), row.r.h, theme_.text);
    } else {
      text_font(theme_.font_size);
      Color c = on ? (row.action == DeleteItems ? Color::hex(0xD14B3C) : theme_.text) : theme_.text_dim.alpha(0.6);
      draw_text(label, row.r.x + S(44), row.r.y, row.r.w - S(50), row.r.h, c);
    }
  }
}

int TacaWindow::crumb_at(int mx, int my) const {
  if (!has_archive() || my > header_h_ || mx < sidebar_w_) return -1;
  auto chain = tree_.ancestry(folder_);
  text_font(theme_.font_size, true);
  int x = sidebar_w_ + S(20), limit = search_->x() - S(60);
  for (size_t i = 0; i < chain.size(); ++i) {
    std::string label = i == 0 ? strip_archive_extension(info_.path) : tree_.node(chain[i]).name;
    int w = std::min(text_width(label) + S(i == 0 ? 50 : 24), S(220));
    if (x + w > limit) break;
    if (mx >= x && mx < x + w && my >= header_h_ / 2 - S(16) && my < header_h_ / 2 + S(16)) return chain[i];
    x += w + S(20);
  }
  return -1;
}

void TacaWindow::draw_header(Painter &p) {
  int sw = sidebar_w_;
  p.fill_rect(sw, 0, w() - sw, header_h_, theme_.background);
  p.line(sw, header_h_ - 0.5, w(), header_h_ - 0.5, theme_.border.alpha(0.7));
  auto chain = tree_.ancestry(folder_);
  text_font(theme_.font_size, true);
  int x = sw + S(20), cy = header_h_ / 2, limit = search_->x() - S(60);
  for (size_t i = 0; i < chain.size(); ++i) {
    std::string label = i == 0 ? strip_archive_extension(info_.path) : tree_.node(chain[i]).name;
    int w = std::min(text_width(label) + S(i == 0 ? 50 : 24), S(220));
    if (x + w > limit) break;
    bool last = i + 1 == chain.size();
    Color bg = last ? theme_.accent.alpha(theme_.dark ? 0.3 : 0.14) : (hover_crumb_ == chain[i] ? theme_.hover : theme_.surface_alt);
    p.fill_rounded(x, cy - S(16), w, S(32), S(16), bg);
    if (i == 0) icon::draw(p, icon::Box, x + S(10), cy - S(9), S(18), last ? theme_.accent_text : theme_.text_dim);
    x += w;
    if (!last) icon::draw(p, icon::Chevron, x + S(2), cy - S(8), S(16), theme_.text_dim);
    x += S(20);
  }
  // Search pill.
  int sx = search_->x() - S(34), sy = search_->y() - S(3), sww = search_->w() + S(44), shh = S(32);
  p.fill_rounded(sx, sy, sww, shh, shh / 2.0, theme_.surface_alt);
  p.stroke_rounded(sx + 0.5, sy + 0.5, sww - 1, shh - 1, shh / 2.0, Fl::focus() == search_ ? theme_.accent : theme_.border,
                   Fl::focus() == search_ ? Sd(1.5) : 1);
  icon::draw(p, icon::Search, sx + S(10), sy + S(7), S(18), theme_.text_dim);
}

void TacaWindow::draw_header_text() {
  auto chain = tree_.ancestry(folder_);
  text_font(theme_.font_size, true);
  int x = sidebar_w_ + S(20), cy = header_h_ / 2, limit = search_->x() - S(60);
  for (size_t i = 0; i < chain.size(); ++i) {
    std::string label = i == 0 ? strip_archive_extension(info_.path) : tree_.node(chain[i]).name;
    int w = std::min(text_width(label) + S(i == 0 ? 50 : 24), S(220));
    if (x + w > limit) {
      draw_text("…", x, cy - S(16), S(20), S(32), theme_.text_dim);
      break;
    }
    bool last = i + 1 == chain.size();
    int pad = i == 0 ? S(36) : S(12);
    draw_text(label, x + pad, cy - S(16), w - pad - S(10), S(32), last ? theme_.accent_text : theme_.text);
    x += w + S(20);
  }
  if (search_->size() == 0) {
    text_font(theme_.font_size);
    draw_text(_("Search in archive"), search_->x() + S(3), search_->y(), search_->w(), search_->h(), theme_.text_dim);
  }
  // Footer status.
  int fy = h() - S(34);
  text_font(theme_.font_size - S(1));
  auto sel = view_->selection();
  std::string status;
  if (!sel.empty()) {
    int64_t total = 0;
    for (int n : sel) total += std::max<int64_t>(0, tree_.node(n).size);
    status = format(_("%zu selected · %s"), sel.size(), human_size(total).c_str());
  } else {
    status = searching() ? fmt_count(view_->items().size(), _("%zu match"), _("%zu matches"))
                         : fmt_count(view_->items().size(), _("%zu item"), _("%zu items"));
  }
  draw_text(status, sidebar_w_ + S(20), fy, w() - sidebar_w_ - S(40), S(34), theme_.text_dim);
  std::string hint = capabilities(info_.format).modify ? _("Drag files here to add them · drag items out to extract")
                                                       : _("Read-only format · drag items out to extract");
  draw_text(hint, sidebar_w_ + S(20), fy, w() - sidebar_w_ - S(40), S(34), theme_.text_dim.alpha(0.8), 2);
}

void TacaWindow::draw_progress(Painter &p) {
  if (!job_) return;
  int cw = std::min(S(560), w() - sidebar_w_ - S(40));
  int cx = sidebar_w_ + (w() - sidebar_w_ - cw) / 2, cy = h() - S(34) - S(96), ch = S(76);
  p.shadow(cx, cy, cw, ch, S(14), theme_.shadow, S(10));
  p.fill_rounded(cx, cy, cw, ch, S(14), theme_.surface);
  p.stroke_rounded(cx + 0.5, cy + 0.5, cw - 1, ch - 1, S(14), theme_.border.alpha(0.7));
  double f = job_->progress().fraction();
  int bx = cx + S(18), bw = cw - S(150), by = cy + ch - S(20);
  p.fill_rounded(bx, by, bw, S(6), S(3), theme_.border.alpha(0.6));
  if (f >= 0) {
    p.fill_rounded(bx, by, std::max(double(S(6)), bw * f), S(6), S(3), theme_.accent);
  } else {
    // Indeterminate: a moving segment.
    double t = std::fmod(Fl::version() * 0 + double(clock()) / CLOCKS_PER_SEC * 0.8, 1.0);
    static double phase = 0;
    phase = std::fmod(phase + 0.04, 1.0);
    (void)t;
    p.fill_rounded(bx + (bw - bw / 4) * phase, by, bw / 4, S(6), S(3), theme_.accent);
  }
}

void TacaWindow::draw_progress_text() {
  if (!job_) return;
  int cw = std::min(S(560), w() - sidebar_w_ - S(40));
  int cx = sidebar_w_ + (w() - sidebar_w_ - cw) / 2, cy = h() - S(34) - S(96);
  double f = job_->progress().fraction();
  text_font(theme_.font_size, true);
  std::string title = job_->title();
  if (f >= 0) title += format("  ·  %d%%", int(f * 100));
  draw_text(title, cx + S(18), cy + S(10), cw - S(150), S(24), theme_.text);
  text_font(theme_.font_size - S(2));
  draw_text(job_->progress().current(), cx + S(18), cy + S(32), cw - S(150), S(20), theme_.text_dim);
}

int TacaWindow::recent_at(int mx, int my) const {
  if (has_archive()) return -1;
  int zw = std::min(S(640), w() - S(64)), zx = (w() - zw) / 2, zy = std::max(S(40), h() / 2 - S(250));
  int ry = zy + S(300);
  for (size_t i = 0; i < recent_.size(); ++i) {
    int y = ry + int(i) * S(54);
    if (y + S(50) > h() - S(40)) break;
    if (mx >= zx && mx < zx + zw && my >= y && my < y + S(48)) return int(i);
  }
  return -1;
}

void TacaWindow::draw_welcome(Painter &p) {
  int zw = std::min(S(640), w() - S(64)), zx = (w() - zw) / 2, zy = std::max(S(40), h() / 2 - S(250)), zh = S(250);
  // Soft accent glow behind the drop zone.
  p.fill_gradient(0, 0, w(), h() / 2, 0, theme_.accent.alpha(theme_.dark ? 0.10 : 0.07), theme_.background.alpha(0));
  p.fill_rounded(zx, zy, zw, zh, S(22), drop_hover_ ? theme_.accent.alpha(0.12) : theme_.surface);
  cairo_t *cr = p.cr();
  cairo_save(cr);
  double dash[] = {Sd(8), Sd(6)};
  cairo_set_dash(cr, dash, 2, 0);
  p.stroke_rounded(zx + S(8), zy + S(8), zw - S(16), zh - S(16), S(16), drop_hover_ ? theme_.accent : theme_.border, Sd(1.6));
  cairo_restore(cr);
  icon::badge(p, w() / 2 - S(34), zy + S(30), S(68), theme_.accent);
  int ry = zy + S(300);
  for (size_t i = 0; i < recent_.size(); ++i) {
    int y = ry + int(i) * S(54);
    if (y + S(50) > h() - S(40)) break;
    bool hv = hover_recent_ == int(i);
    p.fill_rounded(zx, y, zw, S(48), S(12), hv ? theme_.hover : theme_.surface_alt);
    p.fill_rounded(zx + S(10), y + S(10), S(56), S(28), S(8), theme_.accent.alpha(theme_.dark ? 0.3 : 0.15));
  }
}

void TacaWindow::draw_welcome_text() {
  int zw = std::min(S(640), w() - S(64)), zx = (w() - zw) / 2, zy = std::max(S(40), h() / 2 - S(250));
  text_font(theme_.font_size + S(7), true);
  draw_text(drop_hover_ ? _("Drop to open or pack") : _("Drop an archive here"), zx, zy + S(108), zw, S(32), theme_.text, 1);
  text_font(theme_.font_size);
  draw_text(_("or drop files and folders to pack them into a new archive"), zx, zy + S(142), zw, S(24), theme_.text_dim, 1);
  int ry = zy + S(300);
  text_font(theme_.font_size - S(1), true);
  if (!recent_.empty()) draw_text(_("Recent archives"), zx + S(4), ry - S(30), zw, S(24), theme_.text_dim);
  else {
    text_font(theme_.font_size - S(1));
    draw_text(_("ZIP · RAR · 7Z · TAR · GZ · BZ2 · XZ · ZST · LZ4 · ISO · CAB · CPIO · DEB · RPM · XAR · JAR · EPUB"),
              zx, ry - S(20), zw, S(24), theme_.text_dim, 1);
  }
  for (size_t i = 0; i < recent_.size(); ++i) {
    int y = ry + int(i) * S(54);
    if (y + S(50) > h() - S(40)) break;
    Format f = format_from_name(recent_[i]);
    text_font(theme_.font_size - S(3), true);
    draw_text(f == Format::Unknown ? "?" : format_info(f).label, zx + S(10), y + S(10), S(56), S(28), theme_.accent_text, 1);
    text_font(theme_.font_size, true);
    draw_text(basename_of(recent_[i]), zx + S(80), y + S(4), zw - S(96), S(22), theme_.text);
    text_font(theme_.font_size - S(2));
    std::string dir = recent_[i].substr(0, recent_[i].rfind('/'));
    std::string home = home_dir();
    if (starts_with(dir, home)) dir = "~" + dir.substr(home.size());
    draw_text(dir, zx + S(80), y + S(24), zw - S(96), S(20), theme_.text_dim);
  }
}

void TacaWindow::draw() {
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    if (has_archive()) {
      draw_sidebar(p);
      draw_header(p);
      p.line(sidebar_w_, h() - S(34) + 0.5, w(), h() - S(34) + 0.5, theme_.border.alpha(0.6));
    } else {
      draw_welcome(p);
    }
  }
  if (has_archive()) {
    draw_sidebar_text();
    draw_header_text();
  } else {
    draw_welcome_text();
  }
  draw_children();
  if (job_ || !toast_.empty()) {
    {
      Painter p(w(), h());
      draw_progress(p);
      if (!toast_.empty() && !job_) {
        text_font(theme_.font_size);
        int tw = std::min(text_width(toast_) + S(40), w() - S(40)), th = S(40);
        int tx = sidebar_w_ + (w() - sidebar_w_ - tw) / 2, ty = h() - th - S(48);
        Color bg = theme_.dark ? theme_.surface_alt.mix(theme_.text, 0.12) : Color::hex(0x1F2630);
        p.shadow(tx, ty, tw, th, th / 2.0, theme_.shadow, S(8));
        p.fill_rounded(tx, ty, tw, th, th / 2.0, bg);
      }
    }
    draw_progress_text();
    if (job_) draw_child(*cancel_);
    if (!toast_.empty() && !job_) {
      int tw = std::min(text_width(toast_) + S(40), w() - S(40)), th = S(40);
      int tx = sidebar_w_ + (w() - sidebar_w_ - tw) / 2, ty = h() - th - S(48);
      text_font(theme_.font_size);
      draw_text(toast_, tx + S(20), ty, tw - S(40), th, theme_.dark ? theme_.text : Color::hex(0xF5F7FA), 1);
    }
  }
}

void TacaWindow::toast(const std::string &message) {
  toast_ = message;
  Fl::remove_timeout(toast_cb, this);
  Fl::add_timeout(3.2, toast_cb, this);
  redraw();
}

void TacaWindow::toast_cb(void *p) {
  auto *self = static_cast<TacaWindow *>(p);
  self->toast_.clear();
  self->redraw();
}

// ----------------------------------------------------------------- events

int TacaWindow::handle(int event) {
  switch (event) {
    case FL_DND_ENTER:
    case FL_DND_DRAG:
      if (dragging_out_) return 0;
      if (!drop_hover_) {
        drop_hover_ = true;
        redraw();
      }
      return 1;
    case FL_DND_LEAVE:
      drop_hover_ = false;
      redraw();
      return 1;
    case FL_DND_RELEASE:
      drop_hover_ = false;
      redraw();
      return dragging_out_ ? 0 : 1;
    case FL_PASTE: {
      if (dragging_out_) return 1;
      auto paths = parse_uri_list(Fl::event_text());
      if (paths.empty()) return 1;
      if (job_) {
        toast(_("Please wait for the current task to finish"));
        return 1;
      }
      if (has_archive()) {
        add_paths(paths);
      } else {
        struct stat st;
        if (paths.size() == 1 && stat(paths[0].c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            detect_format(paths[0]) != Format::Unknown)
          open_archive(paths[0]);
        else
          action_new(paths);
      }
      return 1;
    }
    case FL_MOVE:
    case FL_ENTER: {
      int a = sidebar_action_at(Fl::event_x(), Fl::event_y());
      int c = crumb_at(Fl::event_x(), Fl::event_y());
      int rc = recent_at(Fl::event_x(), Fl::event_y());
      if (a != hover_action_ || c != hover_crumb_ || rc != hover_recent_) {
        hover_action_ = a;
        hover_crumb_ = c;
        hover_recent_ = rc;
        cursor(a >= 0 || c >= 0 || rc >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
        redraw();
      }
      break;
    }
    case FL_LEAVE:
      hover_action_ = hover_crumb_ = hover_recent_ = -1;
      cursor(FL_CURSOR_DEFAULT);
      redraw();
      break;
    case FL_PUSH: {
      int a = sidebar_action_at(Fl::event_x(), Fl::event_y());
      if (a >= 0) {
        run_sidebar_action(a);
        return 1;
      }
      int c = crumb_at(Fl::event_x(), Fl::event_y());
      if (c >= 0) {
        folder_ = c;
        search_->value("");
        refresh_view(false);
        return 1;
      }
      int rc = recent_at(Fl::event_x(), Fl::event_y());
      if (rc >= 0) {
        open_archive(recent_[size_t(rc)]);
        return 1;
      }
      break;
    }
    case FL_SHORTCUT:
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      bool ctrl = Fl::event_state() & FL_CTRL;
      if (ctrl && (key == 'q' || key == 'w')) {
        hide();
        return 1;
      }
      if (ctrl && key == 'o') {
        action_open();
        return 1;
      }
      if (ctrl && key == 'n') {
        action_new();
        return 1;
      }
      if (!has_archive() || job_) {
        if (key == FL_Escape && job_) {
          job_->cancel();
          return 1;
        }
        break;
      }
      if (ctrl && key == 'f') {
        Fl::focus(search_);
        redraw();
        return 1;
      }
      if (ctrl && key == 'e') {
        action_extract(!view_->selection().empty());
        return 1;
      }
      if (ctrl && key == 'i') {
        action_properties();
        return 1;
      }
      if (ctrl && key == 't') {
        action_test();
        return 1;
      }
      if (key == FL_Delete && Fl::focus() != search_) {
        action_delete();
        return 1;
      }
      if (key == FL_F + 2) {
        action_rename();
        return 1;
      }
      if (Fl::focus() == search_ && (key == FL_Escape || key == FL_Down || key == FL_Enter)) {
        if (key == FL_Escape) {
          search_->value("");
          refresh_view();
        }
        Fl::focus(view_);
        redraw();
        return 1;
      }
      if (key == FL_Escape) {
        if (!view_->selection().empty()) view_->clear_selection();
        return 1;
      }
      break;
    }
  }
  int r = BufferedWindow::handle(event);
  if (event == FL_FOCUS || event == FL_UNFOCUS || event == FL_PUSH) redraw();
  return r;
}

// --------------------------------------------------------- command line

namespace {

class ProgressWindow : public BufferedWindow {
 public:
  ProgressWindow(const std::string &title) : BufferedWindow(S(460), S(150), _("Taca")), title_(title) {
    begin();
    cancel_ = new FlatButton(w() - S(116), h() - S(52), S(96), S(34), _("Cancel"));
    cancel_->callback([](Fl_Widget *, void *p) {
      auto *self = static_cast<ProgressWindow *>(p);
      if (self->job) self->job->cancel();
    }, this);
    end();
    xclass("Taca");
    callback([](Fl_Widget *, void *p) {
      auto *self = static_cast<ProgressWindow *>(p);
      if (self->job) self->job->cancel();
    }, this);
  }
  void draw() override {
    const Theme &t = active_theme();
    double f = job ? job->progress().fraction() : 1;
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
      icon::badge(p, S(20), S(22), S(44), t.accent);
      int bx = S(80), bw = w() - S(100), by = S(84);
      p.fill_rounded(bx, by, bw, S(6), S(3), t.border.alpha(0.6));
      static double phase = 0;
      phase = std::fmod(phase + 0.04, 1.0);
      if (f >= 0) p.fill_rounded(bx, by, std::max(double(S(6)), bw * f), S(6), S(3), t.accent);
      else p.fill_rounded(bx + (bw - bw / 4) * phase, by, bw / 4, S(6), S(3), t.accent);
    }
    text_font(t.font_size + S(1), true);
    std::string head = title_;
    if (f >= 0 && job) head += format("  ·  %d%%", int(f * 100));
    draw_text(head, S(80), S(22), w() - S(100), S(26), t.text);
    text_font(t.font_size - S(2));
    draw_text(job ? job->progress().current() : "", S(80), S(48), w() - S(100), S(22), t.text_dim);
    draw_children();
  }
  std::unique_ptr<Job> job;

 private:
  std::string title_;
  FlatButton *cancel_;
};

void progress_tick(void *p) {
  static_cast<ProgressWindow *>(p)->redraw();
  Fl::repeat_timeout(0.1, progress_tick, p);
}

}  // namespace

int run_cli_job(const std::string &title, Job::Work work, bool quiet_success, std::function<void(const Result &)> done) {
  ProgressWindow win(title);
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  win.position(sx + (sw - win.w()) / 2, sy + (sh - win.h()) / 3);
  bool finished = false;
  Result result;
  // Only show the window when the job takes a moment.
  win.job.reset(new Job(title, std::move(work), [&](const Result &r) {
    result = r;
    finished = true;
  }));
  double waited = 0;
  while (!finished && waited < 0.4) {
    Fl::wait(0.05);
    waited += 0.05;
  }
  if (!finished) {
    win.show();
    Fl::add_timeout(0.1, progress_tick, &win);
    while (!finished) Fl::wait();
    Fl::remove_timeout(progress_tick, &win);
  }
  win.hide();
  win.job.reset();
  if (done) done(result);
  if (!result.ok && !result.cancelled && !result.need_password && !result.wrong_password && !result.error.empty())
    message(nullptr, title, result.error, {_("OK")}, 0, true);
  else if (result.ok && !result.warnings.empty()) {
    std::string text;
    for (size_t i = 0; i < result.warnings.size() && i < 8; ++i) text += result.warnings[i] + "\n";
    message(nullptr, _("Some items were skipped"), text, {_("OK")}, 0, true);
  }
  (void)quiet_success;
  return result.ok ? 0 : result.cancelled ? 3 : 1;
}

}  // namespace taca
