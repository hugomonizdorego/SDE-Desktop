// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "xtarget.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XRes.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <functional>

namespace forceclose {

namespace {

Atom atom(Display *dpy, const char *name) { return XInternAtom(dpy, name, False); }

int ignore_errors(Display *, XErrorEvent *) { return 0; }

// Runs |fn| with X errors ignored (windows that went away meanwhile).
template <typename Fn>
auto quietly(Display *dpy, Fn fn) -> decltype(fn()) {
  XSync(dpy, False);
  XErrorHandler old = XSetErrorHandler(ignore_errors);
  auto r = fn();
  XSync(dpy, False);
  XSetErrorHandler(old);
  return r;
}

bool xres_ids(Display *dpy) {
  static Display *checked = nullptr;
  static bool ok = false;
  if (checked != dpy) {
    checked = dpy;
    int ev, err, major = 0, minor = 0;
    ok = XResQueryExtension(dpy, &ev, &err) && XResQueryVersion(dpy, &major, &minor) &&
         (major > 1 || (major == 1 && minor >= 2));
  }
  return ok;
}

long cardinal(Display *dpy, Window w, const char *prop) {
  Atom type;
  int format;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  long value = 0;
  if (XGetWindowProperty(dpy, w, atom(dpy, prop), 0, 1, False, XA_CARDINAL, &type, &format, &n, &after,
                         &data) == Success &&
      data) {
    if (n == 1 && format == 32) value = reinterpret_cast<long *>(data)[0];
    XFree(data);
  }
  return value;
}

std::string hostname() {
  char buf[256] = {0};
  if (gethostname(buf, sizeof buf - 1) != 0) return "";
  return buf;
}

bool same_machine(Display *dpy, Window w, bool *known) {
  XTextProperty text;
  *known = false;
  if (!XGetWMClientMachine(dpy, w, &text)) return false;
  std::string machine;
  if (text.value && text.nitems) machine.assign(reinterpret_cast<char *>(text.value), text.nitems);
  if (text.value) XFree(text.value);
  if (machine.empty()) return false;
  *known = true;
  std::string here = hostname();
  // The short and the full name of this computer both count.
  auto short_name = [](const std::string &s) { return s.substr(0, s.find('.')); };
  return machine == here || short_name(machine) == short_name(here) || machine == "localhost";
}

}  // namespace

long xres_pid(Display *dpy, XID resource) {
  if (!dpy || !resource || !xres_ids(dpy)) return 0;
  XResClientIdSpec spec;
  spec.client = resource;
  spec.mask = XRES_CLIENT_ID_PID_MASK;
  long n = 0;
  XResClientIdValue *ids = nullptr;
  long pid = 0;
  bool ok = quietly(dpy, [&] { return XResQueryClientIds(dpy, 1, &spec, &n, &ids) == Success; });
  if (ok && ids) {
    for (long i = 0; i < n; ++i)
      if (ids[i].spec.mask == XRES_CLIENT_ID_PID_MASK) {
        pid_t p = XResGetClientPid(&ids[i]);
        if (p > 0) pid = p;
      }
    XResClientIdsDestroy(n, ids);
  }
  return pid;
}

long window_pid(Display *dpy, Window w, bool *remote) {
  if (remote) *remote = false;
  if (!dpy || !w) return 0;
  long pid = xres_pid(dpy, w);
  if (pid > 0) return pid;
  // No X Resource answer: a remote client, or a server without the
  // extension. _NET_WM_PID counts only when the window says it runs here.
  bool known = false;
  bool here = quietly(dpy, [&] { return same_machine(dpy, w, &known); });
  if (known && !here) {
    if (remote) *remote = true;
    return 0;
  }
  if (!here) return 0;
  return quietly(dpy, [&] { return cardinal(dpy, w, "_NET_WM_PID"); });
}

std::vector<Window> client_list(Display *dpy, bool *has_list) {
  std::vector<Window> out;
  if (has_list) *has_list = false;
  Atom type;
  int format;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), atom(dpy, "_NET_CLIENT_LIST"), 0, 4096, False,
                         XA_WINDOW, &type, &format, &n, &after, &data) == Success) {
    if (type == XA_WINDOW && format == 32) {
      if (has_list) *has_list = true;
      for (unsigned long i = 0; i < n; ++i) out.push_back(reinterpret_cast<Window *>(data)[i]);
    }
    if (data) XFree(data);
  }
  return out;
}

bool supports_ping(Display *dpy, Window w) {
  return quietly(dpy, [&] {
    Atom *protocols = nullptr;
    int n = 0;
    bool yes = false;
    if (XGetWMProtocols(dpy, w, &protocols, &n)) {
      Atom ping = atom(dpy, "_NET_WM_PING");
      for (int i = 0; i < n; ++i) yes = yes || protocols[i] == ping;
      XFree(protocols);
    }
    return yes;
  });
}

void send_ping(Display *dpy, Window w, unsigned long id) {
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = w;
  e.xclient.message_type = atom(dpy, "WM_PROTOCOLS");
  e.xclient.format = 32;
  e.xclient.data.l[0] = long(atom(dpy, "_NET_WM_PING"));
  e.xclient.data.l[1] = long(id);
  e.xclient.data.l[2] = long(w);
  quietly(dpy, [&] { return XSendEvent(dpy, w, False, NoEventMask, &e); });
}

bool is_pong(Display *dpy, const XEvent &e, Window *w, unsigned long *id) {
  if (e.type != ClientMessage || e.xclient.window != DefaultRootWindow(dpy) || e.xclient.format != 32 ||
      e.xclient.message_type != atom(dpy, "WM_PROTOCOLS") ||
      Atom(e.xclient.data.l[0]) != atom(dpy, "_NET_WM_PING"))
    return false;
  if (w) *w = Window(e.xclient.data.l[2]);
  if (id) *id = (unsigned long)(e.xclient.data.l[1]) & 0xFFFFFFFFUL;
  return true;
}

bool family_shows_window(Display *dpy, long pid, const std::string &proc) {
  std::vector<long> fam = family(pid, proc);
  if (fam.empty()) return false;
  auto ours = [&](long p) { return p > 0 && std::find(fam.begin(), fam.end(), p) != fam.end(); };
  for (Window w : client_list(dpy))
    if (ours(window_pid(dpy, w))) return true;
  // Its X connections, and whether one of them has a window on screen
  // (a tray icon in the panel, a splash screen without a frame...).
  int n = 0;
  XResClient *clients = nullptr;
  if (!xres_ids(dpy) || !XResQueryClients(dpy, &n, &clients) || !clients) return false;
  std::vector<XResClient> mine;
  for (int i = 0; i < n; ++i)
    if (ours(xres_pid(dpy, clients[i].resource_base))) mine.push_back(clients[i]);
  XFree(clients);
  if (mine.empty()) return false;
  auto belongs = [&](Window w) {
    for (const auto &c : mine)
      if ((w & ~c.resource_mask) == c.resource_base) return true;
    return false;
  };
  std::function<bool(Window, int)> walk = [&](Window parent, int depth) -> bool {
    if (depth > 6) return false;
    Window root_ret, parent_ret, *kids = nullptr;
    unsigned int count = 0;
    if (!quietly(dpy, [&] { return XQueryTree(dpy, parent, &root_ret, &parent_ret, &kids, &count) != 0; }))
      return false;
    std::vector<Window> list(kids, kids + count);
    if (kids) XFree(kids);
    for (Window w : list) {
      XWindowAttributes a;
      if (!quietly(dpy, [&] { return XGetWindowAttributes(dpy, w, &a) != 0; })) continue;
      if (a.map_state != IsViewable) continue;
      if (belongs(w) && a.width > 1 && a.height > 1) return true;
      if (walk(w, depth + 1)) return true;
    }
    return false;
  };
  return walk(DefaultRootWindow(dpy), 0);
}

Verdict force_close_window(Display *dpy, Window w, const Context &ctx, bool dry_run, long *pid_out) {
  if (pid_out) *pid_out = 0;
  if (!dpy || !w) return Verdict::NO_PROCESS;
  XWindowAttributes attr;
  if (!quietly(dpy, [&] { return XGetWindowAttributes(dpy, w, &attr) != 0; })) return Verdict::NO_PROCESS;
  // With a window manager, only the windows it manages: an application's
  // window, not a panel, a menu or an unknown helper window.
  bool has_list = false;
  auto clients = client_list(dpy, &has_list);
  if (has_list && std::find(clients.begin(), clients.end(), w) == clients.end()) return Verdict::UNKNOWN;
  bool remote = false;
  long pid = window_pid(dpy, w, &remote);
  if (pid_out) *pid_out = pid;
  Verdict v;
  if (pid > 0) {
    v = dry_run ? check(pid, ctx) : kill_now(pid, ctx);
    if (v != Verdict::OK) return v;
  } else if (remote) {
    v = Verdict::OK;  // another computer's program: only its connection here
  } else {
    return Verdict::UNKNOWN;
  }
  if (!dry_run) {
    // The connection goes too: the window disappears at once, also when the
    // process cannot take the signal yet (a program stuck in the kernel).
    quietly(dpy, [&] { return XKillClient(dpy, w); });
  }
  return v;
}

}  // namespace forceclose
