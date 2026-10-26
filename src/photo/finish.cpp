// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo's finishing touches (the plugins of the Effects tab): background
// blur, watermark and frame, and Auto Enhance (Adjust tab). They work on a
// rendered picture, so the same code makes the live preview (on a smaller
// copy) and the saved file.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "edit.h"
#include "util.h"

namespace photo {

namespace {

double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

void set_argb(cairo_t *cr, uint32_t c, double alpha = 1) {
  cairo_set_source_rgba(cr, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0,
                        ((c >> 24) & 0xFF) / 255.0 * alpha);
}

// Perceived lightness of a colour (0 dark .. 1 light).
double lightness(uint32_t c) {
  return (0.2126 * ((c >> 16) & 0xFF) + 0.7152 * ((c >> 8) & 0xFF) + 0.0722 * (c & 0xFF)) / 255.0;
}

void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
  r = std::max(0.0, std::min(r, std::min(w, h) / 2));
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

}  // namespace

bool Finish::identity() const {
  bool blur_off = focus == FocusShape::Off || blur <= 0;
  return blur_off && sde::trim(watermark).empty() && frame == FrameStyle::None;
}

bool Finish::operator==(const Finish &o) const {
  return focus == o.focus && focus_x == o.focus_x && focus_y == o.focus_y && focus_size == o.focus_size &&
         blur == o.blur && watermark == o.watermark && watermark_corner == o.watermark_corner &&
         watermark_size == o.watermark_size && watermark_opacity == o.watermark_opacity && frame == o.frame &&
         frame_width == o.frame_width && frame_color == o.frame_color;
}

void frame_insets(const Finish &f, double cw, double ch, double &l, double &t, double &r, double &b) {
  l = t = r = b = 0;
  if (f.frame == FrameStyle::None || cw <= 0 || ch <= 0) return;
  // The width is a share of the shorter side, so that a frame looks the
  // same on a portrait and a landscape photo.
  double m = std::min(cw, ch) * clamp(f.frame_width, 0.5, 30) / 100;
  double L = m, T = m, R = m, B = m;
  switch (f.frame) {
    case FrameStyle::Mat: L = T = R = B = m * 2.2; break;
    case FrameStyle::Print: B = m * 3.6; break;
    case FrameStyle::Shadow: L = T = R = B = m * 1.6; break;
    case FrameStyle::None:
    case FrameStyle::Border:
    case FrameStyle::Rounded: break;
  }
  l = L / cw;
  r = R / cw;
  t = T / ch;
  b = B / ch;
}

cairo_surface_t *add_frame(cairo_surface_t *s, const Finish &f) {
  if (!s || f.frame == FrameStyle::None) return cairo_surface_reference(s);
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  double l, t, r, b;
  frame_insets(f, w, h, l, t, r, b);
  int L = int(std::lround(l * w)), R = int(std::lround(r * w)), T = int(std::lround(t * h)),
      B = int(std::lround(b * h));
  int fw = w + L + R, fh = h + T + B;
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, fw, fh);
  cairo_t *cr = cairo_create(out);
  set_argb(cr, f.frame_color | 0xFF000000u);
  cairo_paint(cr);
  double shorter = std::min(w, h), m = shorter * clamp(f.frame_width, 0.5, 30) / 100;
  bool light = lightness(f.frame_color) > 0.5;
  switch (f.frame) {
    case FrameStyle::Mat: {
      // A wide mat with a fine bevel line around the photo.
      double gap = std::max(1.0, m * 0.35), line = std::max(1.0, shorter / 500);
      cairo_set_line_width(cr, line);
      cairo_set_source_rgba(cr, light ? 0 : 1, light ? 0 : 1, light ? 0 : 1, 0.28);
      cairo_rectangle(cr, L - gap + line / 2, T - gap + line / 2, w + 2 * gap - line, h + 2 * gap - line);
      cairo_stroke(cr);
      break;
    }
    case FrameStyle::Shadow: {
      // A soft shadow below the photo: a few widening, fading outlines.
      double spread = std::max(2.0, m * 0.55), drop = m * 0.18;
      for (int i = 6; i >= 1; --i) {
        double k = i / 6.0;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.055 * (1.2 - k));
        rounded_rect(cr, L - spread * k, T - spread * k + drop, w + 2 * spread * k, h + 2 * spread * k,
                     spread * k);
        cairo_fill(cr);
      }
      break;
    }
    case FrameStyle::Print: {
      // A thin edge between the photo and the paper.
      cairo_set_source_rgba(cr, 0, 0, 0, light ? 0.08 : 0.25);
      cairo_rectangle(cr, L - 1, T - 1, w + 2, h + 2);
      cairo_fill(cr);
      break;
    }
    case FrameStyle::None:
    case FrameStyle::Border:
    case FrameStyle::Rounded: break;
  }
  if (f.frame == FrameStyle::Rounded) {
    rounded_rect(cr, L, T, w, h, std::max(2.0, shorter * 0.045));
    cairo_clip(cr);
  }
  cairo_set_source_surface(cr, s, L, T);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(out);
  return out;
}

void draw_watermark(cairo_surface_t *s, const Finish &f) {
  std::string text = sde::trim(f.watermark);
  if (!s || text.empty()) return;
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  double size = std::max(9.0, std::min(w, h) * clamp(f.watermark_size, 1, 25) / 100);
  double alpha = clamp(f.watermark_opacity, 5, 100) / 100;
  cairo_t *cr = cairo_create(s);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, size);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, text.c_str(), &ext);
  // Too wide: smaller, so that it fits with its margins.
  double margin = size * 0.9;
  if (ext.width > w - 2 * margin && ext.width > 0) {
    size *= std::max(0.2, (w - 2 * margin) / ext.width);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, text.c_str(), &ext);
    margin = size * 0.9;
  }
  int corner = f.watermark_corner;
  double x = corner == 1 || corner == 3 ? w - margin - ext.width : corner == 4 ? (w - ext.width) / 2 : margin;
  double y = corner == 2 || corner == 3 ? h - margin - ext.height : corner == 4 ? (h - ext.height) / 2 : margin;
  x -= ext.x_bearing;
  y -= ext.y_bearing;
  // A soft dark shadow keeps white text readable on light parts.
  double off = std::max(1.0, size * 0.05);
  for (int i = 0; i < 2; ++i) {
    cairo_move_to(cr, x + off * (i + 1) * 0.6, y + off * (i + 1) * 0.6);
    cairo_set_source_rgba(cr, 0, 0, 0, alpha * 0.22);
    cairo_show_text(cr, text.c_str());
  }
  cairo_move_to(cr, x, y);
  cairo_set_source_rgba(cr, 1, 1, 1, alpha);
  cairo_show_text(cr, text.c_str());
  cairo_destroy(cr);
  cairo_surface_flush(s);
}

void apply_focus_blur(cairo_surface_t *s, const Finish &f) {
  if (!s || f.focus == FocusShape::Off || f.blur <= 0) return;
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  if (w < 4 || h < 4) return;
  double shorter = std::min(w, h);
  // The blur is soft anyway: blur a small copy (fast and light on memory
  // for big photos) and lay it over the photo, enlarged.
  double k = std::min(1.0, 420.0 / shorter);
  int sw = std::max(2, int(std::lround(w * k))), sh = std::max(2, int(std::lround(h * k)));
  cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
  cairo_t *sc = cairo_create(small);
  cairo_scale(sc, double(sw) / w, double(sh) / h);
  cairo_set_source_surface(sc, s, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(sc), CAIRO_FILTER_GOOD);
  cairo_paint(sc);
  cairo_destroy(sc);
  int radius = std::max(1, int(std::lround(clamp(f.blur, 0, 100) / 100 * 0.028 * std::min(sw, sh))));
  blur_region(small, 0, 0, sw, sh, radius);

  cairo_t *cr = cairo_create(s);
  cairo_pattern_t *src = cairo_pattern_create_for_surface(small);
  cairo_matrix_t mat;
  cairo_matrix_init_scale(&mat, double(sw) / w, double(sh) / h);
  cairo_pattern_set_matrix(src, &mat);
  cairo_pattern_set_filter(src, CAIRO_FILTER_GOOD);
  cairo_pattern_set_extend(src, CAIRO_EXTEND_PAD);
  cairo_set_source(cr, src);
  // Where the blur shows: nowhere in the sharp part, fully beyond a soft
  // transition around it.
  double fx = clamp(f.focus_x, 0, 1) * w, fy = clamp(f.focus_y, 0, 1) * h;
  double size = clamp(f.focus_size, 5, 100) / 100;
  cairo_pattern_t *mask;
  const double ease[][2] = {{0, 0}, {0.25, 0.1}, {0.5, 0.42}, {0.75, 0.8}, {1, 1}};
  if (f.focus == FocusShape::Circle) {
    double r0 = size * shorter / 2, r1 = r0 * 1.9 + shorter * 0.03;
    mask = cairo_pattern_create_radial(fx, fy, r0, fx, fy, r1);
    for (const auto &e : ease) cairo_pattern_add_color_stop_rgba(mask, e[0], 0, 0, 0, e[1]);
  } else {
    double half = size * h / 2, feather = half * 1.2 + h * 0.03, total = 2 * (half + feather);
    mask = cairo_pattern_create_linear(0, fy - half - feather, 0, fy + half + feather);
    double edge = feather / total;
    for (int i = 4; i >= 0; --i) cairo_pattern_add_color_stop_rgba(mask, edge * (1 - ease[i][0]), 0, 0, 0, ease[i][1]);
    for (const auto &e : ease) cairo_pattern_add_color_stop_rgba(mask, 1 - edge * (1 - e[0]), 0, 0, 0, e[1]);
  }
  cairo_pattern_set_extend(mask, CAIRO_EXTEND_PAD);
  cairo_mask(cr, mask);
  cairo_pattern_destroy(mask);
  cairo_pattern_destroy(src);
  cairo_destroy(cr);
  cairo_surface_destroy(small);
  cairo_surface_flush(s);
}

Adjust auto_adjust(cairo_surface_t *s) {
  Adjust a;
  if (!s) return a;
  cairo_surface_flush(s);
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  const unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  if (!data || w < 1 || h < 1) return a;
  // About 40,000 samples are plenty for the statistics.
  int step = std::max(1, int(std::sqrt(double(w) * h / 40000)));
  std::vector<int> hist(256, 0);
  double sum_r = 0, sum_g = 0, sum_b = 0, sum_sat = 0;
  long n = 0;
  for (int y = 0; y < h; y += step) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(data + size_t(y) * stride);
    for (int x = 0; x < w; x += step) {
      uint32_t px = row[x];
      unsigned al = px >> 24;
      if (al < 128) continue;
      double r = ((px >> 16) & 0xFF) * 255.0 / al / 255, g = ((px >> 8) & 0xFF) * 255.0 / al / 255,
             b = (px & 0xFF) * 255.0 / al / 255;
      double y709 = 0.2126 * r + 0.7152 * g + 0.0722 * b;
      hist[size_t(clamp(std::lround(y709 * 255), 0, 255))]++;
      double mx = std::max({r, g, b}), mn = std::min({r, g, b});
      sum_sat += mx > 0 ? (mx - mn) / mx : 0;
      sum_r += r;
      sum_g += g;
      sum_b += b;
      ++n;
    }
  }
  if (n < 16) return a;
  auto percentile = [&](double p) {
    long want = long(p * n), acc = 0;
    for (int i = 0; i < 256; ++i) {
      acc += hist[size_t(i)];
      if (acc > want) return i / 255.0;
    }
    return 1.0;
  };
  double lo = percentile(0.01), mid = percentile(0.5), hi = percentile(0.99);
  // Exposure: the middle tones to about the middle (2 EV = 100).
  if (mid < 0.36) a.exposure = clamp(std::log2(0.46 / std::max(0.02, mid)) * 50, 0, 60);
  else if (mid > 0.68) a.exposure = clamp(std::log2(0.58 / mid) * 50, -40, 0);
  // Contrast: a flat picture uses only part of the range.
  double range = hi - lo;
  if (range < 0.8) a.contrast = clamp((0.86 - range) * 110, 0, 45);
  // Saturation: dull colours a little livelier.
  double sat = sum_sat / n;
  if (sat < 0.3) a.saturation = clamp((0.32 - sat) * 90, 0, 25);
  // Warmth: neutral on average (a blue cast is warmed, an orange one cooled).
  double mr = sum_r / n, mb = sum_b / n, mg = sum_g / n;
  double cast = (mb - mr) / std::max(0.05, (mr + mg + mb) / 3);
  if (cast > 0.06) a.warmth = clamp(cast * 120, 0, 35);
  else if (cast < -0.18) a.warmth = clamp((cast + 0.12) * 110, -25, 0);
  a.exposure = std::round(a.exposure);
  a.contrast = std::round(a.contrast);
  a.saturation = std::round(a.saturation);
  a.warmth = std::round(a.warmth);
  return a;
}

}  // namespace photo
