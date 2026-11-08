// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-windows: the window and workspace commands behind SDE's keyboard
// shortcuts (0.2.0 Alpha 4; docs/WINDOWS.md, docs/SHORTCUTS.md). SDE WM runs
// it for what Openbox has no action for:
//
//   snap left|right|up|down   Win+arrow keys, as on Windows (snap.h)
//   minimize-all              Win+M
//   restore-minimized         Win+Shift+M
//   minimize-others           Win+Home (again: the others come back)
//   workspace new             Ctrl+Win+D (up to 5, saved in sde.conf)
//   workspace close [N]       Ctrl+Win+F4 (N: 1-based, default the current)
//   next-layout               Win+Space: the next keyboard layout
//   emoji                     Win+. : IBus's emoji picker
//   shortcuts [--openbox]     SDE's shortcut list (or the SDE WM keybinds)
//   list                      workspaces and windows (for scripts and tests)
//
// Plain Xlib requests to the window manager: no window of its own, so a
// shortcut costs a few milliseconds.
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "dbus.h"
#include "i18n.h"
#include "integration.h"
#include "shortcuts.h"
#include "snap.h"
#include "spawn.h"
#include "util.h"
#include "workspaces.h"
#include "x11.h"

using namespace sde;

namespace {

Display *dpy = nullptr;

int ignore_x_errors(Display *, XErrorEvent *) { return 0; }

Window root() { return DefaultRootWindow(dpy); }

void client_message(Window w, const char *type, long l0, long l1 = 0, long l2 = 0, long l3 = 0, long l4 = 0) {
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = w;
  e.xclient.message_type = x11::atom(type);
  e.xclient.format = 32;
  e.xclient.data.l[0] = l0;
  e.xclient.data.l[1] = l1;
  e.xclient.data.l[2] = l2;
  e.xclient.data.l[3] = l3;
  e.xclient.data.l[4] = l4;
  XSendEvent(dpy, root(), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

long current_desktop() {
  auto v = x11::get_cardinals(root(), "_NET_CURRENT_DESKTOP");
  return v.empty() ? 0 : long(v[0]);
}

long window_desktop(Window w) {
  auto v = x11::get_cardinals(w, "_NET_WM_DESKTOP");
  if (v.empty()) return 0;
  return v[0] == 0xFFFFFFFFul ? -1 : long(v[0]);
}

bool minimized(Window w) { return x11::window_has_state(w, "_NET_WM_STATE_HIDDEN"); }

bool maximized(Window w) {
  return x11::window_has_state(w, "_NET_WM_STATE_MAXIMIZED_VERT") &&
         x11::window_has_state(w, "_NET_WM_STATE_MAXIMIZED_HORZ");
}

// Windows of the taskbar on the workspace in view, bottom to top.
std::vector<Window> shown_windows() {
  std::vector<Window> out;
  long cur = current_desktop();
  for (Window w : x11::get_windows(root(), "_NET_CLIENT_LIST_STACKING")) {
    if (!x11::window_is_normal(w) || x11::window_has_state(w, "_NET_WM_STATE_SKIP_TASKBAR")) continue;
    long d = window_desktop(w);
    if (d >= 0 && d != cur) continue;
    out.push_back(w);
  }
  return out;
}

bool exists(Window w) {
  auto all = x11::get_windows(root(), "_NET_CLIENT_LIST");
  return std::find(all.begin(), all.end(), w) != all.end();
}

void remember(const char *prop, const std::vector<Window> &list) {
  if (list.empty()) {
    XDeleteProperty(dpy, root(), x11::atom(prop));
    return;
  }
  std::vector<long> v(list.begin(), list.end());
  XChangeProperty(dpy, root(), x11::atom(prop), XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(v.data()), int(v.size()));
}

void minimize(Window w) { XIconifyWindow(dpy, w, DefaultScreen(dpy)); }
void activate(Window w) { client_message(w, "_NET_ACTIVE_WINDOW", 2, CurrentTime, 0); }

// ------------------------------------------------------------ minimize

// Win+M: every window of this workspace; Win+Shift+M brings back these.
int minimize_all() {
  std::vector<Window> done;
  for (Window w : shown_windows())
    if (!minimized(w)) {
      minimize(w);
      done.push_back(w);
    }
  remember("_SDE_MINIMIZED_ALL", done);
  XSync(dpy, False);
  return 0;
}

int restore_minimized() {
  std::vector<Window> list = x11::get_windows(root(), "_SDE_MINIMIZED_ALL");
  std::vector<Window> back;
  for (Window w : list)
    if (exists(w) && minimized(w)) back.push_back(w);
  // Nothing that Win+M minimized: every minimized window of this workspace
  // (as Windows describes Win+Shift+M).
  if (back.empty())
    for (Window w : shown_windows())
      if (minimized(w)) back.push_back(w);
  // Bottom to top: the window that was on top ends on top, with the focus.
  for (Window w : back) activate(w);
  remember("_SDE_MINIMIZED_ALL", {});
  XSync(dpy, False);
  return 0;
}

// Win+Home: all but the active window; pressed again, the others return.
int minimize_others() {
  Window active = x11::get_window(root(), "_NET_ACTIVE_WINDOW");
  std::vector<Window> before = x11::get_windows(root(), "_SDE_MINIMIZED_OTHERS");
  if (!before.empty() && before[0] == active) {
    bool restored = false;
    for (size_t i = 1; i < before.size(); ++i)
      if (exists(before[i]) && minimized(before[i])) {
        activate(before[i]);
        restored = true;
      }
    if (restored && exists(active)) activate(active);
    remember("_SDE_MINIMIZED_OTHERS", {});
    if (restored) {
      XSync(dpy, False);
      return 0;
    }
  }
  std::vector<Window> done{active};
  for (Window w : shown_windows())
    if (w != active && !minimized(w)) {
      minimize(w);
      done.push_back(w);
    }
  remember("_SDE_MINIMIZED_OTHERS", done.size() > 1 ? done : std::vector<Window>{});
  XSync(dpy, False);
  return 0;
}

// ---------------------------------------------------------------- snap

struct Extents {
  long left = 0, right = 0, top = 0, bottom = 0;
};

Extents frame_extents(Window w) {
  Extents e;
  auto v = x11::get_cardinals(w, "_NET_FRAME_EXTENTS");
  if (v.size() >= 4) e = {long(v[0]), long(v[1]), long(v[2]), long(v[3])};
  return e;
}

// The frame's outer rectangle on the screen.
snap::Rect frame_rect(Window w) {
  snap::Rect r;
  Window rt, child;
  int x, y;
  unsigned cw, ch, bw, depth;
  if (!XGetGeometry(dpy, w, &rt, &x, &y, &cw, &ch, &bw, &depth)) return r;
  int rx = 0, ry = 0;
  XTranslateCoordinates(dpy, w, root(), 0, 0, &rx, &ry, &child);
  Extents e = frame_extents(w);
  r.x = rx - int(e.left);
  r.y = ry - int(e.top);
  r.w = int(cw) + int(e.left + e.right);
  r.h = int(ch) + int(e.top + e.bottom);
  return r;
}

// The work area (without the panel) of the monitor that holds most of |r|.
snap::Rect work_area(const snap::Rect &r) {
  snap::Rect area{0, 0, DisplayWidth(dpy, DefaultScreen(dpy)), DisplayHeight(dpy, DefaultScreen(dpy))};
  int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  int n = 0;
  if (XRRMonitorInfo *mons = XRRGetMonitors(dpy, root(), True, &n)) {
    for (int i = 0; i < n; ++i) {
      const XRRMonitorInfo &m = mons[i];
      if (cx >= m.x && cx < m.x + m.width && cy >= m.y && cy < m.y + m.height) {
        area = {m.x, m.y, m.width, m.height};
        break;
      }
      if (i == 0) area = {m.x, m.y, m.width, m.height};
    }
    XRRFreeMonitors(mons);
  }
  // _NET_WORKAREA leaves out the panel (struts); it spans all monitors.
  auto wa = x11::get_cardinals(root(), "_NET_WORKAREA");
  size_t at = size_t(std::max(0L, current_desktop())) * 4;
  if (wa.size() >= at + 4) {
    int x0 = std::max(area.x, int(wa[at])), y0 = std::max(area.y, int(wa[at + 1]));
    int x1 = std::min(area.x + area.w, int(wa[at] + wa[at + 2]));
    int y1 = std::min(area.y + area.h, int(wa[at + 1] + wa[at + 3]));
    if (x1 - x0 > 100 && y1 - y0 > 100) area = {x0, y0, x1 - x0, y1 - y0};
  }
  return area;
}

void place(Window w, const snap::Rect &frame) {
  Extents e = frame_extents(w);
  int cw = std::max(1, frame.w - int(e.left + e.right)), ch = std::max(1, frame.h - int(e.top + e.bottom));
  // NorthWest gravity: x, y is where the frame's top-left corner goes.
  long flags = 1 | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (2L << 12);
  client_message(w, "_NET_MOVERESIZE_WINDOW", flags, frame.x, frame.y, cw, ch);
}

void set_maximized(Window w, bool on) {
  client_message(w, "_NET_WM_STATE", on ? 1 : 0, long(x11::atom("_NET_WM_STATE_MAXIMIZED_VERT")),
                 long(x11::atom("_NET_WM_STATE_MAXIMIZED_HORZ")), 2);
}

int snap_window(const std::string &how) {
  snap::Key key;
  if (how == "left") key = snap::Key::Left;
  else if (how == "right") key = snap::Key::Right;
  else if (how == "up") key = snap::Key::Up;
  else if (how == "down") key = snap::Key::Down;
  else return 2;
  Window w = x11::get_window(root(), "_NET_ACTIVE_WINDOW");
  if (!w || !x11::window_is_normal(w)) return 1;
  snap::Rect now = frame_rect(w);
  snap::Rect area = work_area(now);
  // _SDE_SNAP: zone, then the frame before the window was snapped.
  auto saved = x11::get_cardinals(w, "_SDE_SNAP");
  snap::Zone zone = snap::Zone::Free;
  bool max = maximized(w);
  if (saved.size() >= 5) {
    auto z = snap::Zone(int(saved[0]));
    if (!max && snap::fills(now, z, area)) zone = z;
  }
  if (!max && zone == snap::Zone::Free && saved.size() >= 5) {
    // Moved or resized by hand since: no longer snapped.
    XDeleteProperty(dpy, w, x11::atom("_SDE_SNAP"));
    saved.clear();
  }
  snap::Plan p = snap::plan(zone, max, key);
  switch (p.step) {
    case snap::Step::Nothing:
      break;
    case snap::Step::Maximize:
      set_maximized(w, true);
      break;
    case snap::Step::Unmaximize:
      set_maximized(w, false);
      break;
    case snap::Step::Minimize:
      minimize(w);
      break;
    case snap::Step::Restore:
      if (saved.size() >= 5) {
        place(w, {int(long(saved[1])), int(long(saved[2])), int(saved[3]), int(saved[4])});
      }
      XDeleteProperty(dpy, w, x11::atom("_SDE_SNAP"));
      break;
    case snap::Step::SnapTo: {
      // Where the window goes back to: what it was before the first snap.
      snap::Rect before = now;
      if (saved.size() >= 5) before = {int(long(saved[1])), int(long(saved[2])), int(saved[3]), int(saved[4])};
      if (max) {
        set_maximized(w, false);
        if (saved.size() < 5) {
          // A maximized window returns to its size before it was maximized;
          // that is not known here, so a centred two-thirds frame.
          before = {area.x + area.w / 6, area.y + area.h / 6, area.w * 2 / 3, area.h * 2 / 3};
        }
      }
      x11::set_cardinals(w, "_SDE_SNAP", {long(p.zone), before.x, before.y, before.w, before.h});
      place(w, snap::zone_rect(p.zone, area));
      break;
    }
  }
  XSync(dpy, False);
  return 0;
}

// ---------------------------------------------------------- workspaces

int workspace(int argc, char **argv) {
  std::string what = argc > 0 ? argv[0] : "";
  Config cfg;
  cfg.load();
  bool changed = false;
  if (what == "new" || what == "add") {
    changed = workspaces::add(dpy, cfg);
  } else if (what == "close" || what == "remove") {
    int index = argc > 1 ? atoi(argv[1]) - 1 : -1;
    changed = workspaces::close(dpy, cfg, index);
  } else if (what == "list") {
    auto live = workspaces::read_live(dpy);
    auto s = workspaces::Settings::from_config(cfg);
    for (int i = 0; i < std::max(live.count, 1); ++i)
      printf("%d\t%s%s\n", i + 1, (i < int(live.names.size()) && !live.names[size_t(i)].empty() ? live.names[size_t(i)]
                                                                                              : s.name(i))
                                      .c_str(),
             i == live.current ? "\t*" : "");
    return 0;
  } else {
    fprintf(stderr, "sde-windows: workspace new|close [N]|list\n");
    return 2;
  }
  if (!changed) return 1;
  cfg.save();
  // The count goes into SDE WM's rc.xml for the next login; the Control
  // Center and the panel read it again.
  announce_config_change(dpy, cfg);
  return 0;
}

// --------------------------------------------------------------- others

int next_layout() {
  XkbStateRec state;
  if (XkbGetState(dpy, XkbUseCoreKbd, &state) != Success) return 1;
  XkbDescPtr desc = XkbGetKeyboard(dpy, XkbControlsMask, XkbUseCoreKbd);
  int groups = 1;
  if (desc) {
    if (desc->ctrls) groups = std::max(1, int(desc->ctrls->num_groups));
    XkbFreeKeyboard(desc, 0, True);
  }
  if (groups < 2) return 1;
  XkbLockGroup(dpy, XkbUseCoreKbd, (state.group + 1) % groups);
  XSync(dpy, False);
  return 0;
}

void notify(const std::string &summary, const std::string &body) {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return;
  DBusMessage *m = dbus::method("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                "org.freedesktop.Notifications", "Notify");
  {
    dbus::Writer w(m);
    w.str("SDE").u32(0).str("face-smile").str(summary).str(body).strings({});
    { dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}"); }
    w.i32(-1);
  }
  if (DBusMessage *reply = bus->call(m, 2000)) dbus_message_unref(reply);
}

int emoji() {
  if (!find_program("ibus").empty() && spawn_detached({"ibus", "emoji"})) return 0;
  notify(_("No emoji picker"), _("Win+. opens the emoji picker of IBus. Install the ibus package to use it."));
  return 1;
}

// Writes the workspaces and the shortcuts of sde.conf into an SDE WM
// configuration (what sync_openbox() does to ~/.config/sde/openbox/rc.xml):
// for the packaged template (data/openbox/rc.xml) and the tests.
int write_openbox_config(const std::string &path) {
  Config cfg;
  cfg.load();
  std::string text;
  if (!read_file(path, text)) {
    fprintf(stderr, "sde-windows: cannot read %s\n", path.c_str());
    return 1;
  }
  bool changed = workspaces::sync_openbox_desktops(text, workspaces::Settings::from_config(cfg));
  changed = shortcuts::sync_openbox_keyboard(text, cfg) || changed;
  if (changed && !write_file_atomic(path, text)) return 1;
  printf("%s %s\n", path.c_str(), changed ? "updated" : "unchanged");
  return 0;
}

int print_shortcuts(bool openbox, bool groups = false) {
  Config cfg;
  cfg.load();
  if (groups) {
    for (const auto &g : shortcuts::groups()) printf("%s\t%s\n", g.id, _(g.title));
    return 0;
  }
  if (openbox) {
    for (const auto &b : shortcuts::bindings(cfg)) printf("%s\n", b.xml.c_str());
    return 0;
  }
  // id, keys as shown, group, scope, keys (Openbox), description, and the
  // action (system) or where it works (application).
  for (const auto &s : shortcuts::table()) {
    std::string keys;
    for (const auto &k : shortcuts::keys_of(s, &cfg)) keys += (keys.empty() ? "" : " ") + k;
    printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n", s.id, shortcuts::display_keys(s).c_str(), s.group,
           s.scope == shortcuts::Scope::System ? "system" : "app", keys.c_str(), _(s.label), s.action);
  }
  return 0;
}

int list() {
  auto live = workspaces::read_live(dpy);
  printf("workspaces\t%d\tcurrent\t%d\n", live.count, live.current + 1);
  Window active = x11::get_window(root(), "_NET_ACTIVE_WINDOW");
  for (const auto &w : workspaces::windows(dpy)) {
    printf("0x%lx\t%ld\t%s%s%s\n", w.win, w.desktop < 0 ? -1 : w.desktop + 1, w.minimized ? "minimized" : "shown",
           w.win == active ? "\tactive" : "", ("\t" + x11::window_title(w.win)).c_str());
  }
  return 0;
}

int usage(int status) {
  printf("%s", _("Usage: sde-windows COMMAND\n\n"
                 "The window and workspace commands of SDE's keyboard shortcuts:\n"
                 "  snap left|right|up|down   snap the active window (Win+arrow keys)\n"
                 "  minimize-all              minimize every window (Win+M)\n"
                 "  restore-minimized         restore them (Win+Shift+M)\n"
                 "  minimize-others           minimize all but the active window (Win+Home)\n"
                 "  workspace new             add a workspace (Ctrl+Win+D)\n"
                 "  workspace close [N]       close workspace N or this one (Ctrl+Win+F4)\n"
                 "  workspace list            the workspaces\n"
                 "  next-layout               the next keyboard layout (Win+Space)\n"
                 "  emoji                     the emoji picker (Win+.)\n"
                 "  shortcuts [--openbox | --groups]  SDE's keyboard shortcuts (as SDE WM\n"
                 "                            keybinds, or their sections)\n"
                 "  openbox-config FILE       write the workspaces and shortcuts into an\n"
                 "                            SDE WM configuration\n"
                 "  list                      workspaces and windows\n"));
  return status;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  if (argc < 2) return usage(2);
  std::string cmd = argv[1];
  if (cmd == "--help" || cmd == "-h" || cmd == "help") return usage(0);
  if (cmd == "--version") return print_version("sde-windows", SDE_VERSION);
  if (cmd == "shortcuts")
    return print_shortcuts(argc > 2 && !strcmp(argv[2], "--openbox"), argc > 2 && !strcmp(argv[2], "--groups"));
  if (cmd == "openbox-config" && argc > 2) return write_openbox_config(argv[2]);
  // FLTK's display variables only, without FLTK's display setup.
  dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    fprintf(stderr, "sde-windows: cannot open the display\n");
    return 1;
  }
  fl_display = dpy;
  fl_screen = DefaultScreen(dpy);
  XSetErrorHandler(ignore_x_errors);
  int status = 2;
  if (cmd == "snap" && argc > 2) status = snap_window(argv[2]);
  else if (cmd == "minimize-all") status = minimize_all();
  else if (cmd == "restore-minimized") status = restore_minimized();
  else if (cmd == "minimize-others") status = minimize_others();
  else if (cmd == "workspace") status = workspace(argc - 2, argv + 2);
  else if (cmd == "next-layout") status = next_layout();
  else if (cmd == "emoji") status = emoji();
  else if (cmd == "list") status = list();
  else status = usage(2);
  XSync(dpy, False);
  XCloseDisplay(dpy);
  fl_display = nullptr;
  return status;
}
