// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// QA helper (not installed): FLTK's check and round buttons in the SDE look
// (SDE-FLTK with SDE's soft boxes, as in the file chooser and the stock
// dialogs), unchecked and checked, read back from the screen and written as
// RGBA PAM images for tests/test_theme_motion.py:
// OUTDIR/fltk-{check,radio}-{off,on}.pam
//
//   sde-fltk-indicator-probe OUTDIR FAMILY MODE SCALE
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <cstdio>
#include <string>

#include "config.h"
#include "menu.h"
#include "theme.h"

using namespace sde;

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s OUTDIR FAMILY MODE SCALE\n", argv[0]);
    return 2;
  }
  fl_open_display();
  Config cfg;
  cfg.set("appearance", "style", argv[2]);
  cfg.set("appearance", "theme", argv[3]);
  cfg.set("appearance", "scale", argv[4]);
  Theme t = Theme::from_config(cfg);
  t.apply_to_fltk();
  Fl::visible_focus(0);  // no focus box around the first button
  int bw = S(48), bh = S(32);
  Fl_Window win(4 * bw, bh, "indicators");
  Fl_Check_Button check_off(0, 0, bw, bh), check_on(bw, 0, bw, bh);
  Fl_Round_Button radio_off(2 * bw, 0, bw, bh), radio_on(3 * bw, 0, bw, bh);
  check_on.value(1);
  radio_on.value(1);
  win.end();
  apply_stock_style(&win);
  win.show();
  for (int i = 0; i < 40; ++i) Fl::wait(0.02);
  win.redraw();
  Fl::flush();
  for (int i = 0; i < 20; ++i) Fl::wait(0.02);
  struct Shot {
    Fl_Widget *w;
    const char *name;
  } shots[] = {{&check_off, "fltk-check-off"}, {&check_on, "fltk-check-on"}, {&radio_off, "fltk-radio-off"},
               {&radio_on, "fltk-radio-on"}};
  win.make_current();
  for (const auto &s : shots) {
    uchar *rgb = fl_read_image(nullptr, s.w->x(), s.w->y(), s.w->w(), s.w->h());
    if (!rgb) return 1;
    std::string path = std::string(argv[1]) + "/" + s.name + ".pam";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return 1;
    fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", s.w->w(), s.w->h());
    for (int i = 0; i < s.w->w() * s.w->h(); ++i) {
      uchar px[4] = {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2], 255};
      fwrite(px, 1, 4, f);
    }
    fclose(f);
    delete[] rgb;
  }
  return 0;
}
