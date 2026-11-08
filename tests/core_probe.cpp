// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
// The client of the SDE core controller (src/common/core_client.h) as a
// command for tests/test_core.py (not installed):
//
//   sde-core-probe available            prints yes or no
//   sde-core-probe write SECTION KEY VALUE [appearance]
//                                       core::write() ("-" resets the key)
//   sde-core-probe watch SECONDS        prints "settings [section] key ..."
//                                       and "look" lines as changes come
//   sde-core-probe instance APP_ID ARG...
//                                       core::single_instance(): prints
//                                       "primary" and then "activated ARG..."
//                                       lines, or "handed-over" and ends
//   sde-core-probe report KIND PID NAME core::report()
#include <FL/Fl.H>
#include <FL/x.H>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core_client.h"

using namespace sde;

namespace {

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Runs the event loop for |seconds| (D-Bus replies, signals, X events).
void run_for(double seconds) {
  double end = now() + seconds;
  while (now() < end) Fl::wait(0.05);
}

}  // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 2) return 2;
  std::string mode = argv[1];
  if (getenv("DISPLAY")) fl_open_display();
  if (mode == "available") {
    printf("%s\n", core::wait_available(1000) ? "yes" : "no");
    return 0;
  }
  if (mode == "write" && argc >= 5) {
    core::wait_available(1000);
    bool reset = std::string(argv[4]) == "-";
    bool appearance = argc > 5 && std::string(argv[5]) == "appearance";
    core::write({{argv[2], argv[3], reset ? "" : argv[4], reset}},
                appearance ? core::Mode::APPEARANCE : core::Mode::NORMAL);
    run_for(0.8);  // the reply (or the fallback after an error)
    printf("written\n");
    return 0;
  }
  if (mode == "watch" && argc >= 3) {
    core::watch_settings([](const std::vector<core::Key> &keys) {
      std::string line = "settings";
      for (const auto &k : keys) line += " [" + k.first + "] " + k.second;
      if (keys.empty()) line += " *";
      printf("%s\n", line.c_str());
    });
    core::watch_look([] { printf("look\n"); });
    core::wait_available(1000);
    printf("watching %s\n", core::available() ? "sde-core" : "the root window");
    run_for(atof(argv[2]));
    return 0;
  }
  if (mode == "instance" && argc >= 3) {
    std::vector<std::string> args(argv + 3, argv + argc);
    bool primary = core::single_instance(argv[2], args, [](const std::vector<std::string> &got) {
      std::string line = "activated";
      for (const auto &a : got) line += " " + a;
      printf("%s\n", line.c_str());
    });
    if (!primary) {
      printf("handed-over\n");
      return 0;
    }
    printf("primary\n");
    run_for(30);
    return 0;
  }
  if (mode == "report" && argc >= 5) {
    core::wait_available(1000);
    core::report(argv[2], atol(argv[3]), argv[4], "", "");
    run_for(0.3);
    return 0;
  }
  return 2;
}
