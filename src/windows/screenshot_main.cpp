// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-screenshot: screenshots, as on Windows (0.2.0 Alpha 4,
// docs/SHORTCUTS.md):
//
//   PrtScn, Win+PrtScn   sde-screenshot --screen   the whole screen
//   Alt+PrtScn           sde-screenshot --window   the active window
//   Win+Shift+S          sde-screenshot --snip     drag an area, or pick a
//                                                  window or the whole screen
//
// Each one is copied to the clipboard (image/png) and saved as
// ~/Pictures/Screenshots/Screenshot <date> <time>.png; a notification says
// where. --no-save, --no-clipboard, --output FILE and --delay SECONDS
// change that. The clipboard is served by a small process of its own that
// ends when another program takes the clipboard (at the latest after 12
// hours). SDE_SCREENSHOT_QA_DIR: the snip's toolbar is written to
// <dir>/screenshot.txt for the UI tests.
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <list>
#include <string>
#include <thread>
#include <vector>

#include "about.h"
#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "dbus.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "screenshot_model.h"
#include "theme.h"
#include "util.h"
#include "x11.h"

using namespace sde;
using screenshot::Rect;

namespace {

Theme g_theme;

int ignore_x_errors(Display *, XErrorEvent *) { return 0; }

Window root() { return RootWindow(fl_display, fl_screen); }

// ------------------------------------------------------------- capture

cairo_surface_t *capture_screen() {
  int w = DisplayWidth(fl_display, fl_screen), h = DisplayHeight(fl_display, fl_screen);
  XImage *img = XGetImage(fl_display, root(), 0, 0, unsigned(w), unsigned(h), AllPlanes, ZPixmap);
  if (!img) return nullptr;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  bool fast = img->bits_per_pixel == 32 && img->red_mask == 0xFF0000 && img->green_mask == 0xFF00 &&
              img->blue_mask == 0xFF && img->byte_order == LSBFirst;
  for (int y = 0; y < h; ++y) {
    uint32_t *row = reinterpret_cast<uint32_t *>(data + y * stride);
    if (fast) {
      memcpy(row, img->data + y * img->bytes_per_line, size_t(w) * 4);
      for (int x = 0; x < w; ++x) row[x] &= 0xFFFFFF;
    } else {
      for (int x = 0; x < w; ++x) row[x] = uint32_t(XGetPixel(img, x, y)) & 0xFFFFFF;
    }
  }
  XDestroyImage(img);
  cairo_surface_mark_dirty(s);
  return s;
}

Rect screen_rect() { return {0, 0, DisplayWidth(fl_display, fl_screen), DisplayHeight(fl_display, fl_screen)}; }

// A window's frame on the screen (the window with its title bar).
Rect frame_rect(Window w) {
  Rect r;
  Window rt, child;
  int x, y;
  unsigned cw, ch, bw, depth;
  if (!w || !XGetGeometry(fl_display, w, &rt, &x, &y, &cw, &ch, &bw, &depth)) return r;
  int rx = 0, ry = 0;
  XTranslateCoordinates(fl_display, w, root(), 0, 0, &rx, &ry, &child);
  auto e = x11::get_cardinals(w, "_NET_FRAME_EXTENTS");
  long l = e.size() >= 4 ? long(e[0]) : 0, rr = e.size() >= 4 ? long(e[1]) : 0;
  long t = e.size() >= 4 ? long(e[2]) : 0, b = e.size() >= 4 ? long(e[3]) : 0;
  r = {rx - int(l), ry - int(t), int(cw) + int(l + rr), int(ch) + int(t + b)};
  return screenshot::intersect(r, screen_rect());
}

cairo_surface_t *crop(cairo_surface_t *src, const Rect &r) {
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, r.w, r.h);
  cairo_t *cr = cairo_create(out);
  cairo_set_source_surface(cr, src, -r.x, -r.y);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(out);
  return out;
}

cairo_status_t append_png(void *closure, const unsigned char *data, unsigned int length) {
  static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), length);
  return CAIRO_STATUS_SUCCESS;
}

std::string png_of(cairo_surface_t *s) {
  std::string out;
  cairo_surface_write_to_png_stream(s, append_png, &out);
  return out;
}

// ---------------------------------------------------------------- snip

// The windows on the screen, top first, for "Window" mode.
std::vector<Rect> window_rects() {
  std::vector<Rect> out;
  auto stack = x11::get_windows(root(), "_NET_CLIENT_LIST_STACKING");
  long cur = 0;
  auto c = x11::get_cardinals(root(), "_NET_CURRENT_DESKTOP");
  if (!c.empty()) cur = long(c[0]);
  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    Window w = *it;
    if (!x11::window_is_normal(w) || x11::window_has_state(w, "_NET_WM_STATE_HIDDEN")) continue;
    auto d = x11::get_cardinals(w, "_NET_WM_DESKTOP");
    if (!d.empty() && d[0] != 0xFFFFFFFFul && long(d[0]) != cur) continue;
    Rect r = frame_rect(w);
    if (!r.empty()) out.push_back(r);
  }
  return out;
}

class Snip : public BufferedWindow {
 public:
  enum Mode { AREA, WINDOW, SCREEN };
  Snip(int w, int h, cairo_surface_t *shot) : BufferedWindow(0, 0, w, h, "Screenshot"), shot_(shot) {
    windows_ = window_rects();
  }

  bool done = false, cancelled = false;
  Rect result;

  void draw() override {
    const Theme &t = g_theme;
    {
      Painter p(w(), h());
      cairo_t *cr = p.cr();
      cairo_set_source_surface(cr, shot_, 0, 0);
      cairo_paint(cr);
      // Everything but the chosen part is dimmed.
      Rect sel = selection();
      cairo_save(cr);
      cairo_rectangle(cr, 0, 0, w(), h());
      if (!sel.empty()) cairo_rectangle(cr, sel.x + sel.w, sel.y, -sel.w, sel.h);
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
      cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
      cairo_fill(cr);
      cairo_restore(cr);
      if (!sel.empty()) p.stroke_rounded(sel.x + 0.5, sel.y + 0.5, sel.w - 1, sel.h - 1, 0, t.accent, Sd(2));
      // The toolbar.
      Rect bar = toolbar();
      p.shadow(bar.x, bar.y, bar.w, bar.h, t.radius, t.shadow, S(10));
      p.fill_rounded(bar.x, bar.y, bar.w, bar.h, t.radius, t.menu_bg);
      p.stroke_rounded(bar.x + 0.5, bar.y + 0.5, bar.w - 1, bar.h - 1, t.radius, t.menu_border, 1);
      for (int i = 0; i < 4; ++i) {
        Rect b = button(i);
        bool on = i == int(mode_), hot = i == hover_;
        if (on || hot) p.fill_rounded(b.x, b.y, b.w, b.h, t.radius_small, on ? t.accent.alpha(0.18) : t.menu_hover.alpha(0.5));
        Color c = on ? t.accent_text : t.menu_text;
        double s = S(18), gx = b.x + S(10), gy = b.y + (b.h - s) / 2;
        if (i == AREA) {
          cairo_save(cr);
          double d[] = {Sd(3), Sd(2.5)};
          cairo_set_dash(cr, d, 2, 0);
          p.stroke_rounded(gx + 1, gy + 2, s - 2, s - 4, S(2), c, Sd(1.6));
          cairo_restore(cr);
        } else if (i == WINDOW) {
          p.stroke_rounded(gx + 1, gy + 2, s - 2, s - 4, S(3), c, Sd(1.6));
          p.line(gx + 1, gy + 6, gx + s - 1, gy + 6, c, Sd(1.6));
        } else if (i == SCREEN) {
          p.stroke_rounded(gx, gy + 1, s, s * 0.7, S(2), c, Sd(1.6));
          p.line(gx + s / 2, gy + s * 0.72, gx + s / 2, gy + s * 0.92, c, Sd(1.6));
          p.line(gx + s * 0.3, gy + s * 0.92, gx + s * 0.7, gy + s * 0.92, c, Sd(1.6));
        } else {
          p.line(gx + 3, gy + 3, gx + s - 3, gy + s - 3, c, Sd(1.6));
          p.line(gx + s - 3, gy + 3, gx + 3, gy + s - 3, c, Sd(1.6));
        }
      }
    }
    static const char *labels[] = {N_("Area"), N_("Window"), N_("Full screen"), N_("Close")};
    for (int i = 0; i < 4; ++i) {
      Rect b = button(i);
      text_font(g_theme.font_size, i == int(mode_));
      draw_text(_(labels[i]), b.x + S(34), b.y, b.w - S(40), b.h, i == int(mode_) ? g_theme.accent_text : g_theme.menu_text);
    }
    Rect sel = selection();
    if (!sel.empty() && mode_ == AREA && dragging_) {
      std::string size = std::to_string(sel.w) + " × " + std::to_string(sel.h);
      text_font(g_theme.font_size - 1, true);
      int tw = text_width(size) + S(16);
      int ty = sel.y + sel.h + S(6) + S(22) < h() ? sel.y + sel.h + S(6) : sel.y - S(28);
      {
        Painter p(w(), h());
        p.fill_rounded(sel.x, ty, tw, S(22), S(6), Color::hex(0x000000, 0.7));
      }
      draw_text(size, sel.x, ty, tw, S(22), Color::hex(0xFFFFFF), 1);
    }
    std::string hint = mode_ == AREA ? _("Drag over the part of the screen to copy; Esc cancels")
                       : mode_ == WINDOW ? _("Click a window to copy it; Esc cancels")
                                         : _("Click to copy the whole screen; Esc cancels");
    Rect bar = toolbar();
    text_font(g_theme.font_size - 1);
    int hw = text_width(hint) + S(24);
    {
      Painter p(w(), h());
      p.fill_rounded((w() - hw) / 2, bar.y + bar.h + S(8), hw, S(26), S(13), Color::hex(0x000000, 0.6));
    }
    draw_text(hint, (w() - hw) / 2, bar.y + bar.h + S(8), hw, S(26), Color::hex(0xFFFFFF), 1);
  }

  int handle(int e) override {
    switch (e) {
      case FL_FOCUS:
      case FL_UNFOCUS:
        return 1;
      case FL_MOVE: {
        int b = button_at(Fl::event_x(), Fl::event_y());
        if (b != hover_) {
          hover_ = b;
          redraw();
        }
        if (mode_ == WINDOW) {
          int k = window_at(Fl::event_x(), Fl::event_y());
          if (k != hot_window_) {
            hot_window_ = k;
            redraw();
          }
        }
        return 1;
      }
      case FL_PUSH: {
        int b = button_at(Fl::event_x(), Fl::event_y());
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          finish(true);
          return 1;
        }
        if (b >= 0) {
          if (b == 3) finish(true);
          else {
            mode_ = Mode(b);
            hot_window_ = mode_ == WINDOW ? window_at(Fl::event_x(), Fl::event_y()) : -1;
            qa();
            redraw();
          }
          return 1;
        }
        x0_ = x1_ = Fl::event_x();
        y0_ = y1_ = Fl::event_y();
        dragging_ = mode_ == AREA;
        redraw();
        return 1;
      }
      case FL_DRAG:
        if (dragging_) {
          x1_ = Fl::event_x();
          y1_ = Fl::event_y();
          redraw();
        }
        return 1;
      case FL_RELEASE:
        if (button_at(Fl::event_x(), Fl::event_y()) >= 0 && !dragging_) return 1;
        if (mode_ == AREA) {
          dragging_ = false;
          Rect r = selection_from_drag();
          if (r.w >= 3 && r.h >= 3) {
            result = r;
            finish(false);
          }
          redraw();
        } else if (mode_ == WINDOW) {
          int k = window_at(Fl::event_x(), Fl::event_y());
          if (k >= 0) {
            result = windows_[size_t(k)];
            finish(false);
          }
        } else {
          result = {0, 0, w(), h()};
          finish(false);
        }
        return 1;
      case FL_KEYBOARD:
      case FL_SHORTCUT:
        switch (Fl::event_key()) {
          case FL_Escape:
            finish(true);
            return 1;
          case FL_Enter:
          case FL_KP_Enter:
            if (mode_ == SCREEN) {
              result = {0, 0, w(), h()};
              finish(false);
            }
            return 1;
          case FL_Left:
          case FL_Right: {
            int m = (int(mode_) + (Fl::event_key() == FL_Left ? 2 : 1)) % 3;
            mode_ = Mode(m);
            qa();
            redraw();
            return 1;
          }
          default:
            return 1;
        }
      default:
        return BufferedWindow::handle(e);
    }
  }

  void take_input() {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool kb = false, ptr = false;
    while (!(kb && ptr) && std::chrono::steady_clock::now() < end) {
      if (!kb) kb = XGrabKeyboard(fl_display, fl_xid(this), True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
      if (!ptr)
        ptr = XGrabPointer(fl_display, fl_xid(this), True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                           GrabModeAsync, GrabModeAsync, None, XCreateFontCursor(fl_display, 34 /* crosshair */),
                           CurrentTime) == GrabSuccess;
      if (!(kb && ptr)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    XSetInputFocus(fl_display, fl_xid(this), RevertToPointerRoot, CurrentTime);
    XSync(fl_display, False);
    Fl::focus(this);
    qa();
  }

  void qa() const {
    const char *dir = getenv("SDE_SCREENSHOT_QA_DIR");
    if (!dir || !*dir) return;
    static const char *names[] = {"area", "window", "screen", "close"};
    std::string text = std::string("mode:") + names[mode_] + "\n";
    for (int i = 0; i < 4; ++i) {
      Rect b = button(i);
      text += std::string("button:") + names[i] + "\t" + std::to_string(b.x) + "\t" + std::to_string(b.y) + "\t" +
              std::to_string(b.w) + "\t" + std::to_string(b.h) + "\n";
    }
    std::string path = std::string(dir) + "/screenshot.txt", tmp = path + ".tmp";
    if (FILE *f = fopen(tmp.c_str(), "w")) {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
      rename(tmp.c_str(), path.c_str());
    }
  }

 private:
  cairo_surface_t *shot_;
  std::vector<Rect> windows_;
  Mode mode_ = AREA;
  int hover_ = -1, hot_window_ = -1;
  bool dragging_ = false;
  int x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;

  Rect toolbar() const {
    int bw = S(132), bh = S(36), pad = S(6);
    int tw = 4 * bw + 5 * pad;
    return {(w() - tw) / 2, S(18), tw, bh + 2 * pad};
  }
  Rect button(int i) const {
    Rect bar = toolbar();
    int bw = S(132), pad = S(6);
    return {bar.x + pad + i * (bw + pad), bar.y + pad, bw, bar.h - 2 * pad};
  }
  int button_at(int x, int y) const {
    for (int i = 0; i < 4; ++i) {
      Rect b = button(i);
      if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
    }
    return -1;
  }
  int window_at(int x, int y) const {
    for (size_t i = 0; i < windows_.size(); ++i) {
      const Rect &r = windows_[i];
      if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return int(i);
    }
    return -1;
  }
  Rect selection_from_drag() const { return screenshot::from_corners(x0_, y0_, x1_, y1_, {0, 0, w(), h()}); }
  Rect selection() const {
    if (mode_ == AREA) return dragging_ ? selection_from_drag() : Rect{};
    if (mode_ == WINDOW) return hot_window_ >= 0 ? windows_[size_t(hot_window_)] : Rect{};
    return {0, 0, w(), h()};
  }
  void finish(bool cancel) {
    cancelled = cancel;
    done = true;
    XUngrabKeyboard(fl_display, CurrentTime);
    XUngrabPointer(fl_display, CurrentTime);
    hide();
  }
};

// ----------------------------------------------------------- clipboard

// Serves |png| as the CLIPBOARD (image/png, INCR for large pictures) and
// returns when another program takes it, or after 12 hours.
void serve_clipboard(const std::string &png, const std::string &path) {
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) return;
  auto A = [dpy](const char *n) { return XInternAtom(dpy, n, False); };
  Window owner = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, 0);
  XSelectInput(dpy, owner, PropertyChangeMask);
  // A timestamp for the selection (ICCCM): a property change on our window.
  XChangeProperty(dpy, owner, A("_SDE_SCREENSHOT"), XA_STRING, 8, PropModeReplace, nullptr, 0);
  XEvent ev;
  Time stamp = CurrentTime;
  for (int i = 0; i < 50; ++i) {
    if (XCheckTypedWindowEvent(dpy, owner, PropertyNotify, &ev)) {
      stamp = ev.xproperty.time;
      break;
    }
    usleep(10000);
  }
  XSetSelectionOwner(dpy, A("CLIPBOARD"), owner, stamp);
  if (XGetSelectionOwner(dpy, A("CLIPBOARD")) != owner) return;
  long max = XExtendedMaxRequestSize(dpy);
  if (!max) max = XMaxRequestSize(dpy);
  size_t chunk = size_t(std::min<long>(max * 4 - 1024, 256 * 1024));
  std::string uri = "file://" + path;
  struct Transfer {
    Window requestor;
    Atom property, target;
    size_t offset;
  };
  std::list<Transfer> transfers;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(12);
  int fd = ConnectionNumber(dpy);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!XPending(dpy)) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(fd, &set);
      timeval tv{60, 0};
      select(fd + 1, &set, nullptr, nullptr, &tv);
      continue;
    }
    XNextEvent(dpy, &ev);
    if (ev.type == SelectionClear) break;
    if (ev.type == SelectionRequest) {
      const XSelectionRequestEvent &r = ev.xselectionrequest;
      XSelectionEvent reply{};
      reply.type = SelectionNotify;
      reply.requestor = r.requestor;
      reply.selection = r.selection;
      reply.target = r.target;
      reply.time = r.time;
      reply.property = r.property ? r.property : r.target;
      if (r.target == A("TARGETS")) {
        std::vector<Atom> targets{A("TARGETS"), A("TIMESTAMP"), A("image/png")};
        if (!path.empty()) targets.push_back(A("text/uri-list"));
        XChangeProperty(dpy, r.requestor, reply.property, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char *>(targets.data()), int(targets.size()));
      } else if (r.target == A("TIMESTAMP")) {
        long t = long(stamp);
        XChangeProperty(dpy, r.requestor, reply.property, XA_INTEGER, 32, PropModeReplace,
                        reinterpret_cast<unsigned char *>(&t), 1);
      } else if (r.target == A("image/png")) {
        if (png.size() > chunk) {
          long size = long(png.size());
          XSelectInput(dpy, r.requestor, PropertyChangeMask);
          XChangeProperty(dpy, r.requestor, reply.property, A("INCR"), 32, PropModeReplace,
                          reinterpret_cast<unsigned char *>(&size), 1);
          transfers.push_back({r.requestor, reply.property, r.target, 0});
        } else {
          XChangeProperty(dpy, r.requestor, reply.property, r.target, 8, PropModeReplace,
                          reinterpret_cast<const unsigned char *>(png.data()), int(png.size()));
        }
      } else if (r.target == A("text/uri-list") && !path.empty()) {
        std::string list = uri + "\r\n";
        XChangeProperty(dpy, r.requestor, reply.property, r.target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(list.data()), int(list.size()));
      } else {
        reply.property = None;
      }
      XSendEvent(dpy, r.requestor, False, 0, reinterpret_cast<XEvent *>(&reply));
      XFlush(dpy);
    } else if (ev.type == PropertyNotify && ev.xproperty.state == PropertyDelete) {
      for (auto it = transfers.begin(); it != transfers.end(); ++it) {
        if (it->requestor != ev.xproperty.window || it->property != ev.xproperty.atom) continue;
        size_t n = std::min(chunk, png.size() - it->offset);
        XChangeProperty(dpy, it->requestor, it->property, it->target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(png.data() + it->offset), int(n));
        it->offset += n;
        if (n == 0) transfers.erase(it);  // the empty chunk ends it
        XFlush(dpy);
        break;
      }
    }
  }
  XCloseDisplay(dpy);
}

void notify(const std::string &summary, const std::string &body, const std::string &icon) {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return;
  DBusMessage *m = dbus::method("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                "org.freedesktop.Notifications", "Notify");
  {
    dbus::Writer w(m);
    w.str(_("Screenshot")).u32(0).str(icon).str(summary).str(body).strings({});
    { dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}"); }
    w.i32(6000);
  }
  if (DBusMessage *reply = bus->call(m, 1500)) dbus_message_unref(reply);
}

bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

AppInfo app_info() {
  AppInfo info;
  info.name = _("Screenshot");
  info.program = "sde-screenshot";
  info.version = SDE_VERSION;
  info.icon = "applets-screenshooter";
  info.description = _("Copies the screen, a window or a part of the screen and saves it in Pictures.");
  return info;
}

int usage(int status) {
  printf("%s", _("Usage: sde-screenshot [--screen | --window | --snip] [--delay SECONDS]\n"
                 "                      [--output FILE] [--no-save] [--no-clipboard]\n\n"
                 "  --screen        the whole screen (PrtScn, Win+PrtScn); the default\n"
                 "  --window        the active window (Alt+PrtScn)\n"
                 "  --snip          drag an area, or pick a window or the whole screen (Win+Shift+S)\n"
                 "  --output FILE   save the picture as FILE (PNG)\n"
                 "  --no-save       only copy it to the clipboard\n"
                 "  --no-clipboard  only save it\n"
                 "Pictures are saved in ~/Pictures/Screenshots.\n"));
  return status;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  enum { SCREEN, WINDOW, SNIP } mode = SCREEN;
  bool save = true, clip = true;
  double delay = 0;
  std::string output;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--screen") mode = SCREEN;
    else if (a == "--window") mode = WINDOW;
    else if (a == "--snip" || a == "--region") mode = SNIP;
    else if (a == "--no-save") save = false;
    else if (a == "--no-clipboard") clip = false;
    else if (a == "--delay" && i + 1 < argc) delay = atof(argv[++i]);
    else if (a == "--output" && i + 1 < argc) output = argv[++i];
    else if (a == "--version") return print_version(app_info());
    else if (a == "--help" || a == "-h") return usage(0);
    else return usage(2);
  }
  {
    const char *im = getenv("XMODIFIERS");
    std::string saved = im ? im : "";
    setenv("XMODIFIERS", "@im=none", 1);
    fl_open_display();
    if (im) setenv("XMODIFIERS", saved.c_str(), 1);
    else unsetenv("XMODIFIERS");
  }
  if (!fl_display) {
    fprintf(stderr, "sde-screenshot: cannot open the display\n");
    return 1;
  }
  XSetErrorHandler(ignore_x_errors);
  Config cfg;
  cfg.load();
  g_theme = Theme::from_config(cfg);
  g_theme.apply_to_fltk();
  if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(int(delay * 1000)));
  cairo_surface_t *shot = capture_screen();
  if (!shot) {
    fprintf(stderr, "sde-screenshot: cannot read the screen\n");
    return 1;
  }
  Rect part = screen_rect();
  if (mode == WINDOW) {
    Rect r = frame_rect(x11::get_window(root(), "_NET_ACTIVE_WINDOW"));
    if (!r.empty()) part = r;
  } else if (mode == SNIP) {
    auto *snip = new Snip(DisplayWidth(fl_display, fl_screen), DisplayHeight(fl_display, fl_screen), shot);
    snip->end();
    snip->border(0);
    snip->set_override();
    snip->show();
    snip->take_input();
    while (!snip->done && Fl::first_window()) Fl::wait(0.5);
    if (snip->cancelled || snip->result.empty()) return 1;
    part = snip->result;
    Fl::flush();
    XSync(fl_display, False);
  }
  cairo_surface_t *picture = crop(shot, part);
  std::string png = png_of(picture);
  std::string path;
  if (!output.empty()) {
    path = output;
  } else if (save) {
    // ~/Pictures unless user-dirs.dirs names another folder.
    std::string pictures = user_dir("PICTURES");
    if (pictures == home_dir()) pictures = home_dir() + "/Pictures";
    std::string dir = screenshot::folder(pictures);
    make_dirs(dir);
    path = dir + "/" + screenshot::file_name(time(nullptr), exists, dir);
  }
  bool saved = false;
  if (!path.empty()) {
    saved = write_file_atomic(path, png);
    if (!saved) fprintf(stderr, "sde-screenshot: cannot write %s\n", path.c_str());
  }
  std::string body;
  if (clip && saved) body = format(_("Copied to the clipboard and saved as %s."), basename_of(path).c_str());
  else if (clip) body = _("Copied to the clipboard.");
  else if (saved) body = format(_("Saved as %s."), path.c_str());
  if (!body.empty())
    notify(format(_("Screenshot %d × %d"), part.w, part.h), body, saved ? "file://" + path : "applets-screenshooter");
  printf("%s\n", saved ? path.c_str() : "");
  fflush(stdout);
  if (!clip) return saved ? 0 : 1;
  // The clipboard stays with a process of its own (on a connection of its
  // own), so that the shortcut's command returns at once.
  XSync(fl_display, False);
  pid_t pid = fork();
  if (pid < 0) return 1;
  if (pid > 0) return 0;
  close(ConnectionNumber(fl_display));
  setsid();
  signal(SIGHUP, SIG_IGN);
  if (FILE *f = freopen("/dev/null", "w", stdout)) (void)f;
  serve_clipboard(png, saved ? path : "");
  _exit(0);
}
