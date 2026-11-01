// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <initializer_list>

namespace sde {
namespace {

const char *g_program = "sde";
char g_report[1024];
const char *(*g_context)() = nullptr;

void put(int fd, const char *s) {
  if (s && write(fd, s, strlen(s)) < 0) {
  }
}

void report(int fd, int sig, void *const *frames, int n) {
  put(fd, g_program);
  put(fd, " crashed: ");
  put(fd, strsignal(sig));
  put(fd, ". Backtrace (please attach it to a bug report):\n");
  if (g_context) {
    const char *what = g_context();
    if (what && *what) {
      put(fd, "It was ");
      put(fd, what);
      put(fd, ".\n");
    }
  }
  backtrace_symbols_fd(frames, n, fd);
  put(fd, "-- end of backtrace\n");
}

void on_crash(int sig) {
  // Only async-signal-safe calls; backtrace() is preloaded below.
  void *frames[64];
  int n = backtrace(frames, 64);
  report(2, sig, frames, n);
  if (g_report[0]) {
    int fd = open(g_report, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
      report(fd, sig, frames, n);
      close(fd);
    }
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

}  // namespace

void set_crash_report(const char *path, const char *(*context)()) {
  g_report[0] = '\0';
  if (path && strlen(path) < sizeof g_report) strcpy(g_report, path);
  g_context = context;
}

void install_crash_handler(const char *program) {
  g_program = program;
  void *warm[1];
  backtrace(warm, 1);  // loads libgcc now, not inside the signal handler
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_crash;
  sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  // A stack overflow needs its own stack to report anything.
  static char stack[64 * 1024];
  stack_t ss;
  ss.ss_sp = stack;
  ss.ss_size = sizeof stack;
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);
  for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(s, &sa, nullptr);
}

}  // namespace sde
