// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "apps.h"

#include <FL/x.H>

#include <set>
#include <thread>

#include "desktop_entry.h"
#include "util.h"
#include "x11.h"

namespace sysmon {

AppIndex &AppIndex::instance() {
  static AppIndex index;
  return index;
}

void AppIndex::load_async() {
  if (started_.exchange(true)) return;
  std::thread([this] { load(); }).detach();
}

void AppIndex::load() {
  // Visible SDE applications; their StartupWMClass, id and Exec name windows.
  auto entries = sde::load_applications("SDE");
  for (auto &e : entries) {
    AppInfo a{e.id, e.name, e.icon};
    size_t index = apps_.size();
    apps_.push_back(a);
    std::string id = sde::lower(e.id);
    if (sde::ends_with(id, ".desktop")) id = id.substr(0, id.size() - 8);
    ids_.emplace(id, index);
    // Reverse-DNS ids: "org.gnome.Calculator" also answers to "calculator".
    size_t dot = id.rfind('.');
    if (dot != std::string::npos) ids_.emplace(id.substr(dot + 1), index);
    if (!e.startup_wm_class.empty()) wm_class_.emplace(sde::lower(e.startup_wm_class), index);
    auto argv = sde::split_command(e.exec);
    size_t first = 0;
    // "env FOO=bar program" and flatpak wrappers start with a launcher.
    while (first < argv.size() && (argv[first] == "env" || argv[first].find('=') != std::string::npos))
      ++first;
    if (first < argv.size()) exes_.emplace(sde::lower(sde::basename_of(argv[first])), index);
  }
  ready_.store(true, std::memory_order_release);
}

const AppInfo *AppIndex::by_wm_class(const std::string &wm_class) const {
  if (!ready() || wm_class.empty()) return nullptr;
  std::string key = sde::lower(wm_class);
  for (const auto *map : {&wm_class_, &ids_, &exes_}) {
    auto it = map->find(key);
    if (it != map->end()) return &apps_[it->second];
  }
  return nullptr;
}

const AppInfo *AppIndex::by_desktop_file(const std::string &path) const {
  if (!ready() || path.empty()) return nullptr;
  std::string id = sde::lower(sde::basename_of(path));
  if (sde::ends_with(id, ".desktop")) id = id.substr(0, id.size() - 8);
  auto it = ids_.find(id);
  return it == ids_.end() ? nullptr : &apps_[it->second];
}

const AppInfo *AppIndex::by_executable(const std::string &exe) const {
  if (!ready() || exe.empty()) return nullptr;
  auto it = exes_.find(sde::lower(exe));
  return it == exes_.end() ? nullptr : &apps_[it->second];
}

// ------------------------------------------------------------ X windows

std::map<int, WindowOwner> scan_windows() {
  std::map<int, WindowOwner> out;
  if (!fl_display) return out;
  Window root = DefaultRootWindow(fl_display);
  for (Window w : sde::x11::get_windows(root, "_NET_CLIENT_LIST")) {
    auto pid = sde::x11::get_cardinals(w, "_NET_WM_PID");
    if (pid.empty() || pid[0] == 0) continue;
    if (!sde::x11::window_is_normal(w)) continue;
    auto &o = out[int(pid[0])];
    if (o.window) continue;  // keep the first (oldest) window
    o.window = w;
    o.wm_class = sde::x11::window_class(w);
  }
  return out;
}

namespace {
std::map<Window, cairo_surface_t *> g_icons;
}

cairo_surface_t *cached_window_icon(Window w, int size) {
  auto it = g_icons.find(w);
  if (it != g_icons.end()) return it->second;
  cairo_surface_t *s = sde::x11::window_icon(w, size);
  g_icons[w] = s;  // may be null: remember that too
  return s;
}

void forget_window_icons(const std::map<int, WindowOwner> &alive) {
  std::set<Window> live;
  for (auto &kv : alive) live.insert(kv.second.window);
  for (auto it = g_icons.begin(); it != g_icons.end();) {
    if (live.count(it->first)) {
      ++it;
      continue;
    }
    if (it->second) cairo_surface_destroy(it->second);
    it = g_icons.erase(it);
  }
}

Resolved resolve_process(const ProcInfo &p, const WindowOwner *owner) {
  Resolved r;
  r.name = p.name.empty() ? p.comm : p.name;
  const AppIndex &index = AppIndex::instance();
  const AppInfo *app = nullptr;
  if (owner) {
    r.app = true;
    r.window = owner->window;
    app = index.by_wm_class(owner->wm_class);
    if (!app) app = index.by_executable(p.exe_base);
    if (!app) app = index.by_executable(p.comm);
    if (!app && !owner->wm_class.empty()) r.name = owner->wm_class;
  } else if (!p.desktop_hint.empty()) {
    app = index.by_desktop_file(p.desktop_hint);
    r.app = app != nullptr;
  }
  if (app) {
    if (!app->name.empty()) r.name = app->name;
    r.icon = app->icon;
  }
  return r;
}

}  // namespace sysmon
