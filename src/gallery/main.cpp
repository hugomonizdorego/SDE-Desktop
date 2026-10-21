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
#include <string>
#include <unistd.h>

#include "build_config.h"
#include "config.h"
#include "gallery.h"
#include "i18n.h"
#include "link.h"
#include "theme.h"
#include "util.h"

using namespace sde;

int main(int argc, char **argv) {
  init_i18n();
  std::string select;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) {
      printf("sde-gallery %s\n", SDE_VERSION);
      return 0;
    }
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      printf("%s", _("Usage: sde-gallery [--select FILE] [--version | --help]\n\n"
                     "Gallery shows the photos in your Pictures folder and in the folders\n"
                     "you add, grouped by month, and the photos on cameras, SD cards and\n"
                     "USB drives (Devices). Settings are kept in the [gallery] section of\n"
                     "~/.config/sde/sde.conf.\n\n"
                     "  --select FILE   show and select FILE (in the running Gallery if any)\n"));
      return 0;
    }
    if (!strcmp(argv[i], "--select") && i + 1 < argc) {
      select = argv[++i];
      continue;
    }
    if (!strncmp(argv[i], "--select=", 9)) {
      select = argv[i] + 9;
      continue;
    }
    fprintf(stderr, "sde-gallery: unknown option %s (see --help)\n", argv[i]);
    return 2;
  }
  if (!select.empty() && select[0] != '/') {
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) select = std::string(cwd) + "/" + select;
  }
  fl_open_display();
  // One Gallery per display: hand the request to the running one.
  if (gallery::gallery_running()) {
    gallery::send_gallery_events({{select.empty() ? "present" : "select", select.empty() ? "/" : select}});
    return 0;
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
  gallery::claim_gallery(fl_xid(window));
  // Events that arrived before we owned the selection are dropped.
  XDeleteProperty(fl_display, RootWindow(fl_display, fl_screen), gallery::events_atom());
  if (!select.empty()) window->select_path(select);
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
