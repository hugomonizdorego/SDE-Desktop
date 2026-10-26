// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// debrun-helper: the root part of Debrun, started by pkexec (polkit actions
// org.debion.sde.debrun.install, .remove and .refresh, auth_admin_keep).
// It accepts only the forms of helper.h, runs no shell and passes argument
// vectors to apt-get. Threat model (docs/DEBRUN.md):
//
//  * Debrun and everything the user runs are untrusted: arguments are
//    validated here again, the .deb is opened with the user's own rights
//    (so root reads nothing on their behalf that they could not read), not
//    through a final symbolic link, and copied to a directory only root can
//    write to before anything looks at it; the copy must have the SHA-256
//    digest and the Package, Version and Architecture the user confirmed.
//  * What the user saw is what happens: the change is simulated again as
//    root and refused when it would remove a package the user was not shown,
//    an essential or protected package, or SDE itself. Held packages and
//    downgrades are never forced.
//  * Every request and its outcome is logged to the system log (authpriv);
//    APT and dpkg keep their own logs (/var/log/apt, /var/log/dpkg.log).
//  * The environment is replaced (PATH, locale, DEBIAN_FRONTEND), the copy
//    is removed afterwards and copies older than a day are cleaned up.
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
// <spawn.h> would find SDE's own spawn.h (src/common is on the include path).
#include <../include/spawn.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "deb.h"
#include "helper.h"
#include "sha256.h"
#include "system.h"
#include "util.h"

extern "C" char **environ;

using namespace sde;
using namespace sde::debrun;

namespace {

const char *const kCacheParent = "/var/cache/sde";
const char *const kCacheDir = "/var/cache/sde/debrun";
const uint64_t kMaxPackage = 8ull << 30;

uid_t g_caller = 0;
helper::Request g_req;
std::string g_copy_dir, g_copy;

void emit(const std::string &line) {
  // Debrun may have gone away: the change still runs to its end.
  std::string l = line;
  for (auto &c : l)
    if ((unsigned char)c < 0x20 && c != '\t') c = ' ';
  l += "\n";
  ssize_t ignored = write(1, l.data(), l.size());
  (void)ignored;
}

void log_event(int priority, const std::string &text) {
  syslog(priority, "uid %u: %s %s: %s", unsigned(g_caller), g_req.action.c_str(),
         g_req.package.empty() ? "-" : g_req.package.c_str(), text.c_str());
}

int finish(const char *result, int code) {
  if (!g_copy.empty()) unlink(g_copy.c_str());
  if (!g_copy_dir.empty()) rmdir(g_copy_dir.c_str());
  emit(std::string("RESULT ") + result);
  log_event(code == 0 ? LOG_NOTICE : LOG_WARNING, result);
  closelog();
  return code;
}

int refuse(const std::string &code, const std::string &text) {
  emit("ERROR " + code + " " + text);
  log_event(LOG_WARNING, "refused (" + code + "): " + text);
  return finish("refused", 2);
}

int failed(const std::string &code, const std::string &text) {
  emit("ERROR " + code + " " + text);
  log_event(LOG_WARNING, "failed (" + code + "): " + text);
  return finish("failed", 1);
}

// A directory owned by root that nobody else can write to.
bool root_dir(const char *path, mode_t mode) {
  if (mkdir(path, mode) != 0 && errno != EEXIST) return false;
  struct stat st;
  if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0) return false;
  if ((st.st_mode & 07777) != mode && chmod(path, mode) != 0) return false;
  return true;
}

// Copies left by a helper that was killed.
void clean_stale() {
  DIR *d = opendir(kCacheDir);
  if (!d) return;
  time_t now = time(nullptr);
  while (struct dirent *e = readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    std::string dir = std::string(kCacheDir) + "/" + name;
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || now - st.st_mtime < 24 * 3600) continue;
    unlink((dir + "/package.deb").c_str());
    rmdir(dir.c_str());
  }
  closedir(d);
}

// Opens |path| with the rights of the calling user.
int open_as_caller(const std::string &path, int *err) {
  struct passwd *pw = getpwuid(g_caller);
  bool drop = g_caller != 0;
  if (drop) {
    if (!pw || initgroups(pw->pw_name, pw->pw_gid) != 0 || setegid(pw->pw_gid) != 0 || seteuid(g_caller) != 0) {
      *err = EPERM;
      if (geteuid() != 0 && seteuid(0) != 0) _exit(3);
      return -1;
    }
  }
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  *err = errno;
  if (drop) {
    // Root again, or nothing more happens at all.
    if (seteuid(0) != 0 || setegid(0) != 0 || setgroups(0, nullptr) != 0) _exit(3);
  }
  return fd;
}

// Runs apt-get: its output becomes LOG lines, its status file descriptor
// PROGRESS lines. Returns the exit code (-1: it did not run) and keeps the
// output for the error messages.
int run_apt(const std::vector<std::string> &argv, std::string *output) {
  std::string path = find_program(argv[0]);
  if (path.empty()) return -1;
  int out[2], status[2];
  if (pipe2(out, O_CLOEXEC) != 0) return -1;
  if (pipe2(status, O_CLOEXEC) != 0) {
    close(out[0]);
    close(out[1]);
    return -1;
  }
  std::vector<char *> args;
  for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, out[1], 1);
  posix_spawn_file_actions_adddup2(&fa, out[1], 2);
  posix_spawn_file_actions_adddup2(&fa, status[1], 3);
  pid_t pid = -1;
  int e = posix_spawn(&pid, path.c_str(), &fa, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(out[1]);
  close(status[1]);
  if (e != 0) {
    close(out[0]);
    close(status[0]);
    return -1;
  }
  std::string buf[2];
  int fds[2] = {out[0], status[0]};
  bool open_fd[2] = {true, true};
  int last_percent = -2;
  std::string last_phase;
  auto line_out = [&](int which, const std::string &line) {
    if (which == 0) {
      if (output->size() < (8u << 20)) *output += line + "\n";
      if (line.find("Waiting for cache lock") != std::string::npos ||
          line.find("Waiting for lock") != std::string::npos) {
        if (last_phase != "wait") emit("PHASE wait");
        last_phase = "wait";
      }
      emit("LOG " + line);
      return;
    }
    helper::Progress p;
    if (!helper::parse_status_line(line, g_req.action, &p)) return;
    if (!p.error.empty()) {
      emit("LOG dpkg: " + p.package + ": " + p.error);
      return;
    }
    if (p.percent != last_percent || p.phase != last_phase) {
      emit("PROGRESS " + std::to_string(p.percent) + " " + p.phase + (p.package.empty() ? "" : " " + p.package));
      last_percent = p.percent;
      last_phase = p.phase;
    }
  };
  while (open_fd[0] || open_fd[1]) {
    struct pollfd p[2];
    int n = 0, map[2];
    for (int i = 0; i < 2; ++i)
      if (open_fd[i]) {
        p[n] = {fds[i], POLLIN, 0};
        map[n++] = i;
      }
    int r = poll(p, nfds_t(n), -1);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) break;
    for (int k = 0; k < n; ++k) {
      if (!(p[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int i = map[k];
      char chunk[4096];
      ssize_t got = read(fds[i], chunk, sizeof chunk);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) {
        open_fd[i] = false;
        if (!buf[i].empty()) line_out(i, buf[i]);
        buf[i].clear();
        continue;
      }
      buf[i].append(chunk, size_t(got));
      size_t nl;
      while ((nl = buf[i].find_first_of("\n\r")) != std::string::npos) {
        std::string line = buf[i].substr(0, nl);
        buf[i].erase(0, nl + 1);
        if (!trim(line).empty()) line_out(i, line);
      }
    }
  }
  close(out[0]);
  close(status[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// The options shared by the simulation and the change.
std::vector<std::string> apt_options() {
  return {"-q", "-o", "Dpkg::Use-Pty=0", "-o", "APT::Color=0", "-o", "DPkg::Lock::Timeout=300",
          "-o", "Dpkg::Options::=--force-confdef", "-o", "Dpkg::Options::=--force-confold"};
}

// ERROR code of a failed apt-get run.
std::string apt_error_code(const AptPlan &plan, const std::string &output) {
  std::string l = lower(output);
  if (l.find("enough free space") != std::string::npos || l.find("no space left") != std::string::npos)
    return "space";
  switch (plan.problem) {
    case AptPlan::Problem::unmet: return "unmet";
    case AptPlan::Problem::held: return "held";
    case AptPlan::Problem::essential: return "essential";
    case AptPlan::Problem::lock: return "lock";
    default: break;
  }
  if (l.find("does not match system") != std::string::npos ||
      l.find("package architecture") != std::string::npos)
    return "arch";
  return "failed";
}

std::string first_error(const AptPlan &plan, const std::string &output) {
  if (!plan.unmet.empty()) return plan.unmet.front();
  if (!plan.errors.empty()) return plan.errors.front();
  std::vector<std::string> lines = split(output, '\n');
  for (auto it = lines.rbegin(); it != lines.rend(); ++it)
    if (!trim(*it).empty()) return trim(*it);
  return "apt-get failed";
}

// The simulation as root: the removals must be ones the user saw.
int check_plan(const std::vector<std::string> &simulate) {
  int status = 0;
  std::string out = run_capture(simulate, &status, 300);
  AptPlan plan = parse_apt_output(out, status);
  if (!plan.ran) return failed("failed", "apt-get could not be run");
  if (!plan.ok) return refuse(apt_error_code(plan, out), first_error(plan, out));
  System sys;
  std::string error;
  load_system(&sys, &error);
  for (const AptItem &r : plan.remove) {
    std::string name = r.name.substr(0, r.name.find(':'));
    if (helper::protected_package(name))
      return refuse("essential", "it would remove " + name + ", a part of the desktop");
    for (const Installed *i : sys.find(name))
      if (i->essential || i->is_protected)
        return refuse("essential", "it would remove the essential package " + name);
    if (name != g_req.package &&
        std::find(g_req.allow_remove.begin(), g_req.allow_remove.end(), name) == g_req.allow_remove.end())
      return refuse("removals", "it would also remove " + name);
    if (name == g_req.package && g_req.action == "install")
      return refuse("removals", "it would remove " + name);
  }
  return 0;
}

int do_install() {
  emit("PHASE copy");
  if (!root_dir(kCacheParent, 0755) || !root_dir(kCacheDir, 0711))
    return failed("failed", std::string("cannot prepare ") + kCacheDir);
  clean_stale();
  int err = 0;
  int src = open_as_caller(g_req.file, &err);
  if (src < 0)
    return refuse("open", err == ELOOP ? "the file is a symbolic link" : std::string(strerror(err)));
  struct stat st;
  if (fstat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(src);
    return refuse("open", "not a regular file");
  }
  if (uint64_t(st.st_size) > kMaxPackage) {
    close(src);
    return refuse("open", "the file is too large");
  }
  char tmpl[] = "/var/cache/sde/debrun/XXXXXX";
  if (!mkdtemp(tmpl) || chmod(tmpl, 0711) != 0) {
    close(src);
    return failed("failed", std::string("cannot create a directory in ") + kCacheDir);
  }
  g_copy_dir = tmpl;
  g_copy = g_copy_dir + "/package.deb";
  int dst = open(g_copy.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (dst < 0) {
    close(src);
    return failed("failed", "cannot create the copy");
  }
  Sha256 hash;
  char buf[1 << 16];
  bool ok = true;
  for (;;) {
    ssize_t n = read(src, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    hash.update(buf, size_t(n));
    for (ssize_t done = 0; done < n;) {
      ssize_t w = write(dst, buf + done, size_t(n - done));
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        ok = false;
        break;
      }
      done += w;
    }
    if (!ok) break;
  }
  close(src);
  if (fsync(dst) != 0) ok = false;
  close(dst);
  if (!ok) return failed("space", "the package could not be copied");
  emit("PHASE check");
  std::string digest = hash.hex_digest();
  if (digest != g_req.sha256)
    return refuse("changed", "the file changed after it was shown (SHA-256 " + digest + ")");
  // The control fields of the copy, as root, from the root-owned copy.
  int fd = open(g_copy.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return failed("failed", "cannot read the copy");
  Package p;
  std::string error;
  ReadOptions opt;
  opt.hash = false;
  opt.data = false;
  bool read = read_package_fd(fd, g_copy, &p, &error, opt);
  close(fd);
  if (!read) return refuse("changed", error);
  if (p.name != g_req.package || p.version != g_req.version || p.architecture != g_req.arch)
    return refuse("changed", "the package is " + p.name + " " + p.version + " " + p.architecture);
  syslog(LOG_NOTICE, "uid %u: install %s %s %s from %s (sha256 %s)", unsigned(g_caller), p.name.c_str(),
         p.version.c_str(), p.architecture.c_str(), g_req.file.c_str(), digest.c_str());

  if (int r = check_plan(simulate_install_argv(g_copy, g_req.reinstall, g_req.downgrade))) return r;

  std::vector<std::string> argv = {"apt-get", "install", "-y", "-o", "APT::Status-Fd=3"};
  for (auto &o : apt_options()) argv.push_back(o);
  if (g_req.reinstall) argv.push_back("--reinstall");
  if (g_req.downgrade) argv.push_back("--allow-downgrades");
  if (g_req.allow_remove.empty()) argv.push_back("--no-remove");
  argv.push_back(g_copy);
  emit("PROGRESS 0 download");
  std::string output;
  int code = run_apt(argv, &output);
  if (code != 0) {
    AptPlan plan = parse_apt_output(output, code < 0 ? 1 : code);
    return failed(apt_error_code(plan, output), first_error(plan, output));
  }
  emit("PROGRESS 100 configure");
  return finish("ok", 0);
}

int do_remove() {
  if (helper::protected_package(g_req.package))
    return refuse("essential", g_req.package + " is a part of the desktop");
  if (int r = check_plan(simulate_remove_argv(g_req.package))) return r;
  std::vector<std::string> argv = {"apt-get", "remove", "-y", "-o", "APT::Status-Fd=3"};
  for (auto &o : apt_options()) argv.push_back(o);
  argv.push_back(g_req.package);
  emit("PHASE remove");
  std::string output;
  int code = run_apt(argv, &output);
  if (code != 0) {
    AptPlan plan = parse_apt_output(output, code < 0 ? 1 : code);
    return failed(apt_error_code(plan, output), first_error(plan, output));
  }
  return finish("ok", 0);
}

int do_refresh() {
  emit("PHASE refresh");
  std::vector<std::string> argv = {"apt-get", "update", "-q", "-o", "APT::Status-Fd=3", "-o", "APT::Color=0",
                                   "-o", "DPkg::Lock::Timeout=300"};
  std::string output;
  int code = run_apt(argv, &output);
  if (code != 0) {
    AptPlan plan = parse_apt_output(output, code < 0 ? 1 : code);
    return failed(apt_error_code(plan, output), first_error(plan, output));
  }
  return finish("ok", 0);
}

}  // namespace

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  umask(022);
  emit(helper::kProtocol);
  // pkexec names the user who asked; root may run it directly.
  const char *pk = getenv("PKEXEC_UID");
  g_caller = getuid();
  if (pk && *pk) {
    char *end = nullptr;
    unsigned long v = strtoul(pk, &end, 10);
    if (end && *end == '\0' && v < 0xFFFFFFFFul) g_caller = uid_t(v);
  }
  openlog("debrun-helper", LOG_PID, LOG_AUTHPRIV);
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string error;
  if (!helper::parse_request(args, &g_req, &error)) return refuse("usage", error);
  if (geteuid() != 0) return refuse("root", "debrun-helper must run as root (through pkexec)");
  // A clean environment for APT and the maintainer scripts.
  clearenv();
  setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
  setenv("LC_ALL", "C.UTF-8", 1);
  setenv("HOME", "/root", 1);
  setenv("DEBIAN_FRONTEND", "noninteractive", 1);
  setenv("APT_LISTCHANGES_FRONTEND", "none", 1);
  setenv("APT_LISTBUGS_FRONTEND", "none", 1);
  if (chdir("/") != 0) return failed("failed", "cannot change to /");
  log_event(LOG_NOTICE, g_req.action == "install" ? "requested for " + g_req.file : "requested");
  if (g_req.action == "install") return do_install();
  if (g_req.action == "remove") return do_remove();
  return do_refresh();
}
