// SPDX-License-Identifier: GPL-3.0-or-later
// Month calendar card of the Action Center (the panel clock opens the Action
// Center with this calendar in view). Month navigation with the arrow
// buttons, Page Up/Down, or the arrow keys when it has keyboard focus.
#pragma once

#include <FL/Fl_Widget.H>

class MonthCalendar : public Fl_Widget {
 public:
  MonthCalendar(int x, int y, int w);
  void draw() override;
  int handle(int event) override;
  // Height for a given width: header row, weekday names and six weeks.
  static int height_for(int w);
  // Back to the current month.
  void today();

 private:
  void shift_month(int delta);
  void layout(int &cell_w, int &cell_h, int &grid_y) const;
  int cell_at(int x, int y) const;  // day of month under the pointer, or 0
  int nav_at(int x, int y) const;   // -1 previous, -2 next, -3 today, 0 none
  int year_ = 0, month_ = 0;        // shown month (month 0-11)
  int first_weekday_ = 1;           // 0 Sunday, 1 Monday
  int hover_ = 0;
  int focus_day_ = 0;
};
