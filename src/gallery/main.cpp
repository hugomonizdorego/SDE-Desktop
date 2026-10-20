// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// sde-gallery: browse the photos in your Pictures folder and other folders.
#include <FL/Fl.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/x.H>
#include <X11/Xutil.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "build_config.h"
#include "config.h"
#include "gallery.h"
#include "i18n.h"
#include "theme.h"

using namespace sde;

int main(int argc, char **argv) {
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) {
      printf("sde-gallery %s\n", SDE_VERSION);
      return 0;
    }
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      printf("%s", _("Usage: sde-gallery [--version | --help]\n\n"
                     "Gallery shows the photos in your Pictures folder and in the folders\n"
                     "you add, grouped by month. Settings are kept in the [gallery] section\n"
                     "of ~/.config/sde/sde.conf.\n"));
      return 0;
    }
    fprintf(stderr, "sde-gallery: unknown option %s (see --help)\n", argv[i]);
    return 2;
  }
  Config config;
  config.load();
  fl_open_display();
  auto theme = Theme::from_config(config);
  theme.apply_to_fltk();
  fl_register_images();
  // Worker threads hand thumbnails to the main loop with Fl::awake().
  Fl::lock();

  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int w = std::min(S(1200), sw - S(40)), h = std::min(S(800), sh - S(40));
  auto *window = new gallery::GalleryWindow(std::max(w, S(560)), std::max(h, S(400)));
  window->position(sx + (sw - window->w()) / 2, sy + (sh - window->h()) / 2);
  window->show();
  XClassHint hint{const_cast<char *>("sde-gallery"), const_cast<char *>("SDE-Gallery")};
  XSetClassHint(fl_display, fl_xid(window), &hint);
  // Follow theme changes made in Settings (_SDE_CONFIG_SERIAL on the root).
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  Fl::focus(nullptr);
  int status = Fl::run();
  delete window;
  return status;
}
