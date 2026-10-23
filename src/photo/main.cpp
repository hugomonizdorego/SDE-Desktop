// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-view: Photo, the SDE photo viewer and editor.
#include <FL/Fl.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/x.H>
#include <X11/Xutil.h>
#include <glib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "link.h"
#include "photo.h"
#include "theme.h"
#include "util.h"

using namespace sde;

namespace {

std::string absolute(const std::string &arg) {
  if (starts_with(arg, "file://")) {
    gchar *p = g_filename_from_uri(arg.c_str(), nullptr, nullptr);
    std::string out = p ? p : "";
    g_free(p);
    return out;
  }
  if (!arg.empty() && arg[0] == '/') return arg;
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) return arg;
  std::string out = std::string(cwd) + "/" + arg;
  // Tidy "./" components.
  std::string clean;
  for (const auto &part : split(out, '/')) {
    if (part == ".") continue;
    if (part == ".." && !clean.empty()) {
      clean.resize(clean.rfind('/'));
      continue;
    }
    clean += "/" + part;
  }
  return clean.empty() ? "/" : clean;
}

void usage() {
  printf("%s", _("Usage: sde-view [OPTIONS] [FILE|URI|FOLDER...]\n\n"
                 "Photo views and edits one image at a time: rotate, flip, crop, resize,\n"
                 "adjust the colours, apply filters, and draw on it (pen, highlighter, shapes,\n"
                 "arrows, text, blur and pixelate). The information panel shows the camera\n"
                 "details and the location. Albums, favorites and slideshows are in Gallery.\n\n"
                 "Options:\n"
                 "  --album=ID      step through a Gallery album: all, favorites, recent,\n"
                 "                  screenshots, folder:PATH, device:PATH, dir:PATH, list:FILE\n"
                 "  --sort=ORDER    newest, oldest or name (default: Gallery's order)\n"
                 "  --slideshow     play a slideshow of the folder in Gallery\n"
                 "  --fullscreen    start in full screen\n"
                 "  --draw          open the drawing tools (Draw tab)\n"
                 "  --version       print the version\n"
                 "  --help          show this help\n\n"
                 "Settings are kept in the [photo] section of ~/.config/sde/sde.conf.\n"));
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  std::vector<std::string> files;
  std::string album_arg, sort_arg;
  bool slideshow = false, full = false, draw = false, options = true;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (options && a == "--") {
      options = false;
      continue;
    }
    if (options && a.size() > 1 && a[0] == '-') {
      if (a == "--version") return sde::print_version("sde-view", SDE_APP_VERSION);
      if (a == "--help" || a == "-h") {
        usage();
        return 0;
      }
      if (a == "--slideshow") slideshow = true;
      else if (a == "--draw") draw = true;
      else if (a == "--fullscreen") full = true;
      else if (starts_with(a, "--album=")) album_arg = a.substr(8);
      else if (a == "--album" && i + 1 < argc) album_arg = argv[++i];
      else if (starts_with(a, "--sort=")) sort_arg = a.substr(7);
      else {
        fprintf(stderr, "sde-view: unknown option %s (see --help)\n", a.c_str());
        return 2;
      }
      continue;
    }
    files.push_back(absolute(a));
  }

  gallery::Album album;
  bool explicit_album = false;
  if (!album_arg.empty()) {
    if (!gallery::parse_album(album_arg, album)) {
      fprintf(stderr, "sde-view: unknown album %s (see --help)\n", album_arg.c_str());
      return 2;
    }
    explicit_album = true;
  }
  Config config;
  config.load();
  gallery::Sort sort = gallery::Sort::Name;
  if (explicit_album) gallery::parse_sort(config.get("gallery", "sort", "newest"), sort);
  if (!sort_arg.empty() && !gallery::parse_sort(sort_arg, sort)) {
    fprintf(stderr, "sde-view: unknown sort order %s\n", sort_arg.c_str());
    return 2;
  }

  // What to show first.
  std::string first;
  if (files.size() > 1) {
    // Several files: step through exactly those.
    std::string list = runtime_dir() + "/sde-view/args-" + std::to_string(getpid()) + ".list";
    if (gallery::write_album_list(list, files)) {
      album = gallery::Album{gallery::AlbumKind::List, list};
      explicit_album = true;
    }
    first = files[0];
  } else if (files.size() == 1) {
    first = files[0];
    if (dir_exists(first)) {
      auto in_dir = gallery::scan_directory(first, gallery::extra_formats());
      gallery::sort_photos(in_dir, gallery::Sort::Name);
      if (!explicit_album) album = gallery::Album{gallery::AlbumKind::Directory, first};
      first = in_dir.empty() ? "" : in_dir[0].path;
    } else if (!explicit_album) {
      std::string dir = first.substr(0, first.rfind('/'));
      album = gallery::Album{gallery::AlbumKind::Directory, dir.empty() ? "/" : dir};
    }
  } else if (explicit_album && album.kind == gallery::AlbumKind::Directory) {
    auto in_dir = gallery::scan_directory(album.path, gallery::extra_formats());
    gallery::sort_photos(in_dir, sort);
    if (!in_dir.empty()) first = in_dir[0].path;
  }

  fl_open_display();
  // Slideshows are Gallery's; hand them over when it is installed.
  if (slideshow && !first.empty() && gallery::slideshow_in_gallery(first)) return 0;
  auto theme = Theme::from_config(config);
  theme.apply_to_fltk();
  fl_register_images();
  Fl::lock();  // worker threads wake the main loop

  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int w = std::min(S(1280), sw - S(40)), h = std::min(S(840), sh - S(40));
  auto *window = new photo::PhotoWindow(std::max(w, S(480)), std::max(h, S(360)));
  window->position(sx + (sw - window->w()) / 2, sy + (sh - window->h()) / 2);
  window->show();
  XClassHint hint{const_cast<char *>("sde-view"), const_cast<char *>("Sdeview")};
  XSetClassHint(fl_display, fl_xid(window), &hint);
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  if (full) window->fullscreen();
  if (!first.empty() && file_exists(first)) {
    window->open(first, album, explicit_album, sort);
  } else if (!first.empty()) {
    fprintf(stderr, "sde-view: %s: no such file\n", first.c_str());
    window->toast(format(_("“%s” was not found"), basename_of(first).c_str()));
  } else if (explicit_album) {
    window->open("", album, true, sort);
  }
  if (slideshow) window->toast(_("Slideshows need Gallery (sde-gallery)"));
  if (draw) window->open_draw_when_loaded();
  Fl::focus(window->canvas());
  int status = Fl::run();
  delete window;
  return status;
}
