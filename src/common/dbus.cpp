// SPDX-License-Identifier: GPL-3.0-or-later
#include "dbus.h"

#include <FL/Fl.H>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>

namespace sde {
namespace dbus {

// ------------------------------------------------------------------ values

const Value &Value::inner() const {
  const Value *v = this;
  while (v->type == DBUS_TYPE_VARIANT && !v->items.empty()) v = &v->items[0];
  return *v;
}

int64_t Value::num() const {
  const Value &v = inner();
  if (v.type == 'd') return int64_t(v.d);
  if (v.type == 'b') return v.b;
  return v.i;
}

const Value *Value::get(const std::string &key) const {
  const Value &v = inner();
  if (v.type != DBUS_TYPE_ARRAY) return nullptr;
  for (auto &e : v.items)
    if (e.type == DBUS_TYPE_DICT_ENTRY && e.items.size() == 2 && e.items[0].s == key)
      return &e.items[1].inner();
  return nullptr;
}

std::vector<uint8_t> Value::bytes() const {
  std::vector<uint8_t> out;
  const Value &v = inner();
  out.reserve(v.items.size());
  for (auto &b : v.items) out.push_back(uint8_t(b.i));
  return out;
}

static Value read_value(DBusMessageIter *it) {
  Value v;
  v.type = dbus_message_iter_get_arg_type(it);
  switch (v.type) {
    case DBUS_TYPE_BOOLEAN: {
      dbus_bool_t b;
      dbus_message_iter_get_basic(it, &b);
      v.b = b;
      break;
    }
    case DBUS_TYPE_BYTE: {
      uint8_t x;
      dbus_message_iter_get_basic(it, &x);
      v.i = x;
      break;
    }
    case DBUS_TYPE_INT16: {
      int16_t x;
      dbus_message_iter_get_basic(it, &x);
      v.i = x;
      break;
    }
    case DBUS_TYPE_UINT16: {
      uint16_t x;
      dbus_message_iter_get_basic(it, &x);
      v.i = x;
      break;
    }
    case DBUS_TYPE_INT32:
    case DBUS_TYPE_UNIX_FD: {
      int32_t x;
      dbus_message_iter_get_basic(it, &x);
      v.i = x;
      break;
    }
    case DBUS_TYPE_UINT32: {
      uint32_t x;
      dbus_message_iter_get_basic(it, &x);
      v.i = x;
      break;
    }
    case DBUS_TYPE_INT64: {
      dbus_int64_t x;
      dbus_message_iter_get_basic(it, &x);
      v.i = x;
      break;
    }
    case DBUS_TYPE_UINT64: {
      dbus_uint64_t x;
      dbus_message_iter_get_basic(it, &x);
      v.i = int64_t(x);
      break;
    }
    case DBUS_TYPE_DOUBLE:
      dbus_message_iter_get_basic(it, &v.d);
      break;
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH:
    case DBUS_TYPE_SIGNATURE: {
      const char *s = nullptr;
      dbus_message_iter_get_basic(it, &s);
      v.s = s ? s : "";
      break;
    }
    case DBUS_TYPE_ARRAY:
    case DBUS_TYPE_STRUCT:
    case DBUS_TYPE_DICT_ENTRY:
    case DBUS_TYPE_VARIANT: {
      DBusMessageIter sub;
      dbus_message_iter_recurse(it, &sub);
      if (v.type == DBUS_TYPE_ARRAY) {
        char *sig = dbus_message_iter_get_signature(&sub);
        if (sig) {
          v.element_signature = sig;
          dbus_free(sig);
        }
        // Byte arrays can be large (icons); read them in one go.
        if (dbus_message_iter_get_element_type(it) == DBUS_TYPE_BYTE) {
          const uint8_t *data = nullptr;
          int n = 0;
          dbus_message_iter_get_fixed_array(&sub, &data, &n);
          v.items.resize(size_t(n));
          for (int k = 0; k < n; ++k) {
            v.items[size_t(k)].type = DBUS_TYPE_BYTE;
            v.items[size_t(k)].i = data[k];
          }
          break;
        }
      }
      while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
        v.items.push_back(read_value(&sub));
        dbus_message_iter_next(&sub);
      }
      break;
    }
    default:
      break;
  }
  return v;
}

std::vector<Value> read_args(DBusMessage *msg) {
  std::vector<Value> out;
  DBusMessageIter it;
  if (!msg || !dbus_message_iter_init(msg, &it)) return out;
  while (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
    out.push_back(read_value(&it));
    dbus_message_iter_next(&it);
  }
  return out;
}

// ------------------------------------------------------------------ writer

Writer::Writer(DBusMessage *msg) { dbus_message_iter_init_append(msg, &it_); }

Writer::Writer(DBusMessageIter *parent, int container, const char *signature) : parent_(parent) {
  dbus_message_iter_open_container(parent, container, signature, &it_);
}

Writer::~Writer() {
  if (parent_) dbus_message_iter_close_container(parent_, &it_);
}

Writer &Writer::str(const std::string &v) {
  const char *c = v.c_str();
  dbus_message_iter_append_basic(&it_, DBUS_TYPE_STRING, &c);
  return *this;
}
Writer &Writer::path(const std::string &v) {
  const char *c = v.c_str();
  dbus_message_iter_append_basic(&it_, DBUS_TYPE_OBJECT_PATH, &c);
  return *this;
}
Writer &Writer::u32(uint32_t v) {
  dbus_message_iter_append_basic(&it_, DBUS_TYPE_UINT32, &v);
  return *this;
}
Writer &Writer::i32(int32_t v) {
  dbus_message_iter_append_basic(&it_, DBUS_TYPE_INT32, &v);
  return *this;
}
Writer &Writer::boolean(bool v) {
  dbus_bool_t b = v;
  dbus_message_iter_append_basic(&it_, DBUS_TYPE_BOOLEAN, &b);
  return *this;
}
Writer &Writer::f64(double v) {
  dbus_message_iter_append_basic(&it_, DBUS_TYPE_DOUBLE, &v);
  return *this;
}
Writer &Writer::strings(const std::vector<std::string> &v) {
  Writer a(&it_, DBUS_TYPE_ARRAY, "s");
  for (auto &s : v) a.str(s);
  return *this;
}
Writer &Writer::variant_str(const std::string &v) {
  Writer x(&it_, DBUS_TYPE_VARIANT, "s");
  x.str(v);
  return *this;
}
Writer &Writer::variant_u32(uint32_t v) {
  Writer x(&it_, DBUS_TYPE_VARIANT, "u");
  x.u32(v);
  return *this;
}
Writer &Writer::variant_i32(int32_t v) {
  Writer x(&it_, DBUS_TYPE_VARIANT, "i");
  x.i32(v);
  return *this;
}
Writer &Writer::variant_bool(bool v) {
  Writer x(&it_, DBUS_TYPE_VARIANT, "b");
  x.boolean(v);
  return *this;
}
Writer &Writer::variant_strings(const std::vector<std::string> &v) {
  Writer x(&it_, DBUS_TYPE_VARIANT, "as");
  x.strings(v);
  return *this;
}
Writer &Writer::variant_path(const std::string &v) {
  Writer x(&it_, DBUS_TYPE_VARIANT, "o");
  x.path(v);
  return *this;
}

// ------------------------------------------------------- FLTK integration

namespace {

struct FdEntry {
  std::vector<DBusWatch *> watches;
};
std::map<int, FdEntry> g_fds;
// libdbus frees watches and timeouts right after removing them, possibly
// while one of their callbacks runs; only live ones may be touched.
std::set<DBusWatch *> g_watches;
std::set<DBusTimeout *> g_timeouts;

void on_fd(int fd, void *) {
  auto it = g_fds.find(fd);
  if (it == g_fds.end()) return;
  // Copy: handling a watch can add or remove (and free) watches.
  auto watches = it->second.watches;
  for (DBusWatch *w : watches) {
    if (!g_watches.count(w) || !dbus_watch_get_enabled(w)) continue;
    unsigned flags = dbus_watch_get_flags(w);
    dbus_watch_handle(w, flags & (DBUS_WATCH_READABLE | DBUS_WATCH_WRITABLE));
  }
}

void update_fd(int fd) {
  auto it = g_fds.find(fd);
  int events = 0;
  if (it != g_fds.end()) {
    for (DBusWatch *w : it->second.watches) {
      if (!dbus_watch_get_enabled(w)) continue;
      unsigned flags = dbus_watch_get_flags(w);
      if (flags & DBUS_WATCH_READABLE) events |= FL_READ;
      if (flags & DBUS_WATCH_WRITABLE) events |= FL_WRITE;
    }
  }
  Fl::remove_fd(fd);
  if (events) Fl::add_fd(fd, events, on_fd, nullptr);
}

dbus_bool_t add_watch(DBusWatch *w, void *) {
  int fd = dbus_watch_get_unix_fd(w);
  g_watches.insert(w);
  g_fds[fd].watches.push_back(w);
  update_fd(fd);
  return TRUE;
}

void remove_watch(DBusWatch *w, void *) {
  g_watches.erase(w);
  int fd = dbus_watch_get_unix_fd(w);
  auto it = g_fds.find(fd);
  if (it == g_fds.end()) return;
  auto &v = it->second.watches;
  v.erase(std::remove(v.begin(), v.end(), w), v.end());
  if (v.empty()) g_fds.erase(it);
  update_fd(fd);
}

void toggle_watch(DBusWatch *w, void *) { update_fd(dbus_watch_get_unix_fd(w)); }

// When a pending call times out, libdbus removes the timeout from inside
// dbus_timeout_handle() and frees it soon after; re-arming it then crashed
// the panel ~5 s after login when org.bluez never answered (alpha12 bug).
void on_timeout(void *data) {
  auto *t = static_cast<DBusTimeout *>(data);
  if (!g_timeouts.count(t)) return;
  dbus_timeout_handle(t);
  if (g_timeouts.count(t) && dbus_timeout_get_enabled(t) && !Fl::has_timeout(on_timeout, t))
    Fl::repeat_timeout(dbus_timeout_get_interval(t) / 1000.0, on_timeout, t);
}

dbus_bool_t add_timeout(DBusTimeout *t, void *) {
  g_timeouts.insert(t);
  if (dbus_timeout_get_enabled(t))
    Fl::add_timeout(dbus_timeout_get_interval(t) / 1000.0, on_timeout, t);
  return TRUE;
}

void remove_timeout(DBusTimeout *t, void *) {
  g_timeouts.erase(t);
  Fl::remove_timeout(on_timeout, t);
}

void toggle_timeout(DBusTimeout *t, void *d) {
  remove_timeout(t, d);
  add_timeout(t, d);
}

}  // namespace

Bus::Bus(DBusConnection *c) : conn_(c) {}

Bus *Bus::connect(DBusBusType type) {
  DBusError err;
  dbus_error_init(&err);
  DBusConnection *c = dbus_bus_get_private(type, &err);
  if (!c) {
    fprintf(stderr, "sde: cannot connect to the D-Bus %s bus: %s\n",
            type == DBUS_BUS_SESSION ? "session" : "system", err.message ? err.message : "?");
    dbus_error_free(&err);
    return nullptr;
  }
  dbus_connection_set_exit_on_disconnect(c, FALSE);
  Bus *bus = new Bus(c);
  dbus_connection_set_watch_functions(c, add_watch, remove_watch, toggle_watch, nullptr, nullptr);
  dbus_connection_set_timeout_functions(c, add_timeout, remove_timeout, toggle_timeout, nullptr,
                                        nullptr);
  dbus_connection_set_dispatch_status_function(c, schedule_dispatch, bus, nullptr);
  dbus_connection_add_filter(c, filter, bus, nullptr);
  schedule_dispatch(c, dbus_connection_get_dispatch_status(c), bus);
  return bus;
}

Bus *Bus::session() {
  static Bus *bus = connect(DBUS_BUS_SESSION);
  return bus;
}

Bus *Bus::system() {
  static Bus *bus = connect(DBUS_BUS_SYSTEM);
  return bus;
}

void Bus::schedule_dispatch(DBusConnection *, DBusDispatchStatus status, void *data) {
  if (status == DBUS_DISPATCH_DATA_REMAINS) Fl::add_timeout(0, dispatch, data);
}

void Bus::dispatch(void *data) {
  auto *bus = static_cast<Bus *>(data);
  while (dbus_connection_dispatch(bus->conn_) == DBUS_DISPATCH_DATA_REMAINS) {
  }
}

std::string Bus::unique_name() const {
  const char *n = dbus_bus_get_unique_name(conn_);
  return n ? n : "";
}

bool Bus::request_name(const std::string &name, bool allow_replacement) {
  DBusError err;
  dbus_error_init(&err);
  unsigned flags = DBUS_NAME_FLAG_DO_NOT_QUEUE;
  if (allow_replacement) flags |= DBUS_NAME_FLAG_ALLOW_REPLACEMENT;
  int r = dbus_bus_request_name(conn_, name.c_str(), flags, &err);
  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "sde: cannot own %s: %s\n", name.c_str(), err.message);
    dbus_error_free(&err);
    return false;
  }
  return r == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER || r == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
}

bool Bus::name_has_owner(const std::string &name) {
  DBusError err;
  dbus_error_init(&err);
  bool r = dbus_bus_name_has_owner(conn_, name.c_str(), &err);
  dbus_error_free(&err);
  return r;
}

DBusHandlerResult Bus::object_message(DBusConnection *, DBusMessage *msg, void *data) {
  auto *fn = static_cast<MethodFn *>(data);
  return (*fn)(msg);
}

void Bus::export_object(const std::string &path, MethodFn fn) {
  auto &slot = objects_[path];
  bool fresh = !slot;
  slot = std::move(fn);
  if (!fresh) return;
  static DBusObjectPathVTable vtable = {nullptr, object_message, nullptr, nullptr, nullptr, nullptr};
  dbus_connection_register_object_path(conn_, path.c_str(), &vtable, &slot);
}

void Bus::add_signal_handler(const std::string &rule, std::function<bool(DBusMessage *)> filter_fn,
                             SignalFn fn) {
  dbus_bus_add_match(conn_, rule.c_str(), nullptr);
  signals_.push_back({std::move(filter_fn), std::move(fn)});
}

DBusHandlerResult Bus::filter(DBusConnection *, DBusMessage *msg, void *data) {
  auto *bus = static_cast<Bus *>(data);
  if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  // Copy the list: handlers may add new ones.
  auto watches = bus->signals_;
  for (auto &w : watches)
    if (!w.filter || w.filter(msg)) w.fn(msg);
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void Bus::send(DBusMessage *msg) {
  if (!msg) return;
  dbus_connection_send(conn_, msg, nullptr);
  dbus_message_unref(msg);
  dbus_connection_flush(conn_);
}

namespace {
void pending_done(DBusPendingCall *pending, void *data) {
  auto *fn = static_cast<Bus::ReplyFn *>(data);
  DBusMessage *reply = dbus_pending_call_steal_reply(pending);
  bool ok = reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN;
  (*fn)(ok ? reply : nullptr);
  if (reply) dbus_message_unref(reply);
  dbus_pending_call_unref(pending);
}
void free_reply_fn(void *data) { delete static_cast<Bus::ReplyFn *>(data); }
}  // namespace

void Bus::call_async(DBusMessage *msg, ReplyFn fn, int timeout_ms) {
  if (!msg) return;
  DBusPendingCall *pending = nullptr;
  if (!dbus_connection_send_with_reply(conn_, msg, &pending, timeout_ms) || !pending) {
    dbus_message_unref(msg);
    if (fn) fn(nullptr);
    return;
  }
  dbus_message_unref(msg);
  auto *copy = new ReplyFn(fn ? std::move(fn) : ReplyFn([](DBusMessage *) {}));
  dbus_pending_call_set_notify(pending, pending_done, copy, free_reply_fn);
  dbus_connection_flush(conn_);
}

DBusMessage *Bus::call(DBusMessage *msg, int timeout_ms) {
  if (!msg) return nullptr;
  DBusError err;
  dbus_error_init(&err);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn_, msg, timeout_ms, &err);
  dbus_message_unref(msg);
  dbus_error_free(&err);
  return reply;
}

DBusMessage *Bus::error(DBusMessage *call, const char *name, const std::string &text) {
  return dbus_message_new_error(call, name, text.c_str());
}

DBusMessage *method(const std::string &dest, const std::string &path, const std::string &iface,
                    const std::string &member) {
  return dbus_message_new_method_call(dest.c_str(), path.c_str(), iface.c_str(), member.c_str());
}

DBusMessage *signal(const std::string &path, const std::string &iface, const std::string &member) {
  return dbus_message_new_signal(path.c_str(), iface.c_str(), member.c_str());
}

void get_all_properties(Bus *bus, const std::string &dest, const std::string &path,
                        const std::string &iface, std::function<void(const Value &)> fn) {
  DBusMessage *m = method(dest, path, "org.freedesktop.DBus.Properties", "GetAll");
  Writer(m).str(iface);
  bus->call_async(m, [fn](DBusMessage *reply) {
    auto args = read_args(reply);
    fn(args.empty() ? Value() : args[0]);
  });
}

}  // namespace dbus
}  // namespace sde
