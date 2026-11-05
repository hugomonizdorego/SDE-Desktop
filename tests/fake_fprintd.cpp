// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// A fake fprintd (net.reactivated.Fprint) for the fingerprint tests (not
// installed; also for tools/showcase.py): the Manager and one device (or
// none) with fprintd's methods, properties, signals and errors, on the bus
// the program is given as the system bus (DBUS_SYSTEM_BUS_ADDRESS: the
// tests' private dbus-daemon). No reader, no polkit and no fingerprints of
// anybody are involved: enrolling and verifying follow scripts.
//
//   sde-fake-fprintd LOG [--devices 0|1] [--enrolled F,F] [--stages N]
//                        [--scan-type press|swipe] [--name TEXT]
//                        [--enroll-script S,S,...] [--verify-script S,...]
//                        [--step-ms MS] [--deny-enroll]
//
// EnrollStart plays the enroll script (default: enroll-stage-passed for
// all stages but the last, then enroll-completed), one status every
// --step-ms (300); VerifyStart plays the verify script, or waits for Touch.
// The control interface org.sde.FakeFprint at /org/sde/FakeFprint: Touch(s)
// sends one status now (VerifyStatus or EnrollStatus, whichever runs),
// SetEnrollScript(as), SetVerifyScript(as), SetEnrolled(as), SetDevices(u),
// GetState() -> s. The same as lines on standard input (the tests need no
// D-Bus tool): "touch STATUS", "enroll-script S,S", "verify-script S,S",
// "enrolled F,F", "devices N", "deny-enroll on|off" (polkit says no),
// "state" (logs "STATE ..."). A claim ends
// when its client leaves the bus, as with fprintd. Every call and signal is
// logged to LOG ("READY" once the name is owned).
#include <dbus/dbus.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

const char *kService = "net.reactivated.Fprint";
const char *kManagerPath = "/net/reactivated/Fprint/Manager";
const char *kDevicePath = "/net/reactivated/Fprint/Device/0";
const char *kManager = "net.reactivated.Fprint.Manager";
const char *kDevice = "net.reactivated.Fprint.Device";
const char *kControlPath = "/org/sde/FakeFprint";
const char *kControl = "org.sde.FakeFprint";
const char *kProps = "org.freedesktop.DBus.Properties";
const std::string kError = "net.reactivated.Fprint.Error.";

volatile sig_atomic_t g_stop = 0;
FILE *g_log = nullptr;
DBusConnection *g_conn = nullptr;

struct State {
  int devices = 1;
  std::vector<std::string> enrolled;
  int stages = 5;
  std::string scan_type = "press";
  std::string name = "SDE Test Fingerprint Reader";
  std::vector<std::string> enroll_script, verify_script;
  int step_ms = 300;
  bool deny_enroll = false;
  std::string claimed_by;
  std::string action;  // "", "enroll", "verify"
  std::string finger;  // being enrolled
  std::deque<std::string> queue;  // statuses still to send
  double next_at = 0;
} s;

double now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void log_line(const std::string &line) {
  fprintf(g_log, "%s\n", line.c_str());
  fflush(g_log);
}

std::vector<std::string> split(const std::string &text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t comma = text.find(',', start);
    std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!item.empty()) out.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string joined(const std::vector<std::string> &v) {
  std::string out;
  for (const std::string &x : v) out += (out.empty() ? "" : ",") + x;
  return out;
}

bool valid_finger(const std::string &f) {
  static const char *const names[] = {"left-thumb", "left-index-finger", "left-middle-finger", "left-ring-finger",
                                      "left-little-finger", "right-thumb", "right-index-finger",
                                      "right-middle-finger", "right-ring-finger", "right-little-finger"};
  for (const char *n : names)
    if (f == n) return true;
  return false;
}

std::vector<std::string> read_strings(DBusMessage *m) {
  std::vector<std::string> out;
  DBusMessageIter it;
  if (!dbus_message_iter_init(m, &it)) return out;
  do {
    int t = dbus_message_iter_get_arg_type(&it);
    if (t == DBUS_TYPE_STRING) {
      const char *v;
      dbus_message_iter_get_basic(&it, &v);
      out.push_back(v);
    } else if (t == DBUS_TYPE_ARRAY && dbus_message_iter_get_element_type(&it) == DBUS_TYPE_STRING) {
      DBusMessageIter sub;
      dbus_message_iter_recurse(&it, &sub);
      while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING) {
        const char *v;
        dbus_message_iter_get_basic(&sub, &v);
        out.push_back(std::string("[]") + v);
        dbus_message_iter_next(&sub);
      }
    } else if (t == DBUS_TYPE_UINT32) {
      dbus_uint32_t v;
      dbus_message_iter_get_basic(&it, &v);
      out.push_back(std::to_string(v));
    }
  } while (dbus_message_iter_next(&it));
  return out;
}

void send(DBusMessage *m) {
  dbus_connection_send(g_conn, m, nullptr);
  dbus_message_unref(m);
}

void reply_empty(DBusMessage *call) { send(dbus_message_new_method_return(call)); }

void reply_error(DBusMessage *call, const std::string &name, const std::string &text) {
  log_line("ERROR " + name);
  send(dbus_message_new_error(call, name.c_str(), text.c_str()));
}

void reply_strings(DBusMessage *call, const std::vector<std::string> &v, int type = DBUS_TYPE_STRING) {
  DBusMessage *r = dbus_message_new_method_return(call);
  DBusMessageIter it, arr;
  dbus_message_iter_init_append(r, &it);
  dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, type == DBUS_TYPE_STRING ? "s" : "o", &arr);
  for (const std::string &x : v) {
    const char *c = x.c_str();
    dbus_message_iter_append_basic(&arr, type, &c);
  }
  dbus_message_iter_close_container(&it, &arr);
  send(r);
}

void reply_string(DBusMessage *call, const std::string &v) {
  DBusMessage *r = dbus_message_new_method_return(call);
  const char *c = v.c_str();
  dbus_message_append_args(r, DBUS_TYPE_STRING, &c, DBUS_TYPE_INVALID);
  send(r);
}

void status_signal(const char *member, const std::string &result, bool done) {
  DBusMessage *m = dbus_message_new_signal(kDevicePath, kDevice, member);
  const char *c = result.c_str();
  dbus_bool_t d = done;
  if (!strcmp(member, "VerifyFingerSelected"))
    dbus_message_append_args(m, DBUS_TYPE_STRING, &c, DBUS_TYPE_INVALID);
  else
    dbus_message_append_args(m, DBUS_TYPE_STRING, &c, DBUS_TYPE_BOOLEAN, &d, DBUS_TYPE_INVALID);
  send(m);
  dbus_connection_flush(g_conn);
  log_line(std::string("SIGNAL ") + member + " " + result + (done ? " done" : ""));
}

bool final_status(const std::string &r) {
  return r == "enroll-completed" || r == "enroll-failed" || r == "enroll-data-full" || r == "enroll-duplicate" ||
         r == "enroll-disconnected" || r == "enroll-unknown-error" || r == "verify-match" ||
         r == "verify-no-match" || r == "verify-disconnected" || r == "verify-unknown-error";
}

// Sends one status of the action that runs.
void play(const std::string &result) {
  if (s.action.empty()) {
    log_line("IGNORED " + result + " (no action)");
    return;
  }
  bool done = final_status(result);
  if (s.action == "enroll") {
    status_signal("EnrollStatus", result, done);
    if (result == "enroll-completed" && std::find(s.enrolled.begin(), s.enrolled.end(), s.finger) == s.enrolled.end())
      s.enrolled.push_back(s.finger);
  } else {
    status_signal("VerifyStatus", result, done);
  }
  if (done) s.queue.clear();
}

void schedule(const std::vector<std::string> &script) {
  s.queue.assign(script.begin(), script.end());
  s.next_at = now_ms() + s.step_ms;
}

void stop_action() {
  s.action.clear();
  s.queue.clear();
}

void append_variant(DBusMessageIter *it, const std::string &prop) {
  DBusMessageIter var;
  if (prop == "num-enroll-stages") {
    dbus_int32_t v = s.stages;
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "i", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &v);
  } else if (prop == "finger-present" || prop == "finger-needed") {
    dbus_bool_t v = prop == "finger-needed" && !s.action.empty();
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "b", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
  } else {
    std::string value = prop == "name" ? s.name : s.scan_type;
    const char *c = value.c_str();
    dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "s", &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &c);
  }
  dbus_message_iter_close_container(it, &var);
}

const char *const kPropNames[] = {"name", "num-enroll-stages", "scan-type", "finger-present", "finger-needed"};

void device_call(DBusMessage *m, const std::string &member, const std::string &sender,
                 const std::vector<std::string> &args) {
  auto claimed = [&] { return s.claimed_by == sender; };
  if (member == "ListEnrolledFingers") {
    if (s.enrolled.empty()) return reply_error(m, kError + "NoEnrolledPrints", "No prints enrolled");
    return reply_strings(m, s.enrolled);
  }
  if (member == "Claim") {
    if (!s.claimed_by.empty()) return reply_error(m, kError + "AlreadyInUse", "Device was already claimed");
    s.claimed_by = sender;
    return reply_empty(m);
  }
  if (member == "Release") {
    if (!claimed()) return reply_error(m, kError + "ClaimDevice", "Device was not claimed before use");
    s.claimed_by.clear();
    stop_action();
    return reply_empty(m);
  }
  if (!claimed()) return reply_error(m, kError + "ClaimDevice", "Device was not claimed before use");
  std::string arg = args.empty() ? "" : args[0];
  if (member == "EnrollStart") {
    if (!s.action.empty()) return reply_error(m, kError + "AlreadyInUse", "Another operation is already in progress");
    if (!valid_finger(arg)) return reply_error(m, kError + "InvalidFingername", "Invalid finger name");
    if (s.deny_enroll) return reply_error(m, kError + "PermissionDenied", "Not Authorized: net.reactivated.fprint.device.enroll");
    s.action = "enroll";
    s.finger = arg;
    std::vector<std::string> script = s.enroll_script;
    if (script.empty()) {
      for (int i = 1; i < s.stages; ++i) script.push_back("enroll-stage-passed");
      script.push_back("enroll-completed");
    }
    schedule(script);
    return reply_empty(m);
  }
  if (member == "VerifyStart") {
    if (!s.action.empty()) return reply_error(m, kError + "AlreadyInUse", "Another operation is already in progress");
    if (s.enrolled.empty()) return reply_error(m, kError + "NoEnrolledPrints", "No fingerprints enrolled");
    s.action = "verify";
    reply_empty(m);
    dbus_connection_flush(g_conn);
    status_signal("VerifyFingerSelected", arg == "any" ? "any" : arg, false);
    schedule(s.verify_script);
    return;
  }
  if (member == "EnrollStop" || member == "VerifyStop") {
    std::string want = member == "EnrollStop" ? "enroll" : "verify";
    // fprintd forgets an action that ended; a stop after it is fine.
    if (!s.action.empty() && s.action != want)
      return reply_error(m, kError + "NoActionInProgress", "No such action in progress");
    stop_action();
    return reply_empty(m);
  }
  if (member == "DeleteEnrolledFinger") {
    auto it = std::find(s.enrolled.begin(), s.enrolled.end(), arg);
    if (it == s.enrolled.end()) return reply_error(m, kError + "NoEnrolledPrints", "Fingerprint not enrolled");
    s.enrolled.erase(it);
    return reply_empty(m);
  }
  if (member == "DeleteEnrolledFingers2") {
    if (s.enrolled.empty()) return reply_error(m, kError + "NoEnrolledPrints", "No prints to delete");
    s.enrolled.clear();
    return reply_empty(m);
  }
  reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method " + member);
}

std::string state_text();

DBusHandlerResult on_message(DBusConnection *, DBusMessage *m, void *) {
  int type = dbus_message_get_type(m);
  const char *iface_c = dbus_message_get_interface(m), *member_c = dbus_message_get_member(m);
  const char *path_c = dbus_message_get_path(m), *sender_c = dbus_message_get_sender(m);
  std::string iface = iface_c ? iface_c : "", member = member_c ? member_c : "", path = path_c ? path_c : "";
  std::string sender = sender_c ? sender_c : "";
  if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
    // A client that held the device left the bus: fprintd releases it.
    if (iface == DBUS_INTERFACE_DBUS && member == "NameOwnerChanged") {
      const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
      if (dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING,
                                &new_owner, DBUS_TYPE_INVALID) &&
          !s.claimed_by.empty() && s.claimed_by == name && !*new_owner) {
        log_line("GONE " + s.claimed_by + " (released)");
        s.claimed_by.clear();
        stop_action();
      }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  if (type != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  std::vector<std::string> args = read_strings(m);
  std::string line = "CALL " + member;
  for (const std::string &a : args) line += " " + (a.empty() ? std::string("''") : a);
  log_line(line + " from " + sender);
  if (iface == kProps && (path == kDevicePath) && s.devices > 0) {
    if (member == "GetAll") {
      DBusMessage *r = dbus_message_new_method_return(m);
      DBusMessageIter it, dict, entry;
      dbus_message_iter_init_append(r, &it);
      dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
      for (const char *p : kPropNames) {
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &p);
        append_variant(&entry, p);
        dbus_message_iter_close_container(&dict, &entry);
      }
      dbus_message_iter_close_container(&it, &dict);
      send(r);
    } else if (member == "Get" && args.size() >= 2) {
      DBusMessage *r = dbus_message_new_method_return(m);
      DBusMessageIter it;
      dbus_message_iter_init_append(r, &it);
      append_variant(&it, args[1]);
      send(r);
    } else {
      reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, "Unknown property call");
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (path == kManagerPath && iface == kManager) {
    if (member == "GetDevices") {
      std::vector<std::string> v;
      if (s.devices > 0) v.push_back(kDevicePath);
      reply_strings(m, v, DBUS_TYPE_OBJECT_PATH);
    } else if (member == "GetDefaultDevice") {
      if (s.devices > 0) {
        DBusMessage *r = dbus_message_new_method_return(m);
        dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH, &kDevicePath, DBUS_TYPE_INVALID);
        send(r);
      } else {
        reply_error(m, kError + "NoSuchDevice", "No devices available");
      }
    } else {
      reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method " + member);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (path == kDevicePath && iface == kDevice && s.devices > 0) {
    device_call(m, member, sender, args);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (path == kControlPath && iface == kControl) {
    auto list = [&] {
      std::vector<std::string> out;
      for (const std::string &a : args)
        if (a.compare(0, 2, "[]") == 0) out.push_back(a.substr(2));
      return out;
    };
    if (member == "Touch" && !args.empty()) {
      play(args[0]);
      reply_empty(m);
    } else if (member == "SetEnrollScript") {
      s.enroll_script = list();
      reply_empty(m);
    } else if (member == "SetVerifyScript") {
      s.verify_script = list();
      reply_empty(m);
    } else if (member == "SetEnrolled") {
      s.enrolled = list();
      reply_empty(m);
    } else if (member == "SetDevices" && !args.empty()) {
      s.devices = atoi(args[0].c_str());
      reply_empty(m);
    } else if (member == "GetState") {
      reply_string(m, state_text());
    } else {
      reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method " + member);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (iface == "org.freedesktop.DBus.Introspectable") {
    reply_string(m, "<node/>");
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  reply_error(m, DBUS_ERROR_UNKNOWN_OBJECT, "No such object " + path);
  return DBUS_HANDLER_RESULT_HANDLED;
}

void on_signal_stop(int) { g_stop = 1; }

std::string state_text() {
  return "claimed=" + s.claimed_by + " action=" + s.action + " enrolled=" + joined(s.enrolled) +
         " devices=" + std::to_string(s.devices);
}

// One command line from standard input.
void command(const std::string &line) {
  size_t sp = line.find(' ');
  std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
  log_line("COMMAND " + line);
  if (cmd == "touch") play(arg);
  else if (cmd == "enroll-script") s.enroll_script = split(arg);
  else if (cmd == "verify-script") s.verify_script = split(arg);
  else if (cmd == "enrolled") s.enrolled = split(arg);
  else if (cmd == "devices") s.devices = atoi(arg.c_str());
  else if (cmd == "deny-enroll") s.deny_enroll = arg == "on";
  else if (cmd == "state") log_line("STATE " + state_text());
}

void read_commands() {
  static std::string pending;
  static bool open = true;
  if (!open) return;
  struct pollfd p = {0, POLLIN, 0};
  while (poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) {
    char buf[512];
    ssize_t n = read(0, buf, sizeof buf);
    if (n <= 0) {
      open = false;  // no more commands (stdin closed or /dev/null)
      return;
    }
    pending.append(buf, size_t(n));
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (!line.empty()) command(line);
    }
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: sde-fake-fprintd LOG [options]\n");
    return 2;
  }
  g_log = fopen(argv[1], "a");
  if (!g_log) return 1;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--devices") s.devices = atoi(value().c_str());
    else if (a == "--enrolled") s.enrolled = split(value());
    else if (a == "--stages") s.stages = std::max(1, atoi(value().c_str()));
    else if (a == "--scan-type") s.scan_type = value();
    else if (a == "--name") s.name = value();
    else if (a == "--enroll-script") s.enroll_script = split(value());
    else if (a == "--verify-script") s.verify_script = split(value());
    else if (a == "--step-ms") s.step_ms = std::max(10, atoi(value().c_str()));
    else if (a == "--deny-enroll") s.deny_enroll = true;
    else {
      fprintf(stderr, "sde-fake-fprintd: unknown option %s\n", a.c_str());
      return 2;
    }
  }
  signal(SIGTERM, on_signal_stop);
  signal(SIGINT, on_signal_stop);
  DBusError err;
  dbus_error_init(&err);
  g_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (!g_conn) {
    fprintf(stderr, "sde-fake-fprintd: %s\n", err.message);
    return 1;
  }
  dbus_connection_set_exit_on_disconnect(g_conn, FALSE);
  if (dbus_bus_request_name(g_conn, kService, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err) !=
      DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    fprintf(stderr, "sde-fake-fprintd: cannot own %s\n", kService);
    return 1;
  }
  dbus_bus_add_match(g_conn, "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged'", nullptr);
  dbus_connection_add_filter(g_conn, on_message, nullptr, nullptr);
  log_line("READY " + std::string(dbus_bus_get_unique_name(g_conn)));
  while (!g_stop) {
    if (!dbus_connection_read_write_dispatch(g_conn, 20)) break;
    read_commands();
    if (!s.queue.empty() && now_ms() >= s.next_at) {
      std::string next = s.queue.front();
      s.queue.pop_front();
      play(next);
      s.next_at = now_ms() + s.step_ms;
    }
  }
  log_line("EXIT");
  return 0;
}
