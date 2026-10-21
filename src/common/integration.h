// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

#include "config.h"
namespace sde {
void sync_gtk_settings(const Config &cfg);
// GTK theme SDE uses for its suite and the GTK settings files: "SDE-Aero"
// when Aero glass is enabled and installed, otherwise "Adwaita".
std::string gtk_theme_name(const Config &cfg);
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
// Backend for [compositor] backend=auto: "xrender" in virtual machines and
// without a DRM device, otherwise "glx".
std::string auto_compositor_backend();
// "full" or "lite" compositor effects for [compositor] effects=auto|full|lite.
std::string compositor_effects(const Config &cfg, bool safe = false);
void migrate_legacy_suite_shortcuts();
}
