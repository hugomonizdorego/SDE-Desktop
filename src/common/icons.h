// SPDX-License-Identifier: GPL-3.0-or-later
// Freedesktop.org Icon Theme Specification lookup and image loading.
#pragma once

#include <cairo.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sde {

struct DesktopEntry;

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
  // Adwaita, AdwaitaLegacy and hicolor, then Papirus for what none of them
  // has (0.2 Alpha 2), then /usr/share/pixmaps. Scaled directories
  // (Scale=2: "24x24@2x") count with their pixel size.
  static std::string lookup(const std::string &name, int size);

  // The icon to draw for an application (0.2 Alpha 2): its Icon= when the
  // icon theme has it, else the first near match the theme has (see
  // app_icon_names()), else, with |by_category|, an icon for its categories
  // (category_icon_names()); "" when nothing fits. |size| is the size it
  // is drawn at (the lookups are cached per size). The answer is cached per
  // desktop entry until the theme changes or applications are installed;
  // the Start menu, the search, the taskbar, the desktop and the Control
  // Center all ask here.
  static std::string app_icon(const DesktopEntry &e, int size, bool by_category = true);
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
  // Forgets the names that were not found (applications were installed:
  // their icons may have come with them).
  static void forget_missing();

  // Icons loaded from disk so far (cache misses) and the time spent (ms),
  // for timing hooks (src/panel/menu_perf.h).
  struct Stats {
    unsigned long loads = 0;
    double ms = 0;
  };
  static Stats stats();
};

// Icon names that may be an application's icon, best first: Icon= (also
// without a file extension, without "-symbolic", in lower case and the last
// part of a reverse-DNS name: "org.gnome.Maps" -> "Maps", "maps"), then
// the same made of its desktop file ID, its program (Exec=, past env,
// interpreters, sudo and "flatpak run") and its window class, and SDE's
// own drawing for a few common applications ("sde-app-" + ID). When Icon=
// is a generic name of the Icon Naming Specification ("preferences-system-
// network", "utilities-terminal"...), the names made of the application
// come first: an icon drawn for the application itself says more (Papirus
// draws preferences-system-network as an Ethernet port).
std::vector<std::string> app_icon_names(const DesktopEntry &e);
// Generic icons for an application's categories, most specific first
// (TerminalEmulator: utilities-terminal; network settings:
// preferences-system-network, network-workgroup...), then its main
// category (applications-internet...) and icons that plain Adwaita has.
std::vector<std::string> category_icon_names(const DesktopEntry &e);
// Everything IconLoader::app_icon() tries, in its order (an absolute Icon=
// path first): for IconLoader::get(), which tries the names only when the
// icon is loaded (the desktop loads its icons on idle).
std::vector<std::string> app_icon_list(const DesktopEntry &e, bool by_category = true);

// Sets the icon of every window the program shows from now on
// (_NET_WM_ICON: the title bar and the taskbar) to the first of |names| in
// the icon theme, at 32 and 64 pixels. Call it before the first show();
// nothing changes when none of the names is installed.
void set_default_window_icon(const std::vector<std::string> &names);

}  // namespace sde
