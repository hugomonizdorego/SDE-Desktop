// SPDX-License-Identifier: GPL-3.0-or-later
// Album art of MPRIS players (mpris:artUrl) for the Action Center's media
// card: file:// pictures are read from disk, http(s) ones are downloaded
// with curl into ~/.cache/sde/art first. Loading happens from the event
// loop after the card asked for it (never while drawing), and the picture
// is kept scaled down to the card's size; a few recent ones are cached.
#pragma once

#include <cairo.h>

#include <functional>
#include <string>

namespace art {

// The picture of |url| scaled to |size| pixels, or nullptr while it loads
// or when it cannot be loaded. |ready| runs once a load started by this
// call finished.
cairo_surface_t *get(const std::string &url, int size, std::function<void()> ready);
// file:///home/a%20b/c.png -> /home/a b/c.png; "" for other schemes.
std::string local_path(const std::string &url);

}  // namespace art
