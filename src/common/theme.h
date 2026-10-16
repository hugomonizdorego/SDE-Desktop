// SPDX-License-Identifier: GPL-3.0-or-later
// SDE colors and metrics. One light and one dark palette, plus an accent color.
#pragma once

#include <FL/Enumerations.H>

#include <cstdint>
#include <string>

namespace sde {

class Config;

struct Color {
  double r = 0, g = 0, b = 0, a = 1;

  static Color hex(uint32_t rgb, double alpha = 1.0);
  static bool parse(const std::string &text, Color &out);  // "#RRGGBB"
  Color alpha(double a2) const { return Color{r, g, b, a2}; }
  Color mix(const Color &o, double t) const;
  Fl_Color fl() const;
  std::string to_hex() const;
  double luminance() const;
};

struct Theme {
  bool dark = false;
  Color accent;           // selection, focus, active indicators
  Color on_accent;        // text drawn on top of the accent
  Color background;       // window background
  Color surface;          // cards, menus, panel
  Color surface_alt;      // sidebars, search fields
  Color hover;            // hovered item background
  Color text;
  Color text_dim;
  Color border;
  Color shadow;

  std::string font;       // fontconfig family, e.g. "Sans"
  int font_size = 13;     // pixels used by FLTK
  int panel_height = 40;

  // Loads theme and metrics from the merged SDE configuration.
  static Theme from_config(const Config &cfg);

  // Applies the palette to stock FLTK widgets (scheme, background colors and
  // the default font) for dialogs such as sde-settings and sde-run.
  void apply_to_fltk() const;
};

// Accent colors offered in Settings.
struct AccentChoice {
  const char *name;
  const char *hex;
};
extern const AccentChoice kAccentChoices[];
extern const int kAccentChoiceCount;

}  // namespace sde
