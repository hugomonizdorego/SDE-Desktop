// SPDX-License-Identifier: GPL-3.0-or-later
// 0.2.0 Alpha 3.1: the Action Center's session buttons (src/panel/
// session_actions.h): the actions and their order, what logind's answers
// hide, the protection against accidental clicks, and the glyphs (drawn into
// an image: inside their box, visible, all different). With a file name as
// argument the glyphs are also written there as a PNG, to look at them.
#include <cairo.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/panel/session_actions.h"

using namespace session_actions;

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

namespace {

struct Ink {
  int pixels = 0;
  int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
  std::vector<unsigned char> alpha;
};

// Draws |kind| at |size| in the middle of a transparent image twice as big.
Ink render(Kind kind, int size) {
  int n = size * 2, off = size / 2;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, n, n);
  cairo_t *cr = cairo_create(s);
  draw(cr, kind, off, off, size, sde::Color::hex(0x000000));
  cairo_destroy(cr);
  cairo_surface_flush(s);
  Ink ink;
  unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      unsigned char a = data[y * stride + x * 4 + 3];
      ink.alpha.push_back(a);
      if (a < 40) continue;
      ++ink.pixels;
      ink.x0 = std::min(ink.x0, x - off), ink.y0 = std::min(ink.y0, y - off);
      ink.x1 = std::max(ink.x1, x - off), ink.y1 = std::max(ink.y1, y - off);
    }
  cairo_surface_destroy(s);
  return ink;
}

void preview(const char *path) {
  const int sizes[] = {16, 24, 32, 64};
  int w = 0;
  for (int z : sizes) w += z + 16;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * 2, 4 * 80);
  cairo_t *cr = cairo_create(s);
  for (int dark = 0; dark < 2; ++dark) {
    cairo_set_source_rgb(cr, dark ? 0.13 : 0.96, dark ? 0.14 : 0.96, dark ? 0.16 : 0.97);
    cairo_rectangle(cr, dark * w, 0, w, 4 * 80);
    cairo_fill(cr);
    for (int k = 0; k < 4; ++k) {
      int x = dark * w + 8;
      for (int z : sizes) {
        draw(cr, Kind(k), x, k * 80 + (80 - z) / 2.0, z,
             sde::Color::hex(dark ? 0xF2F4F8 : 0x1B1F27));
        x += z + 16;
      }
    }
  }
  cairo_destroy(cr);
  cairo_surface_write_to_png(s, path);
  cairo_surface_destroy(s);
}

}  // namespace

int main(int argc, char **argv) {
  // The actions, in the order of the row, and what they run.
  const auto &a = all();
  CHECK(a.size() == 4);
  CHECK(a[0].kind == Kind::SLEEP && !strcmp(a[0].id, "suspend") && !strcmp(a[0].name, "Sleep"));
  CHECK(a[1].kind == Kind::LOGOUT && !strcmp(a[1].id, "logout") && !strcmp(a[1].name, "Log Out"));
  CHECK(a[2].kind == Kind::RESTART && !strcmp(a[2].id, "reboot") && !strcmp(a[2].name, "Restart"));
  CHECK(a[3].kind == Kind::SHUTDOWN && !strcmp(a[3].id, "poweroff") &&
        !strcmp(a[3].name, "Shut Down"));
  CHECK(&of(Kind::RESTART) == &a[2]);
  // Sleep acts at once; the others ask for a second click.
  CHECK(!a[0].destructive() && a[1].destructive() && a[2].destructive() && a[3].destructive());
  CHECK(!strcmp(a[3].confirm, "Click again to shut down"));
  // Log Out is always possible; the others ask logind.
  CHECK(!a[1].logind && !strcmp(a[0].logind, "CanSuspend") && !strcmp(a[2].logind, "CanReboot") &&
        !strcmp(a[3].logind, "CanPowerOff"));

  // logind's answers: "na" (no suspend support) and "no" (polkit refuses)
  // hide a button; no answer keeps it.
  CHECK(available("yes"));
  CHECK(available("challenge"));
  CHECK(available(""));
  CHECK(!available("no"));
  CHECK(!available("na"));
  CHECK(Capabilities::get().signature() == "?,?,?,?,");
  CHECK(Capabilities::get().shown(Kind::SHUTDOWN));

  // Arming: Sleep runs at once.
  {
    Arming arm;
    CHECK(arm.press(0, 10.0) == Press::RUN);
    CHECK(!arm.active(10.0));
    // Shut Down: armed by the first press, run by the second.
    CHECK(arm.press(3, 20.0) == Press::ARM);
    CHECK(arm.active(20.1) && arm.armed == 3);
    CHECK(arm.left(20.0) > 0.99 && arm.left(22.0) > 0.45 && arm.left(22.0) < 0.55);
    // A double click (or a bouncing touchpad) does not confirm.
    CHECK(arm.press(3, 20.0 + kSettleSeconds / 2) == Press::IGNORE);
    CHECK(arm.active(20.3));
    CHECK(arm.press(3, 21.5) == Press::RUN);
    CHECK(!arm.active(21.5) && arm.armed == -1);
    // Too late: the button disarmed, the press arms it again.
    CHECK(arm.press(2, 30.0) == Press::ARM);
    CHECK(!arm.active(30.0 + kArmSeconds + 0.01));
    CHECK(arm.left(30.0 + kArmSeconds + 0.01) == 0);
    CHECK(arm.press(2, 30.0 + kArmSeconds + 0.5) == Press::ARM);
    // Another button: that one is armed instead, nothing runs.
    CHECK(arm.press(1, 36.0) == Press::ARM);
    CHECK(arm.armed == 1);
    CHECK(arm.press(3, 36.6) == Press::ARM);
    CHECK(arm.armed == 3);
    // Sleep while Shut Down is armed: sleeps, nothing stays armed.
    CHECK(arm.press(0, 37.0) == Press::RUN);
    CHECK(arm.armed == -1);
    // Out of range: nothing.
    CHECK(arm.press(4, 40.0) == Press::IGNORE && arm.press(-1, 40.0) == Press::IGNORE);
  }

  // The glyphs: visible, inside their square, at every size, and different.
  for (int size : {16, 24, 36, 48}) {
    std::vector<Ink> inks;
    for (int k = 0; k < 4; ++k) {
      Ink ink = render(Kind(k), size);
      CHECK(ink.pixels > size * size / 12);
      CHECK(ink.x0 >= -1 && ink.y0 >= -1 && ink.x1 <= size && ink.y1 <= size);
      // They fill most of their box (none is a dot in a corner).
      CHECK(ink.x1 - ink.x0 >= size * 0.6 && ink.y1 - ink.y0 >= size * 0.6);
      inks.push_back(ink);
    }
    for (int i = 0; i < 4; ++i)
      for (int j = i + 1; j < 4; ++j) {
        // A fifth of the smaller glyph's ink at least is not shared (Restart
        // and Shut Down are both rings, told apart by the head and the bar).
        int differ = 0;
        for (size_t p = 0; p < inks[size_t(i)].alpha.size(); ++p)
          if ((inks[size_t(i)].alpha[p] > 127) != (inks[size_t(j)].alpha[p] > 127)) ++differ;
        CHECK(differ * 5 > std::min(inks[size_t(i)].pixels, inks[size_t(j)].pixels));
      }
  }
  if (argc > 1) preview(argv[1]);
  if (failures) fprintf(stderr, "%d failure(s)\n", failures);
  else printf("session actions: all checks passed\n");
  return failures ? 1 : 0;
}
