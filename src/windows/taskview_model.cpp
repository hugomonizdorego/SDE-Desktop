// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "taskview_model.h"

#include <algorithm>
#include <cmath>

namespace sde {
namespace taskview {

namespace {
int px(double v, double scale) { return int(std::lround(v * scale)); }
}  // namespace

Box fit(const Box &box, double aspect) {
  if (box.w <= 0 || box.h <= 0) return box;
  if (aspect <= 0) aspect = 16.0 / 10.0;
  int w = box.w, h = int(std::lround(box.w / aspect));
  if (h > box.h) {
    h = box.h;
    w = int(std::lround(box.h * aspect));
  }
  return {box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, std::max(1, w), std::max(1, h)};
}

int best_columns(int n, const Box &area, int gap, int title, const std::vector<double> &aspects) {
  if (n <= 1) return 1;
  int best = 1;
  double best_area = -1;
  for (int cols = 1; cols <= n; ++cols) {
    int rows = (n + cols - 1) / cols;
    int cw = (area.w - (cols - 1) * gap) / cols, ch = (area.h - (rows - 1) * gap) / rows - title;
    if (cw <= 0 || ch <= 0) continue;
    double total = 0;
    for (int i = 0; i < n; ++i) {
      Box b = fit({0, 0, cw, ch}, i < int(aspects.size()) ? aspects[size_t(i)] : 1.6);
      total += double(b.w) * b.h;
    }
    // Fuller rows look calmer: a last row with one card loses a little.
    if (n % cols != 0 && n % cols < cols / 2) total *= 0.97;
    if (total > best_area) {
      best_area = total;
      best = cols;
    }
  }
  return best;
}

Layout layout(int width, int height, int desks, bool can_add, const std::vector<double> &aspects, double scale) {
  Layout l;
  desks = std::max(1, desks);
  // The row of workspaces at the bottom.
  int card_w = px(176, scale), card_h = px(132, scale), gap = px(18, scale);
  int strip_h = card_h + px(36, scale);
  int count = desks + (can_add ? 1 : 0);
  int strip_w = count * card_w + (count - 1) * gap;
  if (strip_w > width - px(48, scale)) {
    card_w = std::max(px(80, scale), (width - px(48, scale) - (count - 1) * gap) / count);
    card_h = card_w * 3 / 4;
    strip_w = count * card_w + (count - 1) * gap;
  }
  int sy = height - strip_h;
  int sx = (width - strip_w) / 2;
  for (int i = 0; i < desks; ++i) l.desks.push_back({sx + i * (card_w + gap), sy, card_w, card_h});
  if (can_add) l.add = {sx + desks * (card_w + gap), sy, card_w, card_h};
  // The windows above it.
  int margin = px(56, scale), top = px(48, scale), title = px(34, scale), wgap = px(28, scale);
  l.grid = {margin, top, width - 2 * margin, sy - top - px(40, scale)};
  int n = int(aspects.size());
  if (n == 0 || l.grid.w <= 0 || l.grid.h <= 0) return l;
  int cols = best_columns(n, l.grid, wgap, title, aspects);
  int rows = (n + cols - 1) / cols;
  int cw = (l.grid.w - (cols - 1) * wgap) / cols, ch = (l.grid.h - (rows - 1) * wgap) / rows;
  // Not larger than a third of the screen: a single window stays a card.
  int max_w = std::max(px(240, scale), width * 2 / 5);
  cw = std::min(cw, max_w);
  ch = std::min(ch, int(cw * 0.8) + title);
  int used_h = rows * ch + (rows - 1) * wgap;
  int y0 = l.grid.y + (l.grid.h - used_h) / 2;
  for (int r = 0; r < rows; ++r) {
    int in_row = std::min(cols, n - r * cols);
    int row_w = in_row * cw + (in_row - 1) * wgap;
    int x0 = l.grid.x + (l.grid.w - row_w) / 2;
    for (int c = 0; c < in_row; ++c) l.windows.push_back({x0 + c * (cw + wgap), y0 + r * (ch + wgap), cw, ch});
  }
  l.columns = cols;
  return l;
}

int move_selection(int current, int n, int columns, int dx, int dy) {
  if (n <= 0) return -1;
  if (current < 0 || current >= n) return 0;
  columns = std::max(1, columns);
  int next = current + dx + dy * columns;
  if (dy != 0 && (next < 0 || next >= n)) return current;
  return (next % n + n) % n;
}

}  // namespace taskview
}  // namespace sde
