// SPDX-License-Identifier: GPL-3.0-or-later
// A test program for Force Close (docs/FORCE-CLOSE.md), never installed.
//
//   sde-frozen-client [--title TEXT] [--class NAME] [--frozen] [--no-window]
//                     [--no-ping] [--ready FILE]
//       An application window that answers _NET_WM_PING like a working
//       program, until SIGUSR1 makes it stop reading X events (a program
//       that hangs: busy, or stuck in a call); SIGUSR2 brings it back.
//       WM_DELETE_WINDOW ends it while it responds. --frozen hangs right
//       after the window is mapped, --no-window never opens one (a program
//       that does not start), --no-ping does not list _NET_WM_PING.
//       --ready FILE is written with the window id once it is mapped.
//   sde-frozen-client --ping XID [--timeout MS]
//       Sends _NET_WM_PING to window XID and waits for the answer on the
//       root window: exit status 0 answered, 1 not (an SDE-FLTK window
//       answers since 0.2.0 Alpha 3.1).
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

volatile sig_atomic_t g_frozen = 0;

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int ping_mode(Display *dpy, Window target, int timeout_ms) {
  Window root = DefaultRootWindow(dpy);
  XWindowAttributes a;
  XGetWindowAttributes(dpy, root, &a);
  XSelectInput(dpy, root, a.your_event_mask | SubstructureNotifyMask);
  Atom protocols = XInternAtom(dpy, "WM_PROTOCOLS", False), ping = XInternAtom(dpy, "_NET_WM_PING", False);
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = target;
  e.xclient.message_type = protocols;
  e.xclient.format = 32;
  e.xclient.data.l[0] = long(ping);
  e.xclient.data.l[1] = 0x5D0C0001L;
  e.xclient.data.l[2] = long(target);
  XSendEvent(dpy, target, False, NoEventMask, &e);
  XFlush(dpy);
  double end = now() + timeout_ms / 1000.0;
  while (now() < end) {
    while (XPending(dpy)) {
      XEvent r;
      XNextEvent(dpy, &r);
      if (r.type == ClientMessage && r.xclient.window == root && r.xclient.message_type == protocols &&
          Atom(r.xclient.data.l[0]) == ping && Window(r.xclient.data.l[2]) == target)
        return 0;
    }
    pollfd p{ConnectionNumber(dpy), POLLIN, 0};
    poll(&p, 1, 20);
  }
  return 1;
}

}  // namespace

int main(int argc, char **argv) {
  std::string title = "SDE Frozen Client", cls = "sde-frozen-client", ready;
  bool frozen = false, no_window = false, no_ping = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i], next = i + 1 < argc ? argv[i + 1] : "";
    if (a == "--ping" && i + 1 < argc) {
      int timeout = 2000;
      if (i + 3 < argc && std::string(argv[i + 2]) == "--timeout") timeout = atoi(argv[i + 3]);
      Display *dpy = XOpenDisplay(nullptr);
      if (!dpy) return 2;
      return ping_mode(dpy, Window(strtoul(next.c_str(), nullptr, 0)), timeout);
    }
    if (a == "--title") title = next, ++i;
    else if (a == "--class") cls = next, ++i;
    else if (a == "--ready") ready = next, ++i;
    else if (a == "--frozen") frozen = true;
    else if (a == "--no-window") no_window = true;
    else if (a == "--no-ping") no_ping = true;
  }
  signal(SIGUSR1, [](int) { g_frozen = 1; });
  signal(SIGUSR2, [](int) { g_frozen = 0; });
  if (no_window) {
    // A program that never gets as far as a window.
    for (;;) pause();
  }
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) return 2;
  Window root = DefaultRootWindow(dpy);
  Window w = XCreateSimpleWindow(dpy, root, 120, 100, 420, 260, 0, 0, 0xF2F4F8);
  XStoreName(dpy, w, title.c_str());
  Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
  XChangeProperty(dpy, w, XInternAtom(dpy, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(title.data()), int(title.size()));
  XClassHint hint;
  std::string name = cls;
  hint.res_name = &name[0];
  hint.res_class = &cls[0];
  XSetClassHint(dpy, w, &hint);
  XSetWMProperties(dpy, w, nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr);  // WM_CLIENT_MACHINE
  long pid = getpid();
  XChangeProperty(dpy, w, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char *>(&pid), 1);
  Atom del = XInternAtom(dpy, "WM_DELETE_WINDOW", False), ping = XInternAtom(dpy, "_NET_WM_PING", False);
  Atom list[2] = {del, ping};
  XSetWMProtocols(dpy, w, list, no_ping ? 1 : 2);
  XSelectInput(dpy, w, ExposureMask | StructureNotifyMask);
  XMapWindow(dpy, w);
  XFlush(dpy);
  bool mapped = false;
  for (;;) {
    if (g_frozen) {
      // Hung: nothing is read, nothing answered.
      usleep(20000);
      continue;
    }
    while (!g_frozen && XPending(dpy)) {
      XEvent e;
      XNextEvent(dpy, &e);
      if (e.type == MapNotify && !mapped) {
        mapped = true;
        if (!ready.empty()) {
          if (FILE *f = fopen((ready + ".tmp").c_str(), "w")) {
            fprintf(f, "0x%lx\n", w);
            fclose(f);
            rename((ready + ".tmp").c_str(), ready.c_str());
          }
        }
        if (frozen) g_frozen = 1;
      } else if (e.type == ClientMessage && Atom(e.xclient.data.l[0]) == ping) {
        XEvent pong = e;
        pong.xclient.window = root;
        XSendEvent(dpy, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &pong);
        XFlush(dpy);
      } else if (e.type == ClientMessage && Atom(e.xclient.data.l[0]) == del) {
        return 0;
      } else if (e.type == Expose) {
        GC gc = XCreateGC(dpy, w, 0, nullptr);
        XSetForeground(dpy, gc, 0x2563EB);
        XFillRectangle(dpy, w, gc, 0, 0, 420, 36);
        XFreeGC(dpy, gc);
      }
    }
    if (g_frozen) continue;
    pollfd p{ConnectionNumber(dpy), POLLIN, 0};
    poll(&p, 1, 50);
  }
}
