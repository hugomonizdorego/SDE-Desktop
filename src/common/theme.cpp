// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"

#include <FL/Fl.H>
#include <FL/x.H>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "config.h"
#include "util.h"

namespace sde {

const AccentChoice kAccentChoices[] = {
    {"Teal", "#167D9A"},   {"Blue", "#2F6FDB"},   {"Purple", "#7A4FD1"},
    {"Pink", "#C2407A"},   {"Orange", "#D9682B"}, {"Green", "#277A4C"},
    {"Graphite", "#5A6472"},
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

Theme Theme::from_config(const Config &cfg) {
  Theme t;
  t.dark = lower(cfg.get("appearance", "theme", "light")) == "dark";
  if (!Color::parse(cfg.get("appearance", "accent", "#167D9A"), t.accent))
    t.accent = Color::hex(0x167D9A);
  Color black = Color::hex(0x17212B), white = Color::hex(0xFFFFFF);
  t.on_accent = t.accent.contrast(black) > t.accent.contrast(white) ? black : white;
  if (t.dark) {
    t.background = Color::hex(0x161B22);
    t.surface = Color::hex(0x1F2630);
    t.surface_alt = Color::hex(0x272F3A);
    t.hover = Color::hex(0x323B48);
    t.text = Color::hex(0xEDF2F7);
    t.text_dim = Color::hex(0xA9B4C2);
    t.border = Color::hex(0x384252);
    t.shadow = Color::hex(0x000000, 0.45);
  } else {
    t.background = Color::hex(0xF5F7FA);
    t.surface = Color::hex(0xFFFFFF);
    t.surface_alt = Color::hex(0xEEF1F5);
    t.hover = Color::hex(0xE3E8EF);
    t.text = Color::hex(0x17212B);
    t.text_dim = Color::hex(0x5B6675);
    t.border = Color::hex(0xD5DBE3);
    t.shadow = Color::hex(0x0B1622, 0.22);
  }
  std::string sc = lower(cfg.get("appearance", "scale", "auto"));
  if (sc == "auto" || sc.empty()) set_ui_scale(auto_scale());
  else set_ui_scale(cfg.get_int("appearance", "scale", 100) / 100.0);
  t.scale = ui_scale();
  // Accent used for text: move it toward the text color until it reaches
  // the WCAG AA ratio (4.5:1) on both surfaces.
  t.accent_text = t.accent;
  for (int i = 1; i <= 20 && (t.accent_text.contrast(t.surface) < 4.5 ||
                              t.accent_text.contrast(t.background) < 4.5);
       ++i)
    t.accent_text = t.accent.mix(t.text, i * 0.05);
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
  return t;
}

static Theme g_active;
const Theme &active_theme() { return g_active; }

void Theme::apply_to_fltk() const {
  g_active = *this;
  Fl::scheme("gtk+");
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
