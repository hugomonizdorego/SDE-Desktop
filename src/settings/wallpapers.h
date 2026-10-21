// SPDX-License-Identifier: GPL-3.0-or-later
// The wallpapers offered in Settings > Appearance: every image below
// /usr/share/backgrounds (recursively, SDE's own first), one picture per
// desktop-base theme and the user's wallpaper folders. No FLTK here; the
// scan runs on a worker thread.
#pragma once

#include <ctime>
#include <set>
#include <string>
#include <vector>

namespace sde {

struct Wallpaper {
  std::string path;   // as found (may be a symbolic link)
  std::string label;  // "Selviana Dark"
  time_t mtime = 0;
};

// Folders searched by default, most important first. |datadir| is
// SDE_DATADIR (".../share/sde").
std::vector<std::string> wallpaper_roots(const std::string &datadir);

// Recursively lists the images below |roots| (jpg, jpeg, png, svg, plus
// |extra| such as webp). Slideshow XML files and hidden folders are
// skipped; files reached twice (symbolic links, overlapping roots) are
// listed once. Folders named "*-theme" with a wallpaper/contents/images
// layout (desktop-base) contribute only their largest picture.
std::vector<Wallpaper> scan_wallpapers(const std::vector<std::string> &roots,
                                       const std::set<std::string> &extra = {},
                                       size_t limit = 5000);

// "selviana-dark.svg" -> "Selviana Dark"; desktop-base pictures
// (".../debion-theme/wallpaper/contents/images/1920x1080.svg") -> "Debion".
std::string wallpaper_label(const std::string &path);

}  // namespace sde
