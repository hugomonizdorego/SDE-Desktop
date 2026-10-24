// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "accounts.h"

#include <cairo.h>
#include <crypt.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>

#include "avatar.h"
#include "dbus.h"
#include "i18n.h"
#include "util.h"

extern char **environ;

namespace sde {
namespace accounts {

namespace {

void set_error(std::string *out, const std::string &text) {
  if (out) *out = text;
}

// ------------------------------------------------------- spelling names

// The ASCII spelling of the Latin letters of Latin-1 and Latin
// Extended-A (and a few more), as the code point ranges that share one.
struct Fold {
  uint32_t first, last;
  const char *ascii;
};
const Fold kFolds[] = {
    {0xC0, 0xC5, "a"},     {0xC6, 0xC6, "ae"},    {0xC7, 0xC7, "c"},     {0xC8, 0xCB, "e"},
    {0xCC, 0xCF, "i"},     {0xD0, 0xD0, "d"},     {0xD1, 0xD1, "n"},     {0xD2, 0xD6, "o"},
    {0xD8, 0xD8, "o"},     {0xD9, 0xDC, "u"},     {0xDD, 0xDD, "y"},     {0xDE, 0xDE, "th"},
    {0xDF, 0xDF, "ss"},    {0xE0, 0xE5, "a"},     {0xE6, 0xE6, "ae"},    {0xE7, 0xE7, "c"},
    {0xE8, 0xEB, "e"},     {0xEC, 0xEF, "i"},     {0xF0, 0xF0, "d"},     {0xF1, 0xF1, "n"},
    {0xF2, 0xF6, "o"},     {0xF8, 0xF8, "o"},     {0xF9, 0xFC, "u"},     {0xFD, 0xFD, "y"},
    {0xFE, 0xFE, "th"},    {0xFF, 0xFF, "y"},     {0x100, 0x105, "a"},   {0x106, 0x10D, "c"},
    {0x10E, 0x111, "d"},   {0x112, 0x11B, "e"},   {0x11C, 0x123, "g"},   {0x124, 0x127, "h"},
    {0x128, 0x131, "i"},   {0x132, 0x133, "ij"},  {0x134, 0x135, "j"},   {0x136, 0x138, "k"},
    {0x139, 0x142, "l"},   {0x143, 0x14B, "n"},   {0x14C, 0x151, "o"},   {0x152, 0x153, "oe"},
    {0x154, 0x159, "r"},   {0x15A, 0x161, "s"},   {0x162, 0x167, "t"},   {0x168, 0x173, "u"},
    {0x174, 0x175, "w"},   {0x176, 0x178, "y"},   {0x179, 0x17E, "z"},   {0x17F, 0x17F, "s"},
    {0x218, 0x219, "s"},   {0x21A, 0x21B, "t"},   {0x1E9E, 0x1E9E, "ss"},
};

// The next code point of |s| at |i| (advanced); 0xFFFD for broken UTF-8.
uint32_t next_code_point(const std::string &s, size_t &i) {
  unsigned char c = (unsigned char)s[i];
  size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
  if (!n || i + n > s.size()) {
    ++i;
    return 0xFFFD;
  }
  uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
  for (size_t k = 1; k < n; ++k) {
    unsigned char b = (unsigned char)s[i + k];
    if ((b >> 6) != 2) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3Fu);
  }
  i += n;
  return cp;
}

size_t code_points(const std::string &s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) {
    next_code_point(s, i);
    ++n;
  }
  return n;
}

// Names that are no one's account even when this system has no such user
// or group (yet).
const char *const kReserved[] = {"root",   "admin",  "administrator", "sudo",  "wheel",  "adm",
                                 "nobody", "nogroup", "users",        "staff", "daemon", "bin",
                                 "sys",    "sync",   "games",         "man",   "lp",     "mail",
                                 "news",   "uucp",   "proxy",         "backup", "list",  "irc",
                                 "operator", "shadow", "utmp",        "tty",   "disk",   "kmem",
                                 "dialout", "audio", "video",         "plugdev", "netdev", "lightdm",
                                 "gdm",    "polkitd", "messagebus",   "systemd-network"};

// Passwords at the top of every list of leaked ones: always weak.
const char *const kCommon[] = {"password", "password1", "passw0rd", "12345678", "123456789",
                               "1234567890", "qwertyui", "qwerty12", "qwerty123", "iloveyou",
                               "sunshine", "princess", "football", "baseball", "welcome1",
                               "admin123", "letmein1", "11111111", "abc12345", "1q2w3e4r",
                               "zaq12wsx", "dragon12", "monkey12", "trustno1", "selviana",
                               "changeme", "superman", "starwars", "whatever", "computer"};

// The longest run of characters that repeat or follow each other
// ("aaaa", "1234", "dcba").
size_t longest_run(const std::string &s) {
  size_t best = s.empty() ? 0 : 1, same = 1, up = 1, down = 1;
  for (size_t i = 1; i < s.size(); ++i) {
    int d = (unsigned char)s[i] - (unsigned char)s[i - 1];
    same = d == 0 ? same + 1 : 1;
    up = d == 1 ? up + 1 : 1;
    down = d == -1 ? down + 1 : 1;
    best = std::max({best, same, up, down});
  }
  return best;
}

// ------------------------------------------------------ the account files

struct LoginDefs {
  uid_t uid_min = 1000, uid_max = 60000;
};

LoginDefs login_defs() {
  LoginDefs d;
  std::string text;
  if (!read_file("/etc/login.defs", text)) return d;
  for (auto &line : split(text, '\n')) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    auto field = [&](const char *key) -> long {
      size_t n = strlen(key);
      if (t.compare(0, n, key) != 0 || t.size() <= n || !isspace((unsigned char)t[n])) return -1;
      return strtol(t.c_str() + n, nullptr, 10);
    };
    long v;
    if ((v = field("UID_MIN")) > 0) d.uid_min = uid_t(v);
    if ((v = field("UID_MAX")) > 0) d.uid_max = uid_t(v);
  }
  return d;
}

bool login_shell(const std::string &shell) {
  return !shell.empty() && !ends_with(shell, "/nologin") && !ends_with(shell, "/false") &&
         !ends_with(shell, "/sync") && shell != "/bin/true";
}

// Names with a session now (display managers and logins write utmp).
std::set<std::string> logged_in_names() {
  static std::mutex mu;  // getutxent() is not reentrant
  std::lock_guard<std::mutex> lock(mu);
  std::set<std::string> out;
  setutxent();
  while (struct utmpx *u = getutxent())
    if (u->ut_type == USER_PROCESS && u->ut_user[0])
      out.insert(std::string(u->ut_user, strnlen(u->ut_user, sizeof u->ut_user)));
  endutxent();
  return out;
}

void sort_users(std::vector<User> &users) {
  std::sort(users.begin(), users.end(), [](const User &a, const User &b) {
    if (a.self != b.self) return a.self;
    std::string x = lower(a.display_name()), y = lower(b.display_name());
    return x != y ? x < y : a.name < b.name;
  });
}

// ------------------------------------------------- AccountsService

const char *const kService = "org.freedesktop.Accounts";
const char *const kManagerPath = "/org/freedesktop/Accounts";
const char *const kManagerIface = "org.freedesktop.Accounts";
const char *const kUserIface = "org.freedesktop.Accounts.User";
const int kLookupTimeoutMs = 4000;    // AccountsService may have to start
const int kChangeTimeoutMs = 120000;  // polkit waits for a password

// An error of AccountsService (or of the bus) in plain words.
std::string explain(const char *name, const char *message) {
  std::string n = name ? name : "", m = message ? message : "";
  if (n == DBUS_ERROR_SERVICE_UNKNOWN || n == DBUS_ERROR_NAME_HAS_NO_OWNER)
    return _("AccountsService is not installed (package accountsservice).");
  if (n == DBUS_ERROR_NO_REPLY || n == DBUS_ERROR_TIMEOUT || n == DBUS_ERROR_TIMED_OUT)
    return _("AccountsService did not answer in time.");
  if (n == "org.freedesktop.Accounts.Error.PermissionDenied" || n == DBUS_ERROR_ACCESS_DENIED ||
      n == "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired")
    return _("Not authorized: this needs the password of an administrator.");
  if (n == "org.freedesktop.Accounts.Error.UserExists")
    return _("An account with this user name exists already.");
  if (n == "org.freedesktop.Accounts.Error.UserDoesNotExist")
    return _("This account does not exist any more.");
  if (n == "org.freedesktop.Accounts.Error.NotSupported")
    return m.empty() ? std::string(_("This system does not support that change.")) : m;
  if (n == "org.freedesktop.Accounts.Error.Failed")
    return m.empty() ? std::string(_("The change did not work.")) : format(_("The change did not work: %s"), m.c_str());
  if (!m.empty()) return m;
  return n.empty() ? std::string(_("AccountsService did not answer.")) : n;
}

// A private connection to the system bus for one piece of work (any
// thread): nothing is shared with the program's FLTK-driven connections.
struct SystemBus {
  DBusConnection *c = nullptr;
  std::string error;
  SystemBus() {
    DBusError err;
    dbus_error_init(&err);
    c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (!c) error = _("The system bus is not available.");
    dbus_error_free(&err);
    if (c) dbus_connection_set_exit_on_disconnect(c, FALSE);
  }
  ~SystemBus() {
    if (c) {
      dbus_connection_close(c);
      dbus_connection_unref(c);
    }
  }
  SystemBus(const SystemBus &) = delete;
  SystemBus &operator=(const SystemBus &) = delete;

  // Sends |m| (taken) and returns the reply's arguments; false with *why.
  bool call(DBusMessage *m, int timeout, std::vector<dbus::Value> *out, std::string *why) {
    if (!m) {
      set_error(why, _("The change could not be sent."));
      return false;
    }
    if (!c) {
      dbus_message_unref(m);
      set_error(why, error);
      return false;
    }
    if (timeout > kLookupTimeoutMs) dbus_message_set_allow_interactive_authorization(m, TRUE);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, timeout, &err);
    dbus_message_unref(m);
    if (!r) {
      set_error(why, explain(err.name, err.message));
      dbus_error_free(&err);
      return false;
    }
    if (out) *out = dbus::read_args(r);
    dbus_message_unref(r);
    return true;
  }

  DBusMessage *manager(const char *member) {
    return dbus_message_new_method_call(kService, kManagerPath, kManagerIface, member);
  }
  DBusMessage *user(const std::string &path, const char *member) {
    if (!dbus::valid_path(path)) return nullptr;
    return dbus_message_new_method_call(kService, path.c_str(), kUserIface, member);
  }

  bool find(const std::string &name, std::string *path, std::string *why) {
    if (!dbus::valid_utf8(name)) return false;
    DBusMessage *m = manager("FindUserByName");
    if (!m) return false;
    dbus::Writer(m).str(name);
    std::vector<dbus::Value> args;
    if (!call(m, kLookupTimeoutMs, &args, why)) return false;
    if (args.empty() || args[0].type != DBUS_TYPE_OBJECT_PATH || args[0].s.empty()) return false;
    *path = args[0].s;
    return true;
  }

  bool read_user(const std::string &path, User *u, bool *system) {
    DBusMessage *m = dbus_message_new_method_call(kService, path.c_str(), "org.freedesktop.DBus.Properties", "GetAll");
    if (!m) return false;
    dbus::Writer(m).str(kUserIface);
    std::vector<dbus::Value> args;
    if (!call(m, kLookupTimeoutMs, &args, nullptr) || args.empty()) return false;
    const dbus::Value &p = args[0];
    auto text = [&](const char *key) {
      const dbus::Value *v = p.get(key);
      return v ? v->str() : std::string();
    };
    auto number = [&](const char *key) -> int64_t {
      const dbus::Value *v = p.get(key);
      return v ? v->num() : 0;
    };
    auto flag = [&](const char *key) {
      const dbus::Value *v = p.get(key);
      return v && v->boolean();
    };
    u->path = path;
    u->uid = uid_t(number("Uid"));
    u->name = text("UserName");
    u->real_name = trim(text("RealName"));
    u->icon = text("IconFile");
    u->home = text("HomeDirectory");
    u->type = number("AccountType") == 1 ? Type::administrator : Type::standard;
    int64_t mode = number("PasswordMode");
    u->password_mode = mode == 1 ? PasswordMode::set_at_login : mode == 2 ? PasswordMode::none : PasswordMode::regular;
    u->locked = flag("Locked");
    u->auto_login = flag("AutomaticLogin");
    *system = flag("SystemAccount");
    return !u->name.empty();
  }

  bool user_call(const User &u, const char *member, const std::function<void(dbus::Writer &)> &args,
                 std::string *why) {
    DBusMessage *m = user(u.path, member);
    if (!m) {
      set_error(why, _("This account is not known to AccountsService."));
      return false;
    }
    {
      dbus::Writer w(m);
      args(w);
    }
    return call(m, kChangeTimeoutMs, nullptr, why);
  }
};

class SystemAccounts : public Backend {
 public:
  bool list(std::vector<User> *users, std::string *error) override {
    SystemBus bus;
    std::vector<std::string> paths;
    {
      std::vector<dbus::Value> args;
      DBusMessage *m = bus.manager("ListCachedUsers");
      if (!bus.call(m, kLookupTimeoutMs, &args, error)) return false;
      if (!args.empty())
        for (const auto &v : args[0].items)
          if (v.type == DBUS_TYPE_OBJECT_PATH) paths.push_back(v.s);
    }
    // ListCachedUsers names those who logged in before; an account added
    // with adduser that never logged in is found by its name.
    for (const User &l : local_users()) {
      std::string path;
      if (bus.find(l.name, &path, nullptr) && std::find(paths.begin(), paths.end(), path) == paths.end())
        paths.push_back(path);
    }
    std::set<std::string> sessions = logged_in_names();
    uid_t me = getuid();
    users->clear();
    for (const std::string &path : paths) {
      User u;
      bool system = false;
      if (!bus.read_user(path, &u, &system)) continue;
      u.self = u.uid == me;
      if (system && !u.self) continue;
      if (std::any_of(users->begin(), users->end(), [&](const User &o) { return o.uid == u.uid; })) continue;
      u.logged_in = sessions.count(u.name) > 0;
      users->push_back(u);
    }
    sort_users(*users);
    return true;
  }
  bool create(const std::string &name, const std::string &real_name, Type type, std::string *path,
              std::string *error) override {
    SystemBus bus;
    DBusMessage *m = bus.manager("CreateUser");
    if (!m) return false;
    dbus::Writer(m).str(name).str(real_name).i32(int32_t(type));
    std::vector<dbus::Value> args;
    if (!bus.call(m, kChangeTimeoutMs, &args, error)) return false;
    if (args.empty() || args[0].type != DBUS_TYPE_OBJECT_PATH) {
      set_error(error, _("AccountsService did not name the new account."));
      return false;
    }
    if (path) *path = args[0].s;
    return true;
  }
  bool remove(const User &u, bool remove_files, std::string *error) override {
    SystemBus bus;
    DBusMessage *m = bus.manager("DeleteUser");
    if (!m) return false;
    {
      dbus::Writer w(m);
      dbus_int64_t id = dbus_int64_t(u.uid);
      dbus_message_iter_append_basic(w.iter(), DBUS_TYPE_INT64, &id);
      w.boolean(remove_files);
    }
    return bus.call(m, kChangeTimeoutMs, nullptr, error);
  }
  bool set_type(const User &u, Type type, std::string *error) override {
    SystemBus bus;
    return bus.user_call(u, "SetAccountType", [&](dbus::Writer &w) { w.i32(int32_t(type)); }, error);
  }
  bool set_password(const User &u, const std::string &hash, std::string *error) override {
    SystemBus bus;
    return bus.user_call(u, "SetPassword", [&](dbus::Writer &w) { w.str(hash).str(""); }, error);
  }
  bool set_password_mode(const User &u, PasswordMode mode, std::string *error) override {
    SystemBus bus;
    return bus.user_call(u, "SetPasswordMode", [&](dbus::Writer &w) { w.i32(int32_t(mode)); }, error);
  }
  bool set_locked(const User &u, bool locked, std::string *error) override {
    SystemBus bus;
    return bus.user_call(u, "SetLocked", [&](dbus::Writer &w) { w.boolean(locked); }, error);
  }
  bool set_auto_login(const User &u, bool on, std::string *error) override {
    SystemBus bus;
    return bus.user_call(u, "SetAutomaticLogin", [&](dbus::Writer &w) { w.boolean(on); }, error);
  }
  bool set_real_name(const User &u, const std::string &name, std::string *error) override {
    SystemBus bus;
    return bus.user_call(u, "SetRealName", [&](dbus::Writer &w) { w.str(name); }, error);
  }
  bool set_icon(const User &u, const std::string &file, std::string *error) override {
    SystemBus bus;
    return bus.user_call(u, "SetIconFile", [&](dbus::Writer &w) { w.str(file); }, error);
  }
};

std::mutex g_backend_mu;
Backend *g_backend = nullptr;

cairo_status_t write_fd(void *closure, const unsigned char *data, unsigned int length) {
  int fd = *static_cast<int *>(closure);
  while (length > 0) {
    ssize_t n = write(fd, data, length);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return CAIRO_STATUS_WRITE_ERROR;
    data += n;
    length -= unsigned(n);
  }
  return CAIRO_STATUS_SUCCESS;
}

// ------------------------------------------------------------ passwd(1)

// The kind of line passwd printed last, in the C locale.
enum class Prompt { none, current, fresh, again };

Prompt prompt_of(const std::string &line) {
  std::string l = lower(trim(line));
  if (l.empty() || l.back() != ':') return Prompt::none;
  if (l.find("password") == std::string::npos) return Prompt::none;
  if (l.find("retype") != std::string::npos || l.find("re-enter") != std::string::npos ||
      l.find("repeat") != std::string::npos || l.find("again") != std::string::npos ||
      l.find("confirm") != std::string::npos)
    return Prompt::again;
  if (l.find("new") != std::string::npos) return Prompt::fresh;
  return Prompt::current;  // "Current password:", "(current) UNIX password:", "Password:"
}

// What passwd printed, as one sentence for the user.
std::string passwd_failure(const std::string &output, const std::vector<std::string> &secrets) {
  std::string l = lower(output);
  if (l.find("authentication token manipulation error") != std::string::npos ||
      l.find("authentication failure") != std::string::npos)
    return _("The current password is not correct.");
  if (l.find("do not match") != std::string::npos || l.find("don't match") != std::string::npos)
    return _("The passwords do not match.");
  if (l.find("wait longer") != std::string::npos)
    return _("The password was changed recently: the system's rules ask you to wait before changing it again.");
  // The system's own reason (pam_unix, pam_pwquality: "BAD PASSWORD: ...",
  // "You must choose a longer password"), the last such line.
  std::string reason;
  for (auto &line : split(output, '\n')) {
    std::string t = trim(line);
    if (starts_with(t, "passwd:")) t = trim(t.substr(7));
    if (t.empty() || prompt_of(t) != Prompt::none) continue;
    std::string lt = lower(t);
    if (starts_with(lt, "changing password") || lt.find("password unchanged") != std::string::npos ||
        lt.find("have exhausted") != std::string::npos)
      continue;
    // Never repeat what was typed, should a program echo it.
    if (std::any_of(secrets.begin(), secrets.end(),
                    [&](const std::string &s) { return !s.empty() && t.find(s) != std::string::npos; }))
      continue;
    reason = t;
  }
  if (!reason.empty()) return format(_("The password was not changed: %s"), reason.c_str());
  return _("The password was not changed.");
}

}  // namespace

// ================================================================== rules

std::string ascii_words(const std::string &text) {
  std::string out, word;
  auto flush = [&] {
    while (!word.empty() && word.back() == '-') word.pop_back();
    size_t start = word.find_first_not_of('-');
    if (start != std::string::npos) {
      if (!out.empty()) out += ' ';
      out += word.substr(start);
    }
    word.clear();
  };
  for (size_t i = 0; i < text.size();) {
    uint32_t cp = next_code_point(text, i);
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == 0xA0) {
      flush();
    } else if (cp < 0x80) {
      char c = char(cp);
      if (isalnum((unsigned char)c)) word += char(tolower((unsigned char)c));
      else if (c == '-' && !word.empty()) word += '-';
    } else {
      for (const Fold &f : kFolds)
        if (cp >= f.first && cp <= f.last) {
          word += f.ascii;
          break;
        }
    }
  }
  flush();
  return out;
}

bool name_in_use(const std::string &name) {
  if (name.empty()) return false;
  long size = std::max(sysconf(_SC_GETPW_R_SIZE_MAX), sysconf(_SC_GETGR_R_SIZE_MAX));
  std::vector<char> buf(size_t(size > 0 ? size : 16384));
  for (int tries = 0; tries < 4; ++tries) {
    struct passwd pw, *found = nullptr;
    int r = getpwnam_r(name.c_str(), &pw, buf.data(), buf.size(), &found);
    if (r == ERANGE) {
      buf.resize(buf.size() * 4);
      continue;
    }
    if (found) return true;
    break;
  }
  for (int tries = 0; tries < 4; ++tries) {
    struct group gr, *found = nullptr;
    int r = getgrnam_r(name.c_str(), &gr, buf.data(), buf.size(), &found);
    if (r == ERANGE) {
      buf.resize(buf.size() * 4);
      continue;
    }
    return found != nullptr;
  }
  return false;
}

std::string user_name_problem(const std::string &name, const std::vector<std::string> &taken) {
  if (name.empty()) return _("Choose a user name.");
  if (name.size() > 32) return _("A user name has at most 32 characters.");
  if (name[0] < 'a' || name[0] > 'z') return _("A user name starts with a lowercase letter (a to z).");
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return _("A user name has only lowercase letters (a to z), digits, - and _.");
  for (const char *r : kReserved)
    if (name == r) return format(_("\"%s\" is a name of the system."), name.c_str());
  if (std::find(taken.begin(), taken.end(), name) != taken.end() || name_in_use(name))
    return format(_("The user name \"%s\" is in use already."), name.c_str());
  return "";
}

std::string suggest_user_name(const std::string &full_name, const std::vector<std::string> &taken) {
  std::vector<std::string> words;
  for (auto &w : split(ascii_words(full_name), ' '))
    if (!w.empty()) words.push_back(w);
  if (words.empty()) return "";
  // A user name starts with a letter: "2pac" becomes "u2pac".
  auto fix = [](std::string s) {
    if (s.empty()) return s;
    if (s[0] < 'a' || s[0] > 'z') s = "u" + s;
    return s.substr(0, 32);
  };
  std::string first = words.front(), last = words.back();
  std::vector<std::string> tries = {fix(first)};
  if (words.size() > 1) {
    tries.push_back(fix(first + last));
    tries.push_back(fix(first.substr(0, 1) + last));
  }
  for (const auto &t : tries)
    if (user_name_problem(t, taken).empty()) return t;
  for (int n = 2; n < 100; ++n) {
    std::string number = std::to_string(n);
    std::string t = fix(first).substr(0, 32 - number.size()) + number;
    if (user_name_problem(t, taken).empty()) return t;
  }
  return "";
}

int password_strength(const std::string &password, const std::string &user, const std::string &real_name) {
  size_t n = code_points(password);
  if (n < 8) return 0;
  bool low = false, up = false, digit = false, other = false;
  for (unsigned char c : password) {
    if (c >= 'a' && c <= 'z') low = true;
    else if (c >= 'A' && c <= 'Z') up = true;
    else if (c >= '0' && c <= '9') digit = true;
    else other = true;
  }
  std::string l = lower(password);
  for (const char *c : kCommon)
    if (l == c) return 1;
  // Bits of a random choice from the kinds of characters used, less for
  // runs ("aaaa", "1234") and for the user's own names.
  double pool = (low ? 26 : 0) + (up ? 26 : 0) + (digit ? 10 : 0) + (other ? 33 : 0);
  std::set<char> distinct(password.begin(), password.end());
  double bits = double(n) * std::log2(std::max(2.0, pool));
  if (distinct.size() * 2 < password.size()) bits *= 0.5;
  size_t run = longest_run(l);
  if (run >= 4) bits -= double(run - 3) * 6;
  std::vector<std::string> names = {lower(user)};
  for (auto &w : split(lower(real_name), ' ')) names.push_back(w);
  for (const auto &name : names)
    if (name.size() >= 3 && l.find(name) != std::string::npos) bits -= double(name.size()) * 4;
  if (bits < 40) return 1;
  if (bits < 60) return 2;
  if (bits < 80) return 3;
  return 4;
}

std::string strength_label(int strength) {
  switch (strength) {
    case 0: return _("Too short");
    case 1: return _("Weak");
    case 2: return _("Fair");
    case 3: return _("Good");
    default: return _("Strong");
  }
}

std::string password_problem(const std::string &password, const std::string &again, const std::string &user,
                             const std::string &real_name) {
  (void)real_name;
  if (password.empty()) return _("Type a password.");
  for (unsigned char c : password)
    if (c < 0x20 || c == 0x7F) return _("A password cannot contain line breaks or control characters.");
  if (!dbus::valid_utf8(password)) return _("The password is not valid text.");
  if (code_points(password) < 8) return _("Use at least 8 characters.");
  if (!user.empty() && lower(password) == lower(user)) return _("The password cannot be the user name.");
  if (password != again) return _("The passwords do not match.");
  return "";
}

std::string hash_password(const std::string &password) {
  char salt[CRYPT_GENSALT_OUTPUT_SIZE];
  // yescrypt, Debian's default since bullseye; SHA-512 where it is missing.
  if (!crypt_gensalt_rn("$y$", 0, nullptr, 0, salt, sizeof salt) &&
      !crypt_gensalt_rn("$6$", 0, nullptr, 0, salt, sizeof salt))
    return "";
  auto data = std::make_unique<struct crypt_data>();  // zeroed: "initialized" is 0
  const char *h = crypt_rn(password.c_str(), salt, data.get(), int(sizeof *data));
  std::string out = h && h[0] == '$' ? h : "";
  explicit_bzero(data.get(), sizeof *data);
  return out;
}

int active_admins(const std::vector<User> &all) {
  return int(std::count_if(all.begin(), all.end(), [](const User &u) { return u.admin() && !u.locked; }));
}

namespace {
bool last_admin(const std::vector<User> &all, const User &target) {
  return target.admin() && !target.locked && active_admins(all) <= 1;
}
}  // namespace

std::string why_not_remove(const std::vector<User> &all, const User &target) {
  if (target.self) return _("You cannot remove your own account.");
  if (last_admin(all, target))
    return _("This is the last administrator. Make another account an administrator first.");
  return "";
}

std::string why_not_set_type(const std::vector<User> &all, const User &target, Type type) {
  if (type == target.type) return "";
  if (target.self) return _("Another administrator can change the type of your account.");
  if (type == Type::standard && last_admin(all, target))
    return _("This is the last administrator. Make another account an administrator first.");
  return "";
}

std::string why_not_lock(const std::vector<User> &all, const User &target) {
  if (target.self) return _("You cannot lock your own account.");
  if (last_admin(all, target))
    return _("This is the last administrator. Make another account an administrator first.");
  return "";
}

std::string type_label(Type t) { return t == Type::administrator ? _("Administrator") : _("Standard"); }

// ======================================================= AccountsService

Backend &backend() {
  std::lock_guard<std::mutex> lock(g_backend_mu);
  if (g_backend) return *g_backend;
  static SystemAccounts system;
  return system;
}

void set_backend(Backend *b) {
  std::lock_guard<std::mutex> lock(g_backend_mu);
  g_backend = b;
}

std::vector<User> local_users() {
  std::vector<User> out;
  LoginDefs defs = login_defs();
  uid_t me = getuid();
  std::set<std::string> sessions = logged_in_names();
  auto add = [&](const struct passwd &pw) {
    if (!pw.pw_name || !*pw.pw_name) return;
    if (std::any_of(out.begin(), out.end(), [&](const User &u) { return u.uid == pw.pw_uid; })) return;
    User u;
    u.uid = pw.pw_uid;
    u.name = pw.pw_name;
    std::string gecos = pw.pw_gecos ? pw.pw_gecos : "";
    u.real_name = trim(gecos.substr(0, gecos.find(',')));
    if (u.real_name == u.name) u.real_name.clear();
    u.home = pw.pw_dir ? pw.pw_dir : "";
    u.type = avatar::administrator(u.name) ? Type::administrator : Type::standard;
    u.self = u.uid == me;
    u.logged_in = sessions.count(u.name) > 0;
    out.push_back(u);
  };
  if (FILE *f = fopen("/etc/passwd", "re")) {
    struct passwd pw, *found = nullptr;
    std::vector<char> buf(16384);
    while (fgetpwent_r(f, &pw, buf.data(), buf.size(), &found) == 0 && found)
      if (pw.pw_uid >= defs.uid_min && pw.pw_uid <= defs.uid_max && login_shell(pw.pw_shell ? pw.pw_shell : ""))
        add(pw);
    fclose(f);
  }
  {
    // Yours, also when it comes from elsewhere (LDAP) or is a system account.
    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buf(size_t(size > 0 ? size : 16384));
    struct passwd pw, *found = nullptr;
    if (getpwuid_r(me, &pw, buf.data(), buf.size(), &found) == 0 && found) add(pw);
  }
  sort_users(out);
  return out;
}

// ================================================================ changes

bool add_user(const NewUser &n, const std::vector<User> &all, std::string *error) {
  std::vector<std::string> taken;
  for (const User &u : all) taken.push_back(u.name);
  std::string why = user_name_problem(n.name, taken);
  std::string real = trim(n.real_name);
  if (why.empty() && real.empty()) why = _("Type the full name of the person.");
  if (why.empty()) avatar::valid_real_name(real, &why);
  if (why.empty() && !n.password_at_login) why = password_problem(n.password, n.again, n.name, real);
  if (!why.empty()) {
    set_error(error, why);
    return false;
  }
  std::string hash;
  if (!n.password_at_login && (hash = hash_password(n.password)).empty()) {
    set_error(error, _("The password could not be encrypted (libcrypt)."));
    return false;
  }
  User u;
  u.name = n.name;
  u.real_name = real;
  u.type = n.type;
  if (!backend().create(n.name, real, n.type, &u.path, error)) return false;
  std::string failed;
  bool ok = n.password_at_login ? backend().set_password_mode(u, PasswordMode::set_at_login, &failed)
                                : backend().set_password(u, hash, &failed);
  explicit_bzero(&hash[0], hash.size());
  if (!ok) {
    set_error(error, format(_("The account \"%s\" was added, but its password was not set: %s"), n.name.c_str(),
                            failed.c_str()));
    return false;
  }
  return true;
}

bool set_password(const User &u, bool at_login, const std::string &password, const std::string &again,
                  std::string *error) {
  if (at_login) return backend().set_password_mode(u, PasswordMode::set_at_login, error);
  std::string why = password_problem(password, again, u.name, u.real_name);
  if (!why.empty()) {
    set_error(error, why);
    return false;
  }
  std::string hash = hash_password(password);
  if (hash.empty()) {
    set_error(error, _("The password could not be encrypted (libcrypt)."));
    return false;
  }
  bool ok = backend().set_password(u, hash, error);
  explicit_bzero(&hash[0], hash.size());
  return ok;
}

bool set_picture(const User &u, const std::string &source, std::string *error) {
  if (source.empty()) return backend().set_icon(u, "", error);
  cairo_surface_t *pic = avatar::load_square(source, avatar::kSize, error);
  if (!pic) return false;
  // A private copy that AccountsService reads (as this user) and copies
  // to /var/lib/AccountsService/icons; removed right after.
  std::string dir = runtime_dir();
  std::string path = (dir.empty() ? std::string("/tmp") : dir) + "/sde-account-picture-XXXXXX";
  int fd = mkostemp(&path[0], O_CLOEXEC);
  if (fd < 0) {
    cairo_surface_destroy(pic);
    set_error(error, format(_("Cannot save the picture: %s"), strerror(errno)));
    return false;
  }
  cairo_status_t st = cairo_surface_write_to_png_stream(pic, write_fd, &fd);
  cairo_surface_destroy(pic);
  close(fd);
  bool ok = st == CAIRO_STATUS_SUCCESS;
  if (!ok) set_error(error, _("The picture could not be converted."));
  ok = ok && backend().set_icon(u, path, error);
  unlink(path.c_str());
  return ok;
}

bool change_own_password(const std::string &current, const std::string &password, const std::string &again,
                         std::string *error, const std::string &program) {
  std::string why = password_problem(password, again, user_name(), "");
  if (why.empty() && current.empty()) why = _("Type your current password.");
  for (unsigned char c : current)
    if (why.empty() && (c < 0x20 || c == 0x7F)) why = _("The current password is not correct.");
  if (why.empty() && password == current) why = _("The new password is the same as the current one.");
  if (!why.empty()) {
    set_error(error, why);
    return false;
  }
  std::string prog = program.empty() ? find_program("passwd") : program;
  if (prog.empty()) {
    set_error(error, _("passwd is not installed (package passwd)."));
    return false;
  }
  // Everything the child needs is made before fork(): after it, in a
  // program with threads, only async-signal-safe calls are allowed.
  std::vector<std::string> envs;
  for (char **e = environ; e && *e; ++e) {
    std::string kv = *e;
    if (starts_with(kv, "LC_") || starts_with(kv, "LANG=") || starts_with(kv, "LANGUAGE=")) continue;
    envs.push_back(kv);
  }
  envs.push_back("LC_ALL=C");
  envs.push_back("LANG=C");
  std::vector<char *> envp;
  for (auto &kv : envs) envp.push_back(&kv[0]);
  envp.push_back(nullptr);
  std::string arg0 = "passwd";
  char *argv[] = {&arg0[0], nullptr};

  int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  char slave_name[128] = "";
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
      ptsname_r(master, slave_name, sizeof slave_name) != 0) {
    if (master >= 0) close(master);
    set_error(error, _("Cannot open a terminal for passwd."));
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(master);
    set_error(error, _("Cannot start passwd."));
    return false;
  }
  if (pid == 0) {
    setsid();
    int slave = open(slave_name, O_RDWR);  // becomes the controlling terminal
    if (slave < 0) _exit(127);
    ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2) close(slave);
    execve(prog.c_str(), argv, envp.data());
    _exit(127);
  }

  // The conversation: answer each prompt once. A prompt that comes again
  // means passwd refused the answer (wrong password, a rule): stop there.
  std::string output, line;
  bool asked_current = false, asked_new = false, asked_again = false, refused = false;
  time_t deadline = time(nullptr) + 60;
  auto answer = [&](const std::string &text) {
    std::string data = text + "\n";
    size_t done = 0;
    while (done < data.size()) {
      ssize_t n = write(master, data.data() + done, data.size() - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      done += size_t(n);
    }
    explicit_bzero(&data[0], data.size());
  };
  for (;;) {
    int left = int(deadline - time(nullptr));
    if (left <= 0) {
      refused = true;
      output += "\n" + std::string(_("passwd did not finish in time."));
      break;
    }
    struct pollfd p = {master, POLLIN, 0};
    int r = poll(&p, 1, std::min(left, 1) * 1000);
    if (r < 0 && errno == EINTR) continue;
    if (r == 0) {
      // Quiet: maybe a prompt without a newline is waiting.
    } else {
      char buf[512];
      ssize_t n = read(master, buf, sizeof buf);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;  // EIO: passwd closed the terminal (it ended)
      for (ssize_t i = 0; i < n; ++i) {
        char c = buf[i];
        if (c == '\r') continue;
        output += c;
        if (c == '\n') line.clear();
        else line += c;
      }
    }
    Prompt pr = prompt_of(line);
    if (pr == Prompt::none) continue;
    line.clear();
    bool &asked = pr == Prompt::current ? asked_current : pr == Prompt::fresh ? asked_new : asked_again;
    if (asked) {
      refused = true;
      break;
    }
    asked = true;
    answer(pr == Prompt::current ? current : password);
  }
  if (refused) kill(pid, SIGTERM);
  int status = 0;
  for (int i = 0; i < 50; ++i) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
    if (w < 0 && errno != EINTR) break;
    if (i == 30) kill(pid, SIGKILL);
    struct timespec ts = {0, 100 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
  close(master);
  bool ok = !refused && WIFEXITED(status) && WEXITSTATUS(status) == 0 && asked_new;
  if (!ok) set_error(error, passwd_failure(output, {current, password}));
  return ok;
}

}  // namespace accounts
}  // namespace sde
