// SPDX-License-Identifier: GPL-3.0-or-later
#include "calendar.h"

#include <FL/Fl.H>
#include <langinfo.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <ctime>
#include <string>

#include "glyphs.h"
#include "i18n.h"
#include "paint.h"
#include "panel.h"
#include "theme.h"

using namespace sde;

namespace {

int days_in_month(int year, int month) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 1 && leap ? 29 : d[month];
}

// 0 = Sunday.
int weekday_of(int year, int month, int day) {
  struct tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month;
  t.tm_mday = day;
  t.tm_hour = 12;
  mktime(&t);
  return t.tm_wday;
}

std::string strftime_str(const char *fmt, const struct tm &t) {
  char buf[128];
  size_t n = strftime(buf, sizeof buf, fmt, &t);
  return std::string(buf, n);
}

}  // namespace

Calendar::Calendar() : PanelPopup("Calendar") {
  end();
#ifdef _NL_TIME_FIRST_WEEKDAY
  // glibc: first weekday of the locale (1 = Sunday in the reference week
  // starting on _NL_TIME_WEEK_1STDAY, which is a Sunday for nearly all locales).
  int first = nl_langinfo(_NL_TIME_FIRST_WEEKDAY)[0];
  first_weekday_ = first >= 1 && first <= 7 ? (first - 1) % 7 : 1;
  // C/POSIX locales report Sunday; ISO weeks start on Monday.
  const char *loc = setlocale(LC_TIME, nullptr);
  if (!loc || std::string(loc) == "C" || std::string(loc) == "POSIX" ||
      std::string(loc).find("C.") == 0)
    first_weekday_ = 1;
#endif
}

void Calendar::prepare() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  year_ = lt.tm_year + 1900;
  month_ = lt.tm_mon;
  focus_day_ = lt.tm_mday;
  hover_ = 0;
  cell_ = S(40);
  header_h_ = S(112);
  grid_x_ = S(16);
  grid_y_ = header_h_ + S(30);
  size(grid_x_ * 2 + 7 * cell_, grid_y_ + 6 * cell_ + S(14));
}

void Calendar::shift_month(int delta) {
  month_ += delta;
  while (month_ < 0) {
    month_ += 12;
    --year_;
  }
  while (month_ > 11) {
    month_ -= 12;
    ++year_;
  }
  focus_day_ = std::min(focus_day_, days_in_month(year_, month_));
  redraw();
}

int Calendar::cell_at(int x, int y) const {
  if (x < grid_x_ || y < grid_y_) return 0;
  int col = (x - grid_x_) / cell_, row = (y - grid_y_) / cell_;
  if (col > 6 || row > 5) return 0;
  int offset = (weekday_of(year_, month_, 1) - first_weekday_ + 7) % 7;
  int day = row * 7 + col - offset + 1;
  return day >= 1 && day <= days_in_month(year_, month_) ? day : 0;
}

void Calendar::draw() {
  const Theme &t = Panel::instance->theme();
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  int W = w();
  int offset = (weekday_of(year_, month_, 1) - first_weekday_ + 7) % 7;
  int ndays = days_in_month(year_, month_);
  bool this_month = year_ == lt.tm_year + 1900 && month_ == lt.tm_mon;
  int nav = S(32), nav_y = header_h_ - S(40);
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.surface);
    p.stroke_rounded(0.5, 0.5, w() - 1, h() - 1, 0, t.border);
    p.line(S(16), header_h_ - S(50) + 0.5, W - S(16), header_h_ - S(50) + 0.5, t.border);
    // Month navigation.
    for (int i = 0; i < 2; ++i) {
      int bx = i == 0 ? W - S(16) - 2 * nav - S(6) : W - S(16) - nav;
      bool hot = hover_ == -1 - i;
      if (hot) p.fill_rounded(bx, nav_y, nav, nav, nav / 2.0, t.hover);
      glyph::chevron(p, bx + Sd(6), nav_y + Sd(6), nav - Sd(12), i == 1, t.text);
    }
    for (int d = 1; d <= ndays; ++d) {
      int cell = offset + d - 1;
      double cx = grid_x_ + (cell % 7) * cell_ + cell_ / 2.0;
      double cy = grid_y_ + (cell / 7) * cell_ + cell_ / 2.0;
      if (this_month && d == lt.tm_mday) p.circle(cx, cy, cell_ * 0.42, t.accent);
      else if (d == hover_) p.circle(cx, cy, cell_ * 0.42, t.hover);
      if (d == focus_day_ && Fl::focus() == this) {
        p.set(t.accent);
        cairo_set_line_width(p.cr(), Sd(2));
        cairo_new_sub_path(p.cr());
        cairo_arc(p.cr(), cx, cy, cell_ * 0.45, 0, 2 * M_PI);
        cairo_stroke(p.cr());
      }
    }
  }
  // Header: weekday, full date and time.
  text_font(t.font_size, false);
  draw_text(strftime_str("%A", lt), S(16), S(14), W - S(32), S(20), t.text_dim);
  text_font(t.font_size + 6, true);
  draw_text(strftime_str("%e %B %Y", lt), S(16), S(34), W - S(32), S(30), t.text);
  struct tm first{};
  first.tm_year = year_ - 1900;
  first.tm_mon = month_;
  first.tm_mday = 1;
  text_font(t.font_size + 2, true);
  draw_text(strftime_str("%B %Y", first), S(16), nav_y, W - 2 * nav - S(40), nav, t.text);
  // Weekday names.
  text_font(t.font_size - 1, true);
  for (int i = 0; i < 7; ++i) {
    struct tm wd{};
    wd.tm_wday = (first_weekday_ + i) % 7;
    std::string name = strftime_str("%a", wd);
    draw_text(name, grid_x_ + i * cell_, grid_y_ - S(28), cell_, S(24), t.text_dim, 1);
  }
  for (int d = 1; d <= ndays; ++d) {
    int cell = offset + d - 1;
    bool today = this_month && d == lt.tm_mday;
    text_font(t.font_size, today);
    draw_text(std::to_string(d), grid_x_ + (cell % 7) * cell_, grid_y_ + (cell / 7) * cell_, cell_, cell_,
              today ? t.on_accent : t.text, 1);
  }
}

int Calendar::handle(int event) {
  int nav = S(32), nav_y = header_h_ - S(40);
  auto nav_at = [&](int x, int y) {
    if (y < nav_y || y >= nav_y + nav) return 0;
    int prev_x = w() - S(16) - 2 * nav - S(6), next_x = w() - S(16) - nav;
    if (x >= prev_x && x < prev_x + nav) return -1;
    if (x >= next_x && x < next_x + nav) return -2;
    return 0;
  };
  switch (event) {
    case FL_SHOW:
      take_focus();
      break;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_MOVE:
    case FL_ENTER: {
      int hv = nav_at(Fl::event_x(), Fl::event_y());
      if (!hv) hv = cell_at(Fl::event_x(), Fl::event_y());
      if (hv != hover_) {
        hover_ = hv;
        redraw();
      }
      return 1;
    }
    case FL_PUSH: {
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (ex >= 0 && ey >= 0 && ex < w() && ey < h()) {
        int n = nav_at(ex, ey);
        if (n) shift_month(n == -1 ? -1 : 1);
        return 1;
      }
      break;
    }
    case FL_MOUSEWHEEL:
      shift_month(Fl::event_dy() > 0 ? 1 : -1);
      return 1;
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      int ndays = days_in_month(year_, month_);
      if (k == FL_Page_Up) shift_month(-1);
      else if (k == FL_Page_Down) shift_month(1);
      else if (k == FL_Home) prepare();
      else if (k == FL_Left || k == FL_Right || k == FL_Up || k == FL_Down) {
        int step = k == FL_Left ? -1 : k == FL_Right ? 1 : k == FL_Up ? -7 : 7;
        int d = focus_day_ + step;
        if (d < 1) {
          shift_month(-1);
          d += days_in_month(year_, month_);
        } else if (d > ndays) {
          d -= ndays;
          shift_month(1);
        }
        focus_day_ = d;
      } else {
        break;
      }
      redraw();
      return 1;
    }
    default:
      break;
  }
  return PanelPopup::handle(event);
}
