// SPDX-License-Identifier: GPL-3.0-or-later
// SDE colors and metrics. One light and one dark palette, plus an accent color.
#pragma once

#include <FL/Enumerations.H>

#include <cmath>
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
  // WCAG contrast ratio between two opaque colors (1 to 21).
  double contrast(const Color &other) const;
};

// User interface scale factor (1.0 = 100 %, up to 2.0). Set by
// Theme::from_config(); every SDE program multiplies its pixel sizes by it.
double ui_scale();
void set_ui_scale(double scale);
inline int S(double px) { return int(std::lround(px * ui_scale())); }
inline double Sd(double px) { return px * ui_scale(); }

struct Theme {
  bool dark = false;
  double scale = 1.0;     // UI scale, see ui_scale()
  Color accent;           // selection, focus, active indicators
  Color on_accent;        // text drawn on top of the accent
  Color accent_text;      // accent shade readable as text on surface/background
  Color background;       // window background
  Color surface;          // cards, menus, panel
  Color surface_alt;      // sidebars, search fields
  Color hover;            // hovered item background
  Color text;
  Color text_dim;
  Color border;
  Color shadow;

  std::string font;       // fontconfig family, e.g. "Sans"
  int font_size = 13;     // pixels used by FLTK (already scaled)
  int panel_height = 40;  // pixels (already scaled)
  int panel_size = 40;    // unscaled setting: 32, 40 or 48
  bool panel_top = false;

  // SDE Aero glass. |opacity| is the window opacity the compositor applies to
  // the panel (popups use popup_opacity, which keeps text more legible).
  bool glass = true;
  int transparency = 18;  // percent, 0-60
  double opacity = 1.0, popup_opacity = 1.0;

  // Loads theme and metrics from the merged SDE configuration.
  static Theme from_config(const Config &cfg);

  // Applies the palette to stock FLTK widgets (scheme, background colors and
  // the default font) for dialogs such as sde-settings and sde-run.
  void apply_to_fltk() const;
};

// The theme last applied with Theme::apply_to_fltk(); used by sde widgets.
const Theme &active_theme();

// Accent colors offered in Settings.
struct AccentChoice {
  const char *name;
  const char *hex;
};
extern const AccentChoice kAccentChoices[];
extern const int kAccentChoiceCount;

}  // namespace sde
