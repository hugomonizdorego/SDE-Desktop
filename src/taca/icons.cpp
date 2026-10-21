// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Taca's line icons and file cards, drawn with cairo on a 24-unit grid so
// they stay sharp at every UI scale and follow the theme colors.
#include <cmath>

#include "taca.h"

namespace taca {
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

void box_shape(cairo_t *cr) {
  // An open parcel: the archive symbol of Taca.
  cairo_move_to(cr, 3.5, 8);
  cairo_line_to(cr, 12, 4);
  cairo_line_to(cr, 20.5, 8);
  cairo_line_to(cr, 20.5, 17);
  cairo_line_to(cr, 12, 21);
  cairo_line_to(cr, 3.5, 17);
  cairo_close_path(cr);
  line(cr, 3.5, 8, 12, 12);
  line(cr, 20.5, 8, 12, 12);
  line(cr, 12, 12, 12, 21);
  line(cr, 7.8, 6, 16.2, 10);
}

void folder_shape(cairo_t *cr) {
  cairo_move_to(cr, 3, 7);
  cairo_line_to(cr, 3, 18);
  cairo_curve_to(cr, 3, 19, 3.5, 19.5, 4.5, 19.5);
  cairo_line_to(cr, 19.5, 19.5);
  cairo_curve_to(cr, 20.5, 19.5, 21, 19, 21, 18);
  cairo_line_to(cr, 21, 9.5);
  cairo_curve_to(cr, 21, 8.5, 20.5, 8, 19.5, 8);
  cairo_line_to(cr, 12, 8);
  cairo_line_to(cr, 10, 5.5);
  cairo_line_to(cr, 4.5, 5.5);
  cairo_curve_to(cr, 3.5, 5.5, 3, 6, 3, 7);
  cairo_close_path(cr);
}

}  // namespace

void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 24.0, size / 24.0);
  cairo_set_line_width(cr, size >= 20 ? 1.7 : 2.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  p.set(c);
  switch (kind) {
    case Box:
      box_shape(cr);
      cairo_stroke(cr);
      break;
    case Open:
      folder_shape(cr);
      cairo_stroke(cr);
      line(cr, 12, 17, 12, 11);
      line(cr, 9.5, 13.5, 12, 11);
      line(cr, 14.5, 13.5, 12, 11);
      cairo_stroke(cr);
      break;
    case New:
      box_shape(cr);
      cairo_stroke(cr);
      break;
    case Extract:
      // Arrow out of a tray.
      line(cr, 12, 3.5, 12, 14);
      line(cr, 7.5, 9.5, 12, 14);
      line(cr, 16.5, 9.5, 12, 14);
      cairo_move_to(cr, 4, 14.5);
      cairo_line_to(cr, 4, 19);
      cairo_curve_to(cr, 4, 19.8, 4.5, 20.5, 5.5, 20.5);
      cairo_line_to(cr, 18.5, 20.5);
      cairo_curve_to(cr, 19.5, 20.5, 20, 19.8, 20, 19);
      cairo_line_to(cr, 20, 14.5);
      cairo_stroke(cr);
      break;
    case Add:
      rrect(cr, 5, 3, 14, 18, 2.5);
      line(cr, 12, 8.5, 12, 15.5);
      line(cr, 8.5, 12, 15.5, 12);
      cairo_stroke(cr);
      break;
    case AddFolder:
      folder_shape(cr);
      line(cr, 12, 11, 12, 17);
      line(cr, 9, 14, 15, 14);
      cairo_stroke(cr);
      break;
    case Trash:
      line(cr, 4, 6.5, 20, 6.5);
      cairo_move_to(cr, 9, 6.5);
      cairo_line_to(cr, 9.5, 4);
      cairo_line_to(cr, 14.5, 4);
      cairo_line_to(cr, 15, 6.5);
      cairo_move_to(cr, 6, 6.5);
      cairo_line_to(cr, 7, 20);
      cairo_line_to(cr, 17, 20);
      cairo_line_to(cr, 18, 6.5);
      line(cr, 10, 10, 10.3, 16.5);
      line(cr, 14, 10, 13.7, 16.5);
      cairo_stroke(cr);
      break;
    case Rename:
      cairo_move_to(cr, 4, 20);
      cairo_line_to(cr, 5, 15.5);
      cairo_line_to(cr, 15.5, 5);
      cairo_line_to(cr, 19, 8.5);
      cairo_line_to(cr, 8.5, 19);
      cairo_close_path(cr);
      line(cr, 13, 7.5, 16.5, 11);
      cairo_stroke(cr);
      break;
    case Shield:
      cairo_move_to(cr, 12, 3);
      cairo_line_to(cr, 19.5, 6);
      cairo_curve_to(cr, 19.5, 13, 17, 18, 12, 21);
      cairo_curve_to(cr, 7, 18, 4.5, 13, 4.5, 6);
      cairo_close_path(cr);
      cairo_move_to(cr, 8.5, 12);
      cairo_line_to(cr, 11, 14.5);
      cairo_line_to(cr, 15.5, 9.5);
      cairo_stroke(cr);
      break;
    case Info:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 12, 9, 0, 2 * M_PI);
      line(cr, 12, 11, 12, 16.5);
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 7.8, 1.2, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Search:
      cairo_new_sub_path(cr);
      cairo_arc(cr, 10.5, 10.5, 6.5, 0, 2 * M_PI);
      line(cr, 15.3, 15.3, 20.5, 20.5);
      cairo_stroke(cr);
      break;
    case List:
      for (int i = 0; i < 3; ++i) {
        double yy = 6.5 + i * 5.5;
        line(cr, 9, yy, 20, yy);
        cairo_stroke(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, 4.8, yy, 1.3, 0, 2 * M_PI);
        cairo_fill(cr);
      }
      break;
    case Grid:
      rrect(cr, 3.5, 3.5, 7, 7, 1.8);
      rrect(cr, 13.5, 3.5, 7, 7, 1.8);
      rrect(cr, 3.5, 13.5, 7, 7, 1.8);
      rrect(cr, 13.5, 13.5, 7, 7, 1.8);
      cairo_stroke(cr);
      break;
    case Home:
      box_shape(cr);
      cairo_stroke(cr);
      break;
    case Chevron:
      cairo_move_to(cr, 9.5, 6);
      cairo_line_to(cr, 15.5, 12);
      cairo_line_to(cr, 9.5, 18);
      cairo_stroke(cr);
      break;
    case Lock:
      rrect(cr, 5, 10.5, 14, 10, 2.5);
      cairo_move_to(cr, 8, 10.5);
      cairo_line_to(cr, 8, 7.5);
      cairo_curve_to(cr, 8, 2.5, 16, 2.5, 16, 7.5);
      cairo_line_to(cr, 16, 10.5);
      cairo_stroke(cr);
      break;
    case Close:
      line(cr, 6, 6, 18, 18);
      line(cr, 18, 6, 6, 18);
      cairo_stroke(cr);
      break;
    case Folder:
      folder_shape(cr);
      cairo_stroke(cr);
      break;
    case Check:
      cairo_move_to(cr, 5, 12.5);
      cairo_line_to(cr, 10, 17.5);
      cairo_line_to(cr, 19, 7);
      cairo_stroke(cr);
      break;
    case Warning:
      cairo_move_to(cr, 12, 3.5);
      cairo_line_to(cr, 21, 19.5);
      cairo_line_to(cr, 3, 19.5);
      cairo_close_path(cr);
      line(cr, 12, 9.5, 12, 14);
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 12, 16.8, 1.1, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Up:
      line(cr, 12, 19, 12, 5);
      cairo_move_to(cr, 6.5, 10.5);
      cairo_line_to(cr, 12, 5);
      cairo_line_to(cr, 17.5, 10.5);
      cairo_stroke(cr);
      break;
    case Plus:
      line(cr, 12, 5, 12, 19);
      line(cr, 5, 12, 19, 12);
      cairo_stroke(cr);
      break;
  }
  cairo_restore(cr);
}

sde::Color category_color(int category, const sde::Color &accent) {
  switch (category) {
    case 1: return sde::Color::hex(0x6B7A8F);  // text
    case 2: return sde::Color::hex(0x2E9E6B);  // image
    case 3: return sde::Color::hex(0xD9822B);  // audio
    case 4: return sde::Color::hex(0xC2457A);  // video
    case 5: return sde::Color::hex(0x8A5CD6);  // archive
    case 6: return sde::Color::hex(0x2B8FBF);  // code
    case 7: return sde::Color::hex(0xD14B3C);  // document
    case 8: return sde::Color::hex(0x5A6B2E);  // program
    default: return accent;
  }
}

void file(sde::Painter &p, const std::string &name, double x, double y, double size, bool dir,
          const sde::Color &accent, bool dark) {
  cairo_t *cr = p.cr();
  if (dir) {
    // A soft folder tile in the accent color.
    double w = size, h = size * 0.78, top = y + size * 0.14;
    sde::Color back = accent.mix(dark ? sde::Color::hex(0x000000) : sde::Color::hex(0xFFFFFF), dark ? 0.35 : 0.25);
    cairo_save(cr);
    cairo_new_sub_path(cr);
    double r = size * 0.08;
    cairo_move_to(cr, x + r, top);
    cairo_line_to(cr, x + w * 0.38, top);
    cairo_line_to(cr, x + w * 0.48, top + h * 0.12);
    cairo_line_to(cr, x + w - r, top + h * 0.12);
    cairo_arc(cr, x + w - r, top + h * 0.12 + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, top + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, top + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, top + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    p.set(back);
    cairo_fill(cr);
    p.fill_gradient(x, top + h * 0.26, w, h * 0.74, r, accent.mix(sde::Color::hex(0xFFFFFF), 0.12), accent);
    cairo_restore(cr);
    return;
  }
  double w = size * 0.76, h = size, fx = x + (size - w) / 2, r = size * 0.08, fold = size * 0.24;
  sde::Color paper = dark ? sde::Color::hex(0x2B3038) : sde::Color::hex(0xFFFFFF);
  sde::Color edge = dark ? sde::Color::hex(0x495260) : sde::Color::hex(0xC9D0DA);
  sde::Color tint = category_color(file_category(name), accent);
  cairo_save(cr);
  cairo_new_sub_path(cr);
  cairo_move_to(cr, fx + r, y);
  cairo_line_to(cr, fx + w - fold, y);
  cairo_line_to(cr, fx + w, y + fold);
  cairo_arc(cr, fx + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, fx + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, fx + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
  p.set(paper);
  cairo_fill_preserve(cr);
  p.set(edge);
  cairo_set_line_width(cr, std::max(1.0, size / 48));
  cairo_stroke(cr);
  // Folded corner in the category tint.
  cairo_move_to(cr, fx + w - fold, y);
  cairo_line_to(cr, fx + w - fold, y + fold - r * 0.6);
  cairo_curve_to(cr, fx + w - fold, y + fold, fx + w - fold, y + fold, fx + w - fold + r * 0.6, y + fold);
  cairo_line_to(cr, fx + w, y + fold);
  cairo_close_path(cr);
  p.set(tint.alpha(0.85));
  cairo_fill(cr);
  // Label band.
  if (!extension_label(name).empty() && size >= 28) {
    double bh = size * 0.22;
    p.fill_rounded(fx + w * 0.12, y + h * 0.58, w * 0.76, bh, bh * 0.3, tint);
  } else {
    for (int i = 0; i < 3; ++i)
      p.fill_rect(fx + w * 0.2, y + h * (0.48 + i * 0.13), w * (i == 2 ? 0.4 : 0.6), std::max(1.0, size / 22),
                  tint.alpha(0.6));
  }
  cairo_restore(cr);
}

void file_label_box(const std::string &name, double x, double y, double size, int &lx, int &ly, int &lw, int &lh) {
  double w = size * 0.76, fx = x + (size - w) / 2;
  lx = int(fx + w * 0.12);
  ly = int(y + size * 0.58);
  lw = int(w * 0.76);
  lh = int(size * 0.22);
  if (extension_label(name).empty() || size < 28) lw = 0;
}

void badge(sde::Painter &p, double x, double y, double size, const sde::Color &accent) {
  sde::Color top = accent.mix(sde::Color::hex(0xFFFFFF), 0.18);
  sde::Color bottom = accent.mix(sde::Color::hex(0x000000), 0.18);
  p.shadow(x, y, size, size, size * 0.24, sde::Color::hex(0x000000).alpha(0.18), int(size / 10));
  p.fill_gradient(x, y, size, size, size * 0.24, top, bottom, true);
  p.glass_sheen(x, y, size, size, size * 0.24, false);
  draw(p, Box, x + size * 0.2, y + size * 0.14, size * 0.6, sde::Color::hex(0xFFFFFF).alpha(0.95));
}

}  // namespace icon
}  // namespace taca
