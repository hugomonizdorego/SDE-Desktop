// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-xsettingsd: the XSETTINGS manager of SDE. GTK and other toolkits read
// the theme, icon theme, fonts and DPI from it. It owns _XSETTINGS_S<n> on
// every screen, publishes the settings from a text file and re-reads the file
// on SIGHUP, so open applications follow changes at once.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "build_config.h"
#include "xsettings.h"

namespace {

struct Screen {
  int number = 0;
  Atom selection = None;
  Window window = None;
};

Display *g_display = nullptr;
std::vector<Screen> g_screens;
Atom g_settings_atom = None;
std::string g_config;
sde::xsettings::Settings g_settings;
uint32_t g_serial = 0;

std::string default_config() {
  const char *home = getenv("XDG_CONFIG_HOME");
  std::string base = home && *home ? home : std::string(getenv("HOME") ? getenv("HOME") : "") + "/.config";
  return base + "/sde/xsettingsd.conf";
}

bool load(bool initial) {
  std::ifstream in(g_config);
  if (!in) {
    fprintf(stderr, "sde-xsettingsd: cannot read %s: %s\n", g_config.c_str(), strerror(errno));
    return false;
  }
  std::stringstream text;
  text << in.rdbuf();
  std::vector<std::string> errors;
  auto next = sde::xsettings::parse(text.str(), &errors);
  for (const auto &e : errors) fprintf(stderr, "sde-xsettingsd: %s: %s\n", g_config.c_str(), e.c_str());
  uint32_t serial = initial ? 0 : g_serial + 1;
  if (!sde::xsettings::merge(g_settings, next, serial) && !initial) return true;
  g_serial = serial;
  return true;
}

void publish() {
  auto data = sde::xsettings::encode(g_settings, g_serial);
  for (const auto &s : g_screens)
    XChangeProperty(g_display, s.window, g_settings_atom, g_settings_atom, 8, PropModeReplace,
                    data.data(), int(data.size()));
  XFlush(g_display);
}

int ignore_errors(Display *, XErrorEvent *) { return 0; }

// Takes the XSETTINGS selection of every screen, as the specification asks,
// and announces the new manager to clients with a MANAGER client message.
bool acquire(bool replace) {
  Atom manager = XInternAtom(g_display, "MANAGER", False);
  for (int n = 0; n < ScreenCount(g_display); ++n) {
    Screen s;
    s.number = n;
    s.selection = XInternAtom(g_display, ("_XSETTINGS_S" + std::to_string(n)).c_str(), False);
    if (!replace && XGetSelectionOwner(g_display, s.selection) != None) {
      fprintf(stderr, "sde-xsettingsd: another XSETTINGS manager runs on screen %d "
                      "(use --replace to take over)\n", n);
      return false;
    }
    Window root = RootWindow(g_display, n);
    s.window = XCreateSimpleWindow(g_display, root, -1, -1, 1, 1, 0, 0, 0);
    XSelectInput(g_display, s.window, PropertyChangeMask | StructureNotifyMask);
    // A server timestamp, obtained from a property change on our own window.
    XChangeProperty(g_display, s.window, XA_WM_NAME, XA_STRING, 8, PropModeAppend, nullptr, 0);
    XEvent ev;
    XWindowEvent(g_display, s.window, PropertyChangeMask, &ev);
    Time when = ev.xproperty.time;
    XSetSelectionOwner(g_display, s.selection, s.window, when);
    if (XGetSelectionOwner(g_display, s.selection) != s.window) {
      fprintf(stderr, "sde-xsettingsd: cannot own %s\n",
              ("_XSETTINGS_S" + std::to_string(n)).c_str());
      return false;
    }
    XClientMessageEvent m{};
    m.type = ClientMessage;
    m.window = root;
    m.message_type = manager;
    m.format = 32;
    m.data.l[0] = long(when);
    m.data.l[1] = long(s.selection);
    m.data.l[2] = long(s.window);
    XSendEvent(g_display, root, False, StructureNotifyMask, reinterpret_cast<XEvent *>(&m));
    g_screens.push_back(s);
  }
  return true;
}

void usage() {
  printf("Usage: sde-xsettingsd [--config FILE] [--replace] [--version]\n\n"
         "Serves XSETTINGS (GTK theme, icons, fonts, DPI) to X applications.\n"
         "The settings file has one \"Name value\" per line: an integer, a \"string\"\n"
         "or a color (red, green, blue, alpha). SIGHUP re-reads it.\n"
         "Default file: $XDG_CONFIG_HOME/sde/xsettingsd.conf\n");
}

}  // namespace

int main(int argc, char **argv) {
  bool replace = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--config" || a == "-c") && i + 1 < argc) g_config = argv[++i];
    else if (a == "--replace") replace = true;
    else if (a == "--version") {
      // The format of sde::version_line() (about.h), which needs sde-common.
      printf("sde-xsettingsd (Selviana Desktop Environment) %s\n"
             "Copyright \xC2\xA9 2026 Selviana DE Project and contributors\n"
             "License GPL-3.0-or-later: GNU GPL version 3 or later <https://www.gnu.org/licenses/gpl-3.0.html>.\n"
             "This is free software: you are free to change and redistribute it.\n"
             "There is NO WARRANTY, to the extent permitted by law.\n",
             SDE_VERSION);
      return 0;
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      fprintf(stderr, "sde-xsettingsd: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (g_config.empty()) g_config = default_config();

  sigset_t mask;
  sigemptyset(&mask);
  for (int s : {SIGHUP, SIGTERM, SIGINT}) sigaddset(&mask, s);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  int sfd = signalfd(-1, &mask, SFD_CLOEXEC);

  g_display = XOpenDisplay(nullptr);
  if (!g_display) {
    fprintf(stderr, "sde-xsettingsd: cannot open the X display\n");
    return 1;
  }
  XSetErrorHandler(ignore_errors);
  g_settings_atom = XInternAtom(g_display, "_XSETTINGS_SETTINGS", False);
  load(true);
  if (!acquire(replace)) return 1;
  publish();

  for (;;) {
    struct pollfd fds[2] = {{sfd, POLLIN, 0}, {ConnectionNumber(g_display), POLLIN, 0}};
    while (XPending(g_display)) {
      XEvent ev;
      XNextEvent(g_display, &ev);
      // Another manager took a selection over: leave quietly.
      if (ev.type == SelectionClear) {
        for (const auto &s : g_screens)
          if (ev.xselectionclear.window == s.window) {
            fprintf(stderr, "sde-xsettingsd: another XSETTINGS manager took over\n");
            return 0;
          }
      }
    }
    if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
    if (fds[0].revents & POLLIN) {
      struct signalfd_siginfo info;
      if (read(sfd, &info, sizeof info) != sizeof info) continue;
      if (info.ssi_signo == SIGHUP) {
        load(false);
        publish();
      } else {
        break;
      }
    }
  }
  for (const auto &s : g_screens) XDestroyWindow(g_display, s.window);
  XCloseDisplay(g_display);
  return 0;
}
