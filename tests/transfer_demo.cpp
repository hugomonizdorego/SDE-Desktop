// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper (not installed): the SDE transfer dialog (src/common/
// transfer_dialog.h) with a simulated copy of 1,234 items (4.2 GB) whose
// speed rises, dips and recovers, for tests/test_transfer_dialog.py and
// tools/showcase.py.
//
//   sde-transfer-demo [--details] [--paused] [--move] [--dark]
//                     [--at FRACTION] [--qa FILE] [--exit-after SECONDS]
//
// --at replays the copy up to FRACTION (0-1) at once, then it goes on in
// real time (100 MB/s on average). --qa writes the dialog's state after
// every update: its texts, whether the mark is animated, the buttons'
// geometry and window size. Exit status: 0 done, 3 cancelled.
#include <FL/Fl.H>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "config.h"
#include "i18n.h"
#include "theme.h"
#include "transfer_dialog.h"
#include "util.h"

namespace {

const long long kTotal = 4200LL * 1000 * 1000;
const long kItems = 1234;

struct Demo {
  sde::TransferDialog *dialog = nullptr;
  double start = 0;    // real time when the copy began
  double replayed = 0; // seconds replayed at once (--at)
  double resumed = 0;  // real time when the live part began
  double done = 0;     // bytes
  double last = 0;
  bool paused = false, cancelled = false;
  std::string qa;
  double exit_after = -1;
};

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// MB/s at |t| seconds: a rise, a dip in the middle (small files), recovery.
double speed_at(double t) {
  double base = 100e6 + 22e6 * std::sin(t * 0.9) + 9e6 * std::sin(t * 3.1);
  if (t > 9 && t < 14) base *= 0.45;
  return std::max(5e6, std::min(base, std::max(5e6, t * 60e6)));
}

void report(Demo &d, double t) {
  sde::TransferState s;
  s.bytes_done = (long long)d.done;
  s.bytes_total = kTotal;
  s.files_done = long(kItems * d.done / double(kTotal));
  s.files_total = kItems;
  static const char *names[] = {"holiday-video.mov", "report-2026.pdf", "photos-archive.zip", "music.flac"};
  s.current = names[int(t / 3.0) % 4];
  d.dialog->update(s, d.start + t);
  if (d.qa.empty()) return;
  std::string text;
  text += "headline=" + d.dialog->headline() + "\n";
  text += "percent=" + d.dialog->percent_text() + "\n";
  text += "speed=" + d.dialog->speed_text() + "\n";
  text += "remaining=" + d.dialog->remaining_text() + "\n";
  text += "items=" + d.dialog->items_text() + "\n";
  text += "animating=" + std::to_string(d.dialog->animating() ? 1 : 0) + "\n";
  text += "details=" + std::to_string(d.dialog->details() ? 1 : 0) + "\n";
  text += "paused=" + std::to_string(d.dialog->paused() ? 1 : 0) + "\n";
  auto geom = [&](const char *name, Fl_Widget *w) {
    text += sde::format("%s=%d,%d,%d,%d\n", name, w->x(), w->y(), w->w(), w->h());
  };
  geom("pause", d.dialog->pause_button());
  geom("cancel", d.dialog->cancel_button());
  geom("toggle", d.dialog->details_button());
  text += sde::format("window=%d,%d\n", d.dialog->w(), d.dialog->h());
  sde::write_file_atomic(d.qa, text);
}

void tick(void *data) {
  Demo &d = *static_cast<Demo *>(data);
  double t = d.replayed + (now() - d.resumed);
  if (!d.paused) d.done = std::min(double(kTotal), d.done + speed_at(t) * (t - d.last));
  d.last = t;
  report(d, t);
  if (d.exit_after >= 0 && now() - d.resumed > d.exit_after) {
    d.dialog->hide();
    return;
  }
  if (d.done >= kTotal) {
    d.dialog->hide();
    return;
  }
  Fl::repeat_timeout(0.1, tick, data);
}

}  // namespace

int main(int argc, char **argv) {
  sde::init_i18n();
  Demo d;
  bool details = false, paused = false, move = false, dark = false;
  double at = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--details") details = true;
    else if (a == "--paused") paused = true;
    else if (a == "--move") move = true;
    else if (a == "--dark") dark = true;
    else if (a == "--at" && i + 1 < argc) at = atof(argv[++i]);
    else if (a == "--qa" && i + 1 < argc) d.qa = argv[++i];
    else if (a == "--exit-after" && i + 1 < argc) d.exit_after = atof(argv[++i]);
  }
  sde::Config cfg;
  cfg.load();
  sde::Theme theme = sde::Theme::from_config(cfg);
  if (dark) theme = sde::Theme::make(theme.family, true, theme.accent_name);
  theme.apply_to_fltk();
  d.dialog = new sde::TransferDialog(move ? sde::TransferDialog::Operation::Move
                                          : sde::TransferDialog::Operation::Copy,
                                     "Videos", "Backup Drive");
  d.dialog->on_pause = [&d](bool p) { d.paused = p; };
  d.dialog->on_cancel = [&d] {
    d.cancelled = true;
    d.dialog->hide();
  };
  d.dialog->set_details(details);
  d.start = now();
  // Replay up to --at in 0.1 s steps.
  double t = 0;
  while (at > 0 && d.done < kTotal * at) {
    t += 0.1;
    d.done = std::min(double(kTotal) * at, d.done + speed_at(t) * 0.1);
    d.last = t;
    report(d, t);
  }
  // The live part goes on from the replayed time.
  d.replayed = t;
  d.resumed = now();
  if (paused) {
    d.paused = true;
    d.dialog->set_paused(true);
  }
  d.dialog->show();
  Fl::add_timeout(0.1, tick, &d);
  Fl::run();
  return d.cancelled ? 3 : 0;
}
