// SPDX-License-Identifier: GPL-3.0-or-later
// sde-session: starts and supervises the Selviana Desktop Environment.
//
// Responsibilities:
//   * export the session environment (XDG_CURRENT_DESKTOP=SDE-Desktop:SDE, ...)
//   * apply saved display and input settings (sde-settings --apply)
//   * start the window manager, sde-desktop, sde-panel, sde-notifyd and
//     sde-watchdog, restarting them when they crash (with a limit, so a
//     broken component cannot loop)
//   * adopt the programs SDE starts (child subreaper): when one that SDE
//     recorded (launches.h) dies from a signal, log it for System Reports
//     and announce it with "Open Again" (sde-force-close --crashed)
//   * start a polkit authentication agent and XDG autostart applications
//   * log everything to ~/.local/state/sde/session.log and show a rescue
//     dialog instead of a black screen when the desktop cannot start
//   * end the session cleanly on "sde-session --logout" or SIGTERM
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#ifdef SDE_HAVE_XDAMAGE
#include <X11/extensions/Xdamage.h>
#endif
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "core_link.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "launches.h"
#include "spawn.h"
#include "util.h"
#include "wm.h"
#include "integration.h"
#include "process.h"
#include "theme.h"

using namespace sde;

namespace {

struct Component {
  std::string name;
  std::vector<std::string> argv;
  bool essential = false;     // restart when it exits, even with status 0
  bool critical = false;      // the desktop is unusable without it
  int max_restarts = 5;       // within 60 seconds
  // Started once instead when the program fails, e.g. the compositor
  // without OpenGL falls back to XRender.
  std::vector<std::string> fallback;
  int fallback_after = 1;  // crashes before the fallback is used
  int crashes = 0;
  pid_t pid = 0;
  std::deque<time_t> starts;  // recent start times, for crash-loop detection
  time_t restart_at = 0;
  bool given_up = false;
};

std::vector<Component> g_components;
Display *g_display = nullptr;
// Owns _SDE_SESSION_S<screen>: programs ask it for theme switches
// (_SDE_APPEARANCE_APPLY) and find out when they are done
// (_SDE_APPEARANCE_DONE on it).
Window g_session_win = None;
int g_log_fd = -1;          // session.log, inherited by children as stdout/stderr
int g_console_fd = -1;      // the original stderr (.xsession-errors)
pid_t g_rescue_pid = 0;
std::vector<std::string> g_problems;
time_t g_compositor_check = 0;  // when to look for a black screen
int g_black_samples = 0;

Component make_component(const std::string &name, const std::vector<std::string> &argv,
                         bool essential, bool critical = false) {
  Component c;
  c.name = name;
  c.argv = argv;
  c.essential = essential;
  c.critical = critical;
  return c;
}

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char *fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  char stamp[32];
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  strftime(stamp, sizeof stamp, "%H:%M:%S", &lt);
  std::string line = std::string(stamp) + " sde-session: " + msg + "\n";
  if (g_log_fd >= 0 && write(g_log_fd, line.data(), line.size()) < 0) {
  }
  int out = g_console_fd >= 0 ? g_console_fd : 2;
  std::string plain = std::string("sde-session: ") + msg + "\n";
  if (out != g_log_fd && write(out, plain.data(), plain.size()) < 0) {
  }
}

std::string log_path() { return state_home() + "/sde/session.log"; }

// Opens a fresh session.log (keeping the previous one as session.log.old) and
// sends this process' and its children's output to it.
void open_log() {
  make_dirs(state_home() + "/sde");
  std::string path = log_path();
  rename(path.c_str(), (path + ".old").c_str());
  g_log_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (g_log_fd < 0) return;
  g_console_fd = fcntl(2, F_DUPFD_CLOEXEC, 10);
  // Children inherit fds 1 and 2.
  dup2(g_log_fd, 1);
  dup2(g_log_fd, 2);
}

std::string pid_file() { return runtime_dir() + "/sde-session-" + env("DISPLAY") + ".pid"; }

pid_t start_child(const std::vector<std::string> &argv) {
  if (argv.empty() || find_program(argv[0]).empty()) return -1;
  std::vector<char *> args;
  for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    if (g_display) close(ConnectionNumber(g_display));
    if (chdir(home_dir().c_str()) != 0) { /* stay where we are */ }
    execvp(args[0], args.data());
    _exit(127);
  }
  return pid;
}

void show_rescue(const std::string &problem);

void start_component(Component &c) {
  c.pid = start_child(c.argv);
  c.starts.push_back(time(nullptr));
  while (int(c.starts.size()) > c.max_restarts) c.starts.pop_front();
  if (c.pid > 0) core_link::report("component-started", c.pid, c.name, "", "");
  if (c.pid <= 0) {
    log("cannot start %s: %s was not found", c.name.c_str(), c.argv[0].c_str());
    c.given_up = true;
    if (c.critical)
      show_rescue(format(_("The %s could not be started because \"%s\" is not installed."),
                         c.name.c_str(), c.argv[0].c_str()));
  }
}

const char *exit_reason(int status, char *buf, size_t n) {
  if (WIFSIGNALED(status)) snprintf(buf, n, "killed by signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
  else if (WEXITSTATUS(status) == 127) snprintf(buf, n, "status 127: program or shared library missing");
  else snprintf(buf, n, "status %d", WEXITSTATUS(status));
  return buf;
}

// Signals that mean a program crashed (not that it was asked to stop or was
// forced to close).
bool crash_signal(int sig) {
  return sig == SIGSEGV || sig == SIGABRT || sig == SIGBUS || sig == SIGILL || sig == SIGFPE ||
         sig == SIGSYS || sig == SIGTRAP;
}

// A program SDE started ended (it was adopted here, the session's child
// subreaper): its launch record goes, and a crash is logged ("<name> exited
// (killed by signal ...)", which System Reports lists) and announced.
void on_program_exit(pid_t pid, int status) {
  launches::Record r;
  if (!launches::load(long(pid), r)) return;
  launches::remove(long(pid));
  std::string name = r.name.empty() && !r.argv.empty() ? basename_of(r.argv[0]) : r.name;
  if (!WIFSIGNALED(status) || !crash_signal(WTERMSIG(status))) {
    core_link::report("ended", pid, name, r.desktop_id, "");
    return;
  }
  char why[128];
  log("%s exited (%s)", name.c_str(), exit_reason(status, why, sizeof why));
  core_link::report("crashed", pid, name, r.desktop_id, std::to_string(WTERMSIG(status)));
  std::vector<std::string> cmd = {"sde-force-close", "--crashed", "--name", name, "--icon", r.icon,
                                  "--desktop-id", r.desktop_id, "--signal",
                                  std::to_string(WTERMSIG(status)), "--"};
  cmd.insert(cmd.end(), r.argv.begin(), r.argv.end());
  start_child(cmd);
}

void on_child_exit(pid_t pid, int status) {
  if (pid == g_rescue_pid) {
    g_rescue_pid = 0;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
    if (code == 10) open_terminal();
    else if (code == 11) kill(getpid(), SIGTERM);
    return;
  }
  for (auto &c : g_components) {
    if (c.pid != pid) continue;
    c.pid = 0;
    bool crashed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    char why[128];
    exit_reason(status, why, sizeof why);
    core_link::report(crashed ? "component-crashed" : "component-stopped", pid, c.name, "", why);
    if (!crashed && !c.essential) return;
    log("%s exited (%s)", c.name.c_str(), why);
    time_t now = time(nullptr);
    if (crashed && !c.fallback.empty() && ++c.crashes >= c.fallback_after) {
      log("starting %s with its fallback configuration", c.name.c_str());
      if (c.name == _("panel"))
        start_child({"notify-send", "-i", "dialog-warning", _("Panel started in safe mode"),
                     _("The panel stopped several times, so tray icons and device status are off "
                       "for this session. The crash details are in ~/.local/state/sde/session.log.")});
      c.argv = c.fallback;
      c.fallback.clear();
      c.starts.clear();
      c.restart_at = now + 1;
      return;
    }
    if (int(c.starts.size()) >= c.max_restarts && now - c.starts.front() < 60) {
      log("%s keeps failing; not restarting it again", c.name.c_str());
      c.given_up = true;
      core_link::report("component-given-up", 0, c.name, "", why);
      if (c.critical)
        show_rescue(format(_("The %s keeps stopping (%s)."), c.name.c_str(), why));
      return;
    }
    c.restart_at = now + 1;
    return;
  }
  on_program_exit(pid, status);
}

// The optional SDE compositor. A user's own compositor.conf wins; otherwise
// the [appearance] style and [aero] settings generate one (OpenGL blur,
// XRender if OpenGL is unavailable or fails).
bool g_auto_dark = theme_mode_dark("auto");

// Announces a configuration change to every SDE program (as
// x11::notify_config_changed() does in FLTK programs).
void bump_config_serial() {
  Atom serial = XInternAtom(g_display, "_SDE_CONFIG_SERIAL", False), type;
  Window root = DefaultRootWindow(g_display);
  int format = 0;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  long value = 1;
  if (XGetWindowProperty(g_display, root, serial, 0, 1, False, XA_CARDINAL, &type, &format, &n, &after,
                         &data) == Success && data) {
    if (n == 1 && format == 32) value = reinterpret_cast<long *>(data)[0] + 1;
    XFree(data);
  }
  XChangeProperty(g_display, root, serial, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char *>(&value), 1);
  XFlush(g_display);
}

void add_compositor(const Config &cfg) {
  if (cfg.get("compositor", "mode", "auto") == "off") return;
  std::string conf = config_home() + "/sde/compositor.conf";
  bool own = file_exists(conf);
  if (!own) conf = write_compositor_config(cfg);
  Component compositor = make_component("SDE compositor", {"sde-compositor", "--config", conf}, false);
  if (!own && conf.find("-xrender.conf") == std::string::npos)
    compositor.fallback = {"sde-compositor", "--config", write_compositor_config(cfg, true)};
  compositor.max_restarts = 1;
  g_components.push_back(compositor);
  start_component(g_components.back());
  g_compositor_check = time(nullptr) + 8;
  g_black_samples = 0;
}

int ignore_x_error(Display *, XErrorEvent *) { return 0; }

// True when every sampled point of the screen is pure black. Used only
// while the desktop (which draws the wallpaper) is running, so a working
// screen is never black everywhere.
bool screen_is_black() {
  if (!g_display) return false;
  XErrorHandler old = XSetErrorHandler(ignore_x_error);
  Window root = DefaultRootWindow(g_display);
  int w = DisplayWidth(g_display, DefaultScreen(g_display));
  int h = DisplayHeight(g_display, DefaultScreen(g_display));
  bool black = true;
  int sampled = 0;
  for (int gy = 1; gy <= 5 && black; ++gy) {
    for (int gx = 1; gx <= 5 && black; ++gx) {
      XImage *img = XGetImage(g_display, root, w * gx / 6, h * gy / 6, 1, 1, AllPlanes, ZPixmap);
      if (!img) continue;
      ++sampled;
      if ((XGetPixel(img, 0, 0) & 0xFFFFFF) != 0) black = false;
      XDestroyImage(img);
    }
  }
  XSync(g_display, False);
  XSetErrorHandler(old);
  return black && sampled > 0;
}

bool component_running(const std::string &name) {
  for (auto &c : g_components)
    if (c.name == name && c.pid > 0) return true;
  return false;
}

Component *find_component(const std::string &name) {
  for (auto &c : g_components)
    if (c.name == name && c.pid > 0) return &c;
  return nullptr;
}

// ------------------------------------------------- switching the look
//
// 0.2.0 Alpha 3.1 (docs/THEMES.md, "Switching themes"). A new theme reaches
// every program in one step: the session is the only one that tells its
// programs, and each of them once and only when its input changed:
// sde-xsettingsd (GTK) re-reads its file, SDE WM reconfigures, the
// compositor reloads its configuration in place (SIGUSR1, see
// suite/patches/sde-compositor/0003) and SDE programs follow
// _SDE_CONFIG_SERIAL. Up to 0.2.0 Alpha 3 the Control Center and the
// session both did it, with pkill for every process of the user: the
// compositor was reset twice per switch (2.8 s of frozen screen with
// software OpenGL, a blank panel with XRender) and xsettingsd re-read its
// file at every configuration change of any kind.
//
// A switch asked for with _SDE_APPEARANCE_APPLY (the Control Center's
// Apply, the panel's dark mode, "sde-session --apply-appearance", the
// automatic mode at 07:00 and 19:00) happens behind a picture of the
// screen: an override-redirect window shows the screen as it was, the
// theme is applied behind it, and once the programs have redrawn (GTK 3
// restyles in two steps some 200 ms apart, the panel, the window frames and
// the applications each at their own moment) the picture fades out, so the
// old look turns into the new one in one calm step.

// What the session's programs read last (their files at start or at the
// last delivery).
std::string g_sent_xsettings, g_sent_wm, g_sent_compositor;

std::string read_text(const std::string &path) {
  std::string text;
  read_file(path, text);
  return text;
}

void remember_delivered(const Config &cfg) {
  g_sent_xsettings = read_text(config_home() + "/sde/xsettingsd.conf");
  g_sent_wm = read_text(openbox_config_path());
  g_sent_compositor = compositor_inputs(cfg);
}

// Tells the programs what changed since they last read their files.
AppearanceFiles deliver(const Config &cfg) {
  AppearanceFiles d;
  std::string xs = read_text(config_home() + "/sde/xsettingsd.conf");
  if (xs != g_sent_xsettings) {
    g_sent_xsettings = xs;
    d.gtk = true;
    if (Component *c = find_component("GTK settings"))
      kill(c->pid, SIGHUP);
    else if (pid_t pid = selection_owner_pid(g_display, "_XSETTINGS_S" + std::to_string(DefaultScreen(g_display)),
                                             "sde-xsettingsd|xsettingsd"))
      kill(pid, SIGHUP);
  }
  std::string wm = read_text(openbox_config_path());
  if (wm != g_sent_wm) {
    g_sent_wm = wm;
    d.wm = true;
    reconfigure_window_manager(g_display);
  }
  std::string comp = compositor_inputs(cfg);
  if (comp != g_sent_compositor) {
    if (!env("SDE_THEME_SWITCH_DEBUG").empty()) {
      auto a = split(g_sent_compositor, '\n'), b = split(comp, '\n');
      for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        if (i >= a.size() || i >= b.size() || a[i] != b[i]) {
          log("compositor input differs at line %zu: \"%s\" -> \"%s\"", i + 1, i < a.size() ? a[i].c_str() : "",
              i < b.size() ? b[i].c_str() : "");
          break;
        }
    }
    g_sent_compositor = comp;
    d.compositor = true;
    if (Component *c = find_component("SDE compositor")) kill(c->pid, SIGUSR1);
  }
  if (d.any())
    log("appearance delivered:%s%s%s", d.gtk ? " GTK settings" : "", d.wm ? " window manager" : "",
        d.compositor ? " compositor (reloaded in place)" : "");
  XFlush(g_display);
  return d;
}

// Writes what sde.conf says, tells the programs and announces the change.
void publish_appearance(Config &cfg) {
  cfg.load();
  write_appearance_files(cfg);
  deliver(cfg);
  bump_config_serial();
}

using Clock = std::chrono::steady_clock;

struct Transition {
  enum Stage { IDLE, SHOWN, HOLD, FADE } stage = IDLE;
  Window win = None;
  Pixmap picture = None;
  Clock::time_point since;
  Clock::time_point busy;       // the last time a window redrew much of itself
  bool fade = false;            // the compositor fades the picture out
  std::vector<long> requests;   // answered in _SDE_APPEARANCE_DONE when done
  std::vector<unsigned long> damages;  // the windows watched while held
} g_transition;

// Durations of a switch: the picture is drawn by the compositor, then the
// new look is published behind it and held until the programs have redrawn
// (no window redrew much of itself for kQuietSeconds, at least
// kHoldSeconds, at most kMaxHoldSeconds: GTK 3 restyles in two steps whose
// second one came 0.2 s to 0.9 s after the change in the measurements), and
// faded out.
constexpr double kShownSeconds = 0.06, kHoldSeconds = 0.25, kQuietSeconds = 0.18, kMaxHoldSeconds = 1.6,
                 kFadeSeconds = 0.22;
int g_damage_event = -1;  // XDamage's first event, or -1 without it

Atom atom(const char *name) { return XInternAtom(g_display, name, False); }

void set_cardinal(Window w, const char *name, long value) {
  XChangeProperty(g_display, w, atom(name), XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char *>(&value), 1);
}

// True when the sampled pixels of |p| are all black: the X server could not
// give a picture of the screen (some OpenGL drivers), so none is shown.
bool picture_is_black(Pixmap p, int w, int h) {
  XErrorHandler old = XSetErrorHandler(ignore_x_error);
  bool black = true;
  for (int gy = 1; gy <= 4 && black; ++gy)
    for (int gx = 1; gx <= 4 && black; ++gx) {
      XImage *img = XGetImage(g_display, p, w * gx / 5, h * gy / 5, 1, 1, AllPlanes, ZPixmap);
      if (!img) continue;
      if ((XGetPixel(img, 0, 0) & 0xFFFFFF) != 0) black = false;
      XDestroyImage(img);
    }
  XSync(g_display, False);
  XSetErrorHandler(old);
  return black;
}

bool compositor_running() {
  return XGetSelectionOwner(g_display, atom(("_NET_WM_CM_S" + std::to_string(DefaultScreen(g_display))).c_str())) !=
         None;
}

// Covers the screen with a picture of itself. False when there is no
// picture to show.
bool show_picture() {
  int screen = DefaultScreen(g_display);
  Window root = RootWindow(g_display, screen);
  int w = DisplayWidth(g_display, screen), h = DisplayHeight(g_display, screen);
  int depth = DefaultDepth(g_display, screen);
  Pixmap picture = XCreatePixmap(g_display, root, unsigned(w), unsigned(h), unsigned(depth));
  XGCValues gv{};
  gv.subwindow_mode = IncludeInferiors;
  GC gc = XCreateGC(g_display, picture, GCSubwindowMode, &gv);
  XCopyArea(g_display, root, picture, gc, 0, 0, unsigned(w), unsigned(h), 0, 0);
  XFreeGC(g_display, gc);
  if (picture_is_black(picture, w, h)) {
    XFreePixmap(g_display, picture);
    return false;
  }
  XSetWindowAttributes a{};
  a.override_redirect = True;
  a.background_pixmap = picture;
  a.backing_store = NotUseful;
  Window win = XCreateWindow(g_display, root, 0, 0, unsigned(w), unsigned(h), 0, depth, InputOutput,
                             DefaultVisual(g_display, screen), CWOverrideRedirect | CWBackPixmap | CWBackingStore,
                             &a);
  // Clicks reach the windows behind it (it is up for half a second).
  XShapeCombineRectangles(g_display, win, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
  XStoreName(g_display, win, "SDE theme switch");
  XClassHint hint{const_cast<char *>("sde-theme-switch"), const_cast<char *>("SDE-Theme-Switch")};
  XSetClassHint(g_display, win, &hint);
  // The compositor neither shadows, rounds, blurs nor fades it (the
  // generated configuration excludes _SDE_CURTAIN), and keeps compositing
  // while it covers the screen: just below opaque, it is not a solid
  // fullscreen window to unredirect.
  set_cardinal(win, "_SDE_CURTAIN", 1);
  Atom desktop = atom("_NET_WM_WINDOW_TYPE_DESKTOP");
  XChangeProperty(g_display, win, atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char *>(&desktop), 1);
  set_cardinal(win, "_NET_WM_WINDOW_OPACITY", 0xFFFFFFFEL);
  set_cardinal(win, "_NET_WM_BYPASS_COMPOSITOR", 2);
  XMapRaised(g_display, win);
  XSync(g_display, False);
  g_transition.win = win;
  g_transition.picture = picture;
  return true;
}

// Watches the windows the user sees (clients, and popups that do not cover
// the screen; not the compositor's overlay) for redraws while the picture
// is held.
bool has_wm_state(Window w) {
  Atom type;
  int fmt = 0;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  bool yes = XGetWindowProperty(g_display, w, atom("WM_STATE"), 0, 0, False, AnyPropertyType, &type, &fmt, &n,
                                &after, &data) == Success && type != None;
  if (data) XFree(data);
  return yes;
}

void watch_redraws() {
#ifdef SDE_HAVE_XDAMAGE
  if (g_damage_event < 0) return;
  XErrorHandler old = XSetErrorHandler(ignore_x_error);
  int screen = DefaultScreen(g_display);
  int sw = DisplayWidth(g_display, screen), sh = DisplayHeight(g_display, screen);
  auto watch = [&](Window w) {
    // Every rectangle on its own: a spinner and a caret far apart must
    // not add up to one large box.
    g_transition.damages.push_back(XDamageCreate(g_display, w, XDamageReportRawRectangles));
  };
  Window root_ret, parent, *kids = nullptr;
  unsigned n = 0;
  if (XQueryTree(g_display, RootWindow(g_display, screen), &root_ret, &parent, &kids, &n)) {
    for (unsigned i = 0; i < n; ++i) {
      Window w = kids[i];
      XWindowAttributes a;
      if (w == g_transition.win || w == g_session_win || !XGetWindowAttributes(g_display, w, &a) ||
          a.map_state != IsViewable || a.c_class == InputOnly)
        continue;
      if (has_wm_state(w)) {
        watch(w);
        continue;
      }
      // A window manager's frame: the client inside it.
      Window r2, p2, *inner = nullptr;
      unsigned m = 0;
      bool client = false;
      if (XQueryTree(g_display, w, &r2, &p2, &inner, &m)) {
        for (unsigned k = 0; k < m; ++k)
          if (has_wm_state(inner[k])) {
            watch(inner[k]);
            client = true;
          }
        if (inner) XFree(inner);
      }
      // Popups and menus, but not what covers the whole screen without
      // being a client (the compositor's overlay window).
      if (!client && a.override_redirect && !(a.width >= sw && a.height >= sh)) watch(w);
    }
    if (kids) XFree(kids);
  }
  XSync(g_display, False);
  XSetErrorHandler(old);
#endif
}

void unwatch_redraws() {
#ifdef SDE_HAVE_XDAMAGE
  XErrorHandler old = XSetErrorHandler(ignore_x_error);
  for (unsigned long d : g_transition.damages) XDamageDestroy(g_display, d);
  XSync(g_display, False);
  XSetErrorHandler(old);
#endif
  g_transition.damages.clear();
}

// A window redrew: all or most of it (a theme change) keeps the picture
// up; a spinner, a caret or a clock does not.
bool handle_redraw(XEvent &ev) {
#ifdef SDE_HAVE_XDAMAGE
  if (g_damage_event < 0 || ev.type != g_damage_event + XDamageNotify) return false;
  auto *de = reinterpret_cast<XDamageNotifyEvent *>(&ev);
  long area = long(de->area.width) * de->area.height, full = long(de->geometry.width) * de->geometry.height;
  // A theme change repaints whole windows; animations (spinners, a video,
  // a caret) repaint parts of them on every frame and must not keep the
  // picture up (gtk3-widget-factory's spinners: a fifth of its window).
  bool big = area >= 4000 && area * 2 >= full;
  if (big) g_transition.busy = Clock::now();
  if (!env("SDE_THEME_SWITCH_DEBUG").empty())
    log("redraw of 0x%lx: %dx%d+%d+%d of %dx%d%s", de->drawable, de->area.width, de->area.height, de->area.x,
        de->area.y, de->geometry.width, de->geometry.height, big ? " (counts)" : "");
  return true;
#else
  (void)ev;
  return false;
#endif
}

void hide_picture() {
  if (g_transition.win != None) XDestroyWindow(g_display, g_transition.win);
  if (g_transition.picture != None) XFreePixmap(g_display, g_transition.picture);
  g_transition.win = None;
  g_transition.picture = None;
  XFlush(g_display);
}

void finish_transition() {
  unwatch_redraws();
  hide_picture();
  for (long r : g_transition.requests) set_cardinal(g_session_win, "_SDE_APPEARANCE_DONE", r);
  g_transition.requests.clear();
  g_transition.stage = Transition::IDLE;
  XFlush(g_display);
}

// Applies the appearance in sde.conf behind a picture of the screen.
// |request|: the number to answer (0 for the session's own switches).
void start_transition(Config &cfg, long request) {
  if (request) g_transition.requests.push_back(request);
  if (g_transition.stage == Transition::HOLD || g_transition.stage == Transition::FADE) {
    // Another change while the picture is up: publish it behind the
    // picture too and hold again.
    if (g_transition.stage == Transition::FADE) set_cardinal(g_transition.win, "_NET_WM_WINDOW_OPACITY", 0xFFFFFFFEL);
    publish_appearance(cfg);
    g_transition.stage = Transition::HOLD;
    g_transition.since = g_transition.busy = Clock::now();
    return;
  }
  if (g_transition.stage == Transition::SHOWN) return;  // published when it is drawn
  cfg.load();
  g_transition.fade = compositor_running() && cfg.get_bool("aero", "animations", true);
  if (env("SDE_THEME_SWITCH") == "instant" || !show_picture()) {
    publish_appearance(cfg);
    finish_transition();
    return;
  }
  g_transition.stage = Transition::SHOWN;
  g_transition.since = Clock::now();
}

// Advances a switch; the milliseconds until it needs this again (-1: idle).
int step_transition(Config &cfg) {
  if (g_transition.stage == Transition::IDLE) return -1;
  double t = std::chrono::duration<double>(Clock::now() - g_transition.since).count();
  switch (g_transition.stage) {
    case Transition::SHOWN:
      if (t < kShownSeconds) return int((kShownSeconds - t) * 1000) + 1;
      watch_redraws();
      publish_appearance(cfg);
      g_transition.stage = Transition::HOLD;
      g_transition.since = g_transition.busy = Clock::now();
      return int(kHoldSeconds * 1000);
    case Transition::HOLD: {
      double quiet = std::chrono::duration<double>(Clock::now() - g_transition.busy).count();
      if (t < kMaxHoldSeconds && (t < kHoldSeconds || quiet < kQuietSeconds))
        return int(std::min(kMaxHoldSeconds - t, std::max(kHoldSeconds - t, kQuietSeconds - quiet)) * 1000) + 1;
      unwatch_redraws();
      if (!g_transition.fade) {
        finish_transition();
        return -1;
      }
      g_transition.stage = Transition::FADE;
      g_transition.since = Clock::now();
      return 0;
    }
    case Transition::FADE: {
      double k = std::min(1.0, t / kFadeSeconds);
      double ease = k * k * (3 - 2 * k);  // smoothstep: soft start and end
      if (k >= 1.0) {
        finish_transition();
        return -1;
      }
      set_cardinal(g_transition.win, "_NET_WM_WINDOW_OPACITY", long((1.0 - ease) * 0xFFFFFFFEUL));
      XFlush(g_display);
      return 16;  // about one frame
    }
    default:
      return -1;
  }
}

// The window that owns _SDE_SESSION_S<screen>: the address of theme switch
// requests (request_appearance_apply()).
void claim_session_selection() {
#ifdef SDE_HAVE_XDAMAGE
  int damage_error = 0;
  if (!XDamageQueryExtension(g_display, &g_damage_event, &damage_error)) g_damage_event = -1;
#endif
  Window root = DefaultRootWindow(g_display);
  XSetWindowAttributes a{};
  a.override_redirect = True;
  g_session_win = XCreateWindow(g_display, root, -1, -1, 1, 1, 0, CopyFromParent, InputOnly, CopyFromParent,
                                CWOverrideRedirect, &a);
  XStoreName(g_display, g_session_win, "sde-session");
  set_cardinal(g_session_win, "_NET_WM_PID", long(getpid()));
  XSetSelectionOwner(g_display, atom(("_SDE_SESSION_S" + std::to_string(DefaultScreen(g_display))).c_str()),
                     g_session_win, CurrentTime);
  XFlush(g_display);
}

// Some OpenGL drivers (virtual machines, old GPUs) let the compositor run but
// draw nothing: the whole screen stays black (alpha11 bug). When that
// happens the compositor is replaced by its XRender configuration, and if
// that is black too, compositing stays off for this session.
void check_compositor(Config &cfg) {
  g_compositor_check = 0;
  if (!component_running("SDE compositor") || !component_running(_("desktop"))) return;
  if (!screen_is_black()) return;
  if (++g_black_samples < 2) {  // look again before acting
    g_compositor_check = time(nullptr) + 2;
    return;
  }
  for (auto it = g_components.begin(); it != g_components.end(); ++it) {
    if (it->name != "SDE compositor") continue;
    std::vector<std::string> fallback = it->fallback;
    if (it->pid > 0) kill(it->pid, SIGTERM);
    g_components.erase(it);
    if (!fallback.empty()) {
      log("the compositor draws a black screen; switching to XRender");
      // Remember it, so the next login does not start with a black screen.
      Config saved;
      saved.load();
      saved.set("compositor", "backend", "xrender");
      saved.save();
      cfg.load();
      Component c = make_component("SDE compositor", fallback, false);
      c.max_restarts = 1;
      g_components.push_back(c);
      start_component(g_components.back());
      g_compositor_check = time(nullptr) + 6;
      g_black_samples = 0;
    } else {
      log("the compositor draws a black screen; compositing is off for this session "
          "(Alt+Shift+F12 turns it on again)");
      start_child({"notify-send", "-i", "dialog-warning", _("Compositor turned off"),
                   _("It drew a black screen. Transparency and shadows are off for this session; "
                     "Alt+Shift+F12 or Settings > Effects turns the compositor on again.")});
    }
    break;
  }
}

// SIGUSR2 (Settings > Effects > Restart compositor): stop the compositor and
// start it again with the current settings, or leave it off.
void restart_compositor(Config &cfg) {
  cfg.load();
  for (auto it = g_components.begin(); it != g_components.end(); ++it) {
    if (it->name != "SDE compositor") continue;
    if (it->pid > 0) kill(it->pid, SIGTERM);
    g_components.erase(it);  // its exit is no longer reported as a crash
    break;
  }
  log("restarting the SDE compositor");
  add_compositor(cfg);
}

// A dialog that replaces the black screen when the desktop cannot start.
void show_rescue(const std::string &problem) {
  g_problems.push_back(problem);
  if (g_rescue_pid > 0) return;
  std::string text = std::string(_("Selviana Desktop Environment could not start correctly.")) +
                     "\n\n" + problem + "\n\n" +
                     format(_("Details are in %s."), log_path().c_str()) + "\n" +
                     _("Run \"sde-session --check\" in a terminal to find missing packages,\n"
                       "then install them with: sudo apt install --fix-broken");
  std::string buttons = std::string(_("Open Terminal")) + ":10," + _("Log Out") + ":11," +
                        _("Continue") + ":12";
  log("showing the rescue dialog: %s", problem.c_str());
  if (!find_program("xmessage").empty()) {
    g_rescue_pid = start_child({"xmessage", "-center", "-title", "SDE", "-buttons", buttons,
                                "-default", _("Open Terminal"), text});
  } else {
    open_terminal();
  }
}

void stop_all() {
  for (auto &c : g_components)
    if (c.pid > 0) kill(c.pid, SIGTERM);
  if (g_rescue_pid > 0) kill(g_rescue_pid, SIGTERM);
  for (int i = 0; i < 30; ++i) {
    bool alive = false;
    for (auto &c : g_components) {
      if (c.pid <= 0) continue;
      if (waitpid(c.pid, nullptr, WNOHANG) == c.pid) c.pid = 0;
      else alive = true;
    }
    if (!alive) return;
    usleep(100 * 1000);
  }
  for (auto &c : g_components)
    if (c.pid > 0) kill(c.pid, SIGKILL);
}

// Flatpak and Snap export their launchers and icons outside /usr/share. startsde
// adds the directories to XDG_DATA_DIRS (see the comment there); this covers
// sessions started without it. Flatpak's order: the user's installation and
// the system one first; snap launchers last.
void add_app_data_dirs() {
  std::vector<std::string> dirs = split(env("XDG_DATA_DIRS", "/usr/local/share:/usr/share"), ':');
  auto has = [&](const std::string &d) {
    return std::find(dirs.begin(), dirs.end(), d) != dirs.end() ||
           std::find(dirs.begin(), dirs.end(), d + "/") != dirs.end();
  };
  std::vector<std::string> front;
  for (const std::string &d : {data_home() + "/flatpak/exports/share", std::string("/var/lib/flatpak/exports/share")})
    if (!has(d)) front.push_back(d);
  dirs.insert(dirs.begin(), front.begin(), front.end());
  if (!has("/var/lib/snapd/desktop")) dirs.push_back("/var/lib/snapd/desktop");
  std::string value;
  for (auto &d : dirs) value += (value.empty() ? "" : ":") + d;
  setenv("XDG_DATA_DIRS", value.c_str(), 1);
  std::string path = env("PATH", "/usr/local/bin:/usr/bin:/bin");
  if ((":" + path + ":").find(":/snap/bin:") == std::string::npos) setenv("PATH", (path + ":/snap/bin").c_str(), 1);
}

void setup_environment() {
  add_app_data_dirs();
  // Applications and display managers know the session as SDE-Desktop; the
  // short name stays in the list so existing OnlyShowIn=SDE entries match.
  setenv("XDG_CURRENT_DESKTOP", "SDE-Desktop:SDE", 1);
  setenv("XDG_SESSION_DESKTOP", "SDE-Desktop", 1);
  setenv("DESKTOP_SESSION", "sde-desktop", 1);
  setenv("SDE_SESSION_VERSION", SDE_VERSION, 1);
  if (env("XDG_SESSION_TYPE").empty()) setenv("XDG_SESSION_TYPE", "x11", 1);
  // Qt applications should follow GTK settings when no platform theme is set.
  if (env("QT_QPA_PLATFORMTHEME").empty()) setenv("QT_QPA_PLATFORMTHEME", "gtk3", 0);
  make_dirs(config_home() + "/sde");
}

// Paints the root window in the wallpaper's base color, so that a failing
// desktop is visibly different from a hung X server.
void paint_root() {
  if (!g_display) return;
  Window root = DefaultRootWindow(g_display);
  XSetWindowBackground(g_display, root, 0x1C6F8A);
  XClearWindow(g_display, root);
  XFlush(g_display);
}

bool wait_for_wm(int seconds) {
  if (!g_display) return false;
  for (int i = 0; i < seconds * 10; ++i) {
    // Re-read the property each time; the WM sets it once it is ready.
    Atom check = XInternAtom(g_display, "_NET_SUPPORTING_WM_CHECK", False);
    Atom actual;
    int format;
    unsigned long n = 0, after = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(g_display, DefaultRootWindow(g_display), check, 0, 1, False,
                           AnyPropertyType, &actual, &format, &n, &after, &data) == Success &&
        data) {
      XFree(data);
      if (n == 1) return true;
    }
    usleep(100 * 1000);
  }
  return false;
}

// Runs a short helper and waits for it (at most |seconds|).
void run_and_wait(const std::vector<std::string> &argv, int seconds) {
  pid_t pid = start_child(argv);
  if (pid <= 0) return;
  for (int i = 0; i < seconds * 10; ++i) {
    if (waitpid(pid, nullptr, WNOHANG) == pid) return;
    usleep(100 * 1000);
  }
  log("%s is taking too long; continuing", argv[0].c_str());
}

// sde-gl-probe: installed next to the polkit agent, or next to this program
// in a build tree.
std::string find_gl_probe() {
  std::string installed = SDE_LIBEXECDIR "/sde/sde-gl-probe";
  if (is_executable(installed)) return installed;
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n > 0) {
    self[n] = 0;
    std::string dir(self);
    dir = dir.substr(0, dir.rfind('/'));
    if (is_executable(dir + "/sde-gl-probe")) return dir + "/sde-gl-probe";
  }
  return find_program("sde-gl-probe");
}

// 0.2.0 Alpha 3.1: which OpenGL renderer the display has, checked once per
// login. On a graphics device whose OpenGL renders in software (its driver
// missing or broken) the compositor's blur and glass shader would run on the
// CPU: the automatic profile is then lite (XRender). The result reaches every
// program of the session (SDE_OPENGL; $XDG_RUNTIME_DIR/sde-opengl for the
// others), so that their themes agree with the compositor. The helper runs
// in its own process with a time limit: a broken OpenGL stack costs a few
// seconds at most, never the session.
void probe_opengl(const Config &cfg) {
  std::string gfx = detect_graphics_class();
  if (gfx != "hardware") {  // XRender and lite anyway
    publish_opengl_info({});
    return;
  }
  OpenGLInfo gl;
  std::string helper = find_gl_probe();
  if (helper.empty()) {
    log("OpenGL not checked: sde-gl-probe was not found");
  } else {
    int status = -1;
    std::string out = run_sync({helper}, &status, 4000);
    if (status == 0)
      gl = parse_opengl_probe(out);
    else
      log("OpenGL not checked: sde-gl-probe failed or took too long (status %d)", status);
  }
  publish_opengl_info(gl);
  AutoProfile p = auto_profile(gfx, gl);
  std::string backend = lower(cfg.get("compositor", "backend", "auto"));
  std::string effects = lower(cfg.get("compositor", "effects", "auto"));
  bool chosen = backend == "glx" || backend == "xrender" || effects == "full" || effects == "lite";
  log("compositor: %s, so the automatic profile is %s%s", p.why.c_str(),
      p.effects == "lite" ? "lite (XRender, no blur)" : "full (OpenGL)",
      chosen ? "; the [compositor] settings chosen by hand are kept" : "");
}

std::string find_polkit_agent() {
  static const char *agents[] = {
      SDE_LIBEXECDIR "/sde/sde-polkit-agent",  // SDE's own agent
      "/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1",
      "/usr/libexec/policykit-1-gnome/polkit-gnome-authentication-agent-1",
      "/usr/libexec/polkit-gnome-authentication-agent-1",
      "/usr/lib/x86_64-linux-gnu/libexec/polkit-mate-authentication-agent-1",
      "/usr/libexec/polkit-mate-authentication-agent-1",
      "/usr/bin/lxpolkit",
      "/usr/bin/lxqt-policykit-agent",
      "/usr/lib/x86_64-linux-gnu/libexec/polkit-kde-authentication-agent-1",
  };
  for (const char *a : agents)
    if (is_executable(a)) return a;
  return "";
}

void run_autostart() {
  // Spawn once. Never restart this helper: retrying could start applications
  // twice. Its children are adopted/reaped by this session's subreaper.
  if (start_child({"sde-autostart"}) <= 0)
    log("cannot start sde-autostart; run sde-session --check");
}

int request_logout() {
  std::string text;
  if (!read_file(pid_file(), text)) {
    fprintf(stderr, "sde-session: %s\n",
            format(_("no running session found for DISPLAY=%s"), env("DISPLAY").c_str()).c_str());
    return 1;
  }
  pid_t pid = pid_t(atol(text.c_str()));
  if (pid <= 1 || kill(pid, SIGTERM) != 0) {
    fprintf(stderr, "sde-session: cannot signal session process %d\n", int(pid));
    return 1;
  }
  return 0;
}

// --compositor off|on|toggle|restart (Alt+Shift+F12 toggles): changes
// [compositor] mode and asks the running session to apply it. A way out
// when a compositor draws a black screen.
int set_compositor(const std::string &how) {
  if (how != "off" && how != "on" && how != "toggle" && how != "restart") {
    fprintf(stderr, "sde-session: --compositor takes off, on, toggle or restart\n");
    return 2;
  }
  Config cfg;
  cfg.load();
  bool off = cfg.get("compositor", "mode", "auto") == "off";
  if (how != "restart") {
    off = how == "off" || (how == "toggle" && !off);
    cfg.set("compositor", "mode", off ? "off" : "auto");
    if (!cfg.save()) {
      fprintf(stderr, "sde-session: cannot save %s\n", (config_home() + "/sde/sde.conf").c_str());
      return 1;
    }
  }
  printf("SDE compositor: %s\n", off ? "off" : "on");
  std::string text;
  if (!read_file(pid_file(), text)) return 0;  // applied at the next login
  pid_t pid = pid_t(atol(text.c_str()));
  if (pid > 1) kill(pid, SIGUSR2);
  return 0;
}

// ------------------------------------------------------------- --check

// Runs |argv| with output captured; returns the exit status (-1 if missing).
int probe(const std::vector<std::string> &argv, std::string &out) {
  out.clear();
  if (find_program(argv[0]).empty()) return -1;
  int fds[2];
  if (pipe(fds) != 0) return -1;
  std::vector<char *> args;
  for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    // Do not let a GUI program open windows during the check.
    unsetenv("DISPLAY");
    execvp(args[0], args.data());
    _exit(127);
  }
  close(fds[1]);
  char buf[512];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, size_t(n));
  close(fds[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

int run_check() {
  int problems = 0;
  auto report = [&](bool ok, const std::string &what, const std::string &detail) {
    printf("  [%s] %-28s %s\n", ok ? " OK " : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++problems;
  };
  auto first_line = [](std::string s) {
    s = trim(s);
    size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
  };
  printf("%s\n", format(_("SDE %s system check"), SDE_VERSION).c_str());
  printf("\n%s\n", _("Programs:"));
  Config cfg;
  cfg.load();
  std::string wm = trim(cfg.get("session", "window_manager", "sde-wm"));
  std::vector<std::vector<std::string>> programs = {
      {"sde-panel", "--version"},     {"sde-desktop", "--version"}, {"sde-notifyd", "--version"},
      {"sde-settings", "--version"},  {"sde-run", "--version"},     {"sde-logout", "--version"},
      {"sde-compositor", "--version"}, {"sde-autostart", "--version"}, {"sde-suite", "--check"},     {"sde-update", "--version"},
      {"sde-power", "--version"},     {"sde-lock", "--version"},
  };
  if (wm.empty() || wm == "sde-wm" || wm == "openbox") programs.insert(programs.begin(), {"sde-wm", "--version"});
  else programs.insert(programs.begin(), {split_command(wm)[0], "--version"});
  for (auto &p : programs) {
    std::string out;
    int st = probe(p, out);
    if (st < 0) report(false, p[0], _("not installed"));
    else if (st != 0) report(false, p[0], first_line(out));
    else report(true, p[0], first_line(out));
  }
  printf("\n%s\n", _("Helpers (optional):"));
  for (const char *h : {"xmessage", "xrandr", "setxkbmap", "xset", "xinput", "pactl", "nmcli",
                        "xdg-open", "x-terminal-emulator"}) {
    bool ok = !find_program(h).empty();
    printf("  [%s] %s\n", ok ? " OK " : "----", h);
  }
  printf("\n%s\n", _("Session:"));
  report(!env("DISPLAY").empty(), "DISPLAY", env("DISPLAY", _("not set")));
  report(!env("DBUS_SESSION_BUS_ADDRESS").empty() || !find_program("dbus-run-session").empty(),
         "D-Bus session bus",
         env("DBUS_SESSION_BUS_ADDRESS").empty() ? _("will be started by startsde")
                                                 : _("available"));
  std::string rc = std::string(SDE_DATADIR) + "/openbox/rc.xml";
  report(file_exists(rc), "SDE data files", rc);
  bool icons = false;
  for (auto &d : list_dir("/usr/share/icons"))
    if (d != "hicolor" && d != "default" && file_exists("/usr/share/icons/" + d + "/index.theme"))
      icons = true;
  report(icons, _("Icon theme"), icons ? _("installed") : _("none (install adwaita-icon-theme)"));
  printf("\n%s\n", format(_("Session log: %s"), log_path().c_str()).c_str());
  if (problems)
    printf("\n%s\n", _("Problems were found. Missing libraries or programs are usually fixed with:\n"
                       "  sudo apt install --fix-broken\n  sudo apt install --reinstall sde"));
  else
    printf("\n%s\n", _("No problems found."));
  return problems ? 1 : 0;
}

int io_error_handler(Display *) {
  // The X server went away: the session is over.
  stop_all();
  unlink(pid_file().c_str());
  _exit(0);
}

// --compositor-config: writes the configuration the session starts the SDE
// compositor with (generated from sde.conf, unless ~/.config/sde/compositor.conf
// is the user's own) and prints its path. For tools such as tools/showcase.py.
int print_compositor_config() {
  Config cfg;
  cfg.load();
  std::string own = config_home() + "/sde/compositor.conf";
  printf("%s\n", file_exists(own) ? own.c_str() : write_compositor_config(cfg).c_str());
  return 0;
}

// --apply-appearance [--wait]: applies the appearance saved in sde.conf (a
// script that edited it, the UI tests): through the session of this display,
// in one calm step, or at once when no session runs. --wait returns once the
// session shows the new look.
int apply_appearance_cli(bool wait) {
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    fprintf(stderr, "sde-session: %s\n", format(_("cannot open display %s"), env("DISPLAY").c_str()).c_str());
    return 1;
  }
  Config cfg;
  cfg.load();
  long request = request_appearance_apply(dpy);
  if (!request) {
    apply_appearance_now(dpy, cfg);
  } else if (wait) {
    Atom done = XInternAtom(dpy, "_SDE_APPEARANCE_DONE", False);
    Window session =
        XGetSelectionOwner(dpy, XInternAtom(dpy, ("_SDE_SESSION_S" + std::to_string(DefaultScreen(dpy))).c_str(), False));
    XSelectInput(dpy, session, PropertyChangeMask);
    auto answered = [&] {
      Atom type;
      int fmt = 0;
      unsigned long n = 0, after = 0;
      unsigned char *data = nullptr;
      bool yes = false;
      if (XGetWindowProperty(dpy, session, done, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) ==
              Success && data) {
        yes = n == 1 && reinterpret_cast<long *>(data)[0] == request;
        XFree(data);
      }
      return yes;
    };
    for (int i = 0; i < 100 && !answered(); ++i) {
      struct pollfd fd = {ConnectionNumber(dpy), POLLIN, 0};
      poll(&fd, 1, 100);
      while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
      }
    }
  }
  XCloseDisplay(dpy);
  return 0;
}

void usage() {
  printf("%s", _("Usage: sde-session [--check | --logout | --compositor MODE | --compositor-config |\n"
                 "                   --apply-appearance [--wait] | --version | --help]\n\n"
                 "Starts the Selviana Desktop Environment. Normally run by startsde from\n"
                 "the display manager.\n\n"
                 "  --check    check that every SDE component can start, and report problems\n"
                 "  --logout   end the running SDE session on this display\n"
                 "  --compositor off|on|toggle|restart\n"
                 "             turn the compositor off or on (Alt+Shift+F12 toggles it)\n"
                 "  --compositor-config\n"
                 "             write the compositor configuration from the settings and print\n"
                 "             its path\n"
                 "  --apply-appearance [--wait]\n"
                 "             apply the theme saved in sde.conf in one calm step (--wait: return\n"
                 "             once it is shown)\n"
                 "  --version  print the version and exit\n"));
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--logout") return request_logout();
    if (a == "--check") return run_check();
    if (a == "--compositor") return set_compositor(i + 1 < argc ? argv[i + 1] : "");
    if (a == "--compositor-config") return print_compositor_config();
    if (a == "--apply-appearance")
      return apply_appearance_cli(i + 1 < argc && std::string(argv[i + 1]) == "--wait");
    if (a == "--version") return print_version("sde-session", SDE_VERSION);
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    }
    fprintf(stderr, "sde-session: %s\n", format(_("unknown option %s"), a.c_str()).c_str());
    return 2;
  }
  if (env("DISPLAY").empty()) {
    fprintf(stderr, "sde-session: %s\n",
            _("DISPLAY is not set; start SDE from a display manager or startx"));
    return 1;
  }
  g_display = XOpenDisplay(nullptr);
  if (!g_display) {
    fprintf(stderr, "sde-session: %s\n",
            format(_("cannot open display %s"), env("DISPLAY").c_str()).c_str());
    return 1;
  }
  XSetIOErrorHandler(io_error_handler);

  std::string existing;
  if (read_file(pid_file(), existing)) {
    pid_t other = pid_t(atol(existing.c_str()));
    if (other > 1 && kill(other, 0) == 0) {
      fprintf(stderr, "sde-session: %s\n",
              format(_("a session is already running on %s"), env("DISPLAY").c_str()).c_str());
      return 1;
    }
  }
  open_log();
  write_file_atomic(pid_file(), std::to_string(getpid()) + "\n");
  if (getuid() == 0) log("warning: running a desktop session as root is not supported");

  // Adopt orphaned grandchildren so that they are reaped here.
  prctl(PR_SET_CHILD_SUBREAPER, 1);

  sigset_t mask;
  sigemptyset(&mask);
  for (int s : {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGUSR2}) sigaddset(&mask, s);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  int sfd = signalfd(-1, &mask, SFD_CLOEXEC);

  setup_environment();
  paint_root();
  Config cfg;
  cfg.load();
  // Theme values of alpha15 and earlier (sde, aero, liquid-glass and the old
  // accent swatches) are rewritten once to SDE Graphite/Fluent/Glass.
  if (migrate_theme_config(cfg)) {
    cfg.save();
    log("theme settings migrated to %s", theme_dir(cfg).c_str());
  }
  // Before anything computes the theme or the compositor's configuration.
  probe_opengl(cfg);
  sync_openbox(cfg);
  sync_gtk_settings(cfg);
  migrate_legacy_suite_shortcuts();
  XSelectInput(g_display, DefaultRootWindow(g_display), PropertyChangeMask);
  log("starting Selviana Desktop Environment %s (DISPLAY=%s, D-Bus %s)", SDE_VERSION,
      env("DISPLAY").c_str(), env("DBUS_SESSION_BUS_ADDRESS").empty() ? "missing" : "ok");

  // 0.2.0 Alpha 4: the SDE core controller (docs/CORE.md) first, so that
  // the programs started next find it. It is restarted when it crashes; the
  // programs fall back to sde.conf meanwhile.
  core_link::open();
  core_link::on_core_started([] {
    for (auto &c : g_components)
      if (c.pid > 0) core_link::report("component-started", c.pid, c.name, "", "");
  });
  if (!find_program("sde-core").empty()) {
    g_components.push_back(make_component(_("core controller"), {"sde-core"}, false));
    start_component(g_components.back());
  }

  // Monitors, keyboard and pointer settings saved in sde-settings.
  run_and_wait({"sde-settings", "--apply"}, 5);

  Component wm = make_component(_("window manager"),
                                window_manager_command(cfg.get("session", "window_manager")),
                                true, true);
  g_components.push_back(wm);
  start_component(g_components.back());
  if (g_components.back().pid > 0 && !wait_for_wm(5))
    log("window manager did not announce itself; continuing");

  g_components.push_back(make_component(_("desktop"), {"sde-desktop"}, false));
  {
    // A panel that keeps crashing (a bad tray icon, device or D-Bus
    // service) comes back in safe mode instead of leaving no panel.
    Component panel = make_component(_("panel"), {"sde-panel"}, true, true);
    panel.fallback = {"sde-panel", "--safe"};
    panel.fallback_after = 3;
    g_components.push_back(panel);
  }
  g_components.push_back(make_component(_("notification service"), {"sde-notifyd"}, false));
  // 0.2.0 Alpha 3.1: notices programs that stop responding or never open.
  if (cfg.get_bool("force_close", "enabled", true) && !find_program("sde-watchdog").empty())
    g_components.push_back(make_component(_("application watchdog"), {"sde-watchdog"}, false));
  for (auto &c : g_components)  // the window manager and the core run already
    if (c.pid == 0 && !c.given_up) start_component(c);

  std::string agent = cfg.get("session", "polkit_agent", "auto");
  if (agent == "auto") agent = find_polkit_agent();
  if (!agent.empty() && agent != "none") {
    Component c = make_component(_("polkit agent"), split_command(agent), false);
    c.max_restarts = 2;  // usually fails because another agent is running
    g_components.push_back(c);
    start_component(g_components.back());
  }
  add_compositor(cfg);
  std::string selection = "_XSETTINGS_S" + std::to_string(DefaultScreen(g_display));
  if (XGetSelectionOwner(g_display, XInternAtom(g_display, selection.c_str(), False)) == None) {
    // SDE's own build of xsettingsd; the distribution's one is a fallback.
    std::string daemon = find_program("sde-xsettingsd").empty() ? "xsettingsd" : "sde-xsettingsd";
    g_components.push_back(make_component("GTK settings", {daemon, "--config", config_home() + "/sde/xsettingsd.conf"}, false));
    start_component(g_components.back());
  }
  run_autostart();
  core_link::phase("running");
  // What the programs started with: later only changes are delivered.
  remember_delivered(cfg);
  claim_session_selection();

  bool running = true;
  while (running) {
    struct pollfd fds[3] = {{sfd, POLLIN, 0}, {ConnectionNumber(g_display), POLLIN, 0}, {core_link::fd(), POLLIN, 0}};
    nfds_t nfds = fds[2].fd >= 0 ? 3 : 2;
    // Sleep until a signal, an X event or the next scheduled restart; an idle
    // session manager must not wake up every second.
    int timeout = step_transition(cfg);  // a theme switch in progress
    time_t now0 = time(nullptr);
    for (auto &c : g_components)
      if (c.pid == 0 && c.restart_at && !c.given_up) {
        int ms = c.restart_at > now0 ? int(c.restart_at - now0) * 1000 : 0;
        timeout = timeout < 0 ? ms : std::min(timeout, ms);
      }
    // [appearance] theme=auto switches between light and dark at 07:00 and
    // 19:00: wake up then and announce the change like Settings does.
    bool auto_mode = lower(cfg.get("appearance", "theme", "light")) == "auto";
    if (auto_mode) {
      int ms = theme_auto_seconds_left() * 1000 + 500;
      timeout = timeout < 0 ? ms : std::min(timeout, ms);
    }
    int r = poll(fds, nfds, timeout);
    if (auto_mode && theme_mode_dark("auto") != g_auto_dark) {
      g_auto_dark = theme_mode_dark("auto");
      start_transition(cfg, 0);  // a calm switch, like Apply in the Control Center
    }
    if (r < 0 && errno != EINTR) break;
    if (nfds == 3 && fds[2].revents) core_link::dispatch();
    if (fds[1].revents) {
      while (XPending(g_display)) {
        XEvent ev;
        XNextEvent(g_display, &ev);
        if (ev.type == PropertyNotify && ev.xproperty.atom == XInternAtom(g_display, "_SDE_CONFIG_SERIAL", False)) {
          // GTK, the window frames and the compositor follow the theme too,
          // no matter which program changed it (Settings, the panel, a
          // script); each is told once and only when its input changed (the
          // session's own announcement finds nothing new).
          cfg.load();
          write_appearance_files(cfg);
          deliver(cfg);
        } else if (handle_redraw(ev)) {
          // a window redrew while a theme switch is held (watch_redraws())
        } else if (ev.type == ClientMessage && ev.xclient.window == g_session_win &&
                   ev.xclient.message_type == XInternAtom(g_display, "_SDE_APPEARANCE_APPLY", False)) {
          start_transition(cfg, ev.xclient.data.l[0]);
        } else if (ev.type == SelectionClear && ev.xselectionclear.window == g_session_win) {
          log("another session took over the theme switches of this display");
        }
      }
    }
    if (fds[0].revents & POLLIN) {
      struct signalfd_siginfo info;
      while (read(sfd, &info, sizeof info) == sizeof info) {
        if (info.ssi_signo == SIGCHLD) {
          int status;
          pid_t pid;
          while ((pid = waitpid(-1, &status, WNOHANG)) > 0) on_child_exit(pid, status);
        } else if (info.ssi_signo == SIGHUP) {
          cfg.load();
        } else if (info.ssi_signo == SIGUSR2) {
          restart_compositor(cfg);
          g_sent_compositor = compositor_inputs(cfg);  // started with them
        } else {
          log("ending session");
          running = false;
        }
        break;
      }
    }
    time_t now = time(nullptr);
    if (g_compositor_check && now >= g_compositor_check) check_compositor(cfg);
    for (auto &c : g_components) {
      if (c.pid == 0 && c.restart_at && now >= c.restart_at && !c.given_up) {
        c.restart_at = 0;
        log("restarting %s", c.name.c_str());
        start_component(c);
      }
    }
  }
  core_link::phase("ending");
  stop_all();
  unlink(pid_file().c_str());
  XCloseDisplay(g_display);
  return 0;
}
