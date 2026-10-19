// SPDX-License-Identifier: GPL-3.0-or-later
#include "system.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "config.h"
#include "process.h"
#include "util.h"

namespace sde {

std::vector<Pointer> query_pointers() {
  std::vector<Pointer> out;
  if (find_program("xinput").empty()) return out;
  std::istringstream ids(run_sync({"xinput", "list", "--id-only"}));
  std::string id;
  while (std::getline(ids, id)) {
    id = trim(id);
    if (id.empty()) continue;
    std::string props = run_sync({"xinput", "list-props", id});
    if (props.find("libinput Accel Speed") == std::string::npos) continue;
    Pointer p;
    p.id = atoi(id.c_str());
    p.touchpad = props.find("libinput Tapping Enabled") != std::string::npos;
    size_t q1 = props.find('\''), q2 = props.find('\'', q1 + 1);
    if (q1 != std::string::npos && q2 != std::string::npos) p.name = props.substr(q1 + 1, q2 - q1 - 1);
    out.push_back(p);
  }
  return out;
}

static void set_prop(int id, const char *prop, const std::string &value) {
  run_sync({"xinput", "set-prop", std::to_string(id), prop, value});
}

void apply_pointers(const Config &cfg) {
  auto speed = [](int v) {
    char buf[16];
    snprintf(buf, sizeof buf, "%.2f", std::max(-100, std::min(100, v)) / 100.0);
    return std::string(buf);
  };
  for (auto &p : query_pointers()) {
    if (p.touchpad) {
      set_prop(p.id, "libinput Accel Speed", speed(cfg.get_int("input", "touchpad_speed", 0)));
      set_prop(p.id, "libinput Tapping Enabled", cfg.get_bool("input", "touchpad_tap", true) ? "1" : "0");
      set_prop(p.id, "libinput Natural Scrolling Enabled",
               cfg.get_bool("input", "touchpad_natural_scroll", true) ? "1" : "0");
      set_prop(p.id, "libinput Disable While Typing Enabled",
               cfg.get_bool("input", "touchpad_disable_while_typing", true) ? "1" : "0");
    } else {
      set_prop(p.id, "libinput Accel Speed", speed(cfg.get_int("input", "mouse_speed", 0)));
      set_prop(p.id, "libinput Natural Scrolling Enabled",
               cfg.get_bool("input", "mouse_natural_scroll", false) ? "1" : "0");
    }
  }
}

void apply_keyboard(const Config &cfg) {
  std::string layout = cfg.get("input", "keyboard_layout");
  std::string second = cfg.get("input", "keyboard_layout2");
  if (!layout.empty() && !find_program("setxkbmap").empty()) {
    std::vector<std::string> argv{"setxkbmap", "-layout", second.empty() ? layout : layout + "," + second};
    if (!second.empty()) {
      // Alt+Shift switches between the two layouts.
      argv.push_back("-option");
      argv.push_back("");
      argv.push_back("-option");
      argv.push_back("grp:alt_shift_toggle");
    }
    run_sync(argv);
  }
  if (!find_program("xset").empty()) {
    int delay = std::max(150, std::min(1000, cfg.get_int("input", "repeat_delay", 500)));
    int rate = std::max(5, std::min(60, cfg.get_int("input", "repeat_rate", 30)));
    run_sync({"xset", "r", "rate", std::to_string(delay), std::to_string(rate)});
  }
}

std::vector<std::pair<std::string, std::string>> keyboard_layouts() {
  std::vector<std::pair<std::string, std::string>> out;
  std::string text;
  if (!read_file("/usr/share/X11/xkb/rules/evdev.lst", text)) return out;
  std::istringstream in(text);
  std::string line;
  bool section = false;
  while (std::getline(in, line)) {
    if (starts_with(line, "!")) {
      section = trim(line) == "! layout";
      continue;
    }
    if (!section) continue;
    std::string t = trim(line);
    size_t sp = t.find_first_of(" \t");
    if (sp == std::string::npos) continue;
    out.push_back({t.substr(0, sp), trim(t.substr(sp))});
  }
  std::sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.second < b.second; });
  return out;
}

void apply_all(const Config &cfg) {
  apply_display_layout(cfg);
  apply_keyboard(cfg);
  apply_pointers(cfg);
}

}  // namespace sde
