// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_actions.h"

#include <cmath>
#include <memory>

#include "dbus.h"
#include "i18n.h"

using namespace sde;

namespace session_actions {

const std::vector<Action> &all() {
  static const std::vector<Action> list = {
      {Kind::SLEEP, "suspend", N_("Sleep"), nullptr, "CanSuspend"},
      {Kind::LOGOUT, "logout", N_("Log Out"), N_("Click again to log out"), nullptr},
      {Kind::RESTART, "reboot", N_("Restart"), N_("Click again to restart"), "CanReboot"},
      {Kind::SHUTDOWN, "poweroff", N_("Shut Down"), N_("Click again to shut down"), "CanPowerOff"},
  };
  return list;
}

const Action &of(Kind kind) { return all()[size_t(kind)]; }

bool available(const std::string &answer) { return answer != "no" && answer != "na"; }

double Arming::left(double now) const {
  if (!active(now)) return 0;
  return std::max(0.0, std::min(1.0, 1.0 - (now - since) / kArmSeconds));
}

Press Arming::press(int index, double now) {
  if (index < 0 || index >= int(all().size())) return Press::IGNORE;
  if (!all()[size_t(index)].destructive()) {
    armed = -1;
    return Press::RUN;
  }
  if (armed == index && active(now)) {
    if (now - since < kSettleSeconds) return Press::IGNORE;
    armed = -1;
    return Press::RUN;
  }
  armed = index;
  since = now;
  return Press::ARM;
}

// ------------------------------------------------------------------ glyphs

namespace {

// The 24-unit grid of the tray icons (src/common/tray_icons.cpp).
struct Grid {
  cairo_t *cr;
  double x, y, u;
  Grid(cairo_t *c, double x0, double y0, double size) : cr(c), x(x0), y(y0), u(size / 24.0) {}
  double X(double v) const { return x + v * u; }
  double Y(double v) const { return y + v * u; }
  void move(double a, double b) { cairo_move_to(cr, X(a), Y(b)); }
  void to(double a, double b) { cairo_line_to(cr, X(a), Y(b)); }
  void stroke(const Color &c, double width = 1.9) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_set_line_width(cr, width * u);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
  }
};

// Sleep: a crescent moon (the outline where a disc meets a second, cutting
// disc) with a small "z"; Do Not Disturb has the filled moon.
void sleep_glyph(Grid &g, const Color &c) {
  const double c1x = 10.5, c1y = 13.5, r1 = 8.0;  // the moon's disc
  const double c2x = 16.0, c2y = 8.0, r2 = 6.5;   // the disc cut out of it
  double dx = c2x - c1x, dy = c2y - c1y, d = std::sqrt(dx * dx + dy * dy);
  double a = (r1 * r1 - r2 * r2 + d * d) / (2 * d), h = std::sqrt(r1 * r1 - a * a);
  double ex = dx / d, ey = dy / d, mx = c1x + a * ex, my = c1y + a * ey;
  // The two tips, where the circles cross.
  double tip[2][2] = {{mx - h * ey, my + h * ex}, {mx + h * ey, my - h * ex}};
  auto norm = [](double v) {
    v = std::fmod(v, 2 * M_PI);
    return v < 0 ? v + 2 * M_PI : v;
  };
  // Whether the arc from |from| to |to| (cairo's direction) passes |at|.
  auto passes = [&](double from, double to, double at) { return norm(at - from) <= norm(to - from); };
  auto angle = [](double cx, double cy, const double *pt) { return std::atan2(pt[1] - cy, pt[0] - cx); };
  // The outer edge: round the moon's disc, away from the cutting disc.
  int first = 0, last = 1;
  if (passes(angle(c1x, c1y, tip[0]), angle(c1x, c1y, tip[1]), std::atan2(dy, dx)))
    std::swap(first, last);
  double s = angle(c1x, c1y, tip[first]), e = angle(c1x, c1y, tip[last]);
  cairo_new_sub_path(g.cr);
  cairo_arc(g.cr, g.X(c1x), g.Y(c1y), r1 * g.u, s, s + norm(e - s));
  // The inner edge: back along the cutting disc, on its side facing the moon.
  double u0 = angle(c2x, c2y, tip[last]), u1 = angle(c2x, c2y, tip[first]);
  if (passes(u0, u1, std::atan2(-dy, -dx)))
    cairo_arc(g.cr, g.X(c2x), g.Y(c2y), r2 * g.u, u0, u0 + norm(u1 - u0));
  else
    cairo_arc_negative(g.cr, g.X(c2x), g.Y(c2y), r2 * g.u, u0, u0 - norm(u0 - u1));
  cairo_close_path(g.cr);
  g.stroke(c);
  g.move(17.2, 2.8);
  g.to(21.4, 2.8);
  g.to(17.2, 7.2);
  g.to(21.4, 7.2);
  g.stroke(c, 1.6);
}

// Log Out: a door frame open to the right, an arrow leaving through it.
void logout_glyph(Grid &g, const Color &c) {
  g.move(11.5, 4.0);
  g.to(6.5, 4.0);
  cairo_arc_negative(g.cr, g.X(6.5), g.Y(6.0), 2.0 * g.u, -M_PI / 2, M_PI);
  g.to(4.5, 18.0);
  cairo_arc_negative(g.cr, g.X(6.5), g.Y(18.0), 2.0 * g.u, M_PI, M_PI / 2);
  g.to(11.5, 20.0);
  g.stroke(c);
  g.move(10.5, 12.0);
  g.to(20.5, 12.0);
  g.move(16.5, 8.0);
  g.to(20.5, 12.0);
  g.to(16.5, 16.0);
  g.stroke(c);
}

// Restart: a circular arrow, open at the top, its head turning clockwise.
void restart_glyph(Grid &g, const Color &c) {
  const double cx = 12.0, cy = 12.8, r = 7.6;
  double start = -M_PI / 2 + 0.55, end = 3 * M_PI / 2 - 0.35;
  cairo_new_sub_path(g.cr);
  cairo_arc(g.cr, g.X(cx), g.Y(cy), r * g.u, start, end);
  g.stroke(c);
  // The head at the end of the arc, along the direction of travel.
  double ex = cx + r * std::cos(end), ey = cy + r * std::sin(end);
  double tx = -std::sin(end), ty = std::cos(end);  // clockwise tangent
  double nx = -ty, ny = tx;                        // its normal
  const double back = 3.4, wing = 3.2;
  g.move(ex - tx * back + nx * wing, ey - ty * back + ny * wing);
  g.to(ex + tx * 0.5, ey + ty * 0.5);
  g.to(ex - tx * back - nx * wing, ey - ty * back - ny * wing);
  g.stroke(c);
}

// Shut Down: the power symbol (IEC 60417-5009).
void shutdown_glyph(Grid &g, const Color &c) {
  cairo_new_sub_path(g.cr);
  cairo_arc(g.cr, g.X(12), g.Y(13), 7.6 * g.u, -M_PI * 0.30, M_PI * 1.30);
  g.stroke(c);
  g.move(12, 3.2);
  g.to(12, 11.8);
  g.stroke(c);
}

}  // namespace

void draw(Painter &p, Kind kind, double x, double y, double size, const Color &c) {
  draw(p.cr(), kind, x, y, size, c);
}

void draw(cairo_t *cr, Kind kind, double x, double y, double size, const Color &c) {
  Grid g(cr, x, y, size);
  cairo_save(g.cr);
  switch (kind) {
    case Kind::SLEEP: sleep_glyph(g, c); break;
    case Kind::LOGOUT: logout_glyph(g, c); break;
    case Kind::RESTART: restart_glyph(g, c); break;
    case Kind::SHUTDOWN: shutdown_glyph(g, c); break;
  }
  cairo_restore(g.cr);
}

// ------------------------------------------------------------ capabilities

Capabilities &Capabilities::get() {
  static Capabilities caps;
  return caps;
}

void Capabilities::refresh(std::function<void()> changed) {
  dbus::Bus *bus = dbus::Bus::system();
  if (!bus) return;
  auto notify = std::make_shared<std::function<void()>>(std::move(changed));
  for (const Action &a : all()) {
    if (!a.logind) continue;
    int k = int(a.kind);
    bus->call_async(dbus::method("org.freedesktop.login1", "/org/freedesktop/login1",
                                 "org.freedesktop.login1.Manager", a.logind),
                    [this, k, notify](DBusMessage *reply) {
                      // No reply (logind not running): unknown, the button stays.
                      std::string value;
                      if (reply) {
                        auto args = dbus::read_args(reply);
                        if (!args.empty() && args[0].is_string()) value = args[0].str();
                      }
                      if (value == answers_[k]) return;
                      answers_[k] = value;
                      if (*notify) (*notify)();
                    },
                    3000);
  }
}

std::string Capabilities::signature() const {
  std::string s;
  for (const auto &a : answers_) s += (a.empty() ? "?" : a) + ",";
  return s;
}

}  // namespace session_actions
