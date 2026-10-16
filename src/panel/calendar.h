// SPDX-License-Identifier: GPL-3.0-or-later
// Month calendar shown when the panel clock is clicked.
#pragma once

#include "popup.h"

class Calendar : public PanelPopup {
 public:
  Calendar();
  void draw() override;
  int handle(int event) override;

 protected:
  void prepare() override;

 private:
  void shift_month(int delta);
  int cell_at(int x, int y) const;  // day of month under the pointer, or 0
  int year_ = 0, month_ = 0;        // shown month (month 0-11)
  int first_weekday_ = 1;           // 0 Sunday, 1 Monday
  int grid_x_ = 0, grid_y_ = 0, cell_ = 0, header_h_ = 0;
  int hover_ = 0;
  int focus_day_ = 0;
};
