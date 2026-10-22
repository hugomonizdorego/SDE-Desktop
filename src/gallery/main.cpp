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
  std::string select, slideshow, folder;
  bool import = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) {
      printf("sde-gallery %s\n", SDE_VERSION);
      return 0;
    }
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      printf("%s", _("Usage: sde-gallery [OPTIONS] [FOLDER]\n\n"
                     "Gallery organizes your photos: the Pictures folder and the folders you\n"
                     "add, grouped by month, your favorites and albums, and the photos on\n"
                     "cameras, SD cards and USB drives (Devices), with slideshows, import and\n"
                     "batch actions. Photos open in Photo (sde-view) for viewing and editing.\n"
                     "Settings are kept in the [gallery] section of ~/.config/sde/sde.conf;\n"
                     "albums in ~/.local/share/sde/gallery/albums.\n\n"
                     "  FOLDER             show the photos of FOLDER\n"
                     "  --select FILE      show and select FILE (in the running Gallery if any)\n"
                     "  --slideshow FILE   play a slideshow of FILE's folder, from FILE\n"
                     "  --import           import photos from a camera or card\n"
                     "  --version          print the version\n"));
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
    if (!strcmp(argv[i], "--slideshow") && i + 1 < argc) {
      slideshow = argv[++i];
      continue;
    }
    if (!strncmp(argv[i], "--slideshow=", 12)) {
      slideshow = argv[i] + 12;
      continue;
    }
    if (!strcmp(argv[i], "--import")) {
      import = true;
      continue;
    }
    if (argv[i][0] != '-') {
      // A folder (or a file: its folder), or a file:// URI from a launcher.
      std::string a = argv[i];
      if (!strncmp(argv[i], "file://", 7)) a = a.substr(7);
      folder = a;
      continue;
    }
    fprintf(stderr, "sde-gallery: unknown option %s (see --help)\n", argv[i]);
    return 2;
  }
  auto absolute = [](std::string &p) {
    if (p.empty() || p[0] == '/') return;
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) p = std::string(cwd) + "/" + p;
  };
  absolute(select);
  absolute(slideshow);
  absolute(folder);
  if (!folder.empty() && !dir_exists(folder)) {
    // A photo: show it selected in its folder.
    if (select.empty()) select = folder;
    folder.clear();
  }
  fl_open_display();
  // One Gallery per display: hand the request to the running one.
  if (gallery::gallery_running()) {
    std::vector<gallery::GalleryEvent> ev;
    if (!folder.empty()) ev.push_back({"browse", folder});
    if (!select.empty()) ev.push_back({"select", select});
    if (!slideshow.empty()) ev.push_back({"slideshow", slideshow});
    if (import) ev.push_back({"import", "/"});
    if (ev.empty()) ev.push_back({"present", "/"});
    gallery::send_gallery_events(ev);
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
  if (!folder.empty()) window->browse(folder);
  if (!select.empty()) window->select_path(select);
  if (!slideshow.empty()) window->slideshow_from(slideshow);
  if (import) Fl::add_timeout(0.3, [](void *w) { static_cast<gallery::GalleryWindow *>(w)->import_dialog(); }, window);
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
