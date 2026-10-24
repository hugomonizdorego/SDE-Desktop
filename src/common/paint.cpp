// SPDX-License-Identifier: GPL-3.0-or-later
// BufferedWindow::flush() takes FLTK's damage region from Fl_X.
#define FL_INTERNALS 1
#include "paint.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <cairo-xlib.h>

#include <cmath>

#include "buffered_window.h"
#include "menu.h"
#include "perf.h"

namespace sde {

// Every program that draws with Painter also gets the SDE look for FLTK's
// menus, tooltips, scroll bars and stock dialogs (menu.cpp).
[[maybe_unused]] static const bool g_ui_style_installed = (install_ui_style(), true);

namespace {
struct Target {
  Drawable drawable = 0;
  int w = 0, h = 0;
  bool active = false;
  cairo_surface_t *surface = nullptr;
};
Target g_target;
cairo_user_data_key_t g_server_copy_key;

void drop_target_surface() {
  if (g_target.surface) cairo_surface_destroy(g_target.surface);
  g_target.surface = nullptr;
  g_target.drawable = 0;
}
}  // namespace

void Painter::begin_target(Drawable d, int w, int h) {
  if (g_target.surface && (g_target.drawable != d || g_target.w != w || g_target.h != h))
    drop_target_surface();
  g_target.drawable = d;
  g_target.w = w;
  g_target.h = h;
  g_target.active = true;
}

void Painter::end_target() {
  g_target.active = false;
  // FLTK/Xft draw on the pixmap between frames; cairo must not trust any
  // state it cached about the drawable.
  if (g_target.surface) cairo_surface_mark_dirty(g_target.surface);
}

void Painter::forget_target(Drawable d) {
  if (g_target.drawable == d) drop_target_surface();
}

Painter::Painter(int w, int h) {
  if (perf::enabled()) ++perf::counters().painter;
  if (g_target.active && fl_window == g_target.drawable && w == g_target.w && h == g_target.h) {
    if (!g_target.surface)
      g_target.surface = cairo_xlib_surface_create(fl_display, fl_window, fl_visual->visual, w, h);
    else
      cairo_surface_mark_dirty(g_target.surface);  // text drawn since the last Painter
    surface_ = g_target.surface;
    shared_ = true;
  } else {
    surface_ = cairo_xlib_surface_create(fl_display, fl_window, fl_visual->visual, w, h);
  }
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
  if (!shared_) cairo_surface_destroy(surface_);
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

void Painter::fill_gradient(double x, double y, double w, double h, double r, const Color &from,
                            const Color &to, bool diagonal) {
  cairo_pattern_t *g =
      cairo_pattern_create_linear(x, y, diagonal ? x + w : x, y + h);
  cairo_pattern_add_color_stop_rgba(g, 0, from.r, from.g, from.b, from.a);
  cairo_pattern_add_color_stop_rgba(g, 1, to.r, to.g, to.b, to.a);
  rounded_rect(x, y, w, h, r);
  cairo_set_source(cr_, g);
  cairo_fill(cr_);
  cairo_pattern_destroy(g);
}

void Painter::glass_sheen(double x, double y, double w, double h, double r, bool dark) {
  // Since 0.1.0~alpha51 a faint, even light from the top that fades out
  // smoothly: no glossy band, no hard edge in the middle.
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
  double top = dark ? 0.05 : 0.18;
  cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, top);
  cairo_pattern_add_color_stop_rgba(g, 0.7, 1, 1, 1, 0);
  cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0);
  rounded_rect(x, y, w, h, r);
  cairo_set_source(cr_, g);
  cairo_fill(cr_);
  cairo_pattern_destroy(g);
}

void Painter::rounded_corners(double x, double y, double w, double h, double r, int corners) {
  r = std::fmin(r, std::fmin(w, h) / 2);
  auto rad = [&](int bit) { return (corners & bit) ? r : 0.0; };
  cairo_new_sub_path(cr_);
  cairo_arc(cr_, x + w - rad(2), y + rad(2), rad(2), -M_PI / 2, 0);
  cairo_arc(cr_, x + w - rad(4), y + h - rad(4), rad(4), 0, M_PI / 2);
  cairo_arc(cr_, x + rad(8), y + h - rad(8), rad(8), M_PI / 2, M_PI);
  cairo_arc(cr_, x + rad(1), y + rad(1), rad(1), M_PI, 3 * M_PI / 2);
  cairo_close_path(cr_);
}

void Painter::surface_edge(double x, double y, double w, double h, double r, const Color &border, double rim,
                           bool dark, int corners) {
  if (rim > 0) {
    // The rim: a hairline just inside the border, lit from the top.
    cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
    double top = rim * (dark ? 0.16 : 0.85), bottom = rim * (dark ? 0.04 : 0.25);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, top);
    cairo_pattern_add_color_stop_rgba(g, std::fmin(0.5, Sd(36) / std::fmax(h, 1.0)), 1, 1, 1, (top + bottom) / 2);
    cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, bottom);
    cairo_set_line_width(cr_, 1.0);
    rounded_corners(x + 1.5, y + 1.5, w - 3, h - 3, std::fmax(0.0, r - 1.0), corners);
    cairo_set_source(cr_, g);
    cairo_stroke(cr_);
    cairo_pattern_destroy(g);
  }
  set(border);
  cairo_set_line_width(cr_, 1.0);
  rounded_corners(x + 0.5, y + 0.5, w - 1, h - 1, r, corners);
  cairo_stroke(cr_);
}

void Painter::glass_surface(double x, double y, double w, double h, double r, const Color &border, bool dark,
                            int corners) {
  // Light from the top that fades out over the first part of the surface,
  // a little stronger at its very top (where real glass catches the light).
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
  double reach = std::fmin(1.0, Sd(120) / std::fmax(h, 1.0));
  cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, dark ? 0.07 : 0.40);
  cairo_pattern_add_color_stop_rgba(g, reach * 0.25, 1, 1, 1, dark ? 0.035 : 0.20);
  cairo_pattern_add_color_stop_rgba(g, reach, 1, 1, 1, 0);
  cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0);
  rounded_corners(x, y, w, h, r, corners);
  cairo_set_source(cr_, g);
  cairo_fill(cr_);
  cairo_pattern_destroy(g);
  surface_edge(x, y, w, h, r, border, 1.0, dark, corners);
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

cairo_surface_t *Painter::server_copy(cairo_surface_t *s) {
  if (!s || cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE) return s;
  if (cairo_surface_get_type(surface_) != CAIRO_SURFACE_TYPE_XLIB) return s;
  auto *cached = static_cast<cairo_surface_t *>(cairo_surface_get_user_data(s, &g_server_copy_key));
  if (cached && cairo_surface_get_device(cached) == cairo_surface_get_device(surface_)) {
    if (perf::enabled()) ++perf::counters().cached;
    return cached;
  }
  int iw = cairo_image_surface_get_width(s), ih = cairo_image_surface_get_height(s);
  if (iw <= 0 || ih <= 0) return s;
  cairo_content_t content =
      cairo_image_surface_get_format(s) == CAIRO_FORMAT_RGB24 ? CAIRO_CONTENT_COLOR
                                                              : CAIRO_CONTENT_COLOR_ALPHA;
  cairo_surface_t *copy = cairo_surface_create_similar(surface_, content, iw, ih);
  if (cairo_surface_status(copy) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(copy);
    return s;
  }
  cairo_t *c = cairo_create(copy);
  cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(c, s, 0, 0);
  cairo_paint(c);
  cairo_destroy(c);
  cairo_surface_flush(copy);
  // Freed together with the image surface.
  if (cairo_surface_set_user_data(s, &g_server_copy_key, copy,
                                  reinterpret_cast<cairo_destroy_func_t>(cairo_surface_destroy)) !=
      CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(copy);
    return s;
  }
  if (perf::enabled()) ++perf::counters().upload;
  return copy;
}

void Painter::image(cairo_surface_t *s, double x, double y, double size, double alpha) {
  if (!s) return;
  int iw = cairo_image_surface_get_width(s), ih = cairo_image_surface_get_height(s);
  if (iw <= 0 || ih <= 0) return;
  cairo_save(cr_);
  double scale = size / std::fmax(iw, ih);
  double tx = x + (size - iw * scale) / 2, ty = y + (size - ih * scale) / 2;
  cairo_surface_t *src = server_copy(s);
  if (std::fabs(scale - 1.0) < 1e-6) {
    // Unscaled: a plain composite, no filtering.
    cairo_set_source_surface(cr_, src, std::round(tx), std::round(ty));
  } else {
    cairo_translate(cr_, tx, ty);
    cairo_scale(cr_, scale, scale);
    cairo_set_source_surface(cr_, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_GOOD);
  }
  cairo_paint_with_alpha(cr_, alpha);
  cairo_restore(cr_);
}

void Painter::surface(cairo_surface_t *s, double x, double y, double alpha) {
  if (!s) return;
  cairo_save(cr_);
  cairo_set_source_surface(cr_, server_copy(s), x, y);
  if (alpha >= 1.0)
    cairo_paint(cr_);
  else
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

// ------------------------------------------------------------ BufferedWindow

void BufferedWindow::flush() {
  if (!shown()) return;
  make_current();
  // Take FLTK's damage region (expose and child/rect damage) ourselves.
  Fl_X *xi = Fl_X::i(this);
  Fl_Region region = xi ? xi->region : 0;
  if (xi) xi->region = 0;
  bool fresh = false;
  if (!buffer_ || buffer_w_ != w() || buffer_h_ != h()) {
    free_buffer();
    buffer_ = fl_create_offscreen(w(), h());
    buffer_w_ = w();
    buffer_h_ = h();
    fresh = true;
  }
  bool stats = perf::enabled();
  double t0 = stats ? perf::now_ms() : 0;
  if (stats) ++perf::counters().flush;

  int cx = 0, cy = 0, cw = w(), ch = h();
  bool clipped = false;
  if (region && !fresh) {
    XRectangle box;
    XClipBox(region, &box);
    cx = box.x;
    cy = box.y;
    cw = box.width;
    ch = box.height;
    clipped = true;
  }
  if (region) XDestroyRegion(region);

  uchar d = damage();
  if (fresh || (d & ~FL_DAMAGE_EXPOSE)) {
    if (clipped && partial_enabled()) {
      // Grow child damage a little: focus rings, outlines and shadows are
      // drawn just outside the widget box.
      int m = std::max(4, int(std::lround(4 * ui_scale())));
      int x0 = std::max(0, cx - m), y0 = std::max(0, cy - m);
      int x1 = std::min(w(), cx + cw + m), y1 = std::min(h(), cy + ch + m);
      cx = x0;
      cy = y0;
      cw = x1 - x0;
      ch = y1 - y0;
    } else {
      cx = cy = 0;
      cw = w();
      ch = h();
      clipped = false;
    }
    if (cw > 0 && ch > 0) {
      if (stats) {
        ++(clipped ? perf::counters().partial : perf::counters().full);
        perf::counters().px += (unsigned long long)cw * ch;
      }
      damage(FL_DAMAGE_ALL);
      fl_begin_offscreen(buffer_);
      Painter::begin_target(buffer_, w(), h());
      if (clipped)
        fl_push_clip(cx, cy, cw, ch);
      else
        fl_push_no_clip();
      draw();
      fl_pop_clip();
      Painter::end_target();
      fl_end_offscreen();
    }
  } else if (stats) {
    ++perf::counters().copy;
  }
  if (stats) perf::counters().draw_ms += perf::now_ms() - t0;
  if (cw > 0 && ch > 0) fl_copy_offscreen(cx, cy, cw, ch, buffer_, cx, cy);
}


}  // namespace sde
