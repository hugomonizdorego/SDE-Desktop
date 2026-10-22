// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Screen Lock: the daemon's last-resort password prompt.
//
// When the lock screen process keeps failing (it crashes or hangs before
// it shows its password field), the daemon would otherwise hold the
// keyboard on a black curtain with nothing to type into: the user could only
// switch the computer off. The rescue prompt is a plain window drawn by the
// daemon itself (no wallpaper, no avatar, no theme) with a password field;
// it checks the password with the same PAM service. The daemon grabs the
// keyboard on it while it is shown.
#pragma once

#include <functional>
#include <string>

#include "auth.h"

namespace sde {
namespace lock {

struct RescueOptions {
  std::string user;
  // Checks a password. Runs on a worker thread (Fl::lock() must have been
  // called once by the process).
  std::function<AuthResult(const char *password)> authenticate;
  // Called on the main thread after a password was accepted.
  std::function<void()> unlocked;
};

// Shows (or raises) the prompt over the whole screen.
void show_rescue_prompt(const RescueOptions &options);
void hide_rescue_prompt();
// Its X window (a Window) while shown, else 0.
unsigned long rescue_prompt_window();

}  // namespace lock
}  // namespace sde
