// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "session.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/extensions/dpms.h>
#include <X11/extensions/scrnsaver.h>
#include <unistd.h>

#include <cstdio>

#include "dbus.h"

namespace sde {
namespace session {

namespace {
const char *kLogin = "org.freedesktop.login1";
const char *kLoginPath = "/org/freedesktop/login1";
const char *kManager = "org.freedesktop.login1.Manager";

bool has_screensaver_ext() {
  static int known = -1;
  if (known < 0) {
    int ev, err;
    known = fl_display && XScreenSaverQueryExtension(fl_display, &ev, &err) ? 1 : 0;
  }
  return known == 1;
}

bool has_dpms() {
  static int known = -1;
  if (known < 0) {
    int ev, err;
    known = fl_display && DPMSQueryExtension(fl_display, &ev, &err) && DPMSCapable(fl_display) ? 1 : 0;
  }
  return known == 1;
}
}  // namespace

long idle_ms() {
  if (!has_screensaver_ext()) return -1;
  XScreenSaverInfo *info = XScreenSaverAllocInfo();
  if (!info) return -1;
  long ms = -1;
  if (XScreenSaverQueryInfo(fl_display, RootWindow(fl_display, fl_screen), info))
    ms = long(info->idle);
  XFree(info);
  return ms;
}

void reset_idle() {
  if (!fl_display) return;
  XResetScreenSaver(fl_display);
  if (has_dpms()) DPMSForceLevel(fl_display, DPMSModeOn);
  XFlush(fl_display);
}

bool monitors_off() {
  if (!has_dpms()) return false;
  DPMSEnable(fl_display);
  DPMSForceLevel(fl_display, DPMSModeOff);
  XFlush(fl_display);
  return true;
}

void disable_x_timers() {
  if (!fl_display) return;
  XSetScreenSaver(fl_display, 0, 0, DontPreferBlanking, AllowExposures);
  if (has_dpms()) {
    DPMSSetTimeouts(fl_display, 0, 0, 0);
    DPMSEnable(fl_display);  // forcing "off" needs DPMS enabled
  }
  XFlush(fl_display);
}

void notify(const std::string &app, const std::string &icon, const std::string &summary,
            const std::string &body, int urgency, uint32_t replaces,
            std::function<void(uint32_t)> done, int timeout_ms) {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return;
  DBusMessage *m = dbus::method("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                "org.freedesktop.Notifications", "Notify");
  {
    dbus::Writer w(m);
    w.str(app).u32(replaces).str(icon).str(summary).str(body).strings({});
    {
      dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
      dbus::Writer entry(hints.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      entry.str("urgency");
      DBusMessageIter v;
      dbus_message_iter_open_container(entry.iter(), DBUS_TYPE_VARIANT, "y", &v);
      unsigned char u = static_cast<unsigned char>(urgency);
      dbus_message_iter_append_basic(&v, DBUS_TYPE_BYTE, &u);
      dbus_message_iter_close_container(entry.iter(), &v);
    }
    w.i32(timeout_ms);
  }
  bus->call_async(m, [done](DBusMessage *reply) {
    auto a = dbus::read_args(reply);
    if (done) done(a.empty() ? 0 : uint32_t(a[0].num()));
  });
}

int logind_inhibit(const std::string &what, const std::string &who, const std::string &why,
                   const std::string &mode) {
  dbus::Bus *bus = dbus::Bus::system();
  if (!bus) return -1;
  DBusMessage *m = dbus::method(kLogin, kLoginPath, kManager, "Inhibit");
  dbus::Writer(m).str(what).str(who).str(why).str(mode);
  DBusMessage *reply = bus->call(m, 3000);
  if (!reply) return -1;
  int fd = -1;
  DBusError err;
  dbus_error_init(&err);
  if (!dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) fd = -1;
  dbus_error_free(&err);
  dbus_message_unref(reply);
  return fd;
}

void logind_action(const std::string &method) {
  dbus::Bus *bus = dbus::Bus::system();
  if (!bus) return;
  DBusMessage *m = dbus::method(kLogin, kLoginPath, kManager, method);
  dbus::Writer(m).boolean(false);
  bus->call_async(m, [method](DBusMessage *reply) {
    if (!reply) fprintf(stderr, "sde: logind %s failed\n", method.c_str());
  }, 25000);
}

std::string logind_can(const std::string &method) {
  dbus::Bus *bus = dbus::Bus::system();
  if (!bus) return "na";
  DBusMessage *reply = bus->call(dbus::method(kLogin, kLoginPath, kManager, "Can" + method), 2000);
  if (!reply) return "na";
  auto a = dbus::read_args(reply);
  dbus_message_unref(reply);
  return a.empty() ? "na" : a[0].str();
}

}  // namespace session
}  // namespace sde
