// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Line icons for Gallery. They are drawn on a 24-unit grid with round caps,
// so they stay crisp at every UI scale and follow the theme's colors.
#include <cmath>

#include "line_icons.h"

namespace gallery {
namespace icon {

namespace {

void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

void heart_path(cairo_t *cr) {
  cairo_move_to(cr, 12, 20);
  cairo_curve_to(cr, 5, 15.5, 2.5, 12, 2.5, 8.6);
  cairo_curve_to(cr, 2.5, 5.6, 4.8, 3.6, 7.4, 3.6);
  cairo_curve_to(cr, 9.4, 3.6, 11, 4.8, 12, 6.6);
  cairo_curve_to(cr, 13, 4.8, 14.6, 3.6, 16.6, 3.6);
  cairo_curve_to(cr, 19.2, 3.6, 21.5, 5.6, 21.5, 8.6);
  cairo_curve_to(cr, 21.5, 12, 19, 15.5, 12, 20);
  cairo_close_path(cr);
}

void magnifier(cairo_t *cr) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, 10.5, 10.5, 6.5, 0, 2 * M_PI);
  cairo_move_to(cr, 15.3, 15.3);
  cairo_line_to(cr, 20.5, 20.5);
}

}  // namespace

void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 24.0, size / 24.0);
  cairo_set_line_width(cr, size >= 20 ? 1.8 : 2.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  p.set(c);
  switch (kind) {
    case Photos:
      rrect(cr, 3, 3, 8, 8, 2);
      rrect(cr, 13, 3, 8, 8, 2);
      rrect(cr, 3, 13, 8, 8, 2);
      rrect(cr, 13, 13, 8, 8, 2);
      cairo_stroke(cr);
      break;
    case Heart:
      heart_path(cr);
      cairo_stroke(cr);
      break;
    case HeartFilled:
      heart_path(cr);
      cairo_fill(cr);
      break;
    case Clock:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      cairo_move_to(cr, 12, 7);
      cairo_line_to(cr, 12, 12);
      cairo_line_to(cr, 15.5, 14.5);
      cairo_stroke(cr);
      break;
    case Screenshot:
      // Viewfinder corners around a small screen.
      cairo_move_to(cr, 3, 8);
      cairo_line_to(cr, 3, 3);
      cairo_line_to(cr, 8, 3);
      cairo_move_to(cr, 16, 3);
      cairo_line_to(cr, 21, 3);
      cairo_line_to(cr, 21, 8);
      cairo_move_to(cr, 21, 16);
      cairo_line_to(cr, 21, 21);
      cairo_line_to(cr, 16, 21);
      cairo_move_to(cr, 8, 21);
      cairo_line_to(cr, 3, 21);
      cairo_line_to(cr, 3, 16);
      cairo_stroke(cr);
      rrect(cr, 7, 8, 10, 8, 1.5);
      cairo_stroke(cr);
      break;
    case Folder:
      cairo_move_to(cr, 3, 7);
      cairo_curve_to(cr, 3, 5.5, 3.8, 4.5, 5.2, 4.5);
      cairo_line_to(cr, 9.5, 4.5);
      cairo_line_to(cr, 11.5, 6.8);
      cairo_line_to(cr, 18.8, 6.8);
      cairo_curve_to(cr, 20.2, 6.8, 21, 7.8, 21, 9.2);
      cairo_line_to(cr, 21, 17.5);
      cairo_curve_to(cr, 21, 18.9, 20.2, 19.6, 18.8, 19.6);
      cairo_line_to(cr, 5.2, 19.6);
      cairo_curve_to(cr, 3.8, 19.6, 3, 18.9, 3, 17.5);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Plus:
      cairo_move_to(cr, 12, 5);
      cairo_line_to(cr, 12, 19);
      cairo_move_to(cr, 5, 12);
      cairo_line_to(cr, 19, 12);
      cairo_stroke(cr);
      break;
    case Close:
      cairo_move_to(cr, 6.5, 6.5);
      cairo_line_to(cr, 17.5, 17.5);
      cairo_move_to(cr, 17.5, 6.5);
      cairo_line_to(cr, 6.5, 17.5);
      cairo_stroke(cr);
      break;
    case Search:
      magnifier(cr);
      cairo_stroke(cr);
      break;
    case Back:
      cairo_move_to(cr, 19, 12);
      cairo_line_to(cr, 5, 12);
      cairo_move_to(cr, 11, 6);
      cairo_line_to(cr, 5, 12);
      cairo_line_to(cr, 11, 18);
      cairo_stroke(cr);
      break;
    case Prev:
      cairo_move_to(cr, 15, 5);
      cairo_line_to(cr, 8, 12);
      cairo_line_to(cr, 15, 19);
      cairo_stroke(cr);
      break;
    case Next:
      cairo_move_to(cr, 9, 5);
      cairo_line_to(cr, 16, 12);
      cairo_line_to(cr, 9, 19);
      cairo_stroke(cr);
      break;
    case ZoomIn:
    case ZoomOut:
      magnifier(cr);
      cairo_move_to(cr, 7.5, 10.5);
      cairo_line_to(cr, 13.5, 10.5);
      if (kind == ZoomIn) {
        cairo_move_to(cr, 10.5, 7.5);
        cairo_line_to(cr, 10.5, 13.5);
      }
      cairo_stroke(cr);
      break;
    case Fit:
      cairo_move_to(cr, 4, 9);
      cairo_line_to(cr, 4, 4);
      cairo_line_to(cr, 9, 4);
      cairo_move_to(cr, 15, 4);
      cairo_line_to(cr, 20, 4);
      cairo_line_to(cr, 20, 9);
      cairo_move_to(cr, 20, 15);
      cairo_line_to(cr, 20, 20);
      cairo_line_to(cr, 15, 20);
      cairo_move_to(cr, 9, 20);
      cairo_line_to(cr, 4, 20);
      cairo_line_to(cr, 4, 15);
      cairo_stroke(cr);
      break;
    case Rotate:
      // Clockwise arrow (mirrored counter-clockwise drawing).
      cairo_translate(cr, 24, 0);
      cairo_scale(cr, -1, 1);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 13, 7.5, -M_PI * 0.95, M_PI * 0.6);
      cairo_stroke(cr);
      cairo_move_to(cr, 3.2, 6.5);
      cairo_line_to(cr, 4.6, 12);
      cairo_line_to(cr, 9.6, 9.8);
      cairo_stroke(cr);
      break;
    case Play:
      cairo_move_to(cr, 8, 5);
      cairo_line_to(cr, 19, 12);
      cairo_line_to(cr, 8, 19);
      cairo_close_path(cr);
      cairo_stroke_preserve(cr);
      cairo_fill(cr);
      break;
    case Pause:
      rrect(cr, 6.5, 5, 3.5, 14, 1);
      rrect(cr, 14, 5, 3.5, 14, 1);
      cairo_fill(cr);
      break;
    case Info:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      cairo_move_to(cr, 12, 11);
      cairo_line_to(cr, 12, 16.5);
      cairo_stroke(cr);
      cairo_arc(cr, 12, 7.6, 1.25, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Trash:
      cairo_move_to(cr, 4, 6.5);
      cairo_line_to(cr, 20, 6.5);
      cairo_move_to(cr, 9.5, 6.5);
      cairo_line_to(cr, 9.5, 4);
      cairo_line_to(cr, 14.5, 4);
      cairo_line_to(cr, 14.5, 6.5);
      cairo_move_to(cr, 6, 6.5);
      cairo_line_to(cr, 7, 20);
      cairo_line_to(cr, 17, 20);
      cairo_line_to(cr, 18, 6.5);
      cairo_move_to(cr, 10, 10.5);
      cairo_line_to(cr, 10, 16);
      cairo_move_to(cr, 14, 10.5);
      cairo_line_to(cr, 14, 16);
      cairo_stroke(cr);
      break;
    case Edit:
      cairo_move_to(cr, 4, 20);
      cairo_line_to(cr, 4.8, 15.6);
      cairo_line_to(cr, 15.5, 4.9);
      cairo_line_to(cr, 19.1, 8.5);
      cairo_line_to(cr, 8.4, 19.2);
      cairo_close_path(cr);
      cairo_move_to(cr, 13, 7.4);
      cairo_line_to(cr, 16.6, 11);
      cairo_stroke(cr);
      break;
    case OpenWith:
      cairo_move_to(cr, 11, 4.5);
      cairo_line_to(cr, 6, 4.5);
      cairo_curve_to(cr, 5, 4.5, 4.5, 5, 4.5, 6);
      cairo_line_to(cr, 4.5, 18);
      cairo_curve_to(cr, 4.5, 19, 5, 19.5, 6, 19.5);
      cairo_line_to(cr, 18, 19.5);
      cairo_curve_to(cr, 19, 19.5, 19.5, 19, 19.5, 18);
      cairo_line_to(cr, 19.5, 13);
      cairo_move_to(cr, 14, 4.5);
      cairo_line_to(cr, 19.5, 4.5);
      cairo_line_to(cr, 19.5, 10);
      cairo_move_to(cr, 19.5, 4.5);
      cairo_line_to(cr, 11, 13);
      cairo_stroke(cr);
      break;
    case Wallpaper:
      rrect(cr, 3, 4, 18, 13, 2);
      cairo_move_to(cr, 9, 20.5);
      cairo_line_to(cr, 15, 20.5);
      cairo_move_to(cr, 3.5, 14.5);
      cairo_line_to(cr, 8.5, 10);
      cairo_line_to(cr, 12, 13);
      cairo_line_to(cr, 14.5, 11);
      cairo_line_to(cr, 20.5, 15.5);
      cairo_stroke(cr);
      break;
    case Files:
      cairo_move_to(cr, 3, 18);
      cairo_line_to(cr, 3, 6);
      cairo_curve_to(cr, 3, 5, 3.6, 4.5, 4.6, 4.5);
      cairo_line_to(cr, 9, 4.5);
      cairo_line_to(cr, 11, 6.7);
      cairo_line_to(cr, 17.5, 6.7);
      cairo_curve_to(cr, 18.5, 6.7, 19, 7.3, 19, 8.3);
      cairo_line_to(cr, 19, 10);
      cairo_move_to(cr, 3, 18);
      cairo_line_to(cr, 6, 10);
      cairo_line_to(cr, 21.5, 10);
      cairo_line_to(cr, 18.5, 18.5);
      cairo_line_to(cr, 3, 18.5);
      cairo_stroke(cr);
      break;
    case Copy:
      rrect(cr, 8, 8, 12, 12, 2);
      cairo_move_to(cr, 16, 5.5);
      cairo_curve_to(cr, 16, 4.5, 15.5, 4, 14.5, 4);
      cairo_line_to(cr, 5.5, 4);
      cairo_curve_to(cr, 4.5, 4, 4, 4.5, 4, 5.5);
      cairo_line_to(cr, 4, 14.5);
      cairo_curve_to(cr, 4, 15.5, 4.5, 16, 5.5, 16);
      cairo_stroke(cr);
      break;
    case Album:
      // A photo on a stack of photos.
      cairo_move_to(cr, 6, 3.5);
      cairo_line_to(cr, 18, 3.5);
      cairo_move_to(cr, 4.5, 6.5);
      cairo_line_to(cr, 19.5, 6.5);
      cairo_stroke(cr);
      rrect(cr, 3, 9.5, 18, 11.5, 2.2);
      cairo_move_to(cr, 3.5, 18.5);
      cairo_line_to(cr, 8.5, 14);
      cairo_line_to(cr, 12.5, 17.5);
      cairo_line_to(cr, 15, 15.5);
      cairo_line_to(cr, 20.5, 19.5);
      cairo_stroke(cr);
      cairo_arc(cr, 16, 12.6, 1.4, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Picture:
    case Small:
    case Large:
      if (kind == Small) {
        cairo_translate(cr, 5, 5);
        cairo_scale(cr, 14.0 / 24, 14.0 / 24);
        cairo_set_line_width(cr, 2.6);
      }
      rrect(cr, 3, 4, 18, 16, 2.5);
      cairo_move_to(cr, 3.5, 17);
      cairo_line_to(cr, 9, 11.5);
      cairo_line_to(cr, 13.5, 16);
      cairo_line_to(cr, 16, 13.5);
      cairo_line_to(cr, 20.5, 18);
      cairo_stroke(cr);
      cairo_arc(cr, 15.5, 8.8, 1.8, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Broken:
      cairo_move_to(cr, 12, 4);
      cairo_line_to(cr, 5, 4);
      cairo_curve_to(cr, 3.8, 4, 3, 4.8, 3, 6);
      cairo_line_to(cr, 3, 18);
      cairo_curve_to(cr, 3, 19.2, 3.8, 20, 5, 20);
      cairo_line_to(cr, 10, 20);
      cairo_line_to(cr, 12, 15);
      cairo_line_to(cr, 9.5, 11);
      cairo_line_to(cr, 12, 4);
      cairo_move_to(cr, 15, 4);
      cairo_line_to(cr, 19, 4);
      cairo_curve_to(cr, 20.2, 4, 21, 4.8, 21, 6);
      cairo_line_to(cr, 21, 18);
      cairo_curve_to(cr, 21, 19.2, 20.2, 20, 19, 20);
      cairo_line_to(cr, 13.5, 20);
      cairo_line_to(cr, 15.5, 15);
      cairo_line_to(cr, 13, 11);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Drive:
      rrect(cr, 3, 7, 18, 10, 2.5);
      cairo_move_to(cr, 6.5, 13.5);
      cairo_line_to(cr, 11, 13.5);
      cairo_stroke(cr);
      cairo_arc(cr, 17, 12, 1.2, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Import:
      cairo_move_to(cr, 12, 3.5);
      cairo_line_to(cr, 12, 14.5);
      cairo_move_to(cr, 7.5, 10);
      cairo_line_to(cr, 12, 14.5);
      cairo_line_to(cr, 16.5, 10);
      cairo_move_to(cr, 4, 14);
      cairo_line_to(cr, 4, 18.5);
      cairo_curve_to(cr, 4, 19.5, 4.5, 20, 5.5, 20);
      cairo_line_to(cr, 18.5, 20);
      cairo_curve_to(cr, 19.5, 20, 20, 19.5, 20, 18.5);
      cairo_line_to(cr, 20, 14);
      cairo_stroke(cr);
      break;
    case Eject:
      cairo_move_to(cr, 12, 5);
      cairo_line_to(cr, 19, 13);
      cairo_line_to(cr, 5, 13);
      cairo_close_path(cr);
      cairo_move_to(cr, 5, 18);
      cairo_line_to(cr, 19, 18);
      cairo_stroke(cr);
      break;
    case Menu:
      for (double yy : {7.0, 12.0, 17.0}) {
        cairo_move_to(cr, 4.5, yy);
        cairo_line_to(cr, 19.5, yy);
      }
      cairo_stroke(cr);
      break;
    case RotateLeft:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 13, 7.5, -M_PI * 0.95, M_PI * 0.6);
      cairo_stroke(cr);
      cairo_move_to(cr, 3.2, 6.5);
      cairo_line_to(cr, 4.6, 12);
      cairo_line_to(cr, 9.6, 9.8);
      cairo_stroke(cr);
      break;
    case FlipH:
    case FlipV:
      if (kind == FlipV) {
        cairo_translate(cr, 12, 12);
        cairo_rotate(cr, M_PI / 2);
        cairo_translate(cr, -12, -12);
      }
      cairo_move_to(cr, 10, 5);
      cairo_line_to(cr, 3.5, 18.5);
      cairo_line_to(cr, 10, 18.5);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 14, 5);
      cairo_line_to(cr, 20.5, 18.5);
      cairo_line_to(cr, 14, 18.5);
      cairo_close_path(cr);
      cairo_fill(cr);
      cairo_set_line_width(cr, 1.2);
      cairo_move_to(cr, 12, 3);
      cairo_line_to(cr, 12, 21);
      cairo_stroke(cr);
      break;
    case Crop:
      cairo_move_to(cr, 7, 3);
      cairo_line_to(cr, 7, 17);
      cairo_line_to(cr, 21, 17);
      cairo_move_to(cr, 3, 7);
      cairo_line_to(cr, 17, 7);
      cairo_line_to(cr, 17, 21);
      cairo_stroke(cr);
      break;
    case Undo:
    case Redo:
      if (kind == Redo) {
        cairo_translate(cr, 24, 0);
        cairo_scale(cr, -1, 1);
      }
      cairo_move_to(cr, 8.5, 5.5);
      cairo_line_to(cr, 4.5, 9.5);
      cairo_line_to(cr, 8.5, 13.5);
      cairo_move_to(cr, 4.5, 9.5);
      cairo_line_to(cr, 14.5, 9.5);
      cairo_curve_to(cr, 17.8, 9.5, 20, 11.7, 20, 14.5);
      cairo_curve_to(cr, 20, 17.3, 17.8, 19.5, 14.5, 19.5);
      cairo_line_to(cr, 10, 19.5);
      cairo_stroke(cr);
      break;
    case Adjust:
      for (int i = 0; i < 3; ++i) {
        double yy = 6 + i * 6, kx = i == 1 ? 15 : i == 0 ? 8 : 11;
        cairo_move_to(cr, 3.5, yy);
        cairo_line_to(cr, kx - 2.2, yy);
        cairo_move_to(cr, kx + 2.2, yy);
        cairo_line_to(cr, 20.5, yy);
        cairo_new_sub_path(cr);
        cairo_arc(cr, kx, yy, 2.2, 0, 2 * M_PI);
      }
      cairo_stroke(cr);
      break;
    case Filters:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 9, 9.5, 5.5, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 15, 9.5, 5.5, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 14.8, 5.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      break;
    case Save:
      cairo_move_to(cr, 4, 5.5);
      cairo_curve_to(cr, 4, 4.5, 4.5, 4, 5.5, 4);
      cairo_line_to(cr, 16, 4);
      cairo_line_to(cr, 20, 8);
      cairo_line_to(cr, 20, 18.5);
      cairo_curve_to(cr, 20, 19.5, 19.5, 20, 18.5, 20);
      cairo_line_to(cr, 5.5, 20);
      cairo_curve_to(cr, 4.5, 20, 4, 19.5, 4, 18.5);
      cairo_close_path(cr);
      cairo_move_to(cr, 8, 4);
      cairo_line_to(cr, 8, 8.5);
      cairo_line_to(cr, 15, 8.5);
      cairo_line_to(cr, 15, 4);
      cairo_stroke(cr);
      rrect(cr, 7.5, 13, 9, 7, 1);
      cairo_stroke(cr);
      break;
    case Fullscreen:
      cairo_move_to(cr, 4, 9); cairo_line_to(cr, 4, 4); cairo_line_to(cr, 9, 4);
      cairo_move_to(cr, 15, 4); cairo_line_to(cr, 20, 4); cairo_line_to(cr, 20, 9);
      cairo_move_to(cr, 20, 15); cairo_line_to(cr, 20, 20); cairo_line_to(cr, 15, 20);
      cairo_move_to(cr, 9, 20); cairo_line_to(cr, 4, 20); cairo_line_to(cr, 4, 15);
      cairo_stroke(cr);
      break;
    case Unfullscreen:
      cairo_move_to(cr, 4, 9); cairo_line_to(cr, 9, 9); cairo_line_to(cr, 9, 4);
      cairo_move_to(cr, 15, 4); cairo_line_to(cr, 15, 9); cairo_line_to(cr, 20, 9);
      cairo_move_to(cr, 20, 15); cairo_line_to(cr, 15, 15); cairo_line_to(cr, 15, 20);
      cairo_move_to(cr, 9, 20); cairo_line_to(cr, 9, 15); cairo_line_to(cr, 4, 15);
      cairo_stroke(cr);
      break;
    case MapPin:
      cairo_move_to(cr, 12, 21);
      cairo_curve_to(cr, 7, 15.5, 5, 12.5, 5, 9.5);
      cairo_curve_to(cr, 5, 5.6, 8.1, 2.8, 12, 2.8);
      cairo_curve_to(cr, 15.9, 2.8, 19, 5.6, 19, 9.5);
      cairo_curve_to(cr, 19, 12.5, 17, 15.5, 12, 21);
      cairo_close_path(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 9.5, 2.6, 0, 2 * M_PI);
      cairo_stroke(cr);
      break;
    case Camera:
      cairo_move_to(cr, 3, 8.5);
      cairo_curve_to(cr, 3, 7.5, 3.5, 7, 4.5, 7);
      cairo_line_to(cr, 7.5, 7);
      cairo_line_to(cr, 9.2, 4.5);
      cairo_line_to(cr, 14.8, 4.5);
      cairo_line_to(cr, 16.5, 7);
      cairo_line_to(cr, 19.5, 7);
      cairo_curve_to(cr, 20.5, 7, 21, 7.5, 21, 8.5);
      cairo_line_to(cr, 21, 18);
      cairo_curve_to(cr, 21, 19, 20.5, 19.5, 19.5, 19.5);
      cairo_line_to(cr, 4.5, 19.5);
      cairo_curve_to(cr, 3.5, 19.5, 3, 19, 3, 18);
      cairo_close_path(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 13, 3.8, 0, 2 * M_PI);
      cairo_stroke(cr);
      break;
    case Print:
      cairo_move_to(cr, 7, 8);
      cairo_line_to(cr, 7, 3.5);
      cairo_line_to(cr, 17, 3.5);
      cairo_line_to(cr, 17, 8);
      cairo_move_to(cr, 7, 16.5);
      cairo_line_to(cr, 4.5, 16.5);
      cairo_curve_to(cr, 3.5, 16.5, 3, 16, 3, 15);
      cairo_line_to(cr, 3, 9.5);
      cairo_curve_to(cr, 3, 8.5, 3.5, 8, 4.5, 8);
      cairo_line_to(cr, 19.5, 8);
      cairo_curve_to(cr, 20.5, 8, 21, 8.5, 21, 9.5);
      cairo_line_to(cr, 21, 15);
      cairo_curve_to(cr, 21, 16, 20.5, 16.5, 19.5, 16.5);
      cairo_line_to(cr, 17, 16.5);
      cairo_stroke(cr);
      rrect(cr, 7, 13, 10, 7.5, 0.8);
      cairo_stroke(cr);
      break;
    case Resize:
      rrect(cr, 3.5, 10, 10.5, 10.5, 1.5);
      cairo_move_to(cr, 8, 5.5);
      cairo_line_to(cr, 8, 4.5);
      cairo_curve_to(cr, 8, 3.8, 8.3, 3.5, 9, 3.5);
      cairo_line_to(cr, 19.5, 3.5);
      cairo_curve_to(cr, 20.2, 3.5, 20.5, 3.8, 20.5, 4.5);
      cairo_line_to(cr, 20.5, 15);
      cairo_curve_to(cr, 20.5, 15.7, 20.2, 16, 19.5, 16);
      cairo_line_to(cr, 18.5, 16);
      cairo_move_to(cr, 12, 12);
      cairo_line_to(cr, 17, 7);
      cairo_move_to(cr, 13, 7);
      cairo_line_to(cr, 17, 7);
      cairo_line_to(cr, 17, 11);
      cairo_stroke(cr);
      break;
    case Gallery:
      rrect(cr, 3, 3, 8, 8, 2);
      rrect(cr, 13, 3, 8, 8, 2);
      rrect(cr, 3, 13, 8, 8, 2);
      cairo_stroke(cr);
      cairo_move_to(cr, 17, 13.5);
      cairo_line_to(cr, 17, 20.5);
      cairo_move_to(cr, 13.5, 17);
      cairo_line_to(cr, 20.5, 17);
      cairo_stroke(cr);
      break;
    case Film:
      rrect(cr, 3, 5, 18, 14, 2);
      cairo_move_to(cr, 7, 5);
      cairo_line_to(cr, 7, 19);
      cairo_move_to(cr, 17, 5);
      cairo_line_to(cr, 17, 19);
      cairo_stroke(cr);
      break;
    case Check:
      cairo_set_line_width(cr, 2.6);
      cairo_move_to(cr, 6.5, 12.5);
      cairo_line_to(cr, 10.5, 16.5);
      cairo_line_to(cr, 17.5, 8);
      cairo_stroke(cr);
      break;
  }
  cairo_restore(cr);
}

}  // namespace icon
}  // namespace gallery
