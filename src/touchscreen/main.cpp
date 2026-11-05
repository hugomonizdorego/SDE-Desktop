// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-touchscreen (0.2.0 Alpha 4, docs/TOUCH.md): the session's touch screen
// helper, started at login (data/autostart/sde-touchscreen.desktop).
//
//  * Keeps every touch screen on its display ([touchscreen] output,
//    src/common/touch.h): X maps a touch screen to the whole desktop, so
//    with an external monitor a laptop's touches landed beside the finger.
//    The mapping is made again when a monitor comes or goes (RandR) and when
//    a touch screen appears (XInput hierarchy: plugged in, or back after
//    suspend with a new device).
//  * Shows touches ([touchscreen] show_touches): a round, click-through
//    window follows each finger. Raw touch events are only selected while
//    it is on, so the helper does not wake up for touches otherwise.
//
// One helper per display (the _SDE_TOUCHSCREEN_S<n> selection); it follows
// sde.conf when the Control Center announces a change (_SDE_CONFIG_SERIAL).
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "config.h"
#include "theme.h"
#include "touch.h"

namespace {

Display *g_dpy = nullptr;
Window g_root = 0;
int g_xi = -1, g_randr_event = 0;
sde::Config g_cfg;
bool g_show = false;
double g_remap_at = -1;  // when to map the touch screens again (debounced)

double now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// How a device's raw values become root coordinates, as the X server does
// it: the axis range to 0..1, the Coordinate Transformation Matrix, the
// size of the root window.
struct DeviceMap {
  double min_x = 0, max_x = 1, min_y = 0, max_y = 1;
  float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};
std::map<int, DeviceMap> g_maps;

void load_map(int id) {
  DeviceMap dm;
  int n = 0;
  XIDeviceInfo *info = XIQueryDevice(g_dpy, id, &n);
  for (int i = 0; info && i < n; ++i)
    for (int c = 0; c < info[i].num_classes; ++c) {
      if (info[i].classes[c]->type != XIValuatorClass) continue;
      auto *v = reinterpret_cast<XIValuatorClassInfo *>(info[i].classes[c]);
      if (v->number == 0) {
        dm.min_x = v->min;
        dm.max_x = v->max;
      } else if (v->number == 1) {
        dm.min_y = v->min;
        dm.max_y = v->max;
      }
    }
  if (info) XIFreeDeviceInfo(info);
  Atom prop = XInternAtom(g_dpy, "Coordinate Transformation Matrix", True);
  Atom type;
  int format;
  unsigned long items, after;
  unsigned char *data = nullptr;
  if (prop && XIGetProperty(g_dpy, id, prop, 0, 9, False, AnyPropertyType, &type, &format, &items, &after,
                            &data) == Success &&
      data && format == 32 && items == 9) {
    memcpy(dm.m, data, sizeof dm.m);  // float, as xinput writes it
  }
  if (data) XFree(data);
  g_maps[id] = dm;
}

bool to_root(int device, const XIRawEvent *raw, double &rx, double &ry) {
  double vx = 0, vy = 0;
  bool hx = false, hy = false;
  const double *v = raw->raw_values;
  for (int i = 0; i < raw->valuators.mask_len * 8; ++i) {
    if (!XIMaskIsSet(raw->valuators.mask, i)) continue;
    if (i == 0) vx = *v, hx = true;
    if (i == 1) vy = *v, hy = true;
    ++v;
  }
  if (!hx || !hy) return false;
  if (!g_maps.count(device)) load_map(device);
  const DeviceMap &d = g_maps[device];
  double nx = d.max_x > d.min_x ? (vx - d.min_x) / (d.max_x - d.min_x) : 0;
  double ny = d.max_y > d.min_y ? (vy - d.min_y) / (d.max_y - d.min_y) : 0;
  const float *m = d.m;
  double px = m[0] * nx + m[1] * ny + m[2], py = m[3] * nx + m[4] * ny + m[5], pw = m[6] * nx + m[7] * ny + m[8];
  if (std::fabs(pw) < 1e-9) pw = 1;
  rx = px / pw * DisplayWidth(g_dpy, DefaultScreen(g_dpy));
  ry = py / pw * DisplayHeight(g_dpy, DefaultScreen(g_dpy));
  return true;
}

// The touch points: a round window per finger, click-through.
std::map<long, Window> g_dots;
int dot_size() { return int(std::lround(44 * sde::ui_scale())); }

Window make_dot() {
  int d = dot_size();
  XSetWindowAttributes a;
  a.override_redirect = True;
  sde::Theme t = sde::Theme::from_config(g_cfg);
  XColor c;
  c.red = (unsigned short)(t.accent.r * 65535);
  c.green = (unsigned short)(t.accent.g * 65535);
  c.blue = (unsigned short)(t.accent.b * 65535);
  c.flags = DoRed | DoGreen | DoBlue;
  XAllocColor(g_dpy, DefaultColormap(g_dpy, DefaultScreen(g_dpy)), &c);
  a.background_pixel = c.pixel;
  Window w = XCreateWindow(g_dpy, g_root, -d, -d, d, d, 0, CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel, &a);
  // A disc (bounding shape) that lets every event through (empty input shape).
  Pixmap mask = XCreatePixmap(g_dpy, w, d, d, 1);
  GC gc = XCreateGC(g_dpy, mask, 0, nullptr);
  XSetForeground(g_dpy, gc, 0);
  XFillRectangle(g_dpy, mask, gc, 0, 0, d, d);
  XSetForeground(g_dpy, gc, 1);
  XFillArc(g_dpy, mask, gc, 0, 0, d, d, 0, 360 * 64);
  XShapeCombineMask(g_dpy, w, ShapeBounding, 0, 0, mask, ShapeSet);
  XShapeCombineRectangles(g_dpy, w, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
  XFreeGC(g_dpy, gc);
  XFreePixmap(g_dpy, mask);
  // Translucent where a compositor runs; named for window lists and tests.
  unsigned long opacity = 0x99999999UL;
  XChangeProperty(g_dpy, w, XInternAtom(g_dpy, "_NET_WM_WINDOW_OPACITY", False), XA_CARDINAL, 32,
                  PropModeReplace, reinterpret_cast<unsigned char *>(&opacity), 1);
  XStoreName(g_dpy, w, "SDE Touch Point");
  return w;
}

void dot(long key, double x, double y, bool down) {
  auto it = g_dots.find(key);
  if (!down) {
    if (it != g_dots.end()) {
      XDestroyWindow(g_dpy, it->second);
      g_dots.erase(it);
    }
    return;
  }
  Window w = it != g_dots.end() ? it->second : (g_dots[key] = make_dot());
  int d = dot_size();
  XMoveWindow(g_dpy, w, int(std::lround(x)) - d / 2, int(std::lround(y)) - d / 2);
  XMapRaised(g_dpy, w);
}

void select_events() {
  unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
  XISetMask(bits, XI_HierarchyChanged);
  XISetMask(bits, XI_PropertyEvent);
  XIEventMask all{XIAllDevices, int(sizeof bits), bits};
  XISelectEvents(g_dpy, g_root, &all, 1);
  unsigned char raw[XIMaskLen(XI_LASTEVENT)] = {};
  if (g_show) {
    XISetMask(raw, XI_RawTouchBegin);
    XISetMask(raw, XI_RawTouchUpdate);
    XISetMask(raw, XI_RawTouchEnd);
  }
  XIEventMask masters{XIAllMasterDevices, g_show ? int(sizeof raw) : 0, raw};
  XISelectEvents(g_dpy, g_root, &masters, 1);
  if (!g_show) {
    for (auto &kv : g_dots) XDestroyWindow(g_dpy, kv.second);
    g_dots.clear();
  }
  XFlush(g_dpy);
}

void load_config() {
  g_cfg.load();
  sde::Theme::from_config(g_cfg);  // the interface scale for the dots
  bool show = sde::load_touch_options(g_cfg).show_touches;
  if (show != g_show) {
    g_show = show;
    select_events();
  }
}

void remap() {
  g_remap_at = -1;
  int n = sde::apply_touch_mapping(g_cfg, g_dpy);
  g_maps.clear();  // matrices changed
  if (n) fprintf(stderr, "sde-touchscreen: %d touch screen(s) mapped\n", n);
}

bool g_quit = false;
void on_signal(int) { g_quit = true; }

}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (!strcmp(argv[i], "--help")) {
      printf("Usage: sde-touchscreen\nKeeps touch screens on their display and shows touches "
             "(Control Center > Touch Screen).\n");
      return 0;
    }
  g_dpy = XOpenDisplay(nullptr);
  if (!g_dpy) {
    fprintf(stderr, "sde-touchscreen: cannot open the display\n");
    return 1;
  }
  g_root = DefaultRootWindow(g_dpy);
  // One helper per display.
  char name[64];
  snprintf(name, sizeof name, "_SDE_TOUCHSCREEN_S%d", DefaultScreen(g_dpy));
  Atom selection = XInternAtom(g_dpy, name, False);
  if (XGetSelectionOwner(g_dpy, selection) != None) return 0;
  Window owner = XCreateSimpleWindow(g_dpy, g_root, -1, -1, 1, 1, 0, 0, 0);
  XSetSelectionOwner(g_dpy, selection, owner, CurrentTime);
  if (XGetSelectionOwner(g_dpy, selection) != owner) return 0;

  int event, error, major = 2, minor = 2;
  if (!XQueryExtension(g_dpy, "XInputExtension", &g_xi, &event, &error) ||
      XIQueryVersion(g_dpy, &major, &minor) != Success || (major == 2 && minor < 2)) {
    fprintf(stderr, "sde-touchscreen: no XInput 2.2\n");
    return 0;
  }
  int rr_error;
  if (XRRQueryExtension(g_dpy, &g_randr_event, &rr_error)) XRRSelectInput(g_dpy, g_root, RRScreenChangeNotifyMask);
  XWindowAttributes attr;
  XGetWindowAttributes(g_dpy, g_root, &attr);
  XSelectInput(g_dpy, g_root, attr.your_event_mask | PropertyChangeMask);
  Atom serial = XInternAtom(g_dpy, "_SDE_CONFIG_SERIAL", False);
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  load_config();
  select_events();
  remap();
  while (!g_quit) {
    int timeout = -1;
    if (g_remap_at >= 0) timeout = std::max(0, int(g_remap_at - now_ms()));
    pollfd fd{ConnectionNumber(g_dpy), POLLIN, 0};
    if (!XPending(g_dpy) && poll(&fd, 1, timeout) < 0) continue;
    if (g_remap_at >= 0 && now_ms() >= g_remap_at) remap();
    while (XPending(g_dpy)) {
      XEvent e;
      XNextEvent(g_dpy, &e);
      if (e.type == g_randr_event + RRScreenChangeNotify) {
        XRRUpdateConfiguration(&e);
        g_remap_at = now_ms() + 700;  // a monitor came or went: map again
      } else if (e.type == PropertyNotify && e.xproperty.window == g_root && e.xproperty.atom == serial) {
        std::string before = sde::load_touch_options(g_cfg).output;
        load_config();
        if (sde::load_touch_options(g_cfg).output != before) g_remap_at = now_ms() + 100;
      } else if (e.type == SelectionClear) {
        g_quit = true;
      } else if (e.type == GenericEvent && e.xcookie.extension == g_xi && XGetEventData(g_dpy, &e.xcookie)) {
        int type = e.xcookie.evtype;
        if (type == XI_HierarchyChanged) {
          auto *h = static_cast<XIHierarchyEvent *>(e.xcookie.data);
          if (h->flags & (XISlaveAdded | XIDeviceEnabled | XISlaveAttached)) g_remap_at = now_ms() + 700;
          g_maps.clear();
        } else if (type == XI_PropertyEvent) {
          g_maps.erase(static_cast<XIPropertyEvent *>(e.xcookie.data)->deviceid);
        } else if (g_show && (type == XI_RawTouchBegin || type == XI_RawTouchUpdate || type == XI_RawTouchEnd)) {
          auto *raw = static_cast<XIRawEvent *>(e.xcookie.data);
          double x, y;
          long key = long(raw->sourceid) << 32 | long(raw->detail & 0xffffffff);
          if (type == XI_RawTouchEnd) dot(key, 0, 0, false);
          else if (to_root(raw->sourceid, raw, x, y)) dot(key, x, y, true);
        }
        XFreeEventData(g_dpy, &e.xcookie);
      }
    }
    XFlush(g_dpy);
  }
  XCloseDisplay(g_dpy);
  return 0;
}
