// SPDX-License-Identifier: GPL-3.0-or-later
#include "dbus.h"

#include <FL/Fl.H>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
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
  const Value &v = inner();
  if (!v.data.empty() || v.items.empty()) return std::vector<uint8_t>(v.data.begin(), v.data.end());
  std::vector<uint8_t> out;
  out.reserve(v.items.size());
  for (auto &b : v.items) out.push_back(uint8_t(b.i));
  return out;
}

bool valid_utf8(const std::string &s) {
  return s.find('\0') == std::string::npos && dbus_validate_utf8(s.c_str(), nullptr);
}

bool valid_path(const std::string &path) { return valid_utf8(path) && dbus_validate_path(path.c_str(), nullptr); }

bool valid_bus_name(const std::string &name) {
  return valid_utf8(name) && dbus_validate_bus_name(name.c_str(), nullptr);
}

bool sandboxed_process(long pid, const std::string &proc) {
  if (pid <= 0) return true;
  std::string base = proc + "/" + std::to_string(pid);
  struct stat st;
  if (lstat((base + "/root/.flatpak-info").c_str(), &st) == 0) return true;
  if (errno != ENOENT) return true;  // cannot look into it: do not trust it
  if (lstat(base.c_str(), &st) != 0) return true;  // gone already
  FILE *f = fopen((base + "/attr/current").c_str(), "re");
  if (f) {
    char label[64] = "";
    size_t n = fread(label, 1, sizeof label - 1, f);
    fclose(f);
    label[n] = 0;
    if (strncmp(label, "snap.", 5) == 0) return true;
  }
  return false;
}

std::string peer_program(long pid, const std::string &proc) {
  if (pid <= 0) return "";
  std::string base = proc + "/" + std::to_string(pid);
  auto read = [](const std::string &path) {
    std::string text;
    if (FILE *f = fopen(path.c_str(), "re")) {
      char buf[4096];
      size_t n = fread(buf, 1, sizeof buf, f);
      fclose(f);
      text.assign(buf, n);
    }
    return text;
  };
  // A Flatpak application: the ID in its sandbox's /.flatpak-info.
  std::string info = read(base + "/root/.flatpak-info");
  for (size_t at = info.find("\nname="); at != std::string::npos;) {
    size_t end = info.find('\n', at + 6);
    std::string id = info.substr(at + 6, end == std::string::npos ? std::string::npos : end - at - 6);
    while (!id.empty() && (id.back() == ' ' || id.back() == '\r')) id.pop_back();
    if (!id.empty()) return "flatpak:" + id;
    break;
  }
  std::vector<std::string> argv;
  std::string cmdline = read(base + "/cmdline"), cur;
  for (char c : cmdline) {
    if (c) cur += c;
    else argv.push_back(cur), cur.clear();
  }
  if (!cur.empty()) argv.push_back(cur);
  char buf[4096];
  ssize_t n = readlink((base + "/exe").c_str(), buf, sizeof buf - 1);
  std::string exe = n > 0 ? std::string(buf, size_t(n)) : argv.empty() ? std::string() : argv[0];
  const std::string deleted = " (deleted)";  // replaced by an upgrade while it runs
  if (exe.size() > deleted.size() && exe.compare(exe.size() - deleted.size(), deleted.size(), deleted) == 0)
    exe.resize(exe.size() - deleted.size());
  auto base_name = [](const std::string &p) { return p.substr(p.rfind('/') == std::string::npos ? 0 : p.rfind('/') + 1); };
  std::string name = base_name(exe);
  // Interpreters run the script named after their options.
  static const char *const interpreters[] = {"python", "perl", "ruby", "node", "nodejs", "gjs", "lua", "java", "sh", "bash"};
  bool interpreted = false;
  for (const char *i : interpreters)
    interpreted = interpreted || name == i || (strcmp(i, "python") == 0 && name.compare(0, 6, "python") == 0);
  for (size_t i = 1; interpreted && i < argv.size(); ++i)
    if (!argv[i].empty() && argv[i][0] != '-') {
      name = base_name(argv[i]);
      size_t dot = name.rfind('.');
      if (dot != std::string::npos && dot > 0) name.resize(dot);
      break;
    }
  return name;
}

std::string sanitize_utf8(const std::string &s) {
  if (valid_utf8(s)) return s;
  std::string out;
  out.reserve(s.size());
  const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
  size_t n = s.size(), i = 0;
  while (i < n) {
    unsigned char c = p[i];
    size_t len = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3
                 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    bool ok = len > 0 && i + len <= n && c != 0;
    for (size_t k = 1; ok && k < len; ++k) ok = (p[i + k] & 0xC0) == 0x80;
    if (ok && len == 3) {
      unsigned cp = (c & 0x0Fu) << 12 | (p[i + 1] & 0x3Fu) << 6 | (p[i + 2] & 0x3Fu);
      ok = cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF);
    } else if (ok && len == 4) {
      unsigned cp = (c & 0x07u) << 18 | (p[i + 1] & 0x3Fu) << 12 | (p[i + 2] & 0x3Fu) << 6 |
                    (p[i + 3] & 0x3Fu);
      ok = cp >= 0x10000 && cp <= 0x10FFFF;
    }
    if (ok) {
      out.append(s, i, len);
      i += len;
    } else {
      out += "\xEF\xBF\xBD";  // U+FFFD
      ++i;
    }
  }
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
    case DBUS_TYPE_INT32: {
      int32_t x;
      dbus_message_iter_get_basic(it, &x);
      v.i = x;
      break;
    }
    case DBUS_TYPE_UNIX_FD: {
      // libdbus hands out a duplicate that the reader must close; nothing
      // read through read_args() keeps descriptors (logind's inhibitor fd
      // is taken with dbus_message_get_args), so a client sending fds
      // cannot make SDE programs run out of them.
      int x = -1;
      dbus_message_iter_get_basic(it, &x);
      if (x >= 0) close(x);
      v.i = -1;
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
          if (data && n > 0) v.data.assign(reinterpret_cast<const char *>(data), size_t(n));
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

Writer::Writer(DBusMessage *msg) : ok_(msg != nullptr) {
  if (ok_) dbus_message_iter_init_append(msg, &it_);
}

Writer::Writer(DBusMessageIter *parent, int container, const char *signature) : parent_(parent) {
  ok_ = parent && dbus_message_iter_open_container(parent, container, signature, &it_);
}

Writer::~Writer() {
  if (ok_ && parent_) dbus_message_iter_close_container(parent_, &it_);
}

Writer &Writer::str(const std::string &v) {
  if (!ok_) return *this;
  std::string safe = sanitize_utf8(v.find('\0') == std::string::npos ? v : v.substr(0, v.find('\0')));
  const char *c = safe.c_str();
  dbus_message_iter_append_basic(&it_, DBUS_TYPE_STRING, &c);
  return *this;
}
Writer &Writer::path(const std::string &v) {
  if (!ok_) return *this;
  const char *c = valid_path(v) ? v.c_str() : "/";
  dbus_message_iter_append_basic(&it_, DBUS_TYPE_OBJECT_PATH, &c);
  return *this;
}
Writer &Writer::u32(uint32_t v) {
  if (ok_) dbus_message_iter_append_basic(&it_, DBUS_TYPE_UINT32, &v);
  return *this;
}
Writer &Writer::i32(int32_t v) {
  if (ok_) dbus_message_iter_append_basic(&it_, DBUS_TYPE_INT32, &v);
  return *this;
}
Writer &Writer::boolean(bool v) {
  dbus_bool_t b = v;
  if (ok_) dbus_message_iter_append_basic(&it_, DBUS_TYPE_BOOLEAN, &b);
  return *this;
}
Writer &Writer::f64(double v) {
  if (ok_) dbus_message_iter_append_basic(&it_, DBUS_TYPE_DOUBLE, &v);
  return *this;
}
Writer &Writer::strings(const std::vector<std::string> &v) {
  Writer a(iter(), DBUS_TYPE_ARRAY, "s");
  for (auto &s : v) a.str(s);
  return *this;
}
Writer &Writer::variant_str(const std::string &v) {
  Writer x(iter(), DBUS_TYPE_VARIANT, "s");
  x.str(v);
  return *this;
}
Writer &Writer::variant_u32(uint32_t v) {
  Writer x(iter(), DBUS_TYPE_VARIANT, "u");
  x.u32(v);
  return *this;
}
Writer &Writer::variant_i32(int32_t v) {
  Writer x(iter(), DBUS_TYPE_VARIANT, "i");
  x.i32(v);
  return *this;
}
Writer &Writer::variant_bool(bool v) {
  Writer x(iter(), DBUS_TYPE_VARIANT, "b");
  x.boolean(v);
  return *this;
}
Writer &Writer::variant_strings(const std::vector<std::string> &v) {
  Writer x(iter(), DBUS_TYPE_VARIANT, "as");
  x.strings(v);
  return *this;
}
Writer &Writer::variant_path(const std::string &v) {
  Writer x(iter(), DBUS_TYPE_VARIANT, "o");
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
void pending_any(DBusPendingCall *pending, void *data) {
  auto *fn = static_cast<Bus::ReplyFn *>(data);
  DBusMessage *reply = dbus_pending_call_steal_reply(pending);
  (*fn)(reply);
  if (reply) dbus_message_unref(reply);
  dbus_pending_call_unref(pending);
}
}  // namespace

void Bus::call_async_any(DBusMessage *msg, ReplyFn fn, int timeout_ms) {
  if (!msg) return;
  DBusPendingCall *pending = nullptr;
  if (!dbus_connection_send_with_reply(conn_, msg, &pending, timeout_ms) || !pending) {
    dbus_message_unref(msg);
    if (fn) fn(nullptr);
    return;
  }
  dbus_message_unref(msg);
  auto *copy = new ReplyFn(fn ? std::move(fn) : ReplyFn([](DBusMessage *) {}));
  dbus_pending_call_set_notify(pending, pending_any, copy, free_reply_fn);
  dbus_connection_flush(conn_);
}

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
  if (!call) return nullptr;
  return dbus_message_new_error(call, name, sanitize_utf8(text).c_str());
}

namespace {
bool valid_interface(const std::string &s) {
  return valid_utf8(s) && dbus_validate_interface(s.c_str(), nullptr);
}
bool valid_member(const std::string &s) { return valid_utf8(s) && dbus_validate_member(s.c_str(), nullptr); }
}  // namespace

DBusMessage *method(const std::string &dest, const std::string &path, const std::string &iface,
                    const std::string &member) {
  if ((!dest.empty() && !valid_bus_name(dest)) || !valid_path(path) ||
      (!iface.empty() && !valid_interface(iface)) || !valid_member(member)) {
    fprintf(stderr, "sde: not calling %s on %s %s: invalid D-Bus name\n",
            sanitize_utf8(member).c_str(), sanitize_utf8(dest).c_str(), sanitize_utf8(path).c_str());
    return nullptr;
  }
  return dbus_message_new_method_call(dest.empty() ? nullptr : dest.c_str(), path.c_str(),
                                      iface.empty() ? nullptr : iface.c_str(), member.c_str());
}

DBusMessage *signal(const std::string &path, const std::string &iface, const std::string &member) {
  if (!valid_path(path) || !valid_interface(iface) || !valid_member(member)) return nullptr;
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
