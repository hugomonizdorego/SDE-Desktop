// SPDX-License-Identifier: GPL-3.0-or-later
#include "widgets.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "theme.h"

namespace sde {

void focus_ring(Painter &p, double x, double y, double w, double h, double r) {
  const Theme &t = active_theme();
  p.stroke_rounded(x - Sd(2), y - Sd(2), w + Sd(4), h + Sd(4), r + Sd(2), t.accent, Sd(2));
}

static bool hovered(Fl_Widget *w) { return Fl::belowmouse() == w; }

// ----------------------------------------------------------------- slider

Slider::Slider(int x, int y, int w, int h, const char *label) : Fl_Slider(x, y, w, h, label) {
  type(FL_HOR_SLIDER);
  box(FL_NO_BOX);
  step(1);
  bounds(0, 100);
}

// The knob of switches and sliders (0.2.0 Alpha 2): a light disc with a soft
// shadow under it and a hairline edge, like a physical button.
static void knob(Painter &p, double cx, double cy, double r, const Theme &t, bool active, bool hot) {
  for (int i = 3; i >= 1; --i)
    p.circle(cx, cy + Sd(0.6) * i, r + Sd(0.5) * i, Color::hex(0x000000, (t.dark ? 0.10 : 0.05) / i));
  Color face = t.dark ? Color::hex(0xF2F2F4) : Color::hex(0xFFFFFF);
  if (!active) face = face.mix(t.surface_alt, 0.5);
  else if (hot) face = face.mix(t.accent, 0.08);
  p.circle(cx, cy, r, t.dark ? face.mix(Color::hex(0x000000), 0.25) : t.border.mix(t.text_dim, 0.25));
  p.circle(cx, cy, r - 1, face);
}

void Slider::draw() {
  const Theme &t = active_theme();
  Painter p(window()->w(), window()->h());
  double track = Sd(4), kr = std::min(h() * 0.42, Sd(9));
  double x0 = x() + kr + Sd(1), x1 = x() + w() - kr - Sd(1), cy = y() + h() / 2.0;
  double range = maximum() - minimum();
  double f = range != 0 ? (value() - minimum()) / range : 0;
  f = std::max(0.0, std::min(1.0, f));
  double kx = x0 + (x1 - x0) * f;
  // The rest of the track: visible against the surface (WCAG 3:1 for UI
  // parts would be heavy; a mid gray between the border and dimmed text).
  p.fill_rounded(x0, cy - track / 2, x1 - x0, track, track / 2, t.border.mix(t.text_dim, t.dark ? 0.25 : 0.35));
  if (active_r()) p.fill_rounded(x0, cy - track / 2, kx - x0, track, track / 2, t.accent);
  knob(p, kx, cy, kr, t, active_r(), Fl::belowmouse() == this);
  if (Fl::focus() == this) {
    p.set(t.accent.alpha(0.85));
    cairo_set_line_width(p.cr(), Sd(2));
    cairo_new_sub_path(p.cr());
    cairo_arc(p.cr(), kx, cy, kr + Sd(3), 0, 2 * M_PI);
    cairo_stroke(p.cr());
  }
}

int Slider::handle(int event) {
  if (event == FL_FOCUS || event == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  if (event == FL_ENTER || event == FL_LEAVE) {
    redraw();  // the knob lights up under the pointer
    Fl_Slider::handle(event);
    return 1;
  }
  if (event == FL_MOUSEWHEEL && Fl::event_inside(this)) {
    value(clamp(value() - Fl::event_dy() * 5));
    set_changed();
    do_callback();
    redraw();
    return 1;
  }
  if (event == FL_KEYBOARD) {
    int k = Fl::event_key();
    double v = value();
    if (k == FL_Left || k == FL_Down) v -= 5;
    else if (k == FL_Right || k == FL_Up) v += 5;
    else if (k == FL_Home) v = minimum();
    else if (k == FL_End) v = maximum();
    else return Fl_Slider::handle(event);
    value(clamp(v));
    set_changed();
    do_callback();
    redraw();
    return 1;
  }
  return Fl_Slider::handle(event);
}

// ----------------------------------------------------------------- switch

Switch::Switch(int x, int y, int w, int h, const char *label) : Fl_Button(x, y, w, h, label) {
  type(FL_TOGGLE_BUTTON);
  box(FL_NO_BOX);
  down_box(FL_NO_BOX);
}

void Switch::draw() {
  const Theme &t = active_theme();
  Painter p(window()->w(), window()->h());
  double sw = Sd(40), sh = Sd(22);
  double sx = x() + w() - sw, sy = y() + (h() - sh) / 2;
  bool on = value() != 0, hot = hovered(this) && active_r();
  // 0.2.0 Alpha 2: an accent track when on, a mid gray one when off, and a
  // light knob with a soft shadow.
  Color off = t.border.mix(t.text_dim, t.dark ? 0.30 : 0.42);
  Color track = on ? (hot ? t.accent.mix(t.text, 0.10) : t.accent) : hot ? off.mix(t.text, 0.10) : off;
  if (!active_r()) track = track.mix(t.surface_alt, 0.55);
  p.fill_rounded(sx, sy, sw, sh, sh / 2, track);
  double r = sh / 2 - Sd(2.5);
  knob(p, on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, r, t, active_r(), false);
  if (Fl::focus() == this) focus_ring(p, sx, sy, sw, sh, sh / 2);
  p.flush();
  if (label()) {
    fl_font(FL_HELVETICA, t.font_size + 1);
    fl_color(t.text.fl());
    fl_draw(label(), x(), y(), w() - int(sw) - S(12), h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  }
}

int Switch::handle(int event) {
  if (event == FL_FOCUS || event == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  if (event == FL_ENTER || event == FL_LEAVE) {
    redraw();  // the track lights up under the pointer
    Fl_Button::handle(event);
    return 1;
  }
  if (event == FL_KEYBOARD && Fl::focus() == this &&
      (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
    value(!value());
    set_changed();
    do_callback();
    return 1;
  }
  return Fl_Button::handle(event);
}

// ------------------------------------------------------------ flat button

FlatButton::FlatButton(int x, int y, int w, int h, const std::string &text, bool primary)
    : Fl_Button(x, y, w, h), text_(text), primary_(primary) {
  box(FL_NO_BOX);
  down_box(FL_NO_BOX);
}

void FlatButton::draw() {
  const Theme &t = active_theme();
  bool hot = hovered(this), down = value() != 0;
  {
    Painter p(window()->w(), window()->h());
    Color bg = primary_ ? t.accent : t.surface_alt;
    if (!primary_ && hot) bg = t.hover;
    if (primary_ && hot) bg = t.accent.mix(t.dark ? Color::hex(0xFFFFFF) : t.text, t.dark ? 0.10 : 0.12);
    if (down) bg = bg.mix(t.text, 0.15);
    if (!active_r()) bg = t.surface_alt.mix(t.surface, 0.5);
    double r = t.radius_for(h());
    p.fill_rounded(x(), y(), w(), h(), r, bg);
    // 0.2.0 Alpha 2: raised like the stock buttons (menu.cpp), a hairline of
    // light inside the top of the edge; the accent button is edged in a
    // deeper accent.
    bool lit = active_r() && !down;
    if (primary_ && active_r())
      p.surface_edge(x(), y(), w(), h(), r, bg.mix(Color::hex(0x000000), 0.14), lit ? 0.3 : 0.0, false);
    else if (!primary_)
      p.surface_edge(x(), y(), w(), h(), r, t.border, lit ? 0.6 : 0.0, t.dark);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  text_font(t.font_size, primary_);
  Color fg = primary_ ? t.on_accent : t.text;
  if (!active_r()) fg = t.text_dim.alpha(0.7).mix(t.surface_alt, 0.3);
  draw_text(text_, x() + S(8), y(), w() - S(16), h(), fg, 1);
}

int FlatButton::handle(int event) {
  if (event == FL_ENTER || event == FL_LEAVE || event == FL_FOCUS || event == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  if (event == FL_KEYBOARD && Fl::focus() == this &&
      (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
    do_callback();
    return 1;
  }
  return Fl_Button::handle(event);
}

// ----------------------------------------------------------- drawn button

DrawnButton::DrawnButton(int x, int y, int w, int h, DrawFn draw, TextFn text, double radius)
    : Fl_Button(x, y, w, h), draw_fn_(std::move(draw)), text_fn_(std::move(text)), radius_(radius) {
  box(FL_NO_BOX);
  down_box(FL_NO_BOX);
}

void DrawnButton::draw() {
  bool hot = hovered(this), down = value() != 0;
  {
    Painter p(window()->w(), window()->h());
    if (draw_fn_) draw_fn_(p, hot, down);
    if (Fl::focus() == this) {
      double r = radius_ >= 0 ? radius_ : active_theme().radius;
      focus_ring(p, x(), y(), w(), h(), r);
    }
  }
  if (text_fn_) text_fn_(hot);
}

int DrawnButton::handle(int event) {
  if (event == FL_ENTER || event == FL_LEAVE || event == FL_FOCUS || event == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  if (event == FL_KEYBOARD && Fl::focus() == this &&
      (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
    do_callback();
    return 1;
  }
  return Fl_Button::handle(event);
}

}  // namespace sde
