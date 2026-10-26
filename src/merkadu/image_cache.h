// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Icons and screenshots that are not local files: https URLs (screenshots
// of the catalogs, Snap Store icons) are downloaded with curl, the icons of
// installed snaps come from snapd (/v2/icons/NAME/icon). They are kept in
// $XDG_CACHE_HOME/merkadu/images; at most four downloads run at once.
#pragma once

#include <cairo.h>

#include <functional>
#include <string>

namespace sde {
namespace merkadu {
namespace images {

// A local file for |ref| (an absolute path, an https URL or
// "snap-icon:NAME"); "" while it is being fetched or when it failed.
std::string path(const std::string &ref);
// True when |ref| could not be fetched or read (it is not tried again).
bool failed(const std::string &ref);
// Called (on the FLTK thread) whenever a download finished.
void on_ready(std::function<void()> fn);
// An image scaled to fit |w| x |h| (cached; nullptr while it is fetched or
// when it cannot be read). Do not free it.
cairo_surface_t *picture(const std::string &ref, int w, int h);
// The icon of an app: an icon name of the theme, a file, a URL or
// "snap-icon:NAME"; |fallback| (an icon name) while it is fetched or when
// it is missing. Do not free it.
cairo_surface_t *icon(const std::string &ref, int size, const std::string &fallback = "application-x-executable");
// An icon file, URL or "snap-icon:NAME" scaled into |size| x |size|
// (cached); nullptr while it is fetched or when it cannot be read.
cairo_surface_t *icon_file(const std::string &ref, int size);
// Forgets the decoded pictures (after a theme change or to save memory).
void clear();

}  // namespace images
}  // namespace merkadu
}  // namespace sde
