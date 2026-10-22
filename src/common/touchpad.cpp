// SPDX-License-Identifier: GPL-3.0-or-later
#include "touchpad.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "config.h"
#include "process.h"
#include "util.h"

namespace sde {

TouchpadOptions load_touchpad_options(const Config &cfg) {
  TouchpadOptions o;
  o.enabled = cfg.get_bool("touchpad", "enabled", true);
  o.tap = cfg.get_bool("input", "touchpad_tap", true);
  o.tap_drag = cfg.get_bool("touchpad", "tap_drag", true);
  o.natural = cfg.get_bool("input", "touchpad_natural_scroll", true);
  o.disable_typing = cfg.get_bool("input", "touchpad_disable_while_typing", true);
  o.scroll = cfg.get("touchpad", "scroll", "two-finger");
  if (o.scroll != "edge" && o.scroll != "none") o.scroll = "two-finger";
  o.click = cfg.get("touchpad", "click", "areas");
  if (o.click != "clickfinger") o.click = "areas";
  o.speed = std::max(-100, std::min(100, cfg.get_int("input", "touchpad_speed", 0)));
  return o;
}

void save_touchpad_options(Config &cfg, const TouchpadOptions &o) {
  auto b = [](bool v) { return std::string(v ? "true" : "false"); };
  cfg.set("touchpad", "enabled", b(o.enabled));
  cfg.set("input", "touchpad_tap", b(o.tap));
  cfg.set("touchpad", "tap_drag", b(o.tap_drag));
  cfg.set("input", "touchpad_natural_scroll", b(o.natural));
  cfg.set("input", "touchpad_disable_while_typing", b(o.disable_typing));
  cfg.set("touchpad", "scroll", o.scroll);
  cfg.set("touchpad", "click", o.click);
  cfg.set("input", "touchpad_speed", std::to_string(o.speed));
}

std::vector<std::string> parse_prop_names(const std::string &text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string l;
  while (std::getline(in, l)) {
    std::string t = trim(l);
    size_t paren = t.find(" (");
    if (paren == std::string::npos || t.find("):") == std::string::npos) continue;
    out.push_back(t.substr(0, paren));
  }
  return out;
}

std::vector<std::pair<std::string, std::vector<std::string>>> touchpad_props(
    const TouchpadOptions &o, const std::vector<std::string> &available) {
  std::vector<std::pair<std::string, std::vector<std::string>>> out;
  auto has = [&](const char *name) {
    return std::find(available.begin(), available.end(), name) != available.end();
  };
  auto set = [&](const char *name, std::vector<std::string> values) {
    if (has(name)) out.push_back({name, std::move(values)});
  };
  auto b = [](bool v) { return std::string(v ? "1" : "0"); };
  set("Device Enabled", {b(o.enabled)});
  set("libinput Tapping Enabled", {b(o.tap)});
  set("libinput Tapping Drag Enabled", {b(o.tap_drag)});
  set("libinput Natural Scrolling Enabled", {b(o.natural)});
  set("libinput Disable While Typing Enabled", {b(o.disable_typing)});
  // Scroll methods: two-finger, edge, on-button-down.
  set("libinput Scroll Method Enabled", {b(o.scroll == "two-finger"), b(o.scroll == "edge"), "0"});
  // Click methods: button areas, clickfinger.
  set("libinput Click Method Enabled", {b(o.click == "areas"), b(o.click == "clickfinger")});
  char speed[16];
  snprintf(speed, sizeof speed, "%.2f", std::max(-100, std::min(100, o.speed)) / 100.0);
  set("libinput Accel Speed", {speed});
  return out;
}

std::vector<TouchpadDevice> find_touchpads() {
  std::vector<TouchpadDevice> out;
  if (find_program("xinput").empty()) return out;
  std::istringstream ids(run_sync({"xinput", "list", "--id-only"}, nullptr, 3000));
  std::string id;
  while (std::getline(ids, id)) {
    id = trim(id);
    if (id.empty()) continue;
    std::string props = run_sync({"xinput", "list-props", id}, nullptr, 3000);
    if (props.find("libinput Tapping Enabled") == std::string::npos) continue;
    TouchpadDevice d;
    d.id = atoi(id.c_str());
    size_t q1 = props.find('\''), q2 = props.find('\'', q1 + 1);
    if (q1 != std::string::npos && q2 != std::string::npos) d.name = props.substr(q1 + 1, q2 - q1 - 1);
    d.props = parse_prop_names(props);
    size_t en = props.find("Device Enabled (");
    if (en != std::string::npos) {
      size_t colon = props.find("):", en);
      if (colon != std::string::npos) d.enabled = atoi(props.c_str() + colon + 2) != 0;
    }
    out.push_back(d);
  }
  return out;
}

int apply_touchpad(const Config &cfg) {
  auto pads = find_touchpads();
  apply_touchpad(cfg, pads);
  return int(pads.size());
}

void apply_touchpad(const Config &cfg, const std::vector<TouchpadDevice> &pads) {
  TouchpadOptions o = load_touchpad_options(cfg);
  for (const auto &d : pads)
    for (const auto &prop : touchpad_props(o, d.props)) {
      std::vector<std::string> argv{"xinput", "set-prop", std::to_string(d.id), prop.first};
      argv.insert(argv.end(), prop.second.begin(), prop.second.end());
      run_sync(argv, nullptr, 3000);
    }
}

}  // namespace sde
