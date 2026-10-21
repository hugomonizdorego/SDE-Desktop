// SPDX-License-Identifier: GPL-3.0-or-later
// sde-session: starts and supervises the Selviana Desktop Environment.
//
// Responsibilities:
//   * export the session environment (XDG_CURRENT_DESKTOP=SDE-Desktop:SDE, ...)
//   * apply saved display and input settings (sde-settings --apply)
//   * start the window manager, sde-desktop, sde-panel and sde-notifyd,
//     restarting them when they crash (with a limit, so a broken component
//     cannot loop)
//   * start a polkit authentication agent and XDG autostart applications
//   * log everything to ~/.local/state/sde/session.log and show a rescue
//     dialog instead of a black screen when the desktop cannot start
//   * end the session cleanly on "sde-session --logout" or SIGTERM
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "spawn.h"
#include "util.h"
#include "wm.h"
#include "integration.h"
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
    if (!crashed && !c.essential) return;
    char why[128];
    log("%s exited (%s)", c.name.c_str(), exit_reason(status, why, sizeof why));
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
      if (c.critical)
        show_rescue(format(_("The %s keeps stopping (%s)."), c.name.c_str(), why));
      return;
    }
    c.restart_at = now + 1;
    return;
  }
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

void setup_environment() {
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

std::string find_polkit_agent() {
  static const char *agents[] = {
      SDE_LIBEXECDIR "/sde/sde-polkit-agent",  // SDE's own (mate-polkit)
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

void usage() {
  printf("%s", _("Usage: sde-session [--check | --logout | --compositor MODE | --version | --help]\n\n"
                 "Starts the Selviana Desktop Environment. Normally run by startsde from\n"
                 "the display manager.\n\n"
                 "  --check    check that every SDE component can start, and report problems\n"
                 "  --logout   end the running SDE session on this display\n"
                 "  --compositor off|on|toggle|restart\n"
                 "             turn the compositor off or on (Alt+Shift+F12 toggles it)\n"
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
    if (a == "--version") {
      printf("sde-session %s (%s)\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    }
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
  sync_openbox(cfg);
  sync_gtk_settings(cfg);
  migrate_legacy_suite_shortcuts();
  XSelectInput(g_display, DefaultRootWindow(g_display), PropertyChangeMask);
  log("starting Selviana Desktop Environment %s (DISPLAY=%s, D-Bus %s)", SDE_VERSION,
      env("DISPLAY").c_str(), env("DBUS_SESSION_BUS_ADDRESS").empty() ? "missing" : "ok");

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
  for (size_t i = 1; i < g_components.size(); ++i) start_component(g_components[i]);

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

  bool running = true;
  while (running) {
    struct pollfd fds[2] = {{sfd, POLLIN, 0}, {ConnectionNumber(g_display), POLLIN, 0}};
    // Sleep until a signal, an X event or the next scheduled restart; an idle
    // session manager must not wake up every second.
    int timeout = -1;
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
    int r = poll(fds, 2, timeout);
    if (auto_mode && theme_mode_dark("auto") != g_auto_dark) {
      g_auto_dark = theme_mode_dark("auto");
      bump_config_serial();  // handled below like any other change
    }
    if (r < 0 && errno != EINTR) break;
    if (fds[1].revents) {
      while (XPending(g_display)) {
        XEvent ev;
        XNextEvent(g_display, &ev);
        if (ev.type == PropertyNotify && ev.xproperty.atom == XInternAtom(g_display, "_SDE_CONFIG_SERIAL", False)) {
          cfg.load(); sync_gtk_settings(cfg);
          // The window frames and the compositor follow the theme too, no
          // matter which program changed it (Settings, the panel, a script).
          if (sync_openbox(cfg)) reconfigure_window_manager();
          if (!file_exists(config_home() + "/sde/compositor.conf")) {
            std::string before, after, path = config_home() + "/sde/aero-compositor.conf";
            read_file(path, before);
            write_compositor_config(cfg);
            write_compositor_config(cfg, true);
            if (read_file(path, after) && after != before) reload_compositor();
          }
          for (auto &c : g_components)
            if (c.name == "GTK settings" && c.pid > 0) kill(c.pid, SIGHUP);
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
  stop_all();
  unlink(pid_file().c_str());
  XCloseDisplay(g_display);
  return 0;
}
