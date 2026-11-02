// SPDX-License-Identifier: GPL-3.0-or-later
// Starting programs without a shell.
#pragma once

#include <string>
#include <vector>

#include "desktop_entry.h"

namespace sde {

// Starts |argv| fully detached (double fork, new session). Returns false if
// the program could not be found. |work_dir| may be empty. |pid|, when
// given, receives the started process's id (0 if unknown).
bool spawn_detached(const std::vector<std::string> &argv, const std::string &work_dir = "",
                    long *pid = nullptr);

// Starts a desktop entry, honoring Terminal=, Path= and Type=Link. Since
// 0.2.0 Alpha 3.1 the start is recorded for sde-watchdog and sde-session
// (launches.h).
bool launch_entry(const DesktopEntry &entry, const std::vector<std::string> &files = {});

// Opens a file, folder or URL with the user's preferred application.
bool open_uri(const std::string &uri);

// Starts the preferred terminal emulator ([applications] terminal in
// sde.conf, else x-terminal-emulator and the usual ones), optionally
// running |argv|.
bool open_terminal(const std::vector<std::string> &argv = {}, const std::string &work_dir = "");

// The argument a terminal wants before a command ("-e", "--", "-x" or ""):
// X-TerminalArgExec= of its desktop entry when present.
std::string terminal_exec_arg(const std::string &program, const DesktopEntry *entry = nullptr);

}  // namespace sde
