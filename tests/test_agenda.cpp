// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the panel clock's alarm and agenda model (src/panel/agenda.h):
// robust INI parsing, round trips and the ring/reminder arithmetic.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "../src/panel/agenda.h"

using namespace sde::agenda;

static int failures = 0;
#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                  \
    }                                                                              \
  } while (0)

static long long at(int y, int m, int d, int h, int min, int s = 0) {
  struct tm t {};
  t.tm_year = y - 1900;
  t.tm_mon = m - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = min;
  t.tm_sec = s;
  t.tm_isdst = -1;
  return (long long)mktime(&t);
}

int main() {
  setenv("TZ", "Asia/Jakarta", 1);  // no daylight saving: fixed arithmetic
  tzset();

  // Times and dates.
  int h = -1, m = -1, y = 0, mo = 0, d = 0;
  CHECK(parse_time("07:30", h, m) && h == 7 && m == 30);
  CHECK(parse_time(" 7.05 ", h, m) && h == 7 && m == 5);
  CHECK(parse_time("2359", h, m) && h == 23 && m == 59);
  CHECK(parse_time("9", h, m) && h == 9 && m == 0);
  CHECK(!parse_time("24:00", h, m));
  CHECK(!parse_time("12:60", h, m));
  CHECK(!parse_time("ab:cd", h, m));
  CHECK(!parse_time("", h, m));
  CHECK(!parse_time("123:4", h, m));
  CHECK(parse_date("2028-02-29", y, mo, d) && y == 2028 && mo == 2 && d == 29);
  CHECK(!parse_date("2026-02-29", y, mo, d));
  CHECK(!parse_date("2026-13-01", y, mo, d));
  CHECK(!parse_date("2026-10-20x", y, mo, d));
  CHECK(!parse_date("garbage", y, mo, d));

  // Day sets.
  CHECK(parse_days("mon,wed, fri") == (1u | 4u | 16u));
  CHECK(parse_days("weekdays") == WEEKDAYS);
  CHECK(parse_days("Saturday,Sunday") == WEEKEND);
  CHECK(parse_days("") == 0);
  CHECK(days_key(WEEKEND) == "sat,sun");
  CHECK(day_bit(0) == 64u && day_bit(1) == 1u);

  // Escaping keeps notes on one line.
  CHECK(unescape(escape("a\\b\nc\td")) == "a\\b\nc\td");
  CHECK(escape("x\ny").find('\n') == std::string::npos);

  // Robust alarm parsing: comments, junk, missing or invalid times, ids.
  const char *alarms_text =
      "# comment\nstray=1\n[alarm 1]\ntime=06:45\ndays=mon,tue,wed,thu,fri\nlabel=Wake up\\nnow\n"
      "snooze=500\n[alarm 2]\ntime=25:00\n[alarm 3]\nid=1\ntime=21:00\nenabled=no\n[broken\n"
      "time=01:00\n[alarm]\ntime=12:00\nsnoozed_until=abc\n[other]\ntime=08:00\n";
  auto alarms = parse_alarms(alarms_text);
  CHECK(alarms.size() == 3);
  CHECK(alarms[0].hour == 6 && alarms[0].minute == 45 && alarms[0].days == WEEKDAYS);
  CHECK(alarms[0].label == "Wake up\nnow");
  CHECK(alarms[0].snooze == 60);  // clamped
  CHECK(!alarms[1].enabled && alarms[1].hour == 21);
  CHECK(alarms[0].id != alarms[1].id && alarms[1].id != alarms[2].id && alarms[0].id != alarms[2].id);
  CHECK(alarms[2].snoozed_until == 0 && alarms[2].enabled);
  // Round trip.
  alarms[2].snoozed_until = 1234567890;
  auto again = parse_alarms(format_alarms(alarms));
  CHECK(again.size() == 3);
  for (size_t i = 0; i < again.size() && i < alarms.size(); ++i) {
    CHECK(again[i].id == alarms[i].id && again[i].hour == alarms[i].hour &&
          again[i].minute == alarms[i].minute && again[i].days == alarms[i].days &&
          again[i].label == alarms[i].label && again[i].enabled == alarms[i].enabled &&
          again[i].snooze == alarms[i].snooze && again[i].snoozed_until == alarms[i].snoozed_until);
  }
  CHECK(parse_alarms("").empty());
  CHECK(next_id(alarms) > alarms[0].id);

  // Ring arithmetic. 2026-10-20 is a Tuesday.
  Alarm a;
  a.hour = 7;
  a.minute = 30;
  CHECK(next_ring(a, at(2026, 10, 20, 6, 0)) == at(2026, 10, 20, 7, 30));
  CHECK(next_ring(a, at(2026, 10, 20, 7, 30)) == at(2026, 10, 21, 7, 30));  // strictly after
  CHECK(next_ring(a, at(2026, 10, 20, 8, 0)) == at(2026, 10, 21, 7, 30));
  a.days = WEEKEND;
  CHECK(next_ring(a, at(2026, 10, 20, 8, 0)) == at(2026, 10, 24, 7, 30));
  a.days = 1u;  // Monday only, from Tuesday: six days later
  CHECK(next_ring(a, at(2026, 10, 20, 8, 0)) == at(2026, 10, 26, 7, 30));
  a.days = 1u << 1;  // Tuesday, after today's ring: next week
  CHECK(next_ring(a, at(2026, 10, 20, 9, 0)) == at(2026, 10, 27, 7, 30));
  // Month and year boundaries.
  a.days = 0;
  CHECK(next_ring(a, at(2026, 12, 31, 23, 0)) == at(2027, 1, 1, 7, 30));
  a.enabled = false;
  CHECK(next_ring(a, at(2026, 10, 20, 6, 0)) == 0);
  a.enabled = true;
  a.snoozed_until = at(2026, 10, 20, 6, 10);
  CHECK(next_due(a, at(2026, 10, 20, 6, 0)) == a.snoozed_until);
  a.snoozed_until = at(2026, 10, 20, 9, 0);
  CHECK(next_due(a, at(2026, 10, 20, 6, 0)) == at(2026, 10, 20, 7, 30));

  // Agenda parsing and reminders.
  const char *items_text =
      "[item 4]\ndate=2026-10-20\ntime=14:30\ntitle=Dentist\nnote=Bring card\\nfloor 2\nremind=15\n"
      "[item 5]\ndate=2026-10-20\ntitle=Mum's birthday\nremind=1440\nreminded=true\n"
      "[item 6]\ndate=2026-02-30\ntitle=Bad date\n"
      "[item 7]\ndate=2026-10-21\ntitle=\n"
      "[item 8]\ndate=2026-10-20\ntime=7:5\ntitle=Run\nremind=junk\n"
      "[item 9]\ndate=2026-10-22\ntime=99:00\ntitle=Bad time is all day\n";
  auto items = parse_items(items_text);
  CHECK(items.size() == 4);
  CHECK(items[0].title == "Dentist" && items[0].note == "Bring card\nfloor 2" && items[0].remind == 15);
  CHECK(items[1].hour == -1 && items[1].reminded && items[1].remind == 1440);
  CHECK(items[2].hour == 7 && items[2].minute == 5 && items[2].remind == -1);
  CHECK(!items[3].timed());
  CHECK(reminder_time(items[0]) == at(2026, 10, 20, 14, 15));
  CHECK(reminder_time(items[1]) == at(2026, 10, 19, 9, 0));  // all-day: 09:00, one day before
  CHECK(reminder_time(items[2]) == 0);
  auto day = items_on(items, 2026, 10, 20);
  CHECK(day.size() == 3 && day[0].title == "Mum's birthday" && day[1].title == "Run" &&
        day[2].title == "Dentist");
  CHECK(has_items(items, 2026, 10, 22) && !has_items(items, 2026, 10, 23));
  auto items2 = parse_items(format_items(items));
  CHECK(items2.size() == items.size());
  for (size_t i = 0; i < items2.size() && i < items.size(); ++i)
    CHECK(items2[i].id == items[i].id && items2[i].title == items[i].title &&
          items2[i].note == items[i].note && items2[i].hour == items[i].hour &&
          items2[i].minute == items[i].minute && items2[i].remind == items[i].remind &&
          items2[i].reminded == items[i].reminded && items2[i].day == items[i].day);
  CHECK(parse_items("[item]\n").empty());

  if (failures) {
    std::fprintf(stderr, "%d agenda checks failed\n", failures);
    return 1;
  }
  std::printf("agenda model: OK\n");
  return 0;
}
