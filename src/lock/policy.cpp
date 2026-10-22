// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "policy.h"

#include <algorithm>
#include <cmath>

namespace sde {
namespace lock {

double AttemptLimiter::delay_after(int failures) {
  if (failures <= 0) return 0;
  if (failures <= 3) return 1;
  return std::min(300.0, std::pow(2.0, failures - 3));
}

void AttemptLimiter::failure(double now) {
  ++failures_;
  next_ = now + delay_after(failures_);
}

void AttemptLimiter::success() {
  failures_ = 0;
  next_ = 0;
}

double AttemptLimiter::wait(double now) const { return std::max(0.0, next_ - now); }

uint32_t InhibitorTracker::add(const std::string &owner, const std::string &application,
                               const std::string &reason) {
  size_t held = 0;
  for (const auto &it : items_)
    if (it.second.owner == owner) ++held;
  if (held >= kMaxPerOwner) return 0;
  // Find a free cookie (wraps around after 2^32 - 1 inhibitors).
  while (next_ == 0 || items_.count(next_)) ++next_;
  uint32_t cookie = next_++;
  Inhibitor in;
  in.cookie = cookie;
  in.owner = owner;
  in.application = application.substr(0, 128);
  in.reason = reason.substr(0, 256);
  items_[cookie] = in;
  return cookie;
}

bool InhibitorTracker::remove(uint32_t cookie, const std::string &owner) {
  auto it = items_.find(cookie);
  if (it == items_.end() || it->second.owner != owner) return false;
  items_.erase(it);
  return true;
}

size_t InhibitorTracker::drop_owner(const std::string &owner) {
  size_t n = 0;
  for (auto it = items_.begin(); it != items_.end();) {
    if (it->second.owner == owner) {
      it = items_.erase(it);
      ++n;
    } else {
      ++it;
    }
  }
  return n;
}

std::vector<Inhibitor> InhibitorTracker::list() const {
  std::vector<Inhibitor> out;
  for (const auto &it : items_) out.push_back(it.second);
  return out;
}

bool should_idle_lock(long idle_ms, int idle_lock, bool inhibited, bool locked) {
  if (locked || inhibited || idle_lock <= 0 || idle_ms < 0) return false;
  return idle_ms >= long(idle_lock) * 1000;
}

InputStep input_step(double waited) {
  if (waited >= 3.0) return InputStep::ShowWaiting;
  if (waited >= 2.0) return InputStep::ReleasePopups;
  if (waited >= 1.0) return InputStep::ClosePopups;
  return InputStep::Retry;
}

bool releasable_popup(const ForeignWindow &w) {
  if (!w.override_redirect || !w.viewable || w.input_only) return false;
  if (w.name == "SDE Screen Lock") return false;
  for (const std::string &t : w.types)
    if (t == "_NET_WM_WINDOW_TYPE_DESKTOP" || t == "_NET_WM_WINDOW_TYPE_DOCK" ||
        t == "_NET_WM_WINDOW_TYPE_NOTIFICATION" || t == "_NET_WM_WINDOW_TYPE_SPLASH")
      return false;
  // Menus and popups are smaller than the screen; a window that covers
  // (nearly) all of it is a desktop, a compositor overlay or a full-screen
  // program, not something holding a menu grab.
  if (w.w <= 0 || w.h <= 0) return false;
  if (w.screen_w > 0 && w.screen_h > 0 && double(w.w) * w.h >= 0.8 * double(w.screen_w) * w.screen_h)
    return false;
  return true;
}

int monitor_at(const std::vector<Monitor> &monitors, int x, int y) {
  int best = 0;
  long best_d = -1;
  for (size_t i = 0; i < monitors.size(); ++i) {
    const Monitor &m = monitors[i];
    long dx = x < m.x ? m.x - x : x >= m.x + m.w ? x - (m.x + m.w - 1) : 0;
    long dy = y < m.y ? m.y - y : y >= m.y + m.h ? y - (m.y + m.h - 1) : 0;
    long d = dx * dx + dy * dy;
    if (best_d < 0 || d < best_d) {
      best_d = d;
      best = int(i);
    }
  }
  return best;
}

bool ui_unresponsive(double now, double last_heard, double timeout) {
  return last_heard > 0 && now - last_heard > timeout;
}

double CrashBackoff::crashed(double now) {
  times_.push_back(now);
  // Only the last minute counts.
  times_.erase(std::remove_if(times_.begin(), times_.end(), [now](double t) { return now - t > 60; }),
               times_.end());
  size_t n = times_.size();
  if (n <= 3) return 0.1;
  return std::min(30.0, double(1u << std::min<size_t>(n - 3, 5)));
}

}  // namespace lock
}  // namespace sde
