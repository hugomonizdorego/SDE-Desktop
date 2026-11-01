// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "watchdog.h"

#include <FL/Fl.H>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "build_config.h"
#include "crash.h"
#include "util.h"

namespace sde {
namespace merkadu {
namespace watchdog {

namespace {

// Everything here is read by the watchdog thread and the signal handlers
// while the FLTK thread runs, and stays alive until the process ends:
// nothing is destroyed at exit (a static destructor racing a thread that
// still runs is how Merkadu 0.2 Alpha 3 hung after its window closed).
std::atomic<long long> g_beat{0};
std::atomic<const char *> g_what{nullptr};
std::atomic<long> g_detail{-1};
std::atomic<long long> g_since{0};
std::atomic<bool> g_running{false};
std::atomic<long long> g_longest{0};
std::atomic<int> g_stalls{0};
std::atomic<int> g_stack_fd{-1};
std::atomic<bool> g_stack_done{false};
pthread_t g_main;
std::mutex *g_log_mutex = new std::mutex;
FILE *g_log = nullptr;
const char *g_program = "merkadu";
// The current activity for the crash handler, rendered without allocating.
char g_crash_what[256];

constexpr long long kStallMs = 250;   // logged
constexpr long long kSlowMs = 2000;   // logged while it lasts
constexpr long long kHangMs = 10000;  // the stack goes to the hang report
constexpr double kBeat = 0.1;

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void beat(void *) {
  g_beat = now_ms();
  Fl::repeat_timeout(kBeat, beat);
}

std::string describe(const char *what, long detail) {
  if (!what) return "drawing or handling events";
  std::string s = what;
  if (detail >= 0) s += " (" + std::to_string(detail) + ")";
  return s;
}

std::string describe_now() { return describe(g_what.load(), g_detail.load()); }

// The crash handler's view of the activity (async-signal-safe: no
// allocation, a fixed buffer filled when a Busy scope starts).
const char *crash_context() { return g_crash_what[0] ? g_crash_what : nullptr; }

void set_crash_what(const char *what, long detail) {
  if (!what) {
    g_crash_what[0] = '\0';
    return;
  }
  snprintf(g_crash_what, sizeof g_crash_what, detail >= 0 ? "%s (%ld)" : "%s", what, detail);
}

// SIGRTMIN+4 on the FLTK thread: its stack into the hang report.
int stack_signal() { return SIGRTMIN + 4; }

void on_stack_signal(int) {
  int fd = g_stack_fd.load();
  if (fd >= 0) {
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, fd);
    const char end[] = "-- end of the stack\n";
    if (write(fd, end, sizeof end - 1) < 0) {
    }
  }
  g_stack_done = true;
}

std::string clock_text(std::time_t t) {
  char buf[64];
  struct tm tm = {};
  localtime_r(&t, &tm);
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

void write_hang_report(long long ms, const std::string &what) {
  std::string path = join_path(state_dir(), "hang-report.txt");
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return;
  std::string head = std::string(g_program) + " " + SDE_VERSION + " did not respond for " +
                     std::to_string(ms / 1000) + " s (" + clock_text(std::time(nullptr)) + ").\nIt was " + what +
                     ".\nThe stack of its window thread (please attach this file to a bug report):\n";
  if (write(fd, head.data(), head.size()) < 0) {
  }
  g_stack_done = false;
  g_stack_fd = fd;
  pthread_kill(g_main, stack_signal());
  for (int i = 0; i < 100 && !g_stack_done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  g_stack_fd = -1;
  if (!g_stack_done) {
    const char note[] = "(the window thread did not answer: it is blocked in the kernel)\n";
    if (write(fd, note, sizeof note - 1) < 0) {
    }
  }
  close(fd);
  log("the window has not responded for " + std::to_string(ms / 1000) + " s; its stack is in " + path);
}

void watch() {
  long long stall_start = 0;
  bool slow_logged = false, hang_written = false;
  std::map<std::string, int> seen;  // what ran during the stall, in samples
  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    long long now = now_ms(), last = g_beat.load();
    long long gap = now - last;
    if (gap > kStallMs + long(kBeat * 1000)) {
      if (!stall_start) {
        stall_start = last;
        slow_logged = hang_written = false;
        seen.clear();
      }
      ++seen[describe_now()];
      if (gap > kSlowMs && !slow_logged) {
        slow_logged = true;
        log("the window has not responded for " + std::to_string(gap / 1000) + " s: " + describe_now());
      }
      if (gap > kHangMs && !hang_written) {
        hang_written = true;
        write_hang_report(gap, describe_now());
      }
    } else if (stall_start) {
      // The beat is back: the stall lasted from the beat before it to the
      // one after it, less one beat interval.
      long long len = last - stall_start - long(kBeat * 1000);
      if (len > kStallMs) {
        ++g_stalls;
        if (len > g_longest) g_longest = len;
        std::vector<std::pair<int, std::string>> order;
        for (auto &kv : seen) order.push_back({kv.second, kv.first});
        std::sort(order.rbegin(), order.rend());
        std::string what;
        for (size_t i = 0; i < order.size() && i < 3; ++i)
          what += (i ? ", " : "") + order[i].second + " " + std::to_string(order[i].first * 50) + " ms";
        log("the window did not respond for " + std::to_string(len) + " ms: " + what);
      }
      stall_start = 0;
    }
  }
}

}  // namespace

std::string state_dir() { return join_path(state_home(), "merkadu"); }

void log(const std::string &line) {
  std::lock_guard<std::mutex> lock(*g_log_mutex);
  fprintf(stderr, "%s: %s\n", g_program, line.c_str());
  if (g_log) {
    long long t = now_ms();
    fprintf(g_log, "%lld.%03lld %s\n", t / 1000 % 100000, t % 1000, line.c_str());
    fflush(g_log);
  }
}

void start(const char *program) {
  if (g_running) return;
  g_program = program;
  g_main = pthread_self();
  std::string dir = state_dir();
  make_dirs(dir);
  std::string path = join_path(dir, "merkadu.log");
  rename(path.c_str(), (path + ".1").c_str());
  {
    std::lock_guard<std::mutex> lock(*g_log_mutex);
    g_log = fopen(path.c_str(), "we");
    if (g_log) fprintf(g_log, "%s %s started %s\n", program, SDE_VERSION, clock_text(std::time(nullptr)).c_str());
  }
  static std::string crash_path = join_path(dir, "crash-report.txt");
  set_crash_report(crash_path.c_str(), crash_context);
  struct sigaction sa = {};
  sa.sa_handler = on_stack_signal;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(stack_signal(), &sa, nullptr);
  void *warm[1];
  backtrace(warm, 1);  // libgcc is loaded now, not in the handler
  g_beat = now_ms();
  Fl::add_timeout(kBeat, beat);
  g_running = true;
  std::thread(watch).detach();
}

void stop() {
  g_running = false;
  Fl::remove_timeout(beat);
}

Busy::Busy(const char *what, long detail)
    : prev_what_(g_what.load()), prev_detail_(g_detail.load()), prev_since_(g_since.load()), since_(now_ms()) {
  g_what = what;
  g_detail = detail;
  g_since = since_;
  set_crash_what(what, detail);
}

Busy::~Busy() {
  long long ms = now_ms() - since_;
  const char *what = g_what.load();
  long detail = g_detail.load();
  g_what = prev_what_;
  g_detail = prev_detail_;
  g_since = prev_since_;
  set_crash_what(prev_what_, prev_detail_);
  if (ms > kStallMs) log("busy " + std::to_string(ms) + " ms: " + describe(what, detail));
}

std::string current() {
  const char *what = g_what.load();
  return what ? describe(what, g_detail.load()) : std::string();
}

std::string last_report(long long *when) {
  std::string dir = state_dir(), best;
  long long newest = 0;
  for (const char *name : {"crash-report.txt", "hang-report.txt"}) {
    struct stat st;
    std::string p = join_path(dir, name);
    if (stat(p.c_str(), &st) == 0 && st.st_size > 0 && (long long)st.st_mtime > newest) {
      newest = st.st_mtime;
      best = p;
    }
  }
  if (best.empty()) return "";
  std::string seen;
  if (read_file(join_path(dir, "report-seen"), seen) && std::atoll(trim(seen).c_str()) >= newest) return "";
  if (when) *when = newest;
  return best;
}

void dismiss_report() {
  long long when = 0;
  if (last_report(&when).empty()) return;
  write_file_atomic(join_path(state_dir(), "report-seen"), std::to_string(when) + "\n");
}

long long longest_stall_ms() { return g_longest; }
int stalls() { return g_stalls; }

}  // namespace watchdog
}  // namespace merkadu
}  // namespace sde
