// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal XDND peer written against Xlib only, used by
// tests/test_desktop_dnd.py to check sde-desktop against an independent
// implementation of the protocol.
//
//   xdnd-peer target X Y W H
//       Maps an XDND-aware window, accepts the first drop as a copy and
//       prints "drop <action> <data with \r\n as |>" (then exits).
//   xdnd-peer source WINDOW X Y TYPE DATA [ACTION]
//       Drags DATA (of MIME TYPE, e.g. text/uri-list) onto WINDOW at root
//       position X,Y suggesting ACTION (XdndActionCopy by default) and
//       prints "status <accept> <action>" and "finished <accepted> <action>".
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

static Display *dpy;

static Atom A(const char *n) { return XInternAtom(dpy, n, False); }

static std::string name_of(Atom a) {
  if (!a) return "None";
  char *n = XGetAtomName(dpy, a);
  std::string s = n ? n : "?";
  if (n) XFree(n);
  return s;
}

static void send(Window to, Atom type, long d0, long d1, long d2, long d3, long d4) {
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = to;
  e.xclient.message_type = type;
  e.xclient.format = 32;
  e.xclient.data.l[0] = d0;
  e.xclient.data.l[1] = d1;
  e.xclient.data.l[2] = d2;
  e.xclient.data.l[3] = d3;
  e.xclient.data.l[4] = d4;
  XSendEvent(dpy, to, False, NoEventMask, &e);
  XFlush(dpy);
}

static bool next_event(XEvent *e, int timeout_s) {
  time_t end = time(nullptr) + timeout_s;
  while (time(nullptr) < end) {
    if (XPending(dpy)) {
      XNextEvent(dpy, e);
      return true;
    }
    struct timespec ts{0, 10 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
  return false;
}

static int run_target(int x, int y, int w, int h) {
  Window root = DefaultRootWindow(dpy);
  XSetWindowAttributes wa;
  wa.override_redirect = True;  // no window manager needed
  wa.background_pixel = 0x808080;
  Window win = XCreateWindow(dpy, root, x, y, unsigned(w), unsigned(h), 0, CopyFromParent, InputOutput,
                             CopyFromParent, CWOverrideRedirect | CWBackPixel, &wa);
  long version = 5;
  XChangeProperty(dpy, win, A("XdndAware"), XA_ATOM, 32, PropModeReplace, (unsigned char *)&version, 1);
  XMapRaised(dpy, win);
  XSync(dpy, False);
  printf("ready %lu\n", win);
  fflush(stdout);
  Window source = 0;
  int src_version = 0;
  for (;;) {
    XEvent e;
    if (!next_event(&e, 60)) return 2;
    if (e.type == ClientMessage && e.xclient.message_type == A("XdndEnter")) {
      source = Window(e.xclient.data.l[0]);
      src_version = int((unsigned long)e.xclient.data.l[1] >> 24);
      printf("enter version=%d first-type=%s\n", src_version, name_of(Atom(e.xclient.data.l[2])).c_str());
      fflush(stdout);
    } else if (e.type == ClientMessage && e.xclient.message_type == A("XdndPosition")) {
      send(Window(e.xclient.data.l[0]), A("XdndStatus"), long(win), 1, 0, 0, long(A("XdndActionCopy")));
    } else if (e.type == ClientMessage && e.xclient.message_type == A("XdndDrop")) {
      XConvertSelection(dpy, A("XdndSelection"), A("text/uri-list"), A("PEER_DATA"), win,
                        Time(e.xclient.data.l[2]));
      XFlush(dpy);
    } else if (e.type == SelectionNotify) {
      std::string data;
      if (e.xselection.property != None) {
        Atom type;
        int format;
        unsigned long n, rest;
        unsigned char *d = nullptr;
        XGetWindowProperty(dpy, win, A("PEER_DATA"), 0, 1 << 20, True, AnyPropertyType, &type, &format, &n,
                           &rest, &d);
        if (d) {
          data.assign((char *)d, n);
          XFree(d);
        }
      }
      for (auto &c : data)
        if (c == '\r') c = '|';
      std::string flat;
      for (char c : data)
        if (c != '\n') flat += c;
      printf("drop XdndActionCopy %s\n", flat.c_str());
      fflush(stdout);
      send(source, A("XdndFinished"), long(win), 1, long(A("XdndActionCopy")), 0, 0);
      XSync(dpy, False);
      return 0;
    }
  }
}

static int run_source(Window target, int x, int y, const char *type, const std::string &data, const char *action) {
  Window root = DefaultRootWindow(dpy);
  Window win = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
  Atom t = A(type);
  Atom sel = A("XdndSelection");
  XSetSelectionOwner(dpy, sel, win, CurrentTime);
  Atom act = A(action);
  Atom actions[] = {A("XdndActionCopy"), A("XdndActionMove"), A("XdndActionLink"), A("XdndActionAsk")};
  XChangeProperty(dpy, win, A("XdndActionList"), XA_ATOM, 32, PropModeReplace, (unsigned char *)actions, 4);
  send(target, A("XdndEnter"), long(win), 5L << 24, long(t), 0, 0);
  send(target, A("XdndPosition"), long(win), 0, long((x << 16) | y), CurrentTime, long(act));
  bool accepted = false;
  for (;;) {
    XEvent e;
    if (!next_event(&e, 15)) {
      printf("timeout\n");
      return 2;
    }
    if (e.type == ClientMessage && e.xclient.message_type == A("XdndStatus")) {
      accepted = e.xclient.data.l[1] & 1;
      printf("status %d %s\n", accepted ? 1 : 0, name_of(Atom(e.xclient.data.l[4])).c_str());
      fflush(stdout);
      if (!accepted) {
        send(target, A("XdndLeave"), long(win), 0, 0, 0, 0);
        return 1;
      }
      send(target, A("XdndDrop"), long(win), 0, CurrentTime, 0, 0);
    } else if (e.type == SelectionRequest) {
      XSelectionRequestEvent &r = e.xselectionrequest;
      XEvent reply;
      memset(&reply, 0, sizeof reply);
      reply.xselection.type = SelectionNotify;
      reply.xselection.requestor = r.requestor;
      reply.xselection.selection = r.selection;
      reply.xselection.target = r.target;
      reply.xselection.time = r.time;
      reply.xselection.property = None;
      if (r.target == t) {
        XChangeProperty(dpy, r.requestor, r.property, t, 8, PropModeReplace, (const unsigned char *)data.data(),
                        int(data.size()));
        reply.xselection.property = r.property;
      }
      XSendEvent(dpy, r.requestor, False, NoEventMask, &reply);
      XFlush(dpy);
    } else if (e.type == ClientMessage && e.xclient.message_type == A("XdndFinished")) {
      printf("finished %d %s\n", int(e.xclient.data.l[1] & 1), name_of(Atom(e.xclient.data.l[2])).c_str());
      fflush(stdout);
      return 0;
    }
  }
}

int main(int argc, char **argv) {
  dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    fprintf(stderr, "xdnd-peer: no display\n");
    return 77;
  }
  if (argc == 6 && !strcmp(argv[1], "target"))
    return run_target(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
  if ((argc == 7 || argc == 8) && !strcmp(argv[1], "source")) {
    std::string data = argv[6];
    // "\n" in the argument stands for CRLF line ends.
    std::string out;
    for (size_t i = 0; i < data.size(); ++i) {
      if (data[i] == '\\' && i + 1 < data.size() && data[i + 1] == 'n') {
        out += "\r\n";
        ++i;
      } else {
        out += data[i];
      }
    }
    return run_source(Window(strtoul(argv[2], nullptr, 0)), atoi(argv[3]), atoi(argv[4]), argv[5], out,
                      argc == 8 ? argv[7] : "XdndActionCopy");
  }
  fprintf(stderr, "usage: xdnd-peer target X Y W H | source WINDOW X Y TYPE DATA [ACTION]\n");
  return 2;
}
