// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Which processes Force Close may end (0.2.0 Alpha 3.1, docs/FORCE-CLOSE.md).
//
// Force Close sends SIGKILL at once, so the rules are strict: only a process
// that can be positively identified, that runs as this user (real,
// effective and saved user id), is not PID 1, not this process or one of
// its ancestors (sde-session, the display manager...), not the session's
// own sde-session (its pid file), and not a part of the session (the
// window manager, panel, desktop, compositor, X server, D-Bus, screen
// locker... by the name of its executable and of its command). The rules
// read /proc only; |proc| replaces it in the tests.
#pragma once

#include <string>
#include <vector>

namespace forceclose {

struct Process {
  long pid = 0;
  bool exists = false;
  char state = '?';
  long ppid = 0, session = 0;
  unsigned long long start = 0;  // clock ticks after boot (stat field 22)
  long ruid = -1, euid = -1, suid = -1;
  std::string comm;  // /proc/<pid>/comm (at most 15 characters)
  std::string exe;   // the executable's file name; "" when it cannot be read
};
Process read_process(long pid, const std::string &proc = "/proc");

// The programs of the session itself, never ended by Force Close.
const std::vector<std::string> &session_programs();
// |name| (an executable's file name, or a 15 character comm) is one of them.
bool session_program(const std::string &name);

enum class Verdict {
  OK,
  NO_PROCESS,  // gone, or not a valid process id
  ZOMBIE,      // already ended, waiting for its parent
  PID1,
  SELF,        // this process, or one of its ancestors
  OTHER_USER,
  SESSION,     // a part of the session (by name or the session's pid file)
  UNKNOWN,     // cannot be identified (no readable executable: kernel threads...)
};
const char *verdict_name(Verdict v);

struct Context {
  long self = 0;
  long uid = -1;
  std::vector<long> protect;  // pids never ended (ancestors, sde-session)
  std::string proc = "/proc";
};
// The context of this process: its id, user, ancestors and the running
// sde-session (runtime_dir()/sde-session-$DISPLAY.pid).
Context current_context();

Verdict check(const Process &p, const Context &ctx);
inline Verdict check(long pid, const Context &ctx) { return check(read_process(pid, ctx.proc), ctx); }

// |pid| and its ancestors up to (not including) PID 1.
std::vector<long> ancestors(long pid, const std::string &proc = "/proc");
// |pid| and every process that descends from it or belongs to its session
// (a program started by SDE leads its own session).
std::vector<long> family(long pid, const std::string &proc = "/proc");
// Whether |pid| is |root| or descends from it or belongs to its session.
bool in_family(long pid, long root, const std::string &proc = "/proc");

// Sends SIGKILL to |pid| if check() allows it, through a pidfd so that a
// recycled process number never gets the signal (kill(2) on kernels
// without pidfd_open). |start|, when not 0, must match the process's start
// time. Returns the verdict; OK means the signal was sent.
Verdict kill_now(long pid, const Context &ctx, unsigned long long start = 0);

}  // namespace forceclose
