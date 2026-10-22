// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Keys, icon buttons, glyphs and the expression field of the Calculator.
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <cmath>

#include "theme.h"
#include "ui.h"
#include "widgets.h"

using sde::Color;
using sde::Painter;
using sde::S;
using sde::Sd;

namespace calc {

void draw_glyph(Painter &p, Glyph g, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  p.set(c);
  cairo_save(cr);
  cairo_set_line_width(cr, std::max(1.2, s / 13));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  double u = s / 24;
  auto P = [&](double px, double py) { return std::make_pair(x + px * u, y + py * u); };
  auto move = [&](double px, double py) { auto q = P(px, py); cairo_move_to(cr, q.first, q.second); };
  auto line = [&](double px, double py) { auto q = P(px, py); cairo_line_to(cr, q.first, q.second); };
  switch (g) {
    case Glyph::Backspace:
      move(8, 5);
      line(21, 5);
      line(21, 19);
      line(8, 19);
      line(2, 12);
      cairo_close_path(cr);
      cairo_stroke(cr);
      move(11, 9);
      line(17, 15);
      move(17, 9);
      line(11, 15);
      cairo_stroke(cr);
      break;
    case Glyph::History:
      cairo_new_sub_path(cr);
      cairo_arc(cr, x + 12.5 * u, y + 12 * u, 8.5 * u, M_PI * 0.95, M_PI * 2.85);
      cairo_stroke(cr);
      move(3, 7);
      line(4, 11.5);
      line(8.5, 10.5);
      cairo_stroke(cr);
      move(12.5, 7.5);
      line(12.5, 12);
      line(15.5, 14);
      cairo_stroke(cr);
      break;
    case Glyph::More:
      for (int i = 0; i < 3; ++i) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + (5 + 7 * i) * u, y + 12 * u, 1.7 * u, 0, 2 * M_PI);
        cairo_fill(cr);
      }
      break;
    case Glyph::Menu:
      for (int i = 0; i < 3; ++i) {
        move(4, 6 + 6 * i);
        line(20, 6 + 6 * i);
      }
      cairo_stroke(cr);
      break;
    case Glyph::Swap:
      move(8, 4);
      line(8, 20);
      move(4, 8);
      line(8, 4);
      line(12, 8);
      move(16, 20);
      line(16, 4);
      move(12, 16);
      line(16, 20);
      line(20, 16);
      cairo_stroke(cr);
      break;
    case Glyph::Chevron:
      move(7, 10);
      line(12, 15);
      line(17, 10);
      cairo_stroke(cr);
      break;
    case Glyph::Trash:
      move(4, 7);
      line(20, 7);
      move(9, 7);
      line(9, 4);
      line(15, 4);
      line(15, 7);
      move(6, 7);
      line(7, 20);
      line(17, 20);
      line(18, 7);
      move(10, 11);
      line(10, 16);
      move(14, 11);
      line(14, 16);
      cairo_stroke(cr);
      break;
  }
  cairo_restore(cr);
}

// ------------------------------------------------------------------- Key

Key::Key(int x, int y, int w, int h, std::string id, std::string label, Kind kind)
    : Fl_Widget(x, y, w, h), id_(std::move(id)), label_(std::move(label)), kind_(kind) {
  box(FL_NO_BOX);
  clear_visible_focus();
}

void Key::flash() {
  down_ = true;
  redraw();
  Fl::remove_timeout(unflash, this);
  Fl::add_timeout(0.12, unflash, this);
}

void Key::unflash(void *data) {
  auto *k = static_cast<Key *>(data);
  k->down_ = false;
  k->redraw();
}

void Key::draw() {
  const sde::Theme &t = sde::active_theme();
  bool hover = Fl::belowmouse() == this && active_r();
  bool on = active_r();
  Color fill, text = t.text;
  switch (kind_) {
    case Digit: fill = t.dark ? t.surface.mix(t.text, 0.08) : t.surface; break;
    case Operator: fill = t.dark ? t.surface.mix(t.text, 0.03) : t.surface_alt.mix(t.background, 0.3); break;
    case Function: fill = t.dark ? t.surface.mix(t.background, 0.35) : t.surface_alt.mix(t.background, 0.55); break;
    case Memory: fill = t.background; break;
    case Equals:
      fill = t.accent;
      text = t.on_accent;
      break;
  }
  if (toggled_) text = t.accent_text;
  if (kind_ == Equals) {
    if (down_) fill = fill.mix(t.dark ? Color{1, 1, 1} : Color{0, 0, 0}, 0.18);
    else if (hover) fill = fill.mix(t.dark ? Color{1, 1, 1} : Color{0, 0, 0}, 0.09);
  } else if (down_) {
    fill = fill.mix(t.text, t.dark ? 0.16 : 0.12);
  } else if (hover) {
    fill = kind_ == Memory ? t.hover : fill.mix(t.text, t.dark ? 0.08 : 0.05);
  }
  {
    Painter p(window()->w(), window()->h());
    double r = std::min(double(t.radius_small + S(2)), h() / 2.0);
    if (kind_ != Memory || hover || down_ || toggled_) {
      if (toggled_) {
        p.fill_rounded(x(), y(), w(), h(), r, kind_ == Equals ? t.accent : Color(fill));
        p.fill_rounded(x(), y(), w(), h(), r, t.accent.alpha(t.dark ? 0.30 : 0.16));
      } else {
        p.fill_rounded(x(), y(), w(), h(), r, fill);
      }
      if (kind_ != Equals && kind_ != Memory && !t.dark)
        p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.border.alpha(0.55));
      if (kind_ != Equals && kind_ != Memory && t.dark)
        p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, Color{1, 1, 1, 0.06});
    }
    if (glyph_ >= 0) {
      double s = std::min(S(22), int(h() * 0.55));
      draw_glyph(p, Glyph(glyph_), x() + (w() - s) / 2, y() + (h() - s) / 2, s,
                 on ? text : t.text_dim.alpha(0.45));
    }
  }
  if (glyph_ >= 0) return;
  int size;
  if (kind_ == Digit) size = std::min(S(21), std::max(S(13), h() * 2 / 5));
  else if (kind_ == Function || kind_ == Memory) size = std::min(S(15), std::max(S(11), h() / 3));
  else size = std::min(S(21), std::max(S(13), h() * 2 / 5));
  if (label_.size() > 4 && kind_ != Digit) size = std::min(size, S(13));
  sde::text_font(size, kind_ == Digit || kind_ == Equals);
  Color c = on ? text : t.text_dim.alpha(0.45);
  if (kind_ == Operator && on && !toggled_) c = t.text;
  sde::draw_text(label_, x() + S(2), y(), w() - S(4), h(), c, 1);
}

int Key::handle(int event) {
  switch (event) {
    case FL_ENTER:
      if (!tooltip_text.empty()) tooltip(tooltip_text.c_str());
      redraw();
      return 1;
    case FL_LEAVE:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      down_ = true;
      redraw();
      return 1;
    case FL_DRAG: {
      bool inside = Fl::event_inside(this);
      if (inside != down_) {
        down_ = inside;
        redraw();
      }
      return 1;
    }
    case FL_RELEASE:
      if (down_) {
        down_ = false;
        redraw();
        if (Fl::event_inside(this) && on_press) on_press(id_);
      }
      return 1;
    default: break;
  }
  return Fl_Widget::handle(event);
}

// ------------------------------------------------------------ IconButton

IconButton::IconButton(int x, int y, int w, int h, Glyph g, std::string text)
    : Fl_Widget(x, y, w, h), glyph_(g), text_(std::move(text)) {
  box(FL_NO_BOX);
  clear_visible_focus();
}

void IconButton::draw() {
  const sde::Theme &t = sde::active_theme();
  bool hover = Fl::belowmouse() == this;
  double s = S(20);
  {
    Painter p(window()->w(), window()->h());
    if (toggled_) p.fill_rounded(x(), y(), w(), h(), t.radius_small, t.accent.alpha(t.dark ? 0.28 : 0.15));
    else if (down_) p.fill_rounded(x(), y(), w(), h(), t.radius_small, t.hover.mix(t.text, 0.08));
    else if (hover) p.fill_rounded(x(), y(), w(), h(), t.radius_small, t.hover);
    Color c = toggled_ ? t.accent_text : t.text;
    if (text_.empty()) {
      draw_glyph(p, glyph_, x() + (w() - s) / 2, y() + (h() - s) / 2, s, c);
    } else {
      draw_glyph(p, glyph_, x() + S(8), y() + (h() - s) / 2, s, c);
      draw_glyph(p, Glyph::Chevron, x() + w() - S(22), y() + (h() - S(16)) / 2.0, S(16), t.text_dim);
    }
  }
  if (!text_.empty()) {
    sde::text_font(t.font_size + S(3), true);
    sde::draw_text(text_, x() + S(36), y(), w() - S(60), h(), t.text);
  }
}

int IconButton::handle(int event) {
  switch (event) {
    case FL_ENTER:
    case FL_LEAVE: redraw(); return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      down_ = true;
      redraw();
      return 1;
    case FL_RELEASE:
      down_ = false;
      redraw();
      if (Fl::event_inside(this) && on_press) on_press();
      return 1;
    default: break;
  }
  return Fl_Widget::handle(event);
}

// ------------------------------------------------------------- ExprInput

ExprInput::ExprInput(int x, int y, int w, int h, CalcWindow *owner) : Fl_Input(x, y, w, h), owner_(owner) {
  box(FL_FLAT_BOX);
  when(FL_WHEN_CHANGED);
  callback([](Fl_Widget *, void *data) { static_cast<CalcWindow *>(data)->input_changed(); }, owner);
}

void ExprInput::draw() {
  // The field is drawn on the display card's color, without a frame.
  Fl_Input::draw();
}

int ExprInput::handle(int event) {
  if (event == FL_KEYBOARD && Fl::focus() == this) {
    if (owner_->key_event()) return 1;
  }
  if (event == FL_PASTE) {
    owner_->paste_text(Fl::event_text() ? Fl::event_text() : "");
    return 1;
  }
  return Fl_Input::handle(event);
}

}  // namespace calc
