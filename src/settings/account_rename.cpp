// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "account_rename.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
// <spawn.h> would find SDE's own spawn.h (src/common is on the include path).
#include <../include/spawn.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "i18n.h"
#include "util.h"

namespace sde {
namespace account_rename {

namespace {

// Names that are no one's account even when this system has no such user
// or group (yet).
const char *const kReserved[] = {"root",   "admin",  "administrator", "sudo",  "wheel",  "adm",
                                 "nobody", "nogroup", "users",        "staff", "daemon", "bin",
                                 "sys",    "sync",   "games",         "man",   "lp",     "mail",
                                 "news",   "uucp",   "proxy",         "backup", "list",  "irc",
                                 "operator", "shadow", "utmp",        "tty",   "disk",   "kmem",
                                 "dialout", "audio", "video",         "plugdev", "netdev", "lightdm",
                                 "gdm",    "polkitd", "messagebus",   "systemd-network"};

bool name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
         c == '.';
}

// An existing login name (maybe made with useradd --badname): letters,
// digits, '.', '_' and '-', not starting with '-'.
bool plausible_name(const std::string &n) {
  if (n.empty() || n.size() > 32 || n[0] == '-' || n == "." || n == "..") return false;
  return std::all_of(n.begin(), n.end(), name_char);
}

bool plain_path(const std::string &p) {
  if (p.empty() || p[0] != '/' || p.size() > 4096) return false;
  for (unsigned char c : p)
    if (c < 0x20 || c == 0x7F) return false;
  for (const auto &part : split(p, '/'))
    if (part == "..") return false;
  return true;
}

bool plain_text(const std::string &s) {
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7F) return false;
  return true;
}

bool parse_id(const std::string &s, unsigned long *out) {
  if (s.empty() || s.size() > 10 || !std::all_of(s.begin(), s.end(), ::isdigit)) return false;
  *out = strtoul(s.c_str(), nullptr, 10);
  return true;
}

std::string dirname_of(const std::string &p) {
  size_t s = p.find_last_of('/');
  if (s == std::string::npos) return ".";
  return s == 0 ? "/" : p.substr(0, s);
}

std::string strip_slashes(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

// The words of |line| (runs of name characters), with where they start.
std::vector<std::pair<size_t, std::string>> words(const std::string &line) {
  std::vector<std::pair<size_t, std::string>> out;
  size_t i = 0;
  while (i < line.size()) {
    if (!name_char(line[i])) {
      ++i;
      continue;
    }
    size_t b = i;
    while (i < line.size() && name_char(line[i])) ++i;
    out.emplace_back(b, line.substr(b, i - b));
  }
  return out;
}

bool has_word(const std::string &text, const std::string &name) {
  for (const auto &w : words(text))
    if (w.second == name) return true;
  return false;
}

// Replaces the words |from| of |line| by |to|.
std::string replace_words(const std::string &line, const std::string &from, const std::string &to) {
  std::string out;
  size_t at = 0;
  for (const auto &w : words(line)) {
    if (w.second != from) continue;
    out += line.substr(at, w.first - at) + to;
    at = w.first + w.second.size();
  }
  return out + line.substr(at);
}

// Applies |edit| to every line of |text| (the line without its end).
std::string each_line(const std::string &text, const std::function<std::string(const std::string &)> &edit) {
  std::string out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    out += edit(line);
    if (e == std::string::npos) break;
    out += '\n';
    pos = e + 1;
  }
  return out;
}

// "key = value" lines of INI-like files: the line with the value |from|
// of one of |keys| replaced by |to|.
std::string edit_keys(const std::string &text, const std::vector<std::string> &keys, const std::string &from,
                      const std::string &to) {
  return each_line(text, [&](const std::string &line) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return line;
    std::string key = trim(line.substr(0, eq));
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) return line;
    std::string value = line.substr(eq + 1);
    if (trim(value) != from) return line;
    size_t v = value.find(from);
    return line.substr(0, eq + 1) + value.substr(0, v) + to + value.substr(v + from.size());
  });
}

// sudoers lines joined where they end with a backslash.
std::vector<std::string> logical_lines(const std::string &text) {
  std::vector<std::string> out;
  std::string cur;
  for (const auto &raw : split(text, '\n', true)) {
    if (!raw.empty() && raw.back() == '\\') {
      cur += raw.substr(0, raw.size() - 1) + " ";
      continue;
    }
    out.push_back(cur + raw);
    cur.clear();
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool comment(const std::string &line) {
  std::string t = trim(line);
  return !t.empty() && t[0] == '#' && !starts_with(t, "#include") && !(t.size() > 1 && isdigit((unsigned char)t[1]));
}

// Whether the sudoers |text| grants something to the user |name|: as a
// user of a rule or of User_Alias / Runas_Alias, in "Defaults:name" or as a
// user to run as. Command paths do not count.
bool sudoers_names(const std::string &text, const std::string &name) {
  for (const auto &line : logical_lines(text)) {
    std::string t = trim(line);
    if (t.empty() || comment(t) || starts_with(t, "#include") || starts_with(t, "@include")) continue;
    if (starts_with(t, "Defaults")) {
      size_t sp = t.find_first_of(" \t");
      if (has_word(t.substr(0, sp), name)) return true;
      continue;
    }
    if (starts_with(t, "Host_Alias") || starts_with(t, "Cmnd_Alias") || starts_with(t, "Cmd_Alias")) continue;
    if (starts_with(t, "User_Alias") || starts_with(t, "Runas_Alias")) {
      size_t eq = t.find('=');
      if (eq != std::string::npos && has_word(t.substr(eq + 1), name)) return true;
      continue;
    }
    size_t eq = t.find('=');
    if (has_word(t.substr(0, eq), name)) return true;
    // "(alice : wheel)": who the commands run as.
    for (size_t open = t.find('(', eq == std::string::npos ? 0 : eq); open != std::string::npos;
         open = t.find('(', open + 1)) {
      size_t close = t.find(')', open);
      if (close == std::string::npos) break;
      if (has_word(t.substr(open + 1, close - open - 1), name)) return true;
    }
  }
  return false;
}

// The files of directory |dir| (below the root) that sudo reads: no '.' or
// '~' in their names.
std::vector<std::string> sudoers_dropins(const std::string &root, const std::string &dir) {
  std::vector<std::string> out;
  for (const auto &n : list_dir(root + dir))
    if (n.find('.') == std::string::npos && n.find('~') == std::string::npos) out.push_back(dir + "/" + n);
  return out;
}

std::vector<std::string> files_with_suffix(const std::string &root, const std::string &dir,
                                           const std::string &suffix, bool recursive = false) {
  std::vector<std::string> out;
  for (const auto &n : list_dir(root + dir)) {
    std::string rel = dir + "/" + n;
    struct stat st;
    if (lstat((root + rel).c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode) && recursive) {
      auto sub = files_with_suffix(root, rel, suffix, true);
      out.insert(out.end(), sub.begin(), sub.end());
    } else if (S_ISREG(st.st_mode) && ends_with(n, suffix)) {
      out.push_back(rel);
    }
  }
  return out;
}

std::string read_text(const std::string &path) {
  std::string s;
  read_file(path, s);
  return s;
}

std::string now_text() {
  char buf[32];
  time_t t = time(nullptr);
  strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", localtime(&t));
  return buf;
}

// Writes |content| to |path| in place of the file there, keeping its
// owner and mode (a new file next to it, then rename(2)).
bool replace_file(const std::string &path, const std::string &content, std::string *error) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    *error = path + ": " + strerror(errno ? errno : EINVAL);
    return false;
  }
  std::string tmp = path + ".sde-rename";
  unlink(tmp.c_str());
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    *error = tmp + ": " + strerror(errno);
    return false;
  }
  bool ok = true;
  size_t done = 0;
  while (ok && done < content.size()) {
    ssize_t n = write(fd, content.data() + done, content.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = false;
    else done += size_t(n);
  }
  if (ok && geteuid() == 0 && fchown(fd, st.st_uid, st.st_gid) != 0) ok = false;
  if (ok && fchmod(fd, st.st_mode & 07777) != 0) ok = false;
  if (ok && fsync(fd) != 0) ok = false;
  close(fd);
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    *error = path + ": " + strerror(errno ? errno : EIO);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::string find_tool(const std::string &name) {
  for (const char *dir : {"/usr/sbin", "/usr/bin", "/sbin", "/bin"}) {
    std::string p = std::string(dir) + "/" + name;
    if (access(p.c_str(), X_OK) == 0) return p;
  }
  return "";
}

}  // namespace

// ================================================================ names

std::string login_name_problem(const std::string &name) {
  if (name.empty()) return _("Choose a user name.");
  if (name.size() > 32) return _("A user name has at most 32 characters.");
  if (name[0] < 'a' || name[0] > 'z') return _("A user name starts with a lowercase letter (a to z).");
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return _("A user name has only lowercase letters (a to z), digits, - and _.");
  for (const char *r : kReserved)
    if (name == r) return format(_("\"%s\" is a name of the system."), name.c_str());
  return "";
}

// ============================================================= requests

std::string default_dir() { return env("SDE_ACCOUNT_RENAME_DIR", "/var/lib/sde/account-renames"); }

std::string request_path(const std::string &dir, uid_t uid) { return dir + "/" + std::to_string(uid) + ".rename"; }

bool parse_request(const std::string &text, Request *out, std::string *error) {
  *out = Request();
  auto fail = [&](const std::string &why) {
    if (error) *error = why;
    return false;
  };
  std::map<std::string, std::string> f;
  for (const auto &line : split(text, '\n')) {
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) return fail("not key=value: " + line);
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    static const char *const known[] = {"uid", "old", "new", "old_home", "new_home", "link", "by", "time",
                                        "state", "error"};
    if (std::find_if(std::begin(known), std::end(known), [&](const char *x) { return k == x; }) == std::end(known))
      return fail("unknown key " + k);
    if (f.count(k)) return fail("twice: " + k);
    if (!plain_text(v)) return fail("control characters in " + k);
    f[k] = v;
  }
  unsigned long n = 0;
  if (!parse_id(f["uid"], &n)) return fail("bad uid");
  out->uid = uid_t(n);
  out->old_name = f["old"];
  out->new_name = f["new"];
  if (!plausible_name(out->old_name)) return fail("bad old name");
  if (!login_name_problem(out->new_name).empty()) return fail("bad new name");
  if (out->old_name == out->new_name) return fail("the same name");
  out->old_home = f["old_home"];
  out->new_home = f["new_home"];
  if (!plain_path(out->old_home) || !plain_path(out->new_home)) return fail("bad home folder");
  if (out->new_home != out->old_home && out->new_home != new_home_for(out->old_home, out->old_name, out->new_name))
    return fail("the new home folder is not the old one renamed");
  out->link = f["link"] == "1";
  if (!f["link"].empty() && f["link"] != "0" && f["link"] != "1") return fail("bad link");
  if (!f["by"].empty()) {
    if (!parse_id(f["by"], &n)) return fail("bad by");
    out->by = uid_t(n);
  }
  if (!f["time"].empty()) {
    if (!parse_id(f["time"], &n)) return fail("bad time");
    out->time = (long long)n;
  }
  out->state = f["state"].empty() ? "pending" : f["state"];
  if (out->state != "pending" && out->state != "failed") return fail("bad state");
  out->error = f["error"];
  return true;
}

std::string format_request(const Request &r) {
  std::string out = "# SDE account rename (sde-account-rename, docs/CONTROL-CENTER.md)\n";
  out += "uid=" + std::to_string(r.uid) + "\n";
  out += "old=" + r.old_name + "\n";
  out += "new=" + r.new_name + "\n";
  out += "old_home=" + r.old_home + "\n";
  out += "new_home=" + r.new_home + "\n";
  out += std::string("link=") + (r.link ? "1" : "0") + "\n";
  out += "by=" + std::to_string(r.by) + "\n";
  out += "time=" + std::to_string(r.time) + "\n";
  out += "state=" + r.state + "\n";
  std::string e = r.error;
  for (char &c : e)
    if ((unsigned char)c < 0x20 || c == 0x7F) c = ' ';
  if (!e.empty()) out += "error=" + e + "\n";
  return out;
}

std::vector<Request> load_requests(const std::string &dir) {
  std::vector<Request> out;
  for (const auto &n : list_dir(dir)) {
    if (!ends_with(n, ".rename")) continue;
    std::string text;
    Request r;
    if (!read_file(dir + "/" + n, text) || !parse_request(text, &r, nullptr)) continue;
    if (n != std::to_string(r.uid) + ".rename") continue;
    out.push_back(r);
  }
  return out;
}

bool save_request(const std::string &dir, const Request &r, std::string *error) {
  std::string path = request_path(dir, r.uid), tmp = path + ".tmp";
  unlink(tmp.c_str());
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  std::string text = format_request(r);
  bool ok = fd >= 0 && write(fd, text.data(), text.size()) == ssize_t(text.size());
  if (fd >= 0) {
    ok = fchmod(fd, 0644) == 0 && fsync(fd) == 0 && ok;
    close(fd);
  }
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    if (error) *error = path + ": " + strerror(errno ? errno : EIO);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::string new_home_for(const std::string &home, const std::string &old_name, const std::string &new_name) {
  std::string h = strip_slashes(home);
  if (basename_of(h) != old_name) return home;
  std::string parent = dirname_of(h);
  return (parent == "/" ? "" : parent) + "/" + new_name;
}

// ========================================================= the database

std::vector<Passwd> read_passwd(const std::string &root) {
  std::vector<Passwd> out;
  for (const auto &line : split(read_text(root + "/etc/passwd"), '\n')) {
    std::vector<std::string> f = split(line, ':', true);
    unsigned long uid = 0, gid = 0;
    if (f.size() != 7 || !parse_id(f[2], &uid) || !parse_id(f[3], &gid)) continue;
    Passwd p;
    p.name = f[0];
    p.uid = uid_t(uid);
    p.gid = gid_t(gid);
    p.gecos = f[4];
    p.home = f[5];
    p.shell = f[6];
    out.push_back(p);
  }
  return out;
}

std::vector<Group> read_group(const std::string &root) {
  std::vector<Group> out;
  for (const auto &line : split(read_text(root + "/etc/group"), '\n')) {
    std::vector<std::string> f = split(line, ':', true);
    unsigned long gid = 0;
    if (f.size() != 4 || !parse_id(f[2], &gid)) continue;
    Group g;
    g.name = f[0];
    g.gid = gid_t(gid);
    for (const auto &m : split(f[3], ','))
      if (!m.empty()) g.members.push_back(m);
    out.push_back(g);
  }
  return out;
}

void uid_range(const std::string &root, uid_t *min, uid_t *max) {
  *min = 1000;
  *max = 60000;
  for (const auto &raw : split(read_text(root + "/etc/login.defs"), '\n')) {
    std::vector<std::string> w;
    for (const auto &x : split(trim(raw), ' '))
      for (const auto &y : split(x, '\t'))
        if (!y.empty()) w.push_back(y);
    unsigned long n = 0;
    if (w.size() >= 2 && w[0] == "UID_MIN" && parse_id(w[1], &n)) *min = uid_t(n);
    if (w.size() >= 2 && w[0] == "UID_MAX" && parse_id(w[1], &n)) *max = uid_t(n);
  }
}

std::vector<std::string> mentions(const std::string &root, const std::string &name) {
  std::vector<std::string> out;
  auto add = [&](const std::string &rel) {
    if (std::find(out.begin(), out.end(), rel) == out.end()) out.push_back(rel);
  };
  for (const auto &g : read_group(root))
    if (std::find(g.members.begin(), g.members.end(), name) != g.members.end()) add("/etc/group");
  for (const auto &line : split(read_text(root + "/etc/gshadow"), '\n')) {
    std::vector<std::string> f = split(line, ':', true);
    if (f.size() != 4) continue;
    for (const auto &list : {f[2], f[3]})
      for (const auto &m : split(list, ','))
        if (m == name) add("/etc/gshadow");
  }
  std::vector<std::string> sudoers = {"/etc/sudoers"};
  for (const auto &f : sudoers_dropins(root, "/etc/sudoers.d")) sudoers.push_back(f);
  for (const auto &f : sudoers)
    if (sudoers_names(read_text(root + f), name)) add(f);
  std::vector<std::string> polkit = files_with_suffix(root, "/etc/polkit-1/rules.d", ".rules");
  for (const auto &f : files_with_suffix(root, "/usr/share/polkit-1/rules.d", ".rules")) polkit.push_back(f);
  for (const auto &f : files_with_suffix(root, "/etc/polkit-1/localauthority", ".pkla", true)) polkit.push_back(f);
  for (const auto &f : files_with_suffix(root, "/var/lib/polkit-1/localauthority", ".pkla", true)) polkit.push_back(f);
  for (const auto &f : polkit) {
    std::string text = read_text(root + f);
    for (const auto &line : split(text, '\n'))
      if (!starts_with(trim(line), "//") && !starts_with(trim(line), "#") && has_word(line, name)) {
        add(f);
        break;
      }
  }
  std::vector<std::string> sshd = {"/etc/ssh/sshd_config"};
  for (const auto &f : files_with_suffix(root, "/etc/ssh/sshd_config.d", ".conf")) sshd.push_back(f);
  for (const auto &f : sshd)
    for (const auto &line : split(read_text(root + f), '\n')) {
      std::string l = lower(trim(line));
      if ((starts_with(l, "allowusers") || starts_with(l, "denyusers") || starts_with(l, "match")) &&
          has_word(line, name)) {
        add(f);
        break;
      }
    }
  for (const auto &line : split(read_text(root + "/etc/security/access.conf"), '\n'))
    if (!comment(line) && has_word(line, name)) {
      add("/etc/security/access.conf");
      break;
    }
  for (const char *f : {"/etc/cron.allow", "/etc/at.allow"})
    for (const auto &line : split(read_text(root + f), '\n'))
      if (trim(line) == name) add(f);
  return out;
}

std::string check(const std::string &root, uid_t uid, const std::string &old_name, const std::string &new_name,
                  const std::vector<Request> &others) {
  std::string why = login_name_problem(new_name);
  if (!why.empty()) return why;
  if (new_name == old_name) return format(_("The account is called %s already."), new_name.c_str());
  std::vector<Passwd> users = read_passwd(root);
  auto me = std::find_if(users.begin(), users.end(), [&](const Passwd &p) { return p.uid == uid; });
  if (me == users.end()) return _("This account does not exist (any more).");
  if (me->name != old_name) return format(_("This account is called %s now."), me->name.c_str());
  uid_t min = 0, max = 0;
  uid_range(root, &min, &max);
  if (uid < min || uid > max) return _("Only the accounts of people can be renamed here, not those of the system.");
  bool taken = std::any_of(users.begin(), users.end(), [&](const Passwd &p) { return p.name == new_name; });
  for (const auto &g : read_group(root))
    if (g.name == new_name) taken = true;
  if (root == "/" && (getpwnam(new_name.c_str()) || getgrnam(new_name.c_str()))) taken = true;
  if (taken) return format(_("The user name \"%s\" is in use already."), new_name.c_str());
  std::vector<std::string> named = mentions(root, new_name);
  if (!named.empty())
    return format(_("The name \"%s\" is named in %s, which would give this account what it grants to that "
                    "name. Choose another one."),
                  new_name.c_str(), named.front().c_str());
  for (const auto &o : others) {
    if (o.uid == uid) continue;
    if (o.new_name == new_name) return format(_("Another account will be renamed to %s already."), new_name.c_str());
    if (o.old_name == new_name)
      return format(_("%s is the name of an account that will be renamed; choose another one."), new_name.c_str());
  }
  std::string home = new_home_for(me->home, old_name, new_name);
  struct stat st;
  if (home != me->home && lstat((root + home).c_str(), &st) == 0)
    return format(_("A folder %s exists already."), home.c_str());
  return "";
}

// ============================================================ file edits

std::string edit_lightdm(const std::string &text, const std::string &old_name, const std::string &new_name) {
  return edit_keys(text, {"autologin-user", "last-user"}, old_name, new_name);
}

std::string edit_gdm(const std::string &text, const std::string &old_name, const std::string &new_name) {
  return edit_keys(text, {"AutomaticLogin", "TimedLogin"}, old_name, new_name);
}

std::string edit_subid(const std::string &text, const std::string &old_name, const std::string &new_name) {
  return each_line(text, [&](const std::string &line) {
    return starts_with(line, old_name + ":") ? new_name + line.substr(old_name.size()) : line;
  });
}

std::string edit_sudoers(const std::string &text, const std::string &old_name, const std::string &new_name) {
  return each_line(text, [&](const std::string &line) {
    std::string t = trim(line);
    if (t.empty() || comment(t) || starts_with(t, "#include") || starts_with(t, "@include")) return line;
    return replace_words(line, old_name, new_name);
  });
}

std::string edit_accountsservice(const std::string &text, const std::string &old_icon, const std::string &new_icon) {
  return edit_keys(text, {"Icon"}, old_icon, new_icon);
}

// =============================================================== applying

int run_program(const std::vector<std::string> &argv, std::string *output) {
  if (argv.empty()) return -1;
  std::string path = argv[0].find('/') == std::string::npos ? find_tool(argv[0]) : argv[0];
  if (path.empty()) {
    if (output) *output = argv[0] + ": not installed";
    return -1;
  }
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return -1;
  std::vector<char *> args;
  for (const auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  const char *envp[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", nullptr};
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  pid_t pid = -1;
  int err = posix_spawn(&pid, path.c_str(), &fa, nullptr, args.data(), const_cast<char **>(envp));
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (err != 0) {
    close(fds[0]);
    return -1;
  }
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fds[0], buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (out.size() < 65536) out.append(buf, size_t(n));
  }
  close(fds[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (output) *output = trim(out);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

bool uid_has_processes(uid_t uid) {
  for (const auto &n : list_dir("/proc")) {
    if (n.empty() || !std::all_of(n.begin(), n.end(), ::isdigit)) continue;
    std::string text;
    if (!read_file("/proc/" + n + "/status", text)) continue;
    for (const auto &line : split(text, '\n')) {
      if (!starts_with(line, "Uid:")) continue;
      for (const auto &w : words(line.substr(4)))
        if (w.second == std::to_string(uid)) return true;
      break;
    }
  }
  return false;
}

Applier::Applier(std::string root, std::string dir, Runner run)
    : root_(root == "/" ? "" : strip_slashes(std::move(root))), dir_(std::move(dir)), run_(std::move(run)) {}

std::string Applier::path(const std::string &p) const { return root_ + p; }

bool Applier::step(const std::string &line) {
  int fd = open(journal_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  std::string l = line + "\n";
  bool ok = write(fd, l.data(), l.size()) == ssize_t(l.size()) && fsync(fd) == 0;
  close(fd);
  return ok;
}

bool Applier::backup(const std::string &file, std::string *saved) {
  std::string dir = journal_.substr(0, journal_.size() - std::string(".journal").size()) + ".backup";
  mkdir(dir.c_str(), 0700);
  std::string text;
  if (!read_file(path(file), text)) return false;
  static int counter = 0;
  *saved = dir + "/" + std::to_string(++counter);
  unlink(saved->c_str());
  int fd = open(saved->c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size()) && fsync(fd) == 0;
  close(fd);
  return ok;
}

bool Applier::edit_file(const std::string &name, const std::string &file,
                        const std::function<std::string(const std::string &)> &edit, std::string *error) {
  std::string text;
  struct stat st;
  if (lstat(path(file).c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !read_file(path(file), text)) return true;
  std::string edited = edit(text);
  if (edited == text) return true;
  if (fail_at == name) {
    *error = name + ": failed on purpose (test)";
    return false;
  }
  std::string saved;
  if (!backup(file, &saved) || !step("edit\t" + file + "\t" + saved)) {
    *error = file + ": the backup cannot be made";
    return false;
  }
  if (name == "sudoers") {
    // The edited rules must be valid, or sudo would refuse every rule.
    std::string check = path(file) + ".sde-check";
    unlink(check.c_str());
    int fd = open(check.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0440);
    bool written = fd >= 0 && write(fd, edited.data(), edited.size()) == ssize_t(edited.size());
    if (fd >= 0) close(fd);
    std::string out;
    int st2 = written && !find_tool("visudo").empty() ? run_({"visudo", "-c", "-q", "-f", check}, &out) : 0;
    unlink(check.c_str());
    if (!written || st2 != 0) {
      *error = file + ": the edited rules are not valid (" + out + ")";
      return false;
    }
  }
  if (!replace_file(path(file), edited, error)) return false;
  if (log) log("edited " + file);
  return true;
}

bool Applier::move(const std::string &name, const std::string &from, const std::string &to, std::string *error) {
  struct stat st;
  if (lstat(path(from).c_str(), &st) != 0) return true;  // nothing to move
  if (fail_at == name) {
    *error = name + ": failed on purpose (test)";
    return false;
  }
  if (lstat(path(to).c_str(), &st) == 0) {
    *error = to + " exists already";
    return false;
  }
  if (!step("move\t" + from + "\t" + to)) {
    *error = "the journal cannot be written";
    return false;
  }
  if (::rename(path(from).c_str(), path(to).c_str()) != 0) {
    *error = from + ": " + strerror(errno);
    return false;
  }
  if (log) log("moved " + from + " to " + to);
  return true;
}

bool Applier::undo(const std::vector<std::string> &lines, std::string *error) {
  bool ok = true;
  // The account database is written back under its lock, as the shadow
  // tools do (only on the real root).
  bool locked = root_.empty() && lckpwdf() == 0;
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    std::vector<std::string> f = split(*it, '\t', true);
    if (f.empty()) continue;
    if (f[0] == "move" && f.size() == 3) {
      struct stat st;
      if (lstat(path(f[2]).c_str(), &st) == 0 && lstat(path(f[1]).c_str(), &st) != 0 &&
          ::rename(path(f[2]).c_str(), path(f[1]).c_str()) != 0) {
        ok = false;
        if (error) *error += f[2] + ": " + strerror(errno) + "; ";
      }
    } else if (f[0] == "edit" && f.size() == 3) {
      std::string text, e;
      if (read_file(f[2], text) && !replace_file(path(f[1]), text, &e)) {
        ok = false;
        if (error) *error += e + "; ";
      }
    } else if (f[0] == "link" && f.size() == 2) {
      struct stat st;
      if (lstat(path(f[1]).c_str(), &st) == 0 && S_ISLNK(st.st_mode)) unlink(path(f[1]).c_str());
    }
    if (log && f[0] != "begin" && f[0] != "note") log("undone: " + *it);
  }
  if (locked) ulckpwdf();
  return ok;
}

bool Applier::recover(uid_t uid, std::string *error, bool *completed) {
  journal_ = dir_ + "/" + std::to_string(uid) + ".journal";
  if (completed) *completed = false;
  std::string text;
  if (!read_file(journal_, text)) return true;
  std::vector<std::string> lines = split(text, '\n');
  bool ended = !lines.empty() && lines.back() == "end";
  if (completed) *completed = ended;
  bool ok = ended || undo(lines, error);
  std::string backups = dir_ + "/" + std::to_string(uid) + ".backup";
  for (const auto &n : list_dir(backups)) unlink((backups + "/" + n).c_str());
  rmdir(backups.c_str());
  unlink(journal_.c_str());
  return ok;
}

bool Applier::apply(const Request &r, std::string *error, bool *postponed) {
  if (postponed) *postponed = false;
  std::string root = root_.empty() ? "/" : root_;
  std::string why = check(root, r.uid, r.old_name, r.new_name, {});
  if (!why.empty()) {
    *error = why;
    return false;
  }
  if (root_.empty() && in_use && in_use(r.uid)) {
    if (postponed) *postponed = true;
    *error = format(_("%s was in use (programs of the account were running)."), r.old_name.c_str());
    return false;
  }
  std::vector<Passwd> users = read_passwd(root);
  auto found = std::find_if(users.begin(), users.end(), [&](const Passwd &p) { return p.uid == r.uid; });
  if (found == users.end()) {
    *error = _("This account does not exist (any more).");
    return false;
  }
  Passwd me = *found;
  journal_ = dir_ + "/" + std::to_string(r.uid) + ".journal";
  unlink(journal_.c_str());
  if (!step("begin\t" + std::to_string(r.uid) + "\t" + r.old_name + "\t" + r.new_name + "\t" + now_text())) {
    *error = journal_ + ": " + strerror(errno);
    return false;
  }
  if (log) log("renaming " + r.old_name + " (uid " + std::to_string(r.uid) + ") to " + r.new_name);
  std::vector<std::string> prefix;
  if (!root_.empty()) prefix = {"-P", root_};
  auto fail = [&](const std::string &e) {
    *error = e;
    if (log) log("failed: " + e + "; undoing");
    std::string text, undo_error;
    read_file(journal_, text);
    if (!undo(split(text, '\n'), &undo_error)) {
      *error += " " + format(_("Undoing it failed too: %s"), undo_error.c_str());
      if (log) log("undoing failed: " + undo_error);
      return false;  // the journal stays for the next start
    }
    std::string backups = dir_ + "/" + std::to_string(r.uid) + ".backup";
    for (const auto &n : list_dir(backups)) unlink((backups + "/" + n).c_str());
    rmdir(backups.c_str());
    unlink(journal_.c_str());
    return false;
  };
  auto crashed = [&](const char *name) {
    if (crash_after != name) return false;
    *error = std::string(name) + ": stopped on purpose (test)";
    return true;
  };

  // 1. The account (passwd, shadow, the groups it is a member of) and its
  //    home folder, when it is named after the account.
  std::string new_home = new_home_for(me.home, r.old_name, r.new_name);
  struct stat st;
  bool move_home = new_home != me.home && lstat(path(me.home).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  if (new_home != me.home && !move_home) new_home = me.home;  // no folder to move
  // The account database is saved first: undoing restores these files
  // exactly as they were (usermod would put the lines in another order).
  auto save_db = [&](const std::vector<std::string> &files) {
    for (const auto &f : files) {
      if (lstat(path(f).c_str(), &st) != 0) continue;
      std::string saved;
      if (!backup(f, &saved) || !step("edit\t" + f + "\t" + saved)) return false;
    }
    return true;
  };
  {
    if (fail_at == "usermod") return fail("usermod: failed on purpose (test)");
    if (!save_db({"/etc/passwd", "/etc/shadow", "/etc/group", "/etc/gshadow", "/etc/subuid", "/etc/subgid"}))
      return fail("the account database cannot be saved");
    if (move_home && !step("move\t" + me.home + "\t" + new_home)) return fail("the journal cannot be written");
    step("note\tusermod -l " + r.new_name + (move_home ? " -d " + new_home + " -m " : " ") + r.old_name);
    std::vector<std::string> argv = {"usermod"};
    argv.insert(argv.end(), prefix.begin(), prefix.end());
    argv.insert(argv.end(), {"-l", r.new_name});
    if (move_home) argv.insert(argv.end(), {"-d", new_home, "-m"});
    argv.push_back(r.old_name);
    std::string out;
    if (run_(argv, &out) != 0) return fail("usermod: " + out);
    if (log) log("usermod " + r.old_name + " -> " + r.new_name + (move_home ? ", home " + new_home : ""));
    if (crashed("usermod")) return false;
  }
  // 2. The user's own group (same name, its primary group).
  for (const auto &g : read_group(root)) {
    if (g.name != r.old_name || g.gid != me.gid) continue;
    if (fail_at == "groupmod") return fail("groupmod: failed on purpose (test)");
    if (!save_db({"/etc/group", "/etc/gshadow"})) return fail("the account database cannot be saved");
    step("note\tgroupmod -n " + r.new_name + " " + r.old_name);
    std::vector<std::string> argv = {"groupmod"};
    argv.insert(argv.end(), prefix.begin(), prefix.end());
    argv.insert(argv.end(), {"-n", r.new_name, r.old_name});
    std::string out;
    if (run_(argv, &out) != 0) return fail("groupmod: " + out);
    if (log) log("groupmod " + r.old_name + " -> " + r.new_name);
    if (crashed("groupmod")) return false;
  }
  std::string e;
  // 3. AccountsService: the user's file and picture.
  {
    const std::string users_dir = "/var/lib/AccountsService/users/", icons = "/var/lib/AccountsService/icons/";
    if (!move("accountsservice", users_dir + r.old_name, users_dir + r.new_name, &e)) return fail(e);
    if (lstat(path(icons + r.old_name).c_str(), &st) == 0) {
      if (!move("accountsservice", icons + r.old_name, icons + r.new_name, &e)) return fail(e);
      if (!edit_file("accountsservice", users_dir + r.new_name,
                     [&](const std::string &t) { return edit_accountsservice(t, icons + r.old_name, icons + r.new_name); },
                     &e))
        return fail(e);
    }
    if (crashed("accountsservice")) return false;
  }
  // 4. Automatic login: LightDM and GDM.
  {
    std::vector<std::string> files = {"/etc/lightdm/lightdm.conf"};
    for (const auto &f : files_with_suffix(root_, "/etc/lightdm/lightdm.conf.d", ".conf")) files.push_back(f);
    // The greeters remember the last user (lightdm-gtk-greeter, slick-greeter).
    for (const auto &d : list_dir(path("/var/lib/lightdm/.cache"))) files.push_back("/var/lib/lightdm/.cache/" + d + "/state");
    for (const auto &f : files)
      if (!edit_file("lightdm", f, [&](const std::string &t) { return edit_lightdm(t, r.old_name, r.new_name); }, &e))
        return fail(e);
    for (const char *f : {"/etc/gdm3/daemon.conf", "/etc/gdm3/custom.conf", "/etc/gdm/custom.conf"})
      if (!edit_file("gdm", f, [&](const std::string &t) { return edit_gdm(t, r.old_name, r.new_name); }, &e))
        return fail(e);
  }
  // 5. The crontab and the mail spool.
  if (!move("crontab", "/var/spool/cron/crontabs/" + r.old_name, "/var/spool/cron/crontabs/" + r.new_name, &e))
    return fail(e);
  if (!move("mail", "/var/mail/" + r.old_name, "/var/mail/" + r.new_name, &e)) return fail(e);
  if (crashed("mail")) return false;
  // 6. Subordinate user and group IDs (containers).
  if (!edit_file("subuid", "/etc/subuid", [&](const std::string &t) { return edit_subid(t, r.old_name, r.new_name); },
                 &e))
    return fail(e);
  if (!edit_file("subgid", "/etc/subgid", [&](const std::string &t) { return edit_subid(t, r.old_name, r.new_name); },
                 &e))
    return fail(e);
  // 7. sudo rules of their own (drop-ins; /etc/sudoers is the sudo
  //    package's file and only reported).
  for (const auto &f : sudoers_dropins(root_, "/etc/sudoers.d"))
    if (!edit_file("sudoers", f, [&](const std::string &t) { return edit_sudoers(t, r.old_name, r.new_name); }, &e))
      return fail(e);
  if (crashed("sudoers")) return false;
  // 8. systemd's linger flag (their services run without a session).
  if (!move("linger", "/var/lib/systemd/linger/" + r.old_name, "/var/lib/systemd/linger/" + r.new_name, &e))
    return fail(e);
  // 9. A link from the old home folder, for programs that remember it.
  if (r.link && move_home) {
    if (fail_at == "link") return fail("link: failed on purpose (test)");
    if (!step("link\t" + me.home)) return fail("the journal cannot be written");
    if (symlink(basename_of(new_home).c_str(), path(me.home).c_str()) != 0)
      return fail(me.home + ": " + strerror(errno));
    if (log) log("linked " + me.home + " to " + new_home);
  }
  if (crashed("link")) return false;
  // What still names the old account (not changed here).
  for (const auto &f : mentions(root, r.old_name))
    if (log) log("note: " + f + " still names " + r.old_name);
  if (fail_at == "end") return fail("end: failed on purpose (test)");
  step("end");
  std::string backups = dir_ + "/" + std::to_string(r.uid) + ".backup";
  for (const auto &n : list_dir(backups)) unlink((backups + "/" + n).c_str());
  rmdir(backups.c_str());
  unlink(journal_.c_str());
  if (log) log("renamed " + r.old_name + " to " + r.new_name);
  return true;
}

}  // namespace account_rename
}  // namespace sde
