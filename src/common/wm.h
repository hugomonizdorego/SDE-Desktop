// SPDX-License-Identifier: GPL-3.0-or-later
// Window manager integration. SDE 0.1 uses Openbox with an SDE configuration.
#pragma once

#include <string>
#include <vector>

namespace sde {

// ~/.config/sde/openbox/rc.xml (created from the packaged template if missing).
std::string openbox_config_path();
bool ensure_openbox_config();

// Points the Openbox configuration at an SDE window theme, e.g.
// "SDE-Graphite-Light-Blue", and sets the button layout ("NLIMC" since
// alpha50: minimize, maximize and close on the right in every family; the
// old left "CIML" of Graphite and Glass is migrated). A theme or layout the
// user chose by hand is left alone. True when the file changed.
bool set_openbox_theme(const std::string &theme, const std::string &title_layout);
// Older form: |style| is a family (or an alpha15 style), blue accent.
bool set_openbox_theme(bool dark, const std::string &style = "graphite");

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

// Fits a new window of up to |w| x |h| into the work area of the screen
// under the pointer, frame included, and sets |x|,|y| to centre it there.
// FLTK asks for StaticGravity: the window manager keeps the client where it
// was asked to be and puts the title bar above it, so a window centred
// without room for the title bar had it cut off at the top of the screen
// (1920x1080 at 150 %, alpha 2). The frame size is estimated from SDE WM's
// theme at the current scale, because the window is not mapped yet.
void fit_to_work_area(int &x, int &y, int &w, int &h);

}  // namespace sde
