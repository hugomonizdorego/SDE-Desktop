// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Liria's line icons, drawn with cairo on a 24 x 24 grid so they stay sharp
// at every interface scale and take the theme's colors: the navigation and
// command buttons, the section headings and small marks (the pin of a
// pinned folder, chevrons). Files, folders, drives and places use the icon
// theme's full-colour icons (IconLoader) instead.
#pragma once

#include <string>

#include "paint.h"

namespace sde {
namespace liria {

// |name|: back forward up refresh search close plus chevron-down
// chevron-right chevron-up more new cut copy paste rename share delete sort
// view select-all invert properties settings pin pin-filled home folder
// drive usb network server trash eject terminal extract compress install
// open info check grid details compact sidebar recent undo link warning
// globe computer minimize maximize restore window-close mail copy-path
// restore-item empty-trash connect hidden zoom-in star filter appimage.
// (x, y): the top left of a |size| square; |weight| the line width in grid units.
void glyph(Painter &p, const std::string &name, double x, double y, double size, const Color &c,
           double weight = 1.6);

}  // namespace liria
}  // namespace sde
