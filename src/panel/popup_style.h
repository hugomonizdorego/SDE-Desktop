// SPDX-License-Identifier: GPL-3.0-or-later
// Compact popup metrics shared by every panel popup (alpha51).
//
// One visual language for the flyouts (Sound, Network, Bluetooth, Power,
// Drives, Touchpad, Keyboard, Tray, Alarms), the Action Center and the
// taskbar previews, in the spirit of the Cinnamon applets: 8-12 px padding,
// 32 px rows, 13 px body text, 16 px icons, 320-400 px wide, soft rounded
// corners and plain text rows instead of big cards. Values are logical
// pixels at 100 %; wrap them in S() where they are used.
#pragma once

#include "theme.h"

namespace popup_style {

constexpr int kPad = 12;        // left/right padding of the popup body
constexpr int kGap = 8;         // between groups of rows
constexpr int kRow = 32;        // one text row (icon + label)
constexpr int kRowSub = 40;     // a row with a dim second line
constexpr int kIcon = 16;       // row icons
constexpr int kHeader = 40;     // the title bar of a flyout
constexpr int kFlyoutW = 340;   // status flyouts
constexpr int kBatteryW = 380;  // the battery (Power) flyout
constexpr int kCenterW = 360;   // the Action Center
constexpr int kCenterMaxH = 620;  // its height at most (it scrolls)
constexpr int kTile = 56;       // Action Center quick tile (button + caption)
constexpr int kSection = 24;    // dim section caption ("Output", "Devices")

// Corner radius of popups (already scaled). A local helper until the theme
// grows a dedicated popup radius token: the theme's card radius, kept in
// the soft 8-10 px range of the compact style.
inline int radius() {
  return sde::S(10);
}

}  // namespace popup_style
