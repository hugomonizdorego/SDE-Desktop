// SPDX-License-Identifier: GPL-3.0-or-later
// Optional paint/timer statistics for finding lag (SDE_PERF_STATS=1).
//
// With SDE_PERF_STATS=1 in the environment every SDE program that draws
// through sde::BufferedWindow / sde::Painter counts its repaints and prints
//   sde-perf: <program> flush=.. full=.. partial=.. copy=.. px=.. draw_ms=..
//             painter=.. upload=.. cached=..
// to stderr when it exits and whenever it receives SIGUSR2 (see
// tools/perf-probe.py). Without the variable nothing is installed and the
// counters cost one predictable branch.
#pragma once

#include <FL/Fl.H>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace sde {
namespace perf {

struct Counters {
  unsigned long flush = 0;    // BufferedWindow::flush() calls that did work
  unsigned long full = 0;     // complete window repaints
  unsigned long partial = 0;  // repaints clipped to a damaged region
  unsigned long copy = 0;     // expose-only: back buffer copied, nothing drawn
  unsigned long long px = 0;  // pixels repainted (full + partial)
  double draw_ms = 0;         // client-side time spent inside draw()
  unsigned long painter = 0;  // sde::Painter instances (cairo contexts)
  unsigned long upload = 0;   // image surfaces uploaded to the X server
  unsigned long cached = 0;   // image paints served from the server-side cache
};

inline Counters &counters() {
  static Counters c;
  return c;
}

inline int &state() {  // -1 unknown, 0 off, 1 on
  static int s = -1;
  return s;
}

inline void print() {
  const Counters &c = counters();
  char name[64] = "?";
  FILE *f = fopen("/proc/self/comm", "r");
  if (f) {
    if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
    fclose(f);
  }
  fprintf(stderr,
          "sde-perf: %s flush=%lu full=%lu partial=%lu copy=%lu px=%llu draw_ms=%.1f "
          "painter=%lu upload=%lu cached=%lu\n",
          name, c.flush, c.full, c.partial, c.copy, c.px, c.draw_ms, c.painter, c.upload,
          c.cached);
  fflush(stderr);
}

inline int *signal_pipe() {
  static int fds[2] = {-1, -1};
  return fds;
}

inline void on_signal(int) {
  int saved = errno;
  char b = 1;
  if (signal_pipe()[1] >= 0) (void)!write(signal_pipe()[1], &b, 1);
  errno = saved;
}

inline void on_pipe(int fd, void *) {
  char buf[16];
  while (read(fd, buf, sizeof buf) > 0) {
  }
  print();
}

inline bool enabled() {
  int &s = state();
  if (s >= 0) return s == 1;
  const char *v = getenv("SDE_PERF_STATS");
  s = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
  if (s == 1) {
    int *fds = signal_pipe();
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
      Fl::add_fd(fds[0], FL_READ, on_pipe, nullptr);
      struct sigaction sa;
      memset(&sa, 0, sizeof sa);
      sa.sa_handler = on_signal;
      sa.sa_flags = SA_RESTART;
      sigaction(SIGUSR2, &sa, nullptr);
    }
    atexit(print);
    fprintf(stderr, "sde-perf: enabled (SIGUSR2 prints counters)\n");
  }
  return s == 1;
}

inline double now_ms() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

}  // namespace perf
}  // namespace sde
