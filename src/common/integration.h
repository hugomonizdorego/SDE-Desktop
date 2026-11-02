// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <X11/Xlib.h>
#include <sys/types.h>

#include <string>

#include "config.h"
namespace sde {
// Writes the GTK settings: settings.ini, the managed block of gtk.css, the
// GSettings keys (in one change) and the XSETTINGS file. True when the
// XSETTINGS file changed. Nothing is signalled (0.2.0 Alpha 3.1): the
// session, or deliver_appearance() without one, tells sde-xsettingsd.
bool sync_gtk_settings(const Config &cfg);
// The block SDE manages in ~/.config/gtk-|gtk_major|.0/gtk.css for |cfg|: a
// color of the user's own, translucent menus (and in GTK 4 the theme's
// libadwaita colors). The theme preview gives it to its GTK widgets.
std::string gtk_user_css(const Config &cfg, int gtk_major);
// GTK theme SDE uses for its suite and the GTK settings files, e.g.
// "SDE-Graphite-Light-Blue" (the light or dark variant is part of the name);
// "Adwaita" or "Adwaita-dark" when the SDE themes are not installed.
std::string gtk_theme_name(const Config &cfg);
// Icon theme for GTK, the same one SDE programs use: [appearance]
// icon_theme, or for "auto" (and themes that are not installed) Papirus /
// Papirus-Dark when installed, otherwise Adwaita (resolve_icon_theme()).
std::string gtk_icon_theme_name(const Config &cfg, bool dark);
// "hardware" (a GPU with a 3D engine), "virtual" (VM/paravirtual or
// display-only DRM device) or "software" (no DRM device, e.g. Xvfb).
// SDE_GRAPHICS overrides the detection; LIBGL_ALWAYS_SOFTWARE forces software.
std::string detect_graphics_class();
// Writes the SDE Aero compositor configuration and returns its path. |safe|
// selects the XRender backend, used when OpenGL is unavailable. Blur,
// rounded corners and fades are left out on VMs and software rendering
// unless [compositor] effects=full.
std::string write_compositor_config(const Config &cfg, bool safe = false);
// Asks the compositor of this X display to reload its configuration (in
// place, see suite/patches/sde-compositor/0003). Only that process is
// signalled: the owner of _NET_WM_CM_S<screen> (_NET_WM_PID), never every
// compositor of the user (pkill did, up to 0.2.0 Alpha 3).
void reload_compositor(Display *dpy = nullptr);
// Asks the sde-session of this display to stop the compositor and start it
// again with the current settings (or leave it off).
void restart_compositor_now();

// ---- Switching the theme (0.2.0 Alpha 3.1; docs/THEMES.md, "Switching themes")
//
// A change of the look reaches every program in one step: everything
// derived from sde.conf is written first, then each program is told once,
// and only when its input changed: sde-xsettingsd re-reads its file (GTK),
// SDE WM reconfigures, the compositor reloads in place, and SDE programs
// follow _SDE_CONFIG_SERIAL. The session of the display does this behind a
// picture of the screen that then fades to the new look.

// What changed in the files the running programs read.
struct AppearanceFiles {
  bool gtk = false;         // ~/.config/sde/xsettingsd.conf (GTK theme, icons, fonts, DPI)
  bool wm = false;          // SDE WM's rc.xml (window theme, button layout, title font)
  bool compositor = false;  // the generated compositor configuration or its shaders
  bool any() const { return gtk || wm || compositor; }
};
// Writes everything the running programs read from |cfg|: GTK settings and
// XSETTINGS, SDE WM's configuration, the compositor configurations and
// their shaders. Signals nobody.
AppearanceFiles write_appearance_files(const Config &cfg);
// The text the compositor's reload depends on: the generated configuration
// and the shaders it names ("" when the compositor is off or the user has a
// compositor.conf of their own).
std::string compositor_inputs(const Config &cfg);
// Tells the programs of this X display what changed: sde-xsettingsd (the
// owner of _XSETTINGS_S<screen>) and the compositor by their _NET_WM_PID,
// SDE WM with its _OB_CONTROL message. For programs that run without a
// session; the session signals its own children.
void deliver_appearance(Display *dpy, const AppearanceFiles &changed);
// The process that owns selection |name| on this display (its _NET_WM_PID)
// when it runs as this user and its name is one of |programs| ("a|b"); else 0.
pid_t selection_owner_pid(Display *dpy, const std::string &name, const std::string &programs);
// Asks the session of this display (the owner of _SDE_SESSION_S<screen>) to
// apply the appearance saved in sde.conf in one calm step. Returns the
// request number the session reports in _SDE_APPEARANCE_DONE on its window
// once the new look is shown, or 0 when no session runs.
long request_appearance_apply(Display *dpy);
// Without a session: writes the files, tells this display's programs and
// announces the change (_SDE_CONFIG_SERIAL), at once and without a fade.
void apply_appearance_now(Display *dpy, const Config &cfg);
// Applies the appearance saved in sde.conf: through the session when one
// runs (true), otherwise at once (false).
bool apply_appearance(Display *dpy, const Config &cfg);
// Whether a session coordinates this display (owns _SDE_SESSION_S<screen>).
bool session_coordinates(Display *dpy);
// Announces a saved configuration change (_SDE_CONFIG_SERIAL) after writing
// what the running programs read from it; the session tells GTK, the window
// manager and the compositor what changed, without a session this does.
void announce_config_change(Display *dpy, const Config &cfg);
// Plain-text report: mode, backend and why, running state, virtual machine,
// OpenGL renderer, configuration file and glass settings (after Eduka's
// eduka-compositor report).
std::string compositor_report(const Config &cfg);
// The hypervisor's name ("VirtualBox", ...) or "" on real hardware.
std::string virtual_machine_name();
// The OpenGL renderer of the display (0.2.0 Alpha 3.1), found once per login
// by sde-session with its helper sde-gl-probe: state "accelerated",
// "software" (Mesa's llvmpipe, softpipe or swrast, or a renderer that says
// it is not accelerated: a GPU without its OpenGL driver) or "none" (no
// GLX), and the renderer's name or why there is none; state "" when it was
// not probed (outside an SDE session, no graphics device, or the helper
// failed or took too long).
struct OpenGLInfo {
  std::string state, detail;
};
// Parses sde-gl-probe's line ("glx=1 accelerated=0 renderer=llvmpipe ...",
// "glx=0 reason=...").
OpenGLInfo parse_opengl_probe(const std::string &output);
// What sde-session found: $SDE_OPENGL ("state:detail", inherited by the
// session's programs), else the same in $XDG_RUNTIME_DIR/sde-opengl.
OpenGLInfo opengl_info();
// Publishes |gl| to this process's children and the runtime directory
// (sde-session at login); an empty state removes both.
void publish_opengl_info(const OpenGLInfo &gl);
// The automatic compositor profile ([compositor] backend and effects auto):
// OpenGL with every effect on a graphics device whose OpenGL is accelerated
// (or was not probed); XRender with the lite profile in virtual machines,
// without a graphics device, without GLX and when OpenGL renders in
// software, where the effects would run on the CPU. |why| says which.
struct AutoProfile {
  std::string backend, effects, why;
};
AutoProfile auto_profile(const std::string &graphics_class, const OpenGLInfo &gl);
// Backend for [compositor] backend=auto: auto_profile()'s, for this machine.
std::string auto_compositor_backend();
// "full" or "lite" compositor effects for [compositor] effects=auto|full|lite.
std::string compositor_effects(const Config &cfg, bool safe = false);
// The backend SDE's generated configuration uses: "glx" or "xrender"
// ([compositor] backend, auto resolved; [compositor] effects=full chosen by
// hand keeps OpenGL on a graphics device even when it renders in software),
// or "" when the compositor is off or runs a compositor.conf of the user's own.
std::string compositor_backend(const Config &cfg);
void migrate_legacy_suite_shortcuts();
}
