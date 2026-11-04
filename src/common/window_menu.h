// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The window menu (0.2.0 Alpha 4; docs/WINDOWS.md): what SDE shows for a
// window instead of Openbox's client-menu, from the title bar's icon and
// right button, Alt+Space (sde-window-menu, through SDE WM patch 0006) and,
// with the same items in the same order, on the window's taskbar button:
//
//   Restore, Move, Size, Minimize, Maximize
//   Always on Top (check)
//   Move to Workspace > the workspaces by name, All Workspaces, New Workspace
//   Close  Alt+F4
//
// Everything is an EWMH request to the window manager, on FLTK's display.
#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

namespace sde {

class ContextMenu;

namespace window_menu {

struct State {
  Window win = 0;
  std::string title;
  bool maximized = false;  // both ways
  bool minimized = false;
  bool fullscreen = false;
  bool above = false;
  long desktop = 0;  // 0-based, -1 on every workspace
  int desktops = 1;
  int current = 0;
  std::vector<std::string> names;  // of the |desktops| workspaces
  // _NET_WM_ALLOWED_ACTIONS; everything when the window manager does not say.
  bool can_move = true, can_resize = true, can_minimize = true, can_maximize = true, can_close = true,
       can_above = true, can_change_desktop = true;
};

// The state of |w| (fl_display).
State read(Window w);

struct Options {
  bool move_size = true;  // Move and Size (keyboard move and resize)
};

// Adds the window's items to |m| (see above), each acting on |st.win|.
void add_items(ContextMenu &m, const State &st, const Options &opts = {});

// The requests behind the items.
void restore(const State &st);
void maximize(Window w);
void start_move(Window w);  // move with the arrow keys or the mouse; Enter ends
void start_size(Window w);
void set_above(Window w, bool on);
// -1: every workspace.
void move_to_workspace(Window w, long desktop);
// A new workspace (as Ctrl+Win+D, saved in sde.conf) with |w| on it.
void move_to_new_workspace(Window w);

}  // namespace window_menu
}  // namespace sde
