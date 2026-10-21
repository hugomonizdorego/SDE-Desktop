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
