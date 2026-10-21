// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE System Monitor: tells applications from background processes. A
// process is an app when it owns a top-level window (_NET_WM_PID) or was
// started from a .desktop file; its name and icon come from that entry.
#pragma once

#include <X11/Xlib.h>
#include <cairo.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "sampler.h"

namespace sysmon {

struct AppInfo {
  std::string id, name, icon;
};

class AppIndex {
 public:
  static AppIndex &instance();
  // Reads the installed .desktop files on a helper thread.
  void load_async();
  bool ready() const { return ready_.load(std::memory_order_acquire); }

  const AppInfo *by_wm_class(const std::string &wm_class) const;
  const AppInfo *by_desktop_file(const std::string &path) const;
  const AppInfo *by_executable(const std::string &exe) const;

 private:
  void load();
  std::atomic<bool> ready_{false}, started_{false};
  std::vector<AppInfo> apps_;
  std::map<std::string, size_t> wm_class_, ids_, exes_;
};

struct WindowOwner {
  Window window = 0;
  std::string wm_class, title;
};

// Top-level client windows by owning pid (from _NET_CLIENT_LIST).
std::map<int, WindowOwner> scan_windows();

// Window icon (_NET_WM_ICON) cached per window; never free the result.
cairo_surface_t *cached_window_icon(Window w, int size);
void forget_window_icons(const std::map<int, WindowOwner> &alive);

// Resolves what to show for |p| when it owns |owner| (may be null).
struct Resolved {
  bool app = false;
  std::string name, icon;
  Window window = 0;
};
Resolved resolve_process(const ProcInfo &p, const WindowOwner *owner);

}  // namespace sysmon
