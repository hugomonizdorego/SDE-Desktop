// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

#include "config.h"
namespace sde {
void sync_gtk_settings(const Config &cfg);
// GTK theme SDE uses for its suite and the GTK settings files: "SDE-Aero"
// when Aero glass is enabled and installed, otherwise "Adwaita".
std::string gtk_theme_name(const Config &cfg);
// Writes the SDE Aero compositor configuration and returns its path. |safe|
// selects the XRender backend, used when OpenGL is unavailable.
std::string write_compositor_config(const Config &cfg, bool safe = false);
// Asks a running SDE compositor to reload its configuration.
void reload_compositor();
void migrate_legacy_suite_shortcuts();
}
