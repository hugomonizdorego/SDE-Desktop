// SPDX-License-Identifier: GPL-3.0-or-later
// QA fixture, never installed: an application that announces a badge with
// the Unity LauncherEntry API (0.2 Alpha 3 live badges), like Telegram,
// Chromium or libunity applications do.
//
//   sde-launcher-entry-fixture URI [--count N] [--progress P] [--urgent] [--partial]
//
// It stays on the session bus, exports com.canonical.Unity.LauncherEntry
// (Query: the URI and every property) on /com/canonical/unity/launcherentry/1
// and sends an Update signal; with --partial that first signal carries only
// "progress", so that the count arrives with Query. Lines on standard input
// change it and send an Update with what changed: "count N", "hide",
// "progress P" (below 0: hidden), "urgent 0|1". It prints READY once its
// first Update is sent, and ends at the end of its input or on SIGTERM.
#include <FL/Fl.H>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "dbus.h"

using namespace sde;

namespace {
const char *kIface = "com.canonical.Unity.LauncherEntry";
const char *kPath = "/com/canonical/unity/launcherentry/1";
dbus::Bus *bus = nullptr;
std::string uri;
long long count = 0;
bool count_visible = false, urgent = false, progress_visible = false;
double progress = 0;

void entry(dbus::Writer &dict, const char *key, int type, const void *value) {
  dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(key);
  char sig[2] = {char(type), 0};
  DBusMessageIter *it = e.iter();
  DBusMessageIter var;
  if (!it) return;
  dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &var);
  dbus_message_iter_append_basic(&var, type, value);
  dbus_message_iter_close_container(it, &var);
}

// The properties: every one, or only "progress" (|only_progress|).
void props(dbus::Writer &w, bool only_progress) {
  dbus::Writer dict(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
  dbus_bool_t pv = progress_visible, cv = count_visible, ur = urgent;
  entry(dict, "progress", DBUS_TYPE_DOUBLE, &progress);
  entry(dict, "progress-visible", DBUS_TYPE_BOOLEAN, &pv);
  if (only_progress) return;
  dbus_int64_t c = count;
  entry(dict, "count", DBUS_TYPE_INT64, &c);
  entry(dict, "count-visible", DBUS_TYPE_BOOLEAN, &cv);
  entry(dict, "urgent", DBUS_TYPE_BOOLEAN, &ur);
}

void update(bool only_progress) {
  DBusMessage *s = dbus::signal(kPath, kIface, "Update");
  {
    dbus::Writer w(s);
    w.str(uri);
    props(w, only_progress);
  }
  bus->send(s);
}

void input(int fd, void *) {
  static std::string pending;
  char buf[256];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n <= 0) {
    Fl::remove_fd(fd);
    exit(0);
  }
  pending.append(buf, size_t(n));
  for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
    std::string line = pending.substr(0, nl);
    pending.erase(0, nl + 1);
    if (line.compare(0, 6, "count ") == 0) {
      count = atoll(line.c_str() + 6);
      count_visible = true;
    } else if (line == "hide") {
      count_visible = false;
    } else if (line.compare(0, 9, "progress ") == 0) {
      progress = atof(line.c_str() + 9);
      progress_visible = progress >= 0;
      if (progress < 0) progress = 0;
    } else if (line.compare(0, 7, "urgent ") == 0) {
      urgent = line[7] == '1';
    } else {
      continue;
    }
    update(false);
  }
}
}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s URI [--count N] [--progress P] [--urgent] [--partial]\n", argv[0]);
    return 2;
  }
  uri = argv[1];
  bool partial = false;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoll(argv[++i]), count_visible = true;
    else if (!strcmp(argv[i], "--progress") && i + 1 < argc) progress = atof(argv[++i]), progress_visible = true;
    else if (!strcmp(argv[i], "--urgent")) urgent = true;
    else if (!strcmp(argv[i], "--partial")) partial = true;
  }
  bus = dbus::Bus::session();
  if (!bus) return 77;
  bus->export_object(kPath, [](DBusMessage *m) {
    if (!dbus_message_is_method_call(m, kIface, "Query")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    DBusMessage *r = dbus_message_new_method_return(m);
    {
      dbus::Writer w(r);
      w.str(uri);
      props(w, false);
    }
    bus->send(r);
    return DBUS_HANDLER_RESULT_HANDLED;
  });
  update(partial);
  printf("READY\n");
  fflush(stdout);
  Fl::add_fd(0, FL_READ, input, nullptr);
  for (;;) Fl::wait(1e20);
}
