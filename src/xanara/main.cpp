// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// xanara: read, annotate, fill in and rearrange PDF documents.
#include <FL/Fl.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/x.H>
#include <X11/Xutil.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

static void usage() {
  printf("%s", _("Usage: xanara [--page=N] [FILE...]\n"
                 "       xanara --version | --help\n\n"
                 "Xanara shows PDF documents sharply at every zoom, each in a tab of its\n"
                 "own. Its modes annotate (highlight, notes, drawings, shapes, stamps,\n"
                 "signatures), edit (text, images, links, watermarks, headers and footers,\n"
                 "Bates numbers, crop), rearrange pages, add and fill in form fields,\n"
                 "convert (images, text, HTML, Markdown), protect (passwords, redaction,\n"
                 "metadata) and offer tools (compress, OCR, compare, print). Structural\n"
                 "changes use qpdf; ghostscript and tesseract are used when installed.\n"
                 "The original is replaced only after the new file was written completely.\n\n"
                 "  --page=N    open FILE at page N (1 is the first page)\n\n"
                 "Settings are kept in the [xanara] section of ~/.config/sde/sde.conf; the\n"
                 "last page of each document in ~/.local/state/sde/xanara-history.\n"));
}

int main(int argc, char **argv) {
  init_i18n();
  std::vector<std::string> files;
  int page = -1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version" || a == "-v") {
      sde::print_version("xanara", SDE_APP_VERSION);
      printf("Rendering by poppler %s.\n", poppler_get_version());
      return 0;
    }
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    }
    if (a.rfind("--page=", 0) == 0) {
      page = atoi(a.c_str() + 7) - 1;
      continue;
    }
    if (a == "--page" && i + 1 < argc) {
      page = atoi(argv[++i]) - 1;
      continue;
    }
    if (a == "--") {
      for (++i; i < argc; ++i) files.push_back(argv[i]);
      break;
    }
    if (a.size() > 1 && a[0] == '-') {
      fprintf(stderr, "xanara: unknown option %s (see --help)\n", a.c_str());
      return 2;
    }
    files.push_back(a);
  }
  Config config;
  config.load();
  fl_open_display();
  auto theme = Theme::from_config(config);
  theme.apply_to_fltk();
  fl_register_images();
  // The renderer thread hands finished tiles to the main loop with Fl::awake().
  Fl::lock();

  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int w = std::min(S(1240), sw - S(40)), h = std::min(S(860), sh - S(40));
  auto *window = new xanara::XanaraWindow(std::max(w, S(560)), std::max(h, S(400)));
  window->position(sx + (sw - window->w()) / 2, sy + (sh - window->h()) / 2);
  window->show();
  XClassHint hint{const_cast<char *>("xanara"), const_cast<char *>("Xanara")};
  XSetClassHint(fl_display, fl_xid(window), &hint);
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  // Every file opens in a tab of its own; without files the tabs of the
  // last session come back (unless [xanara] restore_session=false).
  if (!files.empty()) {
    for (size_t i = 0; i < files.size(); ++i)
      window->open_path(xanara::path_from_argument(files[i]), i == 0 ? page : -1);
    if (files.size() > 1) window->activate_tab(0);
  } else if (config.get_bool("xanara", "restore_session", true)) {
    std::string text;
    std::vector<std::string> paths;
    int active = -1;
    if (read_file(state_home() + "/sde/xanara-session", text) && xanara::TabList::parse(text, paths, active)) {
      for (const auto &p : paths)
        if (file_exists(p)) window->open_path(p);
      window->activate_tab(active < int(paths.size()) ? active : -1);
    }
  }
  int status = Fl::run();
  delete window;
  return status;
}
