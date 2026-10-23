// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Line icons shared by Gallery and Photo, drawn with cairo on a 24-unit grid
// so they stay crisp at every UI scale and follow the theme's colours.
#pragma once

#include "paint.h"

namespace gallery {
namespace icon {
enum Kind {
  Photos, Heart, HeartFilled, Clock, Screenshot, Folder, Plus, Close, Search, Back, Prev, Next,
  ZoomIn, ZoomOut, Fit, Rotate, Play, Pause, Info, Trash, Edit, OpenWith, Wallpaper, Files, Copy,
  Picture, Broken, Check, Small, Large,
  // Added for storage media and Photo.
  Drive, Import, Eject, Menu, RotateLeft, FlipH, FlipV, Crop, Undo, Redo, Adjust, Filters, Save,
  Fullscreen, Unfullscreen, MapPin, Camera, Print, Resize, Gallery, Film,
  // Gallery's albums.
  Album
};
// Draws |kind| in a size x size box at (x, y).
void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c);
}  // namespace icon
}  // namespace gallery
