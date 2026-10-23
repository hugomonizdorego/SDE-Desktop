// SPDX-License-Identifier: GPL-3.0-or-later
// Start menu and search timing (0.2 Alpha 1): how long an interaction takes
// from the event that started it to the frame that shows its result.
//
// Enabled by SDE_MENU_PERF_LOG=<file> (tools/menu-bench.py) or, for the UI
// tests, by SDE_PANEL_QA_DIR (written to <dir>/menu-perf.log). Each line is
//   <kind> <who> ms=<event to frame> draw=<flush> sync=<X server> [k=v ...]
// for an interaction ("open", "key", "wheel", "drag", "nav", "hover"), or
//   frame <who> t=<steady ms> draw=<flush> sync=<X server>
// for the frames that follow it (animations, smooth scrolling). Steps of an
// interaction (prepare, filter, ...) are added as k=v pairs. While enabled
// the windows wait for the X server after each frame (XSync), so the times
// include the server's work. Without the variables nothing is measured.
#pragma once

#include <string>

namespace menu_perf {

bool enabled();
double now_ms();
// An interaction starts now; its result is the next frame of |who| ("menu",
// "search"). A newer interaction replaces a pending one.
void begin(const char *kind, const char *who);
// Adds "name=value" to the pending interaction's line.
void note(const char *name, double value);
void note(const char *name, const std::string &value);
// |who| finished a frame: completes the pending interaction for it (once)
// or logs an animation frame.
void frame(const char *who, double draw_ms, double sync_ms);
// A free-form line (layout of the opened menu, counters).
void log(const std::string &line);

// Times a scope and adds it to the pending interaction as |name|.
class Step {
 public:
  explicit Step(const char *name) : name_(name), t0_(enabled() ? now_ms() : 0) {}
  ~Step() {
    if (t0_ > 0) note(name_, now_ms() - t0_);
  }
  Step(const Step &) = delete;
  Step &operator=(const Step &) = delete;

 private:
  const char *name_;
  double t0_;
};

}  // namespace menu_perf
