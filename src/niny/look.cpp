// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny's colors: chrome derived from the SDE theme family and accent, and
// two syntax palettes (light and dark) tuned for contrast on the editor.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <cstring>

#include "config.h"
#include "niny.h"
#include "util.h"

using namespace sde;

namespace niny {

namespace {

Look g_look;

// The theme family (newer SDE releases keep it in [appearance] style).
std::string theme_family(const Config &cfg) {
  std::string style = lower(cfg.get("appearance", "style", ""));
  if (style == "sde" || style == "aero" || style == "liquid-glass") return style;
  // Alpha 16 families: Graphite (flat), Fluent (Mica-like), Glass (frosted).
  if (style == "graphite") return "sde";
  if (style == "fluent") return "aero";
  if (style == "glass") return "liquid-glass";
  return cfg.get_bool("aero", "enabled", true) ? "aero" : "sde";
}

struct SyntaxPalette {
  uint32_t c[kStyleCount];
};

// Plain, keyword, type, string, comment, number, preproc, function,
// operator, tag, attribute, added, removed, heading, constant, escape,
// special, strong, emphasis, builtin.
const SyntaxPalette kLight = {{0x1E2731, 0x8B2FC9, 0x0B7A8C, 0x1D7A3A, 0x6F7A86, 0xB3560B, 0xC0306B, 0x1F5FBF,
                         0x5B6675, 0x1652B5, 0x9A4A06, 0x1D7A3A, 0xC4262E, 0x1652B5, 0x7A3FC2, 0x0E7490,
                         0xB4520A, 0x1E2731, 0x1E2731, 0x0B7A8C}};
const SyntaxPalette kDark = {{0xE3E9F0, 0xC79BF2, 0x6FD3C9, 0xA6D98A, 0x7F8B99, 0xF4A26B, 0xFF84B4, 0x86B4FF,
                        0x8FD6F2, 0x7FA8F8, 0xF2CA74, 0x86E39A, 0xFF8A80, 0x86B4FF, 0xD2A8FF, 0x8FD6F2,
                        0xF2CA74, 0xE3E9F0, 0xE3E9F0, 0x6FD3C9}};

}  // namespace

Look make_look(const Config &cfg, const std::string &scheme) {
  Look L;
  L.theme = active_theme();
  const Theme &t = L.theme;
  L.style = theme_family(cfg);
  L.dark = scheme == "dark" ? true : scheme == "light" ? false : t.dark;
  L.glass = L.style != "sde" && t.glass;
  L.liquid = L.style == "liquid-glass";
  L.radius = L.liquid ? 12 : L.style == "aero" ? 8 : 6;
  L.accent = t.accent;
  L.on_accent = t.on_accent;
  L.accent_text = t.accent_text;
  if (L.dark == t.dark) {
    L.window = t.background;
    L.text = t.text;
    L.dim = t.text_dim;
    L.border = t.border;
    L.hover = t.hover;
    L.card = t.surface;
    L.field = t.surface_alt;
  } else if (L.dark) {  // dark editor in a light desktop
    L.window = Color::hex(0x161B22);
    L.text = Color::hex(0xEDF2F7);
    L.dim = Color::hex(0xA9B4C2);
    L.border = Color::hex(0x384252);
    L.hover = Color::hex(0x323B48);
    L.card = Color::hex(0x1F2630);
    L.field = Color::hex(0x272F3A);
  } else {
    L.window = Color::hex(0xF5F7FA);
    L.text = Color::hex(0x17212B);
    L.dim = Color::hex(0x5B6675);
    L.border = Color::hex(0xD5DBE3);
    L.hover = Color::hex(0xE3E8EF);
    L.card = Color::hex(0xFFFFFF);
    L.field = Color::hex(0xEEF1F5);
  }
  // Accent text readable on the chosen background.
  L.accent_text = L.accent;
  for (int i = 1; i <= 20 && L.accent_text.contrast(L.card) < 4.5; ++i) L.accent_text = L.accent.mix(L.text, i * 0.05);
  L.shadow = Color::hex(0x000000, L.dark ? 0.5 : 0.18);
  if (L.dark) {
    L.editor = Color::hex(0x12161D).mix(L.window, 0.35);
    L.header_top = L.window.mix(L.accent, L.glass ? 0.10 : 0.0);
    L.header_bottom = L.window;
    L.sidebar = L.window.mix(Color::hex(0x000000), 0.12);
    L.status = L.window;
    L.tab_active = L.editor;
    L.current_line = Color::hex(0xFFFFFF, 0.045);
    L.gutter = L.editor;
    L.gutter_text = L.dim.mix(L.editor, 0.45);
    L.selection = L.accent.mix(L.editor, 0.52);
    L.margin = Color::hex(0xFFFFFF, 0.07);
  } else {
    L.editor = Color::hex(0xFFFFFF);
    L.header_top = L.window.mix(L.accent, L.glass ? 0.09 : 0.0).mix(Color::hex(0xFFFFFF), L.glass ? 0.25 : 0);
    L.header_bottom = L.window.mix(L.accent, L.glass ? 0.03 : 0.0);
    L.sidebar = L.window;
    L.status = L.window;
    L.tab_active = L.editor;
    L.current_line = L.accent.alpha(0.065);
    L.gutter = Color::hex(0xFFFFFF);
    L.gutter_text = L.dim.mix(L.editor, 0.4);
    L.selection = L.accent.mix(L.editor, 0.58);
    L.margin = L.border.alpha(0.6);
  }
  if (L.liquid) {
    // Frosted: lighter, more translucent looking chrome.
    L.header_top = L.header_top.mix(Color::hex(0xFFFFFF), L.dark ? 0.04 : 0.35);
    L.sidebar = L.sidebar.mix(L.accent, 0.035);
  }
  L.header_line = L.border.alpha(L.dark ? 0.9 : 0.8);
  L.tab_hover = L.hover.alpha(0.8);
  L.tab_text = L.text;
  L.tab_text_dim = L.dim;
  L.gutter_current = L.text;
  L.sidebar_active = L.accent.alpha(L.dark ? 0.26 : 0.14);
  L.card_border = L.border;
  L.match = Color::hex(0xF2B84B, L.dark ? 0.32 : 0.42);
  L.match_current = L.accent;
  L.bracket = L.accent;
  // Tab strip (tinted like a Mica title bar) and the lighter bar below it.
  if (L.dark) {
    L.strip = L.window.mix(Color::hex(0x000000), 0.38).mix(L.accent, 0.04);
    L.bar = L.editor.mix(Color::hex(0xFFFFFF), 0.06);
  } else {
    L.strip = L.window.mix(L.accent, 0.075).mix(Color::hex(0x8090A8), 0.06);
    L.bar = Color::hex(0xFBFBFC).mix(L.window, 0.25);
  }
  L.strip_bottom = L.strip;
  if (L.glass) {
    L.strip = L.strip.mix(L.accent, L.dark ? 0.05 : 0.06).mix(Color::hex(0xFFFFFF), L.dark ? 0.0 : 0.15);
    L.strip_bottom = L.strip.mix(L.window, 0.35);
  }
  L.bar_line = L.border.alpha(L.dark ? 0.75 : 0.7);
  L.status = L.bar;
  L.tab_active = L.bar;
  const SyntaxPalette &pal = L.dark ? kDark : kLight;
  for (int i = 0; i < kStyleCount; ++i) L.syntax[i] = Color::hex(pal.c[i]);
  L.syntax[0] = L.text.mix(L.syntax[0], 0.5);
  L.bold[kKeyword - 'A'] = L.bold[kHeading - 'A'] = L.bold[kStrong - 'A'] = true;
  L.italic[kComment - 'A'] = L.italic[kEmphasis - 'A'] = true;
  return L;
}

const Look &look() { return g_look; }
void set_look(const Look &l) { g_look = l; }

const char *language_badge(const Language *lang) {
  static const struct {
    const char *id, *badge;
  } k[] = {{"c", "C"},       {"cpp", "C++"},     {"python", "PY"},  {"shell", "SH"},    {"javascript", "JS"},
           {"typescript", "TS"}, {"json", "{ }"}, {"html", "<>"},   {"xml", "XML"},     {"css", "CSS"},
           {"markdown", "MD"}, {"ini", "INI"},    {"rust", "RS"},    {"go", "GO"},       {"makefile", "MK"},
           {"yaml", "YML"},  {"diff", "+/-"},     {"plain", "TXT"}};
  const std::string &id = language_id(lang);
  for (auto &e : k)
    if (id == e.id) return e.badge;
  return "TXT";
}

Color language_color(const Language *lang) {
  static const struct {
    const char *id;
    uint32_t c;
  } k[] = {{"c", 0x5C6BC0},        {"cpp", 0x3F7FD9},   {"python", 0x3A78B5}, {"shell", 0x3E9B57},
           {"javascript", 0xC9A300}, {"typescript", 0x2F74C0}, {"json", 0xD08A1E}, {"html", 0xE0583A},
           {"xml", 0xD9762F},      {"css", 0x7A4FD1},   {"markdown", 0x5B6B82}, {"ini", 0x6B7787},
           {"rust", 0xC05A2B},     {"go", 0x14A0B8},    {"makefile", 0x8A6A3E}, {"yaml", 0xC2407A},
           {"diff", 0x2E9E5B}};
  const std::string &id = language_id(lang);
  for (auto &e : k)
    if (id == e.id) return Color::hex(e.c);
  return look().dim.mix(look().card, 0.25);
}

void draw_badge(Painter &p, const Language *lang, double x, double y, double size) {
  Color c = language_color(lang);
  p.fill_rounded(x, y, size, size, size * 0.28, c.alpha(look().dark ? 0.30 : 0.16));
  p.stroke_rounded(x, y, size, size, size * 0.28, c.alpha(0.55), 1);
}

void draw_badge_text(const Language *lang, double x, double y, double size) {
  const char *b = language_badge(lang);
  int fs = int(size * (strlen(b) >= 3 ? 0.36 : 0.44));
  fl_font(FL_HELVETICA_BOLD, fs < 7 ? 7 : fs);
  Color c = language_color(lang);
  Color t = look().dark ? c.mix(Color::hex(0xFFFFFF), 0.45) : c.mix(Color::hex(0x000000), 0.15);
  fl_color(t.fl());
  fl_draw(b, int(x), int(y), int(size), int(size), FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
}

void style_input(Fl_Input *in, bool large) {
  const Look &L = look();
  in->box(FL_FLAT_BOX);
  in->color(L.field.fl());
  in->selection_color(L.selection.fl());
  in->textcolor(L.text.fl());
  in->cursor_color(L.accent.fl());
  in->textfont(FL_HELVETICA);
  in->textsize(L.theme.font_size + (large ? S(2) : 0));
}

}  // namespace niny
