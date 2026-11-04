// SPDX-License-Identifier: GPL-3.0-or-later
// Regression tests of the D-Bus layer (src/common/dbus.*), on a session bus:
//  1. the alpha12 panel crash: an async D-Bus call whose reply never
//     arrives must time out cleanly (libdbus frees its timeout from inside
//     the timeout handler);
//  2. the 0.2.0 Alpha 4 desktop freeze: a reply handler that runs a nested
//     event loop (the panel's tray menus) while more messages arrive must
//     not dispatch again from inside the loop (libdbus would wait for its
//     own dispatch lock forever); the messages are delivered afterwards.
// Skipped (exit 77) without a session bus. Run under dbus-run-session by
// CTest when it is installed.
#include <FL/Fl.H>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "dbus.h"

using namespace sde;

namespace {

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int replies = 0, signals_seen = 0, signals_in_loop = 0;
bool in_nested_loop = false, nested_done = false;
DBusConnection *peer = nullptr;
std::string our_name;

void send_signal(void *) {
  // A signal from another connection, addressed to us, while the reply
  // handler below runs its own event loop.
  DBusMessage *s = dbus_message_new_signal("/org/sde/Test", "org.sde.Test", "Ping");
  dbus_message_set_destination(s, our_name.c_str());
  dbus_connection_send(peer, s, nullptr);
  dbus_connection_flush(peer);
  dbus_message_unref(s);
}

void reentry_phase() {
  auto *bus = dbus::Bus::session();
  DBusError err;
  dbus_error_init(&err);
  peer = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (!peer) exit(77);
  our_name = bus->unique_name();
  bus->add_signal_handler(
      "type='signal',interface='org.sde.Test',member='Ping'",
      [](DBusMessage *m) { return dbus_message_is_signal(m, "org.sde.Test", "Ping"); },
      [](DBusMessage *) {
        ++signals_seen;
        if (in_nested_loop) ++signals_in_loop;
      });
  // Any reply will do: the bus daemon's GetId.
  auto *m = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetId");
  bus->call_async(m, [](DBusMessage *) {
    // What SniHost did with a menu: a blocking call (it brings libdbus's
    // dispatch status up to date, so the next message announces itself),
    // then an event loop inside the reply handler.
    auto *bus = dbus::Bus::session();
    auto *again = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetId");
    if (DBusMessage *r = bus->call(again, 1000)) dbus_message_unref(r);
    in_nested_loop = true;
    for (int i = 0; i < 3; ++i) Fl::add_timeout(0.1 + 0.15 * i, send_signal);
    double end = now() + 1.0;
    while (now() < end) Fl::wait(0.05);
    in_nested_loop = false;
    nested_done = true;
  });
  Fl::add_timeout(4.0, [](void *) {
    printf("dbus re-entry: nested loop %s, %d signals (%d inside the loop)\n",
           nested_done ? "returned" : "stuck", signals_seen, signals_in_loop);
    exit(nested_done && signals_seen == 3 && signals_in_loop == 0 ? 0 : 1);
  });
}

}  // namespace

int main() {
  auto *bus = dbus::Bus::session();
  if (!bus) return 77;
  // A deadlock must fail the test, not hang CTest.
  signal(SIGALRM, [](int) {
    static const char msg[] = "dbus re-entry: DEADLOCK (dispatch from inside a handler's event loop)\n";
    if (write(2, msg, sizeof msg - 1) < 0) {
    }
    _exit(1);
  });
  alarm(20);
  // A second connection owns a name and never answers.
  DBusError err;
  dbus_error_init(&err);
  DBusConnection *mute = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (!mute) return 77;
  dbus_bus_request_name(mute, "org.sde.Test.Silent", 0, &err);
  for (int i = 0; i < 3; ++i) {
    auto *m = dbus::method("org.sde.Test.Silent", "/", "org.sde.Test", "Hang");
    bus->call_async(m, [](DBusMessage *) { ++replies; }, 200 + 100 * i);
  }
  Fl::add_timeout(2.5, [](void *) {
    printf("dbus timeouts: %d replies, no crash\n", replies);
    if (replies != 3) exit(1);
    reentry_phase();
  });
  for (;;) Fl::wait(1);
}
