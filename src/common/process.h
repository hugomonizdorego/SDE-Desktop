// SPDX-License-Identifier: GPL-3.0-or-later
// Running helper programs (nmcli, pactl, xrandr ...) without blocking the
// FLTK event loop. No shell is involved; arguments are passed as-is.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sde {

using DoneFn = std::function<void(const std::string &output, int status)>;
using LineFn = std::function<void(const std::string &line)>;

// Starts |argv|, collects its standard output and calls |done| from the event
// loop when it exits. status is the exit code, or -1 if it could not start
// (|done| is still called). Returns false when the program is not installed.
bool run_async(const std::vector<std::string> &argv, DoneFn done);

// Starts a long-running program (e.g. "pactl subscribe") and calls |on_line|
// for every line it prints. Returns an id for stop_stream(), or 0 on failure.
int stream_lines(const std::vector<std::string> &argv, LineFn on_line,
                 std::function<void()> on_exit = nullptr);
void stop_stream(int id);

// Runs |argv| and waits at most |timeout_ms| for it. Returns its output;
// *status receives the exit code (-1 on failure or timeout).
std::string run_sync(const std::vector<std::string> &argv, int *status = nullptr,
                     int timeout_ms = 5000);

}  // namespace sde
