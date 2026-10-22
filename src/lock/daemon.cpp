// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The sde-lock daemon ("sde-lock --daemon", started with the session).
//
// D-Bus (session bus):
//   org.freedesktop.ScreenSaver at /org/freedesktop/ScreenSaver and
//   /ScreenSaver: Lock, GetActive, SetActive, GetActiveTime,
//   GetSessionIdleTime, SimulateUserActivity, Inhibit, UnInhibit and the
//   ActiveChanged signal.
//   org.debion.SDE.ScreenLock at /org/debion/SDE/ScreenLock: Lock,
//   GetActive, IsIdleInhibited, ListInhibitors, SimulateUserActivity and
//   ActiveChanged.
// systemd-logind (system bus): Lock and Unlock of this session,
//   PrepareForSleep with a "delay" sleep inhibitor (the screen is locked
//   before suspend), SetLockedHint.
// Idle: locks after [lock] idle_lock seconds unless an inhibitor is held.
//
// The lock screen runs in a child process ("sde-lock --ui FD", ui.cpp).
// While locked, the daemon keeps a black override-redirect "curtain" window
// over the whole screen (resized with it) mapped below it. If the lock
// screen process dies without a successful password check, the daemon grabs
// the keyboard and pointer itself on the curtain (the screen stays black
// and locked) and starts it again. A lock screen that stops sending its
// heartbeat (hung on a blocked X or input method connection) is killed and
// replaced by a fresh one the same way. After a resume the lock screen is
// told (SIGUSR1) to check the monitors and its grabs again. Only a "U" from
// the lock screen (PAM success) or logind's Unlock ends a lock.
#include <FL/Fl.H>
#include <FL/x.H>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "dbus.h"
#include "i18n.h"
#include "policy.h"
#include "session.h"
#include "util.h"
#include "x11.h"

namespace sde {
namespace lock {

namespace {

const char *kOwnName = "org.debion.SDE.ScreenLock";
const char *kOwnPath = "/org/debion/SDE/ScreenLock";
const char *kFdoName = "org.freedesktop.ScreenSaver";
const char *kFdoPath = "/org/freedesktop/ScreenSaver";
const char *kLogin = "org.freedesktop.login1";

double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + ts.tv_nsec / 1e9;
}

// Timestamped lines on stderr (~/.local/state/sde/session.log).
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char *fmt, ...) {
  char when[32];
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(when, sizeof(when), "%H:%M:%S", &tm);
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fprintf(stderr, "sde-lock %s: %s\n", when, buf);
  fflush(stderr);
}

enum class State { Unlocked, Locking, Locked };

struct Daemon {
  std::string exe;
  Config cfg;
  int idle_lock = 300;
  bool lock_on_suspend = true;

  dbus::Bus *session = nullptr;
  dbus::Bus *system = nullptr;
  std::string session_path;  // logind session object
  int sleep_fd = -1;

  State state = State::Unlocked;
  time_t locked_at = 0;
  std::vector<DBusMessage *> waiting;  // Lock() calls answered once locked

  pid_t child = -1;
  int child_fd = -1;
  bool child_unlocked = false;
  bool child_grab_failed = false;
  bool forced_unlock = false;
  double child_heard = 0;  // last byte (heartbeat) from the lock screen
  bool child_killed = false;
  CrashBackoff backoff;

  std::vector<Window> curtain;
  bool curtain_grabbed = false;

  InhibitorTracker inhibitors;
} d;

// ------------------------------------------------------------ curtain

// One window over the whole root window: it covers every monitor whatever
// RandR does, and is resized with the root window (x_events).
void map_curtain() {
  if (d.curtain.empty()) {
    XSetWindowAttributes a;
    a.override_redirect = True;
    a.background_pixel = BlackPixel(fl_display, fl_screen);
    Window root = RootWindow(fl_display, fl_screen);
    XWindowAttributes ra;
    XGetWindowAttributes(fl_display, root, &ra);
    Window win = XCreateWindow(fl_display, root, 0, 0, unsigned(std::max(1, ra.width)),
                               unsigned(std::max(1, ra.height)), 0, CopyFromParent, InputOutput,
                               CopyFromParent, CWOverrideRedirect | CWBackPixel, &a);
    XStoreName(fl_display, win, "SDE Screen Lock");
    d.curtain.push_back(win);
  }
  for (Window w : d.curtain) {
    XMapRaised(fl_display, w);
  }
  XFlush(fl_display);
}

void unmap_curtain() {
  if (d.curtain_grabbed) {
    XUngrabKeyboard(fl_display, CurrentTime);
    XUngrabPointer(fl_display, CurrentTime);
    d.curtain_grabbed = false;
  }
  for (Window w : d.curtain) XDestroyWindow(fl_display, w);
  d.curtain.clear();
  XFlush(fl_display);
}

// The fallback grab while no lock screen process holds one. The keyboard
// is what matters (no keys reach the session); the pointer is taken too
// when it can be.
bool grab_curtain(int tries = 30) {
  if (d.curtain.empty()) map_curtain();
  for (Window w : d.curtain) XMapRaised(fl_display, w);
  XSync(fl_display, False);
  Window w = d.curtain.front();
  for (int i = 0; i < tries; ++i) {
    bool kb = XGrabKeyboard(fl_display, w, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
    XGrabPointer(fl_display, w, False, ButtonPressMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);
    XFlush(fl_display);
    if (kb) {
      d.curtain_grabbed = true;
      return true;
    }
    if (i + 1 < tries) usleep(50000);
  }
  return false;
}

void release_curtain_grab() {
  if (!d.curtain_grabbed) return;
  XUngrabKeyboard(fl_display, CurrentTime);
  XUngrabPointer(fl_display, CurrentTime);
  d.curtain_grabbed = false;
  XFlush(fl_display);
}

// ------------------------------------------------------------ signals

void emit_active(bool active) {
  if (!d.session) return;
  for (const char *path : {kFdoPath, "/ScreenSaver"}) {
    DBusMessage *s = dbus::signal(path, kFdoName, "ActiveChanged");
    dbus::Writer(s).boolean(active);
    d.session->send(s);
  }
  DBusMessage *s = dbus::signal(kOwnPath, kOwnName, "ActiveChanged");
  dbus::Writer(s).boolean(active);
  d.session->send(s);
}

void set_locked_hint(bool locked) {
  if (!d.system || d.session_path.empty()) return;
  DBusMessage *m = dbus::method(kLogin, d.session_path, "org.freedesktop.login1.Session", "SetLockedHint");
  dbus::Writer(m).boolean(locked);
  d.system->call_async(m, [](DBusMessage *) {});
}

void answer_waiting(bool ok, const std::string &error) {
  for (DBusMessage *call : d.waiting) {
    if (ok)
      d.session->send(dbus_message_new_method_return(call));
    else
      d.session->send(dbus::Bus::error(call, "org.debion.SDE.ScreenLock.Error.GrabFailed", error));
    dbus_message_unref(call);
  }
  d.waiting.clear();
}

void release_sleep_inhibitor() {
  if (d.sleep_fd >= 0) {
    close(d.sleep_fd);
    d.sleep_fd = -1;
  }
}

void take_sleep_inhibitor() {
  if (d.sleep_fd >= 0 || !d.lock_on_suspend || !d.system) return;
  d.sleep_fd = session::logind_inhibit("sleep", "SDE Screen Lock",
                                       "Locking the screen before the computer sleeps", "delay");
  if (d.sleep_fd >= 0) fcntl(d.sleep_fd, F_SETFD, FD_CLOEXEC);
}

// ------------------------------------------------------------ child

void spawn_ui();

void became_locked() {
  if (d.state == State::Locked) return;
  d.state = State::Locked;
  d.locked_at = time(nullptr);
  answer_waiting(true, "");
  emit_active(true);
  set_locked_hint(true);
  // Suspend may continue now.
  release_sleep_inhibitor();
}

void became_unlocked() {
  d.state = State::Unlocked;
  d.locked_at = 0;
  d.backoff.reset();
  unmap_curtain();
  emit_active(false);
  set_locked_hint(false);
  session::reset_idle();
  take_sleep_inhibitor();
}

void lock_failed(const std::string &why) {
  d.state = State::Unlocked;
  unmap_curtain();
  answer_waiting(false, why);
  session::notify(_("SDE Screen Lock"), "dialog-warning", _("The screen could not be locked"),
                  why, 2, 0);
  log_line("%s", why.c_str());
}

void respawn_cb(void *) {
  if (d.state != State::Unlocked && d.child < 0) spawn_ui();
}

void child_exited(int status) {
  bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  bool unlocked = d.child_unlocked && clean;
  bool forced = d.forced_unlock;
  bool grab_failed = d.child_grab_failed;
  d.child = -1;
  d.child_unlocked = d.child_grab_failed = d.forced_unlock = false;
  if (d.state == State::Unlocked) return;
  if (unlocked || forced) {
    became_unlocked();
    return;
  }
  if (d.state == State::Locking && grab_failed) {
    lock_failed(_("The lock screen could not be shown."));
    return;
  }
  // The lock screen died without an accepted password: stay locked behind
  // the black curtain and start it again.
  bool killed = d.child_killed;
  d.child_killed = false;
  log_line("the lock screen %s (status %d); keeping the session locked",
           killed ? "stopped answering and was replaced" : "exited", status);
  if (!d.curtain_grabbed && !grab_curtain())
    log_line("cannot hold the fallback grab either (another program holds the keyboard); retrying");
  // The curtain covers the screen either way: locked.
  if (d.state == State::Locking) became_locked();
  // A hung lock screen is replaced at once, a crashing one with a backoff.
  Fl::add_timeout(killed ? 0.1 : d.backoff.crashed(now_s()), respawn_cb);
}

void child_readable(int fd, void *) {
  char buf[64];
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
  if (n > 0) {
    d.child_heard = now_s();
    for (ssize_t i = 0; i < n; ++i) {
      switch (buf[i]) {
        case 'M':
          // Its windows are mapped above the curtain: let it grab.
          release_curtain_grab();
          break;
        case 'L':
          d.backoff.reset();
          became_locked();
          break;
        case 'W':
          // Covered, still waiting for a grab another program holds: the
          // screen is locked (nothing of the session is visible) and the
          // lock screen says so and keeps trying.
          log_line("the lock screen is up but another program holds the keyboard or the pointer");
          became_locked();
          break;
        case 'H': break;  // heartbeat
        case 'F': d.child_grab_failed = true; break;
        case 'U': d.child_unlocked = true; break;
        default: break;
      }
    }
    return;
  }
  // EOF: the lock screen process is gone.
  Fl::remove_fd(fd);
  close(fd);
  d.child_fd = -1;
  int status = 0;
  if (d.child > 0) {
    while (waitpid(d.child, &status, 0) < 0 && errno == EINTR) {
    }
  }
  child_exited(status);
}

void spawn_ui() {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    lock_failed(strerror(errno));
    return;
  }
  XFlush(fl_display);
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    if (d.state == State::Locking) lock_failed(strerror(errno));
    return;
  }
  if (pid == 0) {
    // The write end survives exec; nothing else of the daemon does
    // (the X and D-Bus connections are close-on-exec).
    int keep = fcntl(fds[1], F_GETFD);
    fcntl(fds[1], F_SETFD, keep & ~FD_CLOEXEC);
    signal(SIGPIPE, SIG_DFL);
    std::string fd = std::to_string(fds[1]);
    execl(d.exe.c_str(), "sde-lock", "--ui", fd.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  close(fds[1]);
  d.child = pid;
  d.child_fd = fds[0];
  d.child_heard = now_s();
  d.child_killed = false;
  Fl::add_fd(fds[0], FL_READ, child_readable);
}

// ------------------------------------------------------------ lock / unlock

void lock(DBusMessage *call) {
  if (call) {
    if (d.state == State::Locked) {
      d.session->send(dbus_message_new_method_return(call));
      return;
    }
    dbus_message_ref(call);
    d.waiting.push_back(call);
  }
  if (d.state != State::Unlocked) return;
  d.state = State::Locking;
  map_curtain();
  spawn_ui();
}

void force_unlock() {
  // logind's Unlock (loginctl unlock-session): an administrator or the
  // session's own user asked for it.
  if (d.state == State::Unlocked) return;
  d.forced_unlock = true;
  if (d.child > 0)
    kill(d.child, SIGTERM);
  else
    became_unlocked();
}

// ------------------------------------------------------------ D-Bus

const char *kIntrospect =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>"
    "<interface name=\"org.freedesktop.ScreenSaver\">"
    "<method name=\"Lock\"/>"
    "<method name=\"GetActive\"><arg type=\"b\" direction=\"out\"/></method>"
    "<method name=\"SetActive\"><arg type=\"b\" direction=\"in\"/><arg type=\"b\" direction=\"out\"/></method>"
    "<method name=\"GetActiveTime\"><arg type=\"u\" direction=\"out\"/></method>"
    "<method name=\"GetSessionIdleTime\"><arg type=\"u\" direction=\"out\"/></method>"
    "<method name=\"SimulateUserActivity\"/>"
    "<method name=\"Inhibit\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"u\" direction=\"out\"/></method>"
    "<method name=\"UnInhibit\"><arg type=\"u\" direction=\"in\"/></method>"
    "<signal name=\"ActiveChanged\"><arg type=\"b\"/></signal>"
    "</interface>"
    "<interface name=\"org.debion.SDE.ScreenLock\">"
    "<method name=\"Lock\"/>"
    "<method name=\"GetActive\"><arg type=\"b\" direction=\"out\"/></method>"
    "<method name=\"IsIdleInhibited\"><arg type=\"b\" direction=\"out\"/></method>"
    "<method name=\"ListInhibitors\"><arg type=\"a(uss)\" direction=\"out\"/></method>"
    "<method name=\"SimulateUserActivity\"/>"
    "<signal name=\"ActiveChanged\"><arg type=\"b\"/></signal>"
    "</interface>"
    "</node>";

DBusHandlerResult on_message(DBusMessage *msg) {
  if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *member_c = dbus_message_get_member(msg);
  if (!member_c) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  std::string m = member_c;
  const char *iface_c = dbus_message_get_interface(msg);
  std::string iface = iface_c ? iface_c : "";
  const char *sender_c = dbus_message_get_sender(msg);
  std::string sender = sender_c ? sender_c : "";
  if (iface == "org.freedesktop.DBus.Introspectable" && m == "Introspect") {
    DBusMessage *r = dbus_message_new_method_return(msg);
    dbus::Writer(r).str(kIntrospect);
    d.session->send(r);
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (!iface.empty() && iface != kFdoName && iface != kOwnName) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  auto args = dbus::read_args(msg);
  DBusMessage *r = nullptr;
  if (m == "Lock") {
    lock(msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  } else if (m == "GetActive") {
    r = dbus_message_new_method_return(msg);
    dbus::Writer(r).boolean(d.state == State::Locked);
  } else if (m == "SetActive") {
    bool on = !args.empty() && args[0].boolean();
    r = dbus_message_new_method_return(msg);
    if (on) {
      lock(nullptr);
      dbus::Writer(r).boolean(true);
    } else {
      // Deactivating never unlocks; it only wakes the monitors.
      session::reset_idle();
      dbus::Writer(r).boolean(d.state == State::Unlocked);
    }
  } else if (m == "GetActiveTime") {
    r = dbus_message_new_method_return(msg);
    uint32_t t = d.state == State::Locked && d.locked_at ? uint32_t(time(nullptr) - d.locked_at) : 0;
    dbus::Writer(r).u32(t);
  } else if (m == "GetSessionIdleTime") {
    r = dbus_message_new_method_return(msg);
    long idle = session::idle_ms();
    dbus::Writer(r).u32(idle > 0 ? uint32_t(idle / 1000) : 0);
  } else if (m == "SimulateUserActivity") {
    session::reset_idle();
    r = dbus_message_new_method_return(msg);
  } else if (m == "Inhibit") {
    std::string app = args.size() > 0 ? args[0].str() : "", why = args.size() > 1 ? args[1].str() : "";
    uint32_t cookie = d.inhibitors.add(sender, app, why);
    if (!cookie) {
      d.session->send(dbus::Bus::error(msg, DBUS_ERROR_LIMITS_EXCEEDED, "Too many inhibitors"));
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    r = dbus_message_new_method_return(msg);
    dbus::Writer(r).u32(cookie);
  } else if (m == "UnInhibit") {
    uint32_t cookie = args.empty() ? 0 : uint32_t(args[0].num());
    if (!d.inhibitors.remove(cookie, sender)) {
      d.session->send(dbus::Bus::error(msg, DBUS_ERROR_INVALID_ARGS, "Unknown cookie"));
      return DBUS_HANDLER_RESULT_HANDLED;
    }
    r = dbus_message_new_method_return(msg);
  } else if (m == "IsIdleInhibited") {
    r = dbus_message_new_method_return(msg);
    dbus::Writer(r).boolean(d.inhibitors.active());
  } else if (m == "ListInhibitors") {
    r = dbus_message_new_method_return(msg);
    dbus::Writer w(r);
    dbus::Writer list(w.iter(), DBUS_TYPE_ARRAY, "(uss)");
    for (const auto &in : d.inhibitors.list()) {
      dbus::Writer s(list.iter(), DBUS_TYPE_STRUCT, nullptr);
      s.u32(in.cookie).str(in.application).str(in.reason);
    }
  } else {
    d.session->send(dbus::Bus::error(msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method " + m));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  d.session->send(r);
  return DBUS_HANDLER_RESULT_HANDLED;
}

// ------------------------------------------------------------ logind

void setup_logind() {
  d.system = dbus::Bus::system();
  if (!d.system) return;
  DBusMessage *m = dbus::method(kLogin, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                "GetSessionByPID");
  dbus::Writer(m).u32(uint32_t(getpid()));
  DBusMessage *reply = d.system->call(m, 2000);
  if (!reply && !env("XDG_SESSION_ID").empty()) {
    m = dbus::method(kLogin, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "GetSession");
    dbus::Writer(m).str(env("XDG_SESSION_ID"));
    reply = d.system->call(m, 2000);
  }
  if (reply) {
    auto a = dbus::read_args(reply);
    if (!a.empty()) d.session_path = a[0].str();
    dbus_message_unref(reply);
  }
  if (!d.session_path.empty()) {
    std::string path = d.session_path;
    d.system->add_signal_handler(
        "type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Session',"
        "path='" + path + "'",
        [path](DBusMessage *s) {
          const char *p = dbus_message_get_path(s);
          return p && path == p;
        },
        [](DBusMessage *s) {
          if (dbus_message_is_signal(s, "org.freedesktop.login1.Session", "Lock")) lock(nullptr);
          else if (dbus_message_is_signal(s, "org.freedesktop.login1.Session", "Unlock")) force_unlock();
        });
  }
  d.system->add_signal_handler(
      "type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager',"
      "member='PrepareForSleep'",
      [](DBusMessage *s) {
        return dbus_message_is_signal(s, "org.freedesktop.login1.Manager", "PrepareForSleep");
      },
      [](DBusMessage *s) {
        auto a = dbus::read_args(s);
        bool start = !a.empty() && a[0].boolean();
        if (start) {
          if (d.lock_on_suspend && d.state == State::Unlocked)
            lock(nullptr);  // the inhibitor is released once locked
          else if (d.state == State::Locked)
            release_sleep_inhibitor();
          else if (!d.lock_on_suspend)
            release_sleep_inhibitor();
        } else {
          take_sleep_inhibitor();
          session::reset_idle();
          // Resumed: the monitors may have changed while asleep. The lock
          // screen checks them, its stacking and its grabs at once.
          if (d.state != State::Unlocked) {
            log_line("resumed while locked; re-checking the lock screen");
            if (d.child > 0)
              kill(d.child, SIGUSR1);
            else if (!d.curtain_grabbed)
              grab_curtain(1);
          }
        }
      });
  take_sleep_inhibitor();
}

void watch_owners() {
  d.session->add_signal_handler(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
      "member='NameOwnerChanged'",
      [](DBusMessage *s) { return dbus_message_is_signal(s, "org.freedesktop.DBus", "NameOwnerChanged"); },
      [](DBusMessage *s) {
        auto a = dbus::read_args(s);
        // An application that quit (or crashed) releases its inhibitors.
        if (a.size() == 3 && a[2].str().empty()) d.inhibitors.drop_owner(a[0].str());
      });
}

// ------------------------------------------------------------ idle

void load_config() {
  d.cfg.load();
  d.idle_lock = std::max(0, d.cfg.get_int("lock", "idle_lock", 300));
  bool was = d.lock_on_suspend;
  d.lock_on_suspend = d.cfg.get_bool("lock", "lock_on_suspend", true);
  if (was && !d.lock_on_suspend) release_sleep_inhibitor();
  if (!was && d.lock_on_suspend && d.state == State::Unlocked) take_sleep_inhibitor();
}

// A lock screen process that stopped sending its heartbeat is hung (it
// would show a lock screen that ignores the keyboard forever): replace it.
void watchdog() {
  if (d.state == State::Unlocked || d.child <= 0 || d.child_killed) return;
  if (!ui_unresponsive(now_s(), d.child_heard)) return;
  log_line("the lock screen (pid %d) has not answered for %.0f s; starting a fresh one",
           int(d.child), now_s() - d.child_heard);
  d.child_killed = true;
  kill(d.child, SIGKILL);
}

void idle_tick(void *) {
  long idle = session::idle_ms();
  double next = 5;
  if (should_idle_lock(idle, d.idle_lock, d.inhibitors.active(), d.state != State::Unlocked)) {
    lock(nullptr);
  } else if (d.idle_lock > 0 && idle >= 0 && d.state == State::Unlocked) {
    long left = long(d.idle_lock) * 1000 - idle;
    next = std::max(0.5, std::min(10.0, left / 1000.0 + 0.05));
  }
  // Keep the curtain on top (and the keyboard) while no lock screen covers it.
  if (d.state != State::Unlocked && d.child < 0) {
    for (Window w : d.curtain) XRaiseWindow(fl_display, w);
    if (!d.curtain_grabbed) grab_curtain(1);
    XFlush(fl_display);
  }
  if (d.state != State::Unlocked) next = std::min(next, 2.0);
  watchdog();
  Fl::repeat_timeout(next, idle_tick);
}

int x_events(int) {
  if (!fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == ConfigureNotify && e.xconfigure.window == RootWindow(fl_display, fl_screen)) {
    // RandR changed the screen size: the curtain follows.
    for (Window w : d.curtain)
      XMoveResizeWindow(fl_display, w, 0, 0, unsigned(std::max(1, e.xconfigure.width)),
                        unsigned(std::max(1, e.xconfigure.height)));
    XFlush(fl_display);
    return 0;
  }
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL"))
    load_config();
  return 0;
}

std::string own_executable() {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return find_program("sde-lock");
  std::string path(buf, size_t(n));
  // After a package upgrade the old file is gone: use the new one.
  const std::string deleted = " (deleted)";
  if (ends_with(path, deleted)) path.erase(path.size() - deleted.size());
  return path;
}

}  // namespace

int run_daemon(bool lock_now) {
  signal(SIGPIPE, SIG_IGN);
  d.exe = own_executable();
  fl_open_display();
  fcntl(ConnectionNumber(fl_display), F_SETFD, FD_CLOEXEC);
  d.session = dbus::Bus::session();
  if (!d.session) {
    fprintf(stderr, "sde-lock: no session bus\n");
    return 1;
  }
  if (!d.session->request_name(kOwnName)) {
    fprintf(stderr, "sde-lock: already running\n");
    return 0;
  }
  if (!d.session->request_name(kFdoName))
    fprintf(stderr, "sde-lock: %s is owned by another program\n", kFdoName);
  for (const char *path : {kOwnPath, kFdoPath, "/ScreenSaver"}) d.session->export_object(path, on_message);
  watch_owners();
  load_config();
  setup_logind();
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask | StructureNotifyMask);
  Fl::add_handler(x_events);
  Fl::add_timeout(2.0, idle_tick);
  if (lock_now) lock(nullptr);
  for (;;) Fl::wait(1e20);
}

}  // namespace lock
}  // namespace sde
