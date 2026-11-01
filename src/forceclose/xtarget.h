// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The X side of Force Close (docs/FORCE-CLOSE.md): which process owns a
// window, _NET_WM_PING, the windows a program shows, and ending a window's
// program. Plain Xlib: used by sde-watchdog and sde-force-close.
#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

#include "process_rules.h"

namespace forceclose {

// The property that marks a window "Not Responding" (CARDINAL: when it
// stopped answering, seconds since the epoch). Set and removed by
// sde-watchdog, and by SDE WM when its own ping after a close attempt gets
// no answer; read by the panel, SDE WM (the title) and the dialog.
constexpr const char *kNotResponding = "_SDE_NOT_RESPONDING";

// The process of the X client that owns |w|: the X server's own record of
// the connection (X Resource extension 1.2, XResQueryClientIds; right for
// Flatpak too), else _NET_WM_PID when WM_CLIENT_MACHINE names this computer.
// 0 when it cannot be known; *remote is set when the client runs on
// another computer.
long window_pid(Display *dpy, Window w, bool *remote = nullptr);
// The X Resource extension's answer alone (0 without it).
long xres_pid(Display *dpy, XID resource);

// Managed windows (_NET_CLIENT_LIST); *has_list is false without a window
// manager that keeps the list.
std::vector<Window> client_list(Display *dpy, bool *has_list = nullptr);

// _NET_WM_PING (EWMH): whether |w| lists it in WM_PROTOCOLS, sending one
// (the |id| comes back), and reading the answers, which clients send to the
// root window (select SubstructureNotifyMask on it to receive them).
bool supports_ping(Display *dpy, Window w);
void send_ping(Display *dpy, Window w, unsigned long id);
bool is_pong(Display *dpy, const XEvent &e, Window *w, unsigned long *id);

// Whether program |pid| (or a process of its family, see family()) shows
// a window: a managed window, or any window of one of its X connections
// that is mapped (a tray icon, say).
bool family_shows_window(Display *dpy, long pid, const std::string &proc = "/proc");

// Ends the program of window |w| at once: SIGKILL to its process
// (kill_now(), so only what check() allows), then XKillClient for its
// connection. A client on another computer is only disconnected. With
// |dry_run| nothing is sent. |pid| receives the process (0 if none).
Verdict force_close_window(Display *dpy, Window w, const Context &ctx, bool dry_run, long *pid = nullptr);

}  // namespace forceclose
