// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Snapping with Win+arrow keys, as on Windows (docs/WINDOWS.md): the
// decisions, without a display (unit tested). sde-windows carries them out.
//
//   Win+Left / Win+Right  the left or right half; from the other half the
//                         window goes back to where it was before snapping
//   Win+Up                maximize; a snapped half becomes its top quarter,
//                         a bottom quarter its half
//   Win+Down              restore a maximized window, a top quarter becomes
//                         its half, a half its bottom quarter; otherwise
//                         minimize
//
// A snapped window remembers where it was in _SDE_SNAP on its window (zone,
// x, y, width, height). When it no longer has the zone's size (it was moved
// or resized by hand), it counts as not snapped.
#pragma once

namespace sde {
namespace snap {

enum class Zone { Free = 0, Left, Right, TopLeft, TopRight, BottomLeft, BottomRight };
enum class Key { Left, Right, Up, Down };

// What a key does to a window in |zone| (|maximized| windows have no zone).
enum class Step {
  Nothing,
  SnapTo,     // move into Plan::zone
  Restore,    // back to where it was before it was snapped
  Maximize,
  Unmaximize,
  Minimize,
};

struct Plan {
  Step step = Step::Nothing;
  Zone zone = Zone::Free;
};

Plan plan(Zone zone, bool maximized, Key key);

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

// The outer (frame) rectangle of |zone| in the work area |area|.
Rect zone_rect(Zone zone, const Rect &area);
// Whether a frame at |r| still fills |zone| of |area|. The tolerance allows
// for windows that resize in steps (terminals) and for the frame's border.
bool fills(const Rect &r, Zone zone, const Rect &area);

}  // namespace snap
}  // namespace sde
