// SPDX-License-Identifier: GPL-3.0-or-later
// The touch gesture recognizer of SDE-FLTK (sde-fltk/FL/Fl_Touch.H), fed
// with touch sequences as the X server sends them: taps and double taps,
// the slop, long presses, pans with axis lock and release velocity,
// momentum (its distance, its end, catching and boosting it), pinches,
// two-finger taps, three fingers, cancelling and the interface scale; and
// SDE's side (src/common/touch.h): the scroll helpers of widgets, the
// [touchscreen] settings and the display a touch screen drives.
#include <FL/Fl.H>
#include <FL/Fl_Touch.H>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "touch.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

using G = Fl_Touch_Gesture;

std::vector<G::Event> drain(G &g) {
  std::vector<G::Event> out;
  G::Event e;
  while (g.next(e)) out.push_back(e);
  return out;
}

std::string types(const std::vector<G::Event> &v) {
  static const char *names[] = {"NONE", "TAP", "LONG_PRESS", "LONG_PRESS_END", "PAN_BEGIN", "PAN",
                                "PAN_END", "MOMENTUM", "MOMENTUM_END", "PINCH_BEGIN", "PINCH",
                                "PINCH_END", "TWO_FINGER_TAP", "CANCEL"};
  std::string s;
  for (const auto &e : v) {
    if (!s.empty()) s += " ";
    s += names[e.type];
  }
  return s;
}

const G::Event *first(const std::vector<G::Event> &v, G::Type t) {
  for (const auto &e : v)
    if (e.type == t) return &e;
  return nullptr;
}

void test_tap() {
  G g;
  g.begin(1, 100, 100, 0);
  g.update(1, 103, 102, 40);  // within the slop
  CHECK(drain(g).empty());
  g.end(1, 104, 101, 80);
  auto v = drain(g);
  CHECK(types(v) == "TAP");
  CHECK(v.size() == 1 && v[0].x == 100 && v[0].y == 100 && v[0].taps == 1);
  CHECK(g.touches() == 0);
  CHECK(g.next_tick() < 0);
}

void test_multi_tap() {
  G g;
  auto tap = [&](double x, double y, double t) {
    g.begin(7, x, y, t);
    g.end(7, x, y, t + 60);
    auto v = drain(g);
    return v.size() == 1 && v[0].type == G::TAP ? v[0].taps : -1;
  };
  CHECK(tap(50, 50, 0) == 1);
  CHECK(tap(60, 55, 250) == 2);    // soon and close: a double tap
  CHECK(tap(55, 52, 500) == 3);    // a triple tap
  CHECK(tap(300, 300, 700) == 1);  // far away
  CHECK(tap(300, 300, 1500) == 1); // too late
}

void test_pan() {
  G g;
  g.begin(1, 100, 100, 0);
  g.update(1, 100, 105, 16);
  CHECK(drain(g).empty());
  g.update(1, 101, 130, 32);  // past the slop, mostly down
  auto v = drain(g);
  CHECK(types(v) == "PAN_BEGIN");
  CHECK(v[0].lock == 2);                // vertical
  CHECK(v[0].x0 == 100 && v[0].y0 == 100);
  CHECK(v[0].dx == 0);                  // locked
  NEAR(v[0].dy, 30 - 10, 0.5);          // the movement past the slop
  g.update(1, 104, 150, 48);  // sideways jitter stays out
  v = drain(g);
  CHECK(types(v) == "PAN");
  CHECK(v[0].dx == 0 && v[0].dy == 20);
  g.end(1, 104, 170, 64);
  v = drain(g);
  CHECK(types(v) == "PAN PAN_END");
  CHECK(v[1].vx == 0 && v[1].vy > 0.5);
  // A diagonal pan is not locked.
  g.begin(2, 0, 0, 1000);
  g.update(2, 20, 20, 1016);
  v = drain(g);
  CHECK(types(v) == "PAN_BEGIN" && v[0].lock == 0 && v[0].dx > 0 && v[0].dy > 0);
  g.end(2, 30, 30, 1032);
  drain(g);
  // Horizontal.
  g.begin(3, 0, 0, 2000);
  g.update(3, -40, 3, 2016);
  v = drain(g);
  CHECK(types(v) == "PAN_BEGIN" && v[0].lock == 1 && v[0].dy == 0 && v[0].dx < 0);
  g.end(3, -40, 3, 2032);
  drain(g);
}

void test_velocity() {
  G g;
  // 1 px/ms down for 200 ms.
  g.begin(1, 0, 0, 0);
  for (int t = 8; t <= 200; t += 8) g.update(1, 0, t, t);
  g.end(1, 0, 200, 200);
  auto v = drain(g);
  const G::Event *end = first(v, G::PAN_END);
  CHECK(end && std::fabs(end->vy - 1.0) < 0.05 && end->vx == 0);
  // A finger that stops before it lifts does not fling.
  g.begin(2, 0, 0, 1000);
  for (int t = 8; t <= 96; t += 8) g.update(2, 0, t * 2, 1000 + t);
  g.update(2, 0, 192, 1150);  // held still for 94 ms
  g.end(2, 0, 192, 1190);
  v = drain(g);
  end = first(v, G::PAN_END);
  CHECK(end && end->vy == 0);
  // Two events in the same millisecond do not divide by zero.
  g.begin(3, 0, 0, 2000);
  g.update(3, 0, 20, 2010);
  g.update(3, 0, 30, 2010);
  g.end(3, 0, 30, 2010);
  v = drain(g);
  end = first(v, G::PAN_END);
  CHECK(end && std::isfinite(end->vy));
}

void test_long_press() {
  G g;
  g.begin(1, 10, 20, 0);
  CHECK(g.next_tick() == 500);
  g.tick(499);
  CHECK(drain(g).empty());
  g.tick(500);
  auto v = drain(g);
  CHECK(types(v) == "LONG_PRESS" && v[0].x == 10 && v[0].y == 20);
  g.update(1, 12, 21, 600);  // still
  CHECK(drain(g).empty());
  g.end(1, 12, 21, 700);
  v = drain(g);
  CHECK(types(v) == "LONG_PRESS_END" && v[0].moved == 0);
  // Long press, then slide (to a menu item) and lift.
  g.begin(2, 10, 20, 1000);
  g.tick(1500);
  drain(g);
  g.update(2, 10, 60, 1600);
  g.update(2, 10, 80, 1616);
  v = drain(g);
  CHECK(types(v) == "PAN_BEGIN PAN");
  CHECK(v[0].lock == 0 && v[1].y == 80);
  g.end(2, 10, 80, 1700);
  v = drain(g);
  CHECK(types(v) == "LONG_PRESS_END" && v[0].moved == 1 && v[0].y == 80);
  // Moving first: no long press.
  g.begin(3, 0, 0, 3000);
  g.update(3, 0, 40, 3100);
  drain(g);
  g.tick(3600);
  CHECK(first(drain(g), G::LONG_PRESS) == nullptr);
  g.end(3, 0, 40, 3700);
  drain(g);
  // The delay is a parameter.
  g.params().long_press_ms = 300;
  g.begin(4, 0, 0, 5000);
  g.tick(5300);
  CHECK(types(drain(g)) == "LONG_PRESS");
  g.end(4, 0, 0, 5400);
  drain(g);
}

void test_momentum() {
  G g;
  CHECK(!g.fling(0, 0.1, 0));  // too slow
  CHECK(!g.flinging());
  CHECK(g.fling(0, 2, 0));
  CHECK(g.flinging());
  CHECK(g.next_tick() == 8);
  double total = 0, last_step = 1e9;
  bool shrinking = true, ended = false;
  int steps = 0;
  for (double t = 8; t < 10000 && !ended; t += 8) {
    g.tick(t);
    for (const auto &e : drain(g)) {
      if (e.type == G::MOMENTUM) {
        total += e.dy;
        CHECK(e.dx == 0);
        if (e.dy > last_step + 1e-9) shrinking = false;
        last_step = e.dy;
        ++steps;
      } else if (e.type == G::MOMENTUM_END) {
        ended = true;
      }
    }
  }
  CHECK(ended && !g.flinging());
  CHECK(shrinking);
  // v * friction minus what is left below the stop speed.
  NEAR(total, 2 * 325 - 0.02 * 325, 2.0);
  CHECK(steps > 50);
  // Velocity is capped.
  G h;
  CHECK(h.fling(30, 0, 0));
  double dist = 0;
  for (double t = 8; t < 20000 && h.flinging(); t += 8) {
    h.tick(t);
    for (const auto &e : drain(h))
      if (e.type == G::MOMENTUM) dist += e.dx;
  }
  NEAR(dist, 8 * 325 - 0.02 * 325, 3.0);
  // stop() ends it.
  G s;
  s.fling(1, 1, 0);
  s.tick(8);
  drain(s);
  s.stop(10);
  CHECK(types(drain(s)) == "MOMENTUM_END" && !s.flinging());
  CHECK(s.next_tick() < 0);
}

void test_catch_and_boost() {
  G g;
  CHECK(g.fling(0, 3, 0));
  g.tick(100);
  drain(g);
  // A finger on the moving content stops it and is not a tap.
  g.begin(1, 50, 50, 120);
  auto v = drain(g);
  CHECK(types(v) == "MOMENTUM_END");
  CHECK(!g.flinging());
  g.end(1, 50, 50, 180);
  v = drain(g);
  CHECK(types(v) == "CANCEL");
  // Catch again and flick the same way: faster than the flick alone.
  CHECK(g.fling(0, 3, 1000));
  g.tick(1100);
  drain(g);
  g.begin(2, 50, 50, 1120);
  g.update(2, 50, 70, 1130);
  g.update(2, 50, 90, 1140);
  v = drain(g);
  CHECK(first(v, G::PAN_BEGIN) && first(v, G::PAN_BEGIN)->caught == 1);
  g.end(2, 50, 100, 1145);
  v = drain(g);
  const G::Event *end = first(v, G::PAN_END);
  CHECK(end && end->vy > 0.5);
  CHECK(g.fling(end->vx, end->vy, 1145));
  double boosted = 0;
  for (double t = 1153; g.flinging() && t < 20000; t += 8) {
    g.tick(t);
    for (const auto &e : drain(g))
      if (e.type == G::MOMENTUM) boosted += e.dy;
  }
  CHECK(boosted > end->vy * 325 * 1.2);
  // A flick the other way is not boosted.
  G h;
  h.fling(0, 3, 0);
  h.tick(100);
  drain(h);
  h.begin(3, 50, 50, 120);
  drain(h);
  h.end(3, 50, 50, 130);
  drain(h);
  CHECK(h.fling(0, -1, 200));
  double back = 0;
  for (double t = 208; h.flinging() && t < 20000; t += 8) {
    h.tick(t);
    for (const auto &e : drain(h))
      if (e.type == G::MOMENTUM) back += e.dy;
  }
  NEAR(back, -1 * 325 + 0.02 * 325, 2.0);
}

void test_pinch() {
  G g;
  g.begin(1, 100, 100, 0);
  g.begin(2, 200, 100, 20);
  CHECK(drain(g).empty());
  CHECK(g.touches() == 2);
  CHECK(g.next_tick() < 0);  // no long press with two fingers
  g.update(2, 250, 100, 40);
  g.update(2, 300, 100, 60);
  auto v = drain(g);
  CHECK(types(v) == "PINCH_BEGIN PINCH");
  double scale = 1, cx = 0;
  for (const auto &e : v)
    if (e.type == G::PINCH) {
      scale *= e.scale;
      cx += e.dx;
    }
  NEAR(scale, 2.0, 1e-6);
  NEAR(cx, 50, 1e-6);
  NEAR(v.back().x, 200, 1e-6);
  // Pinch in.
  g.update(2, 150, 100, 80);
  v = drain(g);
  CHECK(types(v) == "PINCH");
  NEAR(v[0].scale, 0.25, 1e-6);
  // One finger lifts: the other pans on from where it is.
  g.end(2, 150, 100, 100);
  v = drain(g);
  CHECK(types(v) == "PINCH_END PAN_BEGIN");
  CHECK(v[1].dx == 0 && v[1].dy == 0 && v[1].x == 100 && v[1].x0 == 100);
  g.update(1, 100, 104, 116);  // no slop any more
  v = drain(g);
  CHECK(types(v) == "PAN" && v[0].dy == 4);
  g.end(1, 100, 104, 130);
  CHECK(first(drain(g), G::PAN_END) != nullptr);
  CHECK(g.touches() == 0);
}

void test_two_finger_tap() {
  G g;
  g.begin(1, 100, 100, 0);
  g.begin(2, 140, 100, 30);
  g.update(1, 102, 101, 60);
  g.end(1, 102, 101, 120);
  CHECK(drain(g).empty());
  g.end(2, 140, 100, 150);
  auto v = drain(g);
  CHECK(types(v) == "TWO_FINGER_TAP");
  NEAR(v[0].x, 120, 0.01);
  // Too slow: nothing.
  g.begin(3, 100, 100, 1000);
  g.begin(4, 140, 100, 1030);
  g.end(3, 100, 100, 1500);
  g.end(4, 140, 100, 1700);
  CHECK(types(drain(g)) == "CANCEL");
  // The finger left moves: it pans.
  g.begin(5, 100, 100, 3000);
  g.begin(6, 140, 100, 3010);
  g.end(5, 100, 100, 3050);
  g.update(6, 140, 140, 3070);
  auto w = drain(g);
  CHECK(types(w) == "PAN_BEGIN");
  g.end(6, 140, 150, 3090);
  CHECK(first(drain(g), G::PAN_END) != nullptr);
}

void test_pan_then_second_finger() {
  G g;
  g.begin(1, 0, 0, 0);
  g.update(1, 0, 40, 20);
  drain(g);
  g.begin(2, 100, 40, 40);
  auto v = drain(g);
  CHECK(types(v) == "PAN_END" && v[0].vx == 0 && v[0].vy == 0);  // no fling
  g.update(2, 140, 40, 60);
  v = drain(g);
  CHECK(types(v) == "PINCH_BEGIN PINCH");
  g.end(1, 0, 40, 80);
  g.end(2, 140, 40, 90);
  v = drain(g);
  CHECK(first(v, G::PINCH_END) && first(v, G::TWO_FINGER_TAP) == nullptr);
  CHECK(g.touches() == 0);
}

void test_three_fingers() {
  G g;
  g.begin(1, 0, 0, 0);
  g.begin(2, 50, 0, 10);
  g.begin(3, 100, 0, 20);
  CHECK(types(drain(g)) == "CANCEL");
  g.update(1, 0, 100, 40);
  g.end(1, 0, 100, 50);
  g.end(2, 50, 0, 60);
  CHECK(drain(g).empty());
  g.end(3, 100, 0, 70);
  CHECK(drain(g).empty());
  // Afterwards a tap is a tap again.
  g.begin(4, 10, 10, 1000);
  g.end(4, 10, 10, 1050);
  CHECK(types(drain(g)) == "TAP");
}

void test_cancel() {
  G g;
  g.begin(1, 0, 0, 0);
  g.update(1, 0, 50, 16);
  drain(g);
  g.cancel(30);
  CHECK(types(drain(g)) == "PAN_END");
  CHECK(g.touches() == 0);
  g.begin(1, 0, 0, 100);
  g.cancel(110);
  CHECK(types(drain(g)) == "CANCEL");
  g.begin(1, 0, 0, 200);
  g.tick(700);
  drain(g);
  g.cancel(710);
  auto v = drain(g);
  CHECK(types(v) == "LONG_PRESS_END" && v[0].moved == 1);
  // An end for a finger never seen is ignored; a repeated begin restarts.
  g.end(9, 0, 0, 800);
  CHECK(drain(g).empty());
  g.begin(5, 0, 0, 900);
  g.begin(5, 3, 3, 950);
  auto w = drain(g);
  CHECK(types(w) == "TAP");
  CHECK(g.touches() == 1);
  g.end(5, 3, 3, 1000);
  CHECK(types(drain(g)) == "TAP");
}

void test_scale() {
  G g;
  g.scale(2);
  g.begin(1, 0, 0, 0);
  g.update(1, 0, 15, 20);  // inside the slop at 200 %
  CHECK(drain(g).empty());
  g.update(1, 0, 25, 40);
  auto v = drain(g);
  CHECK(types(v) == "PAN_BEGIN");
  NEAR(v[0].dy, 5, 0.01);
  g.end(1, 0, 25, 60);
  drain(g);
  // The fling threshold and speeds grow too.
  CHECK(!g.fling(0, 0.4, 100));
  CHECK(g.fling(0, 0.6, 100));
}

void test_merging() {
  G g;
  g.begin(1, 0, 0, 0);
  g.update(1, 0, 20, 8);
  drain(g);
  // Nobody reads for a while: the movements arrive as one.
  for (int i = 1; i <= 40; ++i) g.update(1, 0, 20 + i * 3, 8 + i * 4);
  auto v = drain(g);
  CHECK(types(v) == "PAN");
  CHECK(v[0].dy == 120);
}

void test_sde_helpers() {
  using namespace sde;
  // Scroll positions move by the finger within their limits, and say
  // whether the content could move that way.
  int pos = 0;
  Fl::e_dy = 30;
  CHECK(touch_scroll_y(pos, 100) == 1 && pos == 30);
  Fl::e_dy = 100;
  CHECK(touch_scroll_y(pos, 100) == 1 && pos == 100);  // reached the end now
  Fl::e_dy = 10;
  CHECK(touch_scroll_y(pos, 100) == 0 && pos == 100);  // at the end
  Fl::e_dy = -10;
  CHECK(touch_scroll_y(pos, 100) == 1 && pos == 90);
  pos = 0;
  Fl::e_dy = 5;
  CHECK(touch_scroll_y(pos, 0) == 0 && pos == 0);  // the content fits
  Fl::e_dy = -5;
  CHECK(touch_scroll_y(pos, 50) == 0);  // at the top
  double dpos = 0;
  Fl::e_dy = 7;
  CHECK(touch_scroll_y(dpos, 50.0) == 1 && dpos == 7);
  Fl::e_dx = -3;
  int hpos = 10;
  CHECK(touch_scroll_x(hpos, 40) == 1 && hpos == 7);
  // Rows: whole rows, the rest kept.
  int first = 0;
  Fl::e_dy = 15;
  CHECK(touch_scroll_rows(first, 10, 20) == 1 && first == 0);
  Fl::e_dy = 10;
  CHECK(touch_scroll_rows(first, 10, 20) == 1 && first == 1);
  Fl::e_dy = 200;
  CHECK(touch_scroll_rows(first, 10, 20) == 1 && first == 10);
  Fl::e_dy = 20;
  CHECK(touch_scroll_rows(first, 10, 20) == 0 && first == 10);
  Fl::e_dx = Fl::e_dy = 0;

  // Which display a touch screen drives.
  TouchOptions o;
  CHECK(touch_output(o, {"eDP-1"}).empty());  // one display: nothing to map
  CHECK(touch_output(o, {"HDMI-1", "eDP-1"}) == "eDP-1");
  CHECK(touch_output(o, {"DP-1", "LVDS1"}) == "LVDS1");
  CHECK(touch_output(o, {"HDMI-1", "DP-2"}).empty());  // no built-in panel: a touch monitor
  o.output = "HDMI-1";
  CHECK(touch_output(o, {"HDMI-1", "eDP-1"}) == "HDMI-1");
  o.output = "DP-9";
  CHECK(touch_output(o, {"HDMI-1", "eDP-1"}).empty());  // not connected now
  o.output = "all";
  CHECK(touch_output(o, {"HDMI-1", "eDP-1"}).empty());
  CHECK(builtin_output("eDP-1") && builtin_output("eDP1") && builtin_output("LVDS-1") &&
        builtin_output("DSI-1") && !builtin_output("HDMI-A-1") && !builtin_output("DP-1"));
  std::string query =
      "Screen 0: minimum 8 x 8, current 4800 x 1800, maximum 32767 x 32767\n"
      "eDP-1 connected primary 2880x1800+0+0 (normal left inverted right x axis y axis) 302mm x 189mm\n"
      "   2880x1800    120.00*+  60.00\n"
      "HDMI-1 connected 1920x1080+2880+0 (normal left inverted right x axis y axis) 527mm x 296mm\n"
      "   1920x1080     60.00*+\n"
      "DP-1 disconnected (normal left inverted right x axis y axis)\n";
  CHECK((parse_connected_outputs(query) == std::vector<std::string>{"eDP-1", "HDMI-1"}));

  // Settings round trip, defaults and limits.
  Config cfg;
  TouchOptions d = load_touch_options(cfg);
  CHECK(d.gestures && !d.show_touches && d.output == "auto" && d.keyboard == "auto" && d.long_press_ms == 500);
  TouchOptions t;
  t.gestures = false;
  t.show_touches = true;
  t.output = "HDMI-1";
  t.keyboard = "off";
  t.long_press_ms = 700;
  save_touch_options(cfg, t);
  TouchOptions u = load_touch_options(cfg);
  CHECK(!u.gestures && u.show_touches && u.output == "HDMI-1" && u.keyboard == "off" && u.long_press_ms == 700);
  cfg.set("touchscreen", "long_press", "50");
  cfg.set("touchscreen", "keyboard", "maybe");
  cfg.unset("touchscreen", "output");
  u = load_touch_options(cfg);
  CHECK(u.long_press_ms == 200);
  CHECK(u.keyboard == "auto");
  CHECK(u.output == "auto");
}

}  // namespace

int main() {
  test_tap();
  test_multi_tap();
  test_pan();
  test_velocity();
  test_long_press();
  test_momentum();
  test_catch_and_boost();
  test_pinch();
  test_two_finger_tap();
  test_pan_then_second_finger();
  test_three_fingers();
  test_cancel();
  test_scale();
  test_merging();
  test_sde_helpers();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("touch gestures: all checks passed\n");
  return 0;
}
