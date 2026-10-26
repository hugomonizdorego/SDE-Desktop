// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "collage.h"

#include <glib.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "draw.h"
#include "edit.h"
#include "i18n.h"

namespace photo {

namespace {

struct Area {
  double x = 0, y = 0, w = 0, h = 0;
};

double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// The geometric mean of the photos' aspect ratios (items [from, to)).
double mean_aspect(const Collage &c, int from = 0, int to = -1) {
  if (to < 0) to = int(c.items.size());
  double s = 0;
  int n = 0;
  for (int i = from; i < to; ++i, ++n) s += std::log(c.items[size_t(i)].aspect());
  return n ? std::exp(s / n) : 4.0 / 3.0;
}

// Deterministic pseudo-random numbers in [0, 1): the scatter of prints
// must not change from one frame to the next.
double hash01(unsigned seed, int i, int k) {
  uint32_t h = seed * 2654435761u ^ uint32_t(i + 1) * 2246822519u ^ uint32_t(k + 7) * 3266489917u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  h *= 3266489917u;
  h ^= h >> 16;
  return (h & 0xFFFFFF) / double(0x1000000);
}

double luma(uint32_t c) {
  return (0.2126 * ((c >> 16) & 0xFF) + 0.7152 * ((c >> 8) & 0xFF) + 0.0722 * (c & 0xFF)) / 255.0;
}

uint32_t mix(uint32_t a, uint32_t b, double t) {
  auto ch = [&](int s) {
    double v = ((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t;
    return uint32_t(std::lround(clampd(v, 0, 255))) << s;
  };
  return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

// Text that reads well on |bg|.
uint32_t ink_for(uint32_t bg) { return luma(bg) > 0.55 ? 0xFF26221F : 0xFFF7F5F2; }

// Rows of a grid: n cells in |a| with |gap|, the shape closest to |aspect|;
// the last row is stretched to the full width.
std::vector<Area> grid_areas(int n, const Area &a, double gap, double aspect) {
  std::vector<Area> out;
  if (n <= 0 || a.w <= 1 || a.h <= 1) return out;
  int best_cols = 1;
  double best = 1e18;
  for (int cols = 1; cols <= n; ++cols) {
    int rows = (n + cols - 1) / cols;
    double ch = (a.h - (rows - 1) * gap) / rows, cw = (a.w - (cols - 1) * gap) / cols;
    if (ch <= 1 || cw <= 1) continue;
    int last = n - (rows - 1) * cols;
    double lw = (a.w - (last - 1) * gap) / last;
    double score = ((n - last) * std::fabs(std::log(cw / ch / aspect)) + last * std::fabs(std::log(lw / ch / aspect))) / n;
    if (score < best - 1e-9) {
      best = score;
      best_cols = cols;
    }
  }
  int cols = best_cols, rows = (n + cols - 1) / cols;
  double ch = std::max(1.0, (a.h - (rows - 1) * gap) / rows);
  for (int r = 0; r < rows; ++r) {
    int k = r + 1 < rows ? cols : n - (rows - 1) * cols;
    double cw = std::max(1.0, (a.w - (k - 1) * gap) / k);
    for (int j = 0; j < k; ++j) out.push_back({a.x + j * (cw + gap), a.y + r * (ch + gap), cw, ch});
  }
  return out;
}

// Justified rows (a mosaic): photos keep their order, every row spans the
// full width, and the number of rows is the one whose natural height is
// closest to the area's.
std::vector<Area> justified_areas(const std::vector<double> &aspects, const Area &a, double gap) {
  std::vector<Area> out;
  int n = int(aspects.size());
  if (n == 0 || a.w <= 1 || a.h <= 1) return out;
  double total = std::accumulate(aspects.begin(), aspects.end(), 0.0);
  std::vector<int> best_breaks{0};
  double best = 1e18;
  for (int rows = 1; rows <= n; ++rows) {
    double target = total / rows, acc = 0;
    std::vector<int> breaks{0};
    for (int i = 0; i < n; ++i) {
      bool first_in_row = i == breaks.back();
      int rows_left = rows - int(breaks.size());
      if (!first_in_row && rows_left > 0 &&
          (n - i <= rows_left || acc + aspects[size_t(i)] - target > target - acc)) {
        breaks.push_back(i);
        acc = 0;
      }
      acc += aspects[size_t(i)];
    }
    if (int(breaks.size()) != rows) continue;
    double natural = 0, hmax = 0, hmin = 1e18;
    for (int r = 0; r < rows; ++r) {
      int b = breaks[size_t(r)], e = r + 1 < rows ? breaks[size_t(r) + 1] : n;
      double sum = 0;
      for (int i = b; i < e; ++i) sum += aspects[size_t(i)];
      double h = (a.w - (e - b - 1) * gap) / sum;
      natural += h;
      hmax = std::max(hmax, h);
      hmin = std::min(hmin, h);
    }
    double avail = a.h - (rows - 1) * gap;
    if (avail <= 1 || natural <= 0) continue;
    double score = std::fabs(std::log(avail / natural)) + 0.35 * std::log(hmax / hmin);
    if (score < best) {
      best = score;
      best_breaks = breaks;
    }
  }
  int rows = int(best_breaks.size());
  std::vector<double> heights;
  double natural = 0;
  for (int r = 0; r < rows; ++r) {
    int b = best_breaks[size_t(r)], e = r + 1 < rows ? best_breaks[size_t(r) + 1] : n;
    double sum = 0;
    for (int i = b; i < e; ++i) sum += aspects[size_t(i)];
    heights.push_back((a.w - (e - b - 1) * gap) / sum);
    natural += heights.back();
  }
  double k = std::max(1.0, a.h - (rows - 1) * gap) / natural;
  double y = a.y;
  for (int r = 0; r < rows; ++r) {
    int b = best_breaks[size_t(r)], e = r + 1 < rows ? best_breaks[size_t(r) + 1] : n;
    double row_w = a.w - (e - b - 1) * gap, sum = 0;
    for (int i = b; i < e; ++i) sum += aspects[size_t(i)];
    double x = a.x, h = std::max(1.0, heights[size_t(r)] * k);
    for (int i = b; i < e; ++i) {
      double w = std::max(1.0, row_w * aspects[size_t(i)] / sum);
      out.push_back({x, y, w, h});
      x += w + gap;
    }
    y += h + gap;
  }
  return out;
}

void add_cell(CollageLayout &l, int item, const Area &a, double radius_pct, double border) {
  CollageCell cell;
  cell.item = item;
  cell.x = a.x;
  cell.y = a.y;
  cell.w = a.w;
  cell.h = a.h;
  cell.radius = std::min(a.w, a.h) * clampd(radius_pct, 0, 50) / 100;
  cell.border = std::min(border, std::min(a.w, a.h) / 4);
  l.cells.push_back(cell);
}

// A caption band at the bottom for designs without a place for a title.
Area title_band(const Collage &c, CollageLayout &l, Area a, double gap) {
  if (!c.show_title || c.title.empty() || a.h < 40) return a;
  double band = std::max(40.0, c.height * 0.1);
  CollageText t;
  t.text = c.title;
  t.x = a.x;
  t.w = a.w;
  t.h = band * 0.8;
  t.y = a.y + a.h - band + (band - t.h) / 2 + gap / 2;
  t.size = band * 0.52;
  t.color = ink_for(c.background);
  l.texts.push_back(t);
  a.h -= band;
  return a;
}

void layout_grid_like(const Collage &c, CollageLayout &l, const Area &a, double gap, double border, bool panorama) {
  int n = int(c.items.size());
  std::vector<Area> cells;
  if (panorama) {
    // Tall slices side by side; more rows when they get too thin.
    int best_rows = 1;
    double best = 1e18;
    for (int rows = 1; rows <= n; ++rows) {
      int per = (n + rows - 1) / rows;
      double cw = (a.w - (per - 1) * gap) / per, ch = (a.h - (rows - 1) * gap) / rows;
      if (cw <= 1 || ch <= 1) continue;
      double score = std::fabs(std::log(cw / ch / 0.62));
      if (score < best - 1e-9) {
        best = score;
        best_rows = rows;
      }
    }
    int per = (n + best_rows - 1) / best_rows;
    double ch = (a.h - (best_rows - 1) * gap) / best_rows;
    int i = 0;
    for (int r = 0; r < best_rows && i < n; ++r) {
      int k = std::min(per, n - i);
      double cw = (a.w - (k - 1) * gap) / k;
      for (int j = 0; j < k; ++j, ++i) cells.push_back({a.x + j * (cw + gap), a.y + r * (ch + gap), cw, ch});
    }
  } else {
    cells = grid_areas(n, a, gap, mean_aspect(c));
  }
  for (size_t i = 0; i < cells.size(); ++i) add_cell(l, int(i), cells[i], c.radius, border);
}

void layout_mosaic(const Collage &c, CollageLayout &l, const Area &a, double gap, double border, int from = 0) {
  std::vector<double> aspects;
  for (size_t i = size_t(from); i < c.items.size(); ++i) aspects.push_back(c.items[i].aspect());
  auto cells = justified_areas(aspects, a, gap);
  for (size_t i = 0; i < cells.size(); ++i) add_cell(l, from + int(i), cells[i], c.radius, border);
}

void layout_featured(const Collage &c, CollageLayout &l, const Area &a, double gap, double border) {
  int n = int(c.items.size());
  if (n == 1) {
    add_cell(l, 0, a, c.radius, border);
    return;
  }
  double f = n == 2 ? 0.58 : 0.64;
  Area big = a, rest = a;
  if (a.w >= a.h) {
    big.w = (a.w - gap) * f;
    rest.x = a.x + big.w + gap;
    rest.w = a.w - big.w - gap;
  } else {
    big.h = (a.h - gap) * f;
    rest.y = a.y + big.h + gap;
    rest.h = a.h - big.h - gap;
  }
  add_cell(l, 0, big, c.radius, border);
  auto cells = grid_areas(n - 1, rest, gap, mean_aspect(c, 1));
  for (size_t i = 0; i < cells.size(); ++i) add_cell(l, int(i) + 1, cells[i], c.radius, border);
}

void layout_polaroid(const Collage &c, CollageLayout &l, const Area &a) {
  int n = int(c.items.size());
  // A print: margins of 6 % of its width, 22 % at the bottom; the photo
  // nearly square.
  const double side = 0.06, bottom = 0.22, photo_ratio = 0.92;
  const double card_aspect = 1 / (side + (1 - 2 * side) * photo_ratio + bottom);
  auto slots = grid_areas(n, a, 0, card_aspect);
  double s = std::min(a.w, a.h);
  for (int i = 0; i < n; ++i) {
    const Area &slot = slots[size_t(i)];
    double cw = std::min(slot.w, slot.h * card_aspect) * (n == 1 ? 0.78 : 1.06);
    double ch = cw / card_aspect;
    double jx = (hash01(c.seed, i, 1) - 0.5) * 0.16 * slot.w, jy = (hash01(c.seed, i, 2) - 0.5) * 0.14 * slot.h;
    double cx = clampd(slot.x + slot.w / 2 + jx, a.x + cw * 0.45, a.x + a.w - cw * 0.45);
    double cy = clampd(slot.y + slot.h / 2 + jy, a.y + ch * 0.45, a.y + a.h - ch * 0.45);
    double sign = (i % 2) ? 1 : -1;
    double angle = sign * (1.5 + hash01(c.seed, i, 3) * 6.5) * M_PI / 180;
    if (n == 1) angle = -2.5 * M_PI / 180;
    CollageCell cell;
    cell.item = i;
    cell.card_side = cell.card_top = cw * side;
    cell.card_bottom = cw * bottom;
    cell.w = cw - 2 * cell.card_side;
    cell.h = cell.w * photo_ratio;
    // The print is centred on (cx, cy); the photo sits higher in it.
    double card_top_y = cy - ch / 2;
    cell.x = cx - cell.w / 2;
    cell.y = card_top_y + cell.card_top;
    cell.angle = angle;
    cell.radius = std::min(cell.w, cell.h) * clampd(c.radius, 0, 50) / 100;
    cell.card_color = 0xFFFBFAF6;
    cell.shadow = true;
    l.cells.push_back(cell);
  }
  if (c.show_title && !c.title.empty()) {
    CollageText t;
    t.text = c.title;
    t.x = a.x;
    t.w = a.w;
    t.h = s * 0.1;
    t.y = a.y + a.h - t.h;
    t.size = s * 0.07;
    t.serif = true;
    t.italic = true;
    t.color = ink_for(c.background);
    l.texts.push_back(t);
  }
}

void layout_filmstrip(const Collage &c, CollageLayout &l, Area a, double gap) {
  int n = int(c.items.size());
  bool vertical = a.h > a.w;
  double len = vertical ? a.h : a.w, across = vertical ? a.w : a.h;
  double sgap = std::max(gap, across * 0.03);
  int best_s = 1;
  double best = 1e18;
  for (int s = 1; s <= n; ++s) {
    int per = (n + s - 1) / s;
    double t = (across - (s - 1) * sgap) / s;
    if (t <= 4) break;
    double fh = t * 0.7, fg = t * 0.07, fw = (len - fg * (per + 1)) / per;
    if (fw <= 2) continue;
    double score = std::fabs(std::log(fw / fh / 1.5)) + (t > across * 0.5 && n > 1 ? 0.2 : 0);
    if (score < best - 1e-9) {
      best = score;
      best_s = s;
    }
  }
  int s = best_s, per = (n + s - 1) / s;
  double t = (across - (s - 1) * sgap) / s;
  // Strips no thicker than needed for 3:2 frames along the full length.
  double fg = t * 0.07, fw = (len - fg * (per + 1)) / per;
  double fh = std::min(t * 0.7, fw / 1.5);
  t = fh / 0.7;
  fg = t * 0.07;
  double block = s * t + (s - 1) * sgap;
  double start = (vertical ? a.x : a.y) + (across - block) / 2;
  int i = 0;
  for (int k = 0; k < s; ++k) {
    double off = start + k * (t + sgap);
    CollageDeco strip;
    strip.kind = CollageDeco::FilmStrip;
    strip.vertical = vertical;
    strip.radius = t * 0.04;
    strip.color = 0xFF1A1A1D;
    strip.color2 = c.background;
    if (vertical) {
      strip.x = off;
      strip.y = a.y;
      strip.w = t;
      strip.h = len;
    } else {
      strip.x = a.x;
      strip.y = off;
      strip.w = len;
      strip.h = t;
    }
    l.under.push_back(strip);
    for (int j = 0; j < per; ++j) {
      double along = (vertical ? a.y : a.x) + fg + j * (fw + fg);
      Area f;
      if (vertical) f = {off + (t - fh) / 2, along, fh, fw};
      else f = {along, off + (t - fh) / 2, fw, fh};
      if (i < n) {
        add_cell(l, i++, f, std::max(c.radius, 3.0), 0);
      } else {
        CollageDeco blank;
        blank.kind = CollageDeco::Fill;
        blank.x = f.x;
        blank.y = f.y;
        blank.w = f.w;
        blank.h = f.h;
        blank.radius = std::min(f.w, f.h) * 0.03;
        blank.color = 0xFF2C2C31;
        l.under.push_back(blank);
      }
    }
  }
  if (c.show_title && !c.title.empty()) {
    CollageText tt;
    tt.text = c.title;
    double room = (vertical ? a.x + (across - block) / 2 - a.x : start - a.y);
    tt.x = a.x;
    tt.w = a.w;
    tt.h = std::max(room * 0.8, c.height * 0.06);
    tt.y = vertical ? a.y + a.h - tt.h : a.y + std::max(0.0, room - tt.h) / 2;
    tt.size = std::min(tt.h * 0.6, c.height * 0.08);
    tt.color = ink_for(c.background);
    l.texts.push_back(tt);
  }
}

void layout_magazine(const Collage &c, CollageLayout &l, double gap) {
  int n = int(c.items.size());
  double W = c.width, H = c.height, s = std::min(W, H);
  add_cell(l, 0, {0, 0, W, H}, 0, 0);
  l.cover = true;
  std::string masthead = c.show_title ? c.shown_title() : std::string();
  if (!masthead.empty()) {
    CollageText t;
    t.text = masthead;
    t.upper = true;
    t.x = W * 0.05;
    t.y = H * 0.025;
    t.w = W * 0.9;
    t.h = H * 0.15;
    t.size = H * 0.14;
    t.color = 0xFFFFFFFF;
    t.shadow = true;
    l.texts.push_back(t);
  }
  if (n == 1) return;
  // Cover lines: the number of photos, large.
  CollageText big;
  big.text = std::to_string(n);
  big.x = W * 0.06;
  big.y = H * 0.3;
  big.w = W * 0.4;
  big.h = H * 0.11;
  big.size = H * 0.1;
  big.align = 0;
  big.color = 0xFFFFFFFF;
  big.shadow = true;
  l.texts.push_back(big);
  CollageText sub = big;
  sub.text = n == 1 ? _("PHOTO") : _("PHOTOS");
  sub.y = big.y + big.h;
  sub.h = H * 0.04;
  sub.size = H * 0.032;
  l.texts.push_back(sub);
  // The other photos as small prints along the bottom.
  int k = n - 1;
  int rows = k <= 5 ? 1 : k <= 12 ? 2 : (k + 6) / 7;
  int per = (k + rows - 1) / rows;
  double band_h = std::min(H * 0.42, H * (rows == 1 ? 0.22 : 0.17) * rows);
  double side = W * 0.05, bottom = H * 0.035;
  double row_h = (band_h - (rows - 1) * gap) / rows;
  double border = s * 0.008;
  int i = 1;
  double cw = std::min(row_h * 1.25, (W - 2 * side - (per - 1) * gap) / per);
  double ch = std::min(row_h, cw);
  double top = H - bottom - (rows * ch + (rows - 1) * gap);
  for (int r = 0; r < rows; ++r) {
    int m = std::min(per, n - i);
    double total = m * cw + (m - 1) * gap;
    double x = (W - total) / 2, y = top + r * (ch + gap);
    for (int j = 0; j < m; ++j, ++i) {
      add_cell(l, i, {x + j * (cw + gap), y, cw, ch}, c.radius, border);
      l.cells.back().shadow = true;
    }
  }
}

void layout_shapes(const Collage &c, CollageLayout &l, Area a, double gap) {
  int n = int(c.items.size());
  if (c.shape == CollageShape::Heart) {
    double hw = std::min(a.w, a.h * 1.106), hh = hw / 1.106;
    double hx = a.x + (a.w - hw) / 2, hy = a.y + (a.h - hh) / 2;
    auto heart = heart_polygon(hx, hy, hw, hh);
    std::vector<Area> cells;
    if (n <= 4) {
      // A few photos: vertical slices of the heart.
      double cw = (hw - (n - 1) * gap) / n;
      for (int i = 0; i < n; ++i) cells.push_back({hx + i * (cw + gap), hy, cw, hh});
    } else {
      // A grid over the heart: the coarsest one with a cell for every photo
      // (cells mostly inside the heart); then neighbours in the fullest rows
      // are joined until there is one cell per photo, so the whole heart
      // stays covered.
      std::vector<std::vector<Area>> grid;
      int count = 0;
      for (int cols = 2; cols <= 64; ++cols) {
        double cs = hw / cols;
        int rows = int(std::ceil(hh / cs - 1e-9));
        grid.clear();
        count = 0;
        for (int r = 0; r < rows; ++r) {
          std::vector<Area> row;
          double y = hy + r * cs, ch = std::min(cs, hy + hh - y);
          for (int k = 0; k < cols; ++k) {
            Area ca{hx + k * cs, y, cs, ch};
            int in = 0;
            for (int sy = 0; sy < 4; ++sy)
              for (int sx = 0; sx < 4; ++sx)
                in += inside_polygon(heart, ca.x + (sx + 0.5) * cs / 4, ca.y + (sy + 0.5) * ch / 4);
            if (in >= 6) row.push_back(ca);
          }
          count += int(row.size());
          if (!row.empty()) grid.push_back(row);
        }
        if (count >= n) break;
      }
      while (count > n) {
        int best_r = -1, best_k = -1;
        size_t best_size = 0;
        double best_w = 1e18;
        for (size_t r = 0; r < grid.size(); ++r) {
          const auto &row = grid[r];
          for (size_t k = 0; k + 1 < row.size(); ++k) {
            if (std::fabs(row[k].x + row[k].w - row[k + 1].x) > 1e-6) continue;  // not neighbours
            double merged = row[k].w + row[k + 1].w;
            if (row.size() > best_size || (row.size() == best_size && merged < best_w - 1e-9)) {
              best_size = row.size();
              best_w = merged;
              best_r = int(r);
              best_k = int(k);
            }
          }
        }
        if (best_r < 0) {
          // Nothing left to join: leave out the last cell.
          grid.back().pop_back();
          if (grid.back().empty()) grid.pop_back();
        } else {
          auto &row = grid[size_t(best_r)];
          row[size_t(best_k)].w += row[size_t(best_k) + 1].w;
          row.erase(row.begin() + best_k + 1);
        }
        --count;
      }
      // Cells reach out to the edge of the heart in their row (the heart
      // clips them): the tip, the sides and the notch between the lobes
      // stay covered.
      for (size_t r = 0; r < grid.size(); ++r) {
        auto &row = grid[r];
        double y0 = r == 0 ? hy : row.front().y, y1 = r + 1 == grid.size() ? hy + hh : row.front().y + row.front().h;
        double xmin = row.front().x, xmax = row.back().x + row.back().w;
        for (const auto &p : heart)
          if (p.second >= y0 && p.second <= y1) {
            xmin = std::min(xmin, p.first);
            xmax = std::max(xmax, p.first);
          }
        row.front().w += row.front().x - xmin;
        row.front().x = xmin;
        row.back().w = xmax - row.back().x;
        for (size_t k = 0; k + 1 < row.size(); ++k) row[k].w = row[k + 1].x - row[k].x;
        if (r == 0)
          for (auto &ca : row) {
            ca.h += ca.y - hy;
            ca.y = hy;
          }
        if (r + 1 == grid.size())
          for (auto &ca : row) ca.h = hy + hh - ca.y;
      }
      for (const auto &row : grid)
        for (Area ca : row) {
          ca.x += gap / 2;
          ca.y += gap / 2;
          ca.w -= gap;
          ca.h -= gap;
          cells.push_back(ca);
        }
    }
    for (size_t i = 0; i < cells.size(); ++i) {
      add_cell(l, int(i), cells[i], 0, 0);
      CollageCell &cell = l.cells.back();
      cell.clip = CellClip::Heart;
      cell.hx = hx;
      cell.hy = hy;
      cell.hw = hw;
      cell.hh = hh;
    }
    return;
  }
  // Circles.
  std::vector<Area> circles;  // squares holding the circles
  double s = std::min(a.w, a.h);
  if (n <= 3) {
    double d = std::min(a.h, (a.w - (n - 1) * gap) / n);
    double total = n * d + (n - 1) * gap;
    for (int i = 0; i < n; ++i)
      circles.push_back({a.x + (a.w - total) / 2 + i * (d + gap), a.y + (a.h - d) / 2, d, d});
  } else if (n <= 7) {
    // One large circle with the others around it.
    double R = (s / 2 - gap) / 2.24, r = R * 0.62, D = R + r + gap;
    double cx = a.x + a.w / 2, cy = a.y + a.h / 2;
    circles.push_back({cx - R, cy - R, 2 * R, 2 * R});
    for (int i = 0; i < n - 1; ++i) {
      double ang = -M_PI / 2 + 2 * M_PI * i / (n - 1);
      circles.push_back({cx + D * std::cos(ang) - r, cy + D * std::sin(ang) - r, 2 * r, 2 * r});
    }
  } else {
    // A honeycomb whose rows alternate between L and L - 1 circles, so that
    // every row is centred; a short last row takes the middle places.
    double best_d = 0;
    int best_l = 2, best_rows = 1;
    for (int len = 2; len <= n; ++len) {
      int rows = 0, cap = 0;
      while (cap < n) cap += (rows++ % 2) ? len - 1 : len;
      double d = std::min(a.w / len, a.h / (1 + (rows - 1) * 0.866));
      if (d > best_d) {
        best_d = d;
        best_l = len;
        best_rows = rows;
      }
    }
    double d = best_d, pitch = d * 0.866;
    double top = a.y + (a.h - (d + (best_rows - 1) * pitch)) / 2;
    int i = 0;
    for (int r = 0; r < best_rows && i < n; ++r) {
      int cap = r % 2 ? best_l - 1 : best_l, k = std::min(cap, n - i);
      double left = a.x + (a.w - cap * d) / 2 + ((cap - k) / 2) * d;
      for (int j = 0; j < k; ++j, ++i) circles.push_back({left + j * d, top + r * pitch, d, d});
    }
    gap = std::max(gap, d * 0.03);
  }
  for (size_t i = 0; i < circles.size(); ++i) {
    Area ca = circles[i];
    double inset = n > 7 ? gap / 2 : 0;
    ca.x += inset;
    ca.y += inset;
    ca.w -= 2 * inset;
    ca.h -= 2 * inset;
    add_cell(l, int(i), ca, 0, std::min(ca.w, ca.h) * c.border * 0.012);
    l.cells.back().clip = CellClip::Circle;
    l.cells.back().shadow = n <= 7;
  }
}

void layout_story(const Collage &c, CollageLayout &l, Area a, double gap) {
  int n = int(c.items.size());
  double W = c.width, H = c.height;
  CollageDeco grad;
  grad.kind = CollageDeco::Gradient;
  grad.w = W;
  grad.h = H;
  grad.color = c.background;
  grad.color2 = mix(c.background, luma(c.background) > 0.5 ? 0xFF000000 : 0xFFFFFFFF, 0.12);
  l.under.push_back(grad);
  std::string title = c.show_title ? c.shown_title() : std::string();
  if (!title.empty()) {
    CollageText t;
    t.text = title;
    t.x = a.x;
    t.y = a.y + H * 0.015;
    t.w = a.w;
    t.h = H * 0.075;
    t.size = std::min(H * 0.055, W * 0.11);
    t.color = ink_for(c.background);
    l.texts.push_back(t);
    a.y += H * 0.105;
    a.h -= H * 0.105;
  }
  double s = std::min(W, H);
  if (n > 8) {
    layout_mosaic(c, l, a, gap, s * c.border / 100);
    return;
  }
  double card = s * 0.025;
  auto card_cell = [&](int item, Area slot, double angle, double width_frac) {
    CollageCell cell;
    cell.item = item;
    double cw = slot.w * width_frac, ch = slot.h;
    cell.card_side = cell.card_top = cell.card_bottom = card;
    cell.w = cw - 2 * card;
    cell.h = ch - 2 * card;
    cell.x = slot.x + (slot.w - cw) / 2 + card;
    cell.y = slot.y + card;
    cell.angle = angle;
    cell.radius = std::min(cell.w, cell.h) * clampd(c.radius, 0, 50) / 100;
    cell.card_color = 0xFFFFFFFF;
    cell.shadow = true;
    l.cells.push_back(cell);
  };
  if (n <= 3) {
    double slot_h = (a.h - (n - 1) * gap) / n;
    for (int i = 0; i < n; ++i) {
      double shift = (i % 2 ? 1 : -1) * W * 0.035;
      double angle = (i % 2 ? 1.8 : -2.2) * M_PI / 180;
      if (n == 1) {
        shift = 0;
        angle = -1.5 * M_PI / 180;
      }
      card_cell(i, {a.x + shift, a.y + i * (slot_h + gap), a.w, slot_h}, angle, 0.88);
    }
    return;
  }
  // Two staggered columns.
  int left = (n + 1) / 2, right = n - left;
  double colw = (a.w - gap) / 2;
  double off = a.h * 0.06;
  for (int col = 0; col < 2; ++col) {
    int k = col ? right : left;
    double top = a.y + (col ? off : 0), height = a.h - off;
    double slot_h = (height - (k - 1) * gap) / k;
    for (int j = 0; j < k; ++j) {
      int item = col ? left + j : j;
      double angle = ((item % 3) - 1) * 1.4 * M_PI / 180;
      card_cell(item, {a.x + col * (colw + gap), top + j * (slot_h + gap), colw, slot_h}, angle, 0.96);
    }
  }
}

void layout_framed(const Collage &c, CollageLayout &l, double gap, double border) {
  double W = c.width, H = c.height, s = std::min(W, H);
  double fb = s * 0.024;
  CollageDeco frame;
  frame.kind = CollageDeco::Fill;
  frame.w = W;
  frame.h = H;
  frame.color = 0xFF2B2522;
  l.under.push_back(frame);
  CollageDeco mat;
  mat.kind = CollageDeco::Fill;
  mat.x = fb;
  mat.y = fb;
  mat.w = W - 2 * fb;
  mat.h = H - 2 * fb;
  mat.color = c.background;
  l.under.push_back(mat);
  CollageDeco bevel;
  bevel.kind = CollageDeco::Line;
  bevel.x = fb;
  bevel.y = fb;
  bevel.w = W - 2 * fb;
  bevel.h = H - 2 * fb;
  bevel.stroke = s * 0.004;
  bevel.color = 0x30000000;
  l.over.push_back(bevel);
  std::string title = c.show_title ? c.shown_title() : std::string();
  double side = fb + s * 0.075, top = fb + s * 0.075, bottom = fb + s * 0.075 + (title.empty() ? 0 : H * 0.12);
  Area win{side, top, W - 2 * side, H - top - bottom};
  CollageDeco line;
  line.kind = CollageDeco::Line;
  double o = s * 0.014;
  line.x = win.x - o;
  line.y = win.y - o;
  line.w = win.w + 2 * o;
  line.h = win.h + 2 * o;
  line.stroke = std::max(1.0, s * 0.0016);
  line.color = mix(c.background, 0xFF000000, 0.22);
  l.under.push_back(line);
  auto cells = grid_areas(int(c.items.size()), win, gap, mean_aspect(c));
  for (size_t i = 0; i < cells.size(); ++i) add_cell(l, int(i), cells[i], c.radius, border);
  if (!title.empty()) {
    CollageText t;
    t.text = title;
    t.x = win.x;
    t.y = win.y + win.h + H * 0.03;
    t.w = win.w;
    t.h = H * 0.075;
    t.size = H * 0.058;
    t.serif = true;
    t.italic = true;
    t.bold = false;
    t.color = ink_for(c.background);
    l.texts.push_back(t);
  }
}

// ----------------------------------------------------------------- drawing

void set_rgba(cairo_t *cr, uint32_t c, double alpha = 1) {
  cairo_set_source_rgba(cr, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0,
                        ((c >> 24) & 0xFF) / 255.0 * alpha);
}

void rounded_path(cairo_t *cr, double x, double y, double w, double h, double r) {
  r = std::max(0.0, std::min(r, std::min(w, h) / 2));
  if (r < 0.5) {
    cairo_rectangle(cr, x, y, w, h);
    return;
  }
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

void soft_shadow(cairo_t *cr, double x, double y, double w, double h, double r, double spread, double alpha,
                 bool fast) {
  // A few growing translucent shapes: cheap and soft enough (one while
  // something is dragged).
  int layers = fast ? 1 : 4;
  for (int i = layers; i >= 1; --i) {
    double g = spread * i / 4;
    rounded_path(cr, x - g * 0.6, y - g * 0.3 + spread * 0.35, w + g * 1.2, h + g * 1.2, r + g);
    cairo_set_source_rgba(cr, 0, 0, 0, fast ? alpha * 0.5 : alpha / 4 * (1.2 - i * 0.2));
    cairo_fill(cr);
  }
}

void placeholder(cairo_t *cr, double x, double y, double w, double h) {
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
  cairo_pattern_add_color_stop_rgb(g, 0, 0.80, 0.83, 0.87);
  cairo_pattern_add_color_stop_rgb(g, 1, 0.70, 0.74, 0.79);
  cairo_rectangle(cr, x, y, w, h);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  double s = std::min(w, h) * 0.3, gx = x + (w - s) / 2, gy = y + (h - s * 0.8) / 2;
  cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
  cairo_move_to(cr, gx, gy + s * 0.8);
  cairo_line_to(cr, gx + s * 0.38, gy + s * 0.3);
  cairo_line_to(cr, gx + s * 0.62, gy + s * 0.58);
  cairo_line_to(cr, gx + s * 0.76, gy + s * 0.44);
  cairo_line_to(cr, gx + s, gy + s * 0.8);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_arc(cr, gx + s * 0.75, gy + s * 0.12, s * 0.09, 0, 2 * M_PI);
  cairo_fill(cr);
}

// Fills |shape| (a path within the box x, y, w, h) with the part of the
// photo the item shows there. Filling with an image pattern, instead of a
// clip and a paint, keeps rotated and rounded cells cheap to draw.
void paint_photo(cairo_t *cr, cairo_surface_t *s, const CollageItem &it, double x, double y, double w, double h,
                 bool fast, const std::function<void()> &shape) {
  if (w <= 0 || h <= 0) return;
  if (!s) {
    cairo_save(cr);
    cairo_new_path(cr);
    shape();
    cairo_clip(cr);
    placeholder(cr, x, y, w, h);
    cairo_restore(cr);
    return;
  }
  int iw = cairo_image_surface_get_width(s), ih = cairo_image_surface_get_height(s);
  double sx, sy, sw, sh;
  cell_source(it, iw, ih, w, h, sx, sy, sw, sh);
  cairo_pattern_t *p = cairo_pattern_create_for_surface(s);
  cairo_matrix_t m;  // user space to photo pixels
  cairo_matrix_init_translate(&m, sx, sy);
  cairo_matrix_scale(&m, sw / w, sh / h);
  cairo_matrix_translate(&m, -x, -y);
  cairo_pattern_set_matrix(p, &m);
  cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
  double k = w / sw;
  cairo_pattern_set_filter(p, fast ? CAIRO_FILTER_BILINEAR : k < 0.75 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
  cairo_new_path(cr);
  shape();
  cairo_set_source(cr, p);
  cairo_fill(cr);
  cairo_pattern_destroy(p);
}

void heart_path(cairo_t *cr, const CollageCell &c) {
  auto poly = heart_polygon(c.hx, c.hy, c.hw, c.hh);
  for (size_t i = 0; i < poly.size(); ++i) {
    if (i == 0) cairo_move_to(cr, poly[i].first, poly[i].second);
    else cairo_line_to(cr, poly[i].first, poly[i].second);
  }
  cairo_close_path(cr);
}

void draw_cell(cairo_t *cr, const Collage &c, const CollageCell &cell, const CollageImages &img, bool fast) {
  cairo_save(cr);
  cairo_translate(cr, cell.cx(), cell.cy());
  if (cell.angle != 0) cairo_rotate(cr, cell.angle);
  double x = -cell.w / 2, y = -cell.h / 2, w = cell.w, h = cell.h;
  double s = std::min(c.width, c.height);
  if (cell.card_color) {
    double cx = x - cell.card_side, cy = y - cell.card_top;
    double cw = w + 2 * cell.card_side, ch = h + cell.card_top + cell.card_bottom;
    double r = s * 0.004;
    if (cell.shadow) soft_shadow(cr, cx, cy, cw, ch, r, s * 0.018, 0.34, fast);
    rounded_path(cr, cx, cy, cw, ch, r);
    set_rgba(cr, cell.card_color);
    cairo_fill(cr);
  } else if (cell.shadow) {
    if (cell.clip == CellClip::Circle) {
      cairo_new_sub_path(cr);
      cairo_arc(cr, s * 0.004, s * 0.008, w / 2 + s * 0.006, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
      cairo_fill(cr);
    } else {
      soft_shadow(cr, x, y, w, h, cell.radius, s * 0.014, 0.4, fast);
    }
  }
  cairo_surface_t *photo = cell.item >= 0 && img.photo ? img.photo(cell.item) : nullptr;
  const CollageItem *it = cell.item >= 0 && cell.item < int(c.items.size()) ? &c.items[size_t(cell.item)] : nullptr;
  CollageItem none;
  if (cell.clip == CellClip::Circle) {
    double r = std::min(w, h) / 2;
    if (cell.border > 0) {
      cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
      set_rgba(cr, c.border_color);
      cairo_fill(cr);
      r -= cell.border;
    }
    r = std::max(1.0, r);
    paint_photo(cr, photo, it ? *it : none, -r, -r, 2 * r, 2 * r, fast, [&] {
      cairo_new_sub_path(cr);
      cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    });
  } else if (cell.clip == CellClip::Heart) {
    cairo_restore(cr);
    cairo_save(cr);
    heart_path(cr, cell);
    cairo_clip(cr);
    paint_photo(cr, photo, it ? *it : none, cell.x, cell.y, cell.w, cell.h, fast,
                [&] { cairo_rectangle(cr, cell.x, cell.y, cell.w, cell.h); });
  } else {
    if (cell.border > 0) {
      rounded_path(cr, x, y, w, h, cell.radius);
      set_rgba(cr, c.border_color);
      cairo_fill(cr);
      x += cell.border;
      y += cell.border;
      w -= 2 * cell.border;
      h -= 2 * cell.border;
    }
    if (w > 0 && h > 0) {
      double r = std::max(0.0, cell.radius - cell.border);
      paint_photo(cr, photo, it ? *it : none, x, y, w, h, fast, [&] { rounded_path(cr, x, y, w, h, r); });
    }
  }
  cairo_restore(cr);
}

void draw_deco(cairo_t *cr, const CollageDeco &d) {
  switch (d.kind) {
    case CollageDeco::Fill:
      rounded_path(cr, d.x, d.y, d.w, d.h, d.radius);
      set_rgba(cr, d.color);
      cairo_fill(cr);
      break;
    case CollageDeco::Gradient: {
      cairo_pattern_t *g = cairo_pattern_create_linear(d.x, d.y, d.x, d.y + d.h);
      auto stop = [&](double at, uint32_t c) {
        cairo_pattern_add_color_stop_rgba(g, at, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
                                          (c & 0xFF) / 255.0, ((c >> 24) & 0xFF) / 255.0);
      };
      stop(0, d.color);
      stop(1, d.color2);
      cairo_rectangle(cr, d.x, d.y, d.w, d.h);
      cairo_set_source(cr, g);
      cairo_fill(cr);
      cairo_pattern_destroy(g);
      break;
    }
    case CollageDeco::Line:
      cairo_rectangle(cr, d.x + d.stroke / 2, d.y + d.stroke / 2, d.w - d.stroke, d.h - d.stroke);
      cairo_set_line_width(cr, d.stroke);
      set_rgba(cr, d.color);
      cairo_stroke(cr);
      break;
    case CollageDeco::FilmStrip: {
      rounded_path(cr, d.x, d.y, d.w, d.h, d.radius);
      set_rgba(cr, d.color);
      cairo_fill(cr);
      // Sprocket holes along both edges.
      double t = d.vertical ? d.w : d.h, len = d.vertical ? d.h : d.w;
      double hw = t * 0.075, hh = t * 0.055, pitch = t * 0.14, edge = t * 0.045;
      int count = std::max(1, int(len / pitch));
      double start = (len - (count - 1) * pitch) / 2;
      set_rgba(cr, d.color2, 0.92);
      for (int i = 0; i < count; ++i) {
        double along = start + i * pitch;
        for (int side = 0; side < 2; ++side) {
          double off = side ? t - edge - hh : edge;
          if (d.vertical) rounded_path(cr, d.x + off, d.y + along - hw / 2, hh, hw, hh * 0.3);
          else rounded_path(cr, d.x + along - hw / 2, d.y + off, hw, hh, hh * 0.3);
          cairo_fill(cr);
        }
      }
      break;
    }
  }
}

void draw_text(cairo_t *cr, const CollageText &t) {
  if (t.text.empty() || t.w <= 0 || t.h <= 0) return;
  std::string text = t.text;
  if (t.upper) {
    gchar *up = g_utf8_strup(text.c_str(), -1);
    text = up;
    g_free(up);
  }
  cairo_save(cr);
  cairo_select_font_face(cr, t.serif ? "Serif" : "Sans", t.italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                         t.bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  double size = std::max(1.0, t.size);
  cairo_set_font_size(cr, size);
  cairo_text_extents_t te;
  cairo_text_extents(cr, text.c_str(), &te);
  if (te.x_advance > t.w && te.x_advance > 0) {
    size *= t.w / te.x_advance;
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, text.c_str(), &te);
  }
  cairo_font_extents_t fe;
  cairo_font_extents(cr, &fe);
  double x = t.align == 0 ? t.x : t.align == 2 ? t.x + t.w - te.x_advance : t.x + (t.w - te.x_advance) / 2;
  double y = t.y + (t.h - (fe.ascent + fe.descent)) / 2 + fe.ascent;
  if (t.shadow) {
    cairo_move_to(cr, x + size * 0.03, y + size * 0.04);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_show_text(cr, text.c_str());
  }
  cairo_move_to(cr, x, y);
  set_rgba(cr, t.color);
  cairo_show_text(cr, text.c_str());
  cairo_restore(cr);
}

}  // namespace

// ------------------------------------------------------------------ catalog

const std::vector<CollageDesignInfo> &collage_designs() {
  static const std::vector<CollageDesignInfo> list{
      {CollageDesign::Grid, "grid", N_("Grid"), N_("Even rows and columns"), 1080, 1080, 0xFFFFFFFF},
      {CollageDesign::Mosaic, "mosaic", N_("Mosaic"), N_("Rows that keep each photo's shape"), 1920, 1080,
       0xFFFFFFFF},
      {CollageDesign::Featured, "featured", N_("Featured"), N_("One large photo and the others beside it"), 1920,
       1080, 0xFFFFFFFF},
      {CollageDesign::Polaroid, "polaroid", N_("Prints"), N_("Instant prints scattered on a table"), 1920, 1080,
       0xFFE9E2D6},
      {CollageDesign::Filmstrip, "filmstrip", N_("Film Strip"), N_("Frames on strips of film"), 1920, 1080,
       0xFFECE7DF},
      {CollageDesign::Magazine, "magazine", N_("Magazine"), N_("A cover with a title and small photos"), 2480, 3508,
       0xFF101010},
      {CollageDesign::Shapes, "shapes", N_("Shapes"), N_("Photos in a heart or in circles"), 1080, 1080, 0xFFFCEEF1},
      {CollageDesign::Story, "story", N_("Story"), N_("A tall 9:16 picture for stories"), 1080, 1920, 0xFFF4EEE6},
      {CollageDesign::Panorama, "panorama", N_("Panorama"), N_("A wide strip of photos side by side"), 3240, 1080,
       0xFF121316},
      {CollageDesign::Framed, "framed", N_("Framed"), N_("A frame with a mat and a title"), 3508, 2480, 0xFFF3EEE4}};
  return list;
}

const CollageDesignInfo &design_info(CollageDesign d) {
  for (const auto &i : collage_designs())
    if (i.id == d) return i;
  return collage_designs()[0];
}

bool parse_design(const std::string &key, CollageDesign &out) {
  for (const auto &i : collage_designs())
    if (key == i.key) {
      out = i.id;
      return true;
    }
  return false;
}

const std::vector<CollageSize> &collage_sizes() {
  static const std::vector<CollageSize> list{{"square", N_("Square"), 1080, 1080},
                                             {"hd", N_("HD 16:9"), 1920, 1080},
                                             {"4k", N_("4K 16:9"), 3840, 2160},
                                             {"a4", N_("A4 Portrait"), 2480, 3508},
                                             {"a4-landscape", N_("A4 Landscape"), 3508, 2480},
                                             {"story", N_("Story 9:16"), 1080, 1920},
                                             {"panorama", N_("Panorama 3:1"), 3240, 1080}};
  return list;
}

int size_preset(int width, int height) {
  const auto &list = collage_sizes();
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i].width == width && list[i].height == height) return int(i);
  return -1;
}

bool Collage::operator==(const Collage &o) const {
  return design == o.design && items == o.items && width == o.width && height == o.height &&
         auto_size == o.auto_size && spacing == o.spacing && radius == o.radius && border == o.border &&
         border_color == o.border_color && background == o.background && auto_background == o.auto_background &&
         blur_background == o.blur_background && title == o.title && suggested_title == o.suggested_title &&
         show_title == o.show_title && shape == o.shape && seed == o.seed;
}

void set_design(Collage &c, CollageDesign d) {
  c.design = d;
  const CollageDesignInfo &info = design_info(d);
  if (c.auto_size) {
    c.width = info.width;
    c.height = info.height;
  }
  if (c.auto_background) c.background = info.background;
}

void set_size(Collage &c, int width, int height) {
  c.width = std::max(kMinCollageSide, std::min(kMaxCollageSide, width));
  c.height = std::max(kMinCollageSide, std::min(kMaxCollageSide, height));
  c.auto_size = false;
}

// ------------------------------------------------------------------- layout

CollageLayout layout_collage(const Collage &c) {
  CollageLayout l;
  if (c.items.empty() || c.width <= 0 || c.height <= 0) return l;
  double s = std::min(c.width, c.height);
  double gap = s * clampd(c.spacing, 0, 20) / 100, border = s * clampd(c.border, 0, 10) / 100;
  Area a{gap, gap, c.width - 2 * gap, c.height - 2 * gap};
  switch (c.design) {
    case CollageDesign::Grid:
      a = title_band(c, l, a, gap);
      layout_grid_like(c, l, a, gap, border, false);
      break;
    case CollageDesign::Mosaic:
      a = title_band(c, l, a, gap);
      layout_mosaic(c, l, a, gap, border);
      break;
    case CollageDesign::Featured:
      a = title_band(c, l, a, gap);
      layout_featured(c, l, a, gap, border);
      break;
    case CollageDesign::Polaroid: {
      double m = s * 0.06;
      layout_polaroid(c, l, {m, m, c.width - 2 * m, c.height - 2 * m});
      break;
    }
    case CollageDesign::Filmstrip: {
      double m = std::max(gap, s * 0.04);
      layout_filmstrip(c, l, {m, m, c.width - 2 * m, c.height - 2 * m}, gap);
      break;
    }
    case CollageDesign::Magazine: layout_magazine(c, l, std::max(gap, s * 0.012)); break;
    case CollageDesign::Shapes: {
      double m = std::max(gap, s * 0.04);
      Area sa = title_band(c, l, {m, m, c.width - 2 * m, c.height - 2 * m}, gap);
      layout_shapes(c, l, sa, gap);
      break;
    }
    case CollageDesign::Story: {
      double m = std::max(gap, s * 0.05);
      layout_story(c, l, {m, m, c.width - 2 * m, c.height - 2 * m}, std::max(gap, s * 0.02));
      break;
    }
    case CollageDesign::Panorama:
      a = title_band(c, l, a, gap);
      layout_grid_like(c, l, a, gap, border, true);
      break;
    case CollageDesign::Framed: layout_framed(c, l, gap, border); break;
  }
  return l;
}

void cell_source(const CollageItem &it, int iw, int ih, double cw, double ch, double &sx, double &sy, double &sw,
                 double &sh) {
  if (iw <= 0 || ih <= 0 || cw <= 0 || ch <= 0) {
    sx = sy = 0;
    sw = std::max(1, iw);
    sh = std::max(1, ih);
    return;
  }
  double zoom = clampd(it.zoom, 1, 5);
  double k = std::max(cw / iw, ch / ih) * zoom;
  sw = std::min(double(iw), cw / k);
  sh = std::min(double(ih), ch / k);
  sx = (iw - sw) * (clampd(it.pan_x, -1, 1) + 1) / 2;
  sy = (ih - sh) * (clampd(it.pan_y, -1, 1) + 1) / 2;
}

void pan_item(CollageItem &it, double cw, double ch, double dx, double dy) {
  double a = it.aspect(), zoom = clampd(it.zoom, 1, 5);
  // The photo as shown: covering the cell, times the zoom.
  double shown_w = std::max(cw, ch * a) * zoom, shown_h = std::max(cw / a, ch) * zoom;
  double tx = shown_w - cw, ty = shown_h - ch;
  if (tx > 0.5) it.pan_x = clampd(it.pan_x - 2 * dx / tx, -1, 1);
  if (ty > 0.5) it.pan_y = clampd(it.pan_y - 2 * dy / ty, -1, 1);
}

void zoom_item(CollageItem &it, double cw, double ch, double factor, double fx, double fy) {
  const int iw = int(std::lround(it.aspect() * 1000)), ih = 1000;
  double sx, sy, sw, sh;
  cell_source(it, iw, ih, cw, ch, sx, sy, sw, sh);
  double ux = sx + clampd(fx, 0, 1) * sw, uy = sy + clampd(fy, 0, 1) * sh;  // photo point under the pointer
  it.zoom = clampd(it.zoom * factor, 1, 5);
  cell_source(it, iw, ih, cw, ch, sx, sy, sw, sh);
  double nsx = ux - clampd(fx, 0, 1) * sw, nsy = uy - clampd(fy, 0, 1) * sh;
  it.pan_x = iw - sw > 0.5 ? clampd(2 * nsx / (iw - sw) - 1, -1, 1) : 0;
  it.pan_y = ih - sh > 0.5 ? clampd(2 * nsy / (ih - sh) - 1, -1, 1) : 0;
}

int cell_at(const CollageLayout &l, double x, double y) {
  for (size_t k = l.cells.size(); k-- > 0;) {
    const CollageCell &c = l.cells[k];
    double dx = x - c.cx(), dy = y - c.cy();
    if (c.angle != 0) {
      double cs = std::cos(-c.angle), sn = std::sin(-c.angle);
      double rx = dx * cs - dy * sn, ry = dx * sn + dy * cs;
      dx = rx;
      dy = ry;
    }
    double left = -c.w / 2 - c.card_side, right = c.w / 2 + c.card_side;
    double top = -c.h / 2 - c.card_top, bottom = c.h / 2 + c.card_bottom;
    if (dx < left || dx > right || dy < top || dy > bottom) continue;
    if (c.clip == CellClip::Circle && dx * dx + dy * dy > (c.w / 2) * (c.w / 2)) continue;
    if (l.cover && k == 0) return int(k);
    return int(k);
  }
  return -1;
}

int cell_of_item(const CollageLayout &l, int item) {
  for (size_t k = 0; k < l.cells.size(); ++k)
    if (l.cells[k].item == item) return int(k);
  return -1;
}

int needed_pixels(const CollageLayout &l, const Collage &c, int item, double scale) {
  if (item < 0 || item >= int(c.items.size())) return 0;
  const CollageItem &it = c.items[size_t(item)];
  double a = it.aspect(), need = 0;
  for (const auto &cell : l.cells) {
    if (cell.item != item) continue;
    double zoom = clampd(it.zoom, 1, 5);
    double shown_w = std::max(cell.w, cell.h * a) * zoom, shown_h = std::max(cell.w / a, cell.h) * zoom;
    need = std::max(need, std::max(shown_w, shown_h) * scale);
  }
  return int(std::ceil(need));
}

void move_item(Collage &c, int from, int to) {
  int n = int(c.items.size());
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
  CollageItem it = c.items[size_t(from)];
  c.items.erase(c.items.begin() + from);
  c.items.insert(c.items.begin() + to, it);
  int &b = c.blur_background;
  if (b == from) b = to;
  else if (from < b && b <= to) --b;
  else if (to <= b && b < from) ++b;
}

void swap_items(Collage &c, int a, int b) {
  int n = int(c.items.size());
  if (a < 0 || a >= n || b < 0 || b >= n || a == b) return;
  std::swap(c.items[size_t(a)], c.items[size_t(b)]);
  // Each photo keeps its own position within its new cell: start centred.
  for (int i : {a, b}) {
    c.items[size_t(i)].pan_x = c.items[size_t(i)].pan_y = 0;
    c.items[size_t(i)].zoom = 1;
  }
  if (c.blur_background == a) c.blur_background = b;
  else if (c.blur_background == b) c.blur_background = a;
}

void remove_item(Collage &c, int index) {
  if (index < 0 || index >= int(c.items.size())) return;
  c.items.erase(c.items.begin() + index);
  if (c.blur_background == index) c.blur_background = -1;
  else if (c.blur_background > index) --c.blur_background;
}

// ----------------------------------------------------------------- drawing

void draw_collage(cairo_t *cr, const Collage &c, const CollageLayout &l, double scale, const CollageImages &img,
                  bool fast) {
  cairo_save(cr);
  cairo_scale(cr, scale, scale);
  cairo_rectangle(cr, 0, 0, c.width, c.height);
  cairo_clip(cr);
  set_rgba(cr, c.background | 0xFF000000u);
  cairo_paint(cr);
  if (c.blur_background >= 0 && c.blur_background < int(c.items.size()) && !l.cover) {
    cairo_surface_t *b = img.backdrop ? img.backdrop(c.blur_background) : nullptr;
    bool own = false;
    if (!b && img.photo) {
      if (cairo_surface_t *p = img.photo(c.blur_background)) {
        b = blurred_backdrop(p);
        own = true;
      }
    }
    if (b) {
      double bw = cairo_image_surface_get_width(b), bh = cairo_image_surface_get_height(b);
      double k = std::max(c.width / bw, c.height / bh);
      cairo_save(cr);
      cairo_translate(cr, (c.width - bw * k) / 2, (c.height - bh * k) / 2);
      cairo_scale(cr, k, k);
      cairo_set_source_surface(cr, b, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
      cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
      cairo_paint(cr);
      cairo_restore(cr);
      // A veil keeps the photos in front.
      cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
      cairo_paint(cr);
      if (own) cairo_surface_destroy(b);
    }
  }
  for (const auto &d : l.under) draw_deco(cr, d);
  for (const auto &cell : l.cells) draw_cell(cr, c, cell, img, fast);
  for (const auto &d : l.over) draw_deco(cr, d);
  if (l.cover && !l.texts.empty()) {
    // A shade under the masthead keeps it readable on light photos.
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, c.height * 0.3);
    cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, 0.42);
    cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0);
    cairo_rectangle(cr, 0, 0, c.width, c.height * 0.3);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
  }
  for (const auto &t : l.texts) draw_text(cr, t);
  cairo_restore(cr);
}

cairo_surface_t *render_collage(const Collage &c, double scale, const CollageImages &img) {
  int w = std::max(1, int(std::lround(c.width * scale))), h = std::max(1, int(std::lround(c.height * scale)));
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  cairo_t *cr = cairo_create(s);
  draw_collage(cr, c, layout_collage(c), scale, img, false);
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}

cairo_surface_t *blurred_backdrop(cairo_surface_t *src, int max_dim) {
  if (!src) return nullptr;
  cairo_surface_t *small = downscale(src, max_dim);
  // downscale() may return |src| itself: blur a copy.
  int w = cairo_image_surface_get_width(small), h = cairo_image_surface_get_height(small);
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(out);
  cairo_set_source_surface(cr, small, 0, 0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(small);
  blur_region(out, 0, 0, w, h, std::max(2, std::max(w, h) / 22));
  return out;
}

// ------------------------------------------------------------------ history

void CollageHistory::reset(const Collage &c) {
  states_.assign(1, c);
  pos_ = 0;
}

void CollageHistory::push(const Collage &c) {
  if (c == states_[pos_]) return;
  states_.resize(pos_ + 1);
  states_.push_back(c);
  pos_ = states_.size() - 1;
  if (states_.size() > 200) {
    states_.erase(states_.begin());
    --pos_;
  }
}

void CollageHistory::amend(const Collage &c) { states_[pos_] = c; }

const Collage &CollageHistory::undo() {
  if (pos_ > 0) --pos_;
  return states_[pos_];
}

const Collage &CollageHistory::redo() {
  if (pos_ + 1 < states_.size()) ++pos_;
  return states_[pos_];
}

// ------------------------------------------------------------------- shapes

std::vector<std::pair<double, double>> heart_polygon(double x, double y, double w, double h, int points) {
  // The classic parametric heart: x = 16 sin^3 t, y = 13 cos t - 5 cos 2t -
  // 2 cos 3t - cos 4t, which spans x -16..16 and y -17..11.93 (flipped).
  std::vector<std::pair<double, double>> out;
  for (int i = 0; i < points; ++i) {
    double t = 2 * M_PI * i / points;
    double hx = 16 * std::pow(std::sin(t), 3);
    double hy = 13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t);
    out.push_back({x + (hx + 16) / 32 * w, y + (11.93 - hy) / 28.93 * h});
  }
  return out;
}

bool inside_polygon(const std::vector<std::pair<double, double>> &poly, double px, double py) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    double xi = poly[i].first, yi = poly[i].second, xj = poly[j].first, yj = poly[j].second;
    if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
  }
  return in;
}

}  // namespace photo
