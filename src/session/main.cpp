// SPDX-License-Identifier: GPL-3.0-or-later
// sde-session: starts and supervises the Selviana Desktop Environment.
//
// Responsibilities (0.1 Alpha):
//   * export the session environment (XDG_CURRENT_DESKTOP=SDE, ...)
//   * start the window manager, sde-desktop and sde-panel, restarting them
//     when they crash (with a limit, so a broken component cannot loop)
//   * start a polkit authentication agent and XDG autostart applications
//   * end the session cleanly on "sde-session --logout" or SIGTERM
#include <X11/Xlib.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

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
#include "spawn.h"
#include "util.h"
#include "wm.h"

using namespace sde;

namespace {

struct Component {
  std::string name;
  std::vector<std::string> argv;
  bool essential = false;     // restart when it exits, even with status 0
  pid_t pid = 0;
  std::deque<time_t> starts;  // recent start times, for crash-loop detection
  time_t restart_at = 0;
  bool given_up = false;
};

std::vector<Component> g_components;

Component make_component(const std::string &name, const std::vector<std::string> &argv,
                         bool essential) {
  Component c;
  c.name = name;
  c.argv = argv;
  c.essential = essential;
  return c;
}
Display *g_display = nullptr;

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "sde-session: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
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

void start_component(Component &c) {
  c.pid = start_child(c.argv);
  c.starts.push_back(time(nullptr));
  while (c.starts.size() > 5) c.starts.pop_front();
  if (c.pid <= 0) log("cannot start %s (%s not found)", c.name.c_str(), c.argv[0].c_str());
}

void on_child_exit(pid_t pid, int status) {
  for (auto &c : g_components) {
    if (c.pid != pid) continue;
    c.pid = 0;
    bool crashed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    if (!crashed && !c.essential) return;
    log("%s exited (%s %d)", c.name.c_str(), WIFSIGNALED(status) ? "signal" : "status",
        WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    time_t now = time(nullptr);
    if (c.starts.size() >= 5 && now - c.starts.front() < 60) {
      log("%s keeps failing; not restarting it again", c.name.c_str());
      c.given_up = true;
      return;
    }
    c.restart_at = now + 1;
    return;
  }
}

void stop_all() {
  for (auto &c : g_components)
    if (c.pid > 0) kill(c.pid, SIGTERM);
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
  setenv("XDG_CURRENT_DESKTOP", "SDE", 1);
  setenv("XDG_SESSION_DESKTOP", "SDE", 1);
  setenv("DESKTOP_SESSION", "sde", 1);
  setenv("SDE_SESSION_VERSION", SDE_VERSION, 1);
  if (env("XDG_SESSION_TYPE").empty()) setenv("XDG_SESSION_TYPE", "x11", 1);
  // Qt applications should follow GTK settings when no platform theme is set.
  if (env("QT_QPA_PLATFORMTHEME").empty()) setenv("QT_QPA_PLATFORMTHEME", "gtk3", 0);
  make_dirs(config_home() + "/sde");
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

std::string find_polkit_agent() {
  static const char *agents[] = {
      "/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1",
      "/usr/libexec/policykit-1-gnome/polkit-gnome-authentication-agent-1",
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
  // User entries override system entries with the same file name.
  std::map<std::string, std::string> entries;
  std::vector<std::string> dirs{config_home()};
  for (auto &d : config_dirs()) dirs.push_back(d);
  for (auto &d : dirs)
    for (auto &f : list_dir(d + "/autostart"))
      if (ends_with(f, ".desktop") && !entries.count(f)) entries[f] = d + "/autostart/" + f;
  for (auto &kv : entries) {
    DesktopEntry e;
    if (!e.load(kv.second, kv.first) || e.type != "Application" || e.exec.empty()) continue;
    if (!e.visible_in("SDE")) continue;
    // Polkit agents are handled separately so that only one ever runs.
    if (lower(e.exec).find("polkit") != std::string::npos) continue;
    if (launch_entry(e)) log("autostart: %s", kv.first.c_str());
  }
}

int request_logout() {
  std::string text;
  if (!read_file(pid_file(), text)) {
    fprintf(stderr, "sde-session: no running session found for DISPLAY=%s\n", env("DISPLAY").c_str());
    return 1;
  }
  pid_t pid = pid_t(atol(text.c_str()));
  if (pid <= 1 || kill(pid, SIGTERM) != 0) {
    fprintf(stderr, "sde-session: cannot signal session process %d\n", int(pid));
    return 1;
  }
  return 0;
}

int io_error_handler(Display *) {
  // The X server went away: the session is over.
  stop_all();
  unlink(pid_file().c_str());
  _exit(0);
}

void usage() {
  printf("Usage: sde-session [--logout | --version | --help]\n\n"
         "Starts the Selviana Desktop Environment. Normally run by startsde from\n"
         "the display manager.\n\n"
         "  --logout   end the running SDE session on this display\n"
         "  --version  print the version and exit\n");
}

}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--logout") return request_logout();
    if (a == "--version") {
      printf("sde-session %s (%s)\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    }
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    }
    fprintf(stderr, "sde-session: unknown option %s\n", a.c_str());
    return 2;
  }
  if (getuid() == 0) log("warning: running a desktop session as root is not supported");
  if (env("DISPLAY").empty()) {
    fprintf(stderr, "sde-session: DISPLAY is not set; start SDE from a display manager or startx\n");
    return 1;
  }
  g_display = XOpenDisplay(nullptr);
  if (!g_display) {
    fprintf(stderr, "sde-session: cannot open display %s\n", env("DISPLAY").c_str());
    return 1;
  }
  XSetIOErrorHandler(io_error_handler);

  std::string existing;
  if (read_file(pid_file(), existing)) {
    pid_t other = pid_t(atol(existing.c_str()));
    if (other > 1 && kill(other, 0) == 0) {
      fprintf(stderr, "sde-session: a session is already running on %s\n", env("DISPLAY").c_str());
      return 1;
    }
  }
  write_file_atomic(pid_file(), std::to_string(getpid()) + "\n");

  // Adopt orphaned grandchildren so that they are reaped here.
  prctl(PR_SET_CHILD_SUBREAPER, 1);

  sigset_t mask;
  sigemptyset(&mask);
  for (int s : {SIGCHLD, SIGTERM, SIGINT, SIGHUP}) sigaddset(&mask, s);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  int sfd = signalfd(-1, &mask, SFD_CLOEXEC);

  setup_environment();
  Config cfg;
  cfg.load();
  set_openbox_theme(lower(cfg.get("appearance", "theme")) == "dark");
  log("starting Selviana Desktop Environment %s", SDE_VERSION);

  Component wm;
  wm.name = "window manager";
  wm.argv = window_manager_command(cfg.get("session", "window_manager"));
  wm.essential = true;
  g_components.push_back(wm);
  start_component(g_components.back());
  if (!wait_for_wm(5)) log("window manager did not announce itself; continuing");

  g_components.push_back(make_component("desktop", {"sde-desktop"}, false));
  g_components.push_back(make_component("panel", {"sde-panel"}, true));
  for (size_t i = 1; i < g_components.size(); ++i) start_component(g_components[i]);

  std::string agent = cfg.get("session", "polkit_agent", "auto");
  if (agent == "auto") agent = find_polkit_agent();
  if (!agent.empty() && agent != "none") {
    g_components.push_back(make_component("polkit agent", split_command(agent), false));
    start_component(g_components.back());
  }
  run_autostart();

  bool running = true;
  while (running) {
    struct pollfd fds[2] = {{sfd, POLLIN, 0}, {ConnectionNumber(g_display), POLLIN, 0}};
    int r = poll(fds, 2, 1000);
    if (r < 0 && errno != EINTR) break;
    if (fds[1].revents) {
      while (XPending(g_display)) {
        XEvent ev;
        XNextEvent(g_display, &ev);
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
        } else {
          log("ending session");
          running = false;
        }
        break;
      }
    }
    time_t now = time(nullptr);
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
