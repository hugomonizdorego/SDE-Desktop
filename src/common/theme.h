// SPDX-License-Identifier: GPL-3.0-or-later
// SDE colors and metrics. Since 0.1.0~alpha16 every palette comes from
// data/themes/palettes.json, the file that also builds the GTK and Openbox
// themes, so SDE programs, GTK applications and window frames match:
//
//   family  graphite (default, Apple HIG), fluent (Fluent Design, Flat
//           Remix) or glass (frosted; replaces SDE Aero and Liquid Glass)
//   mode    light, dark or auto (light from 07:00 to 19:00)
//   accent  blue, teal, green, yellow, orange, red, pink, purple, graphite
//
// sde.conf: [appearance] style=<family>, theme=<mode>, accent_name=<accent>
// and accent=#RRGGBB (the accent's light shade, or a custom color).
#pragma once

#include <FL/Enumerations.H>

#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

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
  Color background;       // window background (Mica tinted in Fluent)
  Color surface;          // cards, menus, panel
  Color surface_alt;      // sidebars, search fields
  Color hover;            // hovered item background
  Color text;
  Color text_dim;
  Color border;
  Color shadow;           // shadow color with its alpha

  // Tokens added in 0.1.0~alpha16, shared with the GTK and Openbox themes.
  Color menu_bg;          // menus, context menus, popups, Start menu, Action Center
  Color menu_text;
  Color menu_hover;       // highlighted menu item (accent in Graphite/Glass, subtle in Fluent)
  Color menu_hover_text;
  Color menu_border;
  Color menu_separator;
  Color selection_bg;     // selected rows and text
  Color selection_text;
  Color panel_bg;         // the panel (taskbar) before compositor translucency
  Color titlebar_bg;      // active window title bar (SDE WM and headerbars)
  Color titlebar_text;
  Color success, warning, error;
  // Corner radii (already scaled). Since 0.1.0~alpha51 every surface is
  // soft: nothing an SDE program draws has square corners. Use radius_for()
  // for controls whose height can be small.
  int radius = 10;        // windows, menus, popups, cards, dialogs
  int radius_small = 6;   // buttons, fields, menu items, list selections, tooltips
  int radius_large = 12;  // big surfaces: Start menu, Action Center, flyouts
  int shadow_radius = 18; // blur radius of drop shadows (already scaled)
  // Radius for a control |h| pixels high: radius_small, at most half the
  // height (a pill for slim controls).
  double radius_for(double h) const { return std::fmin(radius_small, h / 2.0); }

  // Caption buttons (alpha51, Mint-Y like; the same colors as the SDE WM
  // frames and GTK header bars, see caption_colors() in build-themes.py):
  // thin glyphs, a soft disc behind close (and behind the others while
  // hovered), a muted red close while hovered.
  Color caption_glyph;
  Color caption_disc;
  Color caption_press;
  Color caption_close;    // close while hovered
  Color caption_close_press;
  double mica = 0;        // how much of the accent tints window backgrounds (0-1)
  std::string family = "graphite";  // graphite, fluent or glass
  std::string mode = "light";       // the setting: light, dark or auto
  std::string accent_name = "blue"; // nearest named accent
  std::string gtk_theme;            // e.g. "SDE-Graphite-Light-Blue"

  std::string font;       // fontconfig family, e.g. "Sans"
  int font_size = 13;     // pixels used by FLTK (already scaled)
  int panel_height = 40;  // pixels (already scaled)
  int panel_size = 40;    // unscaled setting: 32, 40 or 48
  bool panel_top = false;

  // |style| is the family (kept for older code). |glass| is true for the
  // Glass family with [aero] enabled (sheen and gradients); |translucent|
  // is true whenever [aero] enabled: the compositor then applies |opacity|
  // to the panel and |popup_opacity| to menus (Acrylic, vibrancy).
  //
  // Since 0.1.0~alpha50 one setting, [aero] transparency (0-100, presets
  // Off 0, Subtle 25, Clear 50, Glass 80), drives every translucent surface
  // of every family. 100 is the most transparent each surface can be while
  // its text keeps WCAG contrast (4.5:1 text, 3:1 secondary text) over any
  // backdrop, so text stays crisp; see readable_opacity(). On the lite
  // compositor profile (virtual machines, software rendering) there is no
  // blur and surfaces stay more opaque.
  std::string style = "graphite";
  bool glass = false;
  bool translucent = true;
  int transparency = 25;        // the setting, 0-100
  bool lite = false;            // lite compositor profile: no blur, less transparency
  bool blur = true;             // blur behind translucent surfaces
  double opacity = 1.0;         // panel (whole window, text included)
  double popup_opacity = 1.0;   // SDE menus, Start menu, Action Center, OSDs
  double frame_opacity = 1.0;   // SDE WM title bars and borders (compositor frame-opacity)
  double gtk_menu_alpha = 1.0;  // GTK menu and popover backgrounds (text stays opaque)

  // Loads theme and metrics from the merged SDE configuration.
  static Theme from_config(const Config &cfg);
  // A palette without a configuration (previews in Settings).
  static Theme make(const std::string &family, bool dark, const std::string &accent);

  // Applies the palette to stock FLTK widgets (scheme, background colors and
  // the default font) for dialogs such as sde-settings and sde-run.
  void apply_to_fltk() const;
};

// The user's theme choice with every older value mapped to a current one.
struct ThemeChoice {
  std::string family = "graphite";  // graphite, fluent, glass
  std::string mode = "light";       // light, dark, auto
  std::string accent = "blue";      // named accent
  std::string custom_accent;        // "#RRGGBB" when the accent is not a named one
};

// Maps [appearance] style/theme/accent/accent_name values of any SDE release
// to the current families. Unknown styles become Graphite, unknown modes
// light, unknown accents the nearest named accent (blue without a color):
//   sde -> graphite, aero -> fluent, liquid-glass -> glass.
ThemeChoice map_theme_choice(const std::string &style, const std::string &theme,
                             const std::string &accent_hex, const std::string &accent_name);
ThemeChoice theme_choice(const Config &cfg);
// Rewrites legacy [appearance] values in |cfg| (not saved). True if changed.
bool migrate_theme_config(Config &cfg);

// Whether a mode is dark now ("auto": dark before 07:00 and from 19:00).
bool theme_mode_dark(const std::string &mode, std::time_t now = std::time(nullptr));
// Seconds until "auto" next switches between light and dark.
int theme_auto_seconds_left(std::time_t now = std::time(nullptr));

// The theme family: "graphite", "fluent" or "glass" (theme_choice().family).
std::string theme_style(const Config &cfg);
// GTK/Openbox theme directory, e.g. theme_dir("fluent", true, "green") is
// "SDE-Fluent-Dark-Green". Old style names are accepted.
std::string theme_dir(const std::string &family, bool dark = false, const std::string &accent = "blue");
// The theme directory for the configuration, e.g. "SDE-Graphite-Light-Blue".
std::string theme_dir(const Config &cfg);
// Openbox <titleLayout> of a family. Since 0.1.0~alpha50 every family has
// "NLIMC": the window icon and title, then minimize, maximize and close on
// the right (alpha16 to alpha49 had Graphite and Glass round buttons on
// the left, "CIML", which SDE WM configurations are migrated from).
std::string theme_title_layout(const std::string &family);
// GTK decoration layout matching the SDE WM buttons: ":minimize,maximize,close".
std::string theme_decoration_layout(const std::string &family);

// Families and accents for pickers.
struct ThemeFamilyInfo {
  std::string id, label, description;
};
std::vector<ThemeFamilyInfo> theme_families();
std::vector<std::string> theme_accents();
std::string theme_accent_label(const std::string &accent);
Color theme_accent_color(const std::string &family, const std::string &accent, bool dark);

// Transparency presets of Settings > Appearance.
struct TransparencyPreset {
  const char *id;     // "off", "subtle", "clear", "glass"
  const char *label;  // untranslated (N_) label
  int value;          // [aero] transparency
};
const std::vector<TransparencyPreset> &transparency_presets();
// The preset id of a transparency value, or "" for a value of the user's own.
std::string transparency_preset_id(int value);

// Contrast of |fg| on a surface |bg| drawn with |alpha| over the worst
// backdrop (any gray, the text color or the surface color). |fade_text|:
// the whole window is translucent (_NET_WM_WINDOW_OPACITY), text included;
// otherwise only the background is (GTK CSS alpha, ARGB windows).
double translucent_contrast(const Color &fg, const Color &bg, double alpha, bool fade_text);
// The lowest alpha (0.30-1.00) that keeps translucent_contrast() >= |ratio|.
double readable_opacity(const Color &fg, const Color &bg, double ratio, bool fade_text);

// The theme last applied with Theme::apply_to_fltk(); used by sde widgets.
const Theme &active_theme();

// Accent colors offered in Settings (Graphite's light shades).
struct AccentChoice {
  const char *name;
  const char *hex;
};
extern const AccentChoice kAccentChoices[];
extern const int kAccentChoiceCount;

}  // namespace sde
