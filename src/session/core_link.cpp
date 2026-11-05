// SPDX-License-Identifier: GPL-3.0-or-later
#include "core_link.h"

#include <dbus/dbus.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core_client.h"

namespace core_link {

namespace {

DBusConnection *g_conn = nullptr;
std::string g_phase = "starting";
std::function<void()> g_started;

DBusHandlerResult filter(DBusConnection *, DBusMessage *msg, void *) {
  if (!dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged"))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
  DBusError err;
  dbus_error_init(&err);
  if (dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING,
                            &new_owner, DBUS_TYPE_INVALID) &&
      name && strcmp(name, sde::core::kName) == 0 && new_owner && *new_owner) {
    // sde-core started (again): it knows nothing of this session yet.
    phase(g_phase.c_str());
    if (g_started) g_started();
  }
  dbus_error_free(&err);
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void send(DBusMessage *m) {
  if (!m) return;
  dbus_message_set_no_reply(m, TRUE);
  // sde-core is never started for a message: the session starts it.
  dbus_message_set_auto_start(m, FALSE);
  dbus_connection_send(g_conn, m, nullptr);
  dbus_connection_flush(g_conn);
  dbus_message_unref(m);
}

}  // namespace

void open() {
  if (g_conn) return;
  const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (!address || !*address) return;  // libdbus would start a bus of its own
  DBusError err;
  dbus_error_init(&err);
  g_conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  dbus_error_free(&err);
  if (!g_conn) return;
  dbus_connection_set_exit_on_disconnect(g_conn, FALSE);
  dbus_connection_add_filter(g_conn, filter, nullptr, nullptr);
  std::string rule = std::string("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                                 "member='NameOwnerChanged',arg0='") +
                     sde::core::kName + "'";
  dbus_bus_add_match(g_conn, rule.c_str(), nullptr);
  dbus_connection_flush(g_conn);
}

int fd() {
  int fd = -1;
  if (!g_conn || !dbus_connection_get_unix_fd(g_conn, &fd)) return -1;
  return fd;
}

void dispatch() {
  if (!g_conn) return;
  // Reads what is there without waiting, then handles every message.
  dbus_connection_read_write(g_conn, 0);
  while (dbus_connection_dispatch(g_conn) == DBUS_DISPATCH_DATA_REMAINS) {
  }
  if (!dbus_connection_get_is_connected(g_conn)) {
    dbus_connection_close(g_conn);
    dbus_connection_unref(g_conn);
    g_conn = nullptr;
  }
}

void report(const std::string &kind, long pid, const std::string &name, const std::string &id,
            const std::string &detail) {
  if (!g_conn) return;
  DBusMessage *m = dbus_message_new_method_call(sde::core::kName, sde::core::kPath, sde::core::kAppsIface, "Report");
  if (!m) return;
  const char *k = kind.c_str(), *n = name.c_str(), *i = id.c_str(), *d = detail.c_str();
  dbus_uint32_t p = pid > 0 ? dbus_uint32_t(pid) : 0;
  // The names come from desktop files and argv: never invalid UTF-8 on the bus.
  if (!dbus_validate_utf8(n, nullptr) || !dbus_validate_utf8(i, nullptr) || !dbus_validate_utf8(d, nullptr)) {
    dbus_message_unref(m);
    return;
  }
  dbus_message_append_args(m, DBUS_TYPE_STRING, &k, DBUS_TYPE_UINT32, &p, DBUS_TYPE_STRING, &n, DBUS_TYPE_STRING,
                           &i, DBUS_TYPE_STRING, &d, DBUS_TYPE_INVALID);
  send(m);
}

void phase(const char *value) {
  g_phase = value;
  if (!g_conn) return;
  DBusMessage *m = dbus_message_new_method_call(sde::core::kName, sde::core::kPath, sde::core::kSessionIface,
                                                "SetPhase");
  if (!m) return;
  dbus_message_append_args(m, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
  send(m);
}

void on_core_started(std::function<void()> fn) { g_started = std::move(fn); }

}  // namespace core_link
