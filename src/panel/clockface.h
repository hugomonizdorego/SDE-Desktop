// SPDX-License-Identifier: GPL-3.0-or-later
// Clock faces (alpha52) shared by the panel clock and the Action Center's
// clock hero: one setting for both,
//
//   [clock] style=digital|blink|led|analog
//           seconds=false
//           format=auto|24h|12h   (auto: the locale's time format; an
//                                  older [panel] clock_24h still counts)
//
//  * digital: plain text "21:30";
//  * blink: the same with the colon blinking once a second;
//  * led: seven-segment LED digits drawn with cairo (glow in the hero);
//  * analog: a round face with hands.
//
// SDE_CLOCK_FIXTURE=2026-10-22T21:30[:SS] (or "@file" holding it, re-read
// while the panel runs) fixes the clock's start time (UI
// tests: day and night pictures); it then runs on from there.
//
// The model (formats, seven-segment masks, the day/night decision) is unit
// tested in tests/test_clockface.cpp.
#pragma once

#include <ctime>
#include <string>

namespace sde {
class Config;
class Painter;
struct Color;
struct Theme;
}  // namespace sde

namespace clockface {

enum class Style { Digital, Blink, Led, Analog };
Style parse_style(const std::string &name);  // unknown names: Digital
const char *style_name(Style s);              // "digital", "blink", "led", "analog"

struct Settings {
  Style style = Style::Digital;
  bool seconds = false;
  bool h24 = true;
  bool show_date = true;  // [panel] show_date
  static Settings from_config(const sde::Config &cfg);
};

// A locale time format (nl_langinfo(T_FMT)) uses a 12-hour clock.
bool format_is_12h(const std::string &t_fmt);
// [clock] format: "24h", "12h" or "auto" (|legacy| is [panel] clock_24h
// when it is set: "true"/"false", else ""; |t_fmt| the locale's format).
bool use_24h(const std::string &format, const std::string &legacy, const std::string &t_fmt);
bool use_24h(const sde::Config &cfg);

// "21:30", "21:30:05", "9:30 PM" (no leading zero in 12-hour times).
std::string format_time(const struct tm &t, bool h24, bool seconds);
// The digits only, for the LED face: "2130", "0930" (12-hour: 01-12) and,
// for 12-hour times, the AM/PM marker in |suffix|.
std::string time_digits(const struct tm &t, bool h24, bool seconds, std::string *suffix = nullptr);
// The Action Center's date line, localized: "Thursday, 22 October 2026".
std::string date_line(const struct tm &t);

// Seven-segment mask of |c| ('0'-'9', '-', ' '): bit 0 = a (top), 1 = b
// (top right), 2 = c (bottom right), 3 = d (bottom), 4 = e (bottom left),
// 5 = f (top left), 6 = g (middle). 0 for anything else.
unsigned seven_segment(char c);

// The current time (SDE_CLOCK_FIXTURE aware) and the local time of it.
time_t now();
struct tm local_now();
bool fixture_active();
// "YYYY-MM-DDTHH:MM[:SS]" as local time; false for anything else.
bool parse_fixture(const std::string &text, struct tm &out);

// Day or night for the sky picture at |now|:
//  * with SDE_CLOCK_FIXTURE (|prefer_clock|) or without a weather report,
//    sunrise and sunset at the known location (NOAA), else 06:00-18:00;
//  * otherwise the weather service's own day flag.
struct DayInput {
  bool have_report = false, report_day = true;
  bool have_location = false;
  double lat = 0, lon = 0;
  bool prefer_clock = false;
};
bool is_daytime(const DayInput &in, time_t now);

// ------------------------------------------------------------- drawing
// Width of |digits| ("2130", with a colon after every pair but the last)
// drawn as LED digits |h| pixels tall.
double led_width(const std::string &digits, double h);
// Seven-segment digits at |x|,|y|; |colon| draws the separators, |glow|
// adds a soft halo; unlit segments are a faint ghost of |c|.
void draw_led(sde::Painter &p, const std::string &digits, double x, double y, double h,
              const sde::Color &c, bool colon, bool glow);
// Round face with hour, minute (and second) hands.
void draw_analog(sde::Painter &p, double cx, double cy, double r, const struct tm &t,
                 const sde::Color &face, const sde::Color &ticks, const sde::Color &hands,
                 const sde::Color &accent, bool seconds, bool shadow);

// ------------------------------------------------------- the panel clock
// Whether the clock changes every second (seconds shown, blinking colon,
// LED colon): the panel then ticks once a second instead of a minute.
bool ticks_seconds(const Settings &s);
// The colon of a blinking (blink, LED) clock is on in even seconds.
bool colon_visible(const Settings &s);
// Centered FLTK text whose colons keep their room while hidden.
void draw_blink_text(const std::string &s, int x, int y, int w, int h, const sde::Color &c,
                     bool colon);
// Width of the panel clock button content for a panel |h| high; |date| is
// the short date shown with it (empty: none). |two_lines|: date under the
// time.
int panel_width(const Settings &s, const sde::Theme &t, int h, const std::string &date,
                bool two_lines);
// Draws the panel clock into the button at |x|,0,|w|,|h| (cairo first, then
// FLTK text, inside the panel's draw()).
void draw_panel(const Settings &s, const sde::Theme &t, int win_w, int win_h, int x, int w, int h,
                const std::string &date, bool two_lines);

}  // namespace clockface
