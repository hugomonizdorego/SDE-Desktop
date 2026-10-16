// SPDX-License-Identifier: GPL-3.0-or-later
#include "glyphs.h"

#include <cmath>

namespace sde {
namespace glyph {

namespace {
void stroke(Painter &p, const Color &c, double width) {
  p.set(c);
  cairo_set_line_width(p.cr(), width);
  cairo_set_line_cap(p.cr(), CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(p.cr(), CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(p.cr());
}
}  // namespace

void wifi(Painter &p, double x, double y, double s, int strength, const Color &c, bool off) {
  cairo_t *cr = p.cr();
  double cx = x + s / 2, cy = y + s * 0.82, lw = s * 0.11;
  Color dim = c.alpha(c.a * 0.3);
  for (int i = 1; i <= 3; ++i) {
    double r = s * (0.18 + 0.2 * i);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, -M_PI * 0.75, -M_PI * 0.25);
    stroke(p, (!off && strength > i) ? c : dim, lw);
  }
  p.circle(cx, cy - s * 0.02, s * 0.08, (!off && strength > 0) ? c : dim);
  if (off) {
    cairo_move_to(cr, x + s * 0.15, y + s * 0.15);
    cairo_line_to(cr, x + s * 0.85, y + s * 0.85);
    stroke(p, c, lw);
  }
}

void ethernet(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  double lw = s * 0.1;
  p.rounded_rect(x + s * 0.3, y + s * 0.12, s * 0.4, s * 0.3, s * 0.05);
  stroke(p, c, lw);
  cairo_move_to(cr, x + s * 0.5, y + s * 0.42);
  cairo_line_to(cr, x + s * 0.5, y + s * 0.62);
  cairo_move_to(cr, x + s * 0.18, y + s * 0.88);
  cairo_line_to(cr, x + s * 0.18, y + s * 0.62);
  cairo_line_to(cr, x + s * 0.82, y + s * 0.62);
  cairo_line_to(cr, x + s * 0.82, y + s * 0.88);
  stroke(p, c, lw);
}

void speaker(Painter &p, double x, double y, double s, int level, bool muted, const Color &c) {
  cairo_t *cr = p.cr();
  p.set(c);
  cairo_move_to(cr, x + s * 0.12, y + s * 0.38);
  cairo_line_to(cr, x + s * 0.3, y + s * 0.38);
  cairo_line_to(cr, x + s * 0.52, y + s * 0.16);
  cairo_line_to(cr, x + s * 0.52, y + s * 0.84);
  cairo_line_to(cr, x + s * 0.3, y + s * 0.62);
  cairo_line_to(cr, x + s * 0.12, y + s * 0.62);
  cairo_close_path(cr);
  cairo_fill(cr);
  double lw = s * 0.09;
  if (muted) {
    cairo_move_to(cr, x + s * 0.64, y + s * 0.37);
    cairo_line_to(cr, x + s * 0.9, y + s * 0.63);
    cairo_move_to(cr, x + s * 0.9, y + s * 0.37);
    cairo_line_to(cr, x + s * 0.64, y + s * 0.63);
    stroke(p, c, lw);
    return;
  }
  Color dim = c.alpha(c.a * 0.3);
  for (int i = 1; i <= 3; ++i) {
    double r = s * (0.1 + 0.13 * i);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + s * 0.5, y + s * 0.5, r, -M_PI / 4, M_PI / 4);
    stroke(p, level >= i ? c : dim, lw);
  }
}

void battery(Painter &p, double x, double y, double s, int percent, bool charging, const Color &c,
             const Color &level_color) {
  double bw = s * 0.82, bh = s * 0.46, bx = x + s * 0.04, by = y + (s - bh) / 2;
  double lw = std::fmax(1.2, s * 0.07);
  p.stroke_rounded(bx, by, bw, bh, s * 0.1, c, lw);
  p.fill_rounded(bx + bw + s * 0.02, by + bh * 0.3, s * 0.08, bh * 0.4, s * 0.03, c);
  double inner = lw + s * 0.04;
  double fill_w = (bw - 2 * inner) * std::fmax(0, std::fmin(100, percent)) / 100.0;
  if (fill_w > 0.5) p.fill_rounded(bx + inner, by + inner, fill_w, bh - 2 * inner, s * 0.04, level_color);
  if (charging) {
    cairo_t *cr = p.cr();
    double cx = bx + bw / 2, cy = by + bh / 2;
    cairo_move_to(cr, cx + s * 0.05, cy - bh * 0.42);
    cairo_line_to(cr, cx - s * 0.12, cy + s * 0.03);
    cairo_line_to(cr, cx + s * 0.01, cy + s * 0.03);
    cairo_line_to(cr, cx - s * 0.05, cy + bh * 0.42);
    cairo_line_to(cr, cx + s * 0.12, cy - s * 0.03);
    cairo_line_to(cr, cx - s * 0.01, cy - s * 0.03);
    cairo_close_path(cr);
    p.set(c);
    cairo_fill_preserve(cr);
    Color outline = level_color;
    outline.a = 1;
    p.set(outline);
    cairo_set_line_width(cr, s * 0.04);
    cairo_stroke(cr);
  }
}

void sun(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  double cx = x + s / 2, cy = y + s / 2;
  p.circle(cx, cy, s * 0.18, c);
  for (int i = 0; i < 8; ++i) {
    double a = i * M_PI / 4;
    cairo_move_to(cr, cx + std::cos(a) * s * 0.3, cy + std::sin(a) * s * 0.3);
    cairo_line_to(cr, cx + std::cos(a) * s * 0.42, cy + std::sin(a) * s * 0.42);
  }
  stroke(p, c, s * 0.08);
}

void bell(Painter &p, double x, double y, double s, bool off, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_move_to(cr, x + s * 0.2, y + s * 0.72);
  cairo_curve_to(cr, x + s * 0.28, y + s * 0.6, x + s * 0.26, y + s * 0.5, x + s * 0.28, y + s * 0.4);
  cairo_curve_to(cr, x + s * 0.3, y + s * 0.2, x + s * 0.7, y + s * 0.2, x + s * 0.72, y + s * 0.4);
  cairo_curve_to(cr, x + s * 0.74, y + s * 0.5, x + s * 0.72, y + s * 0.6, x + s * 0.8, y + s * 0.72);
  cairo_close_path(cr);
  stroke(p, c, s * 0.08);
  cairo_move_to(cr, x + s * 0.42, y + s * 0.84);
  cairo_line_to(cr, x + s * 0.58, y + s * 0.84);
  stroke(p, c, s * 0.08);
  if (off) {
    cairo_move_to(cr, x + s * 0.15, y + s * 0.15);
    cairo_line_to(cr, x + s * 0.85, y + s * 0.85);
    stroke(p, c, s * 0.08);
  }
}

void gear(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  double cx = x + s / 2, cy = y + s / 2;
  for (int i = 0; i < 8; ++i) {
    double a = i * M_PI / 4;
    cairo_move_to(cr, cx + std::cos(a) * s * 0.28, cy + std::sin(a) * s * 0.28);
    cairo_line_to(cr, cx + std::cos(a) * s * 0.42, cy + std::sin(a) * s * 0.42);
  }
  stroke(p, c, s * 0.13);
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx, cy, s * 0.26, 0, 2 * M_PI);
  stroke(p, c, s * 0.1);
}

void lock(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  p.fill_rounded(x + s * 0.2, y + s * 0.44, s * 0.6, s * 0.44, s * 0.08, c);
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + s * 0.5, y + s * 0.36, s * 0.18, M_PI, 0);
  cairo_line_to(cr, x + s * 0.68, y + s * 0.46);
  cairo_move_to(cr, x + s * 0.32, y + s * 0.36);
  cairo_line_to(cr, x + s * 0.32, y + s * 0.46);
  stroke(p, c, s * 0.09);
}

void power(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + s / 2, y + s * 0.54, s * 0.32, -M_PI / 2 + 0.6, 3 * M_PI / 2 - 0.6);
  stroke(p, c, s * 0.1);
  cairo_move_to(cr, x + s / 2, y + s * 0.12);
  cairo_line_to(cr, x + s / 2, y + s * 0.5);
  stroke(p, c, s * 0.1);
}

void chevron(Painter &p, double x, double y, double s, bool right, const Color &c) {
  cairo_t *cr = p.cr();
  double d = right ? 1 : -1, cx = x + s / 2;
  cairo_move_to(cr, cx - d * s * 0.12, y + s * 0.25);
  cairo_line_to(cr, cx + d * s * 0.13, y + s * 0.5);
  cairo_line_to(cr, cx - d * s * 0.12, y + s * 0.75);
  stroke(p, c, s * 0.1);
}

void check(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_move_to(cr, x + s * 0.2, y + s * 0.52);
  cairo_line_to(cr, x + s * 0.42, y + s * 0.72);
  cairo_line_to(cr, x + s * 0.8, y + s * 0.3);
  stroke(p, c, s * 0.11);
}

}  // namespace glyph
}  // namespace sde
