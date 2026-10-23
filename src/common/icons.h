// SPDX-License-Identifier: GPL-3.0-or-later
// Freedesktop.org Icon Theme Specification lookup and image loading.
#pragma once

#include <cairo.h>

#include <cstddef>
#include <cstdint>
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

// Builds a |size|x|size| surface from raw pixels (caller owns it):
//  * rgba_pixels: RGB or RGBA rows as sent in notification "image-data" hints
//  * argb_network_pixels: ARGB32 in network byte order (StatusNotifierItem)
cairo_surface_t *rgba_pixels(const uint8_t *data, size_t len, int w, int h, int stride,
                             bool has_alpha, int channels, int size);
cairo_surface_t *argb_network_pixels(const uint8_t *data, size_t len, int w, int h, int size);

// An installed icon theme ($XDG_DATA_DIRS/icons/<id>/index.theme).
struct IconThemeInfo {
  std::string id;        // directory name, e.g. "Papirus-Dark"
  std::string name;      // Name= of index.theme, e.g. "Papirus-Dark"
  std::string comment;   // Comment=
  std::vector<std::string> inherits;
};

// The icon theme for an [appearance] icon_theme setting:
//   "auto" (or empty, or a theme that is not installed): Papirus, or
//   Papirus-Dark in dark mode, when installed; then Adwaita; then the first
//   of a list of well-known themes; hicolor when there is nothing else.
//   Any other installed theme is used as it is.
// GTK (settings.ini, XSETTINGS, GSettings), Liria and every SDE program use
// this, so one choice applies everywhere.
std::string resolve_icon_theme(const std::string &setting, bool dark);

class IconLoader {
 public:
  // |theme| is an [appearance] icon_theme setting (see resolve_icon_theme());
  // "auto" follows the dark mode of the active theme (Theme::apply_to_fltk()).
  // Changing the theme clears the caches, so the next lookup uses it.
  static void set_theme(const std::string &theme);
  static void set_theme(const std::string &theme, bool dark);
  static std::string theme();

  // Absolute path of the best icon file for |name| at |size| pixels, or "".
  // |name| may also be an absolute path. Follows the Icon Theme
  // Specification: the theme, then its Inherits= parents depth first, then
  // Adwaita, AdwaitaLegacy and hicolor, then /usr/share/pixmaps. Scaled
  // directories (Scale=2: "24x24@2x") count with their pixel size.
  static std::string lookup(const std::string &name, int size);
  // The theme chain lookup() searches, e.g. {"Papirus", "breeze", "hicolor"}
  // (only installed themes).
  static std::vector<std::string> theme_chain();

  // Cached cairo surface, scaled to |size|. Tries each name in order and
  // falls back to "application-x-executable". Never free the result.
  static cairo_surface_t *get(const std::vector<std::string> &names, int size);

  // Installed icon themes with application or places icons ("Papirus",
  // "Adwaita", ...): cursor-only, hidden and hicolor/default are left out.
  static std::vector<std::string> installed_themes();
  static std::vector<IconThemeInfo> installed_theme_infos();
  // Looks |name| up in one theme (and its parents) without changing the
  // current theme, e.g. for previews of other themes in Settings.
  static std::string lookup_in(const std::string &theme, const std::string &name, int size);
  static void clear_cache();

  // Icons loaded from disk so far (cache misses) and the time spent (ms),
  // for timing hooks (src/panel/menu_perf.h).
  struct Stats {
    unsigned long loads = 0;
    double ms = 0;
  };
  static Stats stats();
};

}  // namespace sde
