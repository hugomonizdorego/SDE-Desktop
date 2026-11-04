// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-fingerprint-pam (/usr/libexec/sde): where the system's PAM stack asks
// for a fingerprint (Control Center > Fingerprint, "Use for";
// docs/FINGERPRINT.md, fingerprint_pam.h).
//
//   set --sign-in on|off --admin on|off
//       enables and disables pam-auth-update's fingerprint profiles so that
//       the login screen and consoles (sign-in) and sudo and polkit
//       (admin) ask for a finger, or not: Debian's "fprintd" profile for
//       both, SDE's sde-fingerprint-sign-in or sde-fingerprint-admin for
//       one. Through pkexec: polkit action org.debion.sde.fingerprint.configure
//       (an administrator's password).
//   status
//       prints what common-auth does now.
//
// Only pam-auth-update writes the PAM configuration; it runs with
// arguments only, a cleared environment and debconf's noninteractive
// frontend, so it never asks questions and keeps the administrator's own
// changes (it refuses to touch common-auth files that were edited by hand,
// which is reported). The result is checked by reading common-auth again.
// --root DIR (a fake root; pam-auth-update --root) is for the tests and
// refused under pkexec. Output: "OK ..." or "ERROR CODE text" lines; exit
// status 0 done, 1 refused or failed, 2 usage.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
// <spawn.h> would find SDE's own spawn.h (src/common is on the include path).
#include <../include/spawn.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "build_config.h"
#include "fingerprint_pam.h"
#include "i18n.h"
#include "util.h"

extern char **environ;

using namespace sde;
using namespace sde::fingerprint_pam;

namespace {

const char *kPamAuthUpdate = "/usr/sbin/pam-auth-update";

int refuse(const std::string &code, const std::string &text) {
  // One line: the Control Center shows the text.
  std::string t = text;
  for (char &c : t)
    if (c == '\n' || c == '\r') c = ' ';
  printf("ERROR %s %s\n", code.c_str(), t.c_str());
  syslog(LOG_NOTICE, "refused (%s): %s", code.c_str(), t.c_str());
  return 1;
}

int usage() {
  fprintf(stderr,
          "Usage: sde-fingerprint-pam set --sign-in on|off --admin on|off\n"
          "       sde-fingerprint-pam status\n");
  return 2;
}

bool parse_switch(const std::string &v, bool *out) {
  if (v == "on" || v == "yes" || v == "true" || v == "1") {
    *out = true;
    return true;
  }
  if (v == "off" || v == "no" || v == "false" || v == "0") {
    *out = false;
    return true;
  }
  return false;
}

const char *on_off(bool v) { return v ? "on" : "off"; }

std::string joined(const std::vector<std::string> &v, const char *sep = ",") {
  std::string out;
  for (const std::string &s : v) out += (out.empty() ? "" : sep) + s;
  return out;
}

// Runs |argv| with the cleared environment, standard input from /dev/null,
// for at most |timeout| seconds; returns its exit status (-1: could not
// run, or killed) and its output (both streams, at most 64 KiB).
int run(const std::vector<std::string> &argv, std::string *out, int timeout) {
  std::vector<char *> a;
  for (const std::string &s : argv) a.push_back(const_cast<char *>(s.c_str()));
  a.push_back(nullptr);
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return -1;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  pid_t pid = -1;
  int err = posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, a.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (err != 0) {
    close(fds[0]);
    *out = argv[0] + ": " + strerror(err);
    return -1;
  }
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  bool killed = false;
  char buf[4096];
  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long left = timeout * 1000L - ((now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L);
    if (left <= 0) {
      kill(pid, SIGKILL);  // our own child
      killed = true;
      break;
    }
    struct pollfd p = {fds[0], POLLIN, 0};
    int r = poll(&p, 1, int(std::min(left, 1000L)));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) continue;
    ssize_t n = read(fds[0], buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (out->size() < 65536) out->append(buf, size_t(n));
  }
  close(fds[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (killed) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// The last lines of |out| that say something.
std::string tail(const std::string &out) {
  std::vector<std::string> lines;
  for (const std::string &l : split(out, '\n'))
    if (!trim(l).empty()) lines.push_back(trim(l));
  if (lines.size() > 4) lines.erase(lines.begin(), lines.end() - 4);
  return joined(lines, " ");
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  umask(022);
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  if (cmd == "--version") {
    printf("sde-fingerprint-pam %s\n", SDE_VERSION);
    return 0;
  }
  if (cmd == "--help" || cmd == "-h") {
    usage();
    return 0;
  }
  const char *pk = getenv("PKEXEC_UID");
  bool via_pkexec = pk && *pk;
  std::string caller = via_pkexec ? std::string(pk) : std::to_string(getuid());
  std::string lang = env("LANG"), language = env("LANGUAGE"), debconf_rc = env("DEBCONF_SYSTEMRC");
  clearenv();
  setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
  if (!lang.empty()) setenv("LANG", lang.c_str(), 1);  // the messages, in the caller's language
  if (!language.empty()) setenv("LANGUAGE", language.c_str(), 1);
  openlog("sde-fingerprint-pam", LOG_PID, LOG_AUTHPRIV);

  bool sign_in = false, admin = false, have_sign_in = false, have_admin = false;
  std::string root = "/";
  bool test_root = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](std::string *out) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--sign-in" && value(&v) && parse_switch(v, &sign_in)) {
      have_sign_in = true;
    } else if (a == "--admin" && value(&v) && parse_switch(v, &admin)) {
      have_admin = true;
    } else if (a == "--root" && value(&root)) {
      test_root = true;
    } else {
      return usage();
    }
  }
  if (test_root && via_pkexec) return refuse("usage", "--root is for tests");
  if (root.empty() || root[0] != '/') return usage();

  if (cmd == "status") {
    State s = read_state(root);
    printf("debian-pam=%s module=%s debian-profile=%s sde-profiles=%s\n", on_off(s.debian_pam), on_off(s.module),
           on_off(s.debian_profile), on_off(s.sde_profiles));
    printf("enabled=%s by-hand=%s\n", joined(s.enabled).c_str(), on_off(s.by_hand));
    printf("sign-in=%s admin=%s\n", on_off(s.sign_in), on_off(s.admin));
    return 0;
  }
  if (cmd != "set" || !have_sign_in || !have_admin) return usage();
  if (!test_root && geteuid() != 0) return refuse("root", "sde-fingerprint-pam runs as root (pkexec)");
  if (!is_executable(kPamAuthUpdate))
    return refuse("pam", _("pam-auth-update is not installed (package libpam-runtime)."));

  State now = read_state(root);
  Plan p = plan(now, sign_in, admin);
  if (!p.error.empty()) return refuse("pam", p.error);
  if (p.nothing()) {
    printf("OK sign-in=%s admin=%s\n", on_off(now.sign_in), on_off(now.admin));
    return 0;
  }
  // Never a question: noninteractive debconf, which keeps the default
  // answers (pam-auth-update then leaves hand-made files alone).
  setenv("DEBIAN_FRONTEND", "noninteractive", 1);
  if (test_root && !debconf_rc.empty()) setenv("DEBCONF_SYSTEMRC", debconf_rc.c_str(), 1);
  std::vector<std::string> args = {kPamAuthUpdate};
  for (const std::string &a : pam_auth_update_args(p, root)) args.push_back(a);
  std::string out;
  int status = run(args, &out, 120);
  State after = read_state(root);
  if (status != 0)
    return refuse("pam-auth-update", format(_("pam-auth-update failed (status %d): %s"), status, tail(out).c_str()));
  if (after.sign_in != sign_in || after.admin != admin) {
    std::string why = tail(out);
    return refuse("not-changed", why.empty() ? std::string(_("pam-auth-update did not change common-auth."))
                                             : format(_("pam-auth-update did not change common-auth: %s"),
                                                      why.c_str()));
  }
  syslog(LOG_NOTICE, "fingerprint authentication: sign-in %s, administrator %s (enabled %s, disabled %s; uid %s)",
         on_off(sign_in), on_off(admin), joined(p.enable).c_str(), joined(p.disable).c_str(), caller.c_str());
  printf("OK sign-in=%s admin=%s\n", on_off(after.sign_in), on_off(after.admin));
  return 0;
}
