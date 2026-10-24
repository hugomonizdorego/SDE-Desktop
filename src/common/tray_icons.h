// SPDX-License-Identifier: GPL-3.0-or-later
// SDE tray icons (alpha13): the status icons of the panel, drawn with cairo
// on a 24-unit grid with one stroke weight, so they stay sharp at every UI
// scale and show the real state of the device they stand for. (x, y) is the
// top-left corner of a |size| x |size| box; |c| is the foreground colour.
#pragma once

#include <string>

#include "paint.h"

namespace sde {
namespace tray_icon {

// Wi-Fi: |strength| 0-4 bars; !connected dims the fan and adds a cross.
void wifi(Painter &p, double x, double y, double size, int strength, bool connected,
          const Color &c);
// Wired network: an RJ45 plug; !connected adds a cross.
void wired(Painter &p, double x, double y, double size, bool connected, const Color &c);
// No network at all: a globe with a slash.
void offline(Painter &p, double x, double y, double size, const Color &c);
void airplane(Painter &p, double x, double y, double size, const Color &c);
// Speaker with 0-3 waves, or a cross when muted.
void speaker(Painter &p, double x, double y, double size, int level, bool muted, const Color &c);
// Battery with its level; charging adds a bolt; low draws the level in |alert|.
void battery(Painter &p, double x, double y, double size, int percent, bool charging,
             const Color &c, const Color &alert);
// The panel's battery (0.2 Alpha 2): |level| 0-100 (fractional while it
// eases to a new percentage) filled in |fill|, the body drawn in |outline|,
// and |mark| inside it in |c|: a bolt while charging, a plug on mains power
// (full, or not charging), "!" when critically low (in |fill|). |sweep|
// (0-1 through its period, < 0 none) animates charging: a lighter band
// fills the rest of the battery and fades. |pulse| (0-1, 1 steady) is the
// strength of the fill and the mark (the low and critical pulse).
enum class BatteryMark { NONE, BOLT, PLUG, ALERT };
void battery(Painter &p, double x, double y, double size, double level, BatteryMark mark,
             const Color &c, const Color &fill, const Color &outline, double sweep, double pulse);
// Bluetooth rune; !powered dims it with a slash; connected adds two dots.
void bluetooth(Painter &p, double x, double y, double size, bool powered, bool connected,
               const Color &c);
// Keyboard layout as text in a key cap ("US", "ID", ...).
void layout(Painter &p, double x, double y, double size, const std::string &code,
            const Color &c);
// USB flash drive / memory card plugged in.
void drive(Painter &p, double x, double y, double size, bool card, const Color &c);
// Music note, or pause bars when |playing| (click pauses).
void media(Painter &p, double x, double y, double size, bool playing, const Color &c);
// Do Not Disturb (a crescent moon).
void moon(Painter &p, double x, double y, double size, const Color &c);
void power(Painter &p, double x, double y, double size, const Color &c);
// "Show hidden icons" chevron.
void overflow(Painter &p, double x, double y, double size, bool open, const Color &c);
// Action Center: a bell over two switches; |dot| marks unread notifications.
void action_center(Painter &p, double x, double y, double size, bool dot, const Color &c,
                   const Color &dot_color);

}  // namespace tray_icon
}  // namespace sde
