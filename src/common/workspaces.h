// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Workspaces (virtual desktops, 0.2.0 Alpha 4; docs/WINDOWS.md).
//
// SDE has 1 to 5 named workspaces, set up in Control Center > Workspaces:
//
//   [workspaces] count=2          1..5
//                name1=...name5=  empty: "Workspace N" in the user's language
//                wrap=false       Ctrl+Win+Left/Right go round from the last
//                                 workspace to the first
//                taskbar_all=false  the taskbar shows every workspace's windows
//                alt_tab_all=false  so does Alt+Tab
//
// The settings reach SDE WM twice: in its rc.xml (<desktops>, read when it
// starts) and live through EWMH (_NET_NUMBER_OF_DESKTOPS, _NET_DESKTOP_NAMES),
// because Openbox keeps the number of desktops it has when it reloads its
// configuration. Ctrl+Win+D and Ctrl+Win+F4 (sde-windows) change the count
// in sde.conf too, so that it stays the next time.
#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

namespace sde {

class Config;

namespace workspaces {

constexpr int kMin = 1;
constexpr int kMax = 5;
constexpr int kDefault = 2;  // as SDE had before 0.2.0 Alpha 4

struct Settings {
  int count = kDefault;
  std::vector<std::string> names;  // kMax entries, "" for the default name
  bool wrap = false;
  bool taskbar_all = false;
  bool alt_tab_all = false;

  Settings() : names(kMax) {}
  static Settings from_config(const Config &cfg);
  // Writes every key of [workspaces] (count clamped to 1..5).
  void save(Config &cfg) const;
  // The name shown for workspace |index| (0-based): the user's or the
  // default.
  std::string name(int index) const;
  // The names of the first |count| workspaces.
  std::vector<std::string> shown_names() const;
};

// "Workspace 3" in the user's language (|index| is 0-based).
std::string default_name(int index);
int clamp_count(int n);

// rc.xml: replaces <number> and <names> of <desktops> with |s|. Returns
// true when |rc| changed. A configuration without <desktops> gets one.
bool sync_openbox_desktops(std::string &rc, const Settings &s);

// The live state of the window manager of |dpy|.
struct Live {
  int count = 0;    // 0 without a window manager that says
  int current = 0;  // 0-based
  std::vector<std::string> names;
};
Live read_live(Display *dpy);

// Asks the window manager for |s.count| workspaces and sets their names.
// Windows on workspaces that go away move to the last one that stays.
void apply_live(Display *dpy, const Settings &s);

// One managed window for workspace changes and Task View.
struct WindowInfo {
  Window win = 0;
  long desktop = 0;  // 0-based, -1 for every workspace (sticky)
  bool minimized = false;
};
std::vector<WindowInfo> windows(Display *dpy);

// Workspace operations of Ctrl+Win+D and Ctrl+Win+F4: they change the
// window manager's workspaces and |cfg| (saved by the caller). Each returns
// false when nothing changed (already 5, only 1 left, ...).
//
// add(): a new workspace after the last one, shown at once (as on Windows).
bool add(Display *dpy, Config &cfg);
// close(): removes workspace |index| (the current one when -1): its windows
// move to the workspace before it (the next one for the first), the
// workspaces after it move down, its name goes, and the neighbour is shown.
bool close(Display *dpy, Config &cfg, int index = -1);

// The plan close() carries out, without a display (unit tested): the new
// workspace of a window on |desktop| when workspace |removed| of |count|
// closes (-1 stays -1), the names that remain, and the workspace shown.
long moved_desktop(long desktop, int removed, int count);
std::vector<std::string> names_after_close(const Settings &s, int removed);
int shown_after_close(int current, int removed, int count);

// EWMH requests (as a pager, source indication 2).
void switch_to(Display *dpy, int index);
void move_window(Display *dpy, Window w, long desktop);  // -1 = all workspaces

}  // namespace workspaces
}  // namespace sde
