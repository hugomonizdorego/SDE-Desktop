// SPDX-License-Identifier: GPL-3.0-or-later
// Freedesktop.org Icon Theme Specification lookup and image loading.
#pragma once

#include <cairo.h>

#include <string>
#include <vector>

namespace sde {

// Loads an image file (SVG, PNG, JPEG, GIF, BMP, XPM) into an ARGB cairo
// image surface. SVG files are rendered at |w|x|h|; bitmaps are returned at
// their natural size. Caller owns the surface. Returns nullptr on error.
cairo_surface_t *load_image(const std::string &path, int w = 0, int h = 0);

// Renders |path| into a |w|x|h| surface using a wallpaper |mode|:
// "zoom" (cover), "fit" (contain), "stretch", "center" or "tile".
cairo_surface_t *load_wallpaper(const std::string &path, int w, int h, const std::string &mode,
                                double bg_r, double bg_g, double bg_b);

class IconLoader {
 public:
  // |theme| may be "auto" to pick the first installed theme of a sensible list.
  static void set_theme(const std::string &theme);
  static std::string theme();

  // Absolute path of the best icon file for |name| at |size| pixels, or "".
  // |name| may also be an absolute path.
  static std::string lookup(const std::string &name, int size);

  // Cached cairo surface, scaled to |size|. Tries each name in order and
  // falls back to "application-x-executable". Never free the result.
  static cairo_surface_t *get(const std::vector<std::string> &names, int size);

  // Installed icon themes with application icons ("Papirus", "Adwaita", ...).
  static std::vector<std::string> installed_themes();
  static void clear_cache();
};

}  // namespace sde
