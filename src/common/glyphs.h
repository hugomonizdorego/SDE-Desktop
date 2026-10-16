// SPDX-License-Identifier: GPL-3.0-or-later
// Vector status icons drawn with cairo, so that they stay sharp at every UI
// scale and follow the theme's text color whatever icon theme is installed.
// (x, y) is the top-left corner of a |size| x |size| box.
#pragma once

#include "paint.h"

namespace sde {
namespace glyph {

// strength 0-4 bars; off draws the "disconnected" variant.
void wifi(Painter &p, double x, double y, double size, int strength, const Color &c, bool off = false);
void ethernet(Painter &p, double x, double y, double size, const Color &c);
// level 0 (silent) to 3 (loud).
void speaker(Painter &p, double x, double y, double size, int level, bool muted, const Color &c);
void battery(Painter &p, double x, double y, double size, int percent, bool charging, const Color &c,
             const Color &level_color);
void sun(Painter &p, double x, double y, double size, const Color &c);
void bell(Painter &p, double x, double y, double size, bool off, const Color &c);
void gear(Painter &p, double x, double y, double size, const Color &c);
void lock(Painter &p, double x, double y, double size, const Color &c);
void power(Painter &p, double x, double y, double size, const Color &c);
void chevron(Painter &p, double x, double y, double size, bool right, const Color &c);
void check(Painter &p, double x, double y, double size, const Color &c);

}  // namespace glyph
}  // namespace sde
