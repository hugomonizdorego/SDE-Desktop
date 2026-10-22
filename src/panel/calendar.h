// SPDX-License-Identifier: GPL-3.0-or-later
// Month calendar card of the Action Center (the panel clock opens the Action
// Center with this calendar in view). Month navigation with the arrow
// buttons, Page Up/Down, or the arrow keys when it has keyboard focus.
#pragma once

#include <FL/Fl_Widget.H>

#include <functional>

class MonthCalendar : public Fl_Widget {
 public:
  // |row|: the height of a week row at 100 % (0: the default, 29 px;
  // alpha52: smaller on small screens, so the Action Center never scrolls).
  MonthCalendar(int x, int y, int w, int row = 0);
  void draw() override;
  int handle(int event) override;
  // Height for a given width: header row, weekday names and six weeks.
  static int height_for(int w, int row = 0);
  // Back to the current month.
  void today();
  // Shown month (month 1-12) and the selected day (the agenda's date).
  void show_month(int year, int month);
  void select(int year, int month, int day);
  int year() const { return year_; }
  int month() const { return month_ + 1; }
  // Days that get a dot (agenda items); |picked| runs when a day is clicked
  // or chosen with Enter, |moved| when the shown month changes.
  std::function<bool(int year, int month, int day)> marked;
  std::function<void(int year, int month, int day)> picked;
  std::function<void(int year, int month)> moved;

 private:
  void shift_month(int delta);
  void layout(int &cell_w, int &cell_h, int &grid_y) const;
  int cell_at(int x, int y) const;  // day of month under the pointer, or 0
  int nav_at(int x, int y) const;   // -1 previous, -2 next, -3 today, 0 none
  int year_ = 0, month_ = 0;        // shown month (month 0-11)
  int first_weekday_ = 1;           // 0 Sunday, 1 Monday
  int hover_ = 0;
  int row_ = 29;
  int focus_day_ = 0;
  int sel_year_ = 0, sel_month_ = -1, sel_day_ = 0;
};
