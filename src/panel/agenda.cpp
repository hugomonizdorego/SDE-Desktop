// SPDX-License-Identifier: GPL-3.0-or-later
#include "agenda.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

namespace sde {
namespace agenda {

namespace {

std::string trim_ws(const std::string &s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string lower_ascii(std::string s) {
  for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct Section {
  std::string kind;  // "alarm" or "item"
  std::map<std::string, std::string> values;
};

// Sections "[kind]" or "[kind N]"; keys before the first section are dropped.
std::vector<Section> parse_sections(const std::string &text) {
  std::vector<Section> out;
  std::istringstream in(text);
  std::string line;
  bool open = false;
  while (std::getline(in, line)) {
    std::string l = trim_ws(line);
    if (l.empty() || l[0] == '#' || l[0] == ';') continue;
    if (l[0] == '[') {
      size_t close = l.find(']');
      if (close == std::string::npos) {
        open = false;
        continue;
      }
      std::string name = lower_ascii(trim_ws(l.substr(1, close - 1)));
      size_t space = name.find(' ');
      out.push_back({space == std::string::npos ? name : name.substr(0, space), {}});
      open = true;
      continue;
    }
    size_t eq = l.find('=');
    if (!open || eq == std::string::npos || eq == 0) continue;
    out.back().values[lower_ascii(trim_ws(l.substr(0, eq)))] = trim_ws(l.substr(eq + 1));
  }
  return out;
}

bool parse_int(const std::string &s, long long &out) {
  if (s.empty()) return false;
  char *end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (!end || *end) return false;
  out = v;
  return true;
}

int int_or(const std::map<std::string, std::string> &m, const char *key, int fallback, int lo,
           int hi) {
  auto it = m.find(key);
  long long v;
  if (it == m.end() || !parse_int(it->second, v)) return fallback;
  return int(std::max<long long>(lo, std::min<long long>(hi, v)));
}

bool bool_or(const std::map<std::string, std::string> &m, const char *key, bool fallback) {
  auto it = m.find(key);
  if (it == m.end()) return fallback;
  std::string v = lower_ascii(it->second);
  if (v == "true" || v == "yes" || v == "1" || v == "on") return true;
  if (v == "false" || v == "no" || v == "0" || v == "off") return false;
  return fallback;
}

std::string get(const std::map<std::string, std::string> &m, const char *key) {
  auto it = m.find(key);
  return it == m.end() ? "" : unescape(it->second);
}

// Unique positive ids; broken or repeated ones get fresh numbers.
template <class T>
void fix_ids(std::vector<T> &list) {
  std::set<int> seen;
  int next = 1;
  for (auto &e : list)
    if (e.id > 0) next = std::max(next, e.id + 1);
  for (auto &e : list) {
    if (e.id <= 0 || seen.count(e.id)) e.id = next++;
    seen.insert(e.id);
  }
}

long long local_time(int year, int month, int day, int hour, int minute) {
  struct tm t {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_isdst = -1;
  return (long long)mktime(&t);
}

const char *const kDayKeys[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

}  // namespace

std::string escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\t')
      out += "\\t";
    else if (c == '\r')
      continue;
    else
      out += c;
  }
  return out;
}

std::string unescape(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
    } else {
      out += s[i];
    }
  }
  return out;
}

int days_in_month(int year, int month) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : d[month - 1];
}

bool parse_time(const std::string &text, int &hour, int &minute) {
  std::string s = trim_ws(text);
  if (s.empty()) return false;
  std::string h, m;
  size_t colon = s.find_first_of(":.");
  if (colon != std::string::npos) {
    h = s.substr(0, colon);
    m = s.substr(colon + 1);
  } else if (s.size() == 4 || s.size() == 3) {
    h = s.substr(0, s.size() - 2);
    m = s.substr(s.size() - 2);
  } else {
    h = s;
    m = "0";
  }
  if (h.empty() || m.empty() || h.size() > 2 || m.size() > 2) return false;
  for (char c : h + m)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  int hh = std::atoi(h.c_str()), mm = std::atoi(m.c_str());
  if (hh > 23 || mm > 59) return false;
  hour = hh;
  minute = mm;
  return true;
}

bool parse_date(const std::string &text, int &year, int &month, int &day) {
  std::string s = trim_ws(text);
  int y = 0, m = 0, d = 0;
  char a = 0, b = 0;
  std::istringstream in(s);
  if (!(in >> y >> a >> m >> b >> d) || a != '-' || b != '-') return false;
  std::string rest;
  if (in >> rest) return false;
  if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) return false;
  year = y;
  month = m;
  day = d;
  return true;
}

unsigned day_bit(int tm_wday) { return 1u << ((tm_wday + 6) % 7); }

std::string days_key(unsigned days) {
  std::string out;
  for (int i = 0; i < 7; ++i)
    if (days & (1u << i)) out += std::string(out.empty() ? "" : ",") + kDayKeys[i];
  return out;
}

unsigned parse_days(const std::string &text) {
  unsigned days = 0;
  std::string t = lower_ascii(text);
  if (t == "daily" || t == "every day" || t == "all") return EVERY_DAY;
  if (t == "weekdays") return WEEKDAYS;
  if (t == "weekend" || t == "weekends") return WEEKEND;
  std::string part;
  std::istringstream in(t);
  while (std::getline(in, part, ',')) {
    part = trim_ws(part);
    for (int i = 0; i < 7; ++i)
      if (part.size() >= 3 && part.compare(0, 3, kDayKeys[i]) == 0) days |= 1u << i;
  }
  return days;
}

std::vector<Alarm> parse_alarms(const std::string &text) {
  std::vector<Alarm> out;
  for (const auto &s : parse_sections(text)) {
    if (s.kind != "alarm") continue;
    Alarm a;
    int h, m;
    auto t = s.values.find("time");
    if (t == s.values.end() || !parse_time(t->second, h, m)) continue;
    a.hour = h;
    a.minute = m;
    a.id = int_or(s.values, "id", 0, 0, 1000000);
    auto d = s.values.find("days");
    a.days = d == s.values.end() ? 0 : parse_days(d->second);
    a.label = get(s.values, "label");
    a.enabled = bool_or(s.values, "enabled", true);
    a.snooze = int_or(s.values, "snooze", 10, 1, 60);
    long long until = 0;
    auto u = s.values.find("snoozed_until");
    if (u != s.values.end() && parse_int(u->second, until) && until > 0) a.snoozed_until = until;
    out.push_back(a);
    if (out.size() >= 100) break;
  }
  fix_ids(out);
  return out;
}

std::string format_alarms(const std::vector<Alarm> &alarms) {
  std::ostringstream out;
  out << "# SDE alarms (sde-panel). days: mon,tue,... or empty to ring once.\n";
  for (const auto &a : alarms) {
    char time[8];
    snprintf(time, sizeof time, "%02d:%02d", a.hour, a.minute);
    out << "\n[alarm " << a.id << "]\nid=" << a.id << "\ntime=" << time
        << "\ndays=" << days_key(a.days) << "\nlabel=" << escape(a.label)
        << "\nenabled=" << (a.enabled ? "true" : "false") << "\nsnooze=" << a.snooze << "\n";
    if (a.snoozed_until > 0) out << "snoozed_until=" << a.snoozed_until << "\n";
  }
  return out.str();
}

long long next_ring(const Alarm &a, long long after) {
  if (!a.enabled) return 0;
  time_t base = time_t(after);
  struct tm lt;
  localtime_r(&base, &lt);
  for (int d = 0; d <= 8; ++d) {
    struct tm t = lt;
    t.tm_mday += d;
    t.tm_hour = a.hour;
    t.tm_min = a.minute;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t when = mktime(&t);  // normalizes the day and fills tm_wday
    if (when <= base) continue;
    if (a.days == 0 || (a.days & day_bit(t.tm_wday))) return (long long)when;
  }
  return 0;
}

long long next_due(const Alarm &a, long long after) {
  long long ring = next_ring(a, after);
  if (a.snoozed_until > 0 && (ring == 0 || a.snoozed_until < ring)) return a.snoozed_until;
  return ring;
}

std::vector<Item> parse_items(const std::string &text) {
  std::vector<Item> out;
  for (const auto &s : parse_sections(text)) {
    if (s.kind != "item") continue;
    Item it;
    auto d = s.values.find("date");
    if (d == s.values.end() || !parse_date(d->second, it.year, it.month, it.day)) continue;
    it.title = get(s.values, "title");
    if (trim_ws(it.title).empty()) continue;
    int h, m;
    auto t = s.values.find("time");
    if (t != s.values.end() && parse_time(t->second, h, m)) {
      it.hour = h;
      it.minute = m;
    }
    it.id = int_or(s.values, "id", 0, 0, 100000000);
    it.note = get(s.values, "note");
    it.remind = int_or(s.values, "remind", -1, -1, 60 * 24 * 7);
    it.reminded = bool_or(s.values, "reminded", false);
    out.push_back(it);
    if (out.size() >= 5000) break;
  }
  fix_ids(out);
  return out;
}

std::string format_items(const std::vector<Item> &items) {
  std::ostringstream out;
  out << "# SDE agenda (sde-panel). remind: minutes before the start, -1 for none.\n";
  for (const auto &it : items) {
    // Room for any int (the fields are range-checked when parsed).
    char date[40], time[24] = "";
    snprintf(date, sizeof date, "%04d-%02d-%02d", it.year, it.month, it.day);
    if (it.timed()) snprintf(time, sizeof time, "%02d:%02d", it.hour, it.minute);
    out << "\n[item " << it.id << "]\nid=" << it.id << "\ndate=" << date << "\ntime=" << time
        << "\ntitle=" << escape(it.title) << "\nnote=" << escape(it.note)
        << "\nremind=" << it.remind << "\nreminded=" << (it.reminded ? "true" : "false") << "\n";
  }
  return out.str();
}

long long item_start(const Item &item) {
  return local_time(item.year, item.month, item.day, item.timed() ? item.hour : 9,
                    item.timed() ? item.minute : 0);
}

long long reminder_time(const Item &item) {
  if (item.remind < 0) return 0;
  return item_start(item) - (long long)item.remind * 60;
}

std::vector<Item> items_on(const std::vector<Item> &items, int year, int month, int day) {
  std::vector<Item> out;
  for (const auto &it : items)
    if (it.year == year && it.month == month && it.day == day) out.push_back(it);
  std::stable_sort(out.begin(), out.end(), [](const Item &a, const Item &b) {
    int ka = a.timed() ? a.hour * 60 + a.minute : -1, kb = b.timed() ? b.hour * 60 + b.minute : -1;
    return ka < kb;
  });
  return out;
}

bool has_items(const std::vector<Item> &items, int year, int month, int day) {
  for (const auto &it : items)
    if (it.year == year && it.month == month && it.day == day) return true;
  return false;
}

}  // namespace agenda
}  // namespace sde
