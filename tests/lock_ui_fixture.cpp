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
#include "../src/lock/ui.h"

int main(int argc, char **argv) {
  sde::init_i18n();
  sde::lock::UiOptions o;
  sde::lock::default_options(o);
  o.display_name = "Selviana Tester";
  o.avatar.clear();
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
  return sde::lock::run_lock_ui(o);
}
