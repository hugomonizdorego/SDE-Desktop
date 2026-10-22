// SPDX-License-Identifier: GPL-3.0-or-later
// Touchpad options (alpha50): what SDE Settings > Touchpad and the panel's
// touchpad flyout change, saved in sde.conf and applied through libinput's X
// properties (xinput set-prop). sde-settings --apply, run by sde-session at
// login, applies them with apply_touchpad().
//
// Keys: [touchpad] enabled, tap_drag, scroll (two-finger, edge, none), click
// (areas, clickfinger); tap, natural scrolling, disable while typing and the
// speed keep their alpha16 keys in [input] (touchpad_tap,
// touchpad_natural_scroll, touchpad_disable_while_typing, touchpad_speed), so
// the Keyboard & Mouse page and this page always agree.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sde {

class Config;

struct TouchpadOptions {
  bool enabled = true, tap = true, tap_drag = true, natural = true, disable_typing = true;
  std::string scroll = "two-finger";  // two-finger, edge or none
  std::string click = "areas";        // areas or clickfinger
  int speed = 0;                      // -100 .. 100
};

TouchpadOptions load_touchpad_options(const Config &cfg);
void save_touchpad_options(Config &cfg, const TouchpadOptions &o);

// xinput set-prop arguments (property name, values) for |o|, limited to the
// properties |available| lists (the names from xinput list-props).
std::vector<std::pair<std::string, std::vector<std::string>>> touchpad_props(
    const TouchpadOptions &o, const std::vector<std::string> &available);
// Property names of `xinput list-props` output.
std::vector<std::string> parse_prop_names(const std::string &text);

struct TouchpadDevice {
  int id = 0;
  std::string name;
  std::vector<std::string> props;
  bool enabled = true;
};
// libinput touchpads (devices with "libinput Tapping Enabled"), via xinput.
// Blocks for a moment: call it from a helper or at startup only.
std::vector<TouchpadDevice> find_touchpads();
// Applies the saved options to every touchpad. Returns the number found.
int apply_touchpad(const Config &cfg);
// The same for touchpads found earlier.
void apply_touchpad(const Config &cfg, const std::vector<TouchpadDevice> &pads);

}  // namespace sde
