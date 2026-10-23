// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Screen Lock: the lock screen itself (run by the sde-lock daemon as
// "sde-lock --ui FD", a separate process the daemon relaunches if it dies).
//
// It covers every monitor with override-redirect windows and grabs the
// keyboard and the pointer. The grabs are retried all the time: when another
// client holds them (an open menu, a screenshot tool) it asks the panel to
// close its popups, unmaps foreign popup menus and, after a few seconds,
// shows the lock screen with a message saying so instead of a black screen
// that ignores the user. A grab that is lost later is taken again; monitor
// changes (RandR, resume) move the windows. No X input method is used.
//
// Progress is reported on |report_fd|, one byte each:
//   M  windows mapped (the daemon may release its own fallback grab)
//   L  keyboard and pointer grabbed: the session is locked
//   W  covered, but another client still holds a grab (still retrying)
//   H  heartbeat, every second (the daemon replaces a lock screen that
//      stops answering)
//   F  the windows could not be mapped: nothing is locked
//   U  the password was accepted (the process then exits with status 0)
//
// SIGUSR1 (sent by the daemon after a resume) re-checks the monitors, the
// stacking and the grabs at once.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "auth.h"
#include "policy.h"

namespace sde {
namespace lock {

struct UiOptions {
  std::string user;          // login name
  std::string display_name;  // full name, or the login name
  std::string avatar;        // image path, or empty (the initials are shown)
  // Look the real name and the picture up (AccountsService, see
  // src/common/avatar.h) once the screen is locked, and again when they
  // change (_SDE_AVATAR_SERIAL). default_options() turns it on.
  bool lookup_account = false;
  bool switch_user = false;  // offer "Switch User" (LightDM's dm-tool)
  int blank_after = 30;      // seconds without input before the monitors turn off (0 = never)
  int report_fd = -1;
  // QA only (the test fixture simulates monitor changes with it): the
  // monitors instead of the X server's. sde-lock never sets it.
  std::function<std::vector<Monitor>()> monitors;
  // Checks a password. Runs on a worker thread.
  std::function<AuthResult(const char *password)> authenticate;
};

// Fills user, display_name, avatar and switch_user for the current user
// (quickly: the GECOS name and ~/.face; AccountsService is asked later).
void default_options(UiOptions &o);

// Returns 0 after a successful unlock, 3 when the windows could not be mapped.
int run_lock_ui(const UiOptions &options);

}  // namespace lock
}  // namespace sde
