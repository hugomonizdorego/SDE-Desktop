// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The on-screen display of SDE Power: a small themed card near the bottom of
// the screen with the pointer that shows the brightness after a key press.
#pragma once

#include <string>

namespace sde {
namespace power {

enum class OsdKind { Brightness, Battery };

// Shows (or updates) the OSD; it hides itself after |seconds|.
void show_osd(OsdKind kind, int percent, const std::string &label = "", double seconds = 1.6);
// Re-reads colors and sizes from the theme (after a configuration change).
void osd_reload_theme();

}  // namespace power
}  // namespace sde
