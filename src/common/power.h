// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Screen brightness and power profiles, shared by sde-power (the power
// manager), Settings and the panel.
//
// Brightness: the backlight in /sys/class/backlight is read directly; it is
// written through systemd-logind (org.freedesktop.login1.Session.
// SetBrightness, allowed for the user of the active session), then the sysfs
// file when it is writable, then brightnessctl.
//
// Power profiles: power-profiles-daemon on the system bus
// (org.freedesktop.UPower.PowerProfiles, or the older net.hadess.PowerProfiles).
//
// All calls are synchronous with short timeouts (at most ~1 s each); they
// are meant for user actions and for the power manager, not for redraws.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace power {

struct Backlight {
  std::string name;  // directory under /sys/class/backlight
  std::string type;  // firmware, platform or raw
  int value = 0;
  int max = 0;
  bool valid() const { return max > 0; }
  int percent() const;
};

// Every backlight under |root|, best first (see primary_backlight()).
std::vector<Backlight> backlights(const std::string &root = "/sys/class/backlight");
// The backlight that drives the built-in panel: firmware (ACPI) before
// platform before raw interfaces, then the one with the finest steps.
bool primary_backlight(Backlight &out, const std::string &root = "/sys/class/backlight");

// Conversions. Brightness never goes below 1 raw step, so "0 %" does not
// switch the panel off on hardware where 0 means off.
int percent_to_raw(int percent, int max);
int raw_to_percent(int raw, int max);
// The percentage after |steps| key presses of |step| percent from
// |current| (1..100). Below 2*step the steps get finer, so that dark
// levels stay reachable.
int step_percent(int current, int steps, int step = 5);

// Current brightness of the primary backlight in percent, or -1.
int brightness_percent();
// Sets the primary backlight. Returns false when no method worked.
bool set_brightness_percent(int percent);
// Changes the brightness by |steps| (negative = darker). Returns the new
// percentage, or -1 when there is no backlight.
int step_brightness(int steps, int step = 5);

struct Profiles {
  bool available = false;          // power-profiles-daemon answered
  std::vector<std::string> names;  // e.g. power-saver, balanced, performance
  std::string active;
};
Profiles power_profiles();
bool set_power_profile(const std::string &name);
// Translated label for a profile id ("Power saver", "Balanced", ...).
std::string profile_label(const std::string &name);

}  // namespace power
}  // namespace sde
