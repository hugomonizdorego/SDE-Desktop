// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// A fake PackageKit on the session bus for Debrun's UI test (not
// installed): org.freedesktop.PackageKit with CreateTransaction, and
// transactions that answer SetHints, InstallFiles, Resolve, RemovePackages
// and RefreshCache with Status/Percentage changes and Finished. Every call
// is logged to the file named on the command line.
//
//   debrun-fake-packagekit LOG     (FAKE_PK_FAIL=auth: InstallFiles is refused)
#include <dbus/dbus.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
FILE *g_log = nullptr;
int g_next = 1;
const char *kTx = "org.freedesktop.PackageKit.Transaction";

void log_line(const std::string &line) {
  fprintf(g_log, "%s\n", line.c_str());
  fflush(g_log);
}

std::vector<std::string> strings(DBusMessageIter *it) {
  std::vector<std::string> out;
  if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY) return out;
  DBusMessageIter sub;
  dbus_message_iter_recurse(it, &sub);
  while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
    const char *s;
    dbus_message_iter_get_basic(&sub, &s);
    out.push_back(s);
    dbus_message_iter_next(&sub);
  }
  return out;
}

void send_properties(DBusConnection *c, const char *path, unsigned status, unsigned percent) {
  DBusMessage *m = dbus_message_new_signal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
  DBusMessageIter it, dict, entry, var;
  dbus_message_iter_init_append(m, &it);
  const char *iface = kTx;
  dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
  dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
  const char *keys[2] = {"Status", "Percentage"};
  unsigned values[2] = {status, percent};
  for (int i = 0; i < 2; ++i) {
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &keys[i]);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &values[i]);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(&dict, &entry);
  }
  dbus_message_iter_close_container(&it, &dict);
  DBusMessageIter none;
  dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &none);
  dbus_message_iter_close_container(&it, &none);
  dbus_connection_send(c, m, nullptr);
  dbus_message_unref(m);
  dbus_connection_flush(c);
}

void send_finished(DBusConnection *c, const char *path, unsigned exit_code) {
  DBusMessage *m = dbus_message_new_signal(path, kTx, "Finished");
  unsigned runtime = 100;
  dbus_message_append_args(m, DBUS_TYPE_UINT32, &exit_code, DBUS_TYPE_UINT32, &runtime, DBUS_TYPE_INVALID);
  dbus_connection_send(c, m, nullptr);
  dbus_message_unref(m);
  dbus_connection_flush(c);
}

void send_error(DBusConnection *c, const char *path, unsigned code, const char *text) {
  DBusMessage *m = dbus_message_new_signal(path, kTx, "ErrorCode");
  dbus_message_append_args(m, DBUS_TYPE_UINT32, &code, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
  dbus_connection_send(c, m, nullptr);
  dbus_message_unref(m);
  dbus_connection_flush(c);
}

void send_package(DBusConnection *c, const char *path, const std::string &id) {
  DBusMessage *m = dbus_message_new_signal(path, kTx, "Package");
  unsigned info = 1;
  const char *pid = id.c_str(), *summary = "fake";
  dbus_message_append_args(m, DBUS_TYPE_UINT32, &info, DBUS_TYPE_STRING, &pid, DBUS_TYPE_STRING, &summary,
                           DBUS_TYPE_INVALID);
  dbus_connection_send(c, m, nullptr);
  dbus_message_unref(m);
  dbus_connection_flush(c);
}

// A change: some progress, then Finished (success unless |fail|).
void work(DBusConnection *c, const char *path, unsigned status, bool fail) {
  for (unsigned p : {20u, 55u, 90u}) {
    send_properties(c, path, status, p);
    usleep(250 * 1000);
  }
  if (fail) {
    send_error(c, path, 48, "Not authorized");
    send_finished(c, path, 2);
  } else {
    send_finished(c, path, 1);
  }
}

DBusHandlerResult handle(DBusConnection *c, DBusMessage *m, void *) {
  if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  std::string path = dbus_message_get_path(m) ? dbus_message_get_path(m) : "";
  std::string member = dbus_message_get_member(m) ? dbus_message_get_member(m) : "";
  DBusMessageIter it;
  bool args = dbus_message_iter_init(m, &it);
  if (path == "/org/freedesktop/PackageKit" && member == "CreateTransaction") {
    std::string tx = "/org/freedesktop/PackageKit/Transaction/" + std::to_string(g_next++);
    DBusMessage *r = dbus_message_new_method_return(m);
    const char *p = tx.c_str();
    dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
    dbus_connection_send(c, r, nullptr);
    dbus_message_unref(r);
    log_line("CreateTransaction " + tx);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  DBusMessage *r = dbus_message_new_method_return(m);
  dbus_connection_send(c, r, nullptr);
  dbus_message_unref(r);
  dbus_connection_flush(c);
  if (member == "SetHints") {
    std::string line = "SetHints";
    if (args)
      for (auto &h : strings(&it)) line += " " + h;
    log_line(line);
  } else if (member == "InstallFiles" && args) {
    dbus_uint64_t flags = 0;
    if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT64) dbus_message_iter_get_basic(&it, &flags);
    dbus_message_iter_next(&it);
    std::string line = "InstallFiles " + std::to_string((unsigned long long)flags);
    for (auto &f : strings(&it)) line += " " + f;
    log_line(line);
    const char *fail = getenv("FAKE_PK_FAIL");
    work(c, path.c_str(), 9, fail && !strcmp(fail, "auth"));
  } else if (member == "Resolve" && args) {
    dbus_message_iter_next(&it);
    std::vector<std::string> names = strings(&it);
    std::string line = "Resolve";
    for (auto &n : names) {
      line += " " + n;
      send_package(c, path.c_str(), n + ";3.1.4-2;amd64;installed");
    }
    log_line(line);
    send_finished(c, path.c_str(), 1);
  } else if (member == "RemovePackages" && args) {
    dbus_message_iter_next(&it);
    std::string line = "RemovePackages";
    for (auto &id : strings(&it)) line += " " + id;
    log_line(line);
    work(c, path.c_str(), 6, false);
  } else if (member == "RefreshCache") {
    log_line("RefreshCache");
    work(c, path.c_str(), 7, false);
  } else {
    log_line("call " + member);
  }
  return DBUS_HANDLER_RESULT_HANDLED;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  g_log = fopen(argv[1], "a");
  if (!g_log) return 2;
  signal(SIGTERM, [](int) { g_stop = 1; });
  signal(SIGINT, [](int) { g_stop = 1; });
  DBusError err;
  dbus_error_init(&err);
  DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
  if (!c) {
    fprintf(stderr, "no session bus: %s\n", err.message ? err.message : "?");
    return 77;
  }
  dbus_connection_set_exit_on_disconnect(c, FALSE);
  if (dbus_bus_request_name(c, "org.freedesktop.PackageKit", DBUS_NAME_FLAG_DO_NOT_QUEUE, &err) !=
      DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    fprintf(stderr, "cannot own org.freedesktop.PackageKit\n");
    return 1;
  }
  DBusObjectPathVTable vt = {};
  vt.message_function = handle;
  dbus_connection_register_fallback(c, "/", &vt, nullptr);
  log_line("READY");
  while (!g_stop && dbus_connection_read_write_dispatch(c, 100)) {
  }
  dbus_connection_close(c);
  dbus_connection_unref(c);
  fclose(g_log);
  return 0;
}
