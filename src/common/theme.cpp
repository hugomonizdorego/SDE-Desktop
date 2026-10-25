// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"

#include <FL/Fl.H>
#include <FL/x.H>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "config.h"
#include "integration.h"
#include "menu.h"
#include "theme_palettes.h"
#include "util.h"

namespace sde {

// SDE Graphite's light accents (data/themes/palettes.json, alpha51).
const AccentChoice kAccentChoices[] = {
    {"Blue", "#3876B9"},   {"Teal", "#2B807B"}, {"Green", "#4A9157"}, {"Yellow", "#C29C48"},
    {"Orange", "#C9773F"}, {"Red", "#C24D45"},  {"Pink", "#B9547C"},  {"Purple", "#7E61A8"},
    {"Graphite", "#55585E"},
};
const int kAccentChoiceCount = int(sizeof(kAccentChoices) / sizeof(kAccentChoices[0]));

static double g_scale = 1.0;

double ui_scale() { return g_scale; }
void set_ui_scale(double scale) { g_scale = std::fmin(2.0, std::fmax(1.0, scale)); }

// "auto" follows Xft.dpi (set by most display managers and xrandr --dpi),
// rounded to the nearest quarter.
static double auto_scale() {
  if (!fl_display) return 1.0;
  const char *dpi = XGetDefault(fl_display, "Xft", "dpi");
  double d = dpi ? atof(dpi) : 0;
  if (d < 72) return 1.0;
  return std::round(d / 96.0 * 4) / 4;
}

Color Color::hex(uint32_t rgb, double alpha) {
  return Color{((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0,
               alpha};
}

bool Color::parse(const std::string &text, Color &out) {
  std::string t = trim(text);
  if (t.size() != 7 || t[0] != '#') return false;
  char *end = nullptr;
  unsigned long v = strtoul(t.c_str() + 1, &end, 16);
  if (!end || *end) return false;
  out = hex(uint32_t(v));
  return true;
}

Color Color::mix(const Color &o, double t) const {
  return Color{r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t};
}

Fl_Color Color::fl() const {
  auto c = [](double v) { return uchar(std::lround(std::fmin(1, std::fmax(0, v)) * 255)); };
  return fl_rgb_color(c(r), c(g), c(b));
}

std::string Color::to_hex() const {
  char buf[8];
  snprintf(buf, sizeof buf, "#%02X%02X%02X", int(std::lround(r * 255)),
           int(std::lround(g * 255)), int(std::lround(b * 255)));
  return buf;
}

double Color::luminance() const {
  auto lin = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
  return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

double Color::contrast(const Color &o) const {
  double a = luminance(), b = o.luminance();
  if (a < b) std::swap(a, b);
  return (a + 0.05) / (b + 0.05);
}

// ------------------------------------------------------------ palettes

namespace {

const palettes::Family &family_info(const std::string &id) {
  for (const auto &f : palettes::kFamilies)
    if (id == f.id) return f;
  return palettes::kFamilies[0];
}

int accent_index(const std::string &id) {
  for (int i = 0; i < palettes::kAccentCount; ++i)
    if (id == palettes::kAccentIds[i]) return i;
  return -1;
}

bool known_family(const std::string &id) {
  for (const auto &f : palettes::kFamilies)
    if (id == f.id) return true;
  return false;
}

// Hue distance in HSV, so a dark custom blue is still "blue".
std::string nearest_accent(const Color &c) {
  auto hsv = [](const Color &x, double &h, double &s, double &v) {
    double mx = std::max({x.r, x.g, x.b}), mn = std::min({x.r, x.g, x.b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) h = 0;
    else if (mx == x.r) h = std::fmod((x.g - x.b) / d + 6, 6) * 60;
    else if (mx == x.g) h = ((x.b - x.r) / d + 2) * 60;
    else h = ((x.r - x.g) / d + 4) * 60;
  };
  double h, s, v;
  hsv(c, h, s, v);
  if (s < 0.18) return "graphite";
  std::string best = palettes::kDefaultAccent;
  double best_d = 1e9;
  const auto &f = palettes::kFamilies[0];
  for (int i = 0; i < palettes::kAccentCount; ++i) {
    if (std::string(palettes::kAccentIds[i]) == "graphite") continue;
    double h2, s2, v2;
    hsv(Color::hex(f.accent[i][0]), h2, s2, v2);
    double dh = std::fabs(h - h2);
    dh = std::min(dh, 360 - dh);
    double d = dh + std::fabs(s - s2) * 20;
    if (d < best_d) best_d = d, best = palettes::kAccentIds[i];
  }
  return best;
}

}  // namespace

ThemeChoice map_theme_choice(const std::string &style_in, const std::string &theme_in,
                             const std::string &accent_hex, const std::string &accent_name) {
  ThemeChoice c;
  std::string style = lower(trim(style_in));
  c.family = palettes::kDefaultFamily;
  for (const auto &l : palettes::kLegacyStyles)
    if (style == l.from) c.family = l.to;
  std::string mode = lower(trim(theme_in));
  c.mode = mode == "dark" || mode == "auto" ? mode : "light";
  std::string name = lower(trim(accent_name));
  bool named = accent_index(name) >= 0;
  c.accent = named ? name : palettes::kDefaultAccent;
  Color col;
  if (!Color::parse(accent_hex, col)) return c;
  std::string up = col.to_hex();
  // A color SDE offered (alpha15 swatches, or any family's accent) selects
  // that named accent unless accent_name says otherwise.
  for (const auto &l : palettes::kLegacyAccents)
    if (up == l.from) {
      if (!named) c.accent = l.to;
      return c;
    }
  for (const auto &f : palettes::kFamilies)
    for (int i = 0; i < palettes::kAccentCount; ++i)
      for (int m = 0; m < 2; ++m)
        if (Color::hex(f.accent[i][m]).to_hex() == up) {
          if (!named) c.accent = palettes::kAccentIds[i];
          return c;
        }
  // A color of the user's own: SDE uses it, GTK the nearest named accent.
  if (!named) c.accent = nearest_accent(col);
  c.custom_accent = up;
  return c;
}

ThemeChoice theme_choice(const Config &cfg) {
  std::string style = cfg.get("appearance", "style", "");
  // Files from alpha10 to alpha15 without a style: [aero] enabled=false was
  // SDE Light/Dark, anything else Aero; both now mean the default family.
  if (trim(style).empty()) style = palettes::kDefaultFamily;
  return map_theme_choice(style, cfg.get("appearance", "theme", "light"),
                          cfg.get("appearance", "accent", ""), cfg.get("appearance", "accent_name", ""));
}

bool migrate_theme_config(Config &cfg) {
  ThemeChoice c = theme_choice(cfg);
  bool changed = false;
  auto set = [&](const char *key, const std::string &value) {
    if (cfg.get("appearance", key, "") != value) {
      cfg.set("appearance", key, value);
      changed = true;
    }
  };
  std::string style = lower(trim(cfg.get("appearance", "style", "")));
  if (!style.empty() && !known_family(style)) set("style", c.family);
  std::string mode = lower(trim(cfg.get("appearance", "theme", "light")));
  if (mode != c.mode) set("theme", c.mode);
  // Swatch colors of alpha15 and earlier become the named accent; current
  // palette colors and colors of the user's own stay as they are.
  Color col;
  if (Color::parse(cfg.get("appearance", "accent", ""), col))
    for (const auto &l : palettes::kLegacyAccents)
      if (col.to_hex() == l.from) {
        set("accent_name", c.accent);
        set("accent", theme_accent_color(c.family, c.accent, false).to_hex());
      }
  return changed;
}

bool theme_mode_dark(const std::string &mode, std::time_t now) {
  if (mode == "dark") return true;
  if (mode != "auto") return false;
  std::tm tm{};
  localtime_r(&now, &tm);
  return tm.tm_hour < 7 || tm.tm_hour >= 19;
}

int theme_auto_seconds_left(std::time_t now) {
  std::tm tm{};
  localtime_r(&now, &tm);
  int sec = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
  int next = sec < 7 * 3600 ? 7 * 3600 : sec < 19 * 3600 ? 19 * 3600 : 31 * 3600;
  return std::max(1, next - sec);
}

std::string theme_style(const Config &cfg) { return theme_choice(cfg).family; }

std::string theme_dir(const std::string &family, bool dark, const std::string &accent) {
  std::string fam = family;
  for (const auto &l : palettes::kLegacyStyles)
    if (lower(family) == l.from) fam = l.to;
  int a = accent_index(accent);
  const char *label = palettes::kAccentLabels[a < 0 ? 0 : a];
  return std::string(family_info(fam).prefix) + (dark ? "-Dark-" : "-Light-") + label;
}

std::string theme_dir(const Config &cfg) {
  ThemeChoice c = theme_choice(cfg);
  return theme_dir(c.family, theme_mode_dark(c.mode), c.accent);
}

std::string theme_title_layout(const std::string &family) { return family_info(family).title_layout; }

std::string theme_decoration_layout(const std::string &family) {
  (void)family;  // every family: right-hand minimize, maximize and close
  return ":minimize,maximize,close";
}

const std::vector<TransparencyPreset> &transparency_presets() {
  static const std::vector<TransparencyPreset> list = {
      {"off", "Off", 0}, {"subtle", "Subtle", 25}, {"clear", "Clear", 50}, {"glass", "Glass", 80}};
  return list;
}

std::string transparency_preset_id(int value) {
  for (const auto &p : transparency_presets())
    if (p.value == value) return p.id;
  return "";
}

double translucent_contrast(const Color &fg, const Color &bg, double alpha, bool fade_text) {
  auto over = [alpha](const Color &c, const Color &b) {
    return Color{c.r * alpha + b.r * (1 - alpha), c.g * alpha + b.g * (1 - alpha),
                 c.b * alpha + b.b * (1 - alpha), 1};
  };
  // Blur averages the wallpaper and windows behind into a smooth color, so
  // the worst case is a flat backdrop: any gray, or the colors themselves.
  std::vector<Color> backdrops = {fg, bg};
  for (int v = 0; v <= 10; ++v) backdrops.push_back(Color{v / 10.0, v / 10.0, v / 10.0, 1});
  double worst = 1e9;
  for (const auto &b : backdrops)
    worst = std::min(worst, (fade_text ? over(fg, b) : fg).contrast(over(bg, b)));
  return worst;
}

double readable_opacity(const Color &fg, const Color &bg, double ratio, bool fade_text) {
  int a = 100;
  while (a > 30 && translucent_contrast(fg, bg, (a - 1) / 100.0, fade_text) >= ratio) --a;
  return a / 100.0;
}

std::vector<ThemeFamilyInfo> theme_families() {
  std::vector<ThemeFamilyInfo> out;
  for (const auto &f : palettes::kFamilies) out.push_back({f.id, f.label, f.description});
  return out;
}

std::vector<std::string> theme_accents() {
  return std::vector<std::string>(palettes::kAccentIds, palettes::kAccentIds + palettes::kAccentCount);
}

std::string theme_accent_label(const std::string &accent) {
  int a = accent_index(accent);
  return a < 0 ? accent : palettes::kAccentLabels[a];
}

Color theme_accent_color(const std::string &family, const std::string &accent, bool dark) {
  int a = accent_index(accent);
  return Color::hex(family_info(family).accent[a < 0 ? 0 : a][dark ? 1 : 0]);
}

LiquidMaterial theme_liquid_material(const std::string &family, bool dark) {
  const palettes::Liquid &q = family_info(family).liquid;
  LiquidMaterial m;
  if (!q.on) return m;
  int i = dark ? 1 : 0;
  m.on = true;
  m.blur = q.blur;
  m.saturation = q.saturation;
  m.brightness = q.brightness[i];
  m.lens_band = q.lens_band;
  m.lens_shift = q.lens_shift;
  m.rim = q.rim[i];
  m.rim_width = q.rim_width;
  m.key_range = q.key_range;
  return m;
}

double liquid_legible_luminance(const Theme &t) {
  // A little above the WCAG ratios: the shader finds the tint in a few steps.
  auto limit = [&](const Color &fg, double ratio) {
    double l = fg.luminance();
    return t.dark ? (l + 0.05) / ratio - 0.05 : ratio * (l + 0.05) - 0.05;
  };
  double text = limit(t.text, 4.6), menu = limit(t.menu_text, 4.6), dim = limit(t.text_dim, 3.1);
  return t.dark ? std::max(0.0, std::min({text, menu, dim})) : std::min(1.0, std::max({text, menu, dim}));
}

// Fills the palette of |t| from family/mode/accent (t.accent may already
// hold a custom color).
static void fill_palette(Theme &t, const palettes::Family &f, bool custom) {
  const palettes::Mode &m = t.dark ? f.dark : f.light;
  if (!custom) t.accent = theme_accent_color(f.id, t.accent_name, t.dark);
  Color black = Color::hex(0x17191E), white = Color::hex(0xFFFFFF);
  t.on_accent = t.accent.contrast(black) > t.accent.contrast(white) ? black : white;
  t.mica = f.mica;
  t.background = Color::hex(m.background).mix(t.accent, f.mica);
  t.surface = Color::hex(m.surface);
  t.surface_alt = Color::hex(m.surface_alt);
  t.hover = Color::hex(m.hover);
  t.text = Color::hex(m.text);
  t.text_dim = Color::hex(m.text_dim);
  t.border = Color::hex(m.border);
  t.shadow = Color::hex(m.shadow, m.shadow_alpha);
  t.menu_bg = Color::hex(m.menu);
  t.menu_text = t.text;
  bool accent_menu = std::strcmp(f.menu_highlight, "accent") == 0;
  t.menu_hover = accent_menu ? t.accent : t.hover;
  t.menu_hover_text = accent_menu ? t.on_accent : t.text;
  // Highlighted items keep their label readable (WCAG AA): bright accents
  // (red, Flat Remix blue) get a slightly deeper shade there.
  for (int i = 1; i <= 10 && accent_menu && t.menu_hover_text.contrast(t.menu_hover) < 4.5; ++i)
    t.menu_hover = t.accent.mix(t.on_accent.luminance() > 0.5 ? Color::hex(0x000000) : Color::hex(0xFFFFFF),
                                i * 0.04);
  t.menu_border = t.border;
  t.menu_separator = t.border;
  t.selection_bg = t.accent;
  t.selection_text = t.on_accent;
  t.panel_bg = Color::hex(m.panel);
  t.titlebar_bg = Color::hex(m.titlebar).mix(t.accent, f.mica * 1.4);
  t.titlebar_text = Color::hex(m.titlebar_text);
  t.caption_glyph = t.titlebar_text.mix(t.titlebar_bg, 0.15);
  t.caption_disc = t.titlebar_bg.mix(t.titlebar_text, t.dark ? 0.38 : 0.40);
  t.caption_press = t.titlebar_bg.mix(t.titlebar_text, 0.58);
  t.caption_close = Color::hex(f.close_hover);
  t.caption_close_press = t.caption_close.mix(Color::hex(0x000000), 0.15);
  t.success = Color::hex(m.success);
  t.warning = Color::hex(m.warning);
  t.error = Color::hex(m.error);
  t.radius = S(f.radius);
  t.radius_small = S(f.radius_small);
  t.radius_large = S(f.radius_large);
  t.shadow_radius = S(f.shadow_radius);
  // Accent used for text: move it toward the text color until it reaches
  // the WCAG AA ratio (4.5:1) on both surfaces.
  t.accent_text = t.accent;
  for (int i = 1; i <= 20 && (t.accent_text.contrast(t.surface) < 4.5 ||
                              t.accent_text.contrast(t.background) < 4.5);
       ++i)
    t.accent_text = t.accent.mix(t.text, i * 0.05);
  t.family = f.id;
  t.style = f.id;
  t.gtk_theme = theme_dir(f.id, t.dark, t.accent_name);
}

// Whether the compositor runs its lite profile ([compositor] effects, or
// auto on virtual machines and software rendering), as integration.cpp
// decides it for the compositor configuration.
static bool compositor_lite(const Config &cfg) {
  if (cfg.get_bool("appearance", "low_resource", false)) return true;
  return compositor_effects(cfg) == "lite";
}

// Turns the transparency setting into the opacity of each surface: 0 is
// opaque, 100 the readable limit of that surface (readable_opacity()).
// Families keep a little character: Glass goes furthest, Graphite least.
// Without blur (lite profile) surfaces only go part of the way.
static void apply_transparency(Theme &t, const palettes::Family &f) {
  t.opacity = t.popup_opacity = t.frame_opacity = t.gtk_menu_alpha = t.toolkit_menu_opacity = 1.0;
  if (!t.translucent || t.transparency <= 0) return;
  // A square-root curve: the first steps of the slider (Subtle) already
  // show, the last ones (Glass) approach the readable limit gently.
  double s = std::sqrt(t.transparency / 100.0) * (0.75 + 0.25 * std::min(1.0, f.translucency / 0.22));
  if (t.lite) s *= 0.5;
  auto floor_of = [](std::initializer_list<double> floors) { return std::max(floors); };
  auto toward = [s](double floor) { return 1.0 - s * (1.0 - floor); };
  t.opacity = toward(floor_of({readable_opacity(t.text, t.panel_bg, 4.5, true),
                               readable_opacity(t.text_dim, t.panel_bg, 3.0, true)}));
  t.popup_opacity = toward(floor_of({readable_opacity(t.menu_text, t.menu_bg, 4.5, true),
                                     readable_opacity(t.text_dim, t.menu_bg, 3.0, true),
                                     readable_opacity(t.menu_hover_text, t.menu_hover, 3.0, true)}));
  // Menus of other toolkits fade as a whole, like SDE's without Liquid Glass.
  t.toolkit_menu_opacity = t.popup_opacity;
  if (t.liquid) {
    // Liquid Glass: the shader tints only the surface color with the
    // backdrop, keeps text and icons opaque and lifts (or deepens) the glass
    // under text where the backdrop would make it hard to read, so the glass
    // can stay clear: a 32 % tint at Subtle (25), 14 % at 100.
    t.opacity = t.popup_opacity = 0.50 - 0.36 * std::sqrt(t.transparency / 100.0);
  }
  // Frames: translucent title bars need blur behind them to stay calm;
  // Glass goes all the way, the other families part of it.
  double frame = toward(readable_opacity(t.titlebar_text, t.titlebar_bg, 4.5, true));
  t.frame_opacity = t.lite ? 1.0 : f.glass ? frame : 1.0 - (1.0 - frame) * 0.6;
  t.gtk_menu_alpha = toward(floor_of({readable_opacity(t.menu_text, t.menu_bg, 4.5, false),
                                      readable_opacity(t.text_dim, t.menu_bg, 3.0, false)}));
}

Theme Theme::make(const std::string &family, bool dark, const std::string &accent) {
  Theme t;
  t.dark = dark;
  t.mode = dark ? "dark" : "light";
  t.accent_name = accent_index(accent) < 0 ? palettes::kDefaultAccent : accent;
  t.font = "Sans";
  fill_palette(t, family_info(family), false);
  return t;
}

Theme Theme::from_config(const Config &cfg) {
  Theme t;
  std::string sc = lower(cfg.get("appearance", "scale", "auto"));
  if (sc == "auto" || sc.empty()) set_ui_scale(auto_scale());
  else set_ui_scale(cfg.get_int("appearance", "scale", 100) / 100.0);
  t.scale = ui_scale();
  ThemeChoice c = theme_choice(cfg);
  t.mode = c.mode;
  t.dark = theme_mode_dark(c.mode);
  t.accent_name = c.accent;
  bool custom = !c.custom_accent.empty() && Color::parse(c.custom_accent, t.accent);
  const palettes::Family &f = family_info(c.family);
  fill_palette(t, f, custom);
  t.font = cfg.get("appearance", "font", "Sans");
  // Config uses points; at 96 DPI one point is 4/3 pixel.
  int pt = cfg.get_int("appearance", "font_size", 10);
  if (pt < 7) pt = 7;
  if (pt > 20) pt = 20;
  t.font_size = S(pt * 4.0 / 3.0);
  int h = cfg.get_int("panel", "height", 40);
  t.panel_size = (h == 32 || h == 48) ? h : 40;
  t.panel_height = S(t.panel_size);
  t.panel_top = lower(cfg.get("panel", "position", "bottom")) == "top";
  // Translucency (Acrylic, vibrancy, glass) needs [aero] enabled and is
  // turned off by low resource mode.
  t.translucent = cfg.get_bool("aero", "enabled", true) && !cfg.get_bool("appearance", "low_resource", false);
  t.glass = f.glass && t.translucent;
  t.transparency = std::max(0, std::min(100, cfg.get_int("aero", "transparency", 25)));
  t.lite = compositor_lite(cfg);
  t.blur = t.translucent && !t.lite && cfg.get_bool("aero", "blur", true);
  // Liquid Glass needs SDE's own OpenGL configuration of the compositor
  // (blur and the shader): not on the lite profile, with XRender, with the
  // compositor off or with a compositor.conf of the user's own.
  t.liquid = f.liquid.on && t.glass && t.blur && t.transparency > 0 && compositor_backend(cfg) == "glx";
  apply_transparency(t, f);
  return t;
}

static Theme g_active;
const Theme &active_theme() { return g_active; }

void Theme::apply_to_fltk() const {
  g_active = *this;
  Fl::scheme("gtk+");
  // The scheme resets FLTK's box types; give the stock boxes soft corners
  // again (menu.cpp).
  install_soft_boxes();
  auto u = [](double v) { return uchar(std::lround(v * 255)); };
  Fl::background(u(background.r), u(background.g), u(background.b));
  Fl::background2(u(surface.r), u(surface.g), u(surface.b));
  Fl::foreground(u(text.r), u(text.g), u(text.b));
  Fl::set_color(FL_SELECTION_COLOR, accent.fl());
  Fl::set_color(FL_INACTIVE_COLOR, text_dim.fl());
  // FLTK names fonts with a leading space for "regular" and 'B' for bold.
  static std::string regular, bold;
  regular = " " + font;
  bold = "B" + font;
  Fl::set_font(FL_HELVETICA, regular.c_str());
  Fl::set_font(FL_HELVETICA_BOLD, bold.c_str());
  FL_NORMAL_SIZE = font_size;
}

}  // namespace sde
