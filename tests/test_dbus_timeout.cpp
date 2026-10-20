// SPDX-License-Identifier: GPL-3.0-or-later
// Regression test for the alpha12 panel crash: an async D-Bus call whose
// reply never arrives must time out cleanly (libdbus frees its timeout from
// inside the timeout handler). Needs a session bus; skipped (exit 77)
// without one. Run under dbus-run-session by CTest when it is installed.
#include <FL/Fl.H>

#include <cstdio>
#include <cstdlib>

#include "dbus.h"

using namespace sde;

int main() {
  auto *bus = dbus::Bus::session();
  if (!bus) return 77;
  // A second connection owns a name and never answers.
  DBusError err;
  dbus_error_init(&err);
  DBusConnection *mute = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (!mute) return 77;
  dbus_bus_request_name(mute, "org.sde.Test.Silent", 0, &err);
  static int replies = 0;
  for (int i = 0; i < 3; ++i) {
    auto *m = dbus::method("org.sde.Test.Silent", "/", "org.sde.Test", "Hang");
    bus->call_async(m, [](DBusMessage *) { ++replies; }, 200 + 100 * i);
  }
  Fl::add_timeout(2.5, [](void *) {
    printf("dbus timeouts: %d replies, no crash\n", replies);
    exit(replies == 3 ? 0 : 1);
  });
  for (;;) Fl::wait(1);
}
