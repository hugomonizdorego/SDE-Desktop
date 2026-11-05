// SPDX-License-Identifier: GPL-3.0-or-later
#include "core_client.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "config.h"
#include "dbus.h"
#include "integration.h"
#include "rust_core.h"
#include "util.h"
#include "x11.h"

namespace sde {
namespace core {

namespace {

struct Watcher {
  int id;
  bool look;
  std::function<void(const std::vector<Key> &)> settings;
  std::function<void()> on_look;
};

struct Instance {
  std::string app_id;
  std::function<void(const std::vector<std::string> &)> activated;
};

struct State {
  bool asked = false, answered = false, up = false;
  std::string owner;  // sde-core's unique name
  std::vector<Watcher> watchers;
  int next_id = 1;
  bool x_handler = false, exported = false;
  std::vector<Instance> instances;
};
State &g() {
  static State s;
  return s;
}

// The session bus, when one is known: never one started for this program
// (libdbus would "autolaunch" a bus for a program without one).
dbus::Bus *bus() {
  std::string address = env("DBUS_SESSION_BUS_ADDRESS");
  if (address.empty()) {
    std::string runtime = env("XDG_RUNTIME_DIR");
    struct stat st;
    if (runtime.empty() || stat((runtime + "/bus").c_str(), &st) != 0 || !S_ISSOCK(st.st_mode)) return nullptr;
  }
  return dbus::Bus::session();
}

DBusMessage *call(const char *iface, const char *member) {
  DBusMessage *m = dbus::method(kName, kPath, iface, member);
  // Never start sde-core for a call: it runs in SDE sessions only.
  if (m) dbus_message_set_auto_start(m, FALSE);
  return m;
}

std::vector<Key> read_keys(DBusMessage *msg) {
  std::vector<Key> keys;
  auto args = dbus::read_args(msg);
  if (args.empty()) return keys;
  for (const auto &item : args[0].items)
    if (item.items.size() == 2) keys.push_back({item.items[0].s, item.items[1].s});
  return keys;
}

void notify(const std::vector<Key> &keys, bool look_changed) {
  // Copy: a watcher may add or remove watchers.
  auto list = g().watchers;
  for (const auto &w : list) {
    if (w.look && look_changed && w.on_look) w.on_look();
    if (!w.look && w.settings) w.settings(keys);
  }
}

void register_instance_async(const Instance &inst);

void set_owner(const std::string &owner) {
  State &s = g();
  bool was_up = s.up;
  s.owner = owner;
  s.up = !owner.empty();
  // sde-core started again: it does not know this program's instances yet.
  if (s.up && !was_up)
    for (const auto &inst : s.instances) register_instance_async(inst);
}

// The root window's _SDE_CONFIG_SERIAL while sde-core does not run.
int x_event(void *event, void *) {
  const XEvent *e = static_cast<const XEvent *>(event);
  if (!fl_display || e->type != PropertyNotify || g().up) return 0;
  if (e->xproperty.window != RootWindow(fl_display, fl_screen) ||
      e->xproperty.atom != x11::atom("_SDE_CONFIG_SERIAL"))
    return 0;
  notify({}, true);
  return 0;
}

void install_x_handler() {
  State &s = g();
  if (s.x_handler || !fl_display) return;
  s.x_handler = true;
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  if (XGetWindowAttributes(fl_display, root, &attr))
    XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  Fl::add_system_handler(x_event, nullptr);
}

void ensure() {
  State &s = g();
  if (s.asked) return;
  s.asked = true;
  dbus::Bus *b = bus();
  if (!b) {
    s.answered = true;
    return;
  }
  b->add_signal_handler(
      std::string("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                  "member='NameOwnerChanged',arg0='") + kName + "'",
      [](DBusMessage *m) { return dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged"); },
      [](DBusMessage *m) {
        auto args = dbus::read_args(m);
        if (args.size() == 3 && args[0].s == kName) set_owner(args[2].s);
      });
  b->add_signal_handler(
      std::string("type='signal',sender='") + kName + "',path='" + kPath + "'",
      [](DBusMessage *m) {
        const char *sender = dbus_message_get_sender(m);
        return sender && g().up && g().owner == sender;
      },
      [](DBusMessage *m) {
        if (dbus_message_is_signal(m, kSettingsIface, "Changed")) notify(read_keys(m), false);
        else if (dbus_message_is_signal(m, kLookIface, "LookChanged")) notify({}, true);
      });
  DBusMessage *m = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                "GetNameOwner");
  dbus::Writer(m).str(kName);
  b->call_async_any(m, [](DBusMessage *reply) {
    State &st = g();
    if (st.answered) return;  // wait_available() asked meanwhile
    st.answered = true;
    if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
      auto args = dbus::read_args(reply);
      if (!args.empty()) set_owner(args[0].s);
    }
  }, 2000);
}

void fallback(const std::vector<Write> &writes, Mode mode) {
  Config cfg;
  cfg.load();
  bool any = false;
  for (const auto &w : writes) {
    if (w.reset) {
      cfg.unset(w.section, w.key);
      any = true;
      continue;
    }
    char *out = nullptr;
    int r = sde_schema_check(w.section.c_str(), w.key.c_str(), w.value.c_str(), &out);
    std::string text = out ? out : "";
    sde_config_string_free(out);
    if (r == 1 || r == 2) {
      fprintf(stderr, "sde: not saving [%s] %s=%s: %s\n", w.section.c_str(), w.key.c_str(), w.value.c_str(),
              r == 1 ? "not a valid key" : text.c_str());
      continue;
    }
    cfg.set(w.section, w.key, text);
    any = true;
  }
  if (!any) return;
  if (!cfg.save()) {
    fprintf(stderr, "sde: cannot save %s\n", Config::user_path().c_str());
    return;
  }
  if (mode == Mode::APPEARANCE) apply_appearance(nullptr, cfg);
  // The session writes what its programs read from sde.conf when it sees
  // the announcement; without one this program does.
  else if (fl_display && session_coordinates(nullptr)) x11::notify_config_changed();
  else announce_config_change(nullptr, cfg);
}

void register_instance_async(const Instance &inst) {
  dbus::Bus *b = bus();
  DBusMessage *m = call(kAppsIface, "Register");
  if (!b || !m) {
    if (m) dbus_message_unref(m);
    return;
  }
  dbus::Writer(m).str(inst.app_id);
  b->call_async(m, nullptr);
}

DBusHandlerResult instance_method(DBusMessage *msg) {
  dbus::Bus *b = bus();
  if (!b || !dbus_message_is_method_call(msg, kInstanceIface, "Activate") ||
      !dbus_message_has_signature(msg, "sas"))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  // Only sde-core hands over the arguments of another start.
  const char *sender = dbus_message_get_sender(msg);
  if (!sender || !g().up || g().owner != sender) {
    b->send(dbus::Bus::error(msg, "org.freedesktop.DBus.Error.AccessDenied", "only sde-core activates"));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  auto args = dbus::read_args(msg);
  std::vector<std::string> list;
  for (const auto &v : args[1].items) list.push_back(v.s);
  for (const auto &inst : g().instances) {
    if (inst.app_id != args[0].s) continue;
    struct Later {
      std::function<void(const std::vector<std::string> &)> fn;
      std::vector<std::string> args;
    };
    auto *later = new Later{inst.activated, list};
    Fl::add_timeout(0, [](void *p) {
      std::unique_ptr<Later> l(static_cast<Later *>(p));
      if (l->fn) l->fn(l->args);
    }, later);
    b->send(dbus_message_new_method_return(msg));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  b->send(dbus::Bus::error(msg, "org.selviana.Core.Error.NotFound", "not an instance of this program"));
  return DBUS_HANDLER_RESULT_HANDLED;
}

// A boolean reply, or |fallback| without one.
bool reply_bool(DBusMessage *reply, bool fallback_value) {
  bool value = fallback_value;
  if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
    auto args = dbus::read_args(reply);
    if (!args.empty() && args[0].type == 'b') value = args[0].b;
  }
  if (reply) dbus_message_unref(reply);
  return value;
}

}  // namespace

bool available() {
  ensure();
  return g().up;
}

bool wait_available(int timeout_ms) {
  ensure();
  State &s = g();
  if (s.answered) return s.up;
  dbus::Bus *b = bus();
  if (!b) return false;
  DBusMessage *m = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                "GetNameOwner");
  dbus::Writer(m).str(kName);
  DBusMessage *reply = b->call(m, timeout_ms);
  s.answered = true;
  if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
    auto args = dbus::read_args(reply);
    if (!args.empty()) set_owner(args[0].s);
  }
  if (reply) dbus_message_unref(reply);
  return s.up;
}

void write(const std::vector<Write> &writes, Mode mode) {
  if (writes.empty()) return;
  ensure();
  dbus::Bus *b = bus();
  DBusMessage *m = g().up && b ? call(kSettingsIface, "SetMany") : nullptr;
  if (!m) {
    fallback(writes, mode);
    return;
  }
  {
    dbus::Writer w(m);
    {
      dbus::Writer sets(w.iter(), DBUS_TYPE_ARRAY, "(ssv)");
      for (const auto &c : writes) {
        if (c.reset) continue;
        dbus::Writer item(sets.iter(), DBUS_TYPE_STRUCT, nullptr);
        item.str(c.section).str(c.key).variant_str(c.value);
      }
    }
    {
      dbus::Writer resets(w.iter(), DBUS_TYPE_ARRAY, "(ss)");
      for (const auto &c : writes) {
        if (!c.reset) continue;
        dbus::Writer item(resets.iter(), DBUS_TYPE_STRUCT, nullptr);
        item.str(c.section).str(c.key);
      }
    }
    dbus::Writer options(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
    if (mode == Mode::APPEARANCE) {
      dbus::Writer e(options.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      e.str("appearance").variant_bool(true);
    }
  }
  b->call_async_any(m, [writes, mode](DBusMessage *reply) {
    if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN) return;
    const char *error = reply ? dbus_message_get_error_name(reply) : nullptr;
    if (error && starts_with(error, kErrorPrefix)) {
      // Checked by the schema and refused: nothing is written.
      std::string why;
      auto args = dbus::read_args(reply);
      if (!args.empty()) why = args[0].s;
      fprintf(stderr, "sde: sde-core refused the settings: %s\n", why.c_str());
      return;
    }
    // sde-core went away or does not answer: the file, as before.
    fallback(writes, mode);
  }, 5000);
}

void set(const std::string &section, const std::string &key, const std::string &value) {
  write({{section, key, value, false}});
}

void reset(const std::string &section, const std::string &key) { write({{section, key, "", true}}); }

std::vector<Write> diff(const Config &before, const Config &after) {
  std::vector<Write> out;
  sde_config_user_diff(before.rust(), after.rust(),
                       [](void *ud, const char *section, const char *key, const char *value) {
                         auto *list = static_cast<std::vector<Write> *>(ud);
                         list->push_back({section, key, value ? value : "", value == nullptr});
                       },
                       &out);
  return out;
}

int watch_settings(std::function<void(const std::vector<Key> &)> fn) {
  ensure();
  install_x_handler();
  State &s = g();
  s.watchers.push_back({s.next_id, false, std::move(fn), nullptr});
  return s.next_id++;
}

int watch_look(std::function<void()> fn) {
  ensure();
  install_x_handler();
  State &s = g();
  s.watchers.push_back({s.next_id, true, nullptr, std::move(fn)});
  return s.next_id++;
}

void unwatch(int id) {
  auto &list = g().watchers;
  for (auto it = list.begin(); it != list.end(); ++it)
    if (it->id == id) {
      list.erase(it);
      return;
    }
}

bool touches_look(const std::vector<Key> &keys) {
  if (keys.empty()) return true;
  for (const auto &k : keys)
    if (k.first == "appearance" || k.first == "aero" || k.first == "compositor") return true;
  return false;
}

bool single_instance(const std::string &app_id, const std::vector<std::string> &args,
                     std::function<void(const std::vector<std::string> &)> activated) {
  if (!wait_available(300)) return true;
  dbus::Bus *b = bus();
  auto try_register = [&]() {
    DBusMessage *m = call(kAppsIface, "Register");
    dbus::Writer(m).str(app_id);
    // No answer: start on our own rather than not at all.
    return reply_bool(b->call(m, 1000), true);
  };
  bool primary = try_register();
  if (!primary) {
    DBusMessage *m = call(kAppsIface, "Activate");
    dbus::Writer(m).str(app_id).strings(args);
    if (reply_bool(b->call(m, 3000), false)) return false;
    // The other instance did not take them (it hangs or just ended): sde-core
    // let it go, so this one is the instance now.
    primary = try_register();
    if (!primary) return true;
  }
  State &s = g();
  s.instances.push_back({app_id, std::move(activated)});
  if (!s.exported) {
    s.exported = true;
    b->export_object(kInstancePath, instance_method);
  }
  return true;
}

void report(const std::string &kind, long pid, const std::string &name, const std::string &id,
            const std::string &detail) {
  if (!available()) return;
  dbus::Bus *b = bus();
  DBusMessage *m = call(kAppsIface, "Report");
  if (!b || !m) return;
  dbus::Writer(m).str(kind).u32(uint32_t(pid > 0 ? pid : 0)).str(name).str(id).str(detail);
  dbus_message_set_no_reply(m, TRUE);
  b->send(m);
}

void reset_for_tests() {
  State &s = g();
  s.asked = s.answered = s.up = false;
  s.owner.clear();
}

}  // namespace core
}  // namespace sde
