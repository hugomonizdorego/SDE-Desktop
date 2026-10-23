// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "policy.h"

#include <algorithm>
#include <climits>

#include "config.h"

namespace sde {
namespace power {

namespace {

const struct {
  Action action;
  const char *name;
} kActions[] = {
    {Action::Nothing, "nothing"},     {Action::Suspend, "suspend"},
    {Action::Hibernate, "hibernate"}, {Action::PowerOff, "poweroff"},
    {Action::Lock, "lock"},           {Action::Blank, "blank"},
    {Action::Interactive, "interactive"}, {Action::Logind, "logind"},
};

int clamp_seconds(int v) { return std::max(0, std::min(v, 24 * 3600)); }

IdleTimeouts read_timeouts(const Config &cfg, const char *suffix, IdleTimeouts d) {
  IdleTimeouts t;
  t.dim = clamp_seconds(cfg.get_int("power", std::string("idle_dim_") + suffix, d.dim));
  t.blank = clamp_seconds(cfg.get_int("power", std::string("idle_blank_") + suffix, d.blank));
  t.suspend = clamp_seconds(cfg.get_int("power", std::string("idle_suspend_") + suffix, d.suspend));
  return t;
}

bool safe_profile(const std::string &p) {
  for (char c : p)
    if (!(islower(static_cast<unsigned char>(c)) || c == '-')) return false;
  return true;
}

}  // namespace

Action parse_action(const std::string &text, Action fallback) {
  for (const auto &a : kActions)
    if (text == a.name) return a.action;
  return fallback;
}

const char *action_name(Action a) {
  for (const auto &x : kActions)
    if (x.action == a) return x.name;
  return "nothing";
}

Settings Settings::from_config(const Config &cfg) {
  Settings s;
  s.battery = read_timeouts(cfg, "battery", s.battery);
  s.ac = read_timeouts(cfg, "ac", s.ac);
  s.dim_level = std::max(1, std::min(100, cfg.get_int("power", "dim_level", s.dim_level)));
  s.battery_low = std::max(1, std::min(50, cfg.get_int("power", "battery_low", s.battery_low)));
  s.battery_critical =
      std::max(1, std::min(s.battery_low, cfg.get_int("power", "battery_critical", s.battery_critical)));
  s.critical_action = parse_action(cfg.get("power", "critical_action"), s.critical_action);
  if (s.critical_action == Action::Lock || s.critical_action == Action::Blank ||
      s.critical_action == Action::Interactive || s.critical_action == Action::Logind)
    s.critical_action = Action::Hibernate;
  s.lid_battery = parse_action(cfg.get("power", "lid_battery"), s.lid_battery);
  s.lid_ac = parse_action(cfg.get("power", "lid_ac"), s.lid_ac);
  s.power_button = parse_action(cfg.get("power", "power_button"), s.power_button);
  if (s.power_button == Action::Lock || s.power_button == Action::Blank)
    s.power_button = Action::Interactive;
  s.profile_battery = cfg.get("power", "profile_battery");
  s.profile_ac = cfg.get("power", "profile_ac");
  if (!safe_profile(s.profile_battery)) s.profile_battery.clear();
  if (!safe_profile(s.profile_ac)) s.profile_ac.clear();
  s.brightness_step = std::max(1, std::min(25, cfg.get_int("power", "brightness_step", 5)));
  s.osd = cfg.get_bool("power", "show_osd", true);
  s.notify = cfg.get_bool("power", "notify_battery", true);
  return s;
}

IdleStage idle_stage(long idle_ms, const IdleTimeouts &t) {
  IdleStage stage = IdleStage::Active;
  auto reached = [idle_ms](int seconds) { return seconds > 0 && idle_ms >= long(seconds) * 1000; };
  if (reached(t.dim)) stage = IdleStage::Dim;
  if (reached(t.blank)) stage = IdleStage::Blank;
  if (reached(t.suspend)) stage = IdleStage::Suspend;
  return stage;
}

long next_stage_in(long idle_ms, const IdleTimeouts &t) {
  long best = LONG_MAX;
  for (int seconds : {t.dim, t.blank, t.suspend}) {
    if (seconds <= 0) continue;
    long at = long(seconds) * 1000;
    if (at > idle_ms) best = std::min(best, at - idle_ms);
  }
  return best == LONG_MAX ? -1 : best;
}

BatteryLevel battery_level(double percent, bool discharging, BatteryLevel previous, int low,
                           int critical) {
  if (!discharging || percent < 0) return BatteryLevel::Normal;
  if (percent <= critical) return BatteryLevel::Critical;
  if (previous == BatteryLevel::Critical && percent < critical + 2) return BatteryLevel::Critical;
  if (percent <= low) return BatteryLevel::Low;
  if (previous != BatteryLevel::Normal && percent < low + 2) return BatteryLevel::Low;
  return BatteryLevel::Normal;
}

Action lid_action(const Settings &s, bool on_battery, bool docked) {
  if (docked) return Action::Nothing;
  return on_battery ? s.lid_battery : s.lid_ac;
}

}  // namespace power
}  // namespace sde
