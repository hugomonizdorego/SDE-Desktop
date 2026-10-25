// SPDX-License-Identifier: GPL-3.0-or-later
// Clock faces (alpha52): styles, the seconds and blinking colon options and
// the migration of the old "blink" style (0.2 Alpha 2), 12/24-hour formats,
// the Action Center's date line, seven-segment masks and the sky's
// day/night decision.
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "../src/common/config.h"
#include "../src/panel/clockface.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)
#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    std::string x_ = (a), y_ = (b);                                                      \
    if (x_ != y_) {                                                                      \
      std::fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, x_.c_str(), \
                   y_.c_str());                                                          \
      ++failures;                                                                        \
    }                                                                                    \
  } while (0)

struct tm at(int y, int mo, int d, int h, int mi, int s = 0) {
  struct tm t{};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = s;
  t.tm_isdst = -1;
  mktime(&t);  // weekday
  return t;
}
time_t when(int y, int mo, int d, int h, int mi) {
  struct tm t = at(y, mo, d, h, mi);
  return mktime(&t);
}
}  // namespace

int main() {
  using namespace clockface;
  setlocale(LC_ALL, "C");
  // Jakarta (UTC+7, no daylight saving time), like the weather fixtures.
  setenv("TZ", "WIB-7", 1);
  tzset();

  // ------------------------------------------------------------ styles
  CHECK(parse_style("digital") == Style::Digital && parse_style("") == Style::Digital);
  CHECK(parse_style(" led ") == Style::Led);
  CHECK(parse_style("analog") == Style::Analog && parse_style("analogue") == Style::Analog);
  CHECK(parse_style("sundial") == Style::Digital);
  for (Style s : {Style::Digital, Style::Led, Style::Analog})
    CHECK(parse_style(style_name(s)) == s);
  // 0.2 Alpha 2: "Blink" is no style any more but an option of its own.
  CHECK(parse_style("Blink") == Style::Digital && legacy_blink_style(" Blink") &&
        legacy_blink_style("blinking") && !legacy_blink_style("led"));
  Settings blink;
  blink.blink = true;
  Settings plain;
  CHECK(ticks_seconds(blink) && !ticks_seconds(plain));
  plain.seconds = true;
  CHECK(ticks_seconds(plain));
  // The LED colon stays lit unless it blinks; the analog face has none.
  Settings led;
  led.style = Style::Led;
  CHECK(!ticks_seconds(led) && colon_visible(led));
  Settings analog;
  analog.style = Style::Analog;
  analog.blink = true;
  CHECK(!ticks_seconds(analog) && colon_visible(analog));
  analog.seconds = true;
  CHECK(ticks_seconds(analog));

  // --------------------------------- options from sde.conf, the migration
  {
    sde::Config cfg;
    Settings s = Settings::from_config(cfg);
    CHECK(s.style == Style::Digital && !s.seconds && !s.blink);
    // The old style: digital with the blinking colon...
    cfg.set("action_center", "clock_style", "blink");
    s = Settings::from_config(cfg);
    CHECK(s.style == Style::Digital && s.blink && !s.seconds);
    // ...unless clock_blink says otherwise.
    cfg.set("panel", "clock_blink", "false");
    CHECK(!Settings::from_config(cfg).blink);
    cfg.unset("panel", "clock_blink");
    // Rewritten once.
    CHECK(migrate_config(cfg));
    CHECK_EQ(cfg.get("action_center", "clock_style", ""), "digital");
    CHECK_EQ(cfg.get("panel", "clock_blink", ""), "true");
    CHECK(!migrate_config(cfg));
    s = Settings::from_config(cfg);
    CHECK(s.style == Style::Digital && s.blink);
    // [clock] style, seconds and blink are aliases; an explicit blink stays.
    sde::Config old;
    old.set("clock", "style", "blink");
    old.set("clock", "seconds", "true");
    old.set("clock", "blink", "false");
    s = Settings::from_config(old);
    CHECK(s.style == Style::Digital && s.seconds && !s.blink);
    CHECK(migrate_config(old));
    CHECK_EQ(old.get("clock", "style", ""), "digital");
    CHECK(old.get("panel", "clock_blink", "").empty() && !Settings::from_config(old).blink);
    // Every combination: LED with seconds and a blinking colon.
    sde::Config both;
    both.set("action_center", "clock_style", "led");
    both.set("panel", "clock_seconds", "true");
    both.set("panel", "clock_blink", "true");
    s = Settings::from_config(both);
    CHECK(s.style == Style::Led && s.seconds && s.blink && ticks_seconds(s));
    CHECK(!migrate_config(both));
  }

  // ------------------------------------------------------- 12/24 hours
  CHECK(format_is_12h("%I:%M:%S %p") && format_is_12h("%r") && !format_is_12h("%H:%M:%S"));
  CHECK(use_24h("24h", "false", "%I:%M %p") && !use_24h("12h", "true", "%H:%M"));
  CHECK(use_24h("auto", "true", "%I:%M %p") && !use_24h("auto", "false", "%H:%M"));
  CHECK(use_24h("auto", "", "%H:%M:%S") && !use_24h("auto", "", "%I:%M:%S %p"));
  CHECK(use_24h("", "", "%T"));

  struct tm evening = at(2026, 10, 22, 21, 30, 5);
  CHECK_EQ(format_time(evening, true, false), "21:30");
  CHECK_EQ(format_time(evening, true, true), "21:30:05");
  CHECK_EQ(format_time(evening, false, false), "9:30 PM");
  CHECK_EQ(format_time(evening, false, true), "9:30:05 PM");
  CHECK_EQ(format_time(at(2026, 10, 22, 0, 5), false, false), "12:05 AM");
  CHECK_EQ(format_time(at(2026, 10, 22, 12, 0), false, false), "12:00 PM");
  CHECK_EQ(format_time(at(2026, 10, 22, 7, 4), true, false), "07:04");
  std::string suffix = "x";
  CHECK_EQ(time_digits(evening, true, false, &suffix), "2130");
  CHECK(suffix.empty());
  CHECK_EQ(time_digits(evening, false, false, &suffix), "0930");
  CHECK_EQ(suffix, "PM");
  CHECK_EQ(time_digits(evening, true, true), "213005");

  // --------------------------------------------------------- date line
  CHECK_EQ(date_line(evening), "Thursday, 22 October 2026");
  CHECK_EQ(date_line(at(2026, 10, 2, 9, 0)), "Friday, 2 October 2026");
  CHECK_EQ(date_line(at(2027, 1, 1, 0, 0)), "Friday, 1 January 2027");

  // ---------------------------------------------------- seven segments
  CHECK(seven_segment('8') == 0x7F);
  CHECK(seven_segment('1') == 0x06);   // b, c
  CHECK(seven_segment('7') == 0x07);   // a, b, c
  CHECK(seven_segment('0') == 0x3F && !(seven_segment('0') & 0x40));  // no middle bar
  CHECK(seven_segment('2') == 0x5B && !(seven_segment('2') & 0x04));  // no c
  CHECK(seven_segment('5') == 0x6D && !(seven_segment('5') & 0x02));  // no b
  CHECK(seven_segment('6') & 0x10 && !(seven_segment('9') & 0x10));   // e only on 6
  CHECK(seven_segment('-') == 0x40 && seven_segment(' ') == 0 && seven_segment('x') == 0);
  int lit = 0;
  for (char c = '0'; c <= '9'; ++c) lit += __builtin_popcount(seven_segment(c));
  CHECK(lit == 49);  // 6+2+5+5+4+5+6+3+7+6
  CHECK(led_width("2130", 40) > led_width("213", 40) && led_width("213005", 40) > led_width("2130", 40));

  // ------------------------------------------------------- the fixture
  struct tm f{};
  CHECK(parse_fixture("2026-10-22T21:30", f) && f.tm_hour == 21 && f.tm_min == 30 && f.tm_sec == 0);
  CHECK(parse_fixture("2026-10-22T06:15:42", f) && f.tm_sec == 42 && f.tm_mday == 22 && f.tm_mon == 9);
  CHECK(!parse_fixture("2026-10-22", f) && !parse_fixture("21:30", f) &&
        !parse_fixture("2026-13-22T21:30", f) && !parse_fixture("2026-10-22T25:30", f) &&
        !parse_fixture("2026-10-22T21:30x", f));

  // ------------------------------------------------------ day or night
  DayInput jakarta;
  jakarta.have_location = true;
  jakarta.lat = -6.2;
  jakarta.lon = 106.8;
  // Sunrise about 05:30, sunset about 17:50 in October.
  CHECK(!is_daytime(jakarta, when(2026, 10, 22, 21, 30)));
  CHECK(is_daytime(jakarta, when(2026, 10, 22, 12, 0)));
  CHECK(is_daytime(jakarta, when(2026, 10, 22, 6, 0)));
  CHECK(!is_daytime(jakarta, when(2026, 10, 22, 5, 0)));
  CHECK(!is_daytime(jakarta, when(2026, 10, 22, 18, 30)));
  // The weather service's day flag wins, except with the clock fixture.
  DayInput report = jakarta;
  report.have_report = true;
  report.report_day = false;
  CHECK(!is_daytime(report, when(2026, 10, 22, 12, 0)));
  report.prefer_clock = true;
  CHECK(is_daytime(report, when(2026, 10, 22, 12, 0)));
  // Nothing known: 06:00-18:00.
  DayInput none;
  CHECK(!is_daytime(none, when(2026, 10, 22, 5, 59)) && is_daytime(none, when(2026, 10, 22, 6, 0)));
  CHECK(is_daytime(none, when(2026, 10, 22, 17, 59)) && !is_daytime(none, when(2026, 10, 22, 18, 0)));
  // Polar night at 78°N in December: night all day without a report.
  DayInput svalbard;
  svalbard.have_location = true;
  svalbard.lat = 78.2;
  svalbard.lon = 15.6;
  CHECK(!is_daytime(svalbard, when(2026, 12, 21, 12, 0)));
  CHECK(is_daytime(svalbard, when(2026, 6, 21, 23, 0)));  // midnight sun

  if (failures) {
    std::fprintf(stderr, "%d clock face check(s) failed\n", failures);
    return 1;
  }
  std::puts("clock face model: OK");
  return 0;
}
