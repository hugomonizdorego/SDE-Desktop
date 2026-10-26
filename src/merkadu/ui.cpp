// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <map>

#include "buffered_window.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "image_cache.h"
#include "logo_anim.h"
#include "store.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"
#include "wm.h"

namespace sde {
namespace merkadu {

// ------------------------------------------------------------- painting

Color source_color(Source s) {
  switch (s) {
    case Source::Flatpak: return Color::hex(0x3876B9);
    case Source::Snap: return Color::hex(0xC8742F);
    default: return Color::hex(0xC9564E);
  }
}

Color readable(const Color &c, const Color &bg) {
  // Darken (light themes) or lighten (dark themes) until the contrast is 4.5:1.
  bool dark_bg = bg.luminance() < 0.4;
  Color out = c;
  for (int i = 0; i < 10 && out.contrast(bg) < 4.5; ++i)
    out = out.mix(dark_bg ? Color::hex(0xFFFFFF) : Color::hex(0x000000), 0.15);
  return out;
}

int badge_width(const std::string &text) {
  text_font(std::max(S(9), active_theme().font_size - S(2)), true);
  return text_width(text) + S(14);
}

void paint_badge(Painter &p, double x, double y, double w, double h, const Color &c) {
  const Theme &t = active_theme();
  p.fill_rounded(x, y, w, h, h / 2, c.alpha(t.dark ? 0.26 : 0.14));
}

void draw_badge_text(const std::string &text, int x, int y, int w, int h, const Color &c) {
  const Theme &t = active_theme();
  text_font(std::max(S(9), t.font_size - S(2)), true);
  draw_text(text, x, y, w, h, readable(c, t.surface), 1);
}

namespace {

// |text| in at most |max_lines| lines of |w| pixels (current font), drawn
// from |y| down; the last line ends with an ellipsis when text is left.
void draw_lines(const std::string &text, int x, int y, int w, int max_lines) {
  std::vector<std::string> words;
  std::string cur;
  for (char c : text) {
    if (c == ' ' || c == '\n' || c == '\t') {
      if (!cur.empty()) words.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) words.push_back(cur);
  std::vector<std::string> lines;
  std::string line;
  for (size_t k = 0; k < words.size() && max_lines > 0; ++k) {
    if (int(lines.size()) == max_lines - 1) {
      for (size_t j = k; j < words.size(); ++j) line += (line.empty() ? "" : " ") + words[j];
      break;
    }
    std::string candidate = line.empty() ? words[k] : line + " " + words[k];
    if (line.empty() || fl_width(candidate.c_str()) <= w) {
      line = candidate;
      continue;
    }
    lines.push_back(line);
    line = words[k];
  }
  if (!line.empty() && max_lines > 0) lines.push_back(line);
  int lh = fl_height();
  for (size_t i = 0; i < lines.size(); ++i)
    fl_draw(ellipsize(lines[i], w).c_str(), x, y + int(i) * lh + lh - fl_descent());
}

std::vector<Source> badge_sources(const App &a) {
  std::vector<Source> out;
  for (auto &o : a.origins)
    if (std::find(out.begin(), out.end(), o.source) == out.end()) out.push_back(o.source);
  return out;
}

}  // namespace

int paint_source_badges(Painter &p, const App &a, int x, int y, int h, int max_w) {
  int cx = x;
  for (Source s : badge_sources(a)) {
    int w = badge_width(source_label(s));
    if (cx + w - x > max_w) break;
    paint_badge(p, cx, y, w, h, source_color(s));
    cx += w + S(6);
  }
  return cx;
}

void draw_source_badges(const App &a, int x, int y, int h, int max_w) {
  int cx = x;
  for (Source s : badge_sources(a)) {
    std::string label = source_label(s);
    int w = badge_width(label);
    if (cx + w - x > max_w) break;
    draw_badge_text(label, cx, y, w, h, source_color(s));
    cx += w + S(6);
  }
}

void paint_progress(Painter &p, double x, double y, double w, double h, int percent, double phase) {
  const Theme &t = active_theme();
  p.fill_rounded(x, y, w, h, h / 2, t.border.alpha(0.6));
  if (percent >= 0) {
    double fw = std::max(h, w * std::min(100, percent) / 100.0);
    p.fill_rounded(x, y, fw, h, h / 2, t.accent);
  } else {
    // Unknown progress: a segment sliding along.
    double seg = w * 0.3, pos = std::fmod(phase, 1.0) * (w + seg) - seg;
    double sx = std::max(x, x + pos), ex = std::min(x + w, x + pos + seg);
    if (ex > sx) p.fill_rounded(sx, y, ex - sx, h, h / 2, t.accent);
  }
}

void paint_card(Painter &p, double x, double y, double w, double h, bool hot, bool focus) {
  const Theme &t = active_theme();
  double r = std::min(double(t.radius), Sd(12));
  p.fill_rounded(x, y, w, h, r, hot ? t.hover : t.surface);
  p.stroke_rounded(x + 0.5, y + 0.5, w - 1, h - 1, r, hot ? t.accent : t.border);
  if (focus) focus_ring(p, x, y, w, h, r);
}

void paint_symbol(Painter &p, Symbol s, double x, double y, double size, const Color &c) {
  cairo_t *cr = p.cr();
  double u = size / 20.0;
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, u, u);
  p.set(c);
  cairo_set_line_width(cr, 1.6);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  auto rr = [&](double rx, double ry, double rw, double rh, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, rx + rw - r, ry + r, r, -M_PI / 2, 0);
    cairo_arc(cr, rx + rw - r, ry + rh - r, r, 0, M_PI / 2);
    cairo_arc(cr, rx + r, ry + rh - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, rx + r, ry + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
  };
  switch (s) {
    case Symbol::Explore:  // a compass
      cairo_arc(cr, 10, 10, 7.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 13.2, 6.8);
      cairo_line_to(cr, 11.2, 11.2);
      cairo_line_to(cr, 6.8, 13.2);
      cairo_line_to(cr, 8.8, 8.8);
      cairo_close_path(cr);
      cairo_fill(cr);
      break;
    case Symbol::Installed:  // a box with a check
      rr(3, 5, 14, 12, 2);
      cairo_stroke(cr);
      cairo_move_to(cr, 3, 8.5);
      cairo_line_to(cr, 17, 8.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 7.5, 12.6);
      cairo_line_to(cr, 9.4, 14.4);
      cairo_line_to(cr, 12.8, 10.8);
      cairo_stroke(cr);
      break;
    case Symbol::Updates:
    case Symbol::Refresh:  // circular arrows
      cairo_arc(cr, 10, 10, 6.5, -M_PI * 0.15, M_PI * 0.75);
      cairo_stroke(cr);
      cairo_arc(cr, 10, 10, 6.5, M_PI * 0.85, M_PI * 1.75);
      cairo_stroke(cr);
      cairo_move_to(cr, 16.5, 4.2);
      cairo_line_to(cr, 15.9, 8.1);
      cairo_line_to(cr, 12.2, 7.0);
      cairo_stroke(cr);
      cairo_move_to(cr, 3.5, 15.8);
      cairo_line_to(cr, 4.1, 11.9);
      cairo_line_to(cr, 7.8, 13.0);
      cairo_stroke(cr);
      break;
    case Symbol::History:
    case Symbol::Clock:
      cairo_arc(cr, 10, 10, 7.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 10, 5.5);
      cairo_line_to(cr, 10, 10);
      cairo_line_to(cr, 13, 12);
      cairo_stroke(cr);
      break;
    case Symbol::Progress:
    case Symbol::Download:  // an arrow into a tray
      cairo_move_to(cr, 10, 3);
      cairo_line_to(cr, 10, 12.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 6, 8.8);
      cairo_line_to(cr, 10, 12.8);
      cairo_line_to(cr, 14, 8.8);
      cairo_stroke(cr);
      cairo_move_to(cr, 3.5, 13);
      cairo_line_to(cr, 3.5, 16.5);
      cairo_line_to(cr, 16.5, 16.5);
      cairo_line_to(cr, 16.5, 13);
      cairo_stroke(cr);
      break;
    case Symbol::Sources:  // stacked layers
      cairo_move_to(cr, 10, 3);
      cairo_line_to(cr, 17.5, 7);
      cairo_line_to(cr, 10, 11);
      cairo_line_to(cr, 2.5, 7);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 2.5, 10.5);
      cairo_line_to(cr, 10, 14.5);
      cairo_line_to(cr, 17.5, 10.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 2.5, 14);
      cairo_line_to(cr, 10, 18);
      cairo_line_to(cr, 17.5, 14);
      cairo_stroke(cr);
      break;
    case Symbol::Search:
      cairo_arc(cr, 8.5, 8.5, 5.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 12.6, 12.6);
      cairo_line_to(cr, 17, 17);
      cairo_stroke(cr);
      break;
    case Symbol::Back:
      cairo_move_to(cr, 12.5, 4);
      cairo_line_to(cr, 6.5, 10);
      cairo_line_to(cr, 12.5, 16);
      cairo_stroke(cr);
      break;
    case Symbol::Check:
      cairo_move_to(cr, 4.5, 10.5);
      cairo_line_to(cr, 8.5, 14.5);
      cairo_line_to(cr, 15.5, 6);
      cairo_stroke(cr);
      break;
    case Symbol::Plus:
    case Symbol::Minus:
      cairo_move_to(cr, 4.5, 10);
      cairo_line_to(cr, 15.5, 10);
      cairo_stroke(cr);
      if (s == Symbol::Plus) {
        cairo_move_to(cr, 10, 4.5);
        cairo_line_to(cr, 10, 15.5);
        cairo_stroke(cr);
      }
      break;
    case Symbol::Warning:
      cairo_move_to(cr, 10, 3);
      cairo_line_to(cr, 18, 17);
      cairo_line_to(cr, 2, 17);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 10, 8);
      cairo_line_to(cr, 10, 12);
      cairo_stroke(cr);
      cairo_arc(cr, 10, 14.6, 0.9, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Symbol::Trash:
      cairo_move_to(cr, 3.5, 5.5);
      cairo_line_to(cr, 16.5, 5.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 8, 5.5);
      cairo_line_to(cr, 8, 3.5);
      cairo_line_to(cr, 12, 3.5);
      cairo_line_to(cr, 12, 5.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 5, 5.5);
      cairo_line_to(cr, 6, 17);
      cairo_line_to(cr, 14, 17);
      cairo_line_to(cr, 15, 5.5);
      cairo_stroke(cr);
      break;
    case Symbol::Shield:
      cairo_move_to(cr, 10, 2.5);
      cairo_line_to(cr, 16.5, 5);
      cairo_curve_to(cr, 16.5, 11, 14, 15.5, 10, 17.5);
      cairo_curve_to(cr, 6, 15.5, 3.5, 11, 3.5, 5);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Symbol::Folder:
      cairo_move_to(cr, 2.5, 6);
      cairo_line_to(cr, 2.5, 15.5);
      cairo_line_to(cr, 17.5, 15.5);
      cairo_line_to(cr, 17.5, 7);
      cairo_line_to(cr, 9.5, 7);
      cairo_line_to(cr, 8, 4.5);
      cairo_line_to(cr, 2.5, 4.5);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Symbol::Star:
      for (int i = 0; i < 10; ++i) {
        double a = -M_PI / 2 + i * M_PI / 5, r = i % 2 ? 3.4 : 7.6;
        if (i == 0)
          cairo_move_to(cr, 10 + r * std::cos(a), 10.6 + r * std::sin(a));
        else
          cairo_line_to(cr, 10 + r * std::cos(a), 10.6 + r * std::sin(a));
      }
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Symbol::Close:
      cairo_move_to(cr, 5, 5);
      cairo_line_to(cr, 15, 15);
      cairo_move_to(cr, 15, 5);
      cairo_line_to(cr, 5, 15);
      cairo_stroke(cr);
      break;
    case Symbol::Forward:
      cairo_move_to(cr, 7.5, 4);
      cairo_line_to(cr, 13.5, 10);
      cairo_line_to(cr, 7.5, 16);
      cairo_stroke(cr);
      break;
    case Symbol::Home:  // a house
      cairo_move_to(cr, 2.8, 9.6);
      cairo_line_to(cr, 10, 3.2);
      cairo_line_to(cr, 17.2, 9.6);
      cairo_stroke(cr);
      cairo_move_to(cr, 4.8, 8.2);
      cairo_line_to(cr, 4.8, 16.5);
      cairo_line_to(cr, 15.2, 16.5);
      cairo_line_to(cr, 15.2, 8.2);
      cairo_stroke(cr);
      rr(8.4, 11.2, 3.2, 5.3, 0.8);
      cairo_stroke(cr);
      break;
    case Symbol::Package:  // a box with its lid
      cairo_move_to(cr, 10, 2.8);
      cairo_line_to(cr, 16.8, 6.3);
      cairo_line_to(cr, 16.8, 13.8);
      cairo_line_to(cr, 10, 17.3);
      cairo_line_to(cr, 3.2, 13.8);
      cairo_line_to(cr, 3.2, 6.3);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 3.2, 6.3);
      cairo_line_to(cr, 10, 9.8);
      cairo_line_to(cr, 16.8, 6.3);
      cairo_move_to(cr, 10, 9.8);
      cairo_line_to(cr, 10, 17.3);
      cairo_stroke(cr);
      cairo_move_to(cr, 6.6, 4.5);
      cairo_line_to(cr, 13.4, 8.0);
      cairo_stroke(cr);
      break;
    case Symbol::Info:
      cairo_arc(cr, 10, 10, 7.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 10, 9);
      cairo_line_to(cr, 10, 14);
      cairo_stroke(cr);
      cairo_arc(cr, 10, 6.4, 0.95, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Symbol::Grid:  // four tiles
      rr(3, 3, 6, 6, 1.5);
      rr(11, 3, 6, 6, 1.5);
      rr(3, 11, 6, 6, 1.5);
      rr(11, 11, 6, 6, 1.5);
      cairo_stroke(cr);
      break;
    case Symbol::Snap:  // a bird's wing: two strokes meeting
      cairo_move_to(cr, 4, 4.5);
      cairo_line_to(cr, 15.5, 8.5);
      cairo_line_to(cr, 9, 11);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 9, 11);
      cairo_line_to(cr, 12.5, 16);
      cairo_line_to(cr, 15.5, 8.5);
      cairo_stroke(cr);
      break;
    case Symbol::Sparkle:  // a four-pointed star
      cairo_move_to(cr, 10, 2.5);
      cairo_curve_to(cr, 10.8, 7.8, 12.2, 9.2, 17.5, 10);
      cairo_curve_to(cr, 12.2, 10.8, 10.8, 12.2, 10, 17.5);
      cairo_curve_to(cr, 9.2, 12.2, 7.8, 10.8, 2.5, 10);
      cairo_curve_to(cr, 7.8, 9.2, 9.2, 7.8, 10, 2.5);
      cairo_close_path(cr);
      cairo_fill(cr);
      break;
  }
  cairo_restore(cr);
}

namespace {

bool icon_name(const std::string &ref) {
  return !ref.empty() && ref[0] != '/' && !starts_with(ref, "https://") && !starts_with(ref, "http://") &&
         !starts_with(ref, "snap-icon:");
}

// A theme icon, only when the theme has it (IconLoader::get() would give
// the generic application icon instead).
cairo_surface_t *theme_icon(const std::string &name, int size) {
  if (fake_mode()) {
    // The fake sources' launchers name the icons of DIR/icons.
    std::string file = fake_dir() + "/icons/" + name + ".svg";
    if (file_exists(file)) return images::icon_file(file, size);
  }
  if (IconLoader::lookup(name, size).empty()) return nullptr;
  return IconLoader::get({name}, size);
}

Color hue_color(const std::string &seed) {
  // Calm colors of the SDE palette, chosen by the name.
  static const uint32_t palette[] = {0x3876B9, 0x4A9157, 0xC8742F, 0x8E5BB5, 0xC9564E, 0x2E8C8C, 0x5867C2, 0x9A7B2E};
  unsigned h = 2166136261u;
  for (unsigned char c : seed) h = (h ^ c) * 16777619u;
  return Color::hex(palette[h % (sizeof palette / sizeof palette[0])]);
}

}  // namespace

cairo_surface_t *app_icon(const App &a, int size) {
  for (const std::string &ref : icon_candidates(a, size)) {
    cairo_surface_t *s = icon_name(ref) ? theme_icon(ref, size) : images::icon_file(ref, size);
    if (s) return s;
  }
  for (const std::string &name : fallback_icon_names(a))
    if (cairo_surface_t *s = theme_icon(name, size)) return s;
  return nullptr;
}

void paint_icon_placeholder(Painter &p, double x, double y, int size, const std::string &seed, Symbol s) {
  const Theme &t = active_theme();
  Color c = hue_color(seed);
  double r = size * 0.22;
  p.fill_gradient(x, y, size, size, r, c.mix(Color::hex(0xFFFFFF), t.dark ? 0.05 : 0.2), c.mix(Color::hex(0), 0.12),
                  true);
  double ss = size * 0.56;
  paint_symbol(p, s, x + (size - ss) / 2, y + (size - ss) / 2, ss, Color::hex(0xFFFFFF).alpha(0.92));
}

void paint_app_icon(Painter &p, const App &a, double x, double y, int size) {
  if (cairo_surface_t *s = app_icon(a, size))
    p.image(s, x, y, size);
  else
    paint_icon_placeholder(p, x, y, size, a.name, Symbol::Package);
}

void paint_package_icon(Painter &p, const Package &pkg, const App *app, double x, double y, int size) {
  if (app) {
    paint_app_icon(p, *app, x, y, size);
    return;
  }
  std::vector<std::string> names = section_icon_names(section_base(pkg.section));
  names.push_back("package-x-generic");
  for (auto &n : names)
    if (cairo_surface_t *s = theme_icon(n, size)) {
      p.image(s, x, y, size);
      return;
    }
  paint_icon_placeholder(p, x, y, size, section_base(pkg.section), Symbol::Package);
}

Color icon_tint(const App &a) {
  // Computed again when the icon changes (a download finished).
  static std::map<std::string, std::pair<cairo_surface_t *, Color>> cache;
  cairo_surface_t *s = app_icon(a, 48);
  auto it = cache.find(a.id);
  if (it != cache.end() && it->second.first == s) return it->second.second;
  Color out = hue_color(a.name);
  if (s) {
    // The average of the colorful, opaque pixels.
    cairo_surface_flush(s);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    const unsigned char *data = cairo_image_surface_get_data(s);
    double sr = 0, sg = 0, sb = 0, sw = 0;
    for (int yy = 0; data && yy < h; ++yy)
      for (int xx = 0; xx < w; ++xx) {
        const unsigned char *px = data + yy * stride + xx * 4;  // premultiplied BGRA
        double al = px[3] / 255.0;
        if (al < 0.5) continue;
        double b = px[0] / 255.0 / al, g = px[1] / 255.0 / al, r = px[2] / 255.0 / al;
        double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
        double weight = (mx - mn) + 0.05;  // saturated pixels count most
        sr += r * weight;
        sg += g * weight;
        sb += b * weight;
        sw += weight;
      }
    if (sw > 0) out = Color{std::min(1.0, sr / sw), std::min(1.0, sg / sw), std::min(1.0, sb / sw), 1};
  }
  cache[a.id] = {s, out};
  return out;
}

// --------------------------------------------------------------- button

Button::Button(int x, int y, int w, int h, const std::string &text, Kind kind)
    : Fl_Button(x, y, w, h), text_(text), kind_(kind) {
  box(FL_NO_BOX);
  down_box(FL_NO_BOX);
  copy_tooltip(text.c_str());
  callback([](Fl_Widget *w, void *) {
    auto *b = static_cast<Button *>(w);
    if (b->on_click) b->on_click();
  });
}

void Button::text(const std::string &t) {
  text_ = t;
  copy_tooltip(t.c_str());
  redraw();
}

int Button::fit(const std::string &text) {
  text_font(active_theme().font_size, true);
  return std::max(S(84), text_width(text) + S(32));
}

void Button::draw() {
  const Theme &t = active_theme();
  bool hot = Fl::belowmouse() == this && active_r(), down = value() != 0;
  bool strong = kind_ == Primary || kind_ == Danger;
  Color accent = t.accent;
  if (kind_ == Danger) {
    // White text on the error color: dark themes have a light one, deepened
    // until the text reads (4.5:1).
    accent = t.error;
    for (int i = 0; i < 12 && Color::hex(0xFFFFFF).contrast(accent) < 4.5; ++i)
      accent = accent.mix(Color::hex(0x000000), 0.1);
  }
  {
    Painter p(window()->w(), window()->h());
    double r = t.radius_for(h());
    Color bg = strong ? accent : kind_ == Ghost ? t.surface.alpha(0) : t.surface_alt;
    if (hot) bg = strong ? accent.mix(t.text, 0.12) : t.hover;
    if (down) bg = bg.mix(t.text, 0.15);
    if (!active_r()) bg = t.surface_alt.mix(t.surface, 0.5);
    if (bg.a > 0) p.fill_rounded(x(), y(), w(), h(), r, bg);
    if (!strong && kind_ != Ghost) p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.border);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  text_font(t.font_size, strong);
  Color fg = strong ? (kind_ == Danger ? Color::hex(0xFFFFFF) : t.on_accent) : t.text;
  if (kind_ == Ghost) fg = t.accent_text;
  if (!active_r()) fg = t.text_dim;
  draw_text(text_, x() + S(10), y(), w() - S(20), h(), fg, 1);
}

int Button::handle(int e) {
  if (e == FL_ENTER || e == FL_LEAVE || e == FL_FOCUS || e == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  if (e == FL_KEYBOARD && Fl::focus() == this &&
      (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter || Fl::event_key() == ' ')) {
    do_callback();
    return 1;
  }
  return Fl_Button::handle(e);
}

// ----------------------------------------------------------------- chip

Chip::Chip(int x, int y, int w, int h, const std::string &text, bool on)
    : Fl_Widget(x, y, w, h), dot(Color::hex(0, 0)), text_(text), on_(on) {
  copy_tooltip(text.c_str());
}

int Chip::fit(const std::string &text) {
  text_font(active_theme().font_size, false);
  return text_width(text) + S(30);
}

void Chip::draw() {
  const Theme &t = active_theme();
  bool hot = Fl::belowmouse() == this;
  {
    Painter p(window()->w(), window()->h());
    double r = h() / 2.0;
    Color bg = on_ ? t.accent.alpha(t.dark ? 0.30 : 0.16) : hot ? t.hover : t.surface;
    p.fill_rounded(x(), y(), w(), h(), r, bg);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, on_ ? t.accent : t.border);
    if (dot.a > 0) p.circle(x() + S(14), y() + h() / 2.0, Sd(4), dot);
    if (menu) {
      cairo_t *cr = p.cr();
      double cx = x() + w() - S(17), cy = y() + h() / 2.0;
      p.set(on_ ? t.accent_text : t.text_dim);
      cairo_set_line_width(cr, Sd(1.6));
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_move_to(cr, cx - Sd(4), cy - Sd(2));
      cairo_line_to(cr, cx, cy + Sd(2));
      cairo_line_to(cr, cx + Sd(4), cy - Sd(2));
      cairo_stroke(cr);
    }
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  text_font(t.font_size, on_);
  int dx = dot.a > 0 ? S(10) : 0, mx = menu ? S(16) : 0;
  draw_text(text_, x() + S(10) + dx, y(), w() - S(20) - dx - mx, h(), on_ ? t.accent_text : t.text, 1);
}

int Chip::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      return 1;
    case FL_RELEASE:
      if (Fl::event_inside(this) && on_click) on_click();
      return 1;
    case FL_KEYBOARD:
      if ((Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) && on_click) {
        on_click();
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// -------------------------------------------------------------- AppTile

AppTile::AppTile(int x, int y, int w, int h, const std::string &app_id) : Fl_Widget(x, y, w, h), id_(app_id) {
  if (const App *a = Store::get().app(app_id)) copy_tooltip(a->name.c_str());
}

int AppTile::height() { return S(112); }

void AppTile::draw() {
  const Theme &t = active_theme();
  const App *a = Store::get().app(id_);
  if (!a) return;
  int is = S(56), pad = S(16);
  int tx = x() + pad + is + S(16), tw = w() - (tx - x()) - pad;
  int badge_h = S(20), by = y() + h() - pad - badge_h + S(2);
  {
    Painter p(window()->w(), window()->h());
    if (hot_) p.shadow(x(), y() + S(2), w(), h(), std::min(double(t.radius), Sd(14)), t.shadow.alpha(0.18), S(8));
    paint_card(p, x(), y(), w(), h(), hot_, Fl::focus() == this);
    paint_app_icon(p, *a, x() + pad, y() + pad, is);
    paint_source_badges(p, *a, tx, by, badge_h, tw - S(24));
    if (a->installed()) {
      double cs = S(18);
      p.circle(x() + w() - S(14) - cs / 2, y() + S(14) + cs / 2, cs / 2, t.success.alpha(0.9));
      paint_symbol(p, Symbol::Check, x() + w() - S(14) - cs + S(2), y() + S(14) + S(2), cs - S(4),
                   Color::hex(0xFFFFFF));
    }
  }
  int name_w = tw - (a->installed() ? S(24) : 0);
  text_font(t.font_size + 1, true);
  draw_text(a->name, tx, y() + pad - S(3), name_w, S(22), t.text);
  text_font(std::max(S(9), t.font_size - S(1)));
  fl_color(t.text_dim.fl());
  int sy = y() + pad + S(21), sh = by - sy - S(2);
  draw_lines(a->summary, tx, sy, tw, std::max(1, sh / std::max(1, fl_height())));
  draw_source_badges(*a, tx, by, badge_h, tw - S(24));
}

int AppTile::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
      hot_ = e == FL_ENTER;
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      return 1;
    case FL_RELEASE:
      if (Fl::event_inside(this) && on_open) on_open(id_);
      return 1;
    case FL_KEYBOARD:
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == ' ' || Fl::event_key() == FL_KP_Enter) && on_open) {
        on_open(id_);
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// --------------------------------------------------------------- AppRow

AppRow::AppRow(int x, int y, int w, int h, const std::string &app_id, Mode mode)
    : Fl_Widget(x, y, w, h), id_(app_id), mode_(mode) {
  if (const App *a = Store::get().app(app_id)) copy_tooltip(a->name.c_str());
}

std::string AppRow::action_text() const {
  const App *a = Store::get().app(id_);
  if (!a) return "";
  if (Store::get().active_job(id_)) return "";
  switch (mode_) {
    case Installed: {
      const Origin *o = a->installed_origin();
      if (o && o->source == Source::Debian && Store::get().settings().protected_packages.count(o->id))
        return _("Part of SDE");
      return _("Remove");
    }
    case Updates:
      return _("Update");
    default:
      if (a->installed()) return _("Installed");
      return _("Install");
  }
}

void AppRow::action_box(int *bx, int *by, int *bw, int *bh) const {
  std::string text = action_text();
  *bw = text.empty() ? S(150) : Button::fit(text);
  *bh = S(32);
  *bx = x() + w() - *bw - S(14);
  *by = y() + (h() - *bh) / 2;
}

void AppRow::draw() {
  const Theme &t = active_theme();
  const App *a = Store::get().app(id_);
  if (!a) return;
  const Job *job = Store::get().active_job(id_);
  std::string action = action_text();
  bool can_act = !action.empty() && action != _("Installed") && action != _("Part of SDE");
  bool primary = mode_ != Installed && can_act;
  int bx, by, bw, bh;
  action_box(&bx, &by, &bw, &bh);
  int is = S(44), ix = x() + S(14), tx = ix + is + S(14), tw = bx - tx - S(14);
  text_font(t.font_size + 1, true);
  int name_w = std::min(tw, text_width(a->name) + S(4));
  {
    Painter p(window()->w(), window()->h());
    paint_card(p, x(), y(), w(), h(), hot_, Fl::focus() == this);
    paint_app_icon(p, *a, ix, y() + (h() - is) / 2.0, is);
    paint_source_badges(p, *a, tx + name_w + S(8), y() + h() / 2 - S(20), S(18), tw - name_w - S(8));
    if (job) {
      paint_progress(p, bx, by + bh / 2.0 + S(4), bw, S(6), job->percent, 0);
    } else if (!action.empty()) {
      Color accent = mode_ == Installed ? t.error : t.accent;
      double r = t.radius_for(bh);
      if (!can_act) {
        // A label, not a button.
      } else if (primary) {
        p.fill_rounded(bx, by, bw, bh, r, hot_button_ ? accent.mix(t.text, 0.12) : accent);
      } else {
        p.fill_rounded(bx, by, bw, bh, r, hot_button_ ? t.hover : t.surface_alt);
        p.stroke_rounded(bx + 0.5, by + 0.5, bw - 1, bh - 1, r, accent.alpha(0.7));
      }
    }
  }
  text_font(t.font_size + 1, true);
  draw_text(a->name, tx, y() + h() / 2 - S(22), name_w, S(22), t.text);
  draw_source_badges(*a, tx + name_w + S(8), y() + h() / 2 - S(20), S(18), tw - name_w - S(8));
  text_font(std::max(S(9), t.font_size - S(1)));
  std::string line2 = a->summary;
  if (mode_ == Installed) {
    if (const Origin *o = a->installed_origin()) {
      line2 = origin_line(*o);
      if (o->installed_size > 0) line2 += "  \xC2\xB7  " + format_size(o->installed_size);
    }
  } else if (mode_ == Updates) {
    for (auto &o : a->origins)
      if (o.installed && o.update) {
        line2 = origin_line(o);
        if (!o.installed_version.empty() && !o.version.empty())
          line2 += "  \xC2\xB7  " + o.installed_version + " \xE2\x86\x92 " + o.version;
      }
  }
  draw_text(line2, tx, y() + h() / 2 + S(1), tw, S(20), t.text_dim);
  if (job) {
    text_font(std::max(S(9), t.font_size - S(1)));
    std::string st = job->state == Job::Queued ? std::string(_("Waiting")) : job->status;
    if (job->percent >= 0 && job->state == Job::Running) st += format(" %d%%", job->percent);
    draw_text(st, bx, by - S(4), bw, S(20), t.text_dim, 1);
  } else if (!action.empty()) {
    text_font(t.font_size, primary);
    Color fg = primary ? t.on_accent : can_act ? (mode_ == Installed ? readable(t.error, t.surface) : t.text)
                                               : t.text_dim;
    draw_text(action, bx + S(8), by, bw - S(16), bh, fg, can_act ? 1 : 2);
  }
}

int AppRow::handle(int e) {
  int bx, by, bw, bh;
  action_box(&bx, &by, &bw, &bh);
  auto in_button = [&] {
    int ex = Fl::event_x(), ey = Fl::event_y();
    return ex >= bx && ex < bx + bw && ey >= by && ey < by + bh;
  };
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
      hot_ = e == FL_ENTER;
      hot_button_ = hot_ && in_button();
      redraw();
      return 1;
    case FL_MOVE: {
      bool b = in_button();
      if (b != hot_button_) {
        hot_button_ = b;
        redraw();
      }
      return 1;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      return 1;
    case FL_RELEASE: {
      if (!Fl::event_inside(this)) return 1;
      std::string action = action_text();
      bool can_act = !action.empty() && action != _("Installed") && action != _("Part of SDE");
      if (in_button() && can_act && on_action)
        on_action(id_);
      else if (on_open)
        on_open(id_);
      return 1;
    }
    case FL_KEYBOARD:
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) && on_open) {
        on_open(id_);
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// --------------------------------------------------------------- Banner

Banner::Banner(int x, int y, int w, int h, const std::string &app_id) : Fl_Widget(x, y, w, h), id_(app_id) {
  if (const App *a = Store::get().app(app_id)) copy_tooltip(a->name.c_str());
}

void Banner::draw() {
  const Theme &t = active_theme();
  const App *a = Store::get().app(id_);
  if (!a) return;
  double r = t.radius_large;
  int pad = S(32), plate = std::min(S(120), h() - 2 * pad), is = plate * 3 / 4;
  bool shot = !a->screenshots.empty() && w() > S(700) && !images::failed(a->screenshots[0].thumbnail);
  int shot_w = shot ? std::min(int(w() * 0.40), int((h() - S(40)) * 16 / 9.0)) : 0;
  int tx = x() + pad + plate + S(28), tw = w() - (tx - x()) - pad - (shot ? shot_w + S(28) : 0);
  // The app's own color, deep enough for white text.
  Color base = icon_tint(*a).mix(Color::hex(0x000000), t.dark ? 0.35 : 0.15);
  for (int i = 0; i < 12 && Color::hex(0xFFFFFF).contrast(base) < 4.8; ++i) base = base.mix(Color::hex(0), 0.12);
  Color deep = base.mix(Color::hex(0x000000), 0.38);
  Color white = Color::hex(0xFFFFFF);
  {
    Painter p(window()->w(), window()->h());
    if (hot_) p.shadow(x(), y() + S(3), w(), h(), r, t.shadow.alpha(0.25), S(10));
    p.fill_gradient(x(), y(), w(), h(), r, base, deep, true);
    // Soft light circles: depth without an image.
    cairo_t *cr = p.cr();
    cairo_save(cr);
    p.rounded_rect(x(), y(), w(), h(), r);
    cairo_clip(cr);
    p.circle(x() + w() * 0.86, y() - h() * 0.15, h() * 0.75, white.alpha(0.07));
    p.circle(x() + w() * 0.60, y() + h() * 1.25, h() * 0.6, white.alpha(0.05));
    cairo_restore(cr);
    // The icon on a light plate.
    double px = x() + pad, py = y() + (h() - plate) / 2.0;
    p.shadow(px, py + S(2), plate, plate, plate * 0.24, Color::hex(0).alpha(0.25), S(8));
    p.fill_rounded(px, py, plate, plate, plate * 0.24, white.alpha(0.94));
    paint_app_icon(p, *a, px + (plate - is) / 2.0, py + (plate - is) / 2.0, is);
    if (shot) {
      int sh = int(shot_w * 9 / 16.0), sx = x() + w() - pad - shot_w, sy = y() + (h() - sh) / 2;
      cairo_surface_t *img = images::picture(a->screenshots[0].thumbnail, shot_w, sh);
      p.shadow(sx, sy, shot_w, sh, S(10), Color::hex(0).alpha(0.3), S(10));
      p.fill_rounded(sx, sy, shot_w, sh, S(10), white.alpha(0.12));
      if (img) {
        double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
        cairo_save(cr);
        p.rounded_rect(sx, sy, shot_w, sh, S(10));
        cairo_clip(cr);
        p.surface(img, sx + (shot_w - iw) / 2, sy + (sh - ih) / 2);
        cairo_restore(cr);
      }
    }
    // Source pills.
    int cx = tx, by = y() + h() / 2 + S(38);
    for (Source s : badge_sources(*a)) {
      int bw = badge_width(source_label(s));
      if (cx + bw > tx + tw) break;
      p.fill_rounded(cx, by, bw, S(22), S(11), white.alpha(0.18));
      cx += bw + S(6);
    }
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  text_font(std::max(S(9), t.font_size - S(1)), true);
  draw_text(a->installed() ? _("INSTALLED") : _("HIGHLIGHT"), tx, y() + h() / 2 - S(62), tw, S(20), white.alpha(0.78));
  text_font(t.font_size + S(12), true);
  draw_text(a->name, tx, y() + h() / 2 - S(42), tw, S(42), white);
  text_font(t.font_size + 1);
  fl_color(white.alpha(0.88).mix(base, 0.12).fl());
  draw_lines(a->summary, tx, y() + h() / 2 + S(2), tw, 2);
  int cx = tx, by = y() + h() / 2 + S(38);
  text_font(std::max(S(9), t.font_size - S(2)), true);
  for (Source s : badge_sources(*a)) {
    std::string label = source_label(s);
    int bw = badge_width(label);
    if (cx + bw > tx + tw) break;
    draw_text(label, cx, by, bw, S(22), white, 1);
    cx += bw + S(6);
  }
}

int Banner::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
      hot_ = e == FL_ENTER;
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      return 1;
    case FL_RELEASE:
      if (Fl::event_inside(this) && on_open) on_open(id_);
      return 1;
    case FL_KEYBOARD:
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == ' ') && on_open) {
        on_open(id_);
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// --------------------------------------------------------- CategoryTile

CategoryTile::CategoryTile(int x, int y, int w, int h, const std::string &title, const std::string &icon, int count,
                           bool compact)
    : Fl_Widget(x, y, w, h), title_(title), icon_(icon), count_(count), compact_(compact) {
  copy_tooltip(title.c_str());
}

void CategoryTile::draw() {
  const Theme &t = active_theme();
  int box = compact_ ? S(32) : S(40), bx = x() + S(12), by = y() + (h() - box) / 2;
  {
    Painter p(window()->w(), window()->h());
    paint_card(p, x(), y(), w(), h(), hot_, Fl::focus() == this);
    p.fill_rounded(bx, by, box, box, Sd(10), hue_color(title_).alpha(t.dark ? 0.28 : 0.16));
    int is = compact_ ? S(22) : S(26);
    p.image(IconLoader::get({icon_, "applications-other", "folder"}, is), bx + (box - is) / 2.0,
            by + (box - is) / 2.0, is);
  }
  int tx = bx + box + S(12), tw = w() - (tx - x()) - S(10);
  text_font(t.font_size, true);
  draw_text(title_, tx, y() + h() / 2 - S(20), tw, S(20), t.text);
  text_font(std::max(S(9), t.font_size - S(1)));
  draw_text(count_ >= 0 ? apps_count(size_t(count_)) : std::string(), tx, y() + h() / 2 + S(1), tw, S(18),
            t.text_dim);
}

int CategoryTile::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
      hot_ = e == FL_ENTER;
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      return 1;
    case FL_RELEASE:
      if (Fl::event_inside(this) && on_open) on_open();
      return 1;
    case FL_KEYBOARD:
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == ' ') && on_open) {
        on_open();
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// ----------------------------------------------------------- IconButton

bool reduced_motion() { return LogoAnimation::reduced_motion(); }

IconButton::IconButton(int x, int y, int w, int h, Symbol s, const std::string &tip)
    : Fl_Button(x, y, w, h), symbol_(s) {
  box(FL_NO_BOX);
  down_box(FL_NO_BOX);
  copy_tooltip(tip.c_str());
  callback([](Fl_Widget *w, void *) {
    auto *b = static_cast<IconButton *>(w);
    if (b->on_click) b->on_click();
  });
}

void IconButton::spinning(bool on) {
  if (on == spinning_) return;
  spinning_ = on;
  if (on && !reduced_motion())
    Fl::add_timeout(1.0 / 30, tick, this);
  else
    Fl::remove_timeout(tick, this);
  redraw();
}

void IconButton::tick(void *data) {
  auto *b = static_cast<IconButton *>(data);
  if (!b->spinning_) return;
  b->angle_ = std::fmod(b->angle_ + 0.22, 2 * M_PI);
  b->redraw();
  Fl::repeat_timeout(1.0 / 30, tick, data);
}

void IconButton::draw() {
  const Theme &t = active_theme();
  bool hot = Fl::belowmouse() == this && active_r(), down = value() != 0;
  Painter p(window()->w(), window()->h());
  double r = std::min(w(), h()) / 2.0;
  if (hot || down || spinning_)
    p.fill_rounded(x(), y(), w(), h(), r, down ? t.hover.mix(t.text, 0.1) : spinning_ && !hot ? t.accent.alpha(0.12)
                                                                                               : t.hover);
  if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  double s = std::min(w(), h()) * 0.55;
  Color c = !active_r() ? t.text_dim.alpha(0.45) : spinning_ ? t.accent_text : t.text;
  cairo_t *cr = p.cr();
  cairo_save(cr);
  if (spinning_) {
    double cx = x() + w() / 2.0, cy = y() + h() / 2.0;
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, angle_);
    cairo_translate(cr, -cx, -cy);
  }
  paint_symbol(p, symbol_, x() + (w() - s) / 2, y() + (h() - s) / 2, s, c);
  cairo_restore(cr);
}

int IconButton::handle(int e) {
  if (e == FL_ENTER || e == FL_LEAVE || e == FL_FOCUS || e == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  if (e == FL_KEYBOARD && Fl::focus() == this &&
      (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter || Fl::event_key() == ' ')) {
    do_callback();
    return 1;
  }
  return Fl_Button::handle(e);
}

// ---------------------------------------------------------- Placeholder

namespace {
std::vector<Placeholder *> g_placeholders;
double g_shimmer = 0;
}  // namespace

Placeholder::Placeholder(int x, int y, int w, int h, bool banner) : Fl_Widget(x, y, w, h), banner_(banner) {
  if (g_placeholders.empty() && !reduced_motion()) Fl::add_timeout(1.0 / 20, tick, nullptr);
  g_placeholders.push_back(this);
}

Placeholder::~Placeholder() {
  g_placeholders.erase(std::remove(g_placeholders.begin(), g_placeholders.end(), this), g_placeholders.end());
  if (g_placeholders.empty()) Fl::remove_timeout(tick, nullptr);
}

void Placeholder::tick(void *) {
  g_shimmer = std::fmod(g_shimmer + 0.04, 1.6);
  for (auto *p : g_placeholders) p->redraw();
  Fl::repeat_timeout(1.0 / 20, tick, nullptr);
}

void Placeholder::draw() {
  const Theme &t = active_theme();
  Painter p(window()->w(), window()->h());
  double r = banner_ ? double(t.radius_large) : std::min(double(t.radius), Sd(14));
  Color shape = t.text.alpha(t.dark ? 0.10 : 0.07);
  p.fill_rounded(x(), y(), w(), h(), r, banner_ ? t.surface_alt : t.surface);
  p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.border.alpha(0.7));
  int pad = banner_ ? S(32) : S(16), is = banner_ ? std::min(S(120), h() - 2 * pad) : S(56);
  int iy = banner_ ? y() + (h() - is) / 2 : y() + pad;
  p.fill_rounded(x() + pad, iy, is, is, is * 0.22, shape);
  int tx = x() + pad + is + S(18), tw = w() - (tx - x()) - pad;
  int ly = banner_ ? y() + h() / 2 - S(30) : y() + pad;
  int lh = banner_ ? S(22) : S(14);
  p.fill_rounded(tx, ly, tw * 0.55, lh, lh / 2.0, shape);
  p.fill_rounded(tx, ly + lh + S(12), tw * 0.85, S(11), S(5.5), shape);
  p.fill_rounded(tx, ly + lh + S(30), tw * 0.65, S(11), S(5.5), shape);
  if (!reduced_motion()) {
    // A band of light passing from left to right.
    double pos = x() - w() * 0.3 + g_shimmer / 1.2 * w() * 1.6;
    cairo_t *cr = p.cr();
    cairo_save(cr);
    p.rounded_rect(x(), y(), w(), h(), r);
    cairo_clip(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(pos, 0, pos + w() * 0.3, 0);
    Color light = t.dark ? Color::hex(0xFFFFFF) : Color::hex(0xFFFFFF);
    double a = t.dark ? 0.05 : 0.45;
    cairo_pattern_add_color_stop_rgba(g, 0, light.r, light.g, light.b, 0);
    cairo_pattern_add_color_stop_rgba(g, 0.5, light.r, light.g, light.b, a);
    cairo_pattern_add_color_stop_rgba(g, 1, light.r, light.g, light.b, 0);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
    cairo_restore(cr);
  }
}

// ----------------------------------------------------------- EmptyState

EmptyState::EmptyState(int x, int y, int w, Symbol s, const std::string &title, const std::string &text, bool problem)
    : Fl_Widget(x, y, w, measure(w, title, text)), symbol_(s), title_(title), text_(text), problem_(problem) {
  copy_tooltip(title.c_str());
}

int EmptyState::measure(int w, const std::string &title, const std::string &text) {
  int tw = std::min(w, S(560));
  text_font(active_theme().font_size + 5, true);
  int th = 0, ww = tw;
  fl_measure(title.c_str(), ww, th, 0);
  int bh = text.empty() ? 0 : TextBlock::measure(text, tw, 0) + S(10);
  return S(24) + S(96) + S(20) + th + S(10) + bh + S(8);
}

void EmptyState::draw() {
  const Theme &t = active_theme();
  Color c = problem_ ? t.warning : t.accent;
  int cs = S(96), cx = x() + (w() - cs) / 2, cy = y() + S(24);
  {
    Painter p(window()->w(), window()->h());
    p.circle(cx + cs / 2.0, cy + cs / 2.0, cs / 2.0, c.alpha(t.dark ? 0.22 : 0.13));
    p.circle(cx + cs / 2.0, cy + cs / 2.0, cs / 2.0 - S(10), c.alpha(t.dark ? 0.18 : 0.10));
    double ss = S(44);
    paint_symbol(p, symbol_, cx + (cs - ss) / 2, cy + (cs - ss) / 2, ss, readable(c, t.background));
  }
  int tw = std::min(w(), S(560)), tx = x() + (w() - tw) / 2, ty = cy + cs + S(20);
  text_font(t.font_size + 5, true);
  int th = 0, ww = tw;
  fl_measure(title_.c_str(), ww, th, 0);
  fl_color(t.text.fl());
  fl_draw(title_.c_str(), tx, ty, tw, th, Fl_Align(FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE), nullptr, 0);
  if (!text_.empty()) {
    text_font(t.font_size);
    fl_color(t.text_dim.fl());
    int bh = TextBlock::measure(text_, tw, 0);
    fl_draw(text_.c_str(), tx, ty + th + S(10), tw, bh, Fl_Align(FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE),
            nullptr, 0);
  }
}

// --------------------------------------------------------------- Notice

Notice::Notice(int x, int y, int w, int h, const std::string &text, int level, bool busy)
    : Fl_Widget(x, y, w, h), text_(text), level_(level), busy_(busy) {
  copy_tooltip(text.c_str());
}

int Notice::measure(int w, const std::string &text, int button_w) {
  int tw = w - S(56) - (button_w ? button_w + S(16) : S(16));
  return std::max(S(48), TextBlock::measure(text, tw, 0) + S(24));
}

void Notice::draw() {
  const Theme &t = active_theme();
  Color c = level_ == 2 ? t.error : level_ == 1 ? t.warning : t.accent;
  {
    Painter p(window()->w(), window()->h());
    double r = std::min(double(t.radius), Sd(12));
    p.fill_rounded(x(), y(), w(), h(), r, c.alpha(t.dark ? 0.18 : 0.10).mix(t.surface, 0.3));
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, c.alpha(0.45));
    double s = S(20), sx = x() + S(18), sy = y() + (h() - s) / 2;
    paint_symbol(p, busy_ ? Symbol::Refresh : level_ ? Symbol::Warning : Symbol::Info, sx, sy, s,
                 readable(c, t.surface));
  }
  text_font(t.font_size);
  fl_color(t.text.fl());
  int tx = x() + S(56), tw = w() - S(56) - (text_right ? text_right + S(16) : S(16));
  fl_draw(text_.c_str(), tx, y() + S(12), tw, h() - S(24),
          Fl_Align(FL_ALIGN_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE), nullptr, 0);
}

// ------------------------------------------------------ ScreenshotStrip

ScreenshotStrip::ScreenshotStrip(int x, int y, int w, int h, const std::vector<Screenshot> &shots)
    : Fl_Widget(x, y, w, h), shots_(shots) {
  tooltip(_("Screenshots"));
}

int ScreenshotStrip::preferred_height(int w) {
  // One large screenshot and the next ones beside it on wide pages.
  return std::min(S(300), std::max(S(140), int(w * 0.62 * 9 / 16)));
}

// The first screenshot large; the next ones (up to three) as a column of
// thumbnails beside it when there is room.
std::vector<ScreenshotStrip::Box> ScreenshotStrip::boxes() const {
  std::vector<Box> out;
  if (shots_.empty()) return out;
  int sh = h(), sw = std::min(w(), sh * 16 / 9), gap = S(12);
  out.push_back({x(), y(), sw, sh});
  int rest = std::min(3, int(shots_.size()) - 1);
  if (rest <= 0) return out;
  int room = w() - sw - gap;
  int th = (sh - gap * (rest - 1)) / rest, tw = th * 16 / 9;
  if (tw > room) {
    tw = room;
    th = tw * 9 / 16;
  }
  if (tw < S(80)) return out;
  for (int i = 0; i < rest; ++i) out.push_back({x() + sw + gap, y() + i * (th + gap), tw, th});
  return out;
}

int ScreenshotStrip::shot_at(int ex, int ey) const {
  std::vector<Box> b = boxes();
  for (size_t i = 0; i < b.size(); ++i)
    if (ex >= b[i].x && ex < b[i].x + b[i].w && ey >= b[i].y && ey < b[i].y + b[i].h) return int(i);
  return -1;
}

void ScreenshotStrip::draw() {
  const Theme &t = active_theme();
  std::vector<Box> b = boxes();
  std::vector<Box> failed;  // could not be downloaded (offline, or gone)
  {
    Painter p(window()->w(), window()->h());
    for (size_t i = 0; i < b.size(); ++i) {
      int sx = b[i].x, sy = b[i].y, sw = b[i].w, sh = b[i].h;
      double r = i == 0 ? S(10) : S(8);
      p.fill_rounded(sx, sy, sw, sh, r, t.surface_alt);
      // Thumbnails are about 600 px wide: a larger first screenshot (HiDPI)
      // uses the full image.
      bool full = i == 0 && !shots_[i].image.empty() && sw > 640;
      std::string ref = full ? shots_[i].image : shots_[i].thumbnail;
      cairo_surface_t *img = images::picture(ref, sw, sh);
      double s = S(28);
      if (img) {
        double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
        cairo_save(p.cr());
        p.rounded_rect(sx, sy, sw, sh, r);
        cairo_clip(p.cr());
        p.surface(img, sx + (sw - iw) / 2, sy + (sh - ih) / 2);
        cairo_restore(p.cr());
      } else if (images::failed(ref)) {
        paint_symbol(p, Symbol::Warning, sx + (sw - s) / 2, sy + sh / 2.0 - s - S(4), s, t.text_dim.alpha(0.6));
        failed.push_back(b[i]);
      } else {
        // Loading: a picture symbol.
        paint_symbol(p, Symbol::Folder, sx + (sw - s) / 2, sy + (sh - s) / 2, s, t.text_dim.alpha(0.5));
      }
      p.stroke_rounded(sx + 0.5, sy + 0.5, sw - 1, sh - 1, r, int(i) == hover_ ? t.accent : t.border);
    }
  }
  text_font(t.font_size - S(1));
  for (auto &f : failed)
    draw_text(_("Screenshot not available"), f.x + S(8), f.y + f.h / 2 + S(2), f.w - S(16), S(22), t.text_dim, 1);
}

int ScreenshotStrip::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int h2 = shot_at(Fl::event_x(), Fl::event_y());
      if (h2 != hover_) {
        hover_ = h2;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH:
      return Fl::event_button() == FL_LEFT_MOUSE ? 1 : 0;
    case FL_RELEASE: {
      int i = shot_at(Fl::event_x(), Fl::event_y());
      if (i >= 0 && on_open) on_open(i);
      return 1;
    }
    default:
      return Fl_Widget::handle(e);
  }
}

// ---------------------------------------------------------- SearchField

namespace {

// The rounded field behind the search input, with its magnifier; a click
// on it (outside the text) types into the input.
class SearchFrame : public Fl_Widget {
 public:
  SearchFrame(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  Fl_Widget *input = nullptr;
  void draw() override {
    const Theme &t = active_theme();
    bool focused = input && Fl::focus() == input;
    Painter p(window()->w(), window()->h());
    double r = h() / 2.0;
    p.fill_rounded(x(), y(), w(), h(), r, t.surface);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, focused ? t.accent : t.border,
                     focused ? Sd(2) : Sd(1));
    paint_symbol(p, Symbol::Search, x() + S(13), y() + (h() - S(16)) / 2.0, S(16), t.text_dim);
  }
  int handle(int e) override {
    if (e == FL_PUSH && input) {
      input->take_focus();
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

}  // namespace

SearchField::SearchField(int x, int y, int w, int h, const std::string &placeholder)
    : Fl_Input(x + S(36), y + S(4), w - S(50), h - S(8)), placeholder_(placeholder) {
  // The frame is created first so that it is drawn below the input.
  restyle();
  when(FL_WHEN_CHANGED);
  tooltip(_("Search"));
}

SearchField *make_search_field(int x, int y, int w, int h, const std::string &placeholder) {
  auto *frame = new SearchFrame(x, y, w, h);
  auto *input = new SearchField(x, y, w, h, placeholder);
  frame->input = input;
  input->frame = frame;
  return input;
}

void SearchField::restyle() {
  const Theme &t = active_theme();
  box(FL_FLAT_BOX);
  color(t.surface.fl());
  textcolor(t.text.fl());
  cursor_color(t.accent.fl());
  selection_color(t.accent.fl());
  textsize(t.font_size + 1);
}

void SearchField::draw() {
  Fl_Input::draw();
  if (size() == 0 && Fl::focus() != this) {
    const Theme &t = active_theme();
    text_font(t.font_size + 1);
    draw_text(placeholder_, x() + S(3), y(), w() - S(6), h(), t.text_dim);
  }
}

int SearchField::handle(int e) {
  if ((e == FL_FOCUS || e == FL_UNFOCUS) && frame) frame->redraw();
  if (e == FL_KEYBOARD) {
    int k = Fl::event_key();
    if ((k == FL_Enter || k == FL_KP_Enter) && on_enter) {
      on_enter();
      return 1;
    }
    if (k == FL_Escape) {
      if (size() > 0) {
        value("");
        do_callback();
      } else if (on_escape) {
        on_escape();
      }
      return 1;
    }
  }
  return Fl_Input::handle(e);
}

// ------------------------------------------------------------ TextBlock

TextBlock::TextBlock(int x, int y, int w, int h, const std::string &text, bool dim, int size_delta)
    : Fl_Widget(x, y, w, h), text_(text), dim_(dim), delta_(size_delta) {}

int TextBlock::measure(const std::string &text, int w, int size_delta) {
  text_font(active_theme().font_size + size_delta);
  int ww = w, hh = 0;
  fl_measure(text.c_str(), ww, hh, 0);
  return hh;
}

void TextBlock::draw() {
  const Theme &t = active_theme();
  text_font(t.font_size + delta_);
  fl_color((dim_ ? t.text_dim : t.text).fl());
  fl_draw(text_.c_str(), x(), y(), w(), h(), Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE),
          nullptr, 0);
}

// ------------------------------------------------------ screenshot view

namespace {

class ShotWindow : public BufferedWindow {
 public:
  ShotWindow(int w, int h, const std::vector<Screenshot> &shots, int index, const std::string &title)
      : BufferedWindow(w, h), shots_(shots), index_(index), title_(title) {
    update_label();
  }
  void update_label() {
    label_ = shots_.size() > 1 ? format(_("%s \xE2\x80\x94 screenshot %d of %d"), title_.c_str(), index_ + 1,
                                        int(shots_.size()))
                               : format(_("%s \xE2\x80\x94 screenshot"), title_.c_str());
    label(label_.c_str());
  }
  void draw() override {
    const Theme &t = active_theme();
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.background);
    const Screenshot &s = shots_[size_t(index_)];
    cairo_surface_t *img = images::picture(s.image, w() - S(24), h() - S(24));
    if (!img) img = images::picture(s.thumbnail, w() - S(24), h() - S(24));
    if (img) {
      double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
      p.surface(img, (w() - iw) / 2, (h() - ih) / 2);
    } else {
      double sz = S(48);
      paint_symbol(p, Symbol::Folder, (w() - sz) / 2, (h() - sz) / 2, sz, t.text_dim);
    }
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key(), n = int(shots_.size());
      if (k == FL_Escape) {
        hide();
        return 1;
      }
      if ((k == FL_Right || k == ' ' || k == FL_Page_Down) && n > 1) index_ = (index_ + 1) % n;
      else if ((k == FL_Left || k == FL_BackSpace || k == FL_Page_Up) && n > 1) index_ = (index_ + n - 1) % n;
      else return BufferedWindow::handle(e);
      update_label();
      redraw();
      return 1;
    }
    if (e == FL_PUSH) {
      int n = int(shots_.size());
      if (n > 1) {
        index_ = (index_ + (Fl::event_x() < w() / 3 ? n - 1 : 1)) % n;
        update_label();
        redraw();
      }
      return 1;
    }
    return BufferedWindow::handle(e);
  }

 private:
  std::vector<Screenshot> shots_;
  int index_;
  std::string title_, label_;
};

}  // namespace

void show_screenshot(const std::vector<Screenshot> &shots, int index, const std::string &title) {
  if (shots.empty()) return;
  int x = 0, y = 0, w = S(1100), h = int(S(1100) * 9 / 16.0) + S(24);
  fit_to_work_area(x, y, w, h);
  auto *win = new ShotWindow(w, h, shots, std::max(0, std::min(index, int(shots.size()) - 1)), title);
  win->xclass("merkadu");
  win->set_non_modal();
  win->position(x, y);
  win->callback([](Fl_Widget *w, void *) {
    w->hide();
    Fl::delete_widget(w);
  });
  win->show();
}

}  // namespace merkadu
}  // namespace sde
