// SPDX-License-Identifier: GPL-3.0-or-later
// State shared by the sde-settings pages.
#pragma once

#include <string>

#include "avatar.h"
#include "avatar_view.h"
#include "config.h"
#include "theme.h"

namespace sde {

class Page;
class Painter;

Config &settings_config();
const Theme &settings_theme();

// Saves the configuration and tells the running desktop about it.
void apply_change();
// Rebuilds the visible page (after its content changed shape).
void refresh_page();

void build_display_page(Page *page);
void start_display_watch();
void build_input_page(Page *page);
void build_touchpad_page(Page *page);  // touchpad.cpp
// Settings > Power and Settings > Lock Screen (power.cpp).
void build_power_page(Page *page);
void build_lock_page(Page *page);
// Settings > Network: the Proxy and Troubleshooting cards (network.cpp).
void build_network_proxy(Page *page);
void build_network_diagnostics(Page *page);
// Settings > Clock & Weather (clock.cpp, alpha52).
void build_clock_page(Page *page);
// Settings > Default Applications (default_apps_page.cpp).
void build_default_apps_page(Page *page);
// Rebuilds the visible page when it is |page_id| (after background work).
void refresh_if_shown(const char *page_id);

// Shows page |id| ("users"), scrolled to its |anchor| (Page::anchor()).
void open_settings_page(const std::string &id, const std::string &anchor = "");
// Id of the page shown.
const std::string &current_settings_page();
// The native symbol of a page or overview tile (main.cpp).
void draw_settings_glyph(Painter &p, const std::string &id, double x, double y, double size,
                         const Color &c);
// UI test hook: an extra line of the QA dump (Control-Center.txt) for
// something that is not a named widget, in window pixels.
void qa_line(const std::string &name, int x, int y, int w, int h);

// The overview: every setting by category, a search, tiles for the
// separate tools (hub.cpp).
void build_hub(Page *page);
// Symbols of the overview's tools and of the newer pages; false for others.
bool hub_glyph(Painter &p, const std::string &id, double x, double y, double size, const Color &c);

// Users & Avatar (users.cpp): the account and its picture, looked up in the
// background and again when another program changes them.
void build_users_page(Page *page);
void watch_account();
void reload_account();
// "Choose a picture..." (also sde-settings --choose-avatar).
void choose_avatar_picture();
const avatar::Account &settings_account();
avatar::Picture &settings_avatar();

}  // namespace sde
