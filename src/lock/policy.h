// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Screen Lock: rules without I/O, unit tested in tests/test_lock.cpp.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sde {
namespace lock {

// Delays between password attempts. The first three wrong passwords cost
// one second each (on top of PAM's own delay); after that the wait doubles
// up to five minutes, so guessing on an unattended machine is slow while a
// typo costs little.
class AttemptLimiter {
 public:
  static double delay_after(int failures);
  void failure(double now);
  void success();
  // Seconds left before the next attempt is accepted (0 = now).
  double wait(double now) const;
  int failures() const { return failures_; }

 private:
  int failures_ = 0;
  double next_ = 0;
};

// org.freedesktop.ScreenSaver Inhibit/UnInhibit bookkeeping. Cookies are
// never 0 and not reused while held; an inhibitor belongs to the D-Bus
// connection that took it, which alone may release it, and it disappears
// with that connection.
struct Inhibitor {
  uint32_t cookie = 0;
  std::string owner;  // unique bus name, ":1.42"
  std::string application;
  std::string reason;
};

class InhibitorTracker {
 public:
  static constexpr size_t kMaxPerOwner = 32;

  // Returns the cookie, or 0 when |owner| already holds kMaxPerOwner.
  uint32_t add(const std::string &owner, const std::string &application, const std::string &reason);
  bool remove(uint32_t cookie, const std::string &owner);
  // Releases everything |owner| held; returns how many.
  size_t drop_owner(const std::string &owner);
  bool active() const { return !items_.empty(); }
  std::vector<Inhibitor> list() const;
  size_t size() const { return items_.size(); }

 private:
  std::map<uint32_t, Inhibitor> items_;
  uint32_t next_ = 1;
};

// Should the session lock now? |idle_lock| seconds (0 = never).
bool should_idle_lock(long idle_ms, int idle_lock, bool inhibited, bool locked);

// The lock screen while it cannot hold the keyboard and the pointer (another
// client has grabbed them: an open menu, a screenshot tool, a stuck popup).
// It keeps retrying the grabs all the time; after |waited| seconds it also
// asks the SDE panel to close its popups, then unmaps foreign popup menus
// (their grabs end with them) and finally shows the lock screen with a
// visible message instead of a black screen. It never gives up.
enum class InputStep { Retry = 0, ClosePopups = 1, ReleasePopups = 2, ShowWaiting = 3 };
InputStep input_step(double waited);

// A top-level window of another client, seen by the lock screen while it
// tries to get the grabs.
struct ForeignWindow {
  bool override_redirect = false;
  bool viewable = false;
  bool input_only = false;
  int w = 0, h = 0;            // size
  int screen_w = 0, screen_h = 0;  // root window size
  std::vector<std::string> types;  // _NET_WM_WINDOW_TYPE atom names
  std::string name;                // WM_NAME
};
// Is |w| a popup (menu, tooltip, panel popup) that may be unmapped to end
// the grab it holds? Never full-screen windows (desktop, compositor
// overlay), docks, desktops, notifications or SDE Screen Lock's own.
bool releasable_popup(const ForeignWindow &w);

// A monitor of the lock screen, in root window coordinates.
struct Monitor {
  int x = 0, y = 0, w = 0, h = 0;
  bool operator==(const Monitor &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};
// The monitor that contains (x, y), else the nearest one; 0 for none.
int monitor_at(const std::vector<Monitor> &monitors, int x, int y);

// The daemon's watchdog of the lock screen process: it reports a heartbeat
// every second; one that stays silent for |timeout| seconds is hung (a
// blocked X or input method connection) and is replaced by a fresh one.
constexpr double kUiHeartbeatTimeout = 12.0;
bool ui_unresponsive(double now, double last_heard, double timeout = kUiHeartbeatTimeout);

// Watchdog for the lock screen process: allows quick relaunches, but backs
// off when it keeps crashing (the screen stays black and grabbed meanwhile).
class CrashBackoff {
 public:
  // Records a crash at |now|; returns the seconds to wait before relaunching.
  double crashed(double now);
  void reset() { times_.clear(); }

 private:
  std::vector<double> times_;
};

}  // namespace lock
}  // namespace sde
