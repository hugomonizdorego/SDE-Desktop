// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// sde-lock: SDE Screen Lock.
//
//   sde-lock [--lock]    lock the screen now (starts the daemon when needed)
//   sde-lock --daemon    the session service (XDG autostart)
//   sde-lock --query     "locked" or "unlocked"
//   sde-lock --poke      simulate user activity (wake the monitors)
//   sde-lock --inhibitors  list the programs that keep the session awake
//
// Authentication uses the PAM service "sde-lock" (/etc/pam.d/sde-lock).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "build_config.h"
#include "crash.h"
#include "dbus.h"
#include "i18n.h"
#include "spawn.h"
#include "ui.h"

namespace sde {
namespace lock {
int run_daemon(bool lock_now);
}
}  // namespace sde

using namespace sde;

namespace {

const char *kName = "org.debion.SDE.ScreenLock";
const char *kPath = "/org/debion/SDE/ScreenLock";

void usage(FILE *out) {
  fprintf(out, "%s",
          _("Usage: sde-lock [--lock]\n"
            "       sde-lock --daemon | --query | --poke | --inhibitors | --version\n\n"
            "SDE Screen Lock. Without options the screen is locked now.\n"
            "Settings are in sde.conf [lock] (Control Center > Lock Screen).\n"));
}

DBusMessage *call(const char *member, int timeout_ms) {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return nullptr;
  DBusError err;
  dbus_error_init(&err);
  DBusMessage *m = dbus::method(kName, kPath, kName, member);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus->conn(), m, timeout_ms, &err);
  dbus_message_unref(m);
  if (!reply && dbus_error_is_set(&err)) {
    if (strcmp(err.name, DBUS_ERROR_SERVICE_UNKNOWN) && strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER))
      fprintf(stderr, "sde-lock: %s\n", err.message);
  }
  dbus_error_free(&err);
  return reply;
}

bool daemon_running() {
  dbus::Bus *bus = dbus::Bus::session();
  return bus && bus->name_has_owner(kName);
}

int lock_now() {
  if (!daemon_running()) {
    // Not started with the session (or it was stopped): start it now.
    if (!spawn_detached({"sde-lock", "--daemon"})) {
      fprintf(stderr, "sde-lock: %s\n", _("cannot start the screen lock service"));
      return 1;
    }
    for (int i = 0; i < 50 && !daemon_running(); ++i) usleep(100000);
  }
  DBusMessage *reply = call("Lock", 15000);
  if (!reply) {
    fprintf(stderr, "sde-lock: %s\n", _("the screen is NOT locked"));
    return 1;
  }
  dbus_message_unref(reply);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  std::string cmd = argc > 1 ? argv[1] : "--lock";
  if (cmd == "--ui" && argc > 2) {
    // The lock screen process, started by the daemon.
    install_crash_handler("sde-lock-ui");
    lock::UiOptions o;
    lock::default_options(o);
    o.report_fd = atoi(argv[2]);
    std::string user = o.user;
    o.authenticate = [user](const char *password) {
      return lock::authenticate(lock::system_pam(), "sde-lock", user, password);
    };
    return lock::run_lock_ui(o);
  }
  install_crash_handler("sde-lock");
  if (cmd == "--version") {
    printf("sde-lock %s (%s)\n", SDE_APP_VERSION, SDE_VERSION_LABEL);
    return 0;
  }
  if (cmd == "--help" || cmd == "-h") {
    usage(stdout);
    return 0;
  }
  if (cmd == "--daemon") return lock::run_daemon(argc > 2 && !strcmp(argv[2], "--lock"));
  if (cmd == "--lock" || cmd == "--activate") return lock_now();
  if (cmd == "--query") {
    DBusMessage *reply = call("GetActive", 2000);
    if (!reply) {
      printf("unlocked\n");
      return daemon_running() ? 1 : 0;
    }
    auto a = dbus::read_args(reply);
    dbus_message_unref(reply);
    printf("%s\n", !a.empty() && a[0].boolean() ? "locked" : "unlocked");
    return 0;
  }
  if (cmd == "--poke" || cmd == "--deactivate") {
    DBusMessage *reply = call("SimulateUserActivity", 2000);
    if (reply) dbus_message_unref(reply);
    return reply ? 0 : 1;
  }
  if (cmd == "--inhibitors") {
    DBusMessage *reply = call("ListInhibitors", 2000);
    if (!reply) return 1;
    auto a = dbus::read_args(reply);
    dbus_message_unref(reply);
    if (!a.empty())
      for (auto &s : a[0].items)
        if (s.items.size() == 3)
          printf("%lld\t%s\t%s\n", static_cast<long long>(s.items[0].num()), s.items[1].s.c_str(),
                 s.items[2].s.c_str());
    return 0;
  }
  usage(stderr);
  return 2;
}
