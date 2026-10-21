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
