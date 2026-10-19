// SPDX-License-Identifier: GPL-3.0-or-later
#include <FL/Fl.H>
#include <cassert>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include "../src/panel/sni.h"
#include "../src/panel/indicator_policy.h"

template<class F> void wait_for(F predicate) {
  auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate() && std::chrono::steady_clock::now() < limit) Fl::wait(.01);
  assert(predicate());
}
int main(int argc, char **argv) {
  assert(argc == 2);
  SniHost host([] {});
  assert(host.start());
  auto start = [&](const char *id) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) { execl(argv[1], argv[1], "--alias", id, nullptr); _exit(127); }
    return pid;
  };
  auto stop = [](pid_t pid) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); };
  auto system = start("--id=sde-power-manager");
  wait_for([&] { return host.visible_items().size() == 1; });
  // Both alias registrations must resolve before checking cardinality.
  for (int i = 0; i < 50; ++i) Fl::wait(.01);
  assert(host.visible_items().size() == 1);
  auto app = start("--id=battery-notes");
  wait_for([&] { return host.visible_items().size() == 2; });
  host.set_native_roles(indicator::Battery);
  assert(host.visible_items().size() == 1);
  assert(host.visible_items()[0]->id == "battery-notes");
  host.set_native_roles(0);
  assert(host.visible_items().size() == 2);
  stop(system);
  wait_for([&] { return host.visible_items().size() == 1; });
  auto transient = start("--id=sde-power-manager");
  stop(transient);
  for (int i = 0; i < 50; ++i) Fl::wait(.01);
  assert(host.visible_items().size() == 1);
  stop(app);
  wait_for([&] { return host.visible_items().empty(); });
  std::cout << "SNI: aliases, native replacement, ordinary apps and disappearing owners passed.\n";
}
