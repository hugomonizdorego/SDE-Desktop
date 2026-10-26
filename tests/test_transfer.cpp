// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the copy and move progress (0.2.0 Alpha 2, 3): the shared
// model (src/common/transfer_model.h, also built into Liria) — speed, time
// remaining, pauses, the remaining time in words — the picture
// (src/common/transfer_scene.h: it moves only while it should, every state
// draws, a loop later it is the same) and the texts of the SDE-FLTK dialog
// (src/common/transfer_dialog.h). No display.
#include <clocale>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "transfer_dialog.h"
#include "transfer_model.h"
#include "transfer_scene.h"
#include "util.h"

using namespace sde;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                          \
  do {                                                                                          \
    std::string va = (a);                                                                       \
    std::string vb = (b);                                                                       \
    if (!(va == vb)) {                                                                          \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, #a, \
              #b, va.c_str());                                                                  \
      ++g_failures;                                                                             \
    }                                                                                           \
  } while (0)

static bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// A steady 50 MB/s copy of 1 GB in 10 Hz reports.
static void test_steady_speed_and_remaining() {
  SdeTransfer t;
  sde_transfer_init(&t, 100.0);
  CHECK(sde_transfer_progress(&t) < 0);  // nothing known yet
  CHECK(sde_transfer_remaining(&t) < 0);
  double total = 1e9;
  for (int i = 1; i <= 50; ++i) sde_transfer_update(&t, 100.0 + i * 0.1, i * 0.1 * 50e6, total, i, 200);
  // 5 s, 250 MB: a quarter.
  CHECK(near(sde_transfer_progress(&t), 0.25, 1e-9));
  CHECK(near(t.speed, 50e6, 0.5e6));
  // 750 MB left at 50 MB/s.
  CHECK(near(sde_transfer_remaining(&t), 15.0, 0.3));
}

// The first second is "Calculating...", and so is a copy that has not
// moved a byte.
static void test_calculating_at_first() {
  SdeTransfer t;
  sde_transfer_init(&t, 0);
  sde_transfer_update(&t, 0.5, 10e6, 1e9, 1, 10);
  CHECK(sde_transfer_remaining(&t) < 0);
  sde_transfer_init(&t, 0);
  sde_transfer_update(&t, 3.0, 0, 1e9, 0, 10);
  CHECK(sde_transfer_remaining(&t) < 0);
  // Without a byte total the items give the progress.
  sde_transfer_init(&t, 0);
  sde_transfer_update(&t, 1.0, 0, 0, 3, 12);
  CHECK(near(sde_transfer_progress(&t), 0.25, 1e-9));
}

// The current speed follows a change within a few seconds; the time left
// moves less (average and current speed weigh the same) and never jumps
// on a one-report stall.
static void test_speed_change_and_stall() {
  SdeTransfer t;
  sde_transfer_init(&t, 0);
  double done = 0;
  int i = 1;
  for (; i <= 100; ++i) {  // 10 s at 100 MB/s
    done += 10e6;
    sde_transfer_update(&t, i * 0.1, done, 10e9, i, 1000);
  }
  CHECK(near(t.speed, 100e6, 2e6));
  double before = sde_transfer_remaining(&t);  // (10 GB - 1 GB) / 100 MB/s = 90 s
  CHECK(near(before, 90.0, 2.0));
  for (; i <= 140; ++i) {  // 4 s at 20 MB/s
    done += 2e6;
    sde_transfer_update(&t, i * 0.1, done, 10e9, i, 1000);
  }
  CHECK(t.speed < 30e6);
  double after = sde_transfer_remaining(&t);
  CHECK(after > before);
  // A single report without progress does not halve the speed.
  double speed = t.speed;
  sde_transfer_update(&t, i * 0.1, done, 10e9, i, 1000);
  CHECK(t.speed > speed * 0.7);
}

// Paused time counts neither for the speed nor for the average.
static void test_pause() {
  SdeTransfer t;
  sde_transfer_init(&t, 0);
  double done = 0;
  for (int i = 1; i <= 30; ++i) {
    done += 5e6;
    sde_transfer_update(&t, i * 0.1, done, 1e9, i, 100);
  }
  double active = sde_transfer_active_time(&t);
  CHECK(near(active, 3.0, 1e-9));
  sde_transfer_set_paused(&t, 3.0, 1);
  sde_transfer_update(&t, 63.0, done, 1e9, 30, 100);  // a minute later, nothing moved
  CHECK(near(sde_transfer_active_time(&t), 3.0, 1e-9));
  sde_transfer_set_paused(&t, 63.0, 0);
  for (int i = 1; i <= 20; ++i) {
    done += 5e6;
    sde_transfer_update(&t, 63.0 + i * 0.1, done, 1e9, 30 + i, 100);
  }
  CHECK(near(sde_transfer_active_time(&t), 5.0, 1e-9));
  CHECK(near(t.speed, 50e6, 3e6));
  // 750 MB left at 50 MB/s: 15 s, not minutes.
  CHECK(near(sde_transfer_remaining(&t), 15.0, 1.0));
}

static void test_eta_words() {
  int v = 0, m = 0;
  CHECK(sde_transfer_eta_parts(-1, &v, &m) == SDE_TRANSFER_ETA_UNKNOWN);
  CHECK(sde_transfer_eta_parts(3, &v, &m) == SDE_TRANSFER_ETA_FEW_SECONDS);
  CHECK(sde_transfer_eta_parts(12, &v, &m) == SDE_TRANSFER_ETA_SECONDS && v == 15);
  CHECK(sde_transfer_eta_parts(50, &v, &m) == SDE_TRANSFER_ETA_SECONDS && v == 50);
  CHECK(sde_transfer_eta_parts(58, &v, &m) == SDE_TRANSFER_ETA_MINUTES && v == 1);
  CHECK(sde_transfer_eta_parts(150, &v, &m) == SDE_TRANSFER_ETA_MINUTES && v == 3);
  CHECK(sde_transfer_eta_parts(3570, &v, &m) == SDE_TRANSFER_ETA_HOURS && v == 1 && m == 0);
  CHECK(sde_transfer_eta_parts(3600 + 22 * 60, &v, &m) == SDE_TRANSFER_ETA_HOURS && v == 1 && m == 20);
  CHECK(sde_transfer_eta_parts(2 * 3600 + 58 * 60, &v, &m) == SDE_TRANSFER_ETA_HOURS && v == 3 && m == 0);
  CHECK_EQ(TransferDialog::format_left(-1), "Calculating…");
  CHECK_EQ(TransferDialog::format_left(2), "A few seconds left");
  CHECK_EQ(TransferDialog::format_left(31), "About 35 seconds left");
  CHECK_EQ(TransferDialog::format_left(61), "About 1 minute left");
  CHECK_EQ(TransferDialog::format_left(150), "About 3 minutes left");
  CHECK_EQ(TransferDialog::format_left(3600 + 22 * 60), "About 1 hour and 20 minutes left");
  CHECK_EQ(TransferDialog::format_left(7200), "About 2 hours left");
  CHECK_EQ(TransferDialog::format_speed(85.3e6), "85.3\u00a0MB/s");  // GLib keeps number and unit together
}

// Pixels of the picture (an image surface, no display).
static std::string render(const SdeScene &scene, int w = 330, int h = 100) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  sde_scene_draw(cr, 0, 0, w, h, &scene);
  cairo_destroy(cr);
  cairo_surface_flush(s);
  std::string pixels(reinterpret_cast<const char *>(cairo_image_surface_get_data(s)),
                     size_t(cairo_image_surface_get_stride(s)) * h);
  cairo_surface_destroy(s);
  return pixels;
}

// The picture: it moves while the operation runs (the penguin walks from
// one folder to the other), stands still when paused, with reduced motion
// and after the jump when done, and every state draws something.
static void test_scene() {
  SdeScene scene;
  memset(&scene, 0, sizeof scene);
  scene.kind = SDE_SCENE_COPY;
  scene.state = SDE_SCENE_RUNNING;
  scene.progress = 0.4;
  scene.animate = 1;
  CHECK(sde_scene_moving(&scene));
  double cycle = sde_scene_cycle();
  CHECK(cycle > 3.0 && cycle < 7.0);
  std::string a = render(scene);
  scene.seconds = cycle * 0.3;
  std::string b = render(scene);
  scene.seconds = cycle * 0.3 + cycle;  // a loop later: the same picture
  std::string c = render(scene);
  CHECK(a != b);
  CHECK(b == c);
  // Something is drawn in every state, light and dark, also still.
  const SdeSceneState states[] = {SDE_SCENE_RUNNING, SDE_SCENE_PAUSED, SDE_SCENE_QUEUED,
                                  SDE_SCENE_FINISHING, SDE_SCENE_DONE,  SDE_SCENE_ERROR};
  std::string empty = std::string(330 * 4 * 100, '\0');
  for (SdeSceneState st : states)
    for (int dark = 0; dark < 2; ++dark)
      for (int kind = 0; kind < 3; ++kind) {
        scene.state = st;
        scene.dark = dark;
        scene.kind = SdeSceneKind(kind);
        scene.animate = dark;  // still and moving
        scene.state_seconds = 0.3;
        CHECK(render(scene) != empty);
      }
  // Paused, queued, failed: still pictures; done: still after the jump.
  scene.kind = SDE_SCENE_COPY;
  scene.animate = 1;
  scene.state = SDE_SCENE_PAUSED;
  CHECK(!sde_scene_moving(&scene));
  scene.state = SDE_SCENE_ERROR;
  CHECK(!sde_scene_moving(&scene));
  scene.state = SDE_SCENE_DONE;
  scene.state_seconds = 0.5;
  CHECK(sde_scene_moving(&scene));
  scene.state_seconds = 3.0;
  CHECK(!sde_scene_moving(&scene));
  scene.state = SDE_SCENE_FINISHING;
  CHECK(sde_scene_moving(&scene));
  // Reduced motion: the same picture at any time.
  scene.state = SDE_SCENE_RUNNING;
  scene.animate = 0;
  CHECK(!sde_scene_moving(&scene));
  scene.seconds = 0.1;
  std::string still = render(scene);
  scene.seconds = 2.7;
  CHECK(render(scene) == still);
  // A move empties the source folder, a copy does not.
  scene.state = SDE_SCENE_DONE;
  scene.kind = SDE_SCENE_COPY;
  std::string copied = render(scene);
  scene.kind = SDE_SCENE_MOVE;
  CHECK(render(scene) != copied);
}

int main() {
  setlocale(LC_ALL, "C");
  test_steady_speed_and_remaining();
  test_calculating_at_first();
  test_speed_change_and_stall();
  test_pause();
  test_eta_words();
  test_scene();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("sde-transfer tests passed\n");
  return 0;
}
