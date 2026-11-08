// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-screenshot's decisions without a display (unit tested): where a
// screenshot is saved and under which name, and the rectangle of a snip.
#pragma once

#include <ctime>
#include <string>

namespace sde {
namespace screenshot {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

// The rectangle between two corners of a drag, inside |bounds|.
Rect from_corners(int x0, int y0, int x1, int y1, const Rect &bounds);
Rect intersect(const Rect &a, const Rect &b);

// <Pictures>/Screenshots, the folder SDE saves screenshots in.
std::string folder(const std::string &pictures_dir);
// "Screenshot 2026-11-05 10-30-12.png"; when that file exists already
// (|exists|), "Screenshot 2026-11-05 10-30-12 (2).png" and so on.
std::string file_name(std::time_t when, bool (*exists)(const std::string &), const std::string &folder);

}  // namespace screenshot
}  // namespace sde
