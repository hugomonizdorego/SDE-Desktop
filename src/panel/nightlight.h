// SPDX-License-Identifier: GPL-3.0-or-later
// Night Light: warmer screen colors in the evening, set through the
// XRandR gamma ramps of every CRTC (like redshift's randr method). The
// panel applies it ([night_light] in sde.conf), checks the schedule once a
// minute and puts the ramps it found back when Night Light turns off.
//
// Schedules: always, sunset (sunset to sunrise at the weather location,
// [weather] location; 19:00-07:00 when it is unknown) or custom
// ([night_light] from/to, HH:MM).
//
// The color and time math has no X dependency and is unit tested in
// tests/test_weather.cpp.
#pragma once

#include <ctime>
#include <functional>
#include <string>

namespace sde {
class Config;
}

namespace nightlight {

struct Settings {
  bool enabled = false;
  int temperature = 4000;          // Kelvin, 2500 (warm) to 6500 (neutral)
  std::string schedule = "sunset";  // always, sunset or custom
  int from = 20 * 60, to = 7 * 60;  // custom, minutes after midnight
  static Settings from_config(const sde::Config &cfg);
};

struct Rgb {
  double r = 1, g = 1, b = 1;
};
// Channel multipliers for a color temperature, 6500 K = neutral (1, 1, 1).
Rgb whitepoint(int kelvin);

// "HH:MM" (24-hour); false for anything else.
bool parse_time(const std::string &text, int &minutes);
std::string format_time(int minutes);

// Sunrise and sunset in local minutes after midnight (NOAA equations).
// |utc_offset_min| is the local offset from UTC. False at polar day or
// night.
bool sun_times(int year, int month, int day, double lat, double lon, int utc_offset_min,
               int &sunrise, int &sunset);

// Whether |minute| lies in [from, to), wrapping over midnight.
bool in_window(int minute, int from, int to);

// The evening window for |s| on the local date of |now|; |lat|/|lon| are
// used when |have_location| (sunset schedule).
void window_for(const Settings &s, time_t now, bool have_location, double lat, double lon,
                int &from, int &to);
bool active(const Settings &s, time_t now, bool have_location, double lat, double lon);

// Applies the panel's settings: starts a minute timer that follows the
// schedule. |config| gives the current configuration.
class Controller {
 public:
  static Controller &get();
  void start(std::function<const sde::Config *()> config);
  // Re-reads the settings now (after a change in the Action Center).
  void update();
  bool on() const { return applied_; }

 private:
  static void tick(void *);
  bool apply(const Rgb &c);
  void restore();
  std::function<const sde::Config *()> config_;
  bool applied_ = false, started_ = false;
  int applied_temp_ = 0;
};

}  // namespace nightlight
