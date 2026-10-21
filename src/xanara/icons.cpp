// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Xanara's vector icons: rounded 1.6 px strokes on a 24 unit grid, drawn
// with cairo so they stay sharp at every UI scale.
#include <cmath>

#include "xanara.h"

using namespace sde;

namespace xanara {
namespace icon {

namespace {

struct Pen {
  cairo_t *cr;
  void move(double x, double y) { cairo_move_to(cr, x, y); }
  void line(double x, double y) { cairo_line_to(cr, x, y); }
  void seg(double x1, double y1, double x2, double y2) {
    move(x1, y1);
    line(x2, y2);
  }
  void rect(double x, double y, double w, double h, double r = 2) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
  }
  void circle(double x, double y, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x, y, r, 0, 2 * M_PI);
  }
  void arc(double x, double y, double r, double a1, double a2) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x, y, r, a1, a2);
  }
  void stroke() { cairo_stroke(cr); }
  void fill() { cairo_fill(cr); }
};

}  // namespace

void draw(Painter &p, Kind kind, double x, double y, double size, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 24.0, size / 24.0);
  cairo_set_line_width(cr, 1.7);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
  Pen d{cr};
  switch (kind) {
    case Sidebar:
      d.rect(3, 5, 18, 14, 2.5);
      d.seg(9.5, 5, 9.5, 19);
      d.stroke();
      d.seg(5, 9, 7.5, 9);
      d.seg(5, 12, 7.5, 12);
      d.stroke();
      break;
    case Open:
      d.move(3, 7.5);
      d.line(3, 18.5);
      d.line(17.5, 18.5);
      d.line(21, 11);
      d.line(7, 11);
      d.line(3.5, 18);
      d.stroke();
      d.move(3, 7.5);
      d.line(3, 5.5);
      d.line(9, 5.5);
      d.line(11, 7.5);
      d.line(17, 7.5);
      d.line(17, 11);
      d.stroke();
      break;
    case Save:
      d.move(12, 3.5);
      d.line(12, 14.5);
      d.stroke();
      d.move(7.5, 10);
      d.line(12, 14.5);
      d.line(16.5, 10);
      d.stroke();
      d.move(4, 14);
      d.line(4, 19.5);
      d.line(20, 19.5);
      d.line(20, 14);
      d.stroke();
      break;
    case Print:
      d.rect(3.5, 9, 17, 8, 2);
      d.stroke();
      d.move(7, 9); d.line(7, 4); d.line(17, 4); d.line(17, 9);
      d.stroke();
      d.rect(7, 14, 10, 6.5, 1);
      d.stroke();
      break;
    case Find:
      d.circle(10.5, 10.5, 6);
      d.stroke();
      d.seg(15, 15, 20, 20);
      d.stroke();
      break;
    case Menu:
      d.circle(5.5, 12, 1.5);
      d.circle(12, 12, 1.5);
      d.circle(18.5, 12, 1.5);
      d.fill();
      break;
    case ZoomIn:
    case ZoomOut:
      d.seg(6, 12, 18, 12);
      if (kind == ZoomIn) d.seg(12, 6, 12, 18);
      d.stroke();
      break;
    case FitWidth:
      d.rect(5.5, 3.5, 13, 17, 1.5);
      d.stroke();
      d.seg(2, 12, 22, 12);
      d.move(4.5, 9.5); d.line(2, 12); d.line(4.5, 14.5);
      d.move(19.5, 9.5); d.line(22, 12); d.line(19.5, 14.5);
      d.stroke();
      break;
    case FitPage:
      d.rect(6, 3.5, 12, 17, 1.5);
      d.stroke();
      d.move(9, 9); d.line(12, 6.5); d.line(15, 9);
      d.move(9, 15); d.line(12, 17.5); d.line(15, 15);
      d.stroke();
      break;
    case Prev:
    case Chevron:
      if (kind == Prev) {
        d.move(14.5, 6); d.line(8.5, 12); d.line(14.5, 18);
      } else {
        d.move(9.5, 6); d.line(15.5, 12); d.line(9.5, 18);
      }
      d.stroke();
      break;
    case Next:
      d.move(9.5, 6); d.line(15.5, 12); d.line(9.5, 18);
      d.stroke();
      break;
    case Up:
      d.move(6, 14.5); d.line(12, 8.5); d.line(18, 14.5);
      d.stroke();
      break;
    case Down:
    case ChevronDown:
      d.move(6, 9.5); d.line(12, 15.5); d.line(18, 9.5);
      d.stroke();
      break;
    case Select:
      d.move(6, 3.5); d.line(6, 18.5); d.line(10, 14.8); d.line(12.8, 20.5); d.line(15, 19.4);
      d.line(12.3, 13.8); d.line(17.5, 13.5);
      cairo_close_path(cr);
      d.stroke();
      break;
    case Highlight:
      d.move(14, 4); d.line(20, 10); d.line(12, 18); d.line(6, 12);
      cairo_close_path(cr);
      d.stroke();
      d.move(6, 12); d.line(4, 17); d.line(7, 20); d.line(12, 18);
      d.stroke();
      cairo_set_line_width(cr, 2.4);
      d.seg(3, 21.5, 21, 21.5);
      cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * 0.55);
      d.stroke();
      break;
    case Underline:
      d.move(7, 4); d.line(7, 11); cairo_curve_to(cr, 7, 17, 17, 17, 17, 11); d.line(17, 4);
      d.stroke();
      d.seg(5, 20.5, 19, 20.5);
      d.stroke();
      break;
    case Strike:
      d.move(17, 6.5); cairo_curve_to(cr, 15.5, 3.5, 7.5, 3.5, 7.5, 8);
      cairo_curve_to(cr, 7.5, 12, 16.5, 11, 16.5, 16); cairo_curve_to(cr, 16.5, 20.5, 8, 20.5, 6.5, 17.5);
      d.stroke();
      d.seg(3.5, 12, 20.5, 12);
      d.stroke();
      break;
    case Note:
      d.move(4, 4); d.line(20, 4); d.line(20, 14); d.line(14, 20); d.line(4, 20);
      cairo_close_path(cr);
      d.stroke();
      d.move(14, 20); d.line(14, 14); d.line(20, 14);
      d.stroke();
      d.seg(8, 9, 16, 9);
      d.stroke();
      break;
    case TextBox: {
      static const double dash[] = {2.2, 2.2};
      cairo_set_dash(cr, dash, 2, 0);
      d.rect(3, 4, 18, 16, 2);
      d.stroke();
      cairo_set_dash(cr, nullptr, 0, 0);
      d.seg(8, 8.5, 16, 8.5);
      d.seg(12, 8.5, 12, 16.5);
      d.stroke();
      break;
    }
    case Pen:
      d.move(4, 20); d.line(5, 15.5); d.line(16, 4.5); d.line(19.5, 8); d.line(8.5, 19);
      cairo_close_path(cr);
      d.stroke();
      d.seg(13.5, 7, 17, 10.5);
      d.stroke();
      break;
    case Rect:
      d.rect(4, 6, 16, 12, 1.5);
      d.stroke();
      break;
    case Line:
      d.seg(5, 19, 19, 5);
      d.stroke();
      d.circle(5, 19, 1.6);
      d.circle(19, 5, 1.6);
      d.fill();
      break;
    case Signature:
      d.move(3.5, 15.5);
      cairo_curve_to(cr, 6, 6, 9, 5, 8.5, 11);
      cairo_curve_to(cr, 8, 17, 11, 9, 12.5, 12.5);
      cairo_curve_to(cr, 13.5, 15, 15, 11, 16.5, 12.5);
      cairo_curve_to(cr, 17.5, 13.5, 19, 13, 20.5, 12);
      d.stroke();
      d.seg(3.5, 19.5, 20.5, 19.5);
      d.stroke();
      break;
    case Close:
      d.seg(6.5, 6.5, 17.5, 17.5);
      d.seg(17.5, 6.5, 6.5, 17.5);
      d.stroke();
      break;
    case RotateLeft:
    case RotateRight: {
      if (kind == RotateLeft) {
        cairo_translate(cr, 24, 0);
        cairo_scale(cr, -1, 1);
      }
      d.arc(12, 12.5, 7, -M_PI * 0.85, M_PI * 0.6);
      d.stroke();
      d.move(18.5, 3.5); d.line(18.8, 8.2); d.line(14.2, 8.3);
      d.stroke();
      break;
    }
    case Moon:
      d.move(15.5, 3.8);
      cairo_curve_to(cr, 8, 3.5, 4, 9, 4.5, 13.5);
      cairo_curve_to(cr, 5.5, 19.5, 13.5, 22, 19.5, 16.5);
      cairo_curve_to(cr, 12.5, 18, 9, 9.5, 15.5, 3.8);
      d.stroke();
      break;
    case Sun:
      d.circle(12, 12, 4);
      d.stroke();
      for (int i = 0; i < 8; ++i) {
        double a = i * M_PI / 4;
        d.seg(12 + 6.8 * cos(a), 12 + 6.8 * sin(a), 12 + 9 * cos(a), 12 + 9 * sin(a));
      }
      d.stroke();
      break;
    case Sepia:
      d.move(5, 4); d.line(15.5, 4); d.line(19, 7.5); d.line(19, 20); d.line(5, 20);
      cairo_close_path(cr);
      d.stroke();
      d.seg(8.5, 10, 15.5, 10);
      d.seg(8.5, 13.5, 15.5, 13.5);
      d.seg(8.5, 17, 12.5, 17);
      d.stroke();
      break;
    case Trash:
      d.seg(4, 6.5, 20, 6.5);
      d.stroke();
      d.move(9, 6.5); d.line(9.5, 3.5); d.line(14.5, 3.5); d.line(15, 6.5);
      d.stroke();
      d.move(6, 6.5); d.line(7, 20.5); d.line(17, 20.5); d.line(18, 6.5);
      d.stroke();
      break;
    case Undo:
      d.move(8.5, 5); d.line(4.5, 9); d.line(8.5, 13);
      d.stroke();
      d.move(4.5, 9); d.line(14, 9); cairo_curve_to(cr, 21, 9, 21, 19.5, 14, 19.5); d.line(8, 19.5);
      d.stroke();
      break;
    case Present:
      d.rect(3, 4, 18, 12, 1.5);
      d.stroke();
      d.seg(12, 16, 12, 20);
      d.seg(8, 20.5, 16, 20.5);
      d.stroke();
      d.move(10.5, 7.5); d.line(15, 10); d.line(10.5, 12.5);
      cairo_close_path(cr);
      d.fill();
      break;
    case Pages:
      d.rect(4, 3.5, 7, 9, 1);
      d.rect(13, 3.5, 7, 9, 1);
      d.rect(4, 14.5, 7, 6, 1);
      d.rect(13, 14.5, 7, 6, 1);
      d.stroke();
      break;
    case Outline:
      d.seg(4, 6, 6, 6);
      d.seg(9, 6, 20, 6);
      d.seg(7, 12, 9, 12);
      d.seg(12, 12, 20, 12);
      d.seg(7, 18, 9, 18);
      d.seg(12, 18, 20, 18);
      d.stroke();
      break;
    case Notes:
      d.move(4, 5); d.line(20, 5); d.line(20, 16); d.line(11, 16); d.line(6.5, 20); d.line(6.5, 16);
      d.line(4, 16);
      cairo_close_path(cr);
      d.stroke();
      break;
    case Book:
      d.move(12, 6.5); cairo_curve_to(cr, 9, 4.5, 5.5, 4.5, 3, 5.5); d.line(3, 19);
      cairo_curve_to(cr, 5.5, 18, 9, 18, 12, 20); cairo_curve_to(cr, 15, 18, 18.5, 18, 21, 19);
      d.line(21, 5.5); cairo_curve_to(cr, 18.5, 4.5, 15, 4.5, 12, 6.5); d.line(12, 20);
      d.stroke();
      break;
    case Single:
      d.rect(6.5, 3.5, 11, 17, 1.5);
      d.stroke();
      break;
    case Continuous:
      d.rect(6.5, 2, 11, 9, 1.5);
      d.rect(6.5, 13, 11, 9, 1.5);
      d.stroke();
      break;
    case Check:
      d.move(5, 12.5); d.line(10, 17.5); d.line(19, 7);
      d.stroke();
      break;
    case Document:
      d.move(6, 3); d.line(14, 3); d.line(19, 8); d.line(19, 21); d.line(6, 21);
      cairo_close_path(cr);
      d.stroke();
      d.move(14, 3); d.line(14, 8); d.line(19, 8);
      d.stroke();
      break;
    case Lock:
      d.rect(5.5, 10.5, 13, 10, 2);
      d.stroke();
      d.move(8.5, 10.5); d.line(8.5, 7.5); cairo_curve_to(cr, 8.5, 3, 15.5, 3, 15.5, 7.5); d.line(15.5, 10.5);
      d.stroke();
      break;
  }
  cairo_restore(cr);
}

}  // namespace icon
}  // namespace xanara
