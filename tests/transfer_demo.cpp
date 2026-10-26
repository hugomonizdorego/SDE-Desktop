// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper (not installed): the SDE transfer dialog (src/common/
// transfer_dialog.h) with a simulated copy of 1,234 items (4.2 GB) whose
// speed rises, dips and recovers, for tests/test_transfer_dialog.py and
// tools/showcase.py.
//
//   sde-transfer-demo [--details] [--paused] [--move | --delete | --trash]
//                     [--dark] [--at FRACTION] [--queue] [--drive NAME]
//                     [--removable] [--fat] [--finishing | --done | --fail]
//                     [--qa FILE] [--exit-after SECONDS]
//
// --at replays the copy up to FRACTION (0-1) at once, then it goes on in
// real time (100 MB/s on average). --queue adds a second operation (a move
// of 12 items) under the first. --drive names the destination drive
// (--removable: one that may be unplugged, --fat: with FAT). --finishing
// shows "Writing the last data", --done the finished operation, --fail a
// failed one, all at once. --qa writes the dialog's state after every
// update: its texts, whether the picture moves, the buttons' and the
// picture's geometry and the window's size. Exit status: 0 done, 3
// cancelled.
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
  int id = -1, second = -1;
  double start = 0;    // real time when the copy began
  double replayed = 0; // seconds replayed at once (--at)
  double resumed = 0;  // real time when the live part began
  double done = 0;     // bytes
  double last = 0;
  bool paused = false, cancelled = false, finishing = false, ended = false;
  std::string qa, drive;
  bool removable = false, fat = false;
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

std::string rect(Fl_Widget *w) {
  if (!w) return "";
  return sde::format("%d,%d,%d,%d", w->x(), w->y(), w->w(), w->h());
}

void write_qa(Demo &d) {
  if (d.qa.empty()) return;
  sde::TransferDialog *g = d.dialog;
  int id = g->main_id();
  std::string text;
  text += "headline=" + g->headline(id) + "\n";
  text += "status=" + g->status(id) + "\n";
  text += "note=" + g->note(id) + "\n";
  text += "percent=" + g->percent_text(id) + "\n";
  text += "animating=" + std::to_string(g->animating() ? 1 : 0) + "\n";
  text += "details=" + std::to_string(g->details() ? 1 : 0) + "\n";
  text += "paused=" + std::to_string(g->paused(id) ? 1 : 0) + "\n";
  text += "count=" + std::to_string(g->count()) + "\n";
  text += "pause=" + rect(g->pause_button(id)) + "\n";
  text += "cancel=" + rect(g->cancel_button(id)) + "\n";
  text += "toggle=" + rect(g->details_button()) + "\n";
  if (d.second >= 0 && g->has(d.second)) {
    text += "queue_headline=" + g->headline(d.second) + "\n";
    text += "queue_pause=" + rect(g->pause_button(d.second)) + "\n";
  }
  int x, y, w, h;
  g->scene_box(x, y, w, h);
  text += sde::format("scene=%d,%d,%d,%d\n", x, y, w, h);
  text += sde::format("window=%d,%d\n", g->w(), g->h());
  sde::write_file_atomic(d.qa, text);
}

void report(Demo &d, double t) {
  sde::TransferState s;
  s.bytes_done = (long long)d.done;
  s.bytes_total = kTotal;
  s.files_done = long(kItems * d.done / double(kTotal));
  s.files_total = kItems;
  static const char *names[] = {"holiday-video.mov", "report-2026.pdf", "photos-archive.zip", "music.flac"};
  s.current = names[int(t / 3.0) % 4];
  s.drive = d.drive;
  s.removable = d.removable;
  s.fat = d.fat;
  s.finishing = d.finishing;
  d.dialog->update(d.id, s, d.start + t);
  if (d.second >= 0) {
    sde::TransferState q;
    q.bytes_total = 860LL * 1000 * 1000;
    q.bytes_done = std::min<long long>(q.bytes_total, (long long)(d.done / 6.0));
    q.files_total = 12;
    q.files_done = long(12 * double(q.bytes_done) / double(q.bytes_total));
    d.dialog->update(d.second, q, d.start + t);
  }
  write_qa(d);
}

void tick(void *data) {
  Demo &d = *static_cast<Demo *>(data);
  if (!d.dialog->has(d.id)) {
    // Closed (a finished operation's cross) or cancelled.
    Fl::delete_widget(d.dialog);
    return;
  }
  double t = d.replayed + (now() - d.resumed);
  if (!d.paused && !d.ended) d.done = std::min(double(kTotal), d.done + speed_at(t) * (t - d.last));
  d.last = t;
  if (!d.ended) report(d, t);
  else write_qa(d);
  if (d.exit_after >= 0 && now() - d.resumed > d.exit_after) {
    d.dialog->hide();
    return;
  }
  if (!d.ended && d.done >= kTotal) {
    d.ended = true;
    d.dialog->finish(d.id, sde::TransferDialog::Result::Done);
  }
  Fl::repeat_timeout(0.1, tick, data);
}

}  // namespace

int main(int argc, char **argv) {
  sde::init_i18n();
  Demo d;
  bool details = false, paused = false, dark = false, queue = false, done = false, fail = false;
  sde::TransferDialog::Operation op = sde::TransferDialog::Operation::Copy;
  double at = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--details") details = true;
    else if (a == "--paused") paused = true;
    else if (a == "--move") op = sde::TransferDialog::Operation::Move;
    else if (a == "--delete") op = sde::TransferDialog::Operation::Delete;
    else if (a == "--trash") op = sde::TransferDialog::Operation::Trash;
    else if (a == "--dark") dark = true;
    else if (a == "--queue") queue = true;
    else if (a == "--finishing") d.finishing = true;
    else if (a == "--done") done = true;
    else if (a == "--fail") fail = true;
    else if (a == "--removable") d.removable = true;
    else if (a == "--fat") d.fat = true;
    else if (a == "--drive" && i + 1 < argc) d.drive = argv[++i];
    else if (a == "--at" && i + 1 < argc) at = atof(argv[++i]);
    else if (a == "--qa" && i + 1 < argc) d.qa = argv[++i];
    else if (a == "--exit-after" && i + 1 < argc) d.exit_after = atof(argv[++i]);
  }
  sde::Config cfg;
  cfg.load();
  sde::Theme theme = sde::Theme::from_config(cfg);
  if (dark) theme = sde::Theme::make(theme.family, true, theme.accent_name);
  theme.apply_to_fltk();
  d.dialog = sde::TransferDialog::instance();
  bool trash = op == sde::TransferDialog::Operation::Trash;
  d.id = d.dialog->add(op, "Videos", trash ? _("Trash") : "Backup Drive");
  if (queue) d.second = d.dialog->add(sde::TransferDialog::Operation::Move, "Downloads", "Documents");
  d.dialog->on_pause = [&d](int id, bool p) {
    if (id == d.id) d.paused = p;
  };
  d.dialog->on_cancel = [&d](int id) {
    if (id != d.id) {
      d.dialog->finish(id, sde::TransferDialog::Result::Cancelled);
      return;
    }
    d.cancelled = true;
    d.dialog->finish(id, sde::TransferDialog::Result::Cancelled);
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
    d.dialog->set_paused(d.id, true);
  }
  if (done || fail) {
    d.ended = true;
    if (done) d.done = kTotal;
    report(d, t);
    d.dialog->finish(d.id, done ? sde::TransferDialog::Result::Done : sde::TransferDialog::Result::Failed,
                     {"There is not enough space on Backup Drive.", "Cannot write \"report-2026.pdf\": Read-only file system",
                      "Cannot write \"music.flac\": Read-only file system"});
  }
  d.dialog->show();
  Fl::add_timeout(0.1, tick, &d);
  Fl::run();
  return d.cancelled ? 3 : 0;
}
