// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// GLib's default main context inside the FLTK event loop, so that GIO's
// asynchronous calls (mounting drives and network shares, the volume
// monitor's signals, GVfs's password questions) run in Liria's one thread:
// before FLTK waits (Fl::add_check), the context is prepared and its file
// descriptors and next timeout are handed to FLTK (Fl::add_fd,
// Fl::add_timeout); when one of them is ready the context is checked and
// dispatched. Nothing polls: an idle Liria wakes only for events.
#pragma once

namespace sde {
namespace liria {

void start_glib_loop();
void stop_glib_loop();
// Runs whatever GLib has ready now (also from within a nested Fl::wait()).
void dispatch_glib();

}  // namespace liria
}  // namespace sde
