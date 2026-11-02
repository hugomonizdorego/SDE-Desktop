// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's stall detector and its reports (0.2 Alpha 3.1: a window that
// stayed blank and could not be closed on a real Debian system).
//
// The FLTK thread beats every 100 ms; a watchdog thread notices when the
// beats stop. A stall of more than 250 ms is logged with what the FLTK
// thread was doing (the innermost Busy scope), a stall of 10 s writes a hang
// report with the FLTK thread's stack, and a crash writes a crash report
// with its backtrace (sde::install_crash_handler). The next start offers
// the report on the Home page. Everything goes to the session log and to
// $XDG_STATE_HOME/merkadu/merkadu.log.
#pragma once

#include <string>

namespace sde {
namespace merkadu {
namespace watchdog {

// Starts the heartbeat and the watchdog thread, opens the log and installs
// the crash report. Call once, on the FLTK thread, after Fl::lock().
void start(const char *program = "merkadu");
// Stops watching (before the program exits).
void stop();

// What the FLTK thread is doing, for the stall log and the reports. |what|
// must be a string literal; |detail| a number shown with it (apps,
// packages). Scopes nest; the innermost is reported.
class Busy {
 public:
  explicit Busy(const char *what, long detail = -1);
  ~Busy();
  Busy(const Busy &) = delete;
  Busy &operator=(const Busy &) = delete;

 private:
  const char *prev_what_;
  long prev_detail_;
  long long prev_since_;
  long long since_;
};

// What the FLTK thread does now ("" when idle).
std::string current();

// A line for the session log and merkadu.log ("merkadu: " is added).
void log(const std::string &line);

// The directory of the log and the reports ($XDG_STATE_HOME/merkadu).
std::string state_dir();
// The report the previous run left (a crash or a hang), "" when there is
// none or it was dismissed. |when| receives its time.
std::string last_report(long long *when = nullptr);
// The report has been seen: it is not offered again.
void dismiss_report();

// The longest stall since start() in milliseconds, and how many stalls
// over 250 ms there were (tests, the QA dump).
long long longest_stall_ms();
int stalls();

}  // namespace watchdog
}  // namespace merkadu
}  // namespace sde
