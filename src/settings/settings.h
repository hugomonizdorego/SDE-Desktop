// SPDX-License-Identifier: GPL-3.0-or-later
// State shared by the sde-settings pages.
#pragma once

#include "config.h"
#include "theme.h"

namespace sde {

class Page;

Config &settings_config();
const Theme &settings_theme();

// Saves the configuration and tells the running desktop about it.
void apply_change();
// Rebuilds the visible page (after its content changed shape).
void refresh_page();

void build_display_page(Page *page);
void start_display_watch();
void build_input_page(Page *page);
// Settings > Power and Settings > Lock Screen (power.cpp).
void build_power_page(Page *page);
void build_lock_page(Page *page);

}  // namespace sde
