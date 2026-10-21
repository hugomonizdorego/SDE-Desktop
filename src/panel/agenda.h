// SPDX-License-Identifier: GPL-3.0-or-later
// Alarms and agenda items of the panel clock, kept free of FLTK so that the
// parsing and the scheduling arithmetic can be unit tested.
//
// Alarms live in ~/.config/sde/alarms.ini, agenda items in
// ~/.local/share/sde/agenda.ini. Both are small INI files with one section
// per entry; unknown keys, comments and broken lines are ignored, so a hand
// edited or truncated file never stops the panel.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace agenda {

// Repeat days of an alarm: bit 0 Monday ... bit 6 Sunday; 0 rings once.
enum : unsigned { MONDAY = 1, WEEKDAYS = 0x1F, WEEKEND = 0x60, EVERY_DAY = 0x7F };

struct Alarm {
  int id = 0;
  int hour = 7, minute = 0;
  unsigned days = 0;
  std::string label;
  bool enabled = true;
  int snooze = 10;              // minutes
  long long snoozed_until = 0;  // epoch seconds of a pending snooze, 0 for none
};

// Lower-case English day keys used in the file ("mon,tue").
std::string days_key(unsigned days);
unsigned parse_days(const std::string &text);
// Monday-based bit of a struct tm weekday (0 Sunday).
unsigned day_bit(int tm_wday);

std::vector<Alarm> parse_alarms(const std::string &text);
std::string format_alarms(const std::vector<Alarm> &alarms);

// The first scheduled ring of |a| strictly after |after| (epoch seconds, in
// local time), ignoring snoozes; 0 when the alarm is off.
long long next_ring(const Alarm &a, long long after);
// Like next_ring, but a pending snooze comes first.
long long next_due(const Alarm &a, long long after);

struct Item {
  int id = 0;
  int year = 0, month = 0, day = 0;  // month 1-12
  int hour = -1, minute = 0;         // hour -1: all day
  std::string title, note;
  int remind = -1;                   // minutes before the start, -1 none
  bool reminded = false;             // the reminder has fired
  bool timed() const { return hour >= 0; }
};

std::vector<Item> parse_items(const std::string &text);
std::string format_items(const std::vector<Item> &items);

// Start of an item (epoch, local time); all-day items count from 09:00.
long long item_start(const Item &item);
// When the reminder fires, or 0 when the item has none.
long long reminder_time(const Item &item);
// Items of one day, sorted by time (all-day items first).
std::vector<Item> items_on(const std::vector<Item> &items, int year, int month, int day);
bool has_items(const std::vector<Item> &items, int year, int month, int day);

// "HH:MM" (also "H:MM", "HHMM", "H") -> hour and minute. False when invalid.
bool parse_time(const std::string &text, int &hour, int &minute);
// "YYYY-MM-DD" with a real calendar date.
bool parse_date(const std::string &text, int &year, int &month, int &day);
int days_in_month(int year, int month);  // month 1-12

// A new id larger than every id in use.
template <class T>
int next_id(const std::vector<T> &list) {
  int id = 0;
  for (const auto &e : list) id = e.id > id ? e.id : id;
  return id + 1;
}

// Values are stored on one line: backslash, newline and tab are escaped.
std::string escape(const std::string &s);
std::string unescape(const std::string &s);

}  // namespace agenda
}  // namespace sde
