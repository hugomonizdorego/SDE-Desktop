// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "screenshot_model.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace sde {
namespace screenshot {

Rect intersect(const Rect &a, const Rect &b) {
  int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
  int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
  if (x1 <= x0 || y1 <= y0) return {};
  return {x0, y0, x1 - x0, y1 - y0};
}

Rect from_corners(int x0, int y0, int x1, int y1, const Rect &bounds) {
  Rect r{std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1};
  return intersect(r, bounds);
}

std::string folder(const std::string &pictures_dir) {
  std::string base = pictures_dir;
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  return base + "/Screenshots";
}

std::string file_name(std::time_t when, bool (*exists)(const std::string &), const std::string &dir) {
  struct tm lt;
  localtime_r(&when, &lt);
  char stamp[64];
  strftime(stamp, sizeof stamp, "Screenshot %Y-%m-%d %H-%M-%S", &lt);
  std::string name = std::string(stamp) + ".png";
  for (int n = 2; exists && exists(dir + "/" + name) && n < 1000; ++n)
    name = std::string(stamp) + " (" + std::to_string(n) + ").png";
  return name;
}

}  // namespace screenshot
}  // namespace sde
