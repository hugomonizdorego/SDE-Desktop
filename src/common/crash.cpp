// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash.h"

#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <initializer_list>

namespace sde {
namespace {

const char *g_program = "sde";

void put(const char *s) {
  if (write(2, s, strlen(s)) < 0) {
  }
}

void on_crash(int sig) {
  // Only async-signal-safe calls; backtrace() is preloaded below.
  put(g_program);
  put(" crashed: ");
  put(strsignal(sig));
  put(". Backtrace (please attach it to a bug report):\n");
  void *frames[64];
  int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, 2);
  put("-- end of backtrace\n");
  signal(sig, SIG_DFL);
  raise(sig);
}

}  // namespace

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
