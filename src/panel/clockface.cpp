// SPDX-License-Identifier: GPL-3.0-or-later
#include "clockface.h"

#include <langinfo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "config.h"
#include "nightlight.h"
#include "paint.h"
#include "theme.h"
#include "util.h"

using namespace sde;

namespace clockface {

Style parse_style(const std::string &name) {
  std::string n = lower(trim(name));
  if (n == "led" || n == "seven-segment" || n == "7seg") return Style::Led;
  if (n == "analog" || n == "analogue") return Style::Analog;
  return Style::Digital;
}

const char *style_name(Style s) {
  switch (s) {
    case Style::Led: return "led";
    case Style::Analog: return "analog";
    case Style::Digital: break;
  }
  return "digital";
}

bool legacy_blink_style(const std::string &name) {
  std::string n = lower(trim(name));
  return n == "blink" || n == "blinking";
}

bool format_is_12h(const std::string &t_fmt) {
  return t_fmt.find("%p") != std::string::npos || t_fmt.find("%P") != std::string::npos ||
         t_fmt.find("%r") != std::string::npos || t_fmt.find("%I") != std::string::npos ||
         t_fmt.find("%l") != std::string::npos;
}

bool use_24h(const std::string &format, const std::string &legacy, const std::string &t_fmt) {
  std::string f = lower(trim(format));
  if (f == "24h" || f == "24") return true;
  if (f == "12h" || f == "12") return false;
  // auto: an explicit older [panel] clock_24h wins over the locale.
  std::string l = lower(trim(legacy));
  if (l == "true" || l == "yes" || l == "1" || l == "on") return true;
  if (l == "false" || l == "no" || l == "0" || l == "off") return false;
  return !format_is_12h(t_fmt);
}

bool use_24h(const Config &cfg) {
  const char *t_fmt = nl_langinfo(T_FMT);
  return use_24h(cfg.get("clock", "format", "auto"), cfg.get("panel", "clock_24h", ""),
                 t_fmt ? t_fmt : "");
}

namespace {
// [panel] |key|, with [clock] |alias| when it is not set.
bool option(const Config &cfg, const char *key, const char *alias, bool fallback) {
  if (!trim(cfg.get("panel", key, "")).empty()) return cfg.get_bool("panel", key, fallback);
  return cfg.get_bool("clock", alias, fallback);
}
}  // namespace

Settings Settings::from_config(const Config &cfg) {
  Settings s;
  // [action_center] clock_style (the clock's right-click menu, Settings);
  // [clock] style is read as an alias when it is not set.
  std::string style = cfg.get("action_center", "clock_style", "");
  if (trim(style).empty()) style = cfg.get("clock", "style", "digital");
  s.style = parse_style(style);
  // [panel] clock_seconds and clock_blink, with [clock] seconds and blink
  // as their aliases. The old "blink" style blinks unless clock_blink says
  // otherwise.
  s.seconds = option(cfg, "clock_seconds", "seconds", false);
  s.blink = option(cfg, "clock_blink", "blink", legacy_blink_style(style));
  s.h24 = use_24h(cfg);
  s.show_date = cfg.get_bool("panel", "show_date", true);
  return s;
}

bool migrate_config(Config &cfg) {
  bool blink = false;
  for (const char *section : {"action_center", "clock"}) {
    const char *key = std::string(section) == "clock" ? "style" : "clock_style";
    if (!legacy_blink_style(cfg.get(section, key, ""))) continue;
    cfg.set(section, key, "digital");
    blink = true;
  }
  if (!blink) return false;
  if (trim(cfg.get("panel", "clock_blink", "")).empty() &&
      trim(cfg.get("clock", "blink", "")).empty())
    cfg.set("panel", "clock_blink", "true");
  return true;
}

std::string format_time(const struct tm &t, bool h24, bool seconds) {
  char buf[64];
  if (h24) {
    snprintf(buf, sizeof buf, seconds ? "%02d:%02d:%02d" : "%02d:%02d", t.tm_hour, t.tm_min,
             t.tm_sec);
    return buf;
  }
  int h = t.tm_hour % 12 ? t.tm_hour % 12 : 12;
  char ampm[16] = "";
  strftime(ampm, sizeof ampm, "%p", &t);
  std::string p = trim(ampm);
  if (p.empty()) p = t.tm_hour < 12 ? "AM" : "PM";
  if (seconds)
    snprintf(buf, sizeof buf, "%d:%02d:%02d %s", h, t.tm_min, t.tm_sec, p.c_str());
  else
    snprintf(buf, sizeof buf, "%d:%02d %s", h, t.tm_min, p.c_str());
  return buf;
}

std::string time_digits(const struct tm &t, bool h24, bool seconds, std::string *suffix) {
  int h = h24 ? t.tm_hour : (t.tm_hour % 12 ? t.tm_hour % 12 : 12);
  char buf[16];
  snprintf(buf, sizeof buf, seconds ? "%02d%02d%02d" : "%02d%02d", h, t.tm_min, t.tm_sec);
  if (suffix) {
    suffix->clear();
    if (!h24) {
      char ampm[16] = "";
      strftime(ampm, sizeof ampm, "%p", &t);
      *suffix = trim(ampm);
      if (suffix->empty()) *suffix = t.tm_hour < 12 ? "AM" : "PM";
    }
  }
  return buf;
}

std::string date_line(const struct tm &t) {
  char buf[160];
  size_t n = strftime(buf, sizeof buf, "%A, %e %B %Y", &t);
  std::string s(buf, n), out;
  // %e pads single-digit days with a space.
  for (char c : s)
    if (!(c == ' ' && !out.empty() && out.back() == ' ')) out += c;
  return trim(out);
}

unsigned seven_segment(char c) {
  //                         gfedcba
  static const unsigned digits[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                      0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  if (c >= '0' && c <= '9') return digits[c - '0'];
  if (c == '-') return 0x40;
  return 0;
}

// ------------------------------------------------------------- the time

namespace {
// SDE_CLOCK_FIXTURE: the time itself, or "@<file>" holding it (UI tests
// switch between day and night by writing the file; a missing or empty
// file means the real time). The fixed time runs on from when it was read.
struct Fixture {
  bool active = false;
  time_t start = 0;  // the fixture's time
  std::chrono::steady_clock::time_point since, checked;
  std::string file, text;
};
Fixture &fixture() {
  static Fixture f = [] {
    Fixture f;
    const char *v = getenv("SDE_CLOCK_FIXTURE");
    std::string value = v ? v : "";
    if (!value.empty() && value[0] == '@') {
      f.file = value.substr(1);
      return f;
    }
    struct tm t{};
    if (!value.empty() && parse_fixture(value, t)) {
      f.active = true;
      f.start = mktime(&t);
      f.since = std::chrono::steady_clock::now();
    }
    return f;
  }();
  if (!f.file.empty()) {
    auto now = std::chrono::steady_clock::now();
    if (f.checked.time_since_epoch().count() == 0 || now - f.checked > std::chrono::milliseconds(300)) {
      f.checked = now;
      std::string text;
      if (!read_file(f.file, text)) text.clear();
      text = trim(text);
      if (text != f.text) {
        f.text = text;
        struct tm t{};
        f.active = parse_fixture(text, t);
        if (f.active) {
          f.start = mktime(&t);
          f.since = now;
        }
      }
    }
  }
  return f;
}
}  // namespace

bool parse_fixture(const std::string &text, struct tm &out) {
  int Y, M, D, h, m, s = 0;
  char tail = 0;
  std::string v = trim(text);
  int n = sscanf(v.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &Y, &M, &D, &h, &m, &s, &tail);
  if (n == 5 || n == 6) {
    if (n == 5 && v.size() != 16) return false;
  } else {
    return false;
  }
  if (M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 60)
    return false;
  struct tm t{};
  t.tm_year = Y - 1900;
  t.tm_mon = M - 1;
  t.tm_mday = D;
  t.tm_hour = h;
  t.tm_min = m;
  t.tm_sec = s;
  t.tm_isdst = -1;
  out = t;
  return true;
}

bool fixture_active() { return fixture().active; }

time_t now() {
  Fixture &f = fixture();
  if (!f.active) return time(nullptr);
  auto run = std::chrono::steady_clock::now() - f.since;
  return f.start + time_t(std::chrono::duration_cast<std::chrono::seconds>(run).count());
}

struct tm local_now() {
  time_t t = now();
  struct tm lt;
  localtime_r(&t, &lt);
  return lt;
}

bool is_daytime(const DayInput &in, time_t now) {
  if (in.have_report && !in.prefer_clock) return in.report_day;
  struct tm lt;
  localtime_r(&now, &lt);
  int minute = lt.tm_hour * 60 + lt.tm_min;
  if (in.have_location) {
    int rise, set;
    if (nightlight::sun_times(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, in.lat, in.lon,
                              int(lt.tm_gmtoff / 60), rise, set))
      return nightlight::in_window(minute, rise, set);
    // Polar day or night: the service knows better, then the season.
    if (in.have_report) return in.report_day;
    bool northern_summer = lt.tm_mon >= 3 && lt.tm_mon <= 8;
    return northern_summer == (in.lat > 0);
  }
  return minute >= 6 * 60 && minute < 18 * 60;
}

// -------------------------------------------------------------- drawing

namespace {
// Digit cell proportions (of the height).
constexpr double kDigitW = .56, kThick = .13, kGap = .16, kColon = .30;

void segment_path(cairo_t *cr, double x1, double y1, double x2, double y2, double th) {
  // A long hexagon from (x1,y1) to (x2,y2), horizontal or vertical.
  double h = th / 2, g = th * .18;
  if (y1 == y2) {
    cairo_move_to(cr, x1 + g, y1);
    cairo_line_to(cr, x1 + g + h, y1 - h);
    cairo_line_to(cr, x2 - g - h, y1 - h);
    cairo_line_to(cr, x2 - g, y1);
    cairo_line_to(cr, x2 - g - h, y1 + h);
    cairo_line_to(cr, x1 + g + h, y1 + h);
  } else {
    cairo_move_to(cr, x1, y1 + g);
    cairo_line_to(cr, x1 + h, y1 + g + h);
    cairo_line_to(cr, x1 + h, y2 - g - h);
    cairo_line_to(cr, x1, y2 - g);
    cairo_line_to(cr, x1 - h, y2 - g - h);
    cairo_line_to(cr, x1 - h, y1 + g + h);
  }
  cairo_close_path(cr);
}

void digit_path(cairo_t *cr, unsigned mask, bool lit, double x, double y, double h) {
  double w = h * kDigitW, th = h * kThick;
  double l = x + th / 2, r = x + w - th / 2, t = y + th / 2, m = y + h / 2, b = y + h - th / 2;
  struct Seg {
    double x1, y1, x2, y2;
  } segs[7] = {{l, t, r, t}, {r, t, r, m}, {r, m, r, b}, {l, b, r, b},
               {l, m, l, b}, {l, t, l, m}, {l, m, r, m}};
  for (int i = 0; i < 7; ++i)
    if (bool(mask & (1u << i)) == lit) segment_path(cr, segs[i].x1, segs[i].y1, segs[i].x2, segs[i].y2, th);
}
}  // namespace

double led_width(const std::string &digits, double h) {
  double w = 0;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i) w += h * kGap;
    w += h * kDigitW;
    if (i % 2 == 1 && i + 1 < digits.size()) w += h * kColon;
  }
  return w + h * .12;  // the slant
}

void draw_led(Painter &p, const std::string &digits, double x, double y, double h, const Color &c,
              bool colon, bool glow) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  // A slight forward slant, like real LED displays.
  cairo_matrix_t m;
  cairo_matrix_init(&m, 1, 0, -.12, 1, (y + h) * .12, 0);
  cairo_transform(cr, &m);
  auto layout = [&](bool lit) {
    double cx = x;
    cairo_new_path(cr);
    for (size_t i = 0; i < digits.size(); ++i) {
      if (i) cx += h * kGap;
      digit_path(cr, seven_segment(digits[i]), lit, cx, y, h);
      cx += h * kDigitW;
      if (i % 2 == 1 && i + 1 < digits.size()) {
        // Lit dots when |colon|, else their ghost.
        if (colon == lit) {
          double d = h * kThick * .95, ccx = cx + (h * kColon + h * kGap) / 2;
          cairo_rectangle(cr, ccx - d / 2, y + h * .30 - d / 2, d, d);
          cairo_rectangle(cr, ccx - d / 2, y + h * .70 - d / 2, d, d);
        }
        cx += h * kColon;
      }
    }
  };
  // Unlit segments: a faint ghost.
  layout(false);
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * .10);
  cairo_fill(cr);
  layout(true);
  if (glow) {
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * .16);
    cairo_set_line_width(cr, h * kThick * 1.6);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * .22);
    cairo_set_line_width(cr, h * kThick * .7);
    cairo_stroke_preserve(cr);
  }
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
  cairo_fill(cr);
  cairo_restore(cr);
}

void draw_analog(Painter &p, double cx, double cy, double r, const struct tm &t, const Color &face,
                 const Color &ticks, const Color &hands, const Color &accent, bool seconds,
                 bool shadow) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  if (shadow) p.circle(cx, cy + r * .05, r * 1.03, Color::hex(0x000000, .22));
  p.circle(cx, cy, r, face);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  for (int i = 0; i < 12; ++i) {
    double a = i * M_PI / 6, outer = r * .86, inner = i % 3 ? r * .76 : r * .66;
    p.line(cx + std::sin(a) * outer, cy - std::cos(a) * outer, cx + std::sin(a) * inner,
           cy - std::cos(a) * inner, ticks, std::max(1.0, r * (i % 3 ? .04 : .07)));
  }
  double hour = (t.tm_hour % 12 + t.tm_min / 60.0) * M_PI / 6;
  double minute = (t.tm_min + t.tm_sec / 60.0) * M_PI / 30;
  p.line(cx, cy, cx + std::sin(hour) * r * .50, cy - std::cos(hour) * r * .50, hands,
         std::max(1.5, r * .11));
  p.line(cx, cy, cx + std::sin(minute) * r * .74, cy - std::cos(minute) * r * .74, hands,
         std::max(1.2, r * .07));
  if (seconds) {
    double s = t.tm_sec * M_PI / 30;
    p.line(cx - std::sin(s) * r * .14, cy + std::cos(s) * r * .14, cx + std::sin(s) * r * .80,
           cy - std::cos(s) * r * .80, accent, std::max(1.0, r * .03));
  }
  p.circle(cx, cy, std::max(1.5, r * .09), accent);
  cairo_restore(cr);
}

// ------------------------------------------------------- the panel clock

namespace {
// Text with a blinking colon: the colons keep their room while hidden, so
// the digits never move.
void blink_text(const std::string &s, int x, int y, int w, int h, const Color &c, bool colon) {
  int total = text_width(s), cx = x + (w - total) / 2, colon_w = text_width(":");
  size_t start = 0;
  while (true) {
    size_t i = s.find(':', start);
    std::string piece = s.substr(start, i == std::string::npos ? std::string::npos : i - start);
    int pw = text_width(piece);
    if (!piece.empty()) draw_text(piece, cx, y, pw + S(4), h, c);
    cx += pw;
    if (i == std::string::npos) break;
    if (colon) draw_text(":", cx, y, colon_w + S(4), h, c);
    cx += colon_w;
    start = i + 1;
  }
}
int led_height(int h, bool two_lines) {
  return two_lines ? S(14) : std::max(S(10), std::min(S(16), h - S(18)));
}
int analog_size(int h) { return std::max(S(14), std::min(S(30), h - S(12))); }
}  // namespace

bool colon_visible(const Settings &s) {
  // A blinking colon is on in even seconds.
  if (!s.blink || s.style == Style::Analog) return true;
  return now() % 2 == 0;
}

bool ticks_seconds(const Settings &s) {
  return s.seconds || (s.blink && s.style != Style::Analog);
}

void draw_blink_text(const std::string &s, int x, int y, int w, int h, const Color &c,
                     bool colon) {
  blink_text(s, x, y, w, h, c, colon);
}

int panel_width(const Settings &s, const Theme &t, int h, const std::string &date,
                bool two_lines) {
  struct tm sample{};
  sample.tm_hour = 20;  // wide digits for the sample
  sample.tm_min = 58;
  sample.tm_sec = 58;
  if (s.style == Style::Analog) {
    int tw = analog_size(h);
    if (!date.empty()) {
      text_font(t.font_size - 1);
      tw += S(8) + text_width(date);
    }
    return tw;
  }
  int tw;
  if (s.style == Style::Led) {
    std::string suffix, digits = time_digits(sample, s.h24, s.seconds, &suffix);
    tw = int(std::ceil(led_width(digits, led_height(h, two_lines))));
    text_font(std::max(S(8), t.font_size - 3), true);
    if (!suffix.empty()) tw += S(4) + text_width(suffix);
  } else {
    std::string str = format_time(sample, s.h24, s.seconds);
    for (char &ch : str)
      if (ch >= '0' && ch <= '9') ch = '8';
    text_font(t.font_size, true);
    tw = text_width(str);
  }
  if (date.empty()) return tw;
  text_font(two_lines ? t.font_size - 1 : t.font_size, false);
  int dw = text_width(date);
  return two_lines ? std::max(tw, dw) : tw + dw + S(10);
}

void draw_panel(const Settings &s, const Theme &t, int win_w, int win_h, int x, int w, int h,
                const std::string &date, bool two_lines) {
  struct tm lt = local_now();
  bool colon = colon_visible(s);
  if (s.style == Style::Analog) {
    int d = analog_size(h);
    text_font(t.font_size - 1);
    int dw = date.empty() ? 0 : S(8) + text_width(date);
    int fx = x + (w - d - dw) / 2;
    {
      Painter p(win_w, win_h);
      draw_analog(p, fx + d / 2.0, h / 2.0, d / 2.0, lt, t.surface_alt.mix(t.text, .06),
                  t.text_dim, t.text, t.accent, s.seconds, false);
      p.flush();
    }
    if (!date.empty()) {
      text_font(t.font_size - 1);
      draw_text(date, fx + d + S(8), 0, dw, h, t.text);
    }
    return;
  }
  int ty = two_lines ? h / 2 - S(17) + S(2) : 0, th = two_lines ? S(18) : h;
  int tx = x, tw = w;
  if (!two_lines && !date.empty()) {
    // One line: the date, then the time.
    int total = panel_width(s, t, h, date, false);
    text_font(t.font_size, false);
    int dw = text_width(date) + S(10), left = x + (w - total) / 2;
    draw_text(date, left, 0, dw, h, t.text);
    tx = left + dw;
    tw = total - dw;
  }
  if (s.style == Style::Led) {
    std::string suffix, digits = time_digits(lt, s.h24, s.seconds, &suffix);
    double lh = led_height(h, two_lines), lw = led_width(digits, lh);
    text_font(std::max(S(8), t.font_size - 3), true);
    int sw = suffix.empty() ? 0 : S(4) + text_width(suffix);
    double lx = tx + (tw - lw - sw) / 2.0, ly = ty + (th - lh) / 2.0;
    {
      Painter p(win_w, win_h);
      Color ink = t.accent_text.contrast(t.panel_bg) >= 3 ? t.accent_text : t.text;
      draw_led(p, digits, lx, ly, lh, ink, colon, false);
      p.flush();
    }
    if (sw) draw_text(suffix, int(lx + lw + S(4)), ty, sw, th, t.text_dim);
  } else {
    text_font(t.font_size, true);
    blink_text(format_time(lt, s.h24, s.seconds), tx, ty, tw, th, t.text, colon);
  }
  if (two_lines && !date.empty()) {
    text_font(t.font_size - 1);
    draw_text(date, x, h / 2 - S(1), w, S(18), t.text_dim, 1);
  }
}

}  // namespace clockface
