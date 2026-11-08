// SPDX-License-Identifier: GPL-3.0-or-later
// sde-session's line to the SDE core controller (0.2.0 Alpha 4,
// docs/CORE.md): the session starts and supervises sde-core and tells it
// what happens to the programs it runs (components started, stopped,
// crashed or given up; applications that crashed) and its phase. The
// session's poll loop drives the connection (no FLTK here). Nothing is
// sent while sde-core does not run; when it starts (again) it gets the
// current state.
#pragma once

#include <functional>
#include <string>

namespace core_link {

// Connects to the session bus when one is known (never a new one).
void open();
// The connection's file descriptor for poll(), or -1.
int fd();
// Reads and handles what arrived (call when fd() is readable).
void dispatch();
// What the session tells sde-core: see sde::core::report().
void report(const std::string &kind, long pid, const std::string &name, const std::string &id,
            const std::string &detail);
// "starting", "running" or "ending".
void phase(const char *phase);
// Called when sde-core appears, to send the state again (the components).
void on_core_started(std::function<void()> fn);

}  // namespace core_link
