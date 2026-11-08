// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// sde-taskview: Task View (0.2.0 Alpha 4, Win+Tab; docs/WINDOWS.md).
//
// Over a blurred picture of the screen: the windows of a workspace as large
// cards (a live picture when SDE's compositor keeps one, or the window as
// it is on the screen; the icon and title for windows that are not on the
// screen: minimized, or on another workspace), and the workspaces in a row
// at the bottom with a card to add one (up to 5).
//
//   click a window      switch to it (and to its workspace)
//   drag a window       onto a workspace (or "New workspace"): move it there
//   right click         the window menu (window_menu.h)
//   × on a card         close the window, or the workspace
//   point at workspace  show its windows; click it to go there
//   keys                arrows and Tab choose, Enter opens, Delete closes,
//                       Ctrl+Left/Right or Page Up/Down another workspace,
//                       Ctrl+Win+D a new one, Esc or Win+Tab leave
//
// A second sde-taskview closes the open one (Win+Tab toggles).
// SDE_TASKVIEW_QA_DIR: the cards are written to <dir>/taskview.txt.
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo-xlib.h>
#ifdef SDE_HAVE_XCOMPOSITE
#include <X11/extensions/Xcomposite.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "about.h"
#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "integration.h"
#include "menu.h"
#include "paint.h"
#include "spawn.h"
#include "taskview_model.h"
#include "theme.h"
#include "util.h"
#include "window_menu.h"
#include "workspaces.h"
#include "x11.h"

using namespace sde;
using taskview::Box;

namespace {

Theme g_theme;

int ignore_x_errors(Display *, XErrorEvent *) { return 0; }

Window root() { return RootWindow(fl_display, fl_screen); }

bool compositor_running() {
  char name[32];
  snprintf(name, sizeof name, "_NET_WM_CM_S%d", fl_screen);
  return XGetSelectionOwner(fl_display, XInternAtom(fl_display, name, False)) != None;
}

struct Item {
  Window win = 0;
  long desktop = 0;  // -1: every workspace
  std::string title;
  bool minimized = false;
  int x = 0, y = 0, w = 1, h = 1;  // the frame on the screen
  cairo_surface_t *icon = nullptr;
  cairo_surface_t *picture = nullptr;  // what the window shows, when known
};

// The frame of |w| on the screen.
void frame_of(Window w, Item &it) {
  Window rt, child;
  int x, y;
  unsigned cw, ch, bw, depth;
  if (!XGetGeometry(fl_display, w, &rt, &x, &y, &cw, &ch, &bw, &depth)) return;
  int rx = 0, ry = 0;
  XTranslateCoordinates(fl_display, w, root(), 0, 0, &rx, &ry, &child);
  auto e = x11::get_cardinals(w, "_NET_FRAME_EXTENTS");
  long l = e.size() >= 4 ? long(e[0]) : 0, r = e.size() >= 4 ? long(e[1]) : 0;
  long t = e.size() >= 4 ? long(e[2]) : 0, b = e.size() >= 4 ? long(e[3]) : 0;
  it.x = rx - int(l);
  it.y = ry - int(t);
  it.w = std::max(1, int(cw) + int(l + r));
  it.h = std::max(1, int(ch) + int(t + b));
}

#ifdef SDE_HAVE_XCOMPOSITE
Window toplevel(Window w) {
  Window rt = 0, parent = 0, *children = nullptr;
  unsigned n = 0;
  Window cur = w;
  for (int depth = 0; depth < 16; ++depth) {
    if (!XQueryTree(fl_display, cur, &rt, &parent, &children, &n)) return 0;
    if (children) XFree(children);
    if (parent == rt || !parent) return cur;
    cur = parent;
  }
  return 0;
}
#endif

// |src| (sw x sh) scaled down to at most |w| x |h|, as an image surface.
cairo_surface_t *scaled(cairo_surface_t *src, int sx, int sy, int sw, int sh, int w, int h) {
  double k = std::min(double(w) / sw, double(h) / sh);
  k = std::min(k, 1.0);
  int ow = std::max(1, int(sw * k)), oh = std::max(1, int(sh * k));
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, ow, oh);
  cairo_t *cr = cairo_create(out);
  cairo_scale(cr, k, k);
  cairo_set_source_surface(cr, src, -sx, -sy);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(out);
  return out;
}

// The whole screen as it is now (before Task View covers it).
cairo_surface_t *screen_picture() {
  int w = DisplayWidth(fl_display, fl_screen), h = DisplayHeight(fl_display, fl_screen);
  XImage *img = XGetImage(fl_display, root(), 0, 0, unsigned(w), unsigned(h), AllPlanes, ZPixmap);
  if (!img) return nullptr;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < h; ++y) {
    uint32_t *row = reinterpret_cast<uint32_t *>(data + y * stride);
    for (int x = 0; x < w; ++x) row[x] = uint32_t(XGetPixel(img, x, y)) & 0xFFFFFF;
  }
  XDestroyImage(img);
  cairo_surface_mark_dirty(s);
  return s;
}

// A soft, dimmed copy of the screen for the background (scaled down and up:
// a cheap blur).
cairo_surface_t *backdrop_of(cairo_surface_t *screen, int sx, int sy, int w, int h) {
  if (!screen || w <= 0 || h <= 0) return nullptr;
  int tw = std::max(1, w / 24), th = std::max(1, h / 24);
  cairo_surface_t *tiny = cairo_image_surface_create(CAIRO_FORMAT_RGB24, tw, th);
  cairo_t *cr = cairo_create(tiny);
  cairo_scale(cr, double(tw) / w, double(th) / h);
  // The monitor's part of the screen.
  cairo_set_source_surface(cr, screen, -sx, -sy);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cr = cairo_create(out);
  cairo_scale(cr, double(w) / tw, double(h) / th);
  cairo_set_source_surface(cr, tiny, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(tiny);
  cairo_surface_flush(out);
  return out;
}

// A picture of |it| for a card of |w| x |h|: the compositor's copy of its
// frame, or the screen where it is.
cairo_surface_t *picture_of(const Item &it, cairo_surface_t *screen, bool composited, int w, int h) {
  if (it.minimized) return nullptr;
#ifdef SDE_HAVE_XCOMPOSITE
  if (composited) {
    Window frame = toplevel(it.win);
    XWindowAttributes fa;
    if (frame && XGetWindowAttributes(fl_display, frame, &fa) && fa.map_state == IsViewable) {
      XSync(fl_display, False);
      Pixmap pm = XCompositeNameWindowPixmap(fl_display, frame);
      XSync(fl_display, False);
      if (pm) {
        int fw = fa.width + 2 * fa.border_width, fh = fa.height + 2 * fa.border_width;
        cairo_surface_t *src = cairo_xlib_surface_create(fl_display, pm, fa.visual, fw, fh);
        cairo_surface_t *out = scaled(src, 0, 0, fw, fh, w, h);
        cairo_surface_destroy(src);
        XFreePixmap(fl_display, pm);
        return out;
      }
    }
  }
#else
  (void)composited;
#endif
  if (!screen) return nullptr;
  int sw = cairo_image_surface_get_width(screen), sh = cairo_image_surface_get_height(screen);
  int x0 = std::max(0, it.x), y0 = std::max(0, it.y);
  int x1 = std::min(sw, it.x + it.w), y1 = std::min(sh, it.y + it.h);
  if (x1 - x0 < 8 || y1 - y0 < 8) return nullptr;
  return scaled(screen, x0, y0, x1 - x0, y1 - y0, w, h);
}

class TaskView : public BufferedWindow {
 public:
  TaskView(int x, int y, int w, int h) : BufferedWindow(x, y, w, h, "Task View") {}

  cairo_surface_t *screen = nullptr;  // the screen before Task View
  cairo_surface_t *backdrop = nullptr;
  bool composited = false;

  void load() {
    for (auto &it : items_) {
      if (it.icon) cairo_surface_destroy(it.icon);
      if (it.picture) cairo_surface_destroy(it.picture);
    }
    items_.clear();
    auto live = workspaces::read_live(fl_display);
    Config cfg;
    cfg.load();
    auto ws = workspaces::Settings::from_config(cfg);
    count_ = std::max(1, live.count);
    current_ = std::min(std::max(0, live.current), count_ - 1);
    names_.clear();
    for (int i = 0; i < count_; ++i) {
      std::string n = i < int(live.names.size()) ? trim(live.names[size_t(i)]) : "";
      names_.push_back(n.empty() ? ws.name(i) : n);
    }
    if (desk_ < 0 || desk_ >= count_) desk_ = current_;
    // Top of the stack first: the window in use leads.
    auto stack = x11::get_windows(root(), "_NET_CLIENT_LIST_STACKING");
    std::reverse(stack.begin(), stack.end());
    for (Window w : stack) {
      if (w == fl_xid(this) || !x11::window_is_normal(w) ||
          x11::window_has_state(w, "_NET_WM_STATE_SKIP_TASKBAR") || x11::window_has_state(w, "_NET_WM_STATE_SKIP_PAGER"))
        continue;
      Item it;
      it.win = w;
      auto d = x11::get_cardinals(w, "_NET_WM_DESKTOP");
      it.desktop = d.empty() ? 0 : d[0] == 0xFFFFFFFFul ? -1 : long(d[0]);
      it.title = x11::window_title(w);
      if (it.title.empty()) it.title = x11::window_class(w);
      it.minimized = x11::window_has_state(w, "_NET_WM_STATE_HIDDEN");
      frame_of(w, it);
      it.icon = x11::window_icon(w, S(64));
      items_.push_back(it);
    }
    active_ = x11::get_window(root(), "_NET_ACTIVE_WINDOW");
  }

  // The cards of workspace |desk_|.
  void rebuild() {
    shown_.clear();
    for (size_t i = 0; i < items_.size(); ++i)
      if (items_[i].desktop == desk_ || items_[i].desktop < 0) shown_.push_back(int(i));
    std::vector<double> aspects;
    for (int i : shown_) aspects.push_back(double(items_[size_t(i)].w) / std::max(1, items_[size_t(i)].h));
    layout_ = taskview::layout(w(), h(), count_, count_ < workspaces::kMax, aspects, g_theme.scale);
    // Pictures: windows on the screen now (the workspace in view).
    for (size_t k = 0; k < shown_.size(); ++k) {
      Item &it = items_[size_t(shown_[k])];
      if (it.picture || desk_ != current_ || !picture_window(it)) continue;
      Box b = picture_box(int(k));
      it.picture = picture_of(it, screen, composited, b.w, b.h);
    }
    if (selected_ >= int(shown_.size())) selected_ = int(shown_.size()) - 1;
    if (selected_ < 0 && !shown_.empty()) {
      selected_ = 0;
      for (size_t k = 0; k < shown_.size(); ++k)
        if (items_[size_t(shown_[k])].win == active_) selected_ = int(k);
    }
    qa_dump();
    redraw();
  }

  void reload_soon(double delay = 0.2) {
    Fl::remove_timeout(reload_cb, this);
    Fl::add_timeout(delay, reload_cb, this);
  }

  void take_input() {
    // SDE WM holds the keyboard until Tab is let go (Win+Tab): wait for it.
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool kb = false, ptr = false;
    while (!(kb && ptr) && std::chrono::steady_clock::now() < end) {
      if (!kb) kb = XGrabKeyboard(fl_display, fl_xid(this), True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
      if (!ptr)
        ptr = XGrabPointer(fl_display, fl_xid(this), True,
                           ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask,
                           GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess;
      if (!(kb && ptr)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    XSetInputFocus(fl_display, fl_xid(this), RevertToPointerRoot, CurrentTime);
    XSync(fl_display, False);
    Fl::focus(this);
  }

  void leave() {
    XUngrabKeyboard(fl_display, CurrentTime);
    XUngrabPointer(fl_display, CurrentTime);
    XSync(fl_display, False);
    hide();
    exit(0);
  }

  void draw() override {
    const Theme &t = g_theme;
    Color ink = t.dark ? Color::hex(0xFFFFFF) : Color::hex(0x111111);
    {
      Painter p(w(), h());
      cairo_t *cr = p.cr();
      if (backdrop) {
        cairo_set_source_surface(cr, backdrop, 0, 0);
        cairo_paint(cr);
      } else {
        p.fill_rect(0, 0, w(), h(), t.background);
      }
      p.fill_rect(0, 0, w(), h(), t.dark ? Color::hex(0x101114, 0.55) : Color::hex(0xF3F4F6, 0.55));
      // The window cards.
      for (size_t k = 0; k < shown_.size() && k < layout_.windows.size(); ++k) draw_card(p, int(k));
      // The workspaces.
      for (int d = 0; d < count_ && d < int(layout_.desks.size()); ++d) draw_desk(p, d);
      if (layout_.add.w > 0) {
        const Box &b = layout_.add;
        bool hot = hover_add_ || drop_desk_ == count_;
        p.fill_rounded(b.x, b.y, b.w, b.h - S(26), t.radius, hot ? t.surface.alpha(0.95) : t.surface.alpha(0.6));
        p.stroke_rounded(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - S(26) - 1, t.radius, hot ? t.accent : t.border, Sd(1));
        double cx = b.x + b.w / 2.0, cy = b.y + (b.h - S(26)) / 2.0, r = S(14);
        p.line(cx - r, cy, cx + r, cy, hot ? t.accent : ink.alpha(0.75), Sd(2.5));
        p.line(cx, cy - r, cx, cy + r, hot ? t.accent : ink.alpha(0.75), Sd(2.5));
      }
      if (dragging_ >= 0) {
        const Item &it = items_[size_t(dragging_)];
        int dw = S(150), dh = S(96);
        int dx = drag_x_ - dw / 2, dy = drag_y_ - dh / 2;
        p.shadow(dx, dy, dw, dh, t.radius, t.shadow, S(10));
        p.fill_rounded(dx, dy, dw, dh, t.radius, t.surface);
        if (it.picture) {
          Box b = taskview::fit({dx + S(6), dy + S(6), dw - S(12), dh - S(12)},
                                double(cairo_image_surface_get_width(it.picture)) /
                                    std::max(1, cairo_image_surface_get_height(it.picture)));
          paint_picture(cr, it.picture, b, S(4));
        } else if (it.icon) {
          p.image(it.icon, dx + (dw - S(48)) / 2.0, dy + (dh - S(48)) / 2.0, S(48));
        }
      }
    }
    // Text after the cairo drawing.
    text_font(t.font_size + 6, true);
    draw_text(desk_ < int(names_.size()) ? names_[size_t(desk_)] : "", 0, S(10), w(), S(34), ink, 1);
    if (shown_.empty()) {
      text_font(t.font_size + 1);
      draw_text(_("No windows on this workspace"), 0, layout_.grid.y + layout_.grid.h / 2 - S(12), w(), S(24),
                ink.alpha(0.8), 1);
    }
    for (size_t k = 0; k < shown_.size() && k < layout_.windows.size(); ++k) {
      const Box &b = layout_.windows[k];
      const Item &it = items_[size_t(shown_[k])];
      bool hot = int(k) == hover_ || int(k) == selected_;
      text_font(t.font_size, hot);
      int right = hot ? S(30) : S(10);
      draw_text(it.title, b.x + S(38), b.y + S(5), b.w - S(38) - right, S(24), t.text, 0);
    }
    for (int d = 0; d < count_ && d < int(layout_.desks.size()); ++d) {
      const Box &b = layout_.desks[size_t(d)];
      text_font(t.font_size - 1, d == current_);
      draw_text(names_[size_t(d)], b.x, b.y + b.h - S(24), b.w, S(22), ink, 1);
    }
    if (layout_.add.w > 0) {
      const Box &b = layout_.add;
      text_font(t.font_size - 1);
      draw_text(_("New workspace"), b.x, b.y + b.h - S(24), b.w, S(22), ink, 1);
    }
  }

  int handle(int e) override {
    switch (e) {
      case FL_FOCUS:
      case FL_UNFOCUS:
        return 1;
      case FL_MOVE:
      case FL_ENTER:
        hover(Fl::event_x(), Fl::event_y());
        return 1;
      case FL_PUSH:
        press_x_ = Fl::event_x();
        press_y_ = Fl::event_y();
        pressed_ = card_at(press_x_, press_y_);
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          context_menu(press_x_, press_y_);
          pressed_ = -1;
        }
        return 1;
      case FL_DRAG:
        if (pressed_ >= 0 && Fl::event_button1()) {
          if (dragging_ < 0 && (std::abs(Fl::event_x() - press_x_) > S(8) || std::abs(Fl::event_y() - press_y_) > S(8)))
            dragging_ = shown_[size_t(pressed_)];
          if (dragging_ >= 0) {
            drag_x_ = Fl::event_x();
            drag_y_ = Fl::event_y();
            drop_desk_ = desk_at(drag_x_, drag_y_);
            redraw();
          }
        }
        return 1;
      case FL_RELEASE:
        release(Fl::event_x(), Fl::event_y(), Fl::event_button());
        return 1;
      case FL_KEYBOARD:
      case FL_SHORTCUT:
        return key(Fl::event_key());
      default:
        return BufferedWindow::handle(e);
    }
  }

  void qa_dump() const {
    const char *dir = getenv("SDE_TASKVIEW_QA_DIR");
    if (!dir || !*dir) return;
    std::string text = "shown:" + std::to_string(desk_ + 1) + "\ncurrent:" + std::to_string(current_ + 1) + "\n";
    char line[512];
    for (int d = 0; d < count_ && d < int(layout_.desks.size()); ++d) {
      const Box &b = layout_.desks[size_t(d)];
      snprintf(line, sizeof line, "workspace:%d:%s\t%d\t%d\t%d\t%d\n", d + 1, names_[size_t(d)].c_str(), x() + b.x,
               y() + b.y, b.w, b.h);
      text += line;
    }
    if (layout_.add.w > 0) {
      snprintf(line, sizeof line, "new\t%d\t%d\t%d\t%d\n", x() + layout_.add.x, y() + layout_.add.y, layout_.add.w,
               layout_.add.h);
      text += line;
    }
    for (size_t k = 0; k < shown_.size() && k < layout_.windows.size(); ++k) {
      const Item &it = items_[size_t(shown_[k])];
      const Box &b = layout_.windows[k];
      snprintf(line, sizeof line, "window:0x%lx:%s\t%d\t%d\t%d\t%d\t%s%s\n", it.win, it.title.c_str(), x() + b.x,
               y() + b.y, b.w, b.h, it.picture ? "picture" : "icon", int(k) == selected_ ? "\tselected" : "");
      text += line;
    }
    std::string path = std::string(dir) + "/taskview.txt", tmp = path + ".tmp";
    if (FILE *f = fopen(tmp.c_str(), "w")) {
      fwrite(text.data(), 1, text.size(), f);
      fclose(f);
      rename(tmp.c_str(), path.c_str());
    }
  }

 private:
  std::vector<Item> items_;
  std::vector<int> shown_;  // indices into items_ of the cards, in order
  std::vector<std::string> names_;
  taskview::Layout layout_;
  int count_ = 1, current_ = 0, desk_ = -1;
  Window active_ = 0;
  int selected_ = -1, hover_ = -1, hover_desk_ = -1, pressed_ = -1, dragging_ = -1, drop_desk_ = -1;
  bool hover_add_ = false, hover_close_ = false, hover_desk_close_ = false;
  int press_x_ = 0, press_y_ = 0, drag_x_ = 0, drag_y_ = 0;
  int preview_desk_ = -1;

  static void reload_cb(void *data) {
    auto *self = static_cast<TaskView *>(data);
    self->load();
    self->rebuild();
  }

  static bool picture_window(const Item &it) { return !it.minimized; }

  Box picture_box(int k) const {
    const Box &b = layout_.windows[size_t(k)];
    return {b.x + S(8), b.y + S(34), b.w - S(16), b.h - S(42)};
  }

  Box close_box(int k) const {
    const Box &b = layout_.windows[size_t(k)];
    return {b.x + b.w - S(30), b.y + S(5), S(24), S(24)};
  }

  Box desk_close_box(int d) const {
    const Box &b = layout_.desks[size_t(d)];
    return {b.x + b.w - S(26), b.y + S(4), S(22), S(22)};
  }

  static void paint_picture(cairo_t *cr, cairo_surface_t *pic, const Box &b, double r) {
    int pw = cairo_image_surface_get_width(pic), ph = cairo_image_surface_get_height(pic);
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, b.x + b.w - r, b.y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, b.x + b.w - r, b.y + b.h - r, r, 0, M_PI / 2);
    cairo_arc(cr, b.x + r, b.y + b.h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, b.x + r, b.y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_clip(cr);
    cairo_translate(cr, b.x, b.y);
    cairo_scale(cr, double(b.w) / pw, double(b.h) / ph);
    cairo_set_source_surface(cr, pic, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
  }

  void draw_card(Painter &p, int k) {
    const Theme &t = g_theme;
    const Box &b = layout_.windows[size_t(k)];
    const Item &it = items_[size_t(shown_[size_t(k)])];
    bool hot = k == hover_, sel = k == selected_;
    if (shown_[size_t(k)] == dragging_) return;
    p.shadow(b.x, b.y, b.w, b.h, t.radius_large, t.shadow, S(12));
    p.fill_rounded(b.x, b.y, b.w, b.h, t.radius_large, hot ? t.surface : t.surface.alpha(0.94));
    if (sel || hot)
      p.stroke_rounded(b.x + 1, b.y + 1, b.w - 2, b.h - 2, t.radius_large, sel ? t.accent : t.border, sel ? Sd(2.5) : Sd(1));
    if (it.icon) p.image(it.icon, b.x + S(10), b.y + S(7), S(20));
    Box pb = picture_box(k);
    if (it.picture) {
      Box fitted = taskview::fit(pb, double(cairo_image_surface_get_width(it.picture)) /
                                         std::max(1, cairo_image_surface_get_height(it.picture)));
      paint_picture(p.cr(), it.picture, fitted, t.radius_small);
    } else {
      p.fill_rounded(pb.x, pb.y, pb.w, pb.h, t.radius_small, t.surface_alt);
      int is = std::min(S(64), std::min(pb.w, pb.h) - S(8));
      if (it.icon && is > 8) {
        p.image(it.icon, pb.x + (pb.w - is) / 2.0, pb.y + (pb.h - is) / 2.0, is);
      } else if (is > 8) {
        // No icon: a window outline.
        double gx = pb.x + (pb.w - is) / 2.0, gy = pb.y + (pb.h - is) / 2.0;
        p.stroke_rounded(gx, gy + is * 0.15, is, is * 0.7, is * 0.1, t.text_dim, Sd(2));
        p.line(gx, gy + is * 0.32, gx + is, gy + is * 0.32, t.text_dim, Sd(2));
      }
      if (it.minimized) {
        // A small "minimized" bar under the icon.
        p.fill_rounded(pb.x + pb.w / 2.0 - S(14), pb.y + pb.h - S(16), S(28), S(4), S(2), t.text_dim);
      }
    }
    if (hot || sel) {
      Box c = close_box(k);
      bool hc = hot && hover_close_;
      p.fill_rounded(c.x, c.y, c.w, c.h, t.radius_small, hc ? t.caption_close : t.hover);
      Color g = hc ? Color::hex(0xFFFFFF) : t.text;
      p.line(c.x + S(8), c.y + S(8), c.x + c.w - S(8), c.y + c.h - S(8), g, Sd(1.5));
      p.line(c.x + c.w - S(8), c.y + S(8), c.x + S(8), c.y + c.h - S(8), g, Sd(1.5));
    }
  }

  void draw_desk(Painter &p, int d) {
    const Theme &t = g_theme;
    const Box &b = layout_.desks[size_t(d)];
    Box screen{b.x, b.y, b.w, b.h - S(26)};
    bool hot = d == hover_desk_ || d == drop_desk_, shown = d == desk_, cur = d == current_;
    p.fill_rounded(screen.x, screen.y, screen.w, screen.h, t.radius,
                   hot ? t.surface : shown ? t.surface.alpha(0.92) : t.surface.alpha(0.7));
    if (cur || hot || shown)
      p.stroke_rounded(screen.x + 1, screen.y + 1, screen.w - 2, screen.h - 2, t.radius,
                       cur ? t.accent : t.border, cur ? Sd(2.5) : Sd(1));
    // Its windows, small, where they are on the screen.
    double kx = double(screen.w - S(12)) / std::max(1, w()), ky = double(screen.h - S(12)) / std::max(1, h());
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
      if ((it->desktop != d && it->desktop >= 0) || it->minimized) continue;
      double wx = screen.x + S(6) + (it->x - x()) * kx, wy = screen.y + S(6) + (it->y - y()) * ky;
      double ww = std::max(6.0, it->w * kx), wh = std::max(5.0, it->h * ky);
      wx = std::max(double(screen.x + S(4)), std::min(wx, double(screen.x + screen.w - S(4)) - ww));
      wy = std::max(double(screen.y + S(4)), std::min(wy, double(screen.y + screen.h - S(4)) - wh));
      p.fill_rounded(wx, wy, ww, wh, S(3), t.surface_alt);
      p.stroke_rounded(wx + 0.5, wy + 0.5, ww - 1, wh - 1, S(3), t.border, Sd(1));
      if (it->icon && ww > S(22) && wh > S(22)) {
        double is = std::min(S(20) * 1.0, std::min(ww, wh) - S(6));
        p.image(it->icon, wx + (ww - is) / 2, wy + (wh - is) / 2, is);
      }
    }
    if (d == hover_desk_ && count_ > 1) {
      Box c = desk_close_box(d);
      bool hc = hover_desk_close_;
      p.fill_rounded(c.x, c.y, c.w, c.h, t.radius_small, hc ? t.caption_close : t.hover);
      Color g = hc ? Color::hex(0xFFFFFF) : t.text;
      p.line(c.x + S(7), c.y + S(7), c.x + c.w - S(7), c.y + c.h - S(7), g, Sd(1.4));
      p.line(c.x + c.w - S(7), c.y + S(7), c.x + S(7), c.y + c.h - S(7), g, Sd(1.4));
    }
  }

  int card_at(int px, int py) const {
    for (size_t k = 0; k < shown_.size() && k < layout_.windows.size(); ++k)
      if (layout_.windows[k].contains(px, py)) return int(k);
    return -1;
  }

  // A workspace card, count_ for "New workspace", -1 for none.
  int desk_at(int px, int py) const {
    for (int d = 0; d < count_ && d < int(layout_.desks.size()); ++d)
      if (layout_.desks[size_t(d)].contains(px, py)) return d;
    if (layout_.add.contains(px, py)) return count_;
    return -1;
  }

  void hover(int px, int py) {
    int k = card_at(px, py);
    int d = desk_at(px, py);
    bool hc = k >= 0 && close_box(k).contains(px, py);
    bool hdc = d >= 0 && d < count_ && desk_close_box(d).contains(px, py);
    int hd = d >= 0 && d < count_ ? d : -1;
    bool ha = d == count_;
    if (k != hover_ || hd != hover_desk_ || hc != hover_close_ || hdc != hover_desk_close_ || ha != hover_add_) {
      hover_ = k;
      hover_desk_ = hd;
      hover_close_ = hc;
      hover_desk_close_ = hdc;
      hover_add_ = ha;
      redraw();
    }
    // Pointing at a workspace for a moment shows its windows.
    if (hd >= 0 && hd != desk_) {
      if (preview_desk_ != hd) {
        preview_desk_ = hd;
        Fl::remove_timeout(preview_cb, this);
        Fl::add_timeout(0.25, preview_cb, this);
      }
    } else {
      preview_desk_ = -1;
      Fl::remove_timeout(preview_cb, this);
    }
  }

  static void preview_cb(void *data) {
    auto *self = static_cast<TaskView *>(data);
    if (self->preview_desk_ >= 0 && self->preview_desk_ == self->hover_desk_) self->show_desk(self->preview_desk_);
  }

  void show_desk(int d) {
    if (d < 0 || d >= count_) return;
    desk_ = d;
    selected_ = -1;
    hover_ = -1;
    rebuild();
  }

  void activate(int k) {
    if (k < 0 || k >= int(shown_.size())) return;
    const Item &it = items_[size_t(shown_[size_t(k)])];
    if (it.desktop >= 0 && it.desktop != current_) workspaces::switch_to(fl_display, int(it.desktop));
    x11::activate_window(it.win);
    XSync(fl_display, False);
    leave();
  }

  void close_window(int k) {
    if (k < 0 || k >= int(shown_.size())) return;
    x11::close_window(items_[size_t(shown_[size_t(k)])].win);
    reload_soon(0.4);
  }

  void go_to_desk(int d) {
    if (d < 0 || d >= count_) return;
    workspaces::switch_to(fl_display, d);
    XSync(fl_display, False);
    leave();
  }

  void new_desk(Window move = 0) {
    Config cfg;
    cfg.load();
    int before = count_;
    if (!workspaces::add(fl_display, cfg)) return;
    if (move) workspaces::move_window(fl_display, move, before);
    // add() shows the new workspace; Task View stays where it was.
    workspaces::switch_to(fl_display, current_);
    cfg.save();
    announce_config_change(fl_display, cfg);
    desk_ = before;
    reload_soon();
  }

  void close_desk(int d) {
    if (count_ <= 1) return;
    Config cfg;
    cfg.load();
    if (!workspaces::close(fl_display, cfg, d)) return;
    cfg.save();
    announce_config_change(fl_display, cfg);
    desk_ = std::max(0, d - 1);
    reload_soon(0.3);
  }

  void release(int px, int py, int button) {
    if (dragging_ >= 0) {
      Window w = items_[size_t(dragging_)].win;
      long from = items_[size_t(dragging_)].desktop;
      int d = desk_at(px, py);
      dragging_ = -1;
      drop_desk_ = -1;
      pressed_ = -1;
      if (d == count_) new_desk(w);
      else if (d >= 0 && d != from) {
        workspaces::move_window(fl_display, w, d);
        reload_soon();
      }
      redraw();
      return;
    }
    int k = card_at(px, py);
    if (button == FL_MIDDLE_MOUSE) {
      if (k >= 0) close_window(k);
      return;
    }
    if (button != FL_LEFT_MOUSE) return;
    if (k >= 0 && k == pressed_) {
      if (close_box(k).contains(px, py)) close_window(k);
      else activate(k);
      return;
    }
    pressed_ = -1;
    int d = desk_at(px, py);
    if (d == count_) {
      new_desk();
    } else if (d >= 0) {
      if (count_ > 1 && desk_close_box(d).contains(px, py)) close_desk(d);
      else go_to_desk(d);
    } else if (k < 0) {
      leave();  // the empty background: back to where one was
    }
  }

  void context_menu(int px, int py) {
    int k = card_at(px, py), d = desk_at(px, py);
    ContextMenu m;
    if (k >= 0) {
      window_menu::add_items(m, window_menu::read(items_[size_t(shown_[size_t(k)])].win), {false});
    } else if (d >= 0 && d < count_) {
      m.item(_("Show This Workspace"), [this, d] { go_to_desk(d); });
      m.item(_("Close Workspace"), [this, d] { close_desk(d); }, {"close", 0, count_ > 1, true});
      m.item(_("Workspace Settings..."), [] { spawn_detached({"sde-settings", "--page", "workspaces"}); }, {"settings"});
    } else {
      return;
    }
    m.popup(px, py);
    // The menu took the input and gave it back to nobody: take it again.
    take_input();
    reload_soon();
  }

  int key(int k) {
    int n = int(shown_.size());
    bool ctrl = Fl::event_ctrl(), meta = Fl::event_state(FL_META);
    switch (k) {
      case FL_Escape:
        leave();
        return 1;
      case FL_Tab:
        if (meta) {
          leave();  // Win+Tab again
          return 1;
        }
        select(taskview::move_selection(selected_, n, layout_.columns, Fl::event_shift() ? -1 : 1, 0));
        return 1;
      case FL_Left:
      case FL_Right:
        if (ctrl) {
          show_desk((desk_ + (k == FL_Left ? -1 : 1) + count_) % count_);
          return 1;
        }
        select(taskview::move_selection(selected_, n, layout_.columns, k == FL_Left ? -1 : 1, 0));
        return 1;
      case FL_Up:
      case FL_Down:
        select(taskview::move_selection(selected_, n, layout_.columns, 0, k == FL_Up ? -1 : 1));
        return 1;
      case FL_Page_Up:
      case FL_Page_Down:
        show_desk((desk_ + (k == FL_Page_Up ? -1 : 1) + count_) % count_);
        return 1;
      case FL_Enter:
      case FL_KP_Enter:
      case ' ':
        if (n == 0) go_to_desk(desk_);
        else activate(selected_);
        return 1;
      case FL_Delete:
        close_window(selected_);
        return 1;
      case FL_F + 4:
        if (ctrl && meta) close_desk(desk_);
        else if (Fl::event_alt()) leave();
        return 1;
      case 'w':
        if (ctrl) close_window(selected_);
        return 1;
      case 'd':
        if (ctrl && meta) new_desk();
        return 1;
      default:
        if (k >= '1' && k <= '0' + workspaces::kMax && !ctrl) {
          show_desk(k - '1');
          return 1;
        }
        return 0;
    }
  }

  void select(int k) {
    if (k == selected_) return;
    selected_ = k;
    qa_dump();
    redraw();
  }
};

TaskView *g_view = nullptr;

// Only one Task View: a second sde-taskview closes the open one.
Atom selection_atom() {
  return XInternAtom(fl_display, ("_SDE_TASKVIEW_S" + std::to_string(fl_screen)).c_str(), False);
}

int x_events(int) {
  if (!fl_xevent || !g_view) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == ClientMessage && e.xclient.message_type == XInternAtom(fl_display, "_SDE_TASKVIEW_CLOSE", False)) {
    g_view->leave();
    return 1;
  }
  if (e.type == SelectionClear && e.xselectionclear.selection == selection_atom()) {
    g_view->leave();
    return 1;
  }
  return 0;
}

AppInfo app_info() {
  AppInfo info;
  info.name = _("Task View");
  info.program = "sde-taskview";
  info.version = SDE_VERSION;
  info.icon = "preferences-system-windows";
  info.description = _("Every workspace and its windows: switch to them or move windows (Win+Tab).");
  return info;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) return print_version(app_info());
    printf("%s", _("Usage: sde-taskview [--version]\n\n"
                   "Task View (Win+Tab): the workspaces and their windows. Run it again to close it.\n"));
    return strcmp(argv[i], "--help") ? 2 : 0;
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
    fprintf(stderr, "sde-taskview: cannot open the display\n");
    return 1;
  }
  XSetErrorHandler(ignore_x_errors);
  Window owner = XGetSelectionOwner(fl_display, selection_atom());
  if (owner != None) {
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = owner;
    e.xclient.message_type = XInternAtom(fl_display, "_SDE_TASKVIEW_CLOSE", False);
    e.xclient.format = 32;
    XSendEvent(fl_display, owner, False, NoEventMask, &e);
    XSync(fl_display, False);
    return 0;
  }
  Config cfg;
  cfg.load();
  g_theme = Theme::from_config(cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));

  // The monitor with the pointer.
  Window r, child;
  int px = 0, py = 0, wx, wy;
  unsigned mask;
  XQueryPointer(fl_display, root(), &r, &child, &px, &py, &wx, &wy, &mask);
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, px, py);

  auto *view = new TaskView(sx, sy, sw, sh);
  g_view = view;
  view->end();
  view->border(0);
  view->set_override();
  view->composited = compositor_running();
  // Pictures of the screen before Task View covers it.
  cairo_surface_t *full = screen_picture();
  view->backdrop = backdrop_of(full, sx, sy, sw, sh);
  view->screen = full;
  view->load();
  view->show();
  view->rebuild();
  XSetSelectionOwner(fl_display, selection_atom(), fl_xid(view), CurrentTime);
  Fl::add_handler(x_events);
  view->take_input();
  return Fl::run();
}
