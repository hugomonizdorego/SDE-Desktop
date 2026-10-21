// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// How Gallery and Photo talk to each other on the X display (see album.h):
// a running Gallery owns the selection _SDE_GALLERY_S<screen>, and Photo
// appends events to the root window property _SDE_GALLERY_EVENTS, which
// Gallery reads and deletes. Needs an open FLTK display.
#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

#include "album.h"

namespace gallery {

// The atom Gallery watches for on the root window (PropertyNotify).
Atom events_atom();
// True when a Gallery is running on this display.
bool gallery_running();
// Makes |w| the running Gallery; false if another one already is.
bool claim_gallery(Window w);
// Appends events for the running Gallery (nothing happens without one).
void send_gallery_events(const std::vector<GalleryEvent> &events);
// Reads and clears the pending events (Gallery).
std::vector<GalleryEvent> take_gallery_events();

// Starts Photo for |path|, navigating |album| in |sort| order.
bool open_in_photo(const std::string &path, const Album &album, Sort sort);
// Starts Gallery (or asks the running one) to show and select |path|.
bool show_in_gallery(const std::string &path);

}  // namespace gallery
