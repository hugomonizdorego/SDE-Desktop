// SPDX-License-Identifier: GPL-3.0-or-later
// Small C++ layer over libdbus-1, driven by the FLTK event loop.
//
// Used by sde-notifyd (org.freedesktop.Notifications), the panel's
// StatusNotifierItem tray and quick settings (logind brightness).
#pragma once

#include <dbus/dbus.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sde {
namespace dbus {

// A decoded D-Bus value. |type| is the D-Bus type code ('s', 'u', 'a', ...).
struct Value {
  int type = DBUS_TYPE_INVALID;
  bool b = false;
  int64_t i = 0;       // y n q i u x t, and h
  double d = 0;
  std::string s;       // s o g
  std::vector<Value> items;  // a ( { v: children; for 'v' items[0] is the content
  std::string element_signature;  // for arrays
  // The contents of an 'ay' array (icons, UDisks paths), kept as one block
  // instead of one Value per byte: a client's 1 MB pixmap must not cost
  // 100+ MB here. |items| stays empty for byte arrays read by read_args().
  std::string data;

  bool valid() const { return type != DBUS_TYPE_INVALID; }
  bool is_string() const { return type == 's' || type == 'o' || type == 'g'; }
  // Unwraps variants.
  const Value &inner() const;
  std::string str() const { return inner().s; }
  int64_t num() const;
  bool boolean() const { return inner().type == 'b' ? inner().b : num() != 0; }
  // For a{sv}/a{s*}: the value stored under |key|, or nullptr.
  const Value *get(const std::string &key) const;
  // Bytes of an 'ay' array.
  std::vector<uint8_t> bytes() const;
};

std::vector<Value> read_args(DBusMessage *msg);

// True when |s| can be sent as a D-Bus string (valid UTF-8, no NUL), an
// object path, or a bus name. libdbus aborts the whole program when it is
// handed anything else, so names and paths that come from other programs
// (tray items, menus) are checked with these first.
bool valid_utf8(const std::string &s);
bool valid_path(const std::string &path);
bool valid_bus_name(const std::string &name);
// |s| with invalid UTF-8 sequences replaced by U+FFFD.
std::string sanitize_utf8(const std::string &s);

// True when process |pid| (a D-Bus peer, from GetConnectionUnixProcessID)
// runs in an application sandbox: Flatpak (/.flatpak-info in its root; the
// Flatpak D-Bus proxy has it too) or Snap (AppArmor label "snap.*"). Also
// true when that cannot be checked. |proc| is for the tests.
bool sandboxed_process(long pid, const std::string &proc = "/proc");
// The program process |pid| runs, to tell which application a D-Bus peer
// is (0.2 Alpha 3 live badges): "flatpak:<ID>" in a Flatpak sandbox, else
// the name of its executable, or of the script an interpreter runs; "" when
// it cannot be read.
std::string peer_program(long pid, const std::string &proc = "/proc");

// Appending arguments. A Writer for a null message (see method()) ignores
// everything; strings that are not valid UTF-8 are sent sanitized and an
// invalid object path is sent as "/", so that bad data from elsewhere can
// never make libdbus abort the program.
class Writer {
 public:
  explicit Writer(DBusMessage *msg);
  Writer(DBusMessageIter *parent, int container, const char *signature);
  ~Writer();  // closes the container
  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;

  Writer &str(const std::string &v);
  Writer &path(const std::string &v);
  Writer &u32(uint32_t v);
  Writer &i32(int32_t v);
  Writer &boolean(bool v);
  Writer &f64(double v);
  Writer &strings(const std::vector<std::string> &v);
  // Variant holding one basic value.
  Writer &variant_str(const std::string &v);
  Writer &variant_u32(uint32_t v);
  Writer &variant_i32(int32_t v);
  Writer &variant_bool(bool v);
  Writer &variant_strings(const std::vector<std::string> &v);
  Writer &variant_path(const std::string &v);

  // The iterator to append to, or nullptr when this Writer is not usable
  // (a nested Writer constructed from nullptr ignores everything as well).
  DBusMessageIter *iter() { return ok_ ? &it_ : nullptr; }

 private:
  DBusMessageIter it_;
  DBusMessageIter *parent_ = nullptr;
  bool ok_ = false;
};

class Bus {
 public:
  using MethodFn = std::function<DBusHandlerResult(DBusMessage *)>;
  using SignalFn = std::function<void(DBusMessage *)>;
  using ReplyFn = std::function<void(DBusMessage *reply)>;  // nullptr reply on error

  // The session bus, connected on first use and integrated with FLTK.
  // Returns nullptr if no session bus is available.
  static Bus *session();
  // The system bus (for logind), same integration.
  static Bus *system();

  DBusConnection *conn() const { return conn_; }
  std::string unique_name() const;

  // Requests a well-known name. Returns true when this connection owns it.
  bool request_name(const std::string &name, bool allow_replacement = false);
  bool name_has_owner(const std::string &name);

  // Handles method calls to |path| (every interface). Return
  // DBUS_HANDLER_RESULT_NOT_YET_HANDLED for unknown methods.
  void export_object(const std::string &path, MethodFn fn);
  // Receives signals matching |rule| ("type='signal',interface='...'").
  // |fn| is called for every signal accepted by |filter|.
  void add_signal_handler(const std::string &rule, std::function<bool(DBusMessage *)> filter,
                          SignalFn fn);

  void send(DBusMessage *msg);  // takes ownership
  // Asynchronous call; |fn| runs when the reply arrives (or with nullptr on
  // error/timeout). Takes ownership of |msg|.
  void call_async(DBusMessage *msg, ReplyFn fn, int timeout_ms = 5000);
  // The same, but |fn| also receives error replies (check the message type
  // and dbus_message_get_error_name); nullptr only when no reply came.
  void call_async_any(DBusMessage *msg, ReplyFn fn, int timeout_ms = 5000);
  // Blocking call with a short timeout. Caller unrefs the reply.
  DBusMessage *call(DBusMessage *msg, int timeout_ms = 2000);

  // Replies.
  static DBusMessage *error(DBusMessage *call, const char *name, const std::string &text);

 private:
  explicit Bus(DBusConnection *c);
  static Bus *connect(DBusBusType type);
  static DBusHandlerResult filter(DBusConnection *, DBusMessage *, void *data);
  static DBusHandlerResult object_message(DBusConnection *, DBusMessage *, void *data);
  static void schedule_dispatch(DBusConnection *, DBusDispatchStatus, void *data);
  static void dispatch(void *data);

  DBusConnection *conn_;
  std::map<std::string, MethodFn> objects_;
  struct Watch {
    std::function<bool(DBusMessage *)> filter;
    SignalFn fn;
  };
  std::vector<Watch> signals_;
};

// Convenience constructors. They return nullptr (which Writer and Bus
// accept and ignore) when a name or path is not valid D-Bus syntax.
DBusMessage *method(const std::string &dest, const std::string &path, const std::string &iface,
                    const std::string &member);
DBusMessage *signal(const std::string &path, const std::string &iface, const std::string &member);

// Calls org.freedesktop.DBus.Properties.GetAll asynchronously; |fn| receives
// the a{sv} dictionary (an invalid Value on error).
void get_all_properties(Bus *bus, const std::string &dest, const std::string &path,
                        const std::string &iface, std::function<void(const Value &)> fn);

}  // namespace dbus
}  // namespace sde
