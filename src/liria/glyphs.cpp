// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "glyphs.h"

#include <cmath>

namespace sde {
namespace liria {

void glyph(Painter &p, const std::string &name, double x, double y, double s, const Color &c, double weight) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, s / 24.0, s / 24.0);
  p.set(c);
  cairo_set_line_width(cr, weight);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_new_path(cr);
  auto M = [&](double a, double b) { cairo_move_to(cr, a, b); };
  auto L = [&](double a, double b) { cairo_line_to(cr, a, b); };
  auto C = [&](double a, double b, double c2, double d, double e, double f) { cairo_curve_to(cr, a, b, c2, d, e, f); };
  auto Z = [&] { cairo_close_path(cr); };
  auto stroke = [&] { cairo_stroke(cr); };
  auto fill = [&] { cairo_fill(cr); };
  auto rrect = [&](double a, double b, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, a + w - r, b + r, r, -M_PI / 2, 0);
    cairo_arc(cr, a + w - r, b + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, a + r, b + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, a + r, b + r, r, M_PI, 3 * M_PI / 2);
    Z();
  };
  auto circle = [&](double cx, double cy, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  };
  auto folder = [&] {
    M(3, 7.5), C(3, 6.1, 3.9, 5, 5.3, 5), L(9.2, 5), L(11.2, 7), L(18.7, 7), C(20.1, 7, 21, 8, 21, 9.3), L(21, 16.7),
        C(21, 18.1, 20.1, 19, 18.7, 19), L(5.3, 19), C(3.9, 19, 3, 18.1, 3, 16.7), Z();
  };
  auto page = [&] {
    M(6, 3), L(14, 3), L(19, 8), L(19, 21), L(6, 21), Z(), stroke();
    M(14, 3), L(14, 8), L(19, 8), stroke();
  };
  auto bin = [&] {
    M(4.5, 6.5), L(19.5, 6.5), stroke();
    M(9.5, 6.5), L(9.5, 4.5), L(14.5, 4.5), L(14.5, 6.5), stroke();
    M(6.5, 6.5), L(7.4, 19.2), C(7.5, 20.2, 8.2, 21, 9.2, 21), L(14.8, 21), C(15.8, 21, 16.5, 20.2, 16.6, 19.2),
        L(17.5, 6.5), stroke();
  };
  const std::string &n = name;
  if (n == "back") {
    M(19, 12), L(5, 12), stroke();
    M(11, 6), L(5, 12), L(11, 18), stroke();
  } else if (n == "forward") {
    M(5, 12), L(19, 12), stroke();
    M(13, 6), L(19, 12), L(13, 18), stroke();
  } else if (n == "up") {
    M(12, 19), L(12, 5), stroke();
    M(6, 11), L(12, 5), L(18, 11), stroke();
  } else if (n == "refresh") {
    cairo_arc(cr, 12, 12, 7.5, -M_PI * 0.35, M_PI * 1.45);
    stroke();
    M(19.5, 4.5), L(18.6, 8.4), L(14.7, 7.6), stroke();
  } else if (n == "search") {
    circle(10.5, 10.5, 6.3), stroke();
    M(15.2, 15.2), L(20, 20), stroke();
  } else if (n == "close" || n == "window-close") {
    M(7, 7), L(17, 17), M(17, 7), L(7, 17), stroke();
  } else if (n == "plus") {
    M(12, 5), L(12, 19), M(5, 12), L(19, 12), stroke();
  } else if (n == "minimize") {
    M(6, 12.5), L(18, 12.5), stroke();
  } else if (n == "maximize") {
    rrect(6, 6, 12, 12, 1.6), stroke();
  } else if (n == "restore") {
    rrect(5.5, 8.5, 10, 10, 1.4), stroke();
    M(8.5, 5.5), L(17, 5.5), C(17.9, 5.5, 18.5, 6.1, 18.5, 7), L(18.5, 15.5), stroke();
  } else if (n == "chevron-down") {
    M(7, 9.5), L(12, 14.5), L(17, 9.5), stroke();
  } else if (n == "chevron-up") {
    M(7, 14.5), L(12, 9.5), L(17, 14.5), stroke();
  } else if (n == "chevron-right") {
    M(10, 7), L(15, 12), L(10, 17), stroke();
  } else if (n == "more") {
    circle(5.5, 12, 1.6), circle(12, 12, 1.6), circle(18.5, 12, 1.6), fill();
  } else if (n == "new") {
    circle(12, 12, 8.5), stroke();
    M(12, 8), L(12, 16), M(8, 12), L(16, 12), stroke();
  } else if (n == "cut") {
    circle(7, 17, 3), circle(17, 17, 3), stroke();
    M(9.2, 14.8), L(17.5, 3.5), M(14.8, 14.8), L(6.5, 3.5), stroke();
  } else if (n == "copy" || n == "copy-path") {
    rrect(8.5, 8.5, 12, 12, 2), stroke();
    M(5.5, 15.5), C(4.4, 15.5, 3.5, 14.6, 3.5, 13.5), L(3.5, 5.5), C(3.5, 4.4, 4.4, 3.5, 5.5, 3.5), L(13.5, 3.5),
        C(14.6, 3.5, 15.5, 4.4, 15.5, 5.5), stroke();
    if (n == "copy-path") M(11.5, 14.5), L(17.5, 14.5), stroke();
  } else if (n == "paste") {
    rrect(5, 5, 14, 16, 2), stroke();
    rrect(8.5, 3, 7, 4, 1.2), stroke();
    M(8.5, 12), L(15.5, 12), M(8.5, 16), L(13, 16), stroke();
  } else if (n == "rename") {
    rrect(3, 7, 18, 10, 2), stroke();
    M(13, 4.5), L(13, 19.5), M(11, 4.5), L(15, 4.5), M(11, 19.5), L(15, 19.5), stroke();
    M(6.5, 12), L(9, 12), stroke();
  } else if (n == "share") {
    circle(17.5, 6, 2.5), circle(6.5, 12, 2.5), circle(17.5, 18, 2.5), stroke();
    M(8.7, 10.8), L(15.3, 7.2), M(8.7, 13.2), L(15.3, 16.8), stroke();
  } else if (n == "mail") {
    rrect(3, 5.5, 18, 13, 2), stroke();
    M(3.8, 7), L(12, 13), L(20.2, 7), stroke();
  } else if (n == "delete" || n == "trash" || n == "empty-trash") {
    bin();
    if (n == "delete") M(10.5, 10), L(10.8, 17.5), M(13.5, 10), L(13.2, 17.5), stroke();
  } else if (n == "sort") {
    M(7, 4), L(7, 20), stroke();
    M(4, 16.5), L(7, 20), L(10, 16.5), stroke();
    M(13, 6), L(20, 6), M(13, 11), L(18, 11), M(13, 16), L(16, 16), stroke();
  } else if (n == "view" || n == "grid") {
    rrect(4, 4, 7, 7, 1.6), rrect(13, 4, 7, 7, 1.6), rrect(4, 13, 7, 7, 1.6), rrect(13, 13, 7, 7, 1.6), stroke();
  } else if (n == "details") {
    M(9, 6.5), L(20, 6.5), M(9, 12), L(20, 12), M(9, 17.5), L(20, 17.5), stroke();
    circle(4.8, 6.5, 1.25), circle(4.8, 12, 1.25), circle(4.8, 17.5, 1.25), fill();
  } else if (n == "compact") {
    rrect(3.5, 5, 4, 4, 1), rrect(3.5, 15, 4, 4, 1), rrect(13, 5, 4, 4, 1), rrect(13, 15, 4, 4, 1), stroke();
    M(9, 7), L(11, 7), M(9, 17), L(11, 17), M(18.5, 7), L(20.5, 7), M(18.5, 17), L(20.5, 17), stroke();
  } else if (n == "select-all") {
    rrect(3.5, 3.5, 17, 17, 2.5), stroke();
    M(7.5, 12.5), L(10.5, 15.5), L(16.5, 9), stroke();
  } else if (n == "invert") {
    rrect(3.5, 3.5, 17, 17, 2.5), stroke();
    M(3.5, 20.5), L(20.5, 3.5), stroke();
    M(6, 6), L(11, 6), L(6, 11), Z(), fill();
  } else if (n == "properties" || n == "info") {
    circle(12, 12, 8.5), stroke();
    M(12, 11), L(12, 16.5), stroke();
    circle(12, 7.8, 1.1), fill();
  } else if (n == "settings") {
    circle(12, 12, 3), stroke();
    for (int i = 0; i < 8; ++i) {
      double a = i * M_PI / 4;
      M(12 + 6 * cos(a), 12 + 6 * sin(a)), L(12 + 8.6 * cos(a), 12 + 8.6 * sin(a));
    }
    stroke();
    circle(12, 12, 6), stroke();
  } else if (n == "pin" || n == "pin-filled") {
    // A push pin, tilted like Windows' Quick access mark.
    cairo_save(cr);
    cairo_translate(cr, 12, 12);
    cairo_rotate(cr, M_PI / 4);
    cairo_translate(cr, -12, -12);
    M(9, 3.5), L(15, 3.5), M(10, 3.5), L(10, 9.5), L(7, 13.5), L(17, 13.5), L(14, 9.5), L(14, 3.5);
    if (n == "pin-filled") {
      cairo_close_path(cr);
      cairo_fill_preserve(cr);
    }
    stroke();
    M(12, 13.5), L(12, 20.5), stroke();
    cairo_restore(cr);
  } else if (n == "home") {
    M(3.5, 11.2), L(12, 4), L(20.5, 11.2), stroke();
    M(6, 9.5), L(6, 19.5), L(18, 19.5), L(18, 9.5), stroke();
    M(10, 19.5), L(10, 14.5), L(14, 14.5), L(14, 19.5), stroke();
  } else if (n == "folder") {
    folder(), stroke();
  } else if (n == "open") {
    M(3, 17), L(3, 7), C(3, 5.9, 3.9, 5, 5, 5), L(9.2, 5), L(11.2, 7), L(17, 7), C(18.1, 7, 19, 7.9, 19, 9), L(19, 10),
        stroke();
    M(3, 19), L(6.3, 11.5), C(6.6, 10.9, 7.2, 10.5, 7.9, 10.5), L(20.7, 10.5), C(21.4, 10.5, 21.8, 11.2, 21.5, 11.8),
        L(18.6, 18), C(18.3, 18.6, 17.7, 19, 17, 19), Z(), stroke();
  } else if (n == "drive" || n == "computer") {
    if (n == "computer") {
      rrect(3, 4, 18, 12.5, 2), stroke();
      M(8.5, 20), L(15.5, 20), M(12, 16.5), L(12, 20), stroke();
    } else {
      rrect(3, 6.5, 18, 11, 2.5), stroke();
      M(3, 13.5), L(21, 13.5), stroke();
      circle(17.3, 15.7, 0.9), fill();
    }
  } else if (n == "usb") {
    rrect(7, 9, 10, 12, 2), stroke();
    M(9, 9), L(9, 3.5), L(15, 3.5), L(15, 9), stroke();
    M(11, 6), L(11, 6.3), M(13, 6), L(13, 6.3), stroke();
  } else if (n == "network" || n == "globe") {
    circle(12, 12, 8.5), stroke();
    cairo_save(cr);
    cairo_translate(cr, 12, 12);
    cairo_scale(cr, 0.45, 1);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0, 0, 8.5, 0, 2 * M_PI);
    cairo_restore(cr);
    stroke();
    M(3.5, 12), L(20.5, 12), stroke();
  } else if (n == "server" || n == "connect") {
    rrect(4, 3.5, 16, 7, 1.8), rrect(4, 13.5, 16, 7, 1.8), stroke();
    circle(8, 7, 0.9), circle(8, 17, 0.9), fill();
    if (n == "connect") M(13, 7), L(17, 7), M(13, 17), L(17, 17), stroke();
  } else if (n == "eject") {
    M(12, 5), L(19, 13), L(5, 13), Z(), stroke();
    M(5, 18), L(19, 18), stroke();
  } else if (n == "terminal") {
    rrect(3, 4.5, 18, 15, 2.5), stroke();
    M(7, 9.5), L(10, 12), L(7, 14.5), M(12, 15), L(16.5, 15), stroke();
  } else if (n == "extract" || n == "compress") {
    rrect(4, 4, 16, 16, 2.5), stroke();
    M(10.5, 4), L(10.5, 6), M(13.5, 7), L(13.5, 9), M(10.5, 10), L(10.5, 12), stroke();
    if (n == "extract") M(15, 15), L(19, 19), M(19, 15.5), L(19, 19), L(15.5, 19), stroke();
    else M(19, 19), L(15, 15), M(15, 18.5), L(15, 15), L(18.5, 15), stroke();
  } else if (n == "install") {
    M(12, 3.5), L(12, 14), stroke();
    M(7.5, 9.5), L(12, 14), L(16.5, 9.5), stroke();
    M(4, 15), L(4, 18.5), C(4, 19.6, 4.9, 20.5, 6, 20.5), L(18, 20.5), C(19.1, 20.5, 20, 19.6, 20, 18.5), L(20, 15),
        stroke();
  } else if (n == "appimage") {
    rrect(4, 4, 16, 16, 3), stroke();
    M(9, 10), L(15, 10), L(12, 15), Z(), stroke();
  } else if (n == "check") {
    M(5, 12.5), L(10, 17.5), L(19.5, 7), stroke();
  } else if (n == "sidebar") {
    rrect(3, 4.5, 18, 15, 2.5), stroke();
    M(9.5, 4.5), L(9.5, 19.5), stroke();
  } else if (n == "recent") {
    circle(12, 12, 8.5), stroke();
    M(12, 7), L(12, 12), L(15.5, 14.2), stroke();
  } else if (n == "undo") {
    M(9, 6), L(4.5, 10.5), L(9, 15), stroke();
    M(4.5, 10.5), L(14, 10.5), C(17.3, 10.5, 20, 13.2, 20, 16.5), L(20, 18.5), stroke();
  } else if (n == "link") {
    M(10, 14), L(14, 10), stroke();
    M(11.2, 6.8), L(13, 5), C(14.9, 3.1, 17.9, 3.1, 19.5, 4.8), C(21.2, 6.5, 21, 9.3, 19.2, 11.1), L(17.2, 13), stroke();
    M(12.8, 17.2), L(11, 19), C(9.1, 20.9, 6.1, 20.9, 4.5, 19.2), C(2.8, 17.5, 3, 14.7, 4.8, 12.9), L(6.8, 11), stroke();
  } else if (n == "warning") {
    M(12, 3.5), L(21.5, 20), L(2.5, 20), Z(), stroke();
    M(12, 10), L(12, 14), stroke();
    circle(12, 17, 1), fill();
  } else if (n == "file") {
    page();
  } else if (n == "restore-item") {
    M(4, 11), C(4, 6.6, 7.6, 3, 12, 3), C(16.4, 3, 20, 6.6, 20, 11), C(20, 15.4, 16.4, 19, 12, 19), stroke();
    M(4, 5.5), L(4, 11), L(9.5, 11), stroke();
  } else if (n == "hidden") {
    M(3, 12), C(5.5, 7.5, 8.5, 5.5, 12, 5.5), C(15.5, 5.5, 18.5, 7.5, 21, 12), C(18.5, 16.5, 15.5, 18.5, 12, 18.5),
        C(8.5, 18.5, 5.5, 16.5, 3, 12), Z(), stroke();
    circle(12, 12, 3), stroke();
  } else if (n == "zoom-in") {
    circle(10.5, 10.5, 6.3), stroke();
    M(15.2, 15.2), L(20, 20), M(10.5, 7.5), L(10.5, 13.5), M(7.5, 10.5), L(13.5, 10.5), stroke();
  } else if (n == "star") {
    for (int i = 0; i < 10; ++i) {
      double a = -M_PI / 2 + i * M_PI / 5, r = i % 2 ? 3.8 : 8.5;
      if (i == 0) M(12 + r * cos(a), 12.5 + r * sin(a));
      else L(12 + r * cos(a), 12.5 + r * sin(a));
    }
    Z(), stroke();
  } else if (n == "filter") {
    M(4, 5), L(20, 5), L(14, 12.5), L(14, 19), L(10, 17), L(10, 12.5), Z(), stroke();
  }
  cairo_restore(cr);
}

}  // namespace liria
}  // namespace sde
