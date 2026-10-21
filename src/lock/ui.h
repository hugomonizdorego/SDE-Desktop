// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Screen Lock: the lock screen itself (run by the sde-lock daemon as
// "sde-lock --ui FD", a separate process the daemon relaunches if it dies).
//
// It covers every monitor with override-redirect windows, grabs the
// keyboard and the pointer (retrying for a few seconds) and only then
// draws: the blurred, dimmed wallpaper, the clock and date, the user's
// picture and name and the password field. Without the grabs it gives up
// and reports it; it never claims a lock it does not hold.
//
// Progress is reported on |report_fd|, one byte each:
//   M  windows mapped (the daemon may release its own fallback grab)
//   L  keyboard and pointer grabbed: the session is locked
//   F  the grabs failed: nothing is locked
//   U  the password was accepted (the process then exits with status 0)
#pragma once

#include <functional>
#include <string>

#include "auth.h"

namespace sde {
namespace lock {

struct UiOptions {
  std::string user;          // login name
  std::string display_name;  // full name, or the login name
  std::string avatar;        // image path, or empty
  bool switch_user = false;  // offer "Switch User" (LightDM's dm-tool)
  int blank_after = 30;      // seconds without input before the monitors turn off (0 = never)
  int report_fd = -1;
  // Checks a password. Runs on a worker thread.
  std::function<AuthResult(const char *password)> authenticate;
};

// Fills user, display_name, avatar and switch_user for the current user.
void default_options(UiOptions &o);

// Returns 0 after a successful unlock, 3 when the grabs failed.
int run_lock_ui(const UiOptions &options);

}  // namespace lock
}  // namespace sde
