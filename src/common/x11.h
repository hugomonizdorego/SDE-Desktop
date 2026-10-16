// SPDX-License-Identifier: GPL-3.0-or-later
// EWMH/ICCCM helpers built on FLTK's X11 connection (fl_display).
#pragma once

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <cairo.h>

#include <string>
#include <vector>

namespace sde {
namespace x11 {

Atom atom(const char *name);

// Window properties.
std::vector<unsigned long> get_cardinals(Window w, const char *prop, Atom type = XA_CARDINAL);
std::vector<Window> get_windows(Window w, const char *prop);
Window get_window(Window w, const char *prop);
std::vector<Atom> get_atoms(Window w, const char *prop);
std::string get_utf8(Window w, const char *prop);
std::string get_string(Window w, const char *prop);
void set_atoms(Window w, const char *prop, const std::vector<const char *> &values);
void set_cardinals(Window w, const char *prop, const std::vector<long> &values);
void set_string(Window w, const char *prop, const std::string &value);

// EWMH window queries used by the taskbar.
std::string window_title(Window w);
bool window_has_state(Window w, const char *state);
bool window_is_normal(Window w);        // not a dock, desktop, menu, ...
std::string window_class(Window w);     // WM_CLASS res_class
// Best _NET_WM_ICON image for |size|, scaled. Caller owns the surface.
cairo_surface_t *window_icon(Window w, int size);

// EWMH actions.
void activate_window(Window w);
void minimize_window(Window w);
void close_window(Window w);
void toggle_maximize(Window w);
void set_showing_desktop(bool show);
bool showing_desktop();

// Re-maps an already mapped FLTK window so that the window manager notices
// type, strut and state properties set after FLTK's show().
void remap(Window w);

// Window type and struts for panels and the desktop window.
void set_window_type(Window w, const char *type);  // e.g. "_NET_WM_WINDOW_TYPE_DOCK"
void set_bottom_strut(Window w, int height, int x_start, int x_end, int screen_h);

// Simple SDE inter-process messages. A command is written to the root window
// property _SDE_COMMAND; programs that watch the root window react to it.
void send_command(const std::string &command);
std::string read_command();
// Announce that ~/.config/sde/sde.conf changed (bumps _SDE_CONFIG_SERIAL).
void notify_config_changed();

// True when a EWMH window manager is running (_NET_SUPPORTING_WM_CHECK).
bool wm_running(Display *dpy);

}  // namespace x11
}  // namespace sde
