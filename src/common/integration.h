// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

#include "config.h"
namespace sde {
void sync_gtk_settings(const Config &cfg);
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
// Asks a running SDE compositor to reload its configuration.
void reload_compositor();
// Asks sde-session to stop the compositor and start it again with the
// current settings (or leave it off). Enabling it no longer needs a login.
void restart_compositor_now();
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
