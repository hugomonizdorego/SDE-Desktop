// SPDX-License-Identifier: GPL-3.0-or-later
#include "x11.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xutil.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>

namespace sde {
namespace x11 {

static Window root() { return RootWindow(fl_display, fl_screen); }

Atom atom(const char *name) { return XInternAtom(fl_display, name, False); }

// Reads a 32-bit format property into longs.
static std::vector<unsigned long> get_longs(Window w, const char *prop, Atom type,
                                            long max_items = 1 << 20) {
  std::vector<unsigned long> out;
  Atom actual;
  int format;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(fl_display, w, atom(prop), 0, max_items, False, type, &actual, &format,
                         &n, &after, &data) == Success &&
      data) {
    if (format == 32) {
      unsigned long *v = reinterpret_cast<unsigned long *>(data);
      out.assign(v, v + n);
    }
    XFree(data);
  }
  return out;
}

std::vector<unsigned long> get_cardinals(Window w, const char *prop, Atom type) {
  return get_longs(w, prop, type);
}

std::vector<Window> get_windows(Window w, const char *prop) {
  auto v = get_longs(w, prop, XA_WINDOW);
  return std::vector<Window>(v.begin(), v.end());
}

Window get_window(Window w, const char *prop) {
  auto v = get_windows(w, prop);
  return v.empty() ? 0 : v[0];
}

std::vector<Atom> get_atoms(Window w, const char *prop) {
  auto v = get_longs(w, prop, XA_ATOM);
  return std::vector<Atom>(v.begin(), v.end());
}

static std::string get_text(Window w, const char *prop, Atom type) {
  std::string out;
  Atom actual;
  int format;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(fl_display, w, atom(prop), 0, 4096, False, type, &actual, &format, &n,
                         &after, &data) == Success &&
      data) {
    if (format == 8) out.assign(reinterpret_cast<char *>(data), n);
    XFree(data);
  }
  return out;
}

std::string get_utf8(Window w, const char *prop) { return get_text(w, prop, atom("UTF8_STRING")); }

std::string get_string(Window w, const char *prop) { return get_text(w, prop, AnyPropertyType); }

void set_atoms(Window w, const char *prop, const std::vector<const char *> &values) {
  std::vector<Atom> a;
  for (auto v : values) a.push_back(atom(v));
  XChangeProperty(fl_display, w, atom(prop), XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char *>(a.data()), int(a.size()));
}

void set_cardinals(Window w, const char *prop, const std::vector<long> &values) {
  XChangeProperty(fl_display, w, atom(prop), XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(values.data()), int(values.size()));
}

void set_opacity(Window w, double opacity) {
  if (opacity >= 0.999) {
    XDeleteProperty(fl_display, w, atom("_NET_WM_WINDOW_OPACITY"));
    return;
  }
  unsigned long value = (unsigned long)(std::max(0.2, opacity) * 0xFFFFFFFFu);
  XChangeProperty(fl_display, w, atom("_NET_WM_WINDOW_OPACITY"), XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(&value), 1);
}

void set_string(Window w, const char *prop, const std::string &value) {
  XChangeProperty(fl_display, w, atom(prop), atom("UTF8_STRING"), 8, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(value.data()), int(value.size()));
}

std::string window_title(Window w) {
  std::string t = get_utf8(w, "_NET_WM_VISIBLE_NAME");
  if (t.empty()) t = get_utf8(w, "_NET_WM_NAME");
  if (t.empty()) t = get_string(w, "WM_NAME");
  return t;
}

bool window_has_state(Window w, const char *state) {
  Atom a = atom(state);
  for (Atom s : get_atoms(w, "_NET_WM_STATE"))
    if (s == a) return true;
  return false;
}

bool window_is_normal(Window w) {
  auto types = get_atoms(w, "_NET_WM_WINDOW_TYPE");
  if (types.empty()) return XGetTransientForHint(fl_display, w, &w) == 0;
  static const char *hidden[] = {"_NET_WM_WINDOW_TYPE_DOCK",    "_NET_WM_WINDOW_TYPE_DESKTOP",
                                 "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU",
                                 "_NET_WM_WINDOW_TYPE_SPLASH",  "_NET_WM_WINDOW_TYPE_UTILITY",
                                 "_NET_WM_WINDOW_TYPE_NOTIFICATION"};
  for (const char *h : hidden)
    if (types[0] == atom(h)) return false;
  return true;
}

std::string window_class(Window w) {
  XClassHint hint{nullptr, nullptr};
  std::string out;
  if (XGetClassHint(fl_display, w, &hint)) {
    if (hint.res_class) out = hint.res_class;
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
  }
  return out;
}

cairo_surface_t *window_icon(Window w, int size) {
  auto data = get_longs(w, "_NET_WM_ICON", XA_CARDINAL);
  // The property is a list of (width, height, width*height ARGB pixels).
  size_t best = SIZE_MAX;
  unsigned long bw = 0, bh = 0;
  for (size_t i = 0; i + 2 <= data.size();) {
    unsigned long iw = data[i], ih = data[i + 1];
    if (iw == 0 || ih == 0 || iw > 1024 || ih > 1024 || i + 2 + iw * ih > data.size()) break;
    bool better = best == SIZE_MAX ||
                  (bw < unsigned(size) ? iw > bw : (iw >= unsigned(size) && iw < bw));
    if (better) {
      best = i;
      bw = iw;
      bh = ih;
    }
    i += 2 + iw * ih;
  }
  if (best == SIZE_MAX) return nullptr;
  cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(bw), int(bh));
  unsigned char *pixels = cairo_image_surface_get_data(src);
  int stride = cairo_image_surface_get_stride(src);
  for (unsigned long y = 0; y < bh; ++y) {
    uint32_t *row = reinterpret_cast<uint32_t *>(pixels + y * stride);
    for (unsigned long x = 0; x < bw; ++x) {
      uint32_t p = uint32_t(data[best + 2 + y * bw + x]);
      uint32_t a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
      row[x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
    }
  }
  cairo_surface_mark_dirty(src);
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(out);
  double sc = double(size) / std::fmax(bw, bh);
  cairo_scale(cr, sc, sc);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(src);
  return out;
}

static void client_message(Window w, const char *type, long l0, long l1 = 0, long l2 = 0,
                           long l3 = 0, long l4 = 0) {
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = w;
  e.xclient.message_type = atom(type);
  e.xclient.format = 32;
  e.xclient.data.l[0] = l0;
  e.xclient.data.l[1] = l1;
  e.xclient.data.l[2] = l2;
  e.xclient.data.l[3] = l3;
  e.xclient.data.l[4] = l4;
  XSendEvent(fl_display, root(), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
  XFlush(fl_display);
}

void activate_window(Window w) {
  // Source indication 2 = pager/taskbar, so the WM does not refuse focus.
  client_message(w, "_NET_ACTIVE_WINDOW", 2, CurrentTime, 0);
}

void minimize_window(Window w) {
  XIconifyWindow(fl_display, w, fl_screen);
  XFlush(fl_display);
}

void close_window(Window w) { client_message(w, "_NET_CLOSE_WINDOW", CurrentTime, 2); }

void toggle_maximize(Window w) {
  client_message(w, "_NET_WM_STATE", 2 /* toggle */, long(atom("_NET_WM_STATE_MAXIMIZED_VERT")),
                 long(atom("_NET_WM_STATE_MAXIMIZED_HORZ")), 2);
}

void set_showing_desktop(bool show) { client_message(root(), "_NET_SHOWING_DESKTOP", show ? 1 : 0); }

bool showing_desktop() {
  auto v = get_cardinals(root(), "_NET_SHOWING_DESKTOP");
  return !v.empty() && v[0] != 0;
}

void remap(Window w) {
  XWithdrawWindow(fl_display, w, fl_screen);
  XSync(fl_display, False);
  XMapWindow(fl_display, w);
  XFlush(fl_display);
}

void set_window_type(Window w, const char *type) { set_atoms(w, "_NET_WM_WINDOW_TYPE", {type}); }

void set_bottom_strut(Window w, int height, int x_start, int x_end, int screen_h) {
  // Root-relative strut: left, right, top, bottom and the 8 partial values.
  int bottom = screen_h > 0 ? (DisplayHeight(fl_display, fl_screen) - screen_h) + height : height;
  set_cardinals(w, "_NET_WM_STRUT", {0, 0, 0, bottom});
  set_cardinals(w, "_NET_WM_STRUT_PARTIAL", {0, 0, 0, bottom, 0, 0, 0, 0, 0, 0, x_start, x_end});
}

void set_edge_strut(Window w, bool top, int height, int x_start, int x_end, int monitor_y,
                    int monitor_h) {
  if (height <= 0) {
    set_cardinals(w, "_NET_WM_STRUT", {0, 0, 0, 0});
    set_cardinals(w, "_NET_WM_STRUT_PARTIAL", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    return;
  }
  if (top) {
    long t = monitor_y + height;
    set_cardinals(w, "_NET_WM_STRUT", {0, 0, t, 0});
    set_cardinals(w, "_NET_WM_STRUT_PARTIAL", {0, 0, t, 0, 0, 0, 0, 0, x_start, x_end, 0, 0});
  } else {
    set_bottom_strut(w, height, x_start, x_end, monitor_y + monitor_h);
  }
}

void send_command(const std::string &command) {
  // Append a timestamp so that repeating the same command still changes the
  // property and produces a PropertyNotify event.
  std::string value = command + "\n" + std::to_string(time(nullptr)) + "." +
                      std::to_string(clock());
  set_string(root(), "_SDE_COMMAND", value);
  // Wait until the server has the property: `sde-panel --menu` (the Super
  // key) exits right after sending, and requests still buffered in the
  // connection of a client that is gone can be dropped by the server
  // (seen as 1 to 10 in 100 lost Start menu openings before 0.2).
  XSync(fl_display, False);
}

std::string read_command() {
  std::string v = get_utf8(root(), "_SDE_COMMAND");
  return v.substr(0, v.find('\n'));
}

void notify_config_changed() {
  auto v = get_cardinals(root(), "_SDE_CONFIG_SERIAL");
  long serial = v.empty() ? 1 : long(v[0]) + 1;
  set_cardinals(root(), "_SDE_CONFIG_SERIAL", {serial});
  // As send_command(): short-lived programs exit right after this.
  XSync(fl_display, False);
}

bool wm_running(Display *dpy) {
  Atom check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
  Atom actual;
  int format;
  unsigned long n = 0, after = 0;
  unsigned char *data = nullptr;
  bool ok = false;
  if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), check, 0, 1, False, XA_WINDOW, &actual,
                         &format, &n, &after, &data) == Success &&
      data) {
    ok = n == 1;
    XFree(data);
  }
  return ok;
}

}  // namespace x11
}  // namespace sde
