// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise real libdbus timeouts and owner disappearance in the FLTK loop.
#include <FL/Fl.H>
#include <cassert>
#include <chrono>
#include <iostream>
#include "dbus.h"

int main() {
  DBusError error;
  dbus_error_init(&error);
  DBusConnection *peer = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
  assert(peer);
  dbus_connection_set_exit_on_disconnect(peer, FALSE);
  assert(dbus_bus_request_name(peer, "org.debion.Test.NoReply", DBUS_NAME_FLAG_DO_NOT_QUEUE, &error) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
  auto *bus = sde::dbus::Bus::session();
  assert(bus);
  int completed = 0;
  for (int i = 0; i < 64; ++i) {
    auto *m = sde::dbus::method("org.debion.Test.NoReply", "/NoReply", "org.debion.Test.NoReply", "Wait");
    bus->call_async(m, [&](DBusMessage *reply) { assert(!reply); ++completed; }, 20 + i % 5);
  }
  auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (completed < 64 && std::chrono::steady_clock::now() < until) Fl::wait(.01);
  assert(completed == 64);
  // Replies that become impossible due to a disappearing provider also finish.
  for (int i = 0; i < 64; ++i) {
    auto *m = sde::dbus::method("org.debion.Test.NoReply", "/NoReply", "org.debion.Test.NoReply", "Wait");
    bus->call_async(m, [&](DBusMessage *reply) { assert(!reply); ++completed; }, 100);
  }
  dbus_connection_close(peer);
  dbus_connection_unref(peer);
  until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (completed < 128 && std::chrono::steady_clock::now() < until) Fl::wait(.01);
  assert(completed == 128);
  for (int i = 0; i < 20; ++i) Fl::wait(.005);
  std::cout << "D-Bus: 128 timeouts/provider disconnect callbacks completed safely.\n";
}
