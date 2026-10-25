// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the copy and move progress (0.2.0 Alpha 2): the shared
// model (src/common/transfer_model.h, also built into Liria) — speed, time
// remaining, pauses, the graph along the progress, the remaining time in
// words, the mark split into layers and the stripes' pulse — and the
// texts of the SDE-FLTK dialog (src/common/transfer_dialog.h). No display.
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "transfer_dialog.h"
#include "transfer_model.h"
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
  // The graph has a point for every 1 % reached, at the speed of the time.
  for (int i = 0; i <= 24; ++i) CHECK(t.graph[i] >= 0);
  CHECK(t.graph[26] < 0);
  CHECK(near(t.graph[20], 50e6, 1e6));
  CHECK(near(t.peak, 50e6, 1e6));
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
  CHECK_EQ(TransferDialog::format_remaining(-1), "Calculating…");
  CHECK_EQ(TransferDialog::format_remaining(2), "A few seconds");
  CHECK_EQ(TransferDialog::format_remaining(31), "About 35 seconds");
  CHECK_EQ(TransferDialog::format_remaining(61), "About 1 minute");
  CHECK_EQ(TransferDialog::format_remaining(150), "About 3 minutes");
  CHECK_EQ(TransferDialog::format_remaining(3600 + 22 * 60), "About 1 hour and 20 minutes");
  CHECK_EQ(TransferDialog::format_remaining(7200), "About 2 hours");
  CHECK_EQ(TransferDialog::format_speed(85.3e6), "85.3\u00a0MB/s");  // GLib keeps number and unit together
}

// The mark splits into what is under the stripes, each stripe and what is
// over them; the SDE mark of data/branding has all three stripes.
static void test_logo_layers() {
  const char *svg =
      "<svg viewBox=\"0 0 10 10\"><title>SDE</title>"
      "<path id=\"base\" d=\"M0 0\"/><path id=\"screen\" d=\"M1 1\"/>"
      "<path id=\"stripe-red\" fill=\"#F00\" d=\"M2 2\"/><path id=\"stripe-green\" fill=\"#0F0\" d=\"M3 3\"/>"
      "<path id=\"stripe-blue\" fill=\"#00F\" d=\"M4 4\"/><path id=\"ink\" d=\"M5 5\"/></svg>";
  char *under = sde_transfer_logo_layer_svg(svg, 0), *green = sde_transfer_logo_layer_svg(svg, 2),
       *over = sde_transfer_logo_layer_svg(svg, 4);
  CHECK(under && green && over);
  std::string u = under ? under : "", g = green ? green : "", o = over ? over : "";
  CHECK(u.find("id=\"screen\"") != std::string::npos && u.find("stripe") == std::string::npos);
  CHECK(g.find("stripe-green") != std::string::npos && g.find("stripe-red") == std::string::npos &&
        g.find("id=\"ink\"") == std::string::npos);
  CHECK(o.find("id=\"ink\"") != std::string::npos && o.find("stripe") == std::string::npos);
  CHECK(u.rfind("<svg viewBox=\"0 0 10 10\">", 0) == 0 && o.find("</svg>") != std::string::npos);
  free(under);
  free(green);
  free(over);
  CHECK(sde_transfer_logo_layer_svg("<svg><path id=\"x\"/></svg>", 0) == nullptr);
  std::string mark;
  if (read_file(std::string(SDE_TEST_SOURCE_DIR) + "/data/branding/sde-mark.svg", mark)) {
    for (int i = 0; i < 5; ++i) {
      char *layer = sde_transfer_logo_layer_svg(mark.c_str(), i);
      CHECK(layer != nullptr);
      free(layer);
    }
  }
  // The pulse crosses the mark from left to right; the stripes follow
  // each other.
  CHECK(sde_transfer_pulse_position(0, 0) < 0);
  CHECK(sde_transfer_pulse_position(SDE_TRANSFER_PULSE_PERIOD * 0.5, 0) > 0.3);
  CHECK(sde_transfer_pulse_position(SDE_TRANSFER_PULSE_PERIOD * 0.99, 0) > 1.0);
  CHECK(sde_transfer_pulse_position(SDE_TRANSFER_PULSE_PERIOD * 0.5, 2) <
        sde_transfer_pulse_position(SDE_TRANSFER_PULSE_PERIOD * 0.5, 0));
}

int main() {
  setlocale(LC_ALL, "C");
  test_steady_speed_and_remaining();
  test_calculating_at_first();
  test_speed_change_and_stall();
  test_pause();
  test_eta_words();
  test_logo_layers();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("sde-transfer tests passed\n");
  return 0;
}
