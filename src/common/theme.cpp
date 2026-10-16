// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"

#include <FL/Fl.H>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "config.h"
#include "util.h"

namespace sde {

const AccentChoice kAccentChoices[] = {
    {"Teal", "#167D9A"},   {"Blue", "#2F6FDB"},   {"Purple", "#7A4FD1"},
    {"Pink", "#C2407A"},   {"Orange", "#D9682B"}, {"Green", "#2E8B57"},
    {"Graphite", "#5A6472"},
};
const int kAccentChoiceCount = int(sizeof(kAccentChoices) / sizeof(kAccentChoices[0]));

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

Theme Theme::from_config(const Config &cfg) {
  Theme t;
  t.dark = lower(cfg.get("appearance", "theme", "light")) == "dark";
  if (!Color::parse(cfg.get("appearance", "accent", "#167D9A"), t.accent))
    t.accent = Color::hex(0x167D9A);
  t.on_accent = t.accent.luminance() > 0.45 ? Color::hex(0x17212B) : Color::hex(0xFFFFFF);
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
  t.font = cfg.get("appearance", "font", "Sans");
  // Config uses points; at 96 DPI one point is 4/3 pixel.
  int pt = cfg.get_int("appearance", "font_size", 10);
  if (pt < 7) pt = 7;
  if (pt > 20) pt = 20;
  t.font_size = int(std::lround(pt * 4.0 / 3.0));
  int h = cfg.get_int("panel", "height", 40);
  t.panel_height = (h == 32 || h == 48) ? h : 40;
  return t;
}

void Theme::apply_to_fltk() const {
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
