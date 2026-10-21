// SPDX-License-Identifier: GPL-3.0-or-later
#include "tray_icons.h"

#include <algorithm>
#include <cmath>

namespace sde {
namespace tray_icon {

namespace {

// All icons are drawn on a 24-unit grid with a 1.8-unit stroke.
struct Grid {
  Painter &p;
  cairo_t *cr;
  double x, y, u;
  Grid(Painter &painter, double x0, double y0, double size)
      : p(painter), cr(painter.cr()), x(x0), y(y0), u(size / 24.0) {}
  double X(double v) const { return x + v * u; }
  double Y(double v) const { return y + v * u; }
  void move(double a, double b) { cairo_move_to(cr, X(a), Y(b)); }
  void to(double a, double b) { cairo_line_to(cr, X(a), Y(b)); }
  void arc(double cx, double cy, double r, double a0, double a1) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, X(cx), Y(cy), r * u, a0, a1);
  }
  void rrect(double a, double b, double w, double h, double r) { p.rounded_rect(X(a), Y(b), w * u, h * u, r * u); }
  void stroke(const Color &c, double width = 1.8) {
    p.set(c);
    cairo_set_line_width(cr, width * u);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
  }
  void fill(const Color &c) {
    p.set(c);
    cairo_fill(cr);
  }
  void dot(double cx, double cy, double r, const Color &c) { p.circle(X(cx), Y(cy), r * u, c); }
};

Color dim(const Color &c) { return c.alpha(c.a * 0.32); }

// Small cross badge in the lower-right corner (disconnected).
void cross(Grid &g, const Color &c) {
  g.move(16.5, 16.5);
  g.to(21.5, 21.5);
  g.move(21.5, 16.5);
  g.to(16.5, 21.5);
  g.stroke(c, 2.0);
}

}  // namespace

void wifi(Painter &p, double x, double y, double size, int strength, bool connected,
          const Color &c) {
  Grid g(p, x, y, size);
  strength = connected ? std::clamp(strength, 1, 4) : 0;
  // Three arcs and a dot fanning up from (12, 19).
  for (int i = 1; i <= 3; ++i) {
    g.arc(12, 19.5, 3.4 + 4.3 * i, -M_PI * 0.76, -M_PI * 0.24);
    g.stroke(strength > i ? c : dim(c));
  }
  g.dot(12, 18.6, 1.9, strength > 0 ? c : dim(c));
  if (!connected) cross(g, c);
}

void wired(Painter &p, double x, double y, double size, bool connected, const Color &c) {
  Grid g(p, x, y, size);
  // RJ45 plug: body, latch and three pins, cable below.
  g.rrect(5, 4, 14, 11, 2);
  g.stroke(connected ? c : dim(c));
  for (double px : {9.0, 12.0, 15.0}) {
    g.move(px, 4.5);
    g.to(px, 7.5);
  }
  g.stroke(connected ? c : dim(c), 1.5);
  g.move(9, 15);
  g.to(9, 17.5);
  g.to(15, 17.5);
  g.to(15, 15);
  g.move(12, 17.5);
  g.to(12, 21);
  g.stroke(connected ? c : dim(c));
  if (!connected) cross(g, c);
}

void offline(Painter &p, double x, double y, double size, const Color &c) {
  Grid g(p, x, y, size);
  g.arc(12, 12, 8.5, 0, 2 * M_PI);
  g.stroke(dim(c));
  cairo_save(g.cr);
  cairo_translate(g.cr, g.X(12), g.Y(12));
  cairo_scale(g.cr, 3.8 * g.u, 8.5 * g.u);
  cairo_new_sub_path(g.cr);
  cairo_arc(g.cr, 0, 0, 1, 0, 2 * M_PI);
  cairo_restore(g.cr);
  g.move(3.5, 12);
  g.to(20.5, 12);
  g.stroke(dim(c), 1.4);
  g.move(4.5, 4.5);
  g.to(19.5, 19.5);
  g.stroke(c, 2.0);
}

void airplane(Painter &p, double x, double y, double size, const Color &c) {
  Grid g(p, x, y, size);
  g.move(12, 2.5);
  g.to(13.4, 4.2);
  g.to(13.4, 9.5);
  g.to(21, 14);
  g.to(21, 16);
  g.to(13.4, 13.6);
  g.to(13.4, 18.4);
  g.to(15.8, 20.2);
  g.to(15.8, 21.6);
  g.to(12, 20.5);
  g.to(8.2, 21.6);
  g.to(8.2, 20.2);
  g.to(10.6, 18.4);
  g.to(10.6, 13.6);
  g.to(3, 16);
  g.to(3, 14);
  g.to(10.6, 9.5);
  g.to(10.6, 4.2);
  cairo_close_path(g.cr);
  g.fill(c);
}

void speaker(Painter &p, double x, double y, double size, int level, bool muted, const Color &c) {
  Grid g(p, x, y, size);
  g.move(3.5, 9);
  g.to(7, 9);
  g.to(12, 4.5);
  g.to(12, 19.5);
  g.to(7, 15);
  g.to(3.5, 15);
  cairo_close_path(g.cr);
  g.stroke(c);
  if (muted) {
    g.move(15.5, 9.5);
    g.to(20.5, 14.5);
    g.move(20.5, 9.5);
    g.to(15.5, 14.5);
    g.stroke(c, 2.0);
    return;
  }
  for (int i = 1; i <= 3; ++i) {
    g.arc(12, 12, 2.2 + 3.2 * i, -M_PI * 0.27, M_PI * 0.27);
    g.stroke(level >= i ? c : dim(c));
  }
}

void battery(Painter &p, double x, double y, double size, int percent, bool charging,
             const Color &c, const Color &alert) {
  Grid g(p, x, y, size);
  percent = std::clamp(percent, 0, 100);
  bool low = percent <= 15 && !charging;
  g.rrect(2.5, 7, 17, 10, 2.6);
  g.stroke(c);
  g.rrect(20.2, 10, 2, 4, 0.8);
  g.fill(c);
  double w = 13.4 * percent / 100.0;
  if (w > 0.4) {
    g.rrect(4.3, 8.8, std::max(1.2, w), 6.4, 1.2);
    g.fill(low ? alert : c);
  }
  if (charging) {
    // Bolt cut out of the fill, outlined in the panel colour's opposite.
    g.move(12.4, 5.2);
    g.to(8.4, 12.6);
    g.to(11.4, 12.6);
    g.to(10.6, 18.8);
    g.to(14.8, 11.2);
    g.to(11.8, 11.2);
    cairo_close_path(g.cr);
    Color edge = c.r + c.g + c.b > 1.5 ? Color::hex(0x101010) : Color::hex(0xFFFFFF);
    p.set(c);
    cairo_fill_preserve(g.cr);
    g.stroke(edge, 1.1);
  }
}

void bluetooth(Painter &p, double x, double y, double size, bool powered, bool connected,
               const Color &c) {
  Grid g(p, x, y, size);
  Color col = powered ? c : dim(c);
  g.move(7, 7.5);
  g.to(16.5, 16);
  g.to(12, 20.5);
  g.to(12, 3.5);
  g.to(16.5, 8);
  g.to(7, 16.5);
  g.stroke(col);
  if (connected && powered) {
    g.dot(4.2, 12, 1.4, c);
    g.dot(19.8, 12, 1.4, c);
  }
  if (!powered) {
    g.move(4.5, 4.5);
    g.to(19.5, 19.5);
    g.stroke(c, 1.8);
  }
}

void layout(Painter &p, double x, double y, double size, const std::string &code,
            const Color &c) {
  Grid g(p, x, y, size);
  g.rrect(2.5, 5, 19, 14, 3);
  g.stroke(c, 1.5);
  std::string text = code.substr(0, std::min<size_t>(code.size(), 3));
  for (auto &ch : text) ch = char(toupper((unsigned char)ch));
  if (text.empty()) text = "?";
  cairo_select_font_face(g.cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(g.cr, (text.size() > 2 ? 7.4 : 9.6) * g.u);
  cairo_text_extents_t e;
  cairo_text_extents(g.cr, text.c_str(), &e);
  p.set(c);
  cairo_move_to(g.cr, g.X(12) - e.width / 2 - e.x_bearing, g.Y(12) - e.height / 2 - e.y_bearing);
  cairo_show_text(g.cr, text.c_str());
}

void drive(Painter &p, double x, double y, double size, bool card, const Color &c) {
  Grid g(p, x, y, size);
  if (card) {
    // Memory card with a cut corner and contacts.
    g.move(7, 3);
    g.to(17, 3);
    g.to(19, 5);
    g.to(19, 21);
    g.to(5, 21);
    g.to(5, 6.5);
    cairo_close_path(g.cr);
    g.stroke(c);
    for (double px : {9.0, 12.0, 15.0}) {
      g.move(px, 6);
      g.to(px, 9);
    }
    g.stroke(c, 1.5);
    return;
  }
  // USB flash drive: connector, body and a status light.
  g.rrect(9, 2.5, 6, 5.5, 1);
  g.stroke(c, 1.5);
  g.dot(10.9, 5, 0.75, c);
  g.dot(13.1, 5, 0.75, c);
  g.rrect(6.5, 8, 11, 13.5, 2.6);
  g.stroke(c);
  g.dot(12, 17.2, 1.4, c);
}

void media(Painter &p, double x, double y, double size, bool playing, const Color &c) {
  Grid g(p, x, y, size);
  if (playing) {
    g.rrect(6.5, 5, 3.6, 14, 1.2);
    g.fill(c);
    g.rrect(13.9, 5, 3.6, 14, 1.2);
    g.fill(c);
    return;
  }
  // Two beamed eighth notes.
  g.move(9.5, 17);
  g.to(9.5, 5.5);
  g.to(19, 3.5);
  g.to(19, 15);
  g.stroke(c);
  cairo_save(g.cr);
  for (auto pt : {std::make_pair(7.0, 17.2), std::make_pair(16.5, 15.2)}) {
    cairo_save(g.cr);
    cairo_translate(g.cr, g.X(pt.first), g.Y(pt.second));
    cairo_scale(g.cr, 3.0 * g.u, 2.3 * g.u);
    cairo_arc(g.cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore(g.cr);
    g.fill(c);
  }
  cairo_restore(g.cr);
}

void moon(Painter &p, double x, double y, double size, const Color &c) {
  Grid g(p, x, y, size);
  cairo_t *cr = g.cr;
  // A disc with a second disc cut out of it, drawn in a group so the cut
  // leaves the panel behind it visible.
  cairo_push_group(cr);
  cairo_arc(cr, g.X(11), g.Y(13), 8.2 * g.u, 0, 2 * M_PI);
  p.set(c);
  cairo_fill(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_arc(cr, g.X(16.8), g.Y(7.6), 7.6 * g.u, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_pop_group_to_source(cr);
  cairo_paint(cr);
}

void power(Painter &p, double x, double y, double size, const Color &c) {
  Grid g(p, x, y, size);
  g.arc(12, 13, 7.5, -M_PI * 0.32, M_PI * 1.32);
  g.stroke(c);
  g.move(12, 3.5);
  g.to(12, 12);
  g.stroke(c);
}

void overflow(Painter &p, double x, double y, double size, bool open, const Color &c) {
  Grid g(p, x, y, size);
  if (open) {
    g.move(7, 9.5);
    g.to(12, 14.5);
    g.to(17, 9.5);
  } else {
    g.move(7, 14.5);
    g.to(12, 9.5);
    g.to(17, 14.5);
  }
  g.stroke(c, 2.0);
}

void action_center(Painter &p, double x, double y, double size, bool dot, const Color &c,
                   const Color &dot_color) {
  Grid g(p, x, y, size);
  // Two switches: one on (knob right, filled), one off.
  g.rrect(3.5, 4.5, 15, 6.5, 3.25);
  g.stroke(c, 1.6);
  g.dot(15.25, 7.75, 1.9, c);
  g.rrect(3.5, 13, 15, 6.5, 3.25);
  g.stroke(c, 1.6);
  g.dot(6.75, 16.25, 1.9, c);
  if (dot) {
    g.dot(19.5, 4.5, 3.2, dot_color);
  }
}

}  // namespace tray_icon
}  // namespace sde
