// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "buffered_window.h"
#include "config.h"
#include "glyphs.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

namespace sde {
namespace update {

// ------------------------------------------------------------------ look

static Look g_look;
const Look &look() { return g_look; }
void set_look(const Look &l) { g_look = l; }

Look Look::from_config(const Config &cfg) {
  Look l;
  std::string style = lower(cfg.get("appearance", "style", ""));
  if (style == "sde" || style == "aero" || style == "liquid-glass")
    l.style = style;
  // Alpha 16 families: Graphite (flat), Fluent (Mica-like), Glass (frosted).
  else if (style == "graphite")
    l.style = "sde";
  else if (style == "fluent")
    l.style = "aero";
  else if (style == "glass")
    l.style = "liquid-glass";
  else
    l.style = cfg.get_bool("aero", "enabled", true) ? "aero" : "sde";
  return l;
}

double Look::card_radius() const {
  // The theme radius (already scaled); Liquid Glass rounds a little more.
  const Theme &t = active_theme();
  return liquid() ? std::max<double>(t.radius_large, t.radius) : double(t.radius);
}

double Look::control_radius(double h) const {
  return liquid() ? h / 2 : std::min<double>(active_theme().radius_small, h / 2);
}

// ---------------------------------------------------------------- colors

Color readable_on(const Color &c, const Color &bg) {
  const Theme &t = active_theme();
  Color out = c;
  for (int i = 1; i <= 20 && out.contrast(bg) < 4.5; ++i)
    out = c.mix(t.text, i * 0.05);
  return out;
}

Color success_color() { return active_theme().success; }
Color warning_color() { return active_theme().warning; }
Color danger_color() { return active_theme().error; }

Color kind_color(Kind kind) {
  switch (kind) {
  case Kind::Security:
    return danger_color();
  case Kind::Kernel:
    return active_theme().accent.mix(active_theme().error, 0.35);
  case Kind::Backport:
    return warning_color();
  default:
    return active_theme().accent;
  }
}

Color kind_text(Kind kind) {
  return readable_on(kind_color(kind), active_theme().surface);
}

// ----------------------------------------------------------- backgrounds

static void paint_blobs(cairo_t *cr, int w, int h) {
  const Theme &t = active_theme();
  struct Blob {
    double fx, fy, fr;
    Color c;
  } blobs[] = {
      {0.18, 0.08, 0.55, t.accent.alpha(t.dark ? 0.20 : 0.16)},
      {0.92, 0.85, 0.60, t.accent.mix(t.error, 0.45).alpha(t.dark ? 0.16 : 0.12)},
      {0.62, 0.30, 0.35, t.accent.mix(t.success, 0.5).alpha(0.10)},
  };
  for (auto &b : blobs) {
    double r = std::max(w, h) * b.fr;
    cairo_pattern_t *g = cairo_pattern_create_radial(w * b.fx, h * b.fy, 0,
                                                     w * b.fx, h * b.fy, r);
    cairo_pattern_add_color_stop_rgba(g, 0, b.c.r, b.c.g, b.c.b, b.c.a);
    cairo_pattern_add_color_stop_rgba(g, 1, b.c.r, b.c.g, b.c.b, 0);
    cairo_set_source(cr, g);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
  }
}

void paint_window_background(Painter &p, int w, int h) {
  const Theme &t = active_theme();
  p.fill_rect(0, 0, w, h, t.background);
  if (look().liquid()) {
    // Soft colored light behind frosted cards, rendered once per size: the
    // radial gradients are too slow to repeat for every animation frame.
    static cairo_surface_t *cache = nullptr;
    static int cache_w = 0, cache_h = 0;
    if (!cache || cache_w != w || cache_h != h) {
      if (cache)
        cairo_surface_destroy(cache);
      cache = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
      cache_w = w;
      cache_h = h;
      cairo_t *cr = cairo_create(cache);
      paint_blobs(cr, w, h);
      cairo_destroy(cr);
    }
    cairo_set_source_surface(p.cr(), cache, 0, 0);
    cairo_paint(p.cr());
  } else if (look().aero()) {
    p.fill_gradient(0, 0, w, Sd(220), 0, t.accent.alpha(t.dark ? 0.10 : 0.07),
                    t.background.alpha(0));
  }
}

void paint_card(Painter &p, double x, double y, double w, double h, bool hero) {
  const Theme &t = active_theme();
  double r = look().card_radius();
  if (look().liquid()) {
    p.shadow(x, y + Sd(2), w, h, r, t.shadow.alpha(t.dark ? 0.30 : 0.10), S(6));
    p.fill_rounded(x, y, w, h, r, t.surface.alpha(t.dark ? 0.72 : 0.68));
    if (hero)
      p.fill_gradient(x, y, w, h, r, t.accent.alpha(t.dark ? 0.22 : 0.14),
                      t.surface.alpha(0), true);
    p.fill_gradient(x, y, w, h * 0.5, r,
                    Color::hex(0xFFFFFF, t.dark ? 0.07 : 0.45),
                    Color::hex(0xFFFFFF, 0));
    p.stroke_rounded(x + Sd(1), y + Sd(1), w - Sd(2), h - Sd(2), r - Sd(1),
                     Color::hex(0xFFFFFF, t.dark ? 0.10 : 0.65));
    p.stroke_rounded(x, y, w, h, r, t.border.alpha(t.dark ? 0.8 : 0.55));
    return;
  }
  p.fill_rounded(x, y, w, h, r, t.surface);
  if (hero) {
    p.fill_gradient(x, y, w, h, r, t.accent.alpha(t.dark ? 0.20 : 0.12),
                    t.surface.alpha(0), true);
    if (look().aero())
      p.glass_sheen(x, y, w, h, r, t.dark);
  }
  p.stroke_rounded(x, y, w, h, r, t.border);
}

void paint_progress(Painter &p, double x, double y, double w, double h,
                    int percent, double phase) {
  const Theme &t = active_theme();
  p.fill_rounded(x, y, w, h, h / 2, t.border.alpha(0.7));
  if (percent >= 0) {
    double fw = std::max(h, w * std::min(100, percent) / 100.0);
    p.fill_rounded(x, y, fw, h, h / 2, t.accent);
    if (look().liquid() || look().aero())
      p.fill_gradient(x, y, fw, h / 2, h / 4, Color::hex(0xFFFFFF, 0.35),
                      Color::hex(0xFFFFFF, 0));
  } else {
    // Indeterminate: a segment sliding back and forth.
    double seg = w * 0.28;
    double f = 0.5 - 0.5 * std::cos(phase * 2 * M_PI);
    p.fill_rounded(x + (w - seg) * f, y, seg, h, h / 2, t.accent);
  }
}

void paint_checkbox(Painter &p, double x, double y, double size, int state,
                    bool hover) {
  const Theme &t = active_theme();
  double r = look().liquid() ? size / 2.8 : Sd(4);
  if (state) {
    p.fill_rounded(x, y, size, size, r,
                   hover ? t.accent.mix(t.text, 0.12) : t.accent);
    if (state == 1) {
      glyph::check(p, x + size * 0.12, y + size * 0.12, size * 0.76,
                   t.on_accent);
    } else {
      p.fill_rounded(x + size * 0.25, y + size * 0.45, size * 0.5, size * 0.12,
                     size * 0.06, t.on_accent);
    }
  } else {
    p.fill_rounded(x, y, size, size, r, hover ? t.hover : t.surface);
    p.stroke_rounded(x, y, size, size, r,
                     hover ? t.accent : t.text_dim.alpha(0.6), Sd(1.5));
  }
}

void paint_spinner(Painter &p, double cx, double cy, double r, double phase,
                   const Color &c) {
  cairo_t *cr = p.cr();
  cairo_set_line_width(cr, std::max(2.0, r / 4));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  p.set(c.alpha(0.18));
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  cairo_stroke(cr);
  p.set(c);
  double a = phase * 2 * M_PI;
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx, cy, r, a, a + M_PI * 0.6);
  cairo_stroke(cr);
}

// ---------------------------------------------------------------- badges

int badge_width(const std::string &text) {
  text_font(std::max(S(9), active_theme().font_size - S(2)), true);
  return text_width(text) + S(16);
}

void paint_badge(Painter &p, double x, double y, double w, double h,
                 const Color &c) {
  p.fill_rounded(x, y, w, h, h / 2, c.alpha(active_theme().dark ? 0.22 : 0.13));
}

void draw_badge_text(const std::string &text, int x, int y, int w, int h,
                     const Color &c) {
  text_font(std::max(S(9), active_theme().font_size - S(2)), true);
  draw_text(text, x, y, w, h, readable_on(c, active_theme().surface), 1);
}

// ----------------------------------------------------------------- icons

void paint_icon(Painter &p, Icon icon, double x, double y, double s,
                const Color &c) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, s / 24.0, s / 24.0);
  cairo_set_line_width(cr, 1.9);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  p.set(c);
  auto arrow_circle = [&]() {
    cairo_new_sub_path(cr);
    cairo_arc(cr, 12, 12, 8, -M_PI * 0.30, M_PI * 1.45);
    cairo_stroke(cr);
    // Arrow head at the start of the arc.
    double a = -M_PI * 0.30, hx = 12 + 8 * std::cos(a),
           hy = 12 + 8 * std::sin(a);
    cairo_move_to(cr, hx - 4.2, hy - 1.2);
    cairo_line_to(cr, hx + 0.4, hy - 0.2);
    cairo_line_to(cr, hx + 0.9, hy - 4.8);
    cairo_stroke(cr);
  };
  switch (icon) {
  case Icon::Updates:
    arrow_circle();
    cairo_move_to(cr, 12, 8.5);
    cairo_line_to(cr, 12, 15);
    cairo_move_to(cr, 9.2, 12.4);
    cairo_line_to(cr, 12, 15.2);
    cairo_line_to(cr, 14.8, 12.4);
    cairo_stroke(cr);
    break;
  case Icon::Restart:
    arrow_circle();
    break;
  case Icon::History:
  case Icon::Clock:
    cairo_new_sub_path(cr);
    cairo_arc(cr, 12, 12, 8.5, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 12, 7.5);
    cairo_line_to(cr, 12, 12);
    cairo_line_to(cr, 15.2, 14);
    cairo_stroke(cr);
    break;
  case Icon::Sources:
    for (int i = 0; i < 3; ++i) {
      double yy = 4 + i * 6;
      p.rounded_rect(4, yy, 16, 4.6, 1.8);
      cairo_stroke(cr);
      cairo_arc(cr, 7, yy + 2.3, 0.9, 0, 2 * M_PI);
      cairo_fill(cr);
    }
    break;
  case Icon::Maintenance:
  case Icon::Wrench:
    cairo_move_to(cr, 5, 19);
    cairo_line_to(cr, 12.5, 11.5);
    cairo_stroke(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 15.5, 8.5, 4.3, M_PI * 0.95, M_PI * 2.6);
    cairo_stroke(cr);
    break;
  case Icon::Settings:
    cairo_restore(cr);
    glyph::gear(p, x, y, s, c);
    return;
  case Icon::About:
  case Icon::Info:
    cairo_new_sub_path(cr);
    cairo_arc(cr, 12, 12, 8.5, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 12, 11);
    cairo_line_to(cr, 12, 16.2);
    cairo_stroke(cr);
    cairo_arc(cr, 12, 7.9, 1.15, 0, 2 * M_PI);
    cairo_fill(cr);
    break;
  case Icon::Shield:
    cairo_move_to(cr, 12, 3);
    cairo_curve_to(cr, 15, 4.8, 17.5, 5.4, 20, 5.4);
    cairo_curve_to(cr, 20, 13, 17.5, 18.2, 12, 21);
    cairo_curve_to(cr, 6.5, 18.2, 4, 13, 4, 5.4);
    cairo_curve_to(cr, 6.5, 5.4, 9, 4.8, 12, 3);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, 8.6, 12);
    cairo_line_to(cr, 11, 14.4);
    cairo_line_to(cr, 15.6, 9.6);
    cairo_stroke(cr);
    break;
  case Icon::Download:
    cairo_move_to(cr, 12, 4);
    cairo_line_to(cr, 12, 15);
    cairo_move_to(cr, 7.5, 10.8);
    cairo_line_to(cr, 12, 15.3);
    cairo_line_to(cr, 16.5, 10.8);
    cairo_move_to(cr, 4.5, 15.5);
    cairo_line_to(cr, 4.5, 19.5);
    cairo_line_to(cr, 19.5, 19.5);
    cairo_line_to(cr, 19.5, 15.5);
    cairo_stroke(cr);
    break;
  case Icon::Warning:
    cairo_move_to(cr, 12, 3.8);
    cairo_line_to(cr, 21, 19.5);
    cairo_line_to(cr, 3, 19.5);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, 12, 9.5);
    cairo_line_to(cr, 12, 13.8);
    cairo_stroke(cr);
    cairo_arc(cr, 12, 16.6, 1.1, 0, 2 * M_PI);
    cairo_fill(cr);
    break;
  case Icon::Search:
    cairo_new_sub_path(cr);
    cairo_arc(cr, 10.5, 10.5, 6, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 15, 15);
    cairo_line_to(cr, 20, 20);
    cairo_stroke(cr);
    break;
  case Icon::Trash:
    cairo_move_to(cr, 4.5, 7);
    cairo_line_to(cr, 19.5, 7);
    cairo_move_to(cr, 9.5, 7);
    cairo_line_to(cr, 10, 4.5);
    cairo_line_to(cr, 14, 4.5);
    cairo_line_to(cr, 14.5, 7);
    cairo_move_to(cr, 6.5, 7);
    cairo_line_to(cr, 7.5, 19.5);
    cairo_line_to(cr, 16.5, 19.5);
    cairo_line_to(cr, 17.5, 7);
    cairo_stroke(cr);
    break;
  case Icon::Broom:
    cairo_move_to(cr, 19, 4);
    cairo_line_to(cr, 12.5, 11);
    cairo_stroke(cr);
    cairo_move_to(cr, 10.5, 9.5);
    cairo_line_to(cr, 14.2, 13.2);
    cairo_line_to(cr, 10.5, 20);
    cairo_line_to(cr, 4, 13.5);
    cairo_close_path(cr);
    cairo_stroke(cr);
    break;
  case Icon::Terminal:
    p.rounded_rect(3.5, 5, 17, 14, 2.5);
    cairo_stroke(cr);
    cairo_move_to(cr, 7, 9.5);
    cairo_line_to(cr, 10, 12);
    cairo_line_to(cr, 7, 14.5);
    cairo_move_to(cr, 12, 15);
    cairo_line_to(cr, 16.5, 15);
    cairo_stroke(cr);
    break;
  case Icon::Package:
    cairo_move_to(cr, 12, 3.5);
    cairo_line_to(cr, 19.5, 7.5);
    cairo_line_to(cr, 19.5, 16.5);
    cairo_line_to(cr, 12, 20.5);
    cairo_line_to(cr, 4.5, 16.5);
    cairo_line_to(cr, 4.5, 7.5);
    cairo_close_path(cr);
    cairo_move_to(cr, 4.5, 7.5);
    cairo_line_to(cr, 12, 11.5);
    cairo_line_to(cr, 19.5, 7.5);
    cairo_move_to(cr, 12, 11.5);
    cairo_line_to(cr, 12, 20.5);
    cairo_stroke(cr);
    break;
  case Icon::Close:
    cairo_move_to(cr, 7, 7);
    cairo_line_to(cr, 17, 17);
    cairo_move_to(cr, 17, 7);
    cairo_line_to(cr, 7, 17);
    cairo_stroke(cr);
    break;
  case Icon::Check:
    cairo_move_to(cr, 5.5, 12.5);
    cairo_line_to(cr, 10, 17);
    cairo_line_to(cr, 18.5, 7.5);
    cairo_stroke(cr);
    break;
  }
  cairo_restore(cr);
}

void paint_disclosure(Painter &p, double x, double y, double s, bool open, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  p.set(c);
  if (open) {
    cairo_move_to(cr, x + s * 0.15, y + s * 0.32);
    cairo_line_to(cr, x + s * 0.85, y + s * 0.32);
    cairo_line_to(cr, x + s * 0.5, y + s * 0.74);
  } else {
    cairo_move_to(cr, x + s * 0.32, y + s * 0.15);
    cairo_line_to(cr, x + s * 0.74, y + s * 0.5);
    cairo_line_to(cr, x + s * 0.32, y + s * 0.85);
  }
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_restore(cr);
}

// ---------------------------------------------------------------- button

Button::Button(int x, int y, int w, int h, const std::string &text, Kind kind)
    : Fl_Button(x, y, w, h), text_(text), kind_(kind) {
  box(FL_NO_BOX);
  down_box(FL_NO_BOX);
}

void Button::text(const std::string &t) {
  text_ = t;
  redraw();
}

int Button::preferred_width() const {
  text_font(active_theme().font_size, kind_ == Primary || kind_ == Danger);
  return text_width(text_) + S(32) + (has_icon_ ? S(24) : 0);
}

void Button::draw() {
  const Theme &t = active_theme();
  bool hot = Fl::belowmouse() == this && active_r(), down = value() != 0;
  bool strong = kind_ == Primary || kind_ == Danger;
  Color fg = t.text;
  double r = look().control_radius(h());
  {
    Painter p(window()->w(), window()->h());
    Color bg = t.surface_alt;
    if (kind_ == Primary)
      bg = t.accent;
    if (kind_ == Danger)
      bg = danger_color();
    if (kind_ == Ghost)
      bg = t.surface_alt.alpha(0);
    if (hot)
      bg = strong ? bg.mix(t.text, 0.12) : t.hover;
    if (down)
      bg = bg.mix(t.text, 0.15);
    if (!active_r())
      bg = strong ? bg.alpha(0.40) : t.surface_alt.alpha(0.6);
    if (look().liquid() && !strong && kind_ != Ghost)
      bg = bg.alpha(hot ? 0.95 : 0.72);
    p.fill_rounded(x(), y(), w(), h(), r, bg);
    if (strong && (look().liquid() || look().aero()) && active_r())
      p.fill_gradient(x(), y(), w(), h() / 2.0, r, Color::hex(0xFFFFFF, 0.22),
                      Color::hex(0xFFFFFF, 0));
    if (!strong && kind_ != Ghost)
      p.stroke_rounded(x(), y(), w(), h(), r,
                       look().liquid()
                           ? Color::hex(0xFFFFFF, t.dark ? 0.12 : 0.7)
                           : t.border);
    if (Fl::focus() == this)
      focus_ring(p, x(), y(), w(), h(), r);
    if (kind_ == Primary)
      fg = t.on_accent;
    if (kind_ == Danger)
      fg = readable_on(t.on_accent, danger_color());
    if (kind_ == Ghost)
      fg = t.accent_text;
    if (!active_r())
      fg = strong ? fg.alpha(0.75) : t.text_dim.alpha(0.7);
    text_font(t.font_size, strong);
    int tw = text_width(text_) + (has_icon_ ? S(24) : 0);
    int tx = x() + std::max(S(6), (w() - tw) / 2);
    if (has_icon_)
      paint_icon(p, icon_, tx, y() + (h() - S(18)) / 2.0, S(18), fg);
  }
  text_font(t.font_size, strong);
  int tw = text_width(text_) + (has_icon_ ? S(24) : 0);
  int tx = x() + std::max(S(6), (w() - tw) / 2) + (has_icon_ ? S(24) : 0);
  draw_text(text_, tx, y(), x() + w() - tx - S(4), h(), fg);
}

int Button::handle(int event) {
  if (event == FL_ENTER || event == FL_LEAVE || event == FL_FOCUS ||
      event == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  if (event == FL_KEYBOARD && Fl::focus() == this &&
      (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter ||
       Fl::event_key() == ' ')) {
    do_callback();
    return 1;
  }
  return Fl_Button::handle(event);
}

// ----------------------------------------------------------- search field

SearchField::SearchField(int x, int y, int w, int h,
                         const std::string &placeholder)
    : Fl_Input(x, y, w, h), placeholder_(placeholder) {
  box(FL_NO_BOX);
  when(FL_WHEN_CHANGED);
  restyle();
}

void SearchField::restyle() {
  const Theme &t = active_theme();
  textcolor(t.text.fl());
  textsize(t.font_size);
  textfont(FL_HELVETICA);
  cursor_color(t.accent.fl());
  selection_color(t.accent.alpha(0.35).mix(t.surface, 0.5).fl());
}

void SearchField::draw() {
  const Theme &t = active_theme();
  double r = look().control_radius(h());
  bool focused = Fl::focus() == this;
  {
    Painter p(window()->w(), window()->h());
    Color bg = look().liquid() ? t.surface.alpha(0.75) : t.surface;
    p.fill_rounded(x(), y(), w(), h(), r, bg);
    p.stroke_rounded(x(), y(), w(), h(), r, focused ? t.accent : t.border,
                     focused ? Sd(1.5) : 1);
    paint_icon(p, Icon::Search, x() + S(10), y() + (h() - S(16)) / 2.0, S(16),
               t.text_dim);
  }
  int tx = x() + S(32), tw = w() - S(40);
  if (!size() && !focused) {
    text_font(t.font_size);
    draw_text(placeholder_, tx, y(), tw, h(), t.text_dim);
    return;
  }
  fl_push_clip(tx, y() + S(2), tw, h() - S(4));
  // Fl_Input_::drawtext() needs the background drawn already.
  drawtext(tx, y() + (h() - textsize() - S(6)) / 2, tw, textsize() + S(6));
  fl_pop_clip();
}

int SearchField::handle(int event) {
  if (event == FL_FOCUS || event == FL_UNFOCUS) {
    int r = Fl_Input::handle(event);
    redraw();
    return r ? r : 1;
  }
  if (event == FL_KEYBOARD && Fl::event_key() == FL_Escape && size()) {
    value("");
    set_changed();
    do_callback();
    return 1;
  }
  return Fl_Input::handle(event);
}

// -------------------------------------------------------------- list view

ListView::ListView(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}

int ListView::content_height() const {
  int n = count ? count() : 0, total = 0;
  for (int i = 0; i < n; ++i)
    total += row_height(i);
  return total;
}

int ListView::row_top(int row) const {
  int top = 0;
  for (int i = 0; i < row; ++i)
    top += row_height(i);
  return top;
}

int ListView::row_at(int ey) const {
  int n = count ? count() : 0;
  int pos = ey - y() + scroll_ - (framed ? S(4) : 0);
  if (pos < 0)
    return -1;
  int top = 0;
  for (int i = 0; i < n; ++i) {
    int h = row_height(i);
    if (pos < top + h)
      return i;
    top += h;
  }
  return -1;
}

void ListView::clamp_scroll() {
  int inner = h() - (framed ? S(8) : 0);
  int max_scroll = std::max(0, content_height() - inner);
  scroll_ = std::max(0, std::min(scroll_, max_scroll));
}

void ListView::select(int row, bool notify) {
  int n = count ? count() : 0;
  if (row >= n)
    row = n - 1;
  if (row >= 0 && selectable && !selectable(row))
    return;
  if (row == selected_)
    return;
  selected_ = row;
  if (row >= 0)
    ensure_visible(row);
  redraw();
  if (notify && selection_changed)
    selection_changed(row);
}

void ListView::ensure_visible(int row) {
  int inner = h() - (framed ? S(8) : 0);
  int top = row_top(row), bottom = top + row_height(row);
  if (top < scroll_)
    scroll_ = top;
  else if (bottom > scroll_ + inner)
    scroll_ = bottom - inner;
  clamp_scroll();
}

void ListView::move_selection(int delta) {
  int n = count ? count() : 0;
  if (!n)
    return;
  int row = selected_ < 0 ? (delta > 0 ? -1 : n) : selected_;
  for (int step = 0; step < n; ++step) {
    row += delta > 0 ? 1 : -1;
    if (row < 0 || row >= n)
      return;
    if (!selectable || selectable(row))
      break;
  }
  if (row >= 0 && row < n)
    select(row);
}

void ListView::draw() {
  const Theme &t = active_theme();
  clamp_scroll();
  int n = count ? count() : 0;
  int pad = framed ? S(4) : 0;
  int bar_w = S(6);
  int inner = h() - 2 * pad;
  int total = content_height();
  bool bar = total > inner;
  int row_w = w() - 2 * pad - (bar ? bar_w + S(6) : 0);
  fl_push_clip(x(), y(), w(), h());
  {
    Painter p(window()->w(), window()->h());
    if (framed)
      paint_card(p, x(), y(), w(), h());
    cairo_save(p.cr());
    cairo_rectangle(p.cr(), x() + pad, y() + pad, w() - 2 * pad, inner);
    cairo_clip(p.cr());
    int top = y() + pad - scroll_;
    for (int i = 0; i < n; ++i) {
      int rh = row_height(i);
      if (top + rh >= y() && top <= y() + h())
        paint_row(p, i, x() + pad, top, row_w, rh, i == selected_, i == hover_);
      top += rh;
    }
    cairo_restore(p.cr());
    if (bar) {
      double track = inner - S(8);
      double th = std::max(double(S(28)), track * inner / total);
      double ty =
          y() + pad + S(4) + (track - th) * scroll_ / double(total - inner);
      p.fill_rounded(x() + w() - pad - bar_w - S(2), ty, bar_w, th, bar_w / 2.0,
                     t.text_dim.alpha(dragging_bar_ ? 0.6 : 0.35));
    }
    if (!n) {
      double cx = x() + w() / 2.0, cy = y() + h() / 2.0 - S(30);
      if (loading)
        paint_spinner(p, cx, cy, S(16), phase, t.accent);
      else if (!empty_title.empty())
        paint_icon(p, empty_icon, cx - S(22), cy - S(22), S(44),
                   t.text_dim.alpha(0.8));
    }
    if (Fl::focus() == this && framed) {
      double r = look().card_radius();
      p.stroke_rounded(x() + 1, y() + 1, w() - 2, h() - 2, r,
                       t.accent.alpha(0.55), Sd(1.5));
    }
  }
  if (!n) {
    int cy = y() + h() / 2 - S(30);
    text_font(t.font_size + S(2), true);
    draw_text(empty_title, x() + S(16), cy + S(28), w() - S(32), S(26), t.text,
              1);
    text_font(t.font_size);
    fl_color(t.text_dim.fl());
    fl_draw(empty_text.c_str(), x() + S(32), cy + S(56), w() - S(64), S(60),
            Fl_Align(FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE));
  } else {
    fl_push_clip(x() + pad, y() + pad, w() - 2 * pad, inner);
    int top = y() + pad - scroll_;
    for (int i = 0; i < n; ++i) {
      int rh = row_height(i);
      if (top + rh >= y() && top <= y() + h())
        text_row(i, x() + pad, top, row_w, rh, i == selected_, i == hover_);
      top += rh;
    }
    fl_pop_clip();
  }
  fl_pop_clip();
}

int ListView::handle(int event) {
  int n = count ? count() : 0;
  int pad = framed ? S(4) : 0;
  switch (event) {
  case FL_FOCUS:
  case FL_UNFOCUS:
    redraw();
    return 1;
  case FL_ENTER:
  case FL_MOVE: {
    int r = row_at(Fl::event_y());
    if (r != hover_) {
      hover_ = r;
      redraw();
    }
    return 1;
  }
  case FL_LEAVE:
    hover_ = -1;
    redraw();
    return 1;
  case FL_MOUSEWHEEL:
    if (!Fl::event_inside(this))
      return 0;
    scroll_ += Fl::event_dy() * S(56);
    clamp_scroll();
    hover_ = row_at(Fl::event_y());
    redraw();
    return 1;
  case FL_PUSH: {
    if (Fl::visible_focus())
      take_focus();
    int inner = h() - 2 * pad, total = content_height();
    if (total > inner && Fl::event_x() >= x() + w() - pad - S(14)) {
      dragging_bar_ = true;
      drag_offset_ = Fl::event_y();
      double track = inner - S(8);
      double th = std::max(double(S(28)), track * inner / total);
      double ty =
          y() + pad + S(4) + (track - th) * scroll_ / double(total - inner);
      if (Fl::event_y() < ty || Fl::event_y() > ty + th) {
        // Page towards the click.
        scroll_ += (Fl::event_y() < ty ? -1 : 1) * inner;
        clamp_scroll();
      }
      drag_offset_ =
          scroll_ - int((Fl::event_y() - y()) * double(total) / inner);
      redraw();
      return 1;
    }
    int r = row_at(Fl::event_y());
    if (r < 0)
      return 1;
    if (!selectable || selectable(r))
      select(r);
    if (clicked)
      clicked(r, Fl::event_x() - x() - pad, Fl::event_clicks() ? 2 : 1);
    redraw();
    return 1;
  }
  case FL_DRAG:
    if (dragging_bar_) {
      int inner = h() - 2 * pad, total = content_height();
      scroll_ = drag_offset_ +
                int((Fl::event_y() - y()) * double(total) / std::max(1, inner));
      clamp_scroll();
      redraw();
    }
    return 1;
  case FL_RELEASE:
    if (dragging_bar_) {
      dragging_bar_ = false;
      redraw();
    }
    return 1;
  case FL_KEYBOARD: {
    int k = Fl::event_key();
    if (k == FL_Down)
      move_selection(1);
    else if (k == FL_Up)
      move_selection(-1);
    else if (k == FL_Home) {
      selected_ = -1;
      move_selection(1);
    } else if (k == FL_End) {
      selected_ = n;
      move_selection(-1);
    } else if (k == FL_Page_Down || k == FL_Page_Up) {
      for (int i = 0; i < 8; ++i)
        move_selection(k == FL_Page_Down ? 1 : -1);
    } else if (k == ' ' && selected_ >= 0 && toggled) {
      toggled(selected_);
      redraw();
    } else if (key_pressed && selected_ >= 0 && key_pressed(selected_, k)) {
      redraw();
    } else {
      return 0;
    }
    return 1;
  }
  default:
    return Fl_Widget::handle(event);
  }
}

// -------------------------------------------------------------- text view

TextView::TextView(int x, int y, int w, int h, bool mono)
    : Fl_Text_Display(x, y, w, h), mono_(mono) {
  buffer_ = new Fl_Text_Buffer();
  buffer(buffer_);
  box(FL_FLAT_BOX);
  wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
  scrollbar_align(FL_ALIGN_RIGHT);
  restyle();
}

void TextView::restyle() {
  const Theme &t = active_theme();
  bool mono = mono_;
  color(t.surface.fl(), t.accent.alpha(0.3).mix(t.surface, 0.5).fl());
  textcolor(t.text.fl());
  textfont(mono ? FL_COURIER : FL_HELVETICA);
  textsize(mono ? std::max(S(10), t.font_size - S(1)) : t.font_size);
  scrollbar_width(S(10));
  for (Fl_Widget *bar : {static_cast<Fl_Widget *>(mVScrollBar),
                         static_cast<Fl_Widget *>(mHScrollBar)}) {
    bar->box(FL_FLAT_BOX);
    bar->color(t.surface.fl());
    bar->selection_color(t.border.fl());
    bar->labelcolor(t.text_dim.fl());
  }
  redraw();
}

TextView::~TextView() {
  buffer(nullptr);
  delete buffer_;
}

void TextView::set(const std::string &text) {
  buffer_->text(text.c_str());
  scroll(0, 0);
}

void TextView::append(const std::string &text) {
  buffer_->append(text.c_str());
}

void TextView::scroll_to_end() {
  scroll(std::max(0, buffer_->count_lines(0, buffer_->length())), 0);
}

void style_choice(Fl_Choice *c) {
  const Theme &t = active_theme();
  c->box(FL_BORDER_BOX);
  c->down_box(FL_BORDER_BOX);
  c->color(t.surface_alt.fl());
  c->textcolor(t.text.fl());
  c->textsize(t.font_size);
  c->labelsize(t.font_size);
  c->selection_color(t.accent.fl());
}

void restyle_tree(Fl_Widget *w) {
  if (!w)
    return;
  if (auto *v = dynamic_cast<TextView *>(w))
    v->restyle();
  else if (auto *f = dynamic_cast<SearchField *>(w))
    f->restyle();
  else if (auto *c = dynamic_cast<Fl_Choice *>(w))
    style_choice(c);
  if (auto *g = w->as_group())
    for (int i = 0; i < g->children(); ++i)
      restyle_tree(g->child(i));
}

std::string menu_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '/' || c == '&' || c == '_' || c == '\\')
      out += '\\';
    out += c;
  }
  return out;
}

// ---------------------------------------------------------------- dialogs

namespace {

class Dialog : public BufferedWindow {
public:
  Dialog(int w, int h, const std::string &title, const std::string &message,
         bool danger)
      : BufferedWindow(w, h, title.c_str()), title_(title), message_(message),
        danger_(danger) {}
  void draw() override {
    const Theme &t = active_theme();
    {
      Painter p(w(), h());
      paint_window_background(p, w(), h());
      paint_icon(p, danger_ ? Icon::Warning : Icon::Info, S(22), S(22), S(32),
                 danger_ ? danger_color() : t.accent);
    }
    text_font(t.font_size + S(3), true);
    draw_text(title_, S(68), S(18), w() - S(90), S(30), t.text);
    text_font(t.font_size);
    fl_color(t.text_dim.fl());
    fl_draw(message_.c_str(), S(68), S(50), w() - S(90), message_h(),
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE));
    draw_children();
  }
  int message_h() const {
    text_font(active_theme().font_size);
    int ww = w() - S(90), hh = 0;
    fl_measure(message_.c_str(), ww, hh, 0);
    return hh + S(4);
  }
  bool accepted = false;

private:
  std::string title_, message_;
  bool danger_;
};

} // namespace

static bool ask(Fl_Window *parent, const std::string &title,
                const std::string &message, const std::string &details,
                const std::string &accept, bool danger, bool single) {
  int w = S(520);
  auto *d = new Dialog(w, S(200), title, message, danger);
  int mh = d->message_h();
  int y = S(50) + mh + S(12);
  TextView *view = nullptr;
  if (!details.empty()) {
    view = new TextView(S(68), y, w - S(90), S(170), false);
    view->set(details);
    y += S(182);
  }
  y += S(8);
  Button *cancel = nullptr;
  if (!single)
    cancel = new Button(w - S(22) - S(130) - S(10) - S(110), y, S(110), S(36),
                        _("Cancel"));
  auto *ok = new Button(w - S(22) - S(130), y, S(130), S(36), accept,
                        danger ? Button::Danger : Button::Primary);
  d->end();
  d->size(w, y + S(56));
  if (cancel)
    cancel->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  ok->callback([](Fl_Widget *b, void *) {
    static_cast<Dialog *>(b->window())->accepted = true;
    b->window()->hide();
  });
  d->set_modal();
  d->xclass("sde-update");
  if (parent)
    d->position(parent->x() + (parent->w() - d->w()) / 2,
                parent->y() + (parent->h() - d->h()) / 3);
  d->show();
  ok->take_focus();
  while (d->shown())
    Fl::wait();
  bool result = d->accepted;
  Fl::delete_widget(d);
  return result;
}

bool confirm(Fl_Window *parent, const std::string &title,
             const std::string &message, const std::string &details,
             const std::string &accept, bool danger) {
  return ask(parent, title, message, details, accept, danger, false);
}

void inform(Fl_Window *parent, const std::string &title,
            const std::string &message) {
  ask(parent, title, message, "", _("OK"), false, true);
}

void show_log(Fl_Window *parent, const std::string &title,
              const std::string &text) {
  static Fl_Window *window = nullptr;
  static TextView *view = nullptr;
  if (!window) {
    struct LogWindow : BufferedWindow {
      LogWindow(int w, int h, const char *l) : BufferedWindow(w, h, l) {}
      void draw() override {
        {
          Painter p(w(), h());
          paint_window_background(p, w(), h());
          paint_card(p, S(12), S(12), w() - S(24), h() - S(24));
        }
        draw_children();
      }
    };
    window = new LogWindow(S(680), S(420), _("Transaction Details"));
    view = new TextView(S(20), S(20), S(640), S(380));
    window->end();
    window->resizable(view);
    window->xclass("sde-update");
  }
  window->copy_label(title.c_str());
  view->set(text);
  view->scroll_to_end();
  if (parent && !window->shown())
    window->position(parent->x() + (parent->w() - window->w()) / 2,
                     parent->y() + (parent->h() - window->h()) / 2);
  window->show();
}

} // namespace update
} // namespace sde
