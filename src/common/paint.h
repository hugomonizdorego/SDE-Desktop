// SPDX-License-Identifier: GPL-3.0-or-later
// Anti-aliased drawing for FLTK windows using cairo on the X11 drawable that
// FLTK is currently drawing into. Text is drawn by FLTK (Xft) afterwards.
#pragma once

#include <X11/Xlib.h>
#include <cairo.h>

#include <string>

#include "theme.h"

namespace sde {

class Painter {
 public:
  // Must be created inside a window's draw(); |w|,|h| are the window size.
  Painter(int w, int h);
  ~Painter();
  Painter(const Painter &) = delete;
  Painter &operator=(const Painter &) = delete;

  cairo_t *cr() const { return cr_; }

  void set(const Color &c);
  void rounded_rect(double x, double y, double w, double h, double r);
  void fill_rect(double x, double y, double w, double h, const Color &c);
  void fill_rounded(double x, double y, double w, double h, double r, const Color &c);
  void stroke_rounded(double x, double y, double w, double h, double r, const Color &c,
                      double width = 1.0);
  // Rounded rectangle filled with a linear gradient from |from| to |to|,
  // top to bottom, or top-left to bottom-right when |diagonal|.
  void fill_gradient(double x, double y, double w, double h, double r, const Color &from,
                     const Color &to, bool diagonal = false);
  // SDE Liquid Glass: a faint light fading out over the upper part of a surface.
  void glass_sheen(double x, double y, double w, double h, double r, bool dark);
  // The edge of a surface (0.2.0 Alpha 2): a 1 px |border| on the outline
  // and, inside it, a light rim that is brightest at the top (|rim|: its
  // strength, 0 for none). |corners| as in Painter::rounded_corners():
  // 1 top-left, 2 top-right, 4 bottom-right, 8 bottom-left.
  void surface_edge(double x, double y, double w, double h, double r, const Color &border, double rim,
                    bool dark, int corners = 15);
  // SDE Liquid Glass drawn by SDE itself, when the compositor's glass shader
  // does not run (lite profile, XRender): over a surface already filled, a
  // light from the top that fades out and the light rim of surface_edge().
  void glass_surface(double x, double y, double w, double h, double r, const Color &border, bool dark,
                     int corners = 15);
  // A rounded rectangle path with only |corners| rounded (see surface_edge()).
  void rounded_corners(double x, double y, double w, double h, double r, int corners);
  // Soft drop shadow made of a few translucent rounded rectangles.
  void shadow(double x, double y, double w, double h, double r, const Color &c, int spread = 8);
  // Draws an image scaled into a |size| square. Image surfaces are uploaded
  // to the X server once and the server copy is reused while |s| lives, so
  // |s| must not be modified after it was first painted (icons, wallpapers).
  void image(cairo_surface_t *s, double x, double y, double size, double alpha = 1.0);
  // Paints |s| unscaled at |x|,|y| (e.g. a pre-scaled wallpaper), with the
  // same server-side cache as image().
  void surface(cairo_surface_t *s, double x, double y, double alpha = 1.0);
  // Server-side copy of an immutable image surface, cached on |s|.
  cairo_surface_t *server_copy(cairo_surface_t *s);
  void circle(double cx, double cy, double radius, const Color &c);
  void line(double x1, double y1, double x2, double y2, const Color &c, double width = 1.0);

  // Flushes cairo so that FLTK/Xft text can be drawn on top.
  void flush();

  // Used by BufferedWindow: while a back buffer is being drawn, every
  // Painter on it shares one cairo surface instead of creating its own.
  static void begin_target(Drawable d, int w, int h);
  static void end_target();
  static void forget_target(Drawable d);  // the pixmap is about to be freed

 private:
  cairo_surface_t *surface_ = nullptr;
  bool shared_ = false;
  cairo_t *cr_ = nullptr;
};

// FLTK text helpers. |bold| selects FL_HELVETICA_BOLD.
void text_font(int size, bool bold = false);
int text_width(const std::string &s);
// Draws |s| left aligned and vertically centered in the box, shortening it
// with an ellipsis when it does not fit.
void draw_text(const std::string &s, int x, int y, int w, int h, const Color &c,
               int align = 0 /* 0 left, 1 center, 2 right */);
std::string ellipsize(const std::string &s, int max_width);

}  // namespace sde
