// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "watch_model.h"

#include <algorithm>

#include "config.h"

namespace watchdog {

Settings settings(const sde::Config &cfg) {
  Settings s;
  s.enabled = cfg.get_bool("force_close", "enabled", true);
  s.mark_after = std::max(1, std::min(600, cfg.get_int("force_close", "mark_after", 5)));
  s.timeout = std::max(2, std::min(3600, cfg.get_int("force_close", "timeout", 120)));
  return s;
}

bool Model::use(unsigned long w, long pid, double now) {
  if (!w) return false;
  Window &x = wins_[w];
  x.id = w;
  if (pid > 0) x.pid = pid;
  if (x.since == 0) {
    x.since = now;
    x.last_ping = now;
    return true;
  }
  if (now - x.last_ping >= s_.repeat) {
    x.last_ping = now;
    return true;
  }
  return false;
}

bool Model::closing(unsigned long w, long pid, double now) {
  bool ping = use(w, pid, now);
  if (w) wins_[w].closing = true;
  return ping;
}

std::vector<unsigned long> Model::answered(unsigned long w) {
  std::vector<unsigned long> unmark;
  auto it = wins_.find(w);
  long pid = it != wins_.end() ? it->second.pid : 0;
  for (auto i = wins_.begin(); i != wins_.end();) {
    Window &x = i->second;
    if (x.id == w || (pid > 0 && x.pid == pid)) {
      if (x.marked) unmark.push_back(x.id);
      x.since = 0;
      x.marked = false;
      x.closing = false;
      x.wait_until = 0;  // it recovered: a new freeze starts afresh
    }
    // Windows that answered and have no dialog are forgotten.
    if (x.since == 0 && !x.dialog) i = wins_.erase(i);
    else ++i;
  }
  return unmark;
}

void Model::gone(unsigned long w) { wins_.erase(w); }

void Model::dialog_closed(unsigned long w, bool waited, double now) {
  auto it = wins_.find(w);
  if (it == wins_.end()) return;
  long pid = it->second.pid;
  it->second.dialog = false;
  // Wait holds for the program: none of its windows asks again before
  // another timeout.
  for (auto &kv : wins_) {
    Window &x = kv.second;
    if (x.id != w && (pid <= 0 || x.pid != pid)) continue;
    x.closing = false;
    if (waited) x.wait_until = now + s_.timeout;
  }
  if (it->second.since == 0) wins_.erase(it);
}

void Model::never_ask(unsigned long w) {
  auto it = wins_.find(w);
  if (it == wins_.end()) return;
  long pid = it->second.pid;
  for (auto &kv : wins_)
    if (kv.second.id == w || (pid > 0 && kv.second.pid == pid)) {
      kv.second.dialog = false;
      kv.second.closing = false;
      kv.second.wait_until = 1e300;
    }
}

bool Model::dialog_open_for(long pid) const {
  if (pid <= 0) return false;
  for (const auto &kv : wins_)
    if (kv.second.pid == pid && kv.second.dialog) return true;
  return false;
}

Model::Due Model::tick(double now) {
  Due due;
  if (!s_.enabled) return due;
  for (auto &kv : wins_) {
    Window &x = kv.second;
    if (x.since == 0) continue;
    if (now - x.last_ping >= s_.repeat) {
      x.last_ping = now;
      due.ping.push_back(x.id);
    }
    if (!x.marked && now - x.since >= s_.mark_after) {
      x.marked = true;
      due.mark.push_back(x.id);
    }
    // Not offered at all (never_ask()), one dialog per program.
    if (!x.marked || x.dialog || x.wait_until >= 1e299 || dialog_open_for(x.pid)) continue;
    bool long_enough = now - x.since >= s_.timeout && now >= x.wait_until;
    if (x.closing || long_enough) {
      x.dialog = true;
      due.dialog.push_back(x.id);
    }
  }
  return due;
}

double Model::next(double now) const {
  if (!s_.enabled) return -1;
  double best = -1;
  auto take = [&](double at) {
    double d = std::max(0.0, at - now);
    if (best < 0 || d < best) best = d;
  };
  for (const auto &kv : wins_) {
    const Window &x = kv.second;
    if (x.since == 0) continue;
    take(x.last_ping + s_.repeat);
    if (!x.marked) take(x.since + s_.mark_after);
    // The dialog's time, unless it is open (for this program) or refused:
    // tick() would do nothing then, and the timer must not spin.
    else if (!x.dialog && x.wait_until < 1e299 && !dialog_open_for(x.pid))
      take(x.closing ? now : std::max(x.since + s_.timeout, x.wait_until));
  }
  return best;
}

const Window *Model::find(unsigned long w) const {
  auto it = wins_.find(w);
  return it == wins_.end() ? nullptr : &it->second;
}

void Launches::add(long pid, double now, double age, double timeout) {
  if (pid <= 1 || list_.count(pid)) return;
  Entry e;
  e.pid = pid;
  e.due = now - std::max(0.0, age) + timeout;
  list_[pid] = e;
}

std::vector<long> Launches::due(double now) const {
  std::vector<long> out;
  for (const auto &kv : list_)
    if (!kv.second.asked && now >= kv.second.due) out.push_back(kv.first);
  return out;
}

void Launches::asked(long pid) {
  auto it = list_.find(pid);
  if (it != list_.end()) it->second.asked = true;
}

void Launches::forget(long pid) { list_.erase(pid); }

double Launches::next(double now) const {
  double best = -1;
  for (const auto &kv : list_) {
    if (kv.second.asked) continue;
    double d = std::max(0.0, kv.second.due - now);
    if (best < 0 || d < best) best = d;
  }
  return best;
}

}  // namespace watchdog
