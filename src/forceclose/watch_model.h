// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The decisions of sde-watchdog (docs/FORCE-CLOSE.md), without a display:
// when to ping a window, when it is "Not Responding", when to show the
// "Wait / Force Close" dialog, and when a program SDE started has taken too
// long to show a window. Times are seconds on any steady clock. The program
// (watchdog_main.cpp) does the X work and unit tests drive the model
// directly (tests/test_force_close.cpp).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace sde {
class Config;
}

namespace watchdog {

struct Settings {
  bool enabled = true;
  double mark_after = 5;  // seconds without an answer before "Not Responding"
  double timeout = 120;   // ... before the dialog; and for a program to open
  double repeat = 2;      // a pending ping is sent again after this long
};
// [force_close] enabled (true), mark_after (5, at least 1), timeout (120,
// at least 2) in sde.conf.
Settings settings(const sde::Config &cfg);

struct Window {
  unsigned long id = 0;
  long pid = 0;             // its program, 0 when unknown
  double since = 0;         // when the oldest unanswered ping was sent; 0: none
  double last_ping = 0;
  bool marked = false;      // shown "Not Responding"
  bool closing = false;     // the user tried to close it
  bool dialog = false;      // the dialog is open
  double wait_until = 0;    // "Wait" chosen: no dialog before this
};

class Model {
 public:
  explicit Model(Settings s = Settings()) : s_(s) {}
  void configure(Settings s) { s_ = s; }
  const Settings &settings() const { return s_; }

  // The user uses |w| (it became active, a click). True: send a ping now.
  bool use(unsigned long w, long pid, double now);
  // The user tries to close |w|: as use(), and if it does not answer the
  // dialog comes as soon as it is marked, whatever "Wait" said.
  bool closing(unsigned long w, long pid, double now);
  // An answer from |w| (to any ping): it and every window of its program
  // respond. Returns the windows that were marked.
  std::vector<unsigned long> answered(unsigned long w);
  // |w| was destroyed or is no longer managed.
  void gone(unsigned long w);
  // The dialog of |w| closed; |waited|: the user chose Wait.
  void dialog_closed(unsigned long w, bool waited, double now);

  struct Due {
    std::vector<unsigned long> ping, mark, dialog;
  };
  // What is due at |now|; marks and dialogs are recorded as done.
  Due tick(double now);
  // Seconds from |now| until tick() has something to do; -1: nothing.
  double next(double now) const;

  const Window *find(unsigned long w) const;
  size_t size() const { return wins_.size(); }

 private:
  bool dialog_open_for(long pid) const;
  Settings s_;
  std::map<unsigned long, Window> wins_;
};

// Programs SDE started (launches.h), to notice one that runs for |timeout|
// without a window.
class Launches {
 public:
  struct Entry {
    long pid = 0;
    double due = 0;
    bool asked = false;  // the dialog was shown (or is)
  };
  // A launch that started |age| seconds ago (from its record).
  void add(long pid, double now, double age, double timeout);
  bool has(long pid) const { return list_.count(pid) != 0; }
  // Launches whose time is up and that were not asked about yet; the
  // caller checks the process and its windows, then calls asked() or forget().
  std::vector<long> due(double now) const;
  void asked(long pid);
  void forget(long pid);
  double next(double now) const;
  size_t size() const { return list_.size(); }

 private:
  std::map<long, Entry> list_;
};

}  // namespace watchdog
