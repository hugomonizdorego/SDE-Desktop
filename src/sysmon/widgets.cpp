// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <algorithm>
#include <cmath>

#include "glyphs.h"
#include "spawn.h"
#include "widgets.h"
#include "ui.h"

namespace sysmon {

const Palette &palette() {
  static Palette p = [] {
    Palette q;
    const sde::Theme &t = sde::active_theme();
    q.cpu = t.accent;
    q.memory = Color::hex(0x8a5cf6);
    q.swap = Color::hex(0xd946a8);
    q.disk = Color::hex(0x16a34a);
    q.disk_write = Color::hex(0x0ea5a4);
    q.net = Color::hex(0xea7a1a);
    q.net_send = Color::hex(0xd4a017);
    q.gpu = Color::hex(0x0891d1);
    q.battery = Color::hex(0x65a30d);
    q.danger = Color::hex(t.dark ? 0xf87171 : 0xdc2626);
    q.warning = Color::hex(t.dark ? 0xfbbf24 : 0xd97706);
    if (t.dark) {
      q.memory = Color::hex(0xa78bfa);
      q.disk = Color::hex(0x34d399);
      q.disk_write = Color::hex(0x2dd4bf);
      q.net = Color::hex(0xfb923c);
      q.net_send = Color::hex(0xfacc15);
      q.gpu = Color::hex(0x38bdf8);
      q.battery = Color::hex(0xa3e635);
      q.swap = Color::hex(0xf472b6);
    }
    return q;
  }();
  return p;
}

// ----------------------------------------------------------------- icons

void draw_icon(Painter &p, Icon icon, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, s, s);
  cairo_set_line_width(cr, std::max(1.4, s / 12.0) / s);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  p.set(c);
  auto rrect = [&](double rx, double ry, double rw, double rh, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, rx + rw - r, ry + r, r, -M_PI / 2, 0);
    cairo_arc(cr, rx + rw - r, ry + rh - r, r, 0, M_PI / 2);
    cairo_arc(cr, rx + r, ry + rh - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, rx + r, ry + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
  };
  switch (icon) {
    case Icon::App: {
      rrect(0.06, 0.06, 0.88, 0.88, 0.22);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, 1, 1, 1, 1);
      cairo_set_line_width(cr, 0.09);
      cairo_move_to(cr, 0.2, 0.56);
      cairo_line_to(cr, 0.36, 0.56);
      cairo_line_to(cr, 0.46, 0.3);
      cairo_line_to(cr, 0.58, 0.74);
      cairo_line_to(cr, 0.67, 0.48);
      cairo_line_to(cr, 0.8, 0.48);
      cairo_stroke(cr);
      break;
    }
    case Icon::Processes:
      for (int i = 0; i < 3; ++i) {
        double cy = 0.22 + i * 0.28;
        cairo_arc(cr, 0.14, cy, 0.075, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_move_to(cr, 0.34, cy);
        cairo_line_to(cr, 0.9, cy);
        cairo_stroke(cr);
      }
      break;
    case Icon::Performance:
      cairo_move_to(cr, 0.06, 0.56);
      cairo_line_to(cr, 0.28, 0.56);
      cairo_line_to(cr, 0.4, 0.2);
      cairo_line_to(cr, 0.58, 0.84);
      cairo_line_to(cr, 0.7, 0.46);
      cairo_line_to(cr, 0.94, 0.46);
      cairo_stroke(cr);
      break;
    case Icon::Disks:
    case Icon::Drive:
      rrect(0.08, 0.24, 0.84, 0.52, 0.12);
      cairo_stroke(cr);
      cairo_move_to(cr, 0.08, 0.56);
      cairo_line_to(cr, 0.92, 0.56);
      cairo_stroke(cr);
      cairo_arc(cr, 0.76, 0.66, 0.045, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Icon::Startup:
      cairo_move_to(cr, 0.58, 0.06);
      cairo_line_to(cr, 0.2, 0.56);
      cairo_line_to(cr, 0.48, 0.56);
      cairo_line_to(cr, 0.4, 0.94);
      cairo_line_to(cr, 0.8, 0.42);
      cairo_line_to(cr, 0.52, 0.42);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Icon::Search:
      cairo_arc(cr, 0.42, 0.42, 0.28, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 0.63, 0.63);
      cairo_line_to(cr, 0.9, 0.9);
      cairo_stroke(cr);
      break;
    case Icon::Folder:
      cairo_move_to(cr, 0.08, 0.24);
      cairo_line_to(cr, 0.38, 0.24);
      cairo_line_to(cr, 0.48, 0.34);
      cairo_line_to(cr, 0.92, 0.34);
      cairo_line_to(cr, 0.92, 0.8);
      cairo_line_to(cr, 0.08, 0.8);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Icon::Chip:
      rrect(0.22, 0.22, 0.56, 0.56, 0.08);
      cairo_stroke(cr);
      rrect(0.38, 0.38, 0.24, 0.24, 0.03);
      cairo_fill(cr);
      for (int i = 0; i < 3; ++i) {
        double o = 0.34 + i * 0.16;
        cairo_move_to(cr, o, 0.06), cairo_line_to(cr, o, 0.22);
        cairo_move_to(cr, o, 0.78), cairo_line_to(cr, o, 0.94);
        cairo_move_to(cr, 0.06, o), cairo_line_to(cr, 0.22, o);
        cairo_move_to(cr, 0.78, o), cairo_line_to(cr, 0.94, o);
      }
      cairo_stroke(cr);
      break;
    case Icon::Memory:
      rrect(0.06, 0.3, 0.88, 0.4, 0.06);
      cairo_stroke(cr);
      for (int i = 0; i < 4; ++i) {
        double o = 0.2 + i * 0.2;
        cairo_rectangle(cr, o - 0.05, 0.4, 0.1, 0.2);
        cairo_fill(cr);
        cairo_move_to(cr, o, 0.7), cairo_line_to(cr, o, 0.82);
      }
      cairo_stroke(cr);
      break;
    case Icon::Network:
      cairo_move_to(cr, 0.32, 0.86), cairo_line_to(cr, 0.32, 0.14);
      cairo_move_to(cr, 0.14, 0.32), cairo_line_to(cr, 0.32, 0.14), cairo_line_to(cr, 0.5, 0.32);
      cairo_move_to(cr, 0.68, 0.14), cairo_line_to(cr, 0.68, 0.86);
      cairo_move_to(cr, 0.5, 0.68), cairo_line_to(cr, 0.68, 0.86), cairo_line_to(cr, 0.86, 0.68);
      cairo_stroke(cr);
      break;
    case Icon::Gpu:
      rrect(0.06, 0.22, 0.88, 0.56, 0.08);
      cairo_stroke(cr);
      cairo_arc(cr, 0.38, 0.5, 0.15, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 0.66, 0.38), cairo_line_to(cr, 0.8, 0.38);
      cairo_move_to(cr, 0.66, 0.5), cairo_line_to(cr, 0.8, 0.5);
      cairo_move_to(cr, 0.66, 0.62), cairo_line_to(cr, 0.8, 0.62);
      cairo_stroke(cr);
      break;
    case Icon::Battery:
      rrect(0.06, 0.28, 0.78, 0.44, 0.08);
      cairo_stroke(cr);
      cairo_rectangle(cr, 0.86, 0.42, 0.08, 0.16);
      cairo_fill(cr);
      cairo_rectangle(cr, 0.16, 0.38, 0.4, 0.24);
      cairo_fill(cr);
      break;
  }
  cairo_restore(cr);
}

// ----------------------------------------------------------------- graph

double Series::max() const {
  double m = 0;
  for (double v : values) m = std::max(m, v);
  return m;
}

static void graph_path(cairo_t *cr, double x, double y, double w, double h, const Series &s,
                       double top, double *first_x, double *last_x) {
  size_t n = s.values.size();
  double step = w / 60.0;
  double px = 0, py = 0;
  for (size_t i = 0; i < n; ++i) {
    double v = top > 0 ? std::max(0.0, std::min(1.0, s.values[i] / top)) : 0;
    double gx = x + w - double(n - 1 - i) * step;
    double gy = y + h - v * (h - 1);
    if (i == 0) {
      cairo_move_to(cr, gx, gy);
      *first_x = gx;
    } else {
      // Horizontal tangents: smooth and never overshooting the data.
      double mx = (px + gx) / 2;
      cairo_curve_to(cr, mx, py, mx, gy, gx, gy);
    }
    px = gx, py = gy;
  }
  *last_x = px;
}

void draw_graph(Painter &p, double x, double y, double w, double h, const Series &a,
                const Color &ca, const Series *b, const Color &cb, double top, bool grid) {
  const sde::Theme &t = sde::active_theme();
  cairo_t *cr = p.cr();
  cairo_save(cr);
  double r = Sd(8);
  p.fill_rounded(x, y, w, h, r, t.dark ? t.background.mix(t.surface, 0.4) : t.surface_alt.mix(t.surface, 0.3));
  p.rounded_rect(x, y, w, h, r);
  cairo_clip(cr);
  if (grid) {
    Color gc = t.border.alpha(t.dark ? 0.55 : 0.7);
    for (int i = 1; i < 4; ++i) p.line(x, std::round(y + h * i / 4.0) + 0.5, x + w, std::round(y + h * i / 4.0) + 0.5, gc, 1);
    for (int i = 1; i < 6; ++i) p.line(std::round(x + w * i / 6.0) + 0.5, y, std::round(x + w * i / 6.0) + 0.5, y + h, gc.alpha(gc.a * 0.6), 1);
  }
  auto plot = [&](const Series &s, const Color &c, double fill_top, double fill_bottom) {
    if (s.values.size() < 2) return;
    double fx = 0, lx = 0;
    graph_path(cr, x, y, w, h, s, top, &fx, &lx);
    cairo_path_t *line = cairo_copy_path(cr);
    cairo_line_to(cr, lx, y + h);
    cairo_line_to(cr, fx, y + h);
    cairo_close_path(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(g, 0, c.r, c.g, c.b, fill_top);
    cairo_pattern_add_color_stop_rgba(g, 1, c.r, c.g, c.b, fill_bottom);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_new_path(cr);
    cairo_append_path(cr, line);
    cairo_path_destroy(line);
    p.set(c);
    cairo_set_line_width(cr, Sd(1.7));
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
  };
  if (b) plot(*b, cb, 0.10, 0.0);
  plot(a, ca, t.dark ? 0.42 : 0.32, 0.03);
  cairo_restore(cr);
  p.stroke_rounded(x + 0.5, y + 0.5, w - 1, h - 1, r, t.border.alpha(0.8));
}

// ---------------------------------------------------------- MonitorWindow

MonitorWindow::MonitorWindow(int w, int h, const char *label) : Fl_Window(w, h, label) {}
MonitorWindow::~MonitorWindow() { free_buffer(); }

void MonitorWindow::free_buffer() {
  if (buffer_) fl_delete_offscreen(buffer_);
  buffer_ = 0;
}

void MonitorWindow::hide() {
  free_buffer();
  Fl_Window::hide();
}

void MonitorWindow::resize(int x, int y, int w, int h) {
  if (w != this->w() || h != this->h()) free_buffer();
  Fl_Window::resize(x, y, w, h);
}

void MonitorWindow::flush() {
  if (!shown()) return;
  make_current();
  bool fresh = false;
  if (!buffer_ || buffer_w_ != w() || buffer_h_ != h()) {
    free_buffer();
    buffer_ = fl_create_offscreen(w(), h());
    buffer_w_ = w();
    buffer_h_ = h();
    fresh = true;
  }
  uchar d = damage();
  if (fresh || (d & ~(FL_DAMAGE_CHILD | FL_DAMAGE_EXPOSE))) {
    damage(FL_DAMAGE_ALL);
    d = FL_DAMAGE_ALL;
  }
  if (d & ~FL_DAMAGE_EXPOSE) {
    fl_begin_offscreen(buffer_);
    fl_push_no_clip();
    draw();
    fl_pop_clip();
    fl_end_offscreen();
  }
  fl_copy_offscreen(0, 0, w(), h(), buffer_, 0, 0);
}

void MonitorWindow::draw() {
  if (damage() & ~FL_DAMAGE_CHILD) {
    fl_color(sde::active_theme().background.fl());
    fl_rectf(0, 0, w(), h());
  }
  draw_children();
}

// --------------------------------------------------------------- TextLine

TextLine::TextLine(int x, int y, int w, int h, int size_delta, bool bold, bool dim)
    : Fl_Widget(x, y, w, h), size_delta_(size_delta), bold_(bold), dim_(dim) {
  bg = sde::active_theme().background;
  box(FL_NO_BOX);
}

void TextLine::text(const std::string &t) {
  if (t == text_) return;
  text_ = t;
  redraw();
}

void TextLine::draw() {
  const sde::Theme &t = sde::active_theme();
  fl_color(bg.fl());
  fl_rectf(x(), y(), w(), h());
  sde::text_font(t.font_size + size_delta_, bold_);
  sde::draw_text(text_, x(), y(), w(), h(), dim_ ? t.text_dim : t.text);
}

// ------------------------------------------------------------------- Chip

Chip::Chip(int x, int y, int w, int h, const std::string &text) : Fl_Button(x, y, w, h), text_(text) {
  type(FL_TOGGLE_BUTTON);
  box(FL_NO_BOX);
  down_box(FL_NO_BOX);
  bg = sde::active_theme().background;
}

void Chip::draw() {
  const sde::Theme &t = sde::active_theme();
  bool on = value() != 0, hot = Fl::belowmouse() == this;
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), bg);
    double r = h() / 2.0;
    Color fill = on ? t.accent.alpha(t.dark ? 0.28 : 0.16) : hot ? t.hover : t.surface_alt;
    p.fill_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, fill);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, on ? t.accent : t.border);
    if (on) sde::glyph::check(p, x() + S(10), y() + (h() - S(14)) / 2.0, S(14), t.accent_text);
    if (Fl::focus() == this) sde::focus_ring(p, x(), y(), w(), h(), r);
  }
  sde::text_font(t.font_size, on);
  int pad = on ? S(28) : S(12);
  sde::draw_text(text_, x() + pad, y(), w() - pad - S(12), h(), on ? t.accent_text : t.text, on ? 0 : 1);
}

int Chip::handle(int event) {
  if (event == FL_ENTER || event == FL_LEAVE || event == FL_FOCUS || event == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  return Fl_Button::handle(event);
}

// ------------------------------------------------------------ SearchField

namespace {
class PlainInput : public Fl_Input {
 public:
  PlainInput(int x, int y, int w, int h, const std::string *placeholder)
      : Fl_Input(x, y, w, h), placeholder_(placeholder) {}
  void draw() override {
    Fl_Input::draw();
    if (size() == 0 && Fl::focus() != this) {
      const sde::Theme &t = sde::active_theme();
      fl_font(textfont(), textsize());
      fl_color(t.text_dim.fl());
      fl_push_clip(x(), y(), w(), h());
      fl_draw(placeholder_->c_str(), x() + Fl::box_dx(box()) + 2, y(), w(), h(), FL_ALIGN_LEFT);
      fl_pop_clip();
    }
  }
  int handle(int e) override {
    if (e == FL_FOCUS || e == FL_UNFOCUS) {
      parent()->redraw();
    }
    return Fl_Input::handle(e);
  }

 private:
  const std::string *placeholder_;
};
}  // namespace

SearchField::SearchField(int x, int y, int w, int h, const std::string &placeholder)
    : Fl_Group(x, y, w, h), placeholder_(placeholder) {
  const sde::Theme &t = sde::active_theme();
  bg = t.background;
  int pad = S(34);
  input_ = new PlainInput(x + pad, y + S(5), w - pad - S(10), h - S(10), &placeholder_);
  input_->box(FL_FLAT_BOX);
  input_->color(t.surface.fl());
  input_->selection_color(t.accent.fl());
  input_->textcolor(t.text.fl());
  input_->cursor_color(t.text.fl());
  input_->textsize(t.font_size);
  input_->when(FL_WHEN_CHANGED);
  end();
  box(FL_NO_BOX);
}

std::string SearchField::value() const { return input_->value() ? input_->value() : ""; }

void SearchField::draw() {
  const sde::Theme &t = sde::active_theme();
  if (damage() & ~FL_DAMAGE_CHILD) {
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), bg);
      bool focus = Fl::focus() == input_;
      p.fill_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(10), t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(10), focus ? t.accent : t.border,
                       focus ? Sd(1.5) : 1);
      draw_icon(p, Icon::Search, x() + S(11), y() + (h() - S(16)) / 2.0, S(16), t.text_dim);
    }
    draw_child(*input_);
  } else {
    update_child(*input_);
  }
}

// ------------------------------------------------------------------- Page

Page::Page(int x, int y, int w, int h, const std::string &title) : Fl_Group(x, y, w, h), title_(title) {
  box(FL_NO_BOX);
  subtitle_ = new TextLine(x + S(28), y + S(48), w - S(56), S(22), 0, false, true);
}

void Page::draw() {
  const sde::Theme &t = sde::active_theme();
  if (damage() & ~FL_DAMAGE_CHILD) {
    fl_color(t.background.fl());
    fl_rectf(x(), y(), w(), h());
    sde::text_font(t.font_size + S(9), true);
    sde::draw_text(title_, x() + S(28), y() + S(14), w() - S(56), S(34), t.text);
    draw_children();
    return;
  }
  // Partial update: clear behind each damaged child first, so that
  // anti-aliased rounded edges are not painted over themselves.
  Fl_Widget *const *a = array();
  for (int i = children(); i--;) {
    Fl_Widget *c = *a++;
    if (!c->damage() || !c->visible()) continue;
    if (c->damage() & ~FL_DAMAGE_CHILD) {
      fl_color(t.background.fl());
      fl_rectf(c->x(), c->y(), c->w(), c->h());
      c->damage(FL_DAMAGE_ALL);
    }
    update_child(*c);
  }
}

void open_folder(const std::string &path) {
  if (!sde::spawn_detached({"liria", path})) sde::open_uri("file://" + path);
}

}  // namespace sysmon
