// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Task View's layout (sde-taskview, Win+Tab; docs/WINDOWS.md), without a
// display (unit tested): the windows of the chosen workspace as large cards
// in a grid, the workspaces in a row at the bottom with a card to add one.
#pragma once

#include <vector>

namespace sde {
namespace taskview {

struct Box {
  int x = 0, y = 0, w = 0, h = 0;
  bool contains(int px, int py) const { return w > 0 && px >= x && px < x + w && py >= y && py < y + h; }
};

struct Layout {
  std::vector<Box> windows;  // one card per window (title row + picture)
  std::vector<Box> desks;    // one card per workspace
  Box add;                   // "New workspace", w = 0 when there are 5
  Box grid;                  // the area of the window cards
  int columns = 1;           // of the window grid
};

// |aspects|: width / height of each window; |scale| is the UI scale.
Layout layout(int width, int height, int desks, bool can_add, const std::vector<double> &aspects, double scale);

// A |n| x aspect grid in |area|: the number of columns that gives the
// windows the most room (each card |title| pixels taller than its picture).
int best_columns(int n, const Box &area, int gap, int title, const std::vector<double> &aspects);

// The picture of a window |aspect| fitted into a card's picture box.
Box fit(const Box &box, double aspect);

// Keyboard moves in a grid of |n| cards with |columns| columns.
int move_selection(int current, int n, int columns, int dx, int dy);

}  // namespace taskview
}  // namespace sde
