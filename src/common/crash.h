// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace sde {
// On SIGSEGV, SIGBUS, SIGFPE, SIGILL or SIGABRT, writes "<program> crashed"
// with a backtrace to stderr (the session log under sde-session), then lets
// the signal end the process as usual so it is still reported as a crash.
void install_crash_handler(const char *program);
}
