// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace sde {
// On SIGSEGV, SIGBUS, SIGFPE, SIGILL or SIGABRT, writes "<program> crashed"
// with a backtrace to stderr (the session log under sde-session), then lets
// the signal end the process as usual so it is still reported as a crash.
void install_crash_handler(const char *program);
// Also writes the report to |path| (created or replaced, mode 0600), with
// the text |context| returns (what the program was doing; it must only read
// memory, it runs in the signal handler). The program offers the report at
// its next start.
void set_crash_report(const char *path, const char *(*context)() = nullptr);
}
