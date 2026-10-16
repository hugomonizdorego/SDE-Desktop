// SPDX-License-Identifier: GPL-3.0-or-later
// Display, keyboard and pointer settings, applied with the standard X tools
// (xrandr, setxkbmap, xset, xinput). Used by the Settings pages and by
// "sde-settings --apply", which sde-session runs at login.
#pragma once

#include <string>
#include <vector>

namespace sde {

class Config;

struct Output {
  std::string name;              // "HDMI-1"
  bool connected = false;
  bool enabled = false;          // has a mode set
  bool primary = false;
  int x = 0, y = 0, width = 0, height = 0;
  std::string current_mode;      // "1920x1080"
  std::string preferred_mode;
  std::vector<std::string> modes;  // largest first, as listed by xrandr
};

// Parses "xrandr --query" output.
std::vector<Output> parse_xrandr(const std::string &text);
std::vector<Output> query_outputs();

// Command line arguments (after "xrandr") that recreate |outputs|.
std::vector<std::string> xrandr_args(const std::vector<Output> &outputs);

// Saved display layout: applied only when the same monitors are connected.
void save_display_layout(Config &cfg, const std::vector<Output> &outputs);
bool apply_display_layout(const Config &cfg);

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
