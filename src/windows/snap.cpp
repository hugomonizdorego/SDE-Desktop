// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "snap.h"

#include <cstdlib>

namespace sde {
namespace snap {

Plan plan(Zone zone, bool maximized, Key key) {
  Plan p;
  auto to = [&](Zone z) {
    p.step = Step::SnapTo;
    p.zone = z;
    return p;
  };
  auto step = [&](Step s) {
    p.step = s;
    return p;
  };
  if (maximized) {
    switch (key) {
      case Key::Down: return step(Step::Unmaximize);
      case Key::Left: return to(Zone::Left);
      case Key::Right: return to(Zone::Right);
      case Key::Up: return step(Step::Nothing);
    }
  }
  switch (zone) {
    case Zone::Free:
      switch (key) {
        case Key::Left: return to(Zone::Left);
        case Key::Right: return to(Zone::Right);
        case Key::Up: return step(Step::Maximize);
        case Key::Down: return step(Step::Minimize);
      }
      break;
    case Zone::Left:
      switch (key) {
        case Key::Up: return to(Zone::TopLeft);
        case Key::Down: return to(Zone::BottomLeft);
        case Key::Right: return step(Step::Restore);
        case Key::Left: return step(Step::Nothing);
      }
      break;
    case Zone::Right:
      switch (key) {
        case Key::Up: return to(Zone::TopRight);
        case Key::Down: return to(Zone::BottomRight);
        case Key::Left: return step(Step::Restore);
        case Key::Right: return step(Step::Nothing);
      }
      break;
    case Zone::TopLeft:
      switch (key) {
        case Key::Up: return step(Step::Maximize);
        case Key::Down: return to(Zone::Left);
        case Key::Right: return to(Zone::TopRight);
        case Key::Left: return step(Step::Nothing);
      }
      break;
    case Zone::TopRight:
      switch (key) {
        case Key::Up: return step(Step::Maximize);
        case Key::Down: return to(Zone::Right);
        case Key::Left: return to(Zone::TopLeft);
        case Key::Right: return step(Step::Nothing);
      }
      break;
    case Zone::BottomLeft:
      switch (key) {
        case Key::Up: return to(Zone::Left);
        case Key::Down: return step(Step::Minimize);
        case Key::Right: return to(Zone::BottomRight);
        case Key::Left: return step(Step::Nothing);
      }
      break;
    case Zone::BottomRight:
      switch (key) {
        case Key::Up: return to(Zone::Right);
        case Key::Down: return step(Step::Minimize);
        case Key::Left: return to(Zone::BottomLeft);
        case Key::Right: return step(Step::Nothing);
      }
      break;
  }
  return p;
}

Rect zone_rect(Zone zone, const Rect &a) {
  int half_w = a.w / 2, half_h = a.h / 2;
  switch (zone) {
    case Zone::Left: return {a.x, a.y, half_w, a.h};
    case Zone::Right: return {a.x + half_w, a.y, a.w - half_w, a.h};
    case Zone::TopLeft: return {a.x, a.y, half_w, half_h};
    case Zone::TopRight: return {a.x + half_w, a.y, a.w - half_w, half_h};
    case Zone::BottomLeft: return {a.x, a.y + half_h, half_w, a.h - half_h};
    case Zone::BottomRight: return {a.x + half_w, a.y + half_h, a.w - half_w, a.h - half_h};
    case Zone::Free: break;
  }
  return a;
}

bool fills(const Rect &r, Zone zone, const Rect &area) {
  if (zone == Zone::Free) return false;
  Rect z = zone_rect(zone, area);
  return std::abs(r.x - z.x) <= 24 && std::abs(r.y - z.y) <= 24 && std::abs(r.w - z.w) <= 48 &&
         std::abs(r.h - z.h) <= 48;
}

}  // namespace snap
}  // namespace sde
