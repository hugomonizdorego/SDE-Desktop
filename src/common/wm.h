// SPDX-License-Identifier: GPL-3.0-or-later
// Window manager integration. SDE 0.1 uses Openbox with an SDE configuration.
#pragma once

#include <string>
#include <vector>

namespace sde {

// ~/.config/sde/openbox/rc.xml (created from the packaged template if missing).
std::string openbox_config_path();
bool ensure_openbox_config();

// Points the Openbox configuration at the SDE or SDE-Dark window theme.
// Returns true when the file changed.
bool set_openbox_theme(bool dark);

// Sets the font size (points) of all Openbox fonts, e.g. to follow the UI
// scale. Returns true when the file changed.
bool set_openbox_font_size(int points);

// Applies the appearance settings (theme, font size x scale) to Openbox.
// Returns true when the configuration changed.
bool sync_openbox(const class Config &cfg);

// Command line that starts the configured window manager.
std::vector<std::string> window_manager_command(const std::string &configured);

// Asks a running Openbox to reload its configuration.
void reconfigure_window_manager();

}  // namespace sde
