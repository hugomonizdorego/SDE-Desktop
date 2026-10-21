// SPDX-License-Identifier: GPL-3.0-or-later
#include "calendar.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
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
#include "widgets.h"

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

const int kHeader = 34, kWeekdays = 22, kRow = 29, kNav = 28;

}  // namespace

MonthCalendar::MonthCalendar(int x, int y, int w) : Fl_Widget(x, y, w, height_for(w)) {
  // Named for window inspection; the widget draws its own content.
  label(_("Calendar"));
  labeltype(FL_NO_LABEL);
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
  today();
}

int MonthCalendar::height_for(int) { return S(kHeader) + S(kWeekdays) + 6 * S(kRow); }

void MonthCalendar::today() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  year_ = lt.tm_year + 1900;
  month_ = lt.tm_mon;
  focus_day_ = lt.tm_mday;
  hover_ = 0;
  redraw();
}

void MonthCalendar::show_month(int year, int month) {
  if (month < 1 || month > 12) return;
  year_ = year;
  month_ = month - 1;
  focus_day_ = std::min(std::max(1, focus_day_), days_in_month(year_, month_));
  redraw();
}

void MonthCalendar::select(int year, int month, int day) {
  sel_year_ = year;
  sel_month_ = month - 1;
  sel_day_ = day;
  if (year == year_ && month - 1 == month_) focus_day_ = day;
  redraw();
}

void MonthCalendar::shift_month(int delta) {
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
  if (moved) moved(year_, month_ + 1);
}

void MonthCalendar::layout(int &cell_w, int &cell_h, int &grid_y) const {
  cell_w = w() / 7;
  cell_h = S(kRow);
  grid_y = y() + S(kHeader) + S(kWeekdays);
}

int MonthCalendar::cell_at(int ex, int ey) const {
  int cw, ch, gy;
  layout(cw, ch, gy);
  int gx = x() + (w() - 7 * cw) / 2;
  if (ex < gx || ey < gy || cw <= 0) return 0;
  int col = (ex - gx) / cw, row = (ey - gy) / ch;
  if (col > 6 || row > 5) return 0;
  int offset = (weekday_of(year_, month_, 1) - first_weekday_ + 7) % 7;
  int day = row * 7 + col - offset + 1;
  return day >= 1 && day <= days_in_month(year_, month_) ? day : 0;
}

int MonthCalendar::nav_at(int ex, int ey) const {
  int nav = S(kNav), ny = y() + (S(kHeader) - nav) / 2;
  if (ey < ny || ey >= ny + nav) return 0;
  int next_x = x() + w() - nav, prev_x = next_x - nav - S(4);
  if (ex >= prev_x && ex < prev_x + nav) return -1;
  if (ex >= next_x && ex < next_x + nav) return -2;
  return 0;
}

void MonthCalendar::draw() {
  const Theme &t = Panel::instance->theme();
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  int offset = (weekday_of(year_, month_, 1) - first_weekday_ + 7) % 7;
  int ndays = days_in_month(year_, month_);
  bool this_month = year_ == lt.tm_year + 1900 && month_ == lt.tm_mon;
  int cw, ch, gy;
  layout(cw, ch, gy);
  int gx = x() + (w() - 7 * cw) / 2;
  int nav = S(kNav), ny = y() + (S(kHeader) - nav) / 2;
  double radius = std::min(cw, ch) * 0.44;
  {
    Painter p(window()->w(), window()->h());
    for (int i = 0; i < 2; ++i) {
      int bx = i == 0 ? x() + w() - 2 * nav - S(4) : x() + w() - nav;
      if (hover_ == -1 - i) p.fill_rounded(bx, ny, nav, nav, nav / 2.0, t.hover);
      glyph::chevron(p, bx + Sd(7), ny + Sd(7), nav - Sd(14), i == 1, t.text);
    }
    for (int d = 1; d <= ndays; ++d) {
      int cell = offset + d - 1;
      double cx = gx + (cell % 7) * cw + cw / 2.0;
      double cy = gy + (cell / 7) * ch + ch / 2.0;
      bool selected = year_ == sel_year_ && month_ == sel_month_ && d == sel_day_;
      if (this_month && d == lt.tm_mday)
        p.circle(cx, cy, radius, t.accent);
      else if (selected)
        p.circle(cx, cy, radius, t.accent.alpha(t.dark ? 0.35 : 0.2));
      else if (d == hover_)
        p.circle(cx, cy, radius, t.hover);
      if (selected && this_month && d == lt.tm_mday) {
        p.set(t.text);
        cairo_set_line_width(p.cr(), Sd(1.5));
        cairo_new_sub_path(p.cr());
        cairo_arc(p.cr(), cx, cy, radius - Sd(2.5), 0, 2 * M_PI);
        cairo_stroke(p.cr());
      }
      // A dot under days with agenda items.
      if (marked && marked(year_, month_ + 1, d))
        p.circle(cx, cy + radius * 0.62, Sd(2.2),
                 this_month && d == lt.tm_mday ? t.on_accent : t.accent);
      if (d == focus_day_ && Fl::focus() == this) {
        p.set(t.accent);
        cairo_set_line_width(p.cr(), Sd(2));
        cairo_new_sub_path(p.cr());
        cairo_arc(p.cr(), cx, cy, radius + Sd(1.5), 0, 2 * M_PI);
        cairo_stroke(p.cr());
      }
    }
  }
  struct tm first{};
  first.tm_year = year_ - 1900;
  first.tm_mon = month_;
  first.tm_mday = 1;
  text_font(t.font_size + 1, true);
  draw_text(strftime_str("%B %Y", first), x(), y(), w() - 2 * nav - S(12), S(kHeader), t.text);
  text_font(t.font_size - 1, true);
  for (int i = 0; i < 7; ++i) {
    struct tm wd{};
    wd.tm_wday = (first_weekday_ + i) % 7;
    draw_text(strftime_str("%a", wd), gx + i * cw, y() + S(kHeader), cw, S(kWeekdays), t.text_dim,
              1);
  }
  for (int d = 1; d <= ndays; ++d) {
    int cell = offset + d - 1;
    bool today = this_month && d == lt.tm_mday;
    text_font(t.font_size, today);
    draw_text(std::to_string(d), gx + (cell % 7) * cw, gy + (cell / 7) * ch, cw, ch,
              today ? t.on_accent : t.text, 1);
  }
}

int MonthCalendar::handle(int event) {
  switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_ENTER:
    case FL_MOVE: {
      int hv = nav_at(Fl::event_x(), Fl::event_y());
      if (!hv) hv = cell_at(Fl::event_x(), Fl::event_y());
      if (hv != hover_) {
        hover_ = hv;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      if (hover_) {
        hover_ = 0;
        redraw();
      }
      return 1;
    case FL_PUSH: {
      int n = nav_at(Fl::event_x(), Fl::event_y());
      if (n) shift_month(n == -1 ? -1 : 1);
      int day = n ? 0 : cell_at(Fl::event_x(), Fl::event_y());
      if (day) {
        focus_day_ = day;
        select(year_, month_ + 1, day);
        if (picked) {
          auto fn = picked;  // the popup may rebuild in the callback
          int y = year_, m = month_ + 1;
          if (Fl::visible_focus()) take_focus();
          fn(y, m, day);
          return 1;
        }
      }
      if (Fl::visible_focus()) take_focus();
      return 1;
    }
    case FL_RELEASE:
      return 1;
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      int ndays = days_in_month(year_, month_);
      if (k == FL_Page_Up)
        shift_month(-1);
      else if (k == FL_Page_Down)
        shift_month(1);
      else if (k == FL_Home)
        today();
      else if ((k == FL_Enter || k == FL_KP_Enter || k == ' ') && picked) {
        select(year_, month_ + 1, focus_day_);
        auto fn = picked;
        fn(year_, month_ + 1, focus_day_);
        return 1;
      }
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
        return 0;
      }
      redraw();
      return 1;
    }
    default:
      break;
  }
  return Fl_Widget::handle(event);
}
