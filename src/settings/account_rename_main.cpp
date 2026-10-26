// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-account-rename (/usr/libexec/sde): renames user accounts at the next
// start of the computer (account_rename.h, docs/CONTROL-CENTER.md, man
// sde-account-rename(8)).
//
//   schedule-self --uid UID --old NAME --new NAME [--link]
//   schedule      --uid UID --old NAME --new NAME [--link]
//       records the rename of account UID (now NAME) to the new name, after
//       checking it; "-self" for one's own account (polkit action
//       org.debion.sde.account-rename.self: the user's own password), else
//       an administrator's (org.debion.sde.account-rename.other).
//   cancel-self --uid UID | cancel --uid UID
//       forgets the request (and a failed one).
//   apply
//       makes the renames (sde-account-rename.service, early at boot, as
//       root; never through pkexec).
//   status
//       prints the requests.
//
// Only root runs it: pkexec (PKEXEC_UID names the caller) or systemd.
// --root DIR (a fake root) and --dir DIR (the requests) are for the tests
// and refused under pkexec. Arguments only, no shell; the environment is
// cleared. Output: "OK ..." or "ERROR CODE text" lines; exit status 0 done,
// 1 refused or failed, 2 usage.
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "account_rename.h"
#include "build_config.h"
#include "i18n.h"
#include "util.h"

using namespace sde;
using namespace sde::account_rename;

namespace {

std::string g_log_file;

void log_line(const std::string &text) {
  syslog(LOG_NOTICE, "%s", text.c_str());
  if (g_log_file.empty()) return;
  int fd = open(g_log_file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0640);
  if (fd < 0) return;
  char when[32];
  time_t t = time(nullptr);
  strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", localtime(&t));
  std::string line = std::string(when) + " " + text + "\n";
  if (write(fd, line.data(), line.size()) < 0) {
  }
  close(fd);
}

int refuse(const std::string &code, const std::string &text) {
  printf("ERROR %s %s\n", code.c_str(), text.c_str());
  log_line("refused (" + code + "): " + text);
  return 1;
}

int usage() {
  fprintf(stderr,
          "Usage: sde-account-rename schedule[-self] --uid UID --old NAME --new NAME [--link]\n"
          "       sde-account-rename cancel[-self] --uid UID\n"
          "       sde-account-rename apply | status\n");
  return 2;
}

bool parse_uid(const std::string &s, uid_t *out) {
  if (s.empty() || s.size() > 10) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  *out = uid_t(strtoul(s.c_str(), nullptr, 10));
  return true;
}

// The request directory, made (root's, readable by everyone: the Control
// Center shows the requests).
bool request_dir(const std::string &dir) {
  struct stat st;
  if (lstat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  std::string parent = dir.substr(0, dir.rfind('/'));
  if (!parent.empty()) make_dirs(parent);
  return mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  umask(022);
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  if (cmd == "--version") {
    printf("sde-account-rename %s\n", SDE_VERSION);
    return 0;
  }
  if (cmd == "--help" || cmd == "-h") {
    usage();
    return 0;
  }
  // Who asks: pkexec's caller, else whoever runs it.
  const char *pk = getenv("PKEXEC_UID");
  bool via_pkexec = pk && *pk;
  uid_t caller = getuid();
  if (via_pkexec && !parse_uid(pk, &caller)) return refuse("usage", "bad PKEXEC_UID");
  std::string lang = env("LANG"), language = env("LANGUAGE");
  clearenv();
  setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
  if (!lang.empty()) setenv("LANG", lang.c_str(), 1);  // the messages, in the caller's language
  if (!language.empty()) setenv("LANGUAGE", language.c_str(), 1);
  openlog("sde-account-rename", LOG_PID, LOG_AUTHPRIV);

  uid_t uid = 0;
  bool have_uid = false, link = false;
  std::string old_name, new_name, root = "/", dir = "/var/lib/sde/account-renames", fail_at;
  bool test_paths = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](std::string *out) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--uid" && value(&v) && parse_uid(v, &uid)) {
      have_uid = true;
    } else if (a == "--old" && value(&old_name)) {
    } else if (a == "--new" && value(&new_name)) {
    } else if (a == "--link") {
      link = true;
    } else if (a == "--root" && value(&root)) {
      test_paths = true;
    } else if (a == "--dir" && value(&dir)) {
      test_paths = true;
    } else if (a == "--fail-at" && value(&fail_at)) {
      test_paths = true;
    } else {
      return usage();
    }
  }
  if (test_paths && via_pkexec) return refuse("usage", "--root, --dir and --fail-at are for tests");

  // The requests are readable by everyone.
  if (cmd == "status") {
    for (const auto &r : load_requests(dir))
      printf("%s uid=%u old=%s new=%s home=%s link=%d%s%s\n", r.state.c_str(), unsigned(r.uid), r.old_name.c_str(),
             r.new_name.c_str(), r.new_home.c_str(), r.link ? 1 : 0, r.error.empty() ? "" : " error=",
             r.error.c_str());
    return 0;
  }

  if (!test_paths && geteuid() != 0) return refuse("root", "sde-account-rename runs as root (pkexec)");
  if (root.empty() || root[0] != '/' || dir.empty() || dir[0] != '/') return usage();
  g_log_file = test_paths ? dir + "/rename.log" : "/var/log/sde-account-rename.log";

  if (cmd == "schedule" || cmd == "schedule-self") {
    if (!have_uid || old_name.empty() || new_name.empty()) return usage();
    if (cmd == "schedule-self" && uid != caller)
      return refuse("self", "schedule-self renames the caller's own account only");
    if (!request_dir(dir)) return refuse("dir", dir + ": " + strerror(errno));
    std::vector<Request> all = load_requests(dir);
    std::string why = check(root, uid, old_name, new_name, all);
    if (!why.empty()) return refuse("name", why);
    Request r;
    r.uid = uid;
    r.old_name = old_name;
    r.new_name = new_name;
    for (const auto &p : read_passwd(root))
      if (p.uid == uid) r.old_home = p.home;
    r.new_home = new_home_for(r.old_home, old_name, new_name);
    r.link = link && r.new_home != r.old_home;
    r.by = caller;
    r.time = (long long)time(nullptr);
    std::string error;
    if (!save_request(dir, r, &error)) return refuse("write", error);
    log_line("scheduled: " + old_name + " (uid " + std::to_string(uid) + ") to " + new_name + ", home " + r.new_home +
             (r.link ? " with a link" : "") + ", asked by uid " + std::to_string(caller));
    printf("OK scheduled %s %s\n", old_name.c_str(), new_name.c_str());
    return 0;
  }

  if (cmd == "cancel" || cmd == "cancel-self") {
    if (!have_uid) return usage();
    if (cmd == "cancel-self" && uid != caller) return refuse("self", "cancel-self cancels the caller's own rename only");
    std::string path = request_path(dir, uid);
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
      printf("OK nothing to cancel\n");
      return 0;
    }
    if (unlink(path.c_str()) != 0) return refuse("write", path + ": " + strerror(errno));
    log_line("cancelled: the rename of uid " + std::to_string(uid) + ", by uid " + std::to_string(caller));
    printf("OK cancelled\n");
    return 0;
  }

  if (cmd == "apply") {
    if (via_pkexec) return refuse("usage", "apply runs at boot (sde-account-rename.service)");
    if (geteuid() != 0) return refuse("root", "apply runs as root");
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022))
      return refuse("dir", dir + " is not a directory of root's only");
    Applier a(root, dir);
    a.log = log_line;
    a.fail_at = fail_at;
    if (root == "/") a.in_use = uid_has_processes;
    int failures = 0;
    // A rename that a crash interrupted is undone first.
    for (const auto &n : list_dir(dir)) {
      if (!ends_with(n, ".journal")) continue;
      uid_t u = 0;
      if (!parse_uid(n.substr(0, n.size() - 8), &u)) continue;
      std::string error;
      bool completed = false;
      bool undone = a.recover(u, &error, &completed);
      if (completed) {
        // It was made; only the request was left.
        log_line("the rename of uid " + std::to_string(u) + " was made before the computer stopped");
        unlink(request_path(dir, u).c_str());
        continue;
      }
      log_line("interrupted rename of uid " + std::to_string(u) + (undone ? " undone" : " not undone: " + error));
      for (auto r : load_requests(dir))
        if (r.uid == u && r.state == "pending") {
          r.state = "failed";
          r.error = undone ? _("The rename was interrupted (the computer stopped) and undone.")
                           : format(_("The rename was interrupted and could not be undone completely: %s"),
                                    error.c_str());
          save_request(dir, r, &error);
        }
    }
    for (auto r : load_requests(dir)) {
      if (r.state != "pending") continue;
      // Requests written by root only (the directory is root's).
      std::string path = request_path(dir, r.uid);
      if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0) continue;
      std::string error;
      bool postponed = false;
      if (a.apply(r, &error, &postponed)) {
        unlink(path.c_str());
        printf("OK renamed %s %s\n", r.old_name.c_str(), r.new_name.c_str());
      } else if (postponed) {
        log_line("postponed: " + error);
        printf("OK postponed %s\n", r.old_name.c_str());
      } else {
        ++failures;
        r.state = "failed";
        r.error = error;
        save_request(dir, r, &error);
        printf("ERROR failed %s: %s\n", r.old_name.c_str(), r.error.c_str());
      }
    }
    return failures ? 1 : 0;
  }
  return usage();
}
