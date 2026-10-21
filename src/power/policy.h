// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Power: the decisions of the power manager, without any I/O, so that
// they are unit tested (tests/test_power.cpp). The daemon (main.cpp) feeds
// them the idle time, the battery state and the [power] settings.
#pragma once

#include <string>

namespace sde {

class Config;

namespace power {

// What a timeout, the lid or the power button does.
enum class Action { Nothing, Suspend, Hibernate, PowerOff, Lock, Blank, Interactive, Logind };
Action parse_action(const std::string &text, Action fallback);
const char *action_name(Action a);

// Idle timeouts in seconds; 0 disables a stage.
struct IdleTimeouts {
  int dim = 0;
  int blank = 0;
  int suspend = 0;
};

struct Settings {
  IdleTimeouts battery{120, 300, 900};
  IdleTimeouts ac{300, 600, 0};
  int dim_level = 30;        // percent of the current brightness while dimmed
  int battery_low = 10;      // percent
  int battery_critical = 5;  // percent
  Action critical_action = Action::Hibernate;
  Action lid_battery = Action::Suspend;
  Action lid_ac = Action::Suspend;
  Action power_button = Action::Interactive;
  std::string profile_battery;  // power profile on battery, "" = leave alone
  std::string profile_ac;
  int brightness_step = 5;
  bool osd = true;
  bool notify = true;

  static Settings from_config(const Config &cfg);
};

enum class IdleStage { Active = 0, Dim = 1, Blank = 2, Suspend = 3 };

// The deepest stage reached after |idle_ms| of inactivity. Disabled stages
// are skipped; a later stage with a shorter timeout than an earlier one
// still wins once reached.
IdleStage idle_stage(long idle_ms, const IdleTimeouts &t);
// Milliseconds until the next enabled stage after |idle_ms| (how long the
// daemon may sleep before it checks again), or -1 when none is left.
long next_stage_in(long idle_ms, const IdleTimeouts &t);

// Battery warning level with hysteresis: a level is left only when the
// charge rose 2 points above its threshold or the charger was plugged in,
// so a battery at the threshold does not warn again and again.
enum class BatteryLevel { Normal = 0, Low = 1, Critical = 2 };
BatteryLevel battery_level(double percent, bool discharging, BatteryLevel previous,
                           int low, int critical);

// The lid action for the current power source. A docked laptop (external
// monitor) does nothing: the user works on the other screen.
Action lid_action(const Settings &s, bool on_battery, bool docked);

}  // namespace power
}  // namespace sde
