// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// niny: the Niny Text Editor for the Selviana Desktop Environment.
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xutil.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "niny.h"
#include "theme.h"
#include "util.h"

using namespace sde;

static void usage() {
  printf("%s", _("Usage: niny [OPTION...] [FILE|URI...]\n"
                 "\n"
                 "Edit text files. Files that do not exist yet are created when saved.\n"
                 "\n"
                 "  +LINE            put the cursor on LINE of the next file\n"
                 "  FILE:LINE[:COL]  open FILE at a line (and column)\n"
                 "  -n, --new-window open a separate window instead of the running one\n"
                 "  -h, --help       show this help\n"
                 "  -v, --version    show the version\n"
                 "\n"
                 "Settings: ~/.config/sde/niny.conf\n"
                 "Session and backups of unsaved changes: ~/.local/state/niny\n"));
}

int main(int argc, char **argv) {
  init_i18n();
  std::vector<std::string> args;
  bool new_window = false, only_files = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (!only_files && (a == "--version" || a == "-v")) {
      printf("niny %s (SDE %s)\n", NINY_VERSION, SDE_VERSION);
      return 0;
    }
    if (!only_files && (a == "--help" || a == "-h")) {
      usage();
      return 0;
    }
    if (!only_files && (a == "--new-window" || a == "-n")) {
      new_window = true;
      continue;
    }
    if (!only_files && a == "--") {
      only_files = true;
      continue;
    }
    if (!only_files && a.size() > 1 && a[0] == '-' && a != "-") {
      fprintf(stderr, "niny: %s: %s\n", _("unknown option (see --help)"), a.c_str());
      return 2;
    }
    args.push_back(a);
  }
  char cwd_buf[PATH_MAX];
  std::string cwd = getcwd(cwd_buf, sizeof cwd_buf) ? cwd_buf : home_dir();
  if (!new_window && !env("DISPLAY").empty() && niny::hand_off_to_running(args, cwd)) return 0;

  Config config;
  config.load();
  fl_open_display();
  auto theme = Theme::from_config(config);
  theme.apply_to_fltk();
  Fl::visual(FL_RGB);

  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int w = std::min(S(1100), sw - S(40)), h = std::min(S(760), sh - S(40));
  auto *app = new niny::App(std::max(w, S(480)), std::max(h, S(320)));
  app->position(sx + (sw - app->w()) / 2, sy + (sh - app->h()) / 2);
  app->restore_session();
  app->recover_backups();
  app->open_files(args, cwd);
  app->show();
  XClassHint hint{const_cast<char *>("niny"), const_cast<char *>("Niny")};
  XSetClassHint(fl_display, fl_xid(app), &hint);
  // Follow theme changes made in Settings (_SDE_CONFIG_SERIAL on the root).
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  if (app->active()) app->editor()->take_focus();
  int status = Fl::run();
  delete app;
  return status;
}
