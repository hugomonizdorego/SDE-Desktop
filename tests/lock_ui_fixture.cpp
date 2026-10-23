// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// QA fixture (never installed): the SDE lock screen with a fake
// authenticator that accepts only the password "sde-test", for the Xvfb
// test (test_lock_ui.py) and screenshots. sde-lock itself always uses PAM;
// nothing in the installed programs can select this authenticator.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include "i18n.h"
#include "../src/lock/rescue.h"
#include "../src/lock/ui.h"

#include <FL/Fl.H>
#include <FL/x.H>

int main(int argc, char **argv) {
  sde::init_i18n();
  sde::lock::UiOptions o;
  sde::lock::default_options(o);
  o.display_name = "Selviana Tester";
  o.avatar.clear();
  o.lookup_account = false;  // no AccountsService lookups in the tests
  // SDE_LOCK_FIXTURE_AVATAR=FILE: the account picture (screenshots).
  if (const char *picture = getenv("SDE_LOCK_FIXTURE_AVATAR")) o.avatar = picture;
  o.switch_user = argc > 2 && !strcmp(argv[2], "--switch-user");
  o.blank_after = 0;
  o.report_fd = argc > 1 ? atoi(argv[1]) : 1;
  o.authenticate = [](const char *password) {
    sde::lock::AuthResult r;
    usleep(150000);  // like a real PAM stack, not instantaneous
    r.status = strcmp(password, "sde-test") == 0 ? sde::lock::AuthResult::Accepted
                                                 : sde::lock::AuthResult::Rejected;
    return r;
  };
  // SDE_LOCK_FIXTURE_MONITORS=FILE: the monitors ("x y w h" per line) are
  // read from FILE whenever the lock screen looks at them, so the test can
  // simulate RandR changes on Xvfb (whose single output cannot change).
  if (const char *file = getenv("SDE_LOCK_FIXTURE_MONITORS")) {
    std::string path = file;
    o.monitors = [path] {
      std::vector<sde::lock::Monitor> out;
      std::ifstream in(path);
      sde::lock::Monitor m;
      while (in >> m.x >> m.y >> m.w >> m.h)
        if (m.w > 0 && m.h > 0) out.push_back(m);
      return out;
    };
  }
  if (argc > 2 && !strcmp(argv[2], "--rescue")) {
    // The daemon's rescue password prompt alone, grabbed like the daemon
    // does: "L" once the keyboard is held, "U" after the password.
    Fl::lock();
    setenv("XMODIFIERS", "@im=none", 1);
    fl_open_display();
    static bool done = false;
    sde::lock::RescueOptions r;
    r.user = o.user;
    r.authenticate = o.authenticate;
    r.unlocked = [] { done = true; };
    sde::lock::show_rescue_prompt(r);
    for (int i = 0; i < 100; ++i) {
      if (XGrabKeyboard(fl_display, sde::lock::rescue_prompt_window(), False, GrabModeAsync,
                        GrabModeAsync, CurrentTime) == GrabSuccess)
        break;
      Fl::wait(0.05);
    }
    if (write(o.report_fd, "L", 1) < 0) return 1;
    while (!done) Fl::wait(0.25);
    sde::lock::hide_rescue_prompt();
    return write(o.report_fd, "U", 1) == 1 ? 0 : 1;
  }
  return sde::lock::run_lock_ui(o);
}
