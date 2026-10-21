// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The Taca main window: menu bar, toolbar, address bar, the details list
// and a status bar. The list browses the file system; double-clicking an
// archive enters it and ".." (or Backspace) goes back up, out of the
// archive into its folder. Archive work runs on a worker thread and shows
// a progress card that can be cancelled.
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

std::string recent_file() { return state_home() + "/sde/taca-recent"; }

std::string fmt_count(size_t n, const char *one, const char *many) { return format(n == 1 ? one : many, n); }

std::string parent_of(const std::string &path) { return parent_directory(path); }

std::string join(const std::string &dir, const std::string &name) {
  return dir == "/" ? "/" + name : dir + "/" + name;
}

const int kToolCommand[] = {CmdAdd, CmdExtract, CmdTest, CmdView, CmdDelete, CmdFind, CmdWizard, CmdInfo, CmdRepair};

// Escapes '&' so FLTK does not take it as a menu mnemonic.
std::string menu_text(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '&') out += '&';
    out += c;
  }
  return out;
}

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
  settings_ = Settings::load();
  theme_ = active_theme();
  begin();
  menubar_ = new MenuBar(0, 0, w, S(28));
  toolbar_ = new Toolbar(this, 0, S(28), w, Toolbar::wanted_height(h));
  address_ = new Fl_Input(0, 0, S(200), S(22));
  address_->when(FL_WHEN_ENTER_KEY_ALWAYS);
  address_->callback([](Fl_Widget *w, void *p) {
    auto *self = static_cast<TacaWindow *>(p);
    self->navigate_to(static_cast<Fl_Input *>(w)->value());
  }, this);
  view_ = new FileView(this, 0, 0, w, h);
  cancel_ = new FlatButton(0, 0, S(96), S(32), _("Cancel"));
  cancel_->callback([](Fl_Widget *, void *p) {
    auto *self = static_cast<TacaWindow *>(p);
    if (self->job_) self->job_->cancel();
  }, this);
  end();
  toolbar_->on_click = [this](int t) { command(kToolCommand[t]); };
  toolbar_->is_enabled = [this](int t) { return command_enabled(kToolCommand[t]); };
  favorites_ = load_favorites(favorites_file());
  view_->set_grid(settings_.icons);
  resizable(nullptr);
  size_range(S(560), S(380));
  xclass("Taca");
  apply_theme();
  build_menu();
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
  Config cfg;
  cfg.load();
  std::string start = cfg.get("taca", "last_dir", "");
  if (start.empty() || !dir_exists(start)) start = home_dir();
  browse(start);
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
  if (!fs_dir_.empty()) {
    Config cfg;
    cfg.load();
    cfg.set("taca", "last_dir", fs_dir_);
    cfg.save();
  }
  BufferedWindow::hide();
}

void TacaWindow::apply_theme() {
  Config cfg;
  cfg.load();
  if (settings_.theme == "light" || settings_.theme == "dark") cfg.set("appearance", "theme", settings_.theme);
  theme_ = Theme::from_config(cfg);
  theme_.apply_to_fltk();
  if (address_) {
    style_input(address_, theme_);
    address_->color(theme_.surface.fl());
  }
  if (menubar_) menubar_->apply_theme();
  color(theme_.background.fl());
  layout();
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

// ------------------------------------------------------------------- menu

void TacaWindow::menu_cb(Fl_Widget *, void *data) {
  if (instance_) instance_->command(int(reinterpret_cast<long>(data)));
}

void TacaWindow::build_menu() {
  menu_.clear();
  menu_labels_.clear();
  auto item = [&](const char *label, int shortcut, int cmd, int flags = 0) {
    Fl_Menu_Item m;
    memset(&m, 0, sizeof m);
    m.text = label;
    m.shortcut_ = shortcut;
    m.callback_ = menu_cb;
    m.user_data_ = reinterpret_cast<void *>(long(cmd));
    m.flags = flags;
    m.labelsize_ = uchar(theme_.font_size);
    menu_.push_back(m);
  };
  auto sub = [&](const char *label, int flags = 0) {
    Fl_Menu_Item m;
    memset(&m, 0, sizeof m);
    m.text = label;
    m.flags = FL_SUBMENU | flags;
    m.labelsize_ = uchar(theme_.font_size);
    menu_.push_back(m);
  };
  auto end = [&]() {
    Fl_Menu_Item m;
    memset(&m, 0, sizeof m);
    menu_.push_back(m);
  };
  const int D = FL_MENU_DIVIDER;
  sub(_("&File"));
  item(_("Open Archive…"), FL_CTRL + 'o', CmdOpen);
  item(_("Browse Folder…"), FL_CTRL + 'd', CmdBrowse);
  item(_("New Archive…"), FL_CTRL + 'n', CmdNew);
  item(_("Close Archive"), FL_CTRL + 'w', CmdClose, D);
  item(_("Select All"), FL_CTRL + 'a', CmdSelectAll);
  item(_("Invert Selection"), FL_CTRL + 'i', CmdInvert);
  item(_("Deselect"), 0, CmdDeselect, D);
  item(_("Copy Full Path"), FL_CTRL + FL_SHIFT + 'c', CmdCopyPath);
  item(_("Show in File Manager"), 0, CmdShowFolder);
  item(_("Refresh"), FL_F + 5, CmdRefresh, D);
  item(_("Exit"), FL_CTRL + 'q', CmdQuit);
  end();
  sub(_("&Commands"));
  item(_("Add Files to Archive…"), FL_ALT + 'a', CmdAdd);
  item(_("Extract To…"), FL_ALT + 'e', CmdExtract);
  item(_("Extract Here"), FL_ALT + 'w', CmdExtractHere);
  item(_("Test Archive"), FL_ALT + 't', CmdTest);
  item(_("View File"), FL_ALT + 'v', CmdView, D);
  item(_("Rename…"), FL_F + 2, CmdRename);
  item(_("Delete"), FL_Delete, CmdDelete, D);
  item(_("Repair Archive…"), FL_ALT + 'r', CmdRepair);
  item(_("Show Information"), FL_ALT + 'i', CmdInfo, D);
  item(_("Up One Level"), FL_ALT + FL_Up, CmdUp);
  end();
  sub(_("Too&ls"));
  item(_("Wizard…"), FL_ALT + 'z', CmdWizard);
  item(_("Find Files…"), FL_F + 3, CmdFind);
  item(_("Convert Archive…"), FL_ALT + 'q', CmdConvert);
  item(_("Repair Archive…"), 0, CmdRepair);
  end();
  sub(_("Favorite&s"));
  bool here = std::find(favorites_.begin(), favorites_.end(), has_archive() ? info_.path : fs_dir_) != favorites_.end();
  item(here ? _("Remove from Favorites") : _("Add to Favorites"), FL_CTRL + 'b', CmdFavAdd);
  item(_("Organize Favorites…"), 0, CmdFavOrganize, favorites_.empty() ? 0 : D);
  for (size_t i = 0; i < favorites_.size() && i < 30; ++i) {
    std::string home = home_dir(), shown = favorites_[i];
    if (starts_with(shown, home + "/")) shown = "~" + shown.substr(home.size());
    menu_labels_.push_back(menu_text(shown));
    item(menu_labels_.back().c_str(), 0, CmdFavorite + int(i));
  }
  end();
  sub(_("O&ptions"));
  item(_("Settings…"), FL_CTRL + ',', CmdSettings, D);
  item(_("Show Hidden Files"), FL_CTRL + 'h', CmdHidden, FL_MENU_TOGGLE | (settings_.hidden ? FL_MENU_VALUE : 0) | D);
  item(_("Details"), 0, CmdViewDetails, FL_MENU_RADIO | (settings_.icons ? 0 : FL_MENU_VALUE));
  item(_("Icons"), 0, CmdViewIcons, FL_MENU_RADIO | (settings_.icons ? FL_MENU_VALUE : 0) | D);
  item(_("Follow the SDE Theme"), 0, CmdThemeFollow, FL_MENU_RADIO | (settings_.theme == "follow" ? FL_MENU_VALUE : 0));
  item(_("Light"), 0, CmdThemeLight, FL_MENU_RADIO | (settings_.theme == "light" ? FL_MENU_VALUE : 0));
  item(_("Dark"), 0, CmdThemeDark, FL_MENU_RADIO | (settings_.theme == "dark" ? FL_MENU_VALUE : 0));
  end();
  sub(_("&Help"));
  item(_("Keyboard Shortcuts"), FL_F + 1, CmdShortcuts);
  item(_("About Taca"), 0, CmdAbout);
  end();
  end();
  menubar_->menu(menu_.data());
  menubar_->redraw();
}

// ----------------------------------------------------------------- layout

void TacaWindow::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  layout();
}

void TacaWindow::layout() {
  int mh = S(28);
  menubar_->resize(0, 0, w(), mh);
  int th = Toolbar::wanted_height(h());
  toolbar_->resize(0, mh, w(), th);
  address_y_ = mh + th;
  address_h_ = S(40);
  status_h_ = S(28);
  int bx = S(50), by = address_y_ + S(6), bw = w() - bx - S(8), bh = address_h_ - S(12);
  address_->resize(bx + S(32), by + S(3), bw - S(32) - S(32), bh - S(6));
  int vy = address_y_ + address_h_;
  view_->resize(0, vy, w(), h() - vy - status_h_);
  if (job_) {
    int cw = std::min(S(560), w() - S(40));
    int cx = (w() - cw) / 2, cy = h() - status_h_ - S(96);
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

// ------------------------------------------------------------- navigation

void TacaWindow::browse(const std::string &dir_in, const std::string &select) {
  std::string dir = normalize_path(dir_in, fs_dir_.empty() ? "/" : fs_dir_);
  std::vector<Entry> entries;
  std::string err;
  if (!list_directory(dir, settings_.hidden, entries, err)) {
    message(this, _("Cannot open the folder"), format("%s\n\n%s", dir.c_str(), err.c_str()), {_("OK")}, 0, true);
    address_->value(location().c_str());
    return;
  }
  if (has_archive()) {
    info_ = ArchiveInfo{};
    password_.clear();
  }
  results_ = false;
  hits_.clear();
  hit_of_node_.clear();
  fs_dir_ = dir;
  tree_.build_flat(entries);
  folder_ = 0;
  copy_label(format("%s — Taca", dir == "/" ? "/" : basename_of(dir).c_str()).c_str());
  refresh_view(false);
  if (!select.empty()) {
    int n = tree_.find(select);
    if (n > 0) view_->select_node(n);
  }
  build_menu();
}

void TacaWindow::list_folder(bool keep_selection) {
  if (has_archive() || results_) return;
  std::vector<Entry> entries;
  std::string err;
  if (!list_directory(fs_dir_, settings_.hidden, entries, err)) return;
  std::vector<std::string> keep;
  for (int n : view_->selection()) keep.push_back(tree_.node(n).path);
  tree_.build_flat(entries);
  refresh_view(false);
  if (keep_selection)
    for (const auto &k : keep) {
      int n = tree_.find(k);
      if (n > 0) view_->select_node(n);
    }
}

std::string TacaWindow::location() const {
  if (results_) return results_title_;
  if (has_archive()) return location_text(info_.path, current_folder());
  return fs_dir_;
}

void TacaWindow::refresh_view(bool keep_selection) {
  std::vector<int> items = tree_.size() ? tree_.list(folder_, sort_, descending_) : std::vector<int>{};
  bool parent_row = results_ || has_archive() || fs_dir_ != "/";
  view_->set_items(items, parent_row, keep_selection);
  if (Fl::focus() != address_) address_->value(location().c_str());
  redraw();
}

void TacaWindow::set_sort(SortKey key) {
  if (sort_ == key) {
    descending_ = !descending_;
  } else {
    sort_ = key;
    descending_ = key == SortKey::Size || key == SortKey::Modified || key == SortKey::Packed;
  }
  refresh_view(true);
}

void TacaWindow::selection_changed() { redraw(); }

std::string TacaWindow::current_folder() const {
  return has_archive() && folder_ > 0 && folder_ < int(tree_.size()) ? tree_.node(folder_).path : "";
}

std::vector<std::string> TacaWindow::selected_paths() const {
  std::vector<std::string> out;
  if (!has_archive() || results_) return out;
  for (int n : view_->selection()) out.push_back(tree_.node(n).path);
  return out;
}

std::string TacaWindow::disk_path(int node) const {
  if (node <= 0 || node >= int(tree_.size())) return "";
  if (results_) {
    auto it = hit_of_node_.find(node);
    if (it == hit_of_node_.end() || !hits_[it->second].archive.empty()) return "";
    return hits_[it->second].path;
  }
  if (has_archive()) return "";
  return join(fs_dir_, tree_.node(node).path);
}

std::vector<std::string> TacaWindow::selected_disk_paths() const {
  std::vector<std::string> out;
  for (int n : view_->selection()) {
    std::string p = disk_path(n);
    if (!p.empty()) out.push_back(p);
  }
  return out;
}

std::vector<std::string> TacaWindow::selected_archives() const {
  std::vector<std::string> out;
  for (int n : view_->selection()) {
    std::string p = disk_path(n);
    if (!p.empty() && !tree_.node(n).dir && looks_like_archive(p)) out.push_back(p);
  }
  return out;
}

bool TacaWindow::node_is_archive(int node) const {
  if (node <= 0 || node >= int(tree_.size())) return false;
  const Node &n = tree_.node(node);
  return !n.dir && format_from_name(n.name) != Format::Unknown;
}

std::string TacaWindow::type_of(int node) const {
  if (node <= 0 || node >= int(tree_.size())) return "";
  const Node &n = tree_.node(node);
  if (n.symlink && n.dir) return _("Link to folder");
  if (node_is_archive(node)) return describe_format(format_from_name(n.name), n.name);
  return file_kind(n.name, n.dir);
}

std::string TacaWindow::hit_folder(int node) const {
  auto it = hit_of_node_.find(node);
  if (it == hit_of_node_.end()) return "";
  const FindHit &h = hits_[it->second];
  std::string home = home_dir();
  std::string where;
  if (h.archive.empty()) {
    where = parent_of(h.path);
  } else {
    size_t slash = h.path.rfind('/');
    where = location_text(h.archive, slash == std::string::npos ? "" : h.path.substr(0, slash));
  }
  if (starts_with(where, home + "/") || where == home) where = "~" + where.substr(home.size());
  return where;
}

void TacaWindow::enter(int node) {
  if (node == kParentItem) {
    go_up();
    return;
  }
  if (node <= 0 || node >= int(tree_.size())) return;
  if (results_) {
    open_hit(node);
    return;
  }
  const Node &n = tree_.node(node);
  if (has_archive()) {
    if (n.dir) {
      folder_ = node;
      refresh_view(false);
      build_menu();
    } else {
      open_member(node, false);
    }
    return;
  }
  std::string path = join(fs_dir_, n.path);
  if (n.dir) browse(path);
  else if (looks_like_archive(path)) open_archive(path);
  else if (!open_uri(path)) toast(_("No application can open this file"));
}

void TacaWindow::go_up() {
  if (results_) {
    if (!back_archive_.empty()) open_archive(back_archive_, true, back_inner_);
    else browse(back_dir_.empty() ? fs_dir_ : back_dir_);
    return;
  }
  if (has_archive()) {
    if (folder_ > 0) {
      std::string from = tree_.node(folder_).path;
      folder_ = std::max(0, tree_.node(folder_).parent);
      refresh_view(false);
      view_->select_node(tree_.find(from));
      return;
    }
    close_archive();
    return;
  }
  if (fs_dir_ == "/") return;
  std::string from = basename_of(fs_dir_);
  browse(parent_of(fs_dir_), from);
}

void TacaWindow::navigate_to(const std::string &typed) {
  if (trim(typed).empty() || typed == location()) {
    Fl::focus(view_);
    return;
  }
  Location l = resolve_location(typed, has_archive() ? parent_of(info_.path) : fs_dir_);
  if (!l.ok) {
    message(this, _("Location not found"), format(_("“%s” does not exist."), typed.c_str()), {_("OK")}, 0, true);
    address_->value(location().c_str());
    return;
  }
  Fl::focus(view_);
  if (!l.folder.empty()) {
    browse(l.folder);
  } else if (looks_like_archive(l.archive)) {
    if (has_archive() && info_.path == l.archive) {
      int f = tree_.find(l.inner);
      if (f >= 0 && tree_.node(f).dir) {
        folder_ = f;
        refresh_view(false);
        return;
      }
    }
    open_archive(l.archive, false, l.inner);
  } else {
    browse(parent_of(l.archive), basename_of(l.archive));
  }
}

void TacaWindow::address_menu() {
  std::vector<std::string> targets;
  std::vector<std::string> labels;
  std::string loc = has_archive() ? info_.path : fs_dir_;
  for (std::string p = loc;; p = parent_of(p)) {
    targets.push_back(p);
    labels.push_back(p);
    if (p == "/") break;
  }
  size_t parents = targets.size();
  for (const auto &f : favorites_) {
    targets.push_back(f);
    labels.push_back(f);
  }
  size_t favs = targets.size();
  for (const auto &r : recent()) {
    targets.push_back(r);
    labels.push_back(r);
  }
  std::vector<Fl_Menu_Item> menu(targets.size() + 1);
  memset(menu.data(), 0, sizeof(Fl_Menu_Item) * menu.size());
  std::vector<std::string> texts;
  texts.reserve(targets.size());
  for (size_t i = 0; i < targets.size(); ++i) {
    texts.push_back(menu_text(labels[i]));
    menu[i].text = texts.back().c_str();
    menu[i].user_data_ = reinterpret_cast<void *>(long(i));
    menu[i].labelsize_ = uchar(theme_.font_size);
    if (i + 1 == parents || i + 1 == favs) menu[i].flags = FL_MENU_DIVIDER;
  }
  int bx = S(50), by = address_y_ + S(6), bw = w() - bx - S(8), bh = address_h_ - S(12);
  const Fl_Menu_Item *picked = menu.data()->popup(bx, by + bh, nullptr, nullptr, nullptr);
  (void)bw;
  if (picked) navigate_to(targets[size_t(reinterpret_cast<long>(picked->user_data()))]);
}

// --------------------------------------------------------------- archive

void TacaWindow::open_archive(const std::string &path, bool keep_password, const std::string &inner,
                              const std::string &select) {
  if (!file_exists(path)) {
    message(this, _("Cannot open the archive"), format(_("“%s” does not exist."), path.c_str()), {_("OK")}, 0, true);
    return;
  }
  if (info_.path != path && !keep_password) password_.clear();
  std::string pw = password_;
  auto result = std::make_shared<ArchiveInfo>();
  run_job(format(_("Opening “%s”"), basename_of(path).c_str()),
          [path, pw, result](Progress &p) { return list_archive(path, pw, *result, &p); },
          [this, path, inner, select, result](const Result &r) {
            if (!r.ok) {
              if (password_retry(r, [this, path, inner, select] { open_archive(path, true, inner, select); }, path))
                return;
              if (!r.cancelled)
                message(this, _("Cannot open the archive"),
                        format("%s\n\n%s", basename_of(path).c_str(), r.error.c_str()), {_("OK")}, 0, true);
              return;
            }
            info_ = std::move(*result);
            results_ = false;
            hits_.clear();
            hit_of_node_.clear();
            tree_.build(info_.entries);
            int f = tree_.find(inner);
            folder_ = f >= 0 && tree_.node(f).dir ? f : 0;
            fs_dir_ = parent_of(path);
            remember(path);
            copy_label(format("%s — Taca", basename_of(path).c_str()).c_str());
            refresh_view(false);
            if (!select.empty()) view_->select_node(tree_.find(select));
            Fl::focus(view_);
            build_menu();
            if (info_.unsafe)
              toast(fmt_count(info_.unsafe, _("%zu unsafe name will be skipped when extracting"),
                              _("%zu unsafe names will be skipped when extracting")));
          });
}

void TacaWindow::reload() {
  if (has_archive()) open_archive(info_.path, true, current_folder());
  else list_folder(true);
}

void TacaWindow::close_archive() {
  if (!has_archive()) return;
  std::string path = info_.path;
  info_ = ArchiveInfo{};
  tree_.clear();
  password_.clear();
  browse(parent_of(path), basename_of(path));
}

void TacaWindow::open_hit(int node) {
  auto it = hit_of_node_.find(node);
  if (it == hit_of_node_.end()) return;
  FindHit h = hits_[it->second];
  if (h.archive.empty()) {
    if (h.dir) browse(h.path);
    else browse(parent_of(h.path), basename_of(h.path));
    return;
  }
  size_t slash = h.path.rfind('/');
  open_archive(h.archive, false, slash == std::string::npos ? "" : h.path.substr(0, slash), h.path);
}

// ---------------------------------------------------------------- commands

bool TacaWindow::command_enabled(int id) const {
  if (busy() && id != CmdAbout && id != CmdShortcuts && id != CmdQuit) return false;
  auto sel = view_->selection();
  bool archive = has_archive() && !results_;
  bool modify = archive && capabilities(info_.format).modify;
  switch (id) {
    case CmdAdd: return results_ ? false : archive ? modify : true;
    case CmdExtract:
    case CmdExtractHere:
    case CmdTest:
    case CmdRepair:
      return archive || !selected_archives().empty();
    case CmdView: {
      if (sel.size() != 1 && view_->focus_item() <= 0) return false;
      int n = sel.size() == 1 ? sel[0] : view_->focus_item();
      return n > 0 && !tree_.node(n).dir && (archive || !disk_path(n).empty());
    }
    case CmdDelete: return !sel.empty() && !results_ && (archive ? modify : true);
    case CmdRename: return sel.size() == 1 && !results_ && (archive ? modify : true);
    case CmdInfo: return archive || !sel.empty() || !results_;
    case CmdClose: return has_archive();
    case CmdUp: return results_ || has_archive() || fs_dir_ != "/";
    default: return true;
  }
}

void TacaWindow::command(int id) {
  if (id >= CmdFavorite) {
    size_t i = size_t(id - CmdFavorite);
    if (i < favorites_.size()) navigate_to(favorites_[i]);
    return;
  }
  if (!command_enabled(id)) {
    if (busy()) toast(_("Please wait for the current task to finish"));
    else if (id == CmdExtract || id == CmdExtractHere || id == CmdTest || id == CmdRepair)
      toast(_("Select an archive first"));
    else if (id == CmdView) toast(_("Select a file to view"));
    else if (id == CmdAdd) toast(_("Archives of this format cannot be changed"));
    else if (id == CmdDelete || id == CmdRename) toast(_("Select what to change first"));
    return;
  }
  switch (id) {
    case CmdOpen: action_open(); break;
    case CmdBrowse: action_browse(); break;
    case CmdNew: action_new(); break;
    case CmdClose: close_archive(); break;
    case CmdSelectAll: view_->select_all(); break;
    case CmdInvert: view_->invert_selection(); break;
    case CmdDeselect: view_->clear_selection(); break;
    case CmdCopyPath: {
      auto paths = selected_disk_paths();
      std::string text;
      if (paths.empty()) text = location();
      for (const auto &p : paths) text += (text.empty() ? "" : "\n") + p;
      Fl::copy(text.c_str(), int(text.size()), 1);
      Fl::copy(text.c_str(), int(text.size()), 0);
      toast(_("Copied to the clipboard"));
      break;
    }
    case CmdShowFolder: open_uri(has_archive() ? parent_of(info_.path) : fs_dir_); break;
    case CmdRefresh: reload(); break;
    case CmdQuit: hide(); break;
    case CmdAdd: action_add(); break;
    case CmdExtract: action_extract(false); break;
    case CmdExtractHere: action_extract(true); break;
    case CmdTest: action_test(); break;
    case CmdView: action_view(); break;
    case CmdRename: action_rename(); break;
    case CmdDelete: action_delete(); break;
    case CmdRepair: action_repair(); break;
    case CmdInfo: action_info(); break;
    case CmdUp: go_up(); break;
    case CmdWizard: action_wizard(); break;
    case CmdFind: action_find(); break;
    case CmdConvert: {
      std::string a = has_archive() ? info_.path : "";
      if (a.empty() && !selected_archives().empty()) a = selected_archives()[0];
      if (a.empty()) {
        auto files = choose_files(_("Choose an Archive to Convert"), fs_dir_, false, true);
        if (files.empty()) break;
        a = files[0];
      }
      action_convert(a);
      break;
    }
    case CmdFavAdd: {
      std::string p = has_archive() ? info_.path : fs_dir_;
      bool added = toggle_favorite(favorites_, p);
      save_favorites(favorites_file(), favorites_);
      build_menu();
      toast(added ? format(_("“%s” was added to the favorites"), basename_of(p).c_str())
                  : format(_("“%s” was removed from the favorites"), basename_of(p).c_str()));
      break;
    }
    case CmdFavOrganize:
      if (organize_favorites(this, favorites_)) {
        save_favorites(favorites_file(), favorites_);
        build_menu();
      }
      break;
    case CmdSettings: action_settings(); break;
    case CmdHidden:
      settings_.hidden = !settings_.hidden;
      settings_.save();
      list_folder(true);
      build_menu();
      break;
    case CmdViewDetails:
    case CmdViewIcons:
      settings_.icons = id == CmdViewIcons;
      settings_.save();
      view_->set_grid(settings_.icons);
      build_menu();
      break;
    case CmdThemeFollow:
    case CmdThemeLight:
    case CmdThemeDark:
      settings_.theme = id == CmdThemeLight ? "light" : id == CmdThemeDark ? "dark" : "follow";
      settings_.save();
      apply_theme();
      build_menu();
      break;
    case CmdShortcuts: show_shortcuts(this); break;
    case CmdAbout: show_about(this); break;
  }
  redraw();
}

// ---------------------------------------------------------------- actions

void TacaWindow::action_open() {
  std::string start = has_archive() ? parent_of(info_.path) : fs_dir_;
  auto files = choose_files(_("Open an Archive"), start, false, true);
  if (!files.empty()) open_archive(files[0]);
}

void TacaWindow::action_browse() {
  std::string dir = choose_folder(_("Browse a Folder"), has_archive() ? parent_of(info_.path) : fs_dir_);
  if (!dir.empty()) browse(dir);
}

void TacaWindow::action_new(const std::vector<std::string> &given) {
  std::vector<std::string> files = given;
  if (files.empty()) {
    files = choose_files(_("Choose Files to Pack"), fs_dir_.empty() ? home_dir() : fs_dir_, false, false);
    if (files.empty()) return;
  }
  CreateChoice c;
  c.options.format = settings_.format;
  c.options.level = settings_.level;
  if (!has_archive() && !fs_dir_.empty() && access(fs_dir_.c_str(), W_OK) == 0) c.folder = fs_dir_;
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
            toast(format(_("“%s” was created"), basename_of(path).c_str()));
            if (!has_archive() && !results_ && parent_of(path) == fs_dir_) {
              list_folder(false);
              view_->select_node(tree_.find(basename_of(path)));
            } else if (o.volume_size == 0) {
              open_archive(path, true);
            }
          });
}

void TacaWindow::action_add() {
  if (has_archive()) {
    auto files = choose_files(_("Add Files to the Archive"), fs_dir_, false, false);
    if (!files.empty()) add_paths(files);
    return;
  }
  action_new(selected_disk_paths());
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

void TacaWindow::run_extract(const std::string &path, ExtractOptions o, bool here, bool open_after,
                             std::function<void()> next) {
  std::string parent = parent_of(path);
  run_job(format(_("Extracting “%s”"), basename_of(path).c_str()),
          [path, o, here, parent](Progress &p) {
            ExtractOptions eo = o;
            if (here) {
              ArchiveInfo inf;
              Result lr = list_archive(path, eo.password, inf, &p);
              if (!lr.ok) return lr;
              eo.dest = smart_destination(inf, parent);
              if (eo.dest.empty()) return Result::failure(_("Could not create the destination folder"));
              eo.total_hint = inf.total_size;
              p.done = 0;
            } else if (!dir_exists(eo.dest) && !make_dirs(eo.dest)) {
              return Result::failure(format(_("“%s” could not be created."), eo.dest.c_str()));
            }
            Result r = extract_archive(path, eo, &p);
            if (r.ok && r.written.empty()) r.written.push_back(eo.dest);
            return r;
          },
          [this, path, o, here, open_after, next](const Result &r) {
            if (r.need_password || r.wrong_password) {
              std::string pw;
              if (ask_password(this, path, r.wrong_password, pw)) {
                if (path == info_.path) password_ = pw;
                ExtractOptions again = o;
                again.password = pw;
                run_extract(path, again, here, open_after, next);
              }
              return;
            }
            show_result(_("Extraction failed"), r);
            if (!has_archive()) list_folder(true);
            if (!r.ok) return;
            std::string where = r.written.size() == 1 && dir_exists(r.written[0]) ? r.written[0]
                                : here ? parent_of(path) : o.dest;
            toast(format(_("Extracted to %s"), where.c_str()));
            if (next) next();
            else if (open_after) open_uri(where);
          });
}

void TacaWindow::action_extract(bool here) {
  bool archive = has_archive() && !results_;
  std::vector<std::string> archives = archive ? std::vector<std::string>{info_.path} : selected_archives();
  if (archives.empty()) return;
  auto sel = archive ? selected_paths() : std::vector<std::string>{};
  ExtractOptions base;
  base.password = archive ? password_ : "";
  if (here) {
    if (access(parent_of(archives[0]).c_str(), W_OK) != 0) {
      message(this, _("Cannot extract here"), format(_("You cannot write to “%s”."), parent_of(archives[0]).c_str()),
              {_("OK")}, 0, true);
      return;
    }
    // One after the other.
    auto queue = std::make_shared<std::vector<std::string>>(archives);
    auto step = std::make_shared<std::function<void(size_t)>>();
    *step = [this, queue, step, base](size_t i) {
      if (i >= queue->size()) return;
      std::function<void()> next;
      if (i + 1 < queue->size()) next = [step, i] { (*step)(i + 1); };
      run_extract((*queue)[i], base, true, false, next);
    };
    (*step)(0);
    return;
  }
  ExtractChoice c;
  std::string parent = parent_of(archives[0]);
  if (access(parent.c_str(), W_OK) != 0) parent = home_dir();
  c.dest = archives.size() == 1 ? join(parent, strip_archive_extension(basename_of(archives[0]))) : parent;
  c.selected_only = archive && !sel.empty();
  c.open_after = settings_.open_after;
  std::string shown = archives.size() == 1 ? archives[0] : format(_("%zu archives"), archives.size());
  if (!ask_extract(this, shown, archive ? sel.size() : 0, c)) return;
  base.keep_structure = c.keep_structure;
  base.overwrite = c.overwrite;
  if (archives.size() == 1) {
    base.dest = c.dest;
    if (archive) {
      if (c.selected_only) {
        base.only = sel;
        base.strip_prefix = current_folder();
        int64_t total = 0;
        for (int n : view_->selection()) total += std::max<int64_t>(0, tree_.node(n).size);
        base.total_hint = total;
      } else {
        base.total_hint = info_.total_size;
      }
    }
    run_extract(archives[0], base, false, c.open_after, nullptr);
    return;
  }
  // Several archives: each into its own folder below the destination.
  auto queue = std::make_shared<std::vector<std::string>>(archives);
  auto step = std::make_shared<std::function<void(size_t)>>();
  std::string dest = c.dest;
  bool open_after = c.open_after;
  *step = [this, queue, step, base, dest, open_after](size_t i) {
    if (i >= queue->size()) return;
    ExtractOptions o = base;
    o.dest = join(dest, strip_archive_extension(basename_of((*queue)[i])));
    std::function<void()> next;
    if (i + 1 < queue->size()) next = [step, i] { (*step)(i + 1); };
    run_extract((*queue)[i], o, false, open_after && i + 1 == queue->size(), next);
  };
  (*step)(0);
}

void TacaWindow::action_delete() {
  auto nodes = view_->selection();
  if (nodes.empty()) return;
  if (!has_archive()) {
    auto paths = selected_disk_paths();
    std::string q = paths.size() == 1 ? format(_("Move “%s” to the trash?"), basename_of(paths[0]).c_str())
                                      : format(_("Move %zu items to the trash?"), paths.size());
    if (message(this, q, _("You can restore them from the trash later."), {_("Cancel"), _("Move to Trash")}, 1, true) != 1)
      return;
    std::string err;
    auto failed = move_to_trash(paths, err);
    if (!failed.empty() && err.empty()) {
      std::string q2 = fmt_count(failed.size(), _("%zu item cannot be moved to the trash"),
                                 _("%zu items cannot be moved to the trash"));
      if (message(this, q2, _("They are on another drive. Delete them permanently? This cannot be undone."),
                  {_("Cancel"), _("Delete Permanently")}, 0, true) == 1) {
        for (const auto &f : failed) {
          struct stat st;
          if (lstat(f.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) detail::remove_tree(f);
          else unlink(f.c_str());
        }
      }
    } else if (!err.empty()) {
      message(this, _("Could not delete"), err, {_("OK")}, 0, true);
    }
    list_folder(false);
    return;
  }
  auto sel = selected_paths();
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
  auto nodes = view_->selection();
  if (nodes.size() != 1) {
    toast(_("Select one item to rename"));
    return;
  }
  if (!has_archive()) {
    std::string from = disk_path(nodes[0]), name;
    if (!ask_name(this, _("Rename"), basename_of(from), name) || name == basename_of(from)) return;
    std::string to = join(fs_dir_, name);
    if (access(to.c_str(), F_OK) == 0) {
      message(this, _("The name is taken"), format(_("“%s” already exists here."), name.c_str()), {_("OK")}, 0, true);
      return;
    }
    if (rename(from.c_str(), to.c_str()) != 0) message(this, _("Could not rename"), strerror(errno), {_("OK")}, 0, true);
    list_folder(false);
    view_->select_node(tree_.find(name));
    return;
  }
  if (!capabilities(info_.format).modify) {
    toast(_("Archives of this format cannot be changed"));
    return;
  }
  std::string from = tree_.node(nodes[0]).path, name;
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
  if (has_archive() && !results_) {
    std::string path = info_.path, pw = password_;
    run_job(_("Testing integrity"), [path, pw](Progress &p) { return test_archive(path, pw, &p); },
            [this](const Result &r) {
              if (password_retry(r, [this] { action_test(); })) return;
              if (r.cancelled) return;
              if (r.ok)
                message(this, _("The archive is intact"),
                        format(_("Every file in “%s” was read and checked without errors."), basename_of(info_.path).c_str()),
                        {_("OK")});
              else if (message(this, _("The archive is damaged"), r.error, {_("OK"), _("Repair…")}, 0, true) == 1)
                action_repair();
            });
    return;
  }
  test_archives(selected_archives(), 0, {});
}

void TacaWindow::test_archives(const std::vector<std::string> &archives, size_t index, std::vector<std::string> damaged) {
  if (index >= archives.size()) {
    if (damaged.empty())
      message(this, _("No errors were found"),
              archives.size() == 1 ? format(_("Every file in “%s” was read and checked without errors."),
                                            basename_of(archives[0]).c_str())
                                   : format(_("All %zu archives were read and checked without errors."), archives.size()),
              {_("OK")});
    else {
      std::string text;
      for (const auto &d : damaged) text += d + "\n";
      message(this, _("Damaged archives"), text, {_("OK")}, 0, true);
    }
    return;
  }
  std::string path = archives[index];
  std::string pw = path == info_.path ? password_ : "";
  run_job(format(_("Testing “%s”"), basename_of(path).c_str()), [path, pw](Progress &p) { return test_archive(path, pw, &p); },
          [this, archives, index, damaged, path](const Result &r) mutable {
            if (r.cancelled) return;
            if (r.need_password || r.wrong_password) {
              std::string pw;
              if (ask_password(this, path, r.wrong_password, pw)) {
                Result again = test_archive(path, pw);
                if (!again.ok) damaged.push_back(basename_of(path) + ": " + again.error);
              } else {
                damaged.push_back(basename_of(path) + ": " + _("not tested (encrypted)"));
              }
            } else if (!r.ok) {
              damaged.push_back(basename_of(path) + ": " + r.error);
            }
            test_archives(archives, index + 1, damaged);
          });
}

void TacaWindow::preview_file(const std::string &path, const std::string &title) {
  int cat = file_category(path);
  std::string data;
  bool text = cat == 0 || cat == 1 || cat == 6;
  if (text) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f) {
      data.resize(1024 * 1024);
      data.resize(fread(&data[0], 1, data.size(), f));
      fclose(f);
    }
    if (data.find('\0') != std::string::npos) text = false;
  }
  if (!text) {
    if (!open_uri(path)) toast(_("No application can open this file"));
    return;
  }
  if (data.size() == 1024 * 1024) data += "\n…";
  show_text(this, title, data);
}

void TacaWindow::action_view() {
  auto sel = view_->selection();
  int n = sel.size() == 1 ? sel[0] : view_->focus_item();
  if (n <= 0) return;
  if (has_archive() && !results_) {
    open_member(n, true);
    return;
  }
  std::string path = disk_path(n);
  if (path.empty()) {
    open_hit(n);
    return;
  }
  if (tree_.node(n).dir) browse(path);
  else if (looks_like_archive(path)) open_archive(path);
  else preview_file(path, basename_of(path));
}

void TacaWindow::open_member(int node, bool preview) {
  const Node &n = tree_.node(node);
  std::string base = detail::make_temp_dir(detail::cache_dir());
  if (base.empty()) return;
  std::string dir = detail::cache_dir() + "/open-" + basename_of(base).substr(6);
  std::rename(base.c_str(), dir.c_str());
  ExtractOptions o;
  o.dest = dir;
  o.only = {n.path};
  o.strip_prefix = n.path.find('/') == std::string::npos ? "" : n.path.substr(0, n.path.rfind('/'));
  o.password = password_;
  o.total_hint = n.size;
  std::string path = info_.path, file = dir + "/" + n.name, name = n.name;
  run_job(format(_("Opening “%s”"), n.name.c_str()), [path, o](Progress &p) { return extract_archive(path, o, &p); },
          [this, node, file, name, preview](const Result &r) {
            if (password_retry(r, [this, node, preview] { open_member(node, preview); })) return;
            if (!r.ok) {
              show_result(_("Could not open the file"), r);
              return;
            }
            if (!file_exists(file)) return;
            if (preview) preview_file(file, name);
            else if (!open_uri(file)) toast(_("No application can open this file"));
          });
}

void TacaWindow::action_find() {
  FindChoice c;
  c.in_archive = has_archive() && !results_;
  std::string where = c.in_archive ? basename_of(info_.path) : fs_dir_;
  if (!ask_find(this, where, c)) return;
  FindOptions o;
  o.pattern = c.pattern;
  o.recursive = c.recursive;
  o.inside_archives = c.inside_archives;
  o.hidden = settings_.hidden;
  if (c.in_archive) {
    o.archive = info_.path;
    o.password = password_;
  }
  o.root = fs_dir_;
  auto hits = std::make_shared<std::vector<FindHit>>();
  back_archive_ = c.in_archive ? info_.path : "";
  back_inner_ = c.in_archive ? current_folder() : "";
  back_dir_ = fs_dir_;
  run_job(format(_("Finding “%s”"), c.pattern.c_str()), [o, hits](Progress &p) { return find_files(o, *hits, &p); },
          [this, hits, c, where](const Result &r) {
            if (r.cancelled) return;
            if (!r.ok) {
              message(this, _("Could not search"), r.error, {_("OK")}, 0, true);
              return;
            }
            std::string archive_path = info_.path;
            info_ = ArchiveInfo{};
            hits_ = std::move(*hits);
            results_ = true;
            hit_of_node_.clear();
            std::vector<Entry> entries;
            for (size_t i = 0; i < hits_.size(); ++i) {
              const FindHit &h = hits_[i];
              Entry e;
              e.path = h.archive.empty() ? h.path : h.archive + "/" + h.path;
              e.dir = h.dir;
              e.size = h.size;
              e.mtime = h.mtime;
              entries.push_back(e);
              hit_of_node_[int(i) + 1] = i;
            }
            tree_.build_flat(entries);
            folder_ = 0;
            results_title_ = format(_("Find “%s” in %s"), c.pattern.c_str(), where.c_str());
            copy_label(format(_("Find results — Taca")).c_str());
            refresh_view(false);
            Fl::focus(view_);
            toast(hits_.empty() ? std::string(_("Nothing was found"))
                                : fmt_count(hits_.size(), _("%zu match found"), _("%zu matches found")));
            if (!r.warnings.empty()) toast(r.warnings.back());
            (void)archive_path;
          });
}

void TacaWindow::action_wizard() {
  int what = ask_wizard_action(this);
  if (what == 1) {
    action_new();
  } else if (what == 2) {
    auto files = choose_files(_("Choose the Archive to Extract"), fs_dir_, false, true);
    if (files.empty()) return;
    ExtractChoice c;
    c.dest = join(parent_of(files[0]), strip_archive_extension(basename_of(files[0])));
    c.open_after = settings_.open_after;
    if (!ask_extract(this, files[0], 0, c)) return;
    ExtractOptions o;
    o.dest = c.dest;
    o.keep_structure = c.keep_structure;
    o.overwrite = c.overwrite;
    run_extract(files[0], o, false, c.open_after, nullptr);
  } else if (what == 3) {
    auto files = choose_files(_("Choose the Archive to Convert"), fs_dir_, false, true);
    if (!files.empty()) action_convert(files[0]);
  }
}

void TacaWindow::action_convert(const std::string &archive) {
  CreateOptions o;
  o.format = settings_.format;
  o.level = settings_.level;
  std::string dest;
  if (!ask_convert(this, archive, dest, o)) return;
  std::string pw = archive == info_.path ? password_ : "";
  auto run = std::make_shared<std::function<void(std::string)>>();
  *run = [this, archive, dest, o, run](std::string pw) {
    run_job(format(_("Converting “%s”"), basename_of(archive).c_str()),
            [archive, pw, dest, o](Progress &p) { return convert_archive(archive, pw, dest, o, &p); },
            [this, archive, dest, run](const Result &r) {
              if (r.need_password || r.wrong_password) {
                std::string entered;
                if (ask_password(this, archive, r.wrong_password, entered)) (*run)(entered);
                return;
              }
              show_result(_("Could not convert the archive"), r);
              if (!r.ok) return;
              toast(format(_("Saved as %s"), basename_of(dest).c_str()));
              if (!has_archive() && !results_ && parent_of(dest) == fs_dir_) {
                list_folder(false);
                view_->select_node(tree_.find(basename_of(dest)));
              }
            });
  };
  (*run)(pw);
}

void TacaWindow::action_info() {
  if (has_archive() && !results_) {
    show_properties(this, info_, password_.empty() ? "" : _("Entered for this session"));
    return;
  }
  auto archives = selected_archives();
  auto paths = selected_disk_paths();
  if (archives.size() == 1 && paths.size() == 1) {
    std::string path = archives[0];
    auto result = std::make_shared<ArchiveInfo>();
    auto run = std::make_shared<std::function<void(std::string)>>();
    *run = [this, path, result, run](std::string pw) {
      run_job(format(_("Reading “%s”"), basename_of(path).c_str()),
              [path, pw, result](Progress &p) { return list_archive(path, pw, *result, &p); },
              [this, path, result, run, pw](const Result &r) {
                if (r.need_password || r.wrong_password) {
                  std::string entered;
                  if (ask_password(this, path, r.wrong_password, entered)) (*run)(entered);
                  return;
                }
                if (!r.ok) {
                  show_result(_("Cannot read the archive"), r);
                  return;
                }
                show_properties(this, *result, pw.empty() ? "" : _("Entered for this session"));
              });
    };
    (*run)("");
    return;
  }
  if (paths.empty() && !results_) paths = {fs_dir_};
  disk_info(paths);
}

void TacaWindow::disk_info(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  struct Totals {
    int64_t bytes = 0;
    size_t files = 0, folders = 0;
  };
  auto totals = std::make_shared<Totals>();
  run_job(_("Counting"),
          [paths, totals](Progress &p) {
            std::vector<std::string> stack;
            for (const auto &path : paths) {
              struct stat st;
              if (lstat(path.c_str(), &st) != 0) continue;
              if (S_ISDIR(st.st_mode)) stack.push_back(path);
              else {
                totals->files++;
                totals->bytes += st.st_size;
              }
            }
            bool top = true;
            while (!stack.empty() && !p.cancel) {
              std::string d = stack.back();
              stack.pop_back();
              if (!top || paths.size() > 1 || d != paths[0]) totals->folders++;
              top = false;
              p.set_current(d);
              std::vector<Entry> entries;
              std::string err;
              if (!list_directory(d, true, entries, err)) continue;
              for (const auto &e : entries) {
                if (e.dir && !e.symlink) stack.push_back(join(d, e.path));
                else if (!e.dir) {
                  totals->files++;
                  totals->bytes += std::max<int64_t>(0, e.size);
                }
              }
            }
            Result r;
            if (p.cancel) {
              r.ok = false;
              r.cancelled = true;
            }
            return r;
          },
          [this, paths, totals](const Result &r) {
            if (!r.ok) return;
            std::string title = paths.size() == 1 ? basename_of(paths[0]) : format(_("%zu items"), paths.size());
            if (title.empty()) title = "/";
            std::string text = format(_("Location: %s\n"), paths.size() == 1 ? paths[0].c_str() : fs_dir_.c_str());
            text += format(_("Contents: %zu files in %zu folders\n"), totals->files, totals->folders);
            text += format(_("Size: %s (%s bytes)\n"), human_size(totals->bytes).c_str(), group_digits(totals->bytes).c_str());
            if (paths.size() == 1) {
              struct stat st;
              if (stat(paths[0].c_str(), &st) == 0) {
                char date[64];
                struct tm tm;
                localtime_r(&st.st_mtime, &tm);
                strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
                text += format(_("Modified: %s\n"), date);
                if (!S_ISDIR(st.st_mode)) text += format(_("Type: %s\n"), file_kind(paths[0], false).c_str());
              }
            }
            message(this, title, text, {_("OK")});
          });
}

void TacaWindow::action_repair() {
  std::string archive = has_archive() && !results_ ? info_.path : "";
  if (archive.empty()) {
    auto a = selected_archives();
    if (a.empty()) return;
    archive = a[0];
  }
  std::string dest = parent_of(archive);
  if (access(dest.c_str(), W_OK) != 0) dest = home_dir();
  Format f = detect_format(archive);
  RepairPlan plan = plan_repair(archive, f, dest, find_program("zip"), rar_program(), seven_zip_program(), unrar_program());
  if (plan.method == RepairMethod::Unsupported) {
    message(this, _("Cannot repair this file"), plan.summary, {_("OK")}, 0, true);
    return;
  }
  if (!ask_repair(this, archive, plan, dest)) return;
  std::string pw = archive == info_.path ? password_ : "";
  struct Out {
    std::string output;
    size_t recovered = 0;
    long lost = -1;
  };
  auto out = std::make_shared<Out>();
  run_job(format(_("Repairing “%s”"), basename_of(archive).c_str()),
          [archive, dest, pw, out](Progress &p) {
            return repair_archive(archive, dest, pw, &p, out->output, out->recovered, out->lost);
          },
          [this, archive, dest, out](const Result &r) {
            if (r.cancelled) {
              toast(_("Cancelled"));
              return;
            }
            if (!r.ok) {
              message(this, _("The archive could not be repaired"),
                      format(_("%s\n\nNo readable files were found in “%s”."), r.error.c_str(), basename_of(archive).c_str()),
                      {_("OK")}, 0, true);
              return;
            }
            std::string text = fmt_count(out->recovered, _("%zu file was recovered into:"), _("%zu files were recovered into:"));
            text += "\n" + out->output;
            if (out->lost > 0)
              text += "\n\n" + fmt_count(size_t(out->lost), _("%zu damaged file could not be recovered."),
                                         _("%zu damaged files could not be recovered."));
            if (message(this, _("Repair finished"), text, {_("OK"), _("Show")}, 1) == 1) {
              close_archive();
              browse(dest, basename_of(out->output));
            } else if (!has_archive() && dest == fs_dir_) {
              list_folder(true);
            }
          });
}

void TacaWindow::action_settings() {
  Settings s = settings_;
  if (!ask_settings(this, s)) return;
  bool relist = s.hidden != settings_.hidden;
  settings_ = s;
  settings_.save();
  view_->set_grid(settings_.icons);
  apply_theme();
  if (relist) list_folder(true);
  build_menu();
}

void TacaWindow::context_menu(int node) {
  auto sel = view_->selection();
  bool archive = has_archive() && !results_;
  struct Item {
    const char *text;
    int cmd;
    int flags;
  };
  std::vector<Item> items;
  if (node == kParentItem) {
    items.push_back({_("Up One Level"), CmdUp, 0});
  } else if (!sel.empty()) {
    bool one_archive = !archive && !selected_archives().empty();
    items.push_back({_("Open"), -1, sel.size() == 1 ? 0 : FL_MENU_INACTIVE});
    items.push_back({_("View"), CmdView, FL_MENU_DIVIDER});
    if (archive || one_archive) {
      items.push_back({archive ? _("Extract Selected…") : _("Extract To…"), CmdExtract, 0});
      if (!archive) items.push_back({_("Extract Here"), CmdExtractHere, 0});
      if (!archive) items.push_back({_("Test Archive"), CmdTest, 0});
      if (!archive) items.push_back({_("Repair Archive…"), CmdRepair, 0});
      if (!archive) items.push_back({_("Convert Archive…"), CmdConvert, 0});
      items.back().flags |= FL_MENU_DIVIDER;
    }
    if (!archive && !results_) items.push_back({_("Add to Archive…"), CmdAdd, FL_MENU_DIVIDER});
    items.push_back({_("Rename…"), CmdRename, 0});
    items.push_back({_("Delete"), CmdDelete, FL_MENU_DIVIDER});
    if (!archive) items.push_back({_("Copy Full Path"), CmdCopyPath, 0});
    items.push_back({_("Information"), CmdInfo, 0});
  } else {
    if (archive) items.push_back({_("Add Files Here…"), CmdAdd, 0});
    else items.push_back({_("New Archive…"), CmdNew, 0});
    items.push_back({_("Find Files…"), CmdFind, 0});
    items.push_back({_("Refresh"), CmdRefresh, FL_MENU_DIVIDER});
    items.push_back({_("Select All"), CmdSelectAll, 0});
    items.push_back({_("Information"), CmdInfo, 0});
  }
  std::vector<Fl_Menu_Item> menu(items.size() + 1);
  memset(menu.data(), 0, sizeof(Fl_Menu_Item) * menu.size());
  for (size_t i = 0; i < items.size(); ++i) {
    menu[i].text = items[i].text;
    menu[i].user_data_ = reinterpret_cast<void *>(long(items[i].cmd));
    int flags = items[i].flags;
    if (items[i].cmd > 0 && !command_enabled(items[i].cmd)) flags |= FL_MENU_INACTIVE;
    menu[i].flags = flags;
    menu[i].labelsize_ = uchar(theme_.font_size);
  }
  const Fl_Menu_Item *picked = menu.data()->popup(Fl::event_x(), Fl::event_y());
  if (!picked) return;
  int cmd = int(reinterpret_cast<long>(picked->user_data()));
  if (cmd == -1) enter(sel[0]);
  else command(cmd);
}

// Dragging the selection out. Files on disk are offered as they are;
// archive members are extracted to a private cache folder first.
void TacaWindow::start_drag() {
  auto sel = view_->selection();
  if (sel.empty() || job_) return;
  drag_uris_.clear();
  if (!has_archive()) {
    for (const auto &p : selected_disk_paths()) drag_uris_ += path_to_uri(p) + "\r\n";
  } else {
    int64_t total = 0;
    for (int n : sel) total += std::max<int64_t>(0, tree_.node(n).size);
    if (total > 512ll * 1000 * 1000) {
      toast(_("Large selection: use Extract To instead of dragging"));
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
    for (const auto &w : r.written) drag_uris_ += path_to_uri(w) + "\r\n";
  }
  if (drag_uris_.empty()) return;
  Fl::copy(drag_uris_.c_str(), int(drag_uris_.size()), 0);
  dragging_out_ = true;
  Fl::dnd();
  dragging_out_ = false;
}

// --------------------------------------------------------------- drawing

void TacaWindow::draw_address(Painter &p) {
  int y0 = address_y_;
  p.fill_rect(0, y0, w(), address_h_, theme_.surface_alt.mix(theme_.surface, 0.5));
  p.line(0, y0 + address_h_ - 0.5, w(), y0 + address_h_ - 0.5, theme_.border);
  int bx = S(50), by = y0 + S(6), bw = w() - bx - S(8), bh = address_h_ - S(12);
  // Up button.
  bool up_on = command_enabled(CmdUp);
  int ux = S(8);
  if (hover_part_ == 1 && up_on) p.fill_rounded(ux, by, S(36), bh, S(6), theme_.hover);
  p.stroke_rounded(ux + 0.5, by + 0.5, S(36) - 1, bh - 1, S(6), theme_.border);
  icon::draw(p, icon::Up, ux + (S(36) - S(18)) / 2.0, by + (bh - S(18)) / 2.0, S(18),
             up_on ? theme_.accent_text : theme_.text_dim.alpha(0.4));
  // Location field.
  bool focus = Fl::focus() == address_;
  p.fill_rounded(bx, by, bw, bh, S(6), theme_.surface);
  p.stroke_rounded(bx + 0.5, by + 0.5, bw - 1, bh - 1, S(6), focus ? theme_.accent : theme_.border, focus ? Sd(1.5) : 1);
  double isz = S(18);
  if (results_) icon::draw(p, icon::Search, bx + S(8), by + (bh - isz) / 2, isz, theme_.accent_text);
  else if (has_archive()) icon::archive(p, bx + S(7), by + (bh - isz) / 2, isz, theme_.accent, theme_.dark);
  else icon::file(p, "", bx + S(7), by + (bh - isz) / 2, isz, true, theme_.accent, theme_.dark);
  // Drop-down arrow.
  int dx = bx + bw - S(30);
  if (hover_part_ == 2) p.fill_rounded(dx + S(2), by + S(2), S(26), bh - S(4), S(5), theme_.hover);
  p.line(dx + 0.5, by + S(5), dx + 0.5, by + bh - S(5), theme_.border);
  icon::draw(p, icon::Down, dx + S(6), by + (bh - S(18)) / 2.0, S(18), theme_.text_dim);
}

void TacaWindow::draw_status(Painter &p) {
  int y0 = h() - status_h_;
  p.fill_rect(0, y0, w(), status_h_, theme_.surface_alt.mix(theme_.surface, 0.4));
  p.line(0, y0 + 0.5, w(), y0 + 0.5, theme_.border);
  double isz = S(16);
  icon::draw(p, icon::Drive, S(10), y0 + (status_h_ - isz) / 2, isz, theme_.text_dim);
  bool locked = has_archive() && (info_.encrypted || info_.header_encrypted);
  icon::draw(p, icon::Lock, S(32), y0 + (status_h_ - isz) / 2, isz,
             locked ? Color::hex(0xE0A21B) : theme_.text_dim.alpha(0.35));
  int sep1 = S(56);
  p.line(sep1 + 0.5, y0 + S(5), sep1 + 0.5, y0 + status_h_ - S(5), theme_.border);
  int sep2 = w() / 2;
  p.line(sep2 + 0.5, y0 + S(5), sep2 + 0.5, y0 + status_h_ - S(5), theme_.border);
}

void TacaWindow::draw_status_text() {
  int y0 = h() - status_h_;
  text_font(theme_.font_size - S(1));
  // Totals of what is listed.
  int64_t bytes = 0, sel_bytes = 0;
  size_t files = 0, folders = 0, sel_files = 0, sel_folders = 0;
  for (int n : view_->items()) {
    if (n <= 0) continue;
    const Node &node = tree_.node(n);
    if (node.dir) ++folders;
    else {
      ++files;
      bytes += std::max<int64_t>(0, node.size);
    }
  }
  auto sel = view_->selection();
  for (int n : sel) {
    const Node &node = tree_.node(n);
    if (node.dir) {
      ++sel_folders;
      if (has_archive()) sel_bytes += std::max<int64_t>(0, node.size);
    } else {
      ++sel_files;
      sel_bytes += std::max<int64_t>(0, node.size);
    }
  }
  auto counts = [](size_t fo, size_t fi, int64_t b) {
    std::string s;
    if (fo) s += format(fo == 1 ? _("%zu folder") : _("%zu folders"), fo);
    if (fo && (fi || b)) s += _(" and ");
    if (fi || !fo) s += format(fi == 1 ? _("%s bytes in %zu file") : _("%s bytes in %zu files"), group_digits(b).c_str(), fi);
    return s;
  };
  std::string middle;
  if (!sel.empty()) middle = _("Selected ") + counts(sel_folders, sel_files, sel_bytes);
  else if (has_archive() && !results_) {
    middle = describe_format(info_.format, info_.path);
    if (info_.total_size > 0)
      middle += format(_(", ratio %d%%"), int(std::lround(100.0 * double(info_.archive_size) / double(info_.total_size))));
    if (info_.encrypted || info_.header_encrypted) middle += _(", encrypted");
  }
  draw_text(middle, S(66), y0, w() / 2 - S(74), status_h_, theme_.text);
  std::string right = results_ ? fmt_count(view_->items().size() - 1, _("Found %zu item"), _("Found %zu items"))
                               : _("Total ") + counts(folders, files, bytes);
  draw_text(right, w() / 2 + S(10), y0, w() / 2 - S(20), status_h_, theme_.text);
}

void TacaWindow::draw_progress(Painter &p) {
  if (!job_) return;
  int cw = std::min(S(560), w() - S(40));
  int cx = (w() - cw) / 2, cy = h() - status_h_ - S(96), ch = S(76);
  p.shadow(cx, cy, cw, ch, S(14), theme_.shadow, S(10));
  p.fill_rounded(cx, cy, cw, ch, S(14), theme_.surface);
  p.stroke_rounded(cx + 0.5, cy + 0.5, cw - 1, ch - 1, S(14), theme_.border.alpha(0.7));
  double f = job_->progress().fraction();
  int bx = cx + S(18), bw = cw - S(150), by = cy + ch - S(20);
  p.fill_rounded(bx, by, bw, S(6), S(3), theme_.border.alpha(0.6));
  if (f >= 0) {
    p.fill_rounded(bx, by, std::max(double(S(6)), bw * f), S(6), S(3), theme_.accent);
  } else {
    static double phase = 0;
    phase = std::fmod(phase + 0.04, 1.0);
    p.fill_rounded(bx + (bw - bw / 4) * phase, by, bw / 4, S(6), S(3), theme_.accent);
  }
}

void TacaWindow::draw_progress_text() {
  if (!job_) return;
  int cw = std::min(S(560), w() - S(40));
  int cx = (w() - cw) / 2, cy = h() - status_h_ - S(96);
  double f = job_->progress().fraction();
  text_font(theme_.font_size, true);
  std::string title = job_->title();
  if (f >= 0) title += format("  ·  %d%%", int(f * 100));
  draw_text(title, cx + S(18), cy + S(10), cw - S(150), S(24), theme_.text);
  text_font(theme_.font_size - S(2));
  draw_text(job_->progress().current(), cx + S(18), cy + S(32), cw - S(150), S(20), theme_.text_dim);
}

void TacaWindow::draw() {
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    draw_address(p);
    draw_status(p);
  }
  draw_status_text();
  draw_children();
  if (drop_hover_) {
    Painter p(w(), h());
    int vy = view_->y();
    p.fill_rounded(S(6), vy + S(6), w() - S(12), view_->h() - S(12), S(12), theme_.accent.alpha(0.10));
    cairo_t *cr = p.cr();
    cairo_save(cr);
    double dash[] = {Sd(8), Sd(6)};
    cairo_set_dash(cr, dash, 2, 0);
    p.stroke_rounded(S(6), vy + S(6), w() - S(12), view_->h() - S(12), S(12), theme_.accent, Sd(2));
    cairo_restore(cr);
  }
  if (job_ || !toast_.empty()) {
    text_font(theme_.font_size);
    int tw = std::min(text_width(toast_) + S(40), w() - S(40)), th = S(38);
    int tx = (w() - tw) / 2, ty = h() - status_h_ - th - S(16);
    {
      Painter p(w(), h());
      draw_progress(p);
      if (!toast_.empty() && !job_) {
        Color bg = theme_.dark ? theme_.surface_alt.mix(theme_.text, 0.12) : Color::hex(0x1F2630);
        p.shadow(tx, ty, tw, th, th / 2.0, theme_.shadow, S(8));
        p.fill_rounded(tx, ty, tw, th, th / 2.0, bg);
      }
    }
    draw_progress_text();
    if (job_) draw_child(*cancel_);
    if (!toast_.empty() && !job_) {
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
  int bx = S(50), by = address_y_ + S(6), bw = w() - bx - S(8), bh = address_h_ - S(12);
  auto part_at = [&](int mx, int my) {
    if (my < by || my >= by + bh) return 0;
    if (mx >= S(8) && mx < S(44)) return 1;
    if (mx >= bx + bw - S(30) && mx < bx + bw) return 2;
    return 0;
  };
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
      if (has_archive() && !results_) {
        add_paths(paths);
      } else {
        struct stat st;
        if (paths.size() == 1 && stat(paths[0].c_str(), &st) == 0 && S_ISDIR(st.st_mode)) browse(paths[0]);
        else if (paths.size() == 1 && stat(paths[0].c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                 looks_like_archive(paths[0]))
          open_archive(paths[0]);
        else
          action_new(paths);
      }
      return 1;
    }
    case FL_MOVE:
    case FL_ENTER: {
      int part = part_at(Fl::event_x(), Fl::event_y());
      if (part != hover_part_) {
        hover_part_ = part;
        redraw();
      }
      break;
    }
    case FL_LEAVE:
      if (hover_part_) {
        hover_part_ = 0;
        redraw();
      }
      break;
    case FL_PUSH: {
      int part = part_at(Fl::event_x(), Fl::event_y());
      if (part == 1) {
        if (!job_) go_up();
        return 1;
      }
      if (part == 2) {
        if (!job_) address_menu();
        return 1;
      }
      break;
    }
    case FL_SHORTCUT:
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      bool ctrl = Fl::event_state() & FL_CTRL;
      if (key == FL_Escape && job_) {
        job_->cancel();
        return 1;
      }
      if (job_) break;
      if (ctrl && key == 'l') {
        Fl::focus(address_);
        address_->position(0, address_->size());
        redraw();
        return 1;
      }
      if (ctrl && key == 'f') {
        command(CmdFind);
        return 1;
      }
      if (Fl::focus() == address_ && key == FL_Escape) {
        address_->value(location().c_str());
        Fl::focus(view_);
        redraw();
        return 1;
      }
      if (key == FL_Escape && Fl::focus() == view_) {
        if (!view_->selection().empty()) view_->clear_selection();
        return 1;
      }
      break;
    }
  }
  int r = BufferedWindow::handle(event);
  if (event == FL_FOCUS || event == FL_UNFOCUS || event == FL_PUSH || event == FL_KEYBOARD) redraw();
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
