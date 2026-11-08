// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-watchdog: notices programs that stopped responding or never opened
// (0.2.0 Alpha 3.1, docs/FORCE-CLOSE.md). Started and restarted by
// sde-session.
//
//  * The window the user is using - the active window, after a change of
//    focus or a click anywhere (XInput 2 raw button events), and a window
//    someone asks to close (_NET_CLOSE_WINDOW) - gets an EWMH _NET_WM_PING
//    when it lists the protocol. Clients answer by sending the message back
//    to the root window, where this program listens; any answer from a
//    window (to this program's pings, the window manager's or the dialog's)
//    means its program handles events.
//  * Without an answer for [force_close] mark_after seconds (5) the window,
//    and every window of the same program, gets _SDE_NOT_RESPONDING: SDE WM
//    adds " - [Not Responding]" to its title, the panel dims its taskbar
//    button and offers Force Close. The pings go on, and the first answer
//    removes the mark.
//  * Without an answer for [force_close] timeout seconds (120), or as soon
//    as it is marked after a close attempt, sde-force-close asks "Wait /
//    Force Close" (one dialog per program). After Wait it asks again only
//    after another timeout.
//  * Programs SDE started (launches.h): when one still runs after the
//    timeout and neither it nor a process of its family shows a window,
//    sde-force-close asks "<App> has not opened yet". Wait there ends the
//    watch of that program (it may be meant to run in the background).
//
// It never ends a program itself: that is the dialog's Force Close, with
// its checks (process_rules.h).
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "core_client.h"
#include "crash.h"
#include "i18n.h"
#include "launches.h"
#include "util.h"
#include "watch_model.h"
#include "x11.h"
#include "xtarget.h"

using namespace sde;

namespace {

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct State {
  Display *dpy = nullptr;
  Window root = 0;
  Config cfg;
  watchdog::Model model;
  watchdog::Launches launches;
  std::map<Window, long> pids;          // managed windows and their programs
  std::map<Window, double> answered_at;  // the last answer, to space out pings
  std::map<pid_t, Window> dialogs;      // dialog process -> window
  std::map<pid_t, long> launch_dialogs;  // dialog process -> program
  std::map<long, double> dead_since;    // launch records of programs that ended
  unsigned long next_id = 0;
  int xi_opcode = -1;
  int inotify = -1;
  bool verbose = false;
} g;

// --verbose: what it does, one line each ("ping 0x1a00003").
void log(const std::string &what, const std::string &about) {
  if (g.verbose) fprintf(stderr, "sde-watchdog: %s %s\n", what.c_str(), about.c_str());
}
std::string hex(unsigned long w) {
  char buf[32];
  snprintf(buf, sizeof buf, "0x%lx", w);
  return buf;
}

Atom atom(const char *name) { return XInternAtom(g.dpy, name, False); }

// Pings of this program carry ids of their own, far from the small
// numbers of SDE WM's (Openbox counts from 1), so that neither takes the
// other's answer for its own.
unsigned long ping_id() { return 0x5DE00000UL | (++g.next_id & 0xFFFFFUL); }

long pid_of(Window w) {
  auto it = g.pids.find(w);
  if (it != g.pids.end()) return it->second;
  long pid = forceclose::window_pid(g.dpy, w);
  g.pids[w] = pid;
  return pid;
}

// Windows worth watching: managed application windows (not the panel or
// the desktop) that answer pings.
bool watchable(Window w) {
  return w && w != g.root && g.pids.count(w) && x11::window_is_normal(w) &&
         forceclose::supports_ping(g.dpy, w);
}

void ping(Window w) {
  forceclose::send_ping(g.dpy, w, ping_id());
  log("ping", hex(w));
}

void use(Window w, bool closing) {
  if (!g.model.settings().enabled || !watchable(w)) return;
  double t = now();
  // A window that answered a moment ago is not asked again for every
  // click of a double click.
  auto ok = g.answered_at.find(w);
  if (!closing && ok != g.answered_at.end() && t - ok->second < 0.3) return;
  bool send = closing ? g.model.closing(w, pid_of(w), t) : g.model.use(w, pid_of(w), t);
  if (send) ping(w);
}

// The windows of |w|'s program (|w| alone when it is unknown).
std::vector<Window> siblings(Window w) {
  long pid = pid_of(w);
  std::vector<Window> out{w};
  if (pid > 0)
    for (const auto &kv : g.pids)
      if (kv.second == pid && kv.first != w) out.push_back(kv.first);
  return out;
}

void mark(Window w, bool on) {
  long since = long(time(nullptr));
  for (Window s : siblings(w)) {
    XErrorHandler old = XSetErrorHandler([](Display *, XErrorEvent *) { return 0; });
    if (on)
      XChangeProperty(g.dpy, s, atom(forceclose::kNotResponding), XA_CARDINAL, 32, PropModeReplace,
                      reinterpret_cast<unsigned char *>(&since), 1);
    else
      XDeleteProperty(g.dpy, s, atom(forceclose::kNotResponding));
    XSync(g.dpy, False);
    XSetErrorHandler(old);
  }
  log(on ? "not responding" : "responding again", hex(w));
  // 0.2.0 Alpha 4: sde-core keeps the session's problems (docs/CORE.md).
  long pid = pid_of(w);
  launches::Record r;
  bool launched = pid > 0 && launches::load(pid, r);
  core::report(on ? "not-responding" : "responding", pid, launched && !r.name.empty() ? r.name : x11::window_title(w),
               launched ? r.desktop_id : std::string(), hex(w));
}

pid_t run_dialog(const std::vector<std::string> &argv) {
  std::vector<char *> args;
  for (const auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    close(ConnectionNumber(g.dpy));
    if (g.inotify >= 0) close(g.inotify);
    execvp(args[0], args.data());
    _exit(127);
  }
  return pid;
}

void show_dialog(Window w) {
  const auto *x = g.model.find(w);
  std::vector<std::string> argv = {"sde-force-close", "--window", hex(w), "--reason",
                                   x && x->closing ? "closing" : "frozen"};
  pid_t pid = run_dialog(argv);
  if (pid > 0) g.dialogs[pid] = w;
  else g.model.dialog_closed(w, false, now());
  log("dialog for", hex(w));
}

void client_list_changed() {
  bool has_list = false;
  auto list = forceclose::client_list(g.dpy, &has_list);
  std::map<Window, long> next;
  for (Window w : list) {
    auto it = g.pids.find(w);
    if (it != g.pids.end()) {
      next[w] = it->second;
    } else {
      next[w] = forceclose::window_pid(g.dpy, w);
      // A window of the program a launch waits for: it opened.
      for (long launch : g.launches.due(1e300))
        if (next[w] > 0 && forceclose::in_family(next[w], launch)) g.launches.forget(launch);
    }
  }
  for (const auto &kv : g.pids)
    if (!next.count(kv.first)) {
      g.model.gone(kv.first);
      g.answered_at.erase(kv.first);
    }
  g.pids.swap(next);
}

void read_launches() {
  long long epoch = static_cast<long long>(time(nullptr));
  double t = now(), timeout = g.model.settings().timeout;
  for (const auto &r : launches::all()) {
    if (g.launches.has(r.pid)) continue;
    double age = double(epoch - r.time);
    // After a restart of the watchdog, programs started long ago are
    // left alone (they were watched before, or run in the background).
    if (age > 2 * timeout || !launches::alive(r)) continue;
    g.launches.add(r.pid, t, age, timeout);
    log("watching the start of", r.name + " " + std::to_string(r.pid));
  }
}

// Records of programs that ended without sde-session removing them (not
// its children); a grace period leaves sde-session the first look.
void sweep_records() {
  double t = now();
  for (const auto &r : launches::all()) {
    if (launches::alive(r)) {
      g.dead_since.erase(r.pid);
      continue;
    }
    auto it = g.dead_since.find(r.pid);
    if (it == g.dead_since.end()) g.dead_since[r.pid] = t;
    else if (t - it->second > 10) {
      launches::remove(r.pid);
      g.dead_since.erase(it);
      g.launches.forget(r.pid);
    }
  }
}

void check_launches() {
  double t = now();
  for (long pid : g.launches.due(t)) {
    launches::Record r;
    if (!launches::load(pid, r) || !launches::alive(r)) {
      g.launches.forget(pid);
      continue;
    }
    if (forceclose::family_shows_window(g.dpy, pid)) {
      g.launches.forget(pid);
      continue;
    }
    g.launches.asked(pid);
    pid_t dialog = run_dialog({"sde-force-close", "--launch", std::to_string(pid)});
    if (dialog > 0) g.launch_dialogs[dialog] = pid;
    log("has not opened yet:", r.name + " " + std::to_string(pid));
    core::report("not-opened", pid, r.name, r.desktop_id, "");
  }
}

void reap_dialogs() {
  int status = 0;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    auto d = g.dialogs.find(pid);
    log("dialog ended:", std::to_string(code));
    if (d != g.dialogs.end()) {
      // 0 forced to close and 2 resolved: done. 4 refused (a part of the
      // session...): never again. 1 Wait, 3 another dialog is open (SDE
      // WM's), and anything else (no sde-force-close): after another
      // timeout, so that nothing is started over and over.
      if (code == 4) g.model.never_ask(d->second);
      else g.model.dialog_closed(d->second, code != 0 && code != 2, now());
      g.dialogs.erase(d);
    }
    auto l = g.launch_dialogs.find(pid);
    if (l != g.launch_dialogs.end()) {
      g.launches.forget(l->second);  // waited, ended or opened: not asked again
      g.launch_dialogs.erase(l);
    }
  }
}

void schedule();

void tick(void *) {
  reap_dialogs();
  auto due = g.model.tick(now());
  for (Window w : due.ping) ping(w);
  for (Window w : due.mark) mark(w, true);
  for (Window w : due.dialog) show_dialog(w);
  check_launches();
  XFlush(g.dpy);
  schedule();
}

void schedule() {
  double t = now();
  double next = 30;  // the record sweep
  for (double d : {g.model.next(t), g.launches.next(t)})
    if (d >= 0) next = std::min(next, d);
  if (!g.dialogs.empty() || !g.launch_dialogs.empty()) next = std::min(next, 0.5);
  Fl::remove_timeout(tick);
  Fl::add_timeout(std::max(0.05, next + 0.01), tick);
}

void sweep_tick(void *) {
  sweep_records();
  read_launches();
  Fl::repeat_timeout(30, sweep_tick);
}

void reload() {
  g.cfg.load();
  g.model.configure(watchdog::settings(g.cfg));
}

void on_inotify(int fd, void *) {
  char buf[4096];
  while (read(fd, buf, sizeof buf) > 0) {
  }
  read_launches();
  schedule();
}

int on_event(void *event, void *) {
  const XEvent &e = *static_cast<const XEvent *>(event);
  if (e.type == GenericEvent && e.xcookie.extension == g.xi_opcode &&
      e.xcookie.evtype == XI_RawButtonPress) {
    // Clicks only: the wheel's buttons (4 to 7) come many at a time.
    XGenericEventCookie cookie = e.xcookie;
    int button = 0;
    if (XGetEventData(g.dpy, &cookie)) {
      button = static_cast<XIRawEvent *>(cookie.data)->detail;
      XFreeEventData(g.dpy, &cookie);
    }
    if (button < 4 || button > 7) use(x11::get_window(g.root, "_NET_ACTIVE_WINDOW"), false);
    schedule();
    return 0;
  }
  if (e.type == PropertyNotify && e.xproperty.window == g.root) {
    if (e.xproperty.atom == atom("_NET_CLIENT_LIST")) {
      client_list_changed();
    } else if (e.xproperty.atom == atom("_NET_ACTIVE_WINDOW")) {
      client_list_changed();
      use(x11::get_window(g.root, "_NET_ACTIVE_WINDOW"), false);
    } else if (e.xproperty.atom == atom("_SDE_CONFIG_SERIAL")) {
      reload();
    }
    schedule();
    return 0;
  }
  if (e.type == ClientMessage && e.xclient.window == g.root) {
    Window w = 0;
    unsigned long id = 0;
    if (forceclose::is_pong(g.dpy, e, &w, &id)) {
      g.answered_at[w] = now();
      for (Window m : g.model.answered(w)) mark(m, false);
      // A mark left by the window manager or a previous watchdog goes too.
      if (!x11::get_cardinals(w, forceclose::kNotResponding).empty()) mark(w, false);
      schedule();
      return 0;
    }
    if (e.xclient.message_type == atom("_NET_CLOSE_WINDOW")) {
      use(e.xclient.window == g.root ? 0 : e.xclient.window, true);
      schedule();
    }
  }
  return 0;
}

int run() {
  fl_open_display();
  g.dpy = fl_display;
  g.root = DefaultRootWindow(g.dpy);
  reload();
  XWindowAttributes attr;
  XGetWindowAttributes(g.dpy, g.root, &attr);
  XSelectInput(g.dpy, g.root, attr.your_event_mask | PropertyChangeMask | SubstructureNotifyMask);
  int ev, err;
  if (XQueryExtension(g.dpy, "XInputExtension", &g.xi_opcode, &ev, &err)) {
    int major = 2, minor = 0;
    if (XIQueryVersion(g.dpy, &major, &minor) == Success) {
      unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
      XISetMask(bits, XI_RawButtonPress);
      XIEventMask mask{XIAllMasterDevices, int(sizeof bits), bits};
      XISelectEvents(g.dpy, g.root, &mask, 1);
    } else {
      g.xi_opcode = -1;
    }
  }
  Fl::add_system_handler(on_event, nullptr);
  client_list_changed();
  // Marks left behind (a previous watchdog): ask those windows again.
  for (const auto &kv : g.pids)
    if (!x11::get_cardinals(kv.first, forceclose::kNotResponding).empty()) use(kv.first, false);
  std::string dir = launches::dir();
  make_dirs(dir);
  g.inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (g.inotify >= 0 && inotify_add_watch(g.inotify, dir.c_str(), IN_MOVED_TO | IN_CLOSE_WRITE) >= 0)
    Fl::add_fd(g.inotify, FL_READ, on_inotify);
  read_launches();
  Fl::add_timeout(30, sweep_tick);
  XFlush(g.dpy);
  schedule();
  for (;;) Fl::wait(1e20);
}

void usage(FILE *out) {
  fprintf(out, "%s", _("Usage: sde-watchdog [--verbose | --version | --help]\n\n"
                       "Notices programs that stop responding or never open a window and asks\n"
                       "whether to wait for them or to force them to close. Started by\n"
                       "sde-session; [force_close] in sde.conf sets it up.\n"));
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  install_crash_handler("sde-watchdog");
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") return print_version("sde-watchdog", SDE_VERSION);
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    }
    if (a == "--verbose") {
      g.verbose = true;
      continue;
    }
    usage(stderr);
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  return run();
}
