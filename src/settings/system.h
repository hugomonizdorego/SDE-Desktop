// SPDX-License-Identifier: GPL-3.0-or-later
// Display, keyboard and pointer settings, applied with the standard X tools
// (xrandr, setxkbmap, xset, xinput). Used by the Settings pages and by
// "sde-settings --apply", which sde-session runs at login.
#pragma once

#include <string>
#include <vector>
#include "../display/backend.h"

namespace sde {

class Config;

struct Pointer {
  int id = 0;
  std::string name;
  bool touchpad = false;
};
std::vector<Pointer> query_pointers();  // libinput devices only

void apply_keyboard(const Config &cfg);
void apply_pointers(const Config &cfg);

// Keyboard layouts known to XKB: code and description, sorted by description.
std::vector<std::pair<std::string, std::string>> keyboard_layouts();

// Everything above; used at login.
void apply_all(const Config &cfg);

}  // namespace sde
