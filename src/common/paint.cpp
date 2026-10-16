// SPDX-License-Identifier: GPL-3.0-or-later
#include "paint.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <cairo-xlib.h>

#include <cmath>

namespace sde {

Painter::Painter(int w, int h) {
  surface_ = cairo_xlib_surface_create(fl_display, fl_window, fl_visual->visual, w, h);
  cr_ = cairo_create(surface_);
  // Respect FLTK's clipping (e.g. inside Fl_Scroll), which cairo does not see.
  if (Fl_Region r = fl_clip_region()) {
    XRectangle box;
    XClipBox(r, &box);
    cairo_rectangle(cr_, box.x, box.y, box.width, box.height);
    cairo_clip(cr_);
  }
}

Painter::~Painter() {
  flush();
  cairo_destroy(cr_);
  cairo_surface_destroy(surface_);
}

void Painter::flush() { cairo_surface_flush(surface_); }

void Painter::set(const Color &c) { cairo_set_source_rgba(cr_, c.r, c.g, c.b, c.a); }

void Painter::rounded_rect(double x, double y, double w, double h, double r) {
  r = std::fmin(r, std::fmin(w, h) / 2);
  cairo_new_sub_path(cr_);
  cairo_arc(cr_, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr_, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr_, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr_, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr_);
}

void Painter::fill_rect(double x, double y, double w, double h, const Color &c) {
  set(c);
  cairo_rectangle(cr_, x, y, w, h);
  cairo_fill(cr_);
}

void Painter::fill_rounded(double x, double y, double w, double h, double r, const Color &c) {
  set(c);
  rounded_rect(x, y, w, h, r);
  cairo_fill(cr_);
}

void Painter::stroke_rounded(double x, double y, double w, double h, double r, const Color &c,
                             double width) {
  set(c);
  cairo_set_line_width(cr_, width);
  rounded_rect(x + width / 2, y + width / 2, w - width, h - width, r);
  cairo_stroke(cr_);
}

void Painter::shadow(double x, double y, double w, double h, double r, const Color &c,
                     int spread) {
  for (int i = spread; i > 0; --i) {
    double t = double(i) / spread;
    fill_rounded(x - i, y - i + spread / 3.0, w + 2 * i, h + 2 * i, r + i,
                 c.alpha(c.a * (1 - t) * (1 - t) / spread * 2.2));
  }
}

void Painter::image(cairo_surface_t *s, double x, double y, double size, double alpha) {
  if (!s) return;
  int iw = cairo_image_surface_get_width(s), ih = cairo_image_surface_get_height(s);
  if (iw <= 0 || ih <= 0) return;
  cairo_save(cr_);
  double scale = size / std::fmax(iw, ih);
  cairo_translate(cr_, x + (size - iw * scale) / 2, y + (size - ih * scale) / 2);
  cairo_scale(cr_, scale, scale);
  cairo_set_source_surface(cr_, s, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_GOOD);
  cairo_paint_with_alpha(cr_, alpha);
  cairo_restore(cr_);
}

void Painter::circle(double cx, double cy, double radius, const Color &c) {
  set(c);
  cairo_new_sub_path(cr_);
  cairo_arc(cr_, cx, cy, radius, 0, 2 * M_PI);
  cairo_fill(cr_);
}

void Painter::line(double x1, double y1, double x2, double y2, const Color &c, double width) {
  set(c);
  cairo_set_line_width(cr_, width);
  cairo_move_to(cr_, x1, y1);
  cairo_line_to(cr_, x2, y2);
  cairo_stroke(cr_);
}

void text_font(int size, bool bold) { fl_font(bold ? FL_HELVETICA_BOLD : FL_HELVETICA, size); }

int text_width(const std::string &s) { return int(std::ceil(fl_width(s.c_str()))); }

std::string ellipsize(const std::string &s, int max_width) {
  if (max_width <= 0) return "";
  if (text_width(s) <= max_width) return s;
  static const std::string dots = "\xE2\x80\xA6";  // U+2026
  std::string cut = s;
  while (!cut.empty()) {
    // Remove one UTF-8 code point from the end.
    size_t n = cut.size() - 1;
    while (n > 0 && (uchar(cut[n]) & 0xC0) == 0x80) --n;
    cut.erase(n);
    if (text_width(cut + dots) <= max_width) return cut + dots;
  }
  return "";
}

void draw_text(const std::string &s, int x, int y, int w, int h, const Color &c, int align) {
  std::string t = ellipsize(s, w);
  fl_color(c.fl());
  int tw = text_width(t);
  int tx = align == 1 ? x + (w - tw) / 2 : align == 2 ? x + w - tw : x;
  int ty = y + (h + fl_height()) / 2 - fl_descent();
  fl_draw(t.c_str(), tx, ty);
}

}  // namespace sde
