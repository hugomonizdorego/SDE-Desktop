// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_perf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace menu_perf {

namespace {

FILE *g_log = nullptr;
int g_state = -1;  // -1 unknown, 0 off, 1 on

struct Pending {
  bool active = false;
  std::string kind, who, notes;
  double t0 = 0;
};
Pending g_pending;

FILE *log_file() {
  if (g_state >= 0) return g_log;
  g_state = 0;
  std::string path;
  if (const char *p = getenv("SDE_MENU_PERF_LOG"); p && *p) {
    path = p;
  } else if (const char *qa = getenv("SDE_PANEL_QA_DIR"); qa && *qa) {
    path = std::string(qa) + "/menu-perf.log";
  }
  if (path.empty()) return nullptr;
  g_log = fopen(path.c_str(), "a");
  if (g_log) {
    setvbuf(g_log, nullptr, _IOLBF, 0);
    g_state = 1;
  }
  return g_log;
}

std::string number(double v) {
  char buf[32];
  snprintf(buf, sizeof buf, "%.2f", v);
  return buf;
}

}  // namespace

bool enabled() { return log_file() != nullptr; }

double now_ms() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

void begin(const char *kind, const char *who) {
  if (!enabled()) return;
  g_pending.active = true;
  g_pending.kind = kind;
  g_pending.who = who;
  g_pending.notes.clear();
  g_pending.t0 = now_ms();
}

void note(const char *name, double value) {
  if (!enabled() || !g_pending.active) return;
  g_pending.notes += std::string(" ") + name + "=" + number(value);
}

void note(const char *name, const std::string &value) {
  if (!enabled() || !g_pending.active) return;
  std::string v = value;
  for (char &c : v)
    if (c == ' ' || c == '\n' || c == '\t') c = '_';
  g_pending.notes += std::string(" ") + name + "=" + v;
}

void frame(const char *who, double draw_ms, double sync_ms) {
  FILE *f = log_file();
  if (!f) return;
  double t = now_ms();
  if (g_pending.active && g_pending.who == who) {
    g_pending.active = false;
    fprintf(f, "%s %s ms=%s draw=%s sync=%s t=%s%s\n", g_pending.kind.c_str(), who,
            number(t - g_pending.t0).c_str(), number(draw_ms).c_str(), number(sync_ms).c_str(),
            number(t).c_str(), g_pending.notes.c_str());
    return;
  }
  fprintf(f, "frame %s t=%s draw=%s sync=%s\n", who, number(t).c_str(), number(draw_ms).c_str(),
          number(sync_ms).c_str());
}

void log(const std::string &line) {
  if (FILE *f = log_file()) fprintf(f, "%s\n", line.c_str());
}

}  // namespace menu_perf
