// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Liria's application: its windows, what every window shares (preferences,
// pins, recent servers, drives, the clipboard, the undo history), the
// folders watched with inotify (changes appear at once, coalesced over a
// tenth of a second) and the theme, which follows the Control Center's
// changes while Liria runs.
#include <FL/Fl.H>
#include <FL/platform.H>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "clipboard.h"
#include "config.h"
#include "icons.h"
#include "location.h"
#include "transfer_dialog.h"
#include "ui.h"
#include "util.h"
#include "wm.h"
#include "x11.h"

namespace sde {
namespace liria {

namespace {

const uint32_t kFolderMask =
    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

}  // namespace

App &App::get() {
  static App app;
  return app;
}

MainWindow *App::open_window(const std::vector<std::string> &locations_in, const std::vector<std::string> &select) {
  double sc = ui_scale() > 0 ? ui_scale() : 1.0;
  int W = prefs.width > 0 ? int(prefs.width * sc) : S(1120);
  int H = prefs.height > 0 ? int(prefs.height * sc) : S(720);
  int x = 0, y = 0;
  fit_to_work_area(x, y, W, H);
  if (!windows.empty()) {
    // A new window steps down and right from the last one.
    MainWindow *last = windows.back();
    if (last->shown()) {
      x = last->x() + S(32);
      y = last->y() + S(32);
    }
  }
  Fl_Group *current = Fl_Group::current();
  Fl_Group::current(nullptr);
  auto *w = new MainWindow(std::max(W, S(480)), std::max(H, S(360)));
  Fl_Group::current(current);
  w->position(x, y);
  std::vector<std::string> locations = locations_in;
  // Items to select: their folders, the items selected there.
  std::vector<std::pair<std::string, std::vector<std::string>>> shown;
  for (const auto &p : select) {
    std::string dir = chooser::parent_dir(p);
    if (dir.empty()) continue;
    auto it = std::find_if(shown.begin(), shown.end(), [&](const auto &s) { return s.first == dir; });
    if (it == shown.end()) shown.push_back({dir, {p}});
    else it->second.push_back(p);
  }
  if (locations.empty() && shown.empty()) locations.push_back(kHome);
  bool first = true;
  auto open = [&](const std::string &loc, const std::vector<std::string> &items) {
    std::string sel = items.empty() ? std::string() : items[0];
    if (first) {
      w->go(loc, true, sel);
      first = false;
    } else {
      w->new_tab(loc, false, sel);
    }
    if (items.size() > 1) {
      for (int i = 0; i < w->tab_count(); ++i)
        if (w->tab_at(i).location == normalize_location(loc))
          const_cast<Tab &>(w->tab_at(i)).also_select.insert(items.begin() + 1, items.end());
    }
  };
  for (const auto &loc : locations) open(loc, {});
  for (const auto &s : shown) open(s.first, s.second);
  w->select_tab(0);
  w->layout();
  w->show();
  if (prefs.maximized && windows.size() == 1) w->maximize();
  last_active = w;
  if (!drives.started()) Fl::add_timeout(0.2, drives_cb, this);
  return w;
}

void App::drives_cb(void *data) {
  auto *self = static_cast<App *>(data);
  if (self->drives.started()) return;
  self->drives.start([self] { self->places_changed(); });
}

void App::window_closed(MainWindow *w) {
  windows.erase(std::remove(windows.begin(), windows.end(), w), windows.end());
  if (last_active == w) last_active = windows.empty() ? nullptr : windows.back();
  Fl::delete_widget(w);
  unwatch_unused();
  maybe_quit();
}

MainWindow *App::active_window() {
  if (last_active && std::find(windows.begin(), windows.end(), last_active) != windows.end()) return last_active;
  return windows.empty() ? nullptr : windows.back();
}

void App::places_changed() {
  for (auto *w : windows) w->places_changed();
}

void App::theme_changed() {
  Config cfg;
  cfg.load();
  Theme::from_config(cfg).apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  for (auto *w : windows) w->retheme();
}

void App::check_pins() {
  if (Pins::file_stamp(pins.path()) != pins.stamp()) {
    pins.load();
    places_changed();
  }
}

bool App::should_quit() {
  if (quit_requested && running_jobs() == 0) return true;
  if (!windows.empty() || running_jobs() > 0) return false;
  if (clipboard && clipboard->owned()) return false;  // other programs may still paste Liria's files
  TransferDialog *d = TransferDialog::instance();
  if (d && d->shown()) return false;
  if (service_mode && now_ms() < service_until) return false;
  return true;
}

void App::maybe_quit() {
  // The main loop asks should_quit() after every event.
  Fl::awake();
}

// ------------------------------------------------------------------ watching folders

void App::init_watch() {
  if (inotify_fd_ >= 0) return;
  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd_ < 0) return;
  Fl::add_fd(inotify_fd_, FL_READ, inotify_cb, this);
  // The pin and server files (another window or program changed them) and
  // the recently used files of the Home page.
  std::string conf = config_home() + "/sde";
  make_dirs(conf);
  config_watch_ = inotify_add_watch(inotify_fd_, conf.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
  data_watch_ = inotify_add_watch(inotify_fd_, data_home().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
}

void App::watch(const std::string &dir) {
  if (inotify_fd_ < 0 || !is_local(dir) || watched_.count(dir)) return;
  int wd = inotify_add_watch(inotify_fd_, dir.c_str(), kFolderMask | IN_ONLYDIR);
  if (wd < 0) return;
  watched_[dir] = wd;
  watches_[wd] = dir;
}

void App::unwatch_unused() {
  std::set<std::string> used;
  for (auto *w : windows)
    for (int i = 0; i < w->tab_count(); ++i) used.insert(w->tab_at(i).location);
  for (auto it = watched_.begin(); it != watched_.end();) {
    if (!used.count(it->first)) {
      inotify_rm_watch(inotify_fd_, it->second);
      watches_.erase(it->second);
      it = watched_.erase(it);
    } else {
      ++it;
    }
  }
}

void App::inotify_cb(int fd, void *data) {
  auto *self = static_cast<App *>(data);
  alignas(struct inotify_event) char buf[16384];
  bool any = false;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) break;
    for (char *p = buf; p < buf + n;) {
      auto *ev = reinterpret_cast<struct inotify_event *>(p);
      std::string name = ev->len ? std::string(ev->name) : std::string();
      if (ev->wd == self->config_watch_) {
        if (name == "liria-quick-access" || name == "liria-servers") self->config_changed_ = true;
      } else if (ev->wd == self->data_watch_) {
        if (name == "recently-used.xbel") self->recent_changed_ = true;
      } else {
        auto it = self->watches_.find(ev->wd);
        if (it != self->watches_.end()) {
          auto &names = self->pending_changes_[it->second];
          if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW) || names.size() > 400) {
            names.assign(1, std::string());  // the whole folder again
          } else if (!(names.size() == 1 && names[0].empty()) && !name.empty()) {
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
          }
        }
      }
      any = true;
      p += sizeof(struct inotify_event) + ev->len;
    }
  }
  if (any) {
    // Changes come in bursts (a copy of many files): one update for each.
    Fl::remove_timeout(changes_cb, self);
    Fl::add_timeout(0.12, changes_cb, self);
  }
}

void App::changes_cb(void *data) {
  auto *self = static_cast<App *>(data);
  auto pending = std::move(self->pending_changes_);
  self->pending_changes_.clear();
  for (auto &kv : pending) {
    std::vector<std::string> names = kv.second;
    if (names.size() == 1 && names[0].empty()) names.clear();
    for (auto *w : std::vector<MainWindow *>(self->windows)) w->folder_changed(kv.first, names);
  }
  if (self->config_changed_) {
    self->config_changed_ = false;
    bool changed = Pins::file_stamp(self->pins.path()) != self->pins.stamp();
    if (changed) self->pins.load();
    std::vector<std::string> before = self->servers.items();
    self->servers.load();
    if (changed || before != self->servers.items()) self->places_changed();
  }
  if (self->recent_changed_) {
    self->recent_changed_ = false;
    for (auto *w : self->windows)
      if (w->tab().location == kHome && w->homeview()) w->homeview()->rebuild();
  }
}

// ------------------------------------------------------------------ the theme

namespace {

void theme_cb(void *) { App::get().theme_changed(); }

int x_events(int) {
  if (!fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
    Fl::remove_timeout(theme_cb);
    Fl::add_timeout(0.05, theme_cb);
  }
  return 0;
}

}  // namespace

void App::start_services() {
  init_watch();
  clipboard = std::make_unique<FileClipboard>();
  clipboard->changed = [this] {
    for (auto *w : windows) {
      w->fileview()->scroll->redraw();
      w->update_commands();
    }
  };
  // Follow the Control Center (_SDE_CONFIG_SERIAL on the root window).
  ::Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  Fl::add_handler(x_events);
}

}  // namespace liria
}  // namespace sde
