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
    case Drive:
      rrect(cr, 2.5, 7.5, 19, 10, 2.5);
      line(cr, 6, 14, 12, 14);
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 17.5, 12.5, 1.4, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Star:
      for (int i = 0; i < 10; ++i) {
        double a = -M_PI / 2 + i * M_PI / 5, r = i % 2 ? 4 : 9;
        double px = 12 + r * cos(a), py = 12.8 + r * sin(a);
        if (i == 0) cairo_move_to(cr, px, py);
        else cairo_line_to(cr, px, py);
      }
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Down:
      cairo_move_to(cr, 6.5, 9.5);
      cairo_line_to(cr, 12, 15);
      cairo_line_to(cr, 17.5, 9.5);
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

// ------------------------------------------------------- colour pictures

namespace {

using sde::Color;

void vgrad(cairo_t *cr, double y0, double y1, const Color &a, const Color &b) {
  cairo_pattern_t *pat = cairo_pattern_create_linear(0, y0, 0, y1);
  cairo_pattern_add_color_stop_rgba(pat, 0, a.r, a.g, a.b, a.a);
  cairo_pattern_add_color_stop_rgba(pat, 1, b.r, b.g, b.b, b.a);
  cairo_set_source(cr, pat);
  cairo_pattern_destroy(pat);
}

void set(cairo_t *cr, const Color &c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

// A parcel: lid, body and a band down the middle.
void parcel(cairo_t *cr, double x, double y, double w, double h, const Color &light, const Color &dark_c,
            const Color &band) {
  double lid = h * 0.28;
  rrect(cr, x + 1, y + lid * 0.6, w - 2, h - lid * 0.6, 2.2);
  vgrad(cr, y, y + h, light, dark_c);
  cairo_fill(cr);
  rrect(cr, x, y, w, lid, 1.8);
  vgrad(cr, y, y + lid, light.mix(Color::hex(0xFFFFFF), 0.25), light);
  cairo_fill(cr);
  // Shade under the lid.
  set(cr, dark_c.mix(Color::hex(0x000000), 0.2).alpha(0.35));
  cairo_rectangle(cr, x + 1, y + lid, w - 2, 1.2);
  cairo_fill(cr);
  // Band.
  double bw = w * 0.2;
  set(cr, band);
  cairo_rectangle(cr, x + (w - bw) / 2, y, bw, h);
  cairo_fill(cr);
  set(cr, Color::hex(0xFFFFFF, 0.35));
  cairo_rectangle(cr, x + (w - bw) / 2, y, bw * 0.35, h);
  cairo_fill(cr);
}

void sparkle(cairo_t *cr, double cx, double cy, double r, const Color &c) {
  cairo_move_to(cr, cx, cy - r);
  cairo_curve_to(cr, cx + r * 0.15, cy - r * 0.15, cx + r * 0.15, cy - r * 0.15, cx + r, cy);
  cairo_curve_to(cr, cx + r * 0.15, cy + r * 0.15, cx + r * 0.15, cy + r * 0.15, cx, cy + r);
  cairo_curve_to(cr, cx - r * 0.15, cy + r * 0.15, cx - r * 0.15, cy + r * 0.15, cx - r, cy);
  cairo_curve_to(cr, cx - r * 0.15, cy - r * 0.15, cx - r * 0.15, cy - r * 0.15, cx, cy - r);
  cairo_close_path(cr);
  set(cr, c);
  cairo_fill(cr);
}

void folder_pic(cairo_t *cr, const Color &back, const Color &front_top, const Color &front_bottom) {
  // Back with a tab.
  cairo_move_to(cr, 3, 9);
  cairo_curve_to(cr, 3, 7.3, 3.8, 6.5, 5.5, 6.5);
  cairo_line_to(cr, 12, 6.5);
  cairo_line_to(cr, 14.5, 9);
  cairo_line_to(cr, 26.5, 9);
  cairo_curve_to(cr, 28.2, 9, 29, 9.8, 29, 11.5);
  cairo_line_to(cr, 29, 25);
  cairo_curve_to(cr, 29, 26.7, 28.2, 27.5, 26.5, 27.5);
  cairo_line_to(cr, 5.5, 27.5);
  cairo_curve_to(cr, 3.8, 27.5, 3, 26.7, 3, 25);
  cairo_close_path(cr);
  set(cr, back);
  cairo_fill(cr);
  rrect(cr, 3, 12, 26, 15.5, 2.5);
  vgrad(cr, 12, 27.5, front_top, front_bottom);
  cairo_fill(cr);
  set(cr, Color::hex(0xFFFFFF, 0.35));
  cairo_rectangle(cr, 4.5, 12.6, 23, 1);
  cairo_fill(cr);
}

}  // namespace

void tool(sde::Painter &p, Tool kind, double x, double y, double size, bool dark, bool enabled) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  if (!enabled) cairo_push_group(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 32.0, size / 32.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  Color white = Color::hex(0xFFFFFF);
  switch (kind) {
    case TAdd: {
      parcel(cr, 3, 6, 22, 21, Color::hex(0xFFC94D), Color::hex(0xEE8E1C), Color::hex(0xC9761A, 0.55));
      cairo_new_sub_path(cr);
      cairo_arc(cr, 24, 23.5, 7, 0, 2 * M_PI);
      set(cr, dark ? Color::hex(0x1F2630) : white);
      cairo_fill(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 24, 23.5, 5.8, 0, 2 * M_PI);
      vgrad(cr, 17.7, 29.3, Color::hex(0x47D17F), Color::hex(0x1E9E52));
      cairo_fill(cr);
      cairo_set_line_width(cr, 2.2);
      set(cr, white);
      line(cr, 24, 20.4, 24, 26.6);
      line(cr, 20.9, 23.5, 27.1, 23.5);
      cairo_stroke(cr);
      break;
    }
    case TExtract: {
      folder_pic(cr, Color::hex(0x2F7FD8), Color::hex(0x8CC8FA), Color::hex(0x3D8EE6));
      // Arrow leaving the folder.
      cairo_move_to(cr, 16, 2.5);
      cairo_line_to(cr, 23, 10);
      cairo_line_to(cr, 19, 10);
      cairo_line_to(cr, 19, 20);
      cairo_line_to(cr, 13, 20);
      cairo_line_to(cr, 13, 10);
      cairo_line_to(cr, 9, 10);
      cairo_close_path(cr);
      vgrad(cr, 2.5, 20, Color::hex(0xFFA04D), Color::hex(0xF0601F));
      cairo_fill_preserve(cr);
      set(cr, dark ? Color::hex(0x1F2630, 0.8) : white);
      cairo_set_line_width(cr, 1.2);
      cairo_stroke(cr);
      break;
    }
    case TTest: {
      cairo_move_to(cr, 16, 2.5);
      cairo_curve_to(cr, 19.5, 4.5, 23, 5.5, 27, 5.8);
      cairo_curve_to(cr, 27, 17, 23.5, 25, 16, 29.5);
      cairo_curve_to(cr, 8.5, 25, 5, 17, 5, 5.8);
      cairo_curve_to(cr, 9, 5.5, 12.5, 4.5, 16, 2.5);
      cairo_close_path(cr);
      vgrad(cr, 2.5, 29.5, Color::hex(0x52D98A), Color::hex(0x179650));
      cairo_fill(cr);
      // Left half highlight.
      cairo_move_to(cr, 16, 4.6);
      cairo_curve_to(cr, 13, 6.2, 10, 7.1, 7, 7.5);
      cairo_curve_to(cr, 7.3, 16.5, 10.3, 23.2, 16, 27.2);
      cairo_close_path(cr);
      set(cr, Color::hex(0xFFFFFF, 0.18));
      cairo_fill(cr);
      cairo_set_line_width(cr, 3);
      set(cr, white);
      cairo_move_to(cr, 10.5, 16);
      cairo_line_to(cr, 14.5, 20);
      cairo_line_to(cr, 22, 11.5);
      cairo_stroke(cr);
      break;
    }
    case TView: {
      // A page...
      cairo_move_to(cr, 8, 3);
      cairo_line_to(cr, 20, 3);
      cairo_line_to(cr, 26, 9);
      cairo_line_to(cr, 26, 27);
      cairo_curve_to(cr, 26, 28.3, 25.3, 29, 24, 29);
      cairo_line_to(cr, 8, 29);
      cairo_curve_to(cr, 6.7, 29, 6, 28.3, 6, 27);
      cairo_line_to(cr, 6, 5);
      cairo_curve_to(cr, 6, 3.7, 6.7, 3, 8, 3);
      cairo_close_path(cr);
      set(cr, dark ? Color::hex(0xE8ECF2) : white);
      cairo_fill_preserve(cr);
      set(cr, Color::hex(0xAEB8C6));
      cairo_set_line_width(cr, 1);
      cairo_stroke(cr);
      cairo_move_to(cr, 20, 3);
      cairo_line_to(cr, 20, 9);
      cairo_line_to(cr, 26, 9);
      cairo_close_path(cr);
      set(cr, Color::hex(0xC9B6F5));
      cairo_fill(cr);
      set(cr, Color::hex(0xB9C2CE));
      cairo_set_line_width(cr, 1.6);
      line(cr, 10, 8, 17, 8);
      line(cr, 10, 11.5, 22, 11.5);
      cairo_stroke(cr);
      // ... with an eye.
      cairo_move_to(cr, 3.5, 20);
      cairo_curve_to(cr, 8, 13, 24, 13, 28.5, 20);
      cairo_curve_to(cr, 24, 27, 8, 27, 3.5, 20);
      cairo_close_path(cr);
      set(cr, Color::hex(0xF3EDFF));
      cairo_fill_preserve(cr);
      set(cr, Color::hex(0x7B4FD6));
      cairo_set_line_width(cr, 1.6);
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 16, 20, 4.6, 0, 2 * M_PI);
      vgrad(cr, 15.4, 24.6, Color::hex(0xA47BFF), Color::hex(0x5E35C4));
      cairo_fill(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 16, 20, 1.9, 0, 2 * M_PI);
      set(cr, Color::hex(0x24124F));
      cairo_fill(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 17.6, 18.4, 1.1, 0, 2 * M_PI);
      set(cr, white);
      cairo_fill(cr);
      break;
    }
    case TDelete: {
      rrect(cr, 12.5, 2.5, 7, 4, 1.5);
      set(cr, Color::hex(0xB8322A));
      cairo_set_line_width(cr, 1.8);
      cairo_stroke(cr);
      rrect(cr, 5, 5.5, 22, 4, 1.6);
      vgrad(cr, 5.5, 9.5, Color::hex(0xFF7A6E), Color::hex(0xE0453B));
      cairo_fill(cr);
      cairo_move_to(cr, 7, 11);
      cairo_line_to(cr, 25, 11);
      cairo_line_to(cr, 23.4, 27.2);
      cairo_curve_to(cr, 23.2, 28.5, 22.5, 29.5, 21, 29.5);
      cairo_line_to(cr, 11, 29.5);
      cairo_curve_to(cr, 9.5, 29.5, 8.8, 28.5, 8.6, 27.2);
      cairo_close_path(cr);
      vgrad(cr, 11, 29.5, Color::hex(0xFF6B5E), Color::hex(0xC9302A));
      cairo_fill(cr);
      cairo_set_line_width(cr, 1.6);
      set(cr, Color::hex(0xFFFFFF, 0.55));
      line(cr, 12, 14.5, 12.6, 25.5);
      line(cr, 16, 14.5, 16, 25.5);
      line(cr, 20, 14.5, 19.4, 25.5);
      cairo_stroke(cr);
      break;
    }
    case TFind: {
      cairo_set_line_width(cr, 5);
      set(cr, Color::hex(0x4A5363));
      line(cr, 20.5, 20.5, 27.5, 27.5);
      cairo_stroke(cr);
      cairo_set_line_width(cr, 3);
      set(cr, Color::hex(0xF5B342));
      line(cr, 22.5, 22.5, 27, 27);
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 13.5, 13.5, 9.5, 0, 2 * M_PI);
      vgrad(cr, 4, 23, Color::hex(0xA8E0FF, 0.95), Color::hex(0x3C9BEA, 0.95));
      cairo_fill_preserve(cr);
      set(cr, Color::hex(0x7C8797));
      cairo_set_line_width(cr, 2.6);
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 13.5, 13.5, 6, M_PI * 1.05, M_PI * 1.5);
      set(cr, Color::hex(0xFFFFFF, 0.85));
      cairo_set_line_width(cr, 2);
      cairo_stroke(cr);
      break;
    }
    case TWizard: {
      cairo_set_line_width(cr, 4);
      set(cr, Color::hex(0x3A3F4B));
      line(cr, 5, 28, 19.5, 13.5);
      cairo_stroke(cr);
      set(cr, dark ? Color::hex(0xF2F4F8) : Color::hex(0xFFFFFF));
      line(cr, 19.5, 13.5, 22.5, 10.5);
      cairo_stroke(cr);
      cairo_set_line_width(cr, 1);
      set(cr, Color::hex(0x3A3F4B, 0.6));
      line(cr, 19.5, 13.5, 22.5, 10.5);
      cairo_stroke(cr);
      sparkle(cr, 25, 6.5, 5.5, Color::hex(0xFFC93C));
      sparkle(cr, 12.5, 7.5, 3.6, Color::hex(0xFF6FA8));
      sparkle(cr, 27, 18, 3.4, Color::hex(0x3CC8E8));
      sparkle(cr, 19, 3.5, 2, Color::hex(0x8F6BFF));
      break;
    }
    case TInfo: {
      cairo_new_sub_path(cr);
      cairo_arc(cr, 16, 16, 13, 0, 2 * M_PI);
      vgrad(cr, 3, 29, Color::hex(0x5DB0FA), Color::hex(0x1D63C6));
      cairo_fill(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 16, 16, 11.5, M_PI * 1.05, M_PI * 1.85);
      set(cr, Color::hex(0xFFFFFF, 0.3));
      cairo_set_line_width(cr, 1.4);
      cairo_stroke(cr);
      cairo_new_sub_path(cr);
      cairo_arc(cr, 16, 9.5, 2.3, 0, 2 * M_PI);
      set(cr, white);
      cairo_fill(cr);
      rrect(cr, 13.9, 13.5, 4.2, 11.5, 1.6);
      cairo_fill(cr);
      break;
    }
    case TRepair: {
      parcel(cr, 3, 5, 26, 23, Color::hex(0x4FD6B4), Color::hex(0x179C80), Color::hex(0x0F7A64, 0.45));
      // A plaster across the box.
      cairo_save(cr);
      cairo_translate(cr, 16, 18);
      cairo_rotate(cr, -0.65);
      rrect(cr, -12.5, -4, 25, 8, 4);
      vgrad(cr, -4, 4, Color::hex(0xFAD7B5), Color::hex(0xE6A877));
      cairo_fill_preserve(cr);
      set(cr, Color::hex(0xB97B4C, 0.7));
      cairo_set_line_width(cr, 0.8);
      cairo_stroke(cr);
      rrect(cr, -4, -3.2, 8, 6.4, 1.2);
      set(cr, Color::hex(0xFFF1E3));
      cairo_fill(cr);
      set(cr, Color::hex(0xC88A5A));
      for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
          cairo_new_sub_path(cr);
          cairo_arc(cr, (i ? 8 : -8) + (j ? 1.4 : -1.4), j ? 1.2 : -1.2, 0.6, 0, 2 * M_PI);
          cairo_fill(cr);
        }
      cairo_restore(cr);
      break;
    }
    case ToolCount: break;
  }
  if (!enabled) {
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.32);
  }
  cairo_restore(cr);
}

void archive(sde::Painter &p, double x, double y, double size, const sde::Color &accent, bool dark) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 32.0, size / 32.0);
  (void)accent;
  (void)dark;
  parcel(cr, 3, 4, 26, 25, Color::hex(0xB48CF2), Color::hex(0x6F3FCB), Color::hex(0xFFC93C, 0.95));
  // Zip teeth on the band.
  set(cr, Color::hex(0x8A5A10, 0.75));
  for (int i = 0; i < 6; ++i) cairo_rectangle(cr, i % 2 ? 16 : 14.4, 6 + i * 3.6, 1.6, 1.4);
  cairo_fill(cr);
  cairo_restore(cr);
}

void parent_folder(sde::Painter &p, double x, double y, double size, const sde::Color &accent, bool dark) {
  file(p, "", x, y, size, true, accent, dark);
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, size / 24.0, size / 24.0);
  cairo_set_line_width(cr, 2);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  set(cr, Color::hex(0xFFFFFF, 0.95));
  line(cr, 12, 19, 12, 11.5);
  cairo_move_to(cr, 9, 14.5);
  cairo_line_to(cr, 12, 11.5);
  cairo_line_to(cr, 15, 14.5);
  cairo_stroke(cr);
  cairo_restore(cr);
}

}  // namespace icon
}  // namespace taca
