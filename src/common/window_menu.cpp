// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "window_menu.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>

#include <algorithm>
#include <cstring>

#include "config.h"
#include "i18n.h"
#include "menu.h"
#include "util.h"
#include "workspaces.h"
#include "x11.h"

namespace sde {
namespace window_menu {

namespace {

Window root() { return RootWindow(fl_display, fl_screen); }

void client_message(Window w, const char *type, long l0, long l1 = 0, long l2 = 0, long l3 = 0, long l4 = 0) {
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = w;
  e.xclient.message_type = x11::atom(type);
  e.xclient.format = 32;
  e.xclient.data.l[0] = l0;
  e.xclient.data.l[1] = l1;
  e.xclient.data.l[2] = l2;
  e.xclient.data.l[3] = l3;
  e.xclient.data.l[4] = l4;
  XSendEvent(fl_display, root(), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
  XFlush(fl_display);
}

// _NET_WM_STATE: 0 remove, 1 add, 2 toggle.
void set_state(Window w, int how, const char *a, const char *b = nullptr) {
  client_message(w, "_NET_WM_STATE", how, long(x11::atom(a)), b ? long(x11::atom(b)) : 0, 2);
}

// _NET_WM_MOVERESIZE from the pointer position; |direction| 10 is a
// keyboard move, 9 a keyboard resize.
void moveresize(Window w, long direction) {
  // The menu let go of the pointer and the keyboard: the window manager
  // grabs both for the move.
  XUngrabPointer(fl_display, CurrentTime);
  XUngrabKeyboard(fl_display, CurrentTime);
  XSync(fl_display, False);
  Window r, child;
  int x = 0, y = 0, wx, wy;
  unsigned mask;
  XQueryPointer(fl_display, root(), &r, &child, &x, &y, &wx, &wy, &mask);
  x11::activate_window(w);
  client_message(w, "_NET_WM_MOVERESIZE", x, y, direction, 0, 2);
}

}  // namespace

State read(Window w) {
  State st;
  st.win = w;
  st.title = x11::window_title(w);
  for (Atom a : x11::get_atoms(w, "_NET_WM_STATE")) {
    if (a == x11::atom("_NET_WM_STATE_HIDDEN")) st.minimized = true;
    if (a == x11::atom("_NET_WM_STATE_FULLSCREEN")) st.fullscreen = true;
    if (a == x11::atom("_NET_WM_STATE_ABOVE")) st.above = true;
  }
  st.maximized = x11::window_has_state(w, "_NET_WM_STATE_MAXIMIZED_VERT") &&
                 x11::window_has_state(w, "_NET_WM_STATE_MAXIMIZED_HORZ");
  auto d = x11::get_cardinals(w, "_NET_WM_DESKTOP");
  st.desktop = d.empty() ? 0 : d[0] == 0xFFFFFFFFul ? -1 : long(d[0]);
  auto live = workspaces::read_live(fl_display);
  st.desktops = std::max(1, live.count);
  st.current = live.current;
  Config cfg;
  cfg.load();
  auto ws = workspaces::Settings::from_config(cfg);
  for (int i = 0; i < st.desktops; ++i) {
    std::string n = i < int(live.names.size()) ? trim(live.names[size_t(i)]) : "";
    st.names.push_back(n.empty() ? ws.name(i) : n);
  }
  auto allowed = x11::get_atoms(w, "_NET_WM_ALLOWED_ACTIONS");
  if (!allowed.empty()) {
    auto has = [&](const char *name) {
      return std::find(allowed.begin(), allowed.end(), x11::atom(name)) != allowed.end();
    };
    st.can_move = has("_NET_WM_ACTION_MOVE");
    st.can_resize = has("_NET_WM_ACTION_RESIZE");
    st.can_minimize = has("_NET_WM_ACTION_MINIMIZE");
    st.can_maximize = has("_NET_WM_ACTION_MAXIMIZE_HORZ") || has("_NET_WM_ACTION_MAXIMIZE_VERT");
    st.can_close = has("_NET_WM_ACTION_CLOSE");
    st.can_above = has("_NET_WM_ACTION_ABOVE");
    st.can_change_desktop = has("_NET_WM_ACTION_CHANGE_DESKTOP");
  }
  return st;
}

void restore(const State &st) {
  if (st.minimized) x11::activate_window(st.win);
  if (st.fullscreen) set_state(st.win, 0, "_NET_WM_STATE_FULLSCREEN");
  if (st.maximized || x11::window_has_state(st.win, "_NET_WM_STATE_MAXIMIZED_VERT") ||
      x11::window_has_state(st.win, "_NET_WM_STATE_MAXIMIZED_HORZ"))
    set_state(st.win, 0, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
}

void maximize(Window w) { set_state(w, 1, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"); }

void start_move(Window w) { moveresize(w, 10); }

void start_size(Window w) { moveresize(w, 9); }

void set_above(Window w, bool on) { set_state(w, on ? 1 : 0, "_NET_WM_STATE_ABOVE"); }

void move_to_workspace(Window w, long desktop) { workspaces::move_window(fl_display, w, desktop); }

void move_to_new_workspace(Window w) {
  Config cfg;
  cfg.load();
  auto live = workspaces::read_live(fl_display);
  int count = live.count > 0 ? live.count : workspaces::Settings::from_config(cfg).count;
  if (count >= workspaces::kMax) return;
  if (!workspaces::add(fl_display, cfg)) return;
  workspaces::move_window(fl_display, w, count);
  x11::activate_window(w);
  cfg.save();
  x11::notify_config_changed();
}

void add_items(ContextMenu &m, const State &st, const Options &opts) {
  Window w = st.win;
  bool restorable = st.maximized || st.minimized || st.fullscreen;
  MenuItemOptions o;
  o.icon = "window-restore";
  o.enabled = restorable;
  m.item(_("Restore"), [st] { restore(st); }, o);
  if (opts.move_size) {
    m.item(_("Move"), [w] { start_move(w); }, {"", 0, st.can_move && !st.maximized && !st.fullscreen});
    m.item(_("Size"), [w] { start_size(w); }, {"", 0, st.can_resize && !st.maximized && !st.fullscreen});
  }
  o.icon = "window-minimize";
  o.enabled = st.can_minimize && !st.minimized;
  m.item(_("Minimize"), [w] { x11::minimize_window(w); }, o);
  o.icon = "window-maximize";
  o.enabled = st.can_maximize && !st.maximized;
  o.divider = true;
  m.item(_("Maximize"), [w] { maximize(w); }, o);
  m.check(_("Always on Top"), st.above, [w, on = st.above] { set_above(w, !on); }, st.can_above);
  ContextMenu ws;
  for (int i = 0; i < st.desktops && i < workspaces::kMax * 4; ++i) {
    std::string name = i < int(st.names.size()) ? st.names[size_t(i)] : workspaces::default_name(i);
    ws.radio(name, st.desktop == i, [w, i] { move_to_workspace(w, i); });
  }
  ws.separator();
  ws.check(_("All Workspaces"), st.desktop < 0, [w, all = st.desktop < 0, cur = st.current] {
    move_to_workspace(w, all ? cur : -1);
  });
  if (st.desktops < workspaces::kMax) ws.item(_("New Workspace"), [w] { move_to_new_workspace(w); }, {"new"});
  if (st.can_change_desktop) m.submenu(_("Move to Workspace"), std::move(ws), "workspaces");
  m.separator();
  MenuItemOptions c;
  c.icon = "close";
  c.shortcut = FL_ALT + FL_F + 4;
  c.enabled = st.can_close;
  m.item(_("Close"), [w] { x11::close_window(w); }, c);
}

}  // namespace window_menu
}  // namespace sde
