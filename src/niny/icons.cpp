// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Line icons for Niny, drawn on a 24-unit grid with round caps so they stay
// sharp at every UI scale and follow the theme colors.
#include <cmath>

#include "niny.h"

using namespace sde;

namespace niny {
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

void line(cairo_t *cr, double x1, double y1, double x2, double y2) {
  cairo_move_to(cr, x1, y1);
  cairo_line_to(cr, x2, y2);
}

void page(cairo_t *cr) {
  cairo_move_to(cr, 6, 3);
  cairo_line_to(cr, 14, 3);
  cairo_line_to(cr, 19, 8);
  cairo_line_to(cr, 19, 21);
  cairo_line_to(cr, 6, 21);
  cairo_close_path(cr);
  line(cr, 14, 3, 14, 8);
  cairo_line_to(cr, 19, 8);
}

void folder(cairo_t *cr) {
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
}

void arrow_curve(cairo_t *cr, bool redo) {
  // Curved arrow (undo points left).
  double s = redo ? -1 : 1;
  cairo_save(cr);
  if (redo) {
    cairo_translate(cr, 24, 0);
    cairo_scale(cr, -1, 1);
  }
  (void)s;
  cairo_move_to(cr, 9, 5);
  cairo_line_to(cr, 4.5, 9.5);
  cairo_line_to(cr, 9, 14);
  cairo_move_to(cr, 4.5, 9.5);
  cairo_line_to(cr, 14, 9.5);
  cairo_curve_to(cr, 17.5, 9.5, 20, 12, 20, 15);
  cairo_curve_to(cr, 20, 18, 17.5, 20, 14, 20);
  cairo_line_to(cr, 11, 20);
  cairo_restore(cr);
}

}  // namespace

void draw(Painter &p, Kind kind, double x, double y, double size, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 24.0, size / 24.0);
  cairo_set_line_width(cr, size >= 20 ? 1.8 : 2.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  p.set(c);
  switch (kind) {
    case Menu:
      line(cr, 4, 7, 20, 7);
      line(cr, 4, 12, 20, 12);
      line(cr, 4, 17, 14, 17);
      cairo_stroke(cr);
      break;
    case Sidebar:
      rrect(cr, 3, 4.5, 18, 15, 2.5);
      line(cr, 9.5, 4.5, 9.5, 19.5);
      cairo_stroke(cr);
      line(cr, 5.5, 8.5, 7.5, 8.5);
      line(cr, 5.5, 11.5, 7.5, 11.5);
      cairo_stroke(cr);
      break;
    case Plus:
      line(cr, 12, 5, 12, 19);
      line(cr, 5, 12, 19, 12);
      cairo_stroke(cr);
      break;
    case Close:
      line(cr, 7, 7, 17, 17);
      line(cr, 17, 7, 7, 17);
      cairo_stroke(cr);
      break;
    case Search:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 10.5, 10.5, 6.5, 0, 2 * M_PI);
      line(cr, 15.3, 15.3, 20.5, 20.5);
      cairo_stroke(cr);
      break;
    case Command:
      // A prompt chevron and cursor in a rounded box.
      rrect(cr, 3, 4.5, 18, 15, 3);
      cairo_stroke(cr);
      cairo_move_to(cr, 7.5, 9.5);
      cairo_line_to(cr, 10.5, 12);
      cairo_line_to(cr, 7.5, 14.5);
      line(cr, 12.5, 15, 16.5, 15);
      cairo_stroke(cr);
      break;
    case ChevronUp:
      cairo_move_to(cr, 6, 15);
      cairo_line_to(cr, 12, 9);
      cairo_line_to(cr, 18, 15);
      cairo_stroke(cr);
      break;
    case ChevronDown:
      cairo_move_to(cr, 6, 9);
      cairo_line_to(cr, 12, 15);
      cairo_line_to(cr, 18, 9);
      cairo_stroke(cr);
      break;
    case ChevronLeft:
      cairo_move_to(cr, 15, 6);
      cairo_line_to(cr, 9, 12);
      cairo_line_to(cr, 15, 18);
      cairo_stroke(cr);
      break;
    case ChevronRight:
      cairo_move_to(cr, 9, 6);
      cairo_line_to(cr, 15, 12);
      cairo_line_to(cr, 9, 18);
      cairo_stroke(cr);
      break;
    case Folder:
      folder(cr);
      cairo_stroke(cr);
      break;
    case Open:
      folder(cr);
      cairo_stroke(cr);
      line(cr, 12, 10, 12, 16);
      cairo_move_to(cr, 9.5, 12.5);
      cairo_line_to(cr, 12, 10);
      cairo_line_to(cr, 14.5, 12.5);
      cairo_stroke(cr);
      break;
    case File:
      page(cr);
      cairo_stroke(cr);
      break;
    case Documents:
      cairo_move_to(cr, 9, 3);
      cairo_line_to(cr, 16, 3);
      cairo_line_to(cr, 20, 7);
      cairo_line_to(cr, 20, 17);
      cairo_stroke(cr);
      rrect(cr, 4, 6.5, 12, 14.5, 1.5);
      cairo_stroke(cr);
      line(cr, 7, 11, 13, 11);
      line(cr, 7, 14.5, 13, 14.5);
      line(cr, 7, 18, 11, 18);
      cairo_stroke(cr);
      break;
    case Up:
      line(cr, 12, 19, 12, 5);
      cairo_move_to(cr, 6.5, 10.5);
      cairo_line_to(cr, 12, 5);
      cairo_line_to(cr, 17.5, 10.5);
      cairo_stroke(cr);
      break;
    case Gear: {
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 3, 0, 2 * M_PI);
      cairo_stroke(cr);
      for (int i = 0; i < 8; ++i) {
        double a = i * M_PI / 4;
        line(cr, 12 + std::cos(a) * 6.2, 12 + std::sin(a) * 6.2, 12 + std::cos(a) * 8.6, 12 + std::sin(a) * 8.6);
      }
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 6.2, 0, 2 * M_PI);
      cairo_stroke(cr);
      break;
    }
    case Save:
      rrect(cr, 4, 4, 16, 16, 2.5);
      cairo_stroke(cr);
      rrect(cr, 8, 4, 8, 5.5, 0.8);
      cairo_stroke(cr);
      rrect(cr, 7.5, 13, 9, 7, 1);
      cairo_stroke(cr);
      break;
    case Warning:
      cairo_move_to(cr, 12, 3.5);
      cairo_line_to(cr, 21.5, 20);
      cairo_line_to(cr, 2.5, 20);
      cairo_close_path(cr);
      cairo_stroke(cr);
      line(cr, 12, 9.5, 12, 14);
      cairo_stroke(cr);
      cairo_arc(cr, 12, 17, 1.1, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Info:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      cairo_stroke(cr);
      line(cr, 12, 11, 12, 16.5);
      cairo_stroke(cr);
      cairo_arc(cr, 12, 7.8, 1.1, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Replace:
      cairo_move_to(cr, 4, 9);
      cairo_line_to(cr, 4, 6);
      cairo_curve_to(cr, 4, 4.5, 5, 4, 6.5, 4);
      cairo_line_to(cr, 16, 4);
      cairo_move_to(cr, 13.5, 1.5);
      cairo_line_to(cr, 16, 4);
      cairo_line_to(cr, 13.5, 6.5);
      cairo_stroke(cr);
      rrect(cr, 9, 11, 11, 9, 2);
      cairo_stroke(cr);
      break;
    case ReplaceAll:
      rrect(cr, 3, 3, 10, 8, 2);
      cairo_stroke(cr);
      rrect(cr, 11, 13, 10, 8, 2);
      cairo_stroke(cr);
      cairo_move_to(cr, 16, 5);
      cairo_curve_to(cr, 18.5, 5, 19.5, 6.5, 19.5, 9.5);
      cairo_move_to(cr, 17.5, 8);
      cairo_line_to(cr, 19.5, 10);
      cairo_line_to(cr, 21.5, 8);
      cairo_stroke(cr);
      break;
    case Refresh:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 7.5, -M_PI * 0.15, M_PI * 1.55);
      cairo_stroke(cr);
      cairo_move_to(cr, 19.8, 4.5);
      cairo_line_to(cr, 19.6, 9.6);
      cairo_line_to(cr, 14.6, 9.2);
      cairo_stroke(cr);
      break;
    case Wrap:
      line(cr, 4, 6, 20, 6);
      cairo_move_to(cr, 4, 12);
      cairo_line_to(cr, 16.5, 12);
      cairo_curve_to(cr, 19, 12, 20, 13.5, 20, 15);
      cairo_curve_to(cr, 20, 16.5, 19, 18, 16.5, 18);
      cairo_line_to(cr, 12, 18);
      cairo_move_to(cr, 14, 16);
      cairo_line_to(cr, 12, 18);
      cairo_line_to(cr, 14, 20);
      line(cr, 4, 18, 8, 18);
      cairo_stroke(cr);
      break;
    case Dot:
      cairo_arc(cr, 12, 12, 4.2, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Recent:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 8.5, 0, 2 * M_PI);
      cairo_move_to(cr, 12, 7);
      cairo_line_to(cr, 12, 12);
      cairo_line_to(cr, 15.5, 14.5);
      cairo_stroke(cr);
      break;
    case Cut:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 7, 17.5, 3, 0, 2 * M_PI);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 17, 17.5, 3, 0, 2 * M_PI);
      line(cr, 9, 15.2, 16.5, 3.5);
      line(cr, 15, 15.2, 7.5, 3.5);
      cairo_stroke(cr);
      break;
    case Copy:
      rrect(cr, 8, 7, 11.5, 14, 2);
      cairo_move_to(cr, 5, 16.5);
      cairo_line_to(cr, 5, 5);
      cairo_curve_to(cr, 5, 3.8, 5.8, 3, 7, 3);
      cairo_line_to(cr, 15, 3);
      cairo_stroke(cr);
      break;
    case Paste:
      rrect(cr, 5, 5, 14, 16.5, 2);
      cairo_stroke(cr);
      rrect(cr, 8.5, 2.8, 7, 4, 1.5);
      cairo_stroke(cr);
      line(cr, 9, 12, 15, 12);
      line(cr, 9, 16, 13, 16);
      cairo_stroke(cr);
      break;
    case SelectAll:
      for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) rrect(cr, 4.5 + i * 8.5, 4.5 + j * 8.5, 6.5, 6.5, 1.5);
      cairo_stroke(cr);
      break;
    case Trash:
      line(cr, 4, 6.5, 20, 6.5);
      cairo_move_to(cr, 9, 6.5);
      cairo_line_to(cr, 9.5, 3.5);
      cairo_line_to(cr, 14.5, 3.5);
      cairo_line_to(cr, 15, 6.5);
      cairo_move_to(cr, 6, 6.5);
      cairo_line_to(cr, 7, 20.5);
      cairo_line_to(cr, 17, 20.5);
      cairo_line_to(cr, 18, 6.5);
      line(cr, 10, 10.5, 10, 16.5);
      line(cr, 14, 10.5, 14, 16.5);
      cairo_stroke(cr);
      break;
    case Globe:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 8.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_save(cr);
      cairo_translate(cr, 12, 12);
      cairo_scale(cr, 0.45, 1);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 0, 0, 8.5, 0, 2 * M_PI);
      cairo_restore(cr);
      line(cr, 3.5, 12, 20.5, 12);
      cairo_stroke(cr);
      break;
    case TextTools:
      // "Aa"
      cairo_move_to(cr, 3, 18);
      cairo_line_to(cr, 8, 5);
      cairo_line_to(cr, 13, 18);
      line(cr, 5, 13.5, 11, 13.5);
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 17.5, 15, 3, 0, 2 * M_PI);
      line(cr, 20.5, 11.5, 20.5, 18);
      cairo_stroke(cr);
      break;
    case Spelling:
      // "abc" with a check mark.
      cairo_move_to(cr, 3, 12);
      cairo_line_to(cr, 6, 4);
      cairo_line_to(cr, 9, 12);
      line(cr, 4.2, 9, 7.8, 9);
      line(cr, 11.5, 4, 11.5, 12);
      cairo_move_to(cr, 11.5, 8);
      cairo_curve_to(cr, 15.5, 5.5, 15.5, 12.5, 11.5, 11.5);
      cairo_move_to(cr, 21, 6);
      cairo_curve_to(cr, 17.5, 4, 16.5, 12.5, 21, 11);
      cairo_move_to(cr, 8, 16.5);
      cairo_line_to(cr, 11.5, 20);
      cairo_line_to(cr, 19, 13.5);
      cairo_stroke(cr);
      break;
    case Unicode:
      // An Omega.
      cairo_move_to(cr, 4.5, 19.5);
      cairo_line_to(cr, 9, 19.5);
      cairo_line_to(cr, 9, 17.5);
      cairo_curve_to(cr, 4.5, 15.5, 4, 8, 7.5, 5.5);
      cairo_curve_to(cr, 10, 3.5, 14, 3.5, 16.5, 5.5);
      cairo_curve_to(cr, 20, 8, 19.5, 15.5, 15, 17.5);
      cairo_line_to(cr, 15, 19.5);
      cairo_line_to(cr, 19.5, 19.5);
      cairo_stroke(cr);
      break;
    case Bold:
      cairo_set_line_width(cr, 2.6);
      cairo_move_to(cr, 7, 4.5);
      cairo_line_to(cr, 13, 4.5);
      cairo_curve_to(cr, 17.5, 4.5, 17.5, 11.5, 13, 11.5);
      cairo_line_to(cr, 7, 11.5);
      cairo_move_to(cr, 13, 11.5);
      cairo_curve_to(cr, 18.5, 11.5, 18.5, 19.5, 13, 19.5);
      cairo_line_to(cr, 7, 19.5);
      cairo_line_to(cr, 7, 4.5);
      cairo_stroke(cr);
      break;
    case Italic:
      line(cr, 10, 4.5, 18, 4.5);
      line(cr, 6, 19.5, 14, 19.5);
      line(cr, 14.5, 4.5, 9.5, 19.5);
      cairo_stroke(cr);
      break;
    case Link:
      rrect(cr, 2.5, 8, 11, 8, 4);
      rrect(cr, 10.5, 8, 11, 8, 4);
      cairo_stroke(cr);
      break;
    case ClearFormat:
      cairo_move_to(cr, 4, 18);
      cairo_line_to(cr, 9, 5);
      cairo_line_to(cr, 14, 18);
      line(cr, 6, 13.5, 12, 13.5);
      line(cr, 15, 13, 21, 19);
      line(cr, 21, 13, 15, 19);
      cairo_stroke(cr);
      break;
    case ListBullet:
    case ListNumbered:
    case ListTask:
      for (int i = 0; i < 3; ++i) {
        double yy = 6 + i * 6;
        line(cr, 9.5, yy, 20.5, yy);
      }
      cairo_stroke(cr);
      for (int i = 0; i < 3; ++i) {
        double yy = 6 + i * 6;
        if (kind == ListBullet) {
          cairo_new_sub_path(cr);
          cairo_arc(cr, 4.8, yy, 1.4, 0, 2 * M_PI);
          cairo_fill(cr);
        } else if (kind == ListTask) {
          rrect(cr, 3, yy - 2, 4, 4, 0.8);
          cairo_set_line_width(cr, 1.3);
          cairo_stroke(cr);
          cairo_set_line_width(cr, size >= 20 ? 1.8 : 2.0);
        } else {
          line(cr, 5, yy - 2, 5, yy + 2);
          cairo_set_line_width(cr, 1.4);
          cairo_stroke(cr);
          cairo_set_line_width(cr, size >= 20 ? 1.8 : 2.0);
        }
      }
      break;
    case Quote:
      line(cr, 5, 5, 5, 19);
      line(cr, 9.5, 8, 19.5, 8);
      line(cr, 9.5, 12, 19.5, 12);
      line(cr, 9.5, 16, 16, 16);
      cairo_stroke(cr);
      break;
    case Eye:
      cairo_move_to(cr, 2.5, 12);
      cairo_curve_to(cr, 7, 4.5, 17, 4.5, 21.5, 12);
      cairo_curve_to(cr, 17, 19.5, 7, 19.5, 2.5, 12);
      cairo_close_path(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 3.2, 0, 2 * M_PI);
      cairo_stroke(cr);
      break;
    case Check:
      cairo_move_to(cr, 5, 12.5);
      cairo_line_to(cr, 10, 17.5);
      cairo_line_to(cr, 19.5, 7);
      cairo_stroke(cr);
      break;
    case Pilcrow:
      line(cr, 12, 4.5, 12, 19.5);
      line(cr, 16, 4.5, 16, 19.5);
      line(cr, 9.5, 4.5, 19, 4.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 12, 4.5);
      cairo_curve_to(cr, 5.5, 4.5, 5.5, 12.5, 12, 12.5);
      cairo_close_path(cr);
      cairo_fill(cr);
      break;
    case Font:
      cairo_move_to(cr, 4, 19);
      cairo_line_to(cr, 10, 4.5);
      cairo_line_to(cr, 16, 19);
      line(cr, 6.3, 13.5, 13.7, 13.5);
      line(cr, 18, 10, 18, 19);
      cairo_stroke(cr);
      break;
    case Theme:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 8.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 12, 3.5);
      cairo_arc(cr, 12, 12, 8.5, -M_PI / 2, M_PI / 2);
      cairo_close_path(cr);
      cairo_fill(cr);
      break;
    case Window:
      rrect(cr, 3, 4.5, 18, 15, 2.5);
      line(cr, 3, 8.5, 21, 8.5);
      cairo_stroke(cr);
      break;
    case Clock:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 8.5, 0, 2 * M_PI);
      cairo_move_to(cr, 12, 7);
      cairo_line_to(cr, 12, 12);
      cairo_line_to(cr, 15.5, 14);
      cairo_stroke(cr);
      break;
    case Heading:
      line(cr, 5, 5, 5, 19);
      line(cr, 15, 5, 15, 19);
      line(cr, 5, 12, 15, 12);
      line(cr, 18.5, 11, 20, 9.5);
      line(cr, 20, 9.5, 20, 19);
      cairo_stroke(cr);
      break;
    case Wrench:
      cairo_move_to(cr, 14.5, 3.5);
      cairo_curve_to(cr, 11, 3.5, 9, 7, 10.3, 10.2);
      cairo_line_to(cr, 3.8, 16.7);
      cairo_curve_to(cr, 2.8, 17.7, 2.8, 19.3, 3.8, 20.2);
      cairo_curve_to(cr, 4.7, 21.2, 6.3, 21.2, 7.3, 20.2);
      cairo_line_to(cr, 13.8, 13.7);
      cairo_curve_to(cr, 17, 15, 20.5, 13, 20.5, 9.5);
      cairo_line_to(cr, 17, 11.5);
      cairo_line_to(cr, 13.5, 10.5);
      cairo_line_to(cr, 12.5, 7);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Undo:
    case Redo:
      arrow_curve(cr, kind == Redo);
      cairo_stroke(cr);
      break;
    case Keyboard:
      // A keyboard: rows of keys and the space bar.
      rrect(cr, 2.5, 6, 19, 12.5, 2.5);
      cairo_stroke(cr);
      for (double kx : {6.5, 10.0, 13.5, 17.5}) line(cr, kx, 9.6, kx + 0.01, 9.6);
      for (double kx : {8.3, 11.8, 15.5}) line(cr, kx, 12.4, kx + 0.01, 12.4);
      line(cr, 8, 15.2, 16, 15.2);
      cairo_stroke(cr);
      break;
  }
  cairo_restore(cr);
}

// The Niny mark: a folded sheet with a code caret, on an accent gradient.
void logo(Painter &p, double x, double y, double s) {
  cairo_t *cr = p.cr();
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + s, y + s);
  cairo_pattern_add_color_stop_rgb(g, 0, 0xFF / 255.0, 0x8A / 255.0, 0x5B / 255.0);
  cairo_pattern_add_color_stop_rgb(g, 0.55, 0xE8 / 255.0, 0x4A / 255.0, 0x7A / 255.0);
  cairo_pattern_add_color_stop_rgb(g, 1, 0x8E / 255.0, 0x3B / 255.0, 0xC9 / 255.0);
  p.rounded_rect(x, y, s, s, s * 0.27);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  // Sheet.
  double px = x + s * 0.26, py = y + s * 0.18, pw = s * 0.48, ph = s * 0.64, fold = s * 0.14;
  cairo_move_to(cr, px, py);
  cairo_line_to(cr, px + pw - fold, py);
  cairo_line_to(cr, px + pw, py + fold);
  cairo_line_to(cr, px + pw, py + ph);
  cairo_line_to(cr, px, py + ph);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
  cairo_fill(cr);
  // Lines of "text": an N-like stroke and two rules.
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_width(cr, s * 0.065);
  cairo_set_source_rgb(cr, 0xC8 / 255.0, 0x3E / 255.0, 0x86 / 255.0);
  cairo_move_to(cr, px + pw * 0.24, py + ph * 0.62);
  cairo_line_to(cr, px + pw * 0.24, py + ph * 0.30);
  cairo_line_to(cr, px + pw * 0.70, py + ph * 0.62);
  cairo_line_to(cr, px + pw * 0.70, py + ph * 0.30);
  cairo_stroke(cr);
  cairo_set_line_width(cr, s * 0.045);
  cairo_set_source_rgba(cr, 0x8E / 255.0, 0x3B / 255.0, 0xC9 / 255.0, 0.55);
  cairo_move_to(cr, px + pw * 0.22, py + ph * 0.80);
  cairo_line_to(cr, px + pw * 0.78, py + ph * 0.80);
  cairo_stroke(cr);
}

}  // namespace icon
}  // namespace niny
