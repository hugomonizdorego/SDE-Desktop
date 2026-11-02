// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "process_rules.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

#include "util.h"

using namespace sde;

namespace forceclose {

namespace {

bool numeric(const char *s) {
  if (!*s) return false;
  for (; *s; ++s)
    if (!isdigit(static_cast<unsigned char>(*s))) return false;
  return true;
}

std::string base(std::string path) {
  // A program replaced on disk while it runs (an upgrade): "/usr/bin/x (deleted)".
  const std::string deleted = " (deleted)";
  if (path.size() > deleted.size() && path.compare(path.size() - deleted.size(), deleted.size(), deleted) == 0)
    path.resize(path.size() - deleted.size());
  return basename_of(path);
}

// "pid (comm) state ppid pgrp session ... starttime": the name may hold
// spaces and brackets, so the fields are counted after the last ')'.
bool parse_stat(const std::string &stat, Process &p) {
  size_t close = stat.rfind(')');
  if (close == std::string::npos) return false;
  std::istringstream in(stat.substr(close + 1));
  std::string f;
  for (int i = 3; i <= 22 && in >> f; ++i) {
    if (i == 3) p.state = f.empty() ? '?' : f[0];
    else if (i == 4) p.ppid = atol(f.c_str());
    else if (i == 6) p.session = atol(f.c_str());
    else if (i == 22) p.start = strtoull(f.c_str(), nullptr, 10);
  }
  return p.start != 0;
}

std::vector<long> all_pids(const std::string &proc) {
  std::vector<long> out;
  DIR *d = opendir(proc.c_str());
  if (!d) return out;
  while (dirent *e = readdir(d))
    if (numeric(e->d_name)) out.push_back(atol(e->d_name));
  closedir(d);
  return out;
}

int pidfd_open(long pid) {
#ifdef SYS_pidfd_open
  return int(syscall(SYS_pidfd_open, pid, 0));
#else
  (void)pid;
  errno = ENOSYS;
  return -1;
#endif
}

int pidfd_kill(int fd) {
#ifdef SYS_pidfd_send_signal
  return int(syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0));
#else
  (void)fd;
  errno = ENOSYS;
  return -1;
#endif
}

}  // namespace

Process read_process(long pid, const std::string &proc) {
  Process p;
  p.pid = pid;
  if (pid <= 0) return p;
  std::string dir = proc + "/" + std::to_string(pid), stat, status;
  if (!read_file(dir + "/stat", stat) || !parse_stat(stat, p)) return p;
  p.exists = true;
  if (read_file(dir + "/status", status)) {
    std::istringstream in(status);
    std::string line;
    while (std::getline(in, line))
      if (line.compare(0, 4, "Uid:") == 0) {
        std::istringstream ids(line.substr(4));
        ids >> p.ruid >> p.euid >> p.suid;
        break;
      }
  }
  if (read_file(dir + "/comm", p.comm)) p.comm = trim(p.comm);
  char buf[4096];
  ssize_t n = readlink((dir + "/exe").c_str(), buf, sizeof buf - 1);
  if (n > 0) p.exe = base(std::string(buf, size_t(n)));
  return p;
}

const std::vector<std::string> &session_programs() {
  static const std::vector<std::string> list = {
      // SDE's session (sde-session starts and supervises these).
      "sde-session", "startsde", "sde-wm", "openbox", "sde-panel", "sde-desktop", "sde-notifyd",
      "sde-compositor", "picom", "compton", "sde-xsettingsd", "xsettingsd", "sde-polkit-agent",
      "sde-lock", "sde-power", "sde-autostart", "sde-watchdog", "sde-force-close",
      "polkit-gnome-authentication-agent-1", "polkit-mate-authentication-agent-1", "lxpolkit",
      "lxqt-policykit-agent", "polkit-kde-authentication-agent-1",
      // The X server.
      "X", "Xorg", "Xorg.bin", "Xorg.wrap", "Xwrapper", "Xvfb", "Xwayland", "Xephyr", "Xnest",
      "Xvnc", "Xtigervnc", "Xdummy", "xinit", "startx",
      // Session services and the login.
      "dbus-daemon", "dbus-broker", "dbus-broker-launch", "dbus-launch", "dbus-run-session",
      "systemd", "init", "lightdm", "lightdm-session", "gdm", "gdm3", "gdm-x-session", "sddm",
      "sddm-helper", "login", "sshd"};
  return list;
}

bool session_program(const std::string &name) {
  if (name.empty()) return false;
  for (const auto &s : session_programs()) {
    if (name == s) return true;
    // /proc/<pid>/comm keeps 15 characters: "sde-polkit-agen".
    if (name.size() == 15 && s.size() > 15 && s.compare(0, 15, name) == 0) return true;
  }
  return false;
}

const char *verdict_name(Verdict v) {
  switch (v) {
    case Verdict::OK: return "ok";
    case Verdict::NO_PROCESS: return "no-process";
    case Verdict::ZOMBIE: return "zombie";
    case Verdict::PID1: return "pid1";
    case Verdict::SELF: return "self";
    case Verdict::OTHER_USER: return "other-user";
    case Verdict::SESSION: return "session";
    case Verdict::UNKNOWN: return "unknown";
  }
  return "unknown";
}

Context current_context() {
  Context c;
  c.self = long(getpid());
  c.uid = long(getuid());
  c.protect = ancestors(c.self, c.proc);
  std::string text;
  std::string file = runtime_dir() + "/sde-session-" + env("DISPLAY") + ".pid";
  if (read_file(file, text)) {
    long session = atol(trim(text).c_str());
    if (session > 1) c.protect.push_back(session);
  }
  return c;
}

Verdict check(const Process &p, const Context &ctx) {
  if (p.pid == 1) return Verdict::PID1;
  if (p.pid <= 1 || !p.exists) return Verdict::NO_PROCESS;
  if (p.pid == ctx.self || std::find(ctx.protect.begin(), ctx.protect.end(), p.pid) != ctx.protect.end())
    return Verdict::SELF;
  if (p.state == 'Z' || p.state == 'X') return Verdict::ZOMBIE;
  if (ctx.uid < 0 || p.ruid != ctx.uid || p.euid != ctx.uid || p.suid != ctx.uid)
    return Verdict::OTHER_USER;
  if (p.exe.empty()) return Verdict::UNKNOWN;  // kernel threads, unreadable programs
  if (session_program(p.exe) || session_program(p.comm)) return Verdict::SESSION;
  return Verdict::OK;
}

std::vector<long> ancestors(long pid, const std::string &proc) {
  std::vector<long> out;
  for (int depth = 0; pid > 1 && depth < 256; ++depth) {
    out.push_back(pid);
    Process p = read_process(pid, proc);
    if (!p.exists || p.ppid == pid) break;
    pid = p.ppid;
  }
  return out;
}

std::vector<long> family(long root, const std::string &proc) {
  std::vector<long> out;
  if (root <= 1) return out;
  std::map<long, std::vector<long>> children;
  std::vector<long> same_session;
  for (long pid : all_pids(proc)) {
    Process p = read_process(pid, proc);
    if (!p.exists) continue;
    children[p.ppid].push_back(pid);
    if (p.session == root && pid != root) same_session.push_back(pid);
  }
  std::vector<long> todo{root};
  while (!todo.empty()) {
    long pid = todo.back();
    todo.pop_back();
    if (std::find(out.begin(), out.end(), pid) != out.end()) continue;
    out.push_back(pid);
    for (long c : children[pid]) todo.push_back(c);
  }
  for (long pid : same_session)
    if (std::find(out.begin(), out.end(), pid) == out.end()) out.push_back(pid);
  return out;
}

bool in_family(long pid, long root, const std::string &proc) {
  if (pid <= 1 || root <= 1) return false;
  for (long a : ancestors(pid, proc))
    if (a == root) return true;
  Process p = read_process(pid, proc);
  return p.exists && p.session == root;
}

Verdict kill_now(long pid, const Context &ctx, unsigned long long start) {
  if (pid == 1) return Verdict::PID1;
  if (pid <= 1) return Verdict::NO_PROCESS;
  // The pidfd pins the process: should it end and its number be given to
  // another program between the checks and the signal, the signal fails
  // instead of reaching the other program.
  int fd = pidfd_open(pid);
  if (fd < 0 && errno == ESRCH) return Verdict::NO_PROCESS;
  Process p = read_process(pid, ctx.proc);
  Verdict v = check(p, ctx);
  if (v == Verdict::OK && start && p.start != start) v = Verdict::NO_PROCESS;
  if (v != Verdict::OK) {
    if (fd >= 0) close(fd);
    return v;
  }
  int r;
  if (fd >= 0) {
    r = pidfd_kill(fd);
    if (r != 0 && errno == ENOSYS) r = kill(pid_t(pid), SIGKILL);
    close(fd);
  } else {
    r = kill(pid_t(pid), SIGKILL);
  }
  return r == 0 ? Verdict::OK : Verdict::NO_PROCESS;
}

}  // namespace forceclose
