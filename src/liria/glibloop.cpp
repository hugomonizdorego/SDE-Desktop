// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "glibloop.h"

#include <FL/Fl.H>
#include <glib.h>

#include <map>
#include <vector>

namespace sde {
namespace liria {

namespace {

bool g_active = false;
bool g_dispatching = false;
std::vector<GPollFD> g_fds(16);
std::map<int, int> g_watched;  // fd -> FLTK events

void dispatch_cb(void *) { dispatch_glib(); }
void fd_cb(FL_SOCKET, void *) { dispatch_glib(); }

// Fills g_fds; returns how many there are and the timeout (ms, -1 none).
int query(gint priority, gint *timeout) {
  GMainContext *ctx = g_main_context_default();
  int n = g_main_context_query(ctx, priority, timeout, g_fds.data(), int(g_fds.size()));
  if (n > int(g_fds.size())) {
    g_fds.resize(size_t(n) + 8);
    n = g_main_context_query(ctx, priority, timeout, g_fds.data(), int(g_fds.size()));
  }
  return n;
}

// Before FLTK waits: GLib's descriptors and next timeout become FLTK's.
void prepare(void *) {
  if (!g_active || g_dispatching) return;
  GMainContext *ctx = g_main_context_default();
  if (!g_main_context_acquire(ctx)) return;
  gint priority = 0, timeout = -1;
  gboolean ready = g_main_context_prepare(ctx, &priority);
  int n = query(priority, &timeout);
  g_main_context_release(ctx);
  std::map<int, int> want;
  for (int i = 0; i < n; ++i) {
    int ev = 0;
    if (g_fds[size_t(i)].events & (G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_PRI)) ev |= FL_READ;
    if (g_fds[size_t(i)].events & G_IO_OUT) ev |= FL_WRITE;
    if (ev) want[g_fds[size_t(i)].fd] |= ev;
  }
  for (auto it = g_watched.begin(); it != g_watched.end();) {
    auto w = want.find(it->first);
    if (w == want.end() || w->second != it->second) {
      Fl::remove_fd(it->first);
      it = g_watched.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto &w : want)
    if (!g_watched.count(w.first)) {
      Fl::add_fd(w.first, w.second, fd_cb, nullptr);
      g_watched[w.first] = w.second;
    }
  Fl::remove_timeout(dispatch_cb, nullptr);
  if (ready) Fl::add_timeout(0, dispatch_cb, nullptr);
  else if (timeout >= 0) Fl::add_timeout(timeout / 1000.0, dispatch_cb, nullptr);
}

}  // namespace

void start_glib_loop() {
  if (g_active) return;
  g_active = true;
  Fl::add_check(prepare, nullptr);
}

void stop_glib_loop() {
  if (!g_active) return;
  g_active = false;
  Fl::remove_check(prepare, nullptr);
  Fl::remove_timeout(dispatch_cb, nullptr);
  for (const auto &w : g_watched) Fl::remove_fd(w.first);
  g_watched.clear();
}

void dispatch_glib() {
  if (!g_active || g_dispatching) return;
  GMainContext *ctx = g_main_context_default();
  if (!g_main_context_acquire(ctx)) return;
  g_dispatching = true;
  gint priority = 0, timeout = -1;
  g_main_context_prepare(ctx, &priority);
  int n = query(priority, &timeout);
  if (n > 0) g_poll(g_fds.data(), guint(n), 0);
  if (g_main_context_check(ctx, priority, g_fds.data(), n)) g_main_context_dispatch(ctx);
  g_dispatching = false;
  g_main_context_release(ctx);
}

}  // namespace liria
}  // namespace sde
