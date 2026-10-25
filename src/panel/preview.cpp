// SPDX-License-Identifier: GPL-3.0-or-later
#include "preview.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <cairo-xlib.h>
#ifdef SDE_HAVE_XCOMPOSITE
#include <X11/extensions/Xcomposite.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>

#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "panel.h"
#include "popup.h"
#include "popup_style.h"
#include "status_model.h"
#include "theme.h"
#include "x11.h"

using namespace sde;

namespace {

// Thumbnail box at 100 % and the title row above it.
constexpr int kThumbW = 220, kThumbH = 140, kTitle = 24, kPad = 8, kGap = 6, kMinW = 120;

struct Thumb {
  cairo_surface_t *img = nullptr;
  long long at = 0;  // steady ms of the capture
};
std::map<Window, Thumb> &thumbs() {
  static std::map<Window, Thumb> m;
  return m;
}
long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// X errors while capturing (a window that just went away) only fail the
// capture.
bool g_failed = false;
int trap_error(Display *, XErrorEvent *) {
  g_failed = true;
  return 0;
}
struct ErrorTrap {
  XErrorHandler old;
  ErrorTrap() {
    XSync(fl_display, False);
    g_failed = false;
    old = XSetErrorHandler(trap_error);
  }
  ~ErrorTrap() {
    XSync(fl_display, False);
    XSetErrorHandler(old);
  }
  bool failed() {
    XSync(fl_display, False);
    return g_failed;
  }
};

bool compositor_running() {
  char name[32];
  snprintf(name, sizeof name, "_NET_WM_CM_S%d", fl_screen);
  return XGetSelectionOwner(fl_display, XInternAtom(fl_display, name, False)) != None;
}

#ifdef SDE_HAVE_XCOMPOSITE
// The child of the root window that holds |w| (the window manager's frame).
Window toplevel(Window w) {
  Window root = 0, parent = 0, *children = nullptr;
  unsigned n = 0;
  Window cur = w;
  for (int depth = 0; depth < 16; ++depth) {
    if (!XQueryTree(fl_display, cur, &root, &parent, &children, &n)) return 0;
    if (children) XFree(children);
    if (parent == root || !parent) return cur;
    cur = parent;
  }
  return 0;
}
#endif

// Scales |source| (sw x sh, the window at ox,oy inside it) to a w x h image.
// XRender does the large reduction on the server (bilinear, to twice the
// size); cairo smooths the last step on the small image.
cairo_surface_t *scale_capture(cairo_surface_t *source, int ox, int oy, int win_w, int win_h,
                               int w, int h) {
  int mw = std::min(win_w, w * 2), mh = std::min(win_h, h * 2);
  cairo_surface_t *mid = cairo_surface_create_similar(source, CAIRO_CONTENT_COLOR, mw, mh);
  cairo_t *cr = cairo_create(mid);
  cairo_scale(cr, double(mw) / win_w, double(mh) / win_h);
  cairo_set_source_surface(cr, source, -ox, -oy);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cr = cairo_create(img);
  cairo_scale(cr, double(w) / mw, double(h) / mh);
  cairo_set_source_surface(cr, mid, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(mid);
  cairo_surface_flush(img);
  return img;
}

cairo_surface_t *grab(Window w) {
  ErrorTrap trap;
  XWindowAttributes a;
  if (!XGetWindowAttributes(fl_display, w, &a) || trap.failed()) return nullptr;
  if (a.map_state != IsViewable || a.width < 4 || a.height < 4) return nullptr;
  // Thumbnails are kept at the 100 % box size times the UI scale.
  PreviewSize s = preview_fit(a.width, a.height, S(kThumbW), S(kThumbH));
  if (s.w <= 0) return nullptr;
  cairo_surface_t *img = nullptr;
#ifdef SDE_HAVE_XCOMPOSITE
  if (compositor_running()) {
    // The compositor keeps the frame's contents in a pixmap, also where
    // other windows cover it.
    Window frame = toplevel(w);
    XWindowAttributes fa;
    if (frame && XGetWindowAttributes(fl_display, frame, &fa) && fa.map_state == IsViewable) {
      Pixmap pm = XCompositeNameWindowPixmap(fl_display, frame);
      if (!trap.failed() && pm) {
        int ox = 0, oy = 0;
        Window child;
        XTranslateCoordinates(fl_display, w, frame, 0, 0, &ox, &oy, &child);
        ox += fa.border_width, oy += fa.border_width;
        cairo_surface_t *src = cairo_xlib_surface_create(
            fl_display, pm, fa.visual, fa.width + 2 * fa.border_width,
            fa.height + 2 * fa.border_width);
        img = scale_capture(src, ox, oy, a.width, a.height, s.w, s.h);
        cairo_surface_destroy(src);
        XFreePixmap(fl_display, pm);
        if (trap.failed()) {
          cairo_surface_destroy(img);
          img = nullptr;
        }
      }
      g_failed = false;
    }
  }
#endif
  if (!img) {
    // No compositor: the mapped window itself (what is on screen there).
    cairo_surface_t *src = cairo_xlib_surface_create(fl_display, w, a.visual, a.width, a.height);
    img = scale_capture(src, 0, 0, a.width, a.height, s.w, s.h);
    cairo_surface_destroy(src);
    if (trap.failed()) {
      cairo_surface_destroy(img);
      img = nullptr;
    }
  }
  return img;
}

void hover_path(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

}  // namespace

// ---------------------------------------------------------------- cache

bool TaskPreview::capture(Window w) {
  cairo_surface_t *img = grab(w);
  if (!img) return false;
  auto &t = thumbs()[w];
  if (t.img) cairo_surface_destroy(t.img);
  t.img = img;
  t.at = now_ms();
  return true;
}

void TaskPreview::forget(Window w) {
  auto it = thumbs().find(w);
  if (it == thumbs().end()) return;
  if (it->second.img) cairo_surface_destroy(it->second.img);
  thumbs().erase(it);
}

void TaskPreview::retain(const std::vector<Window> &alive) {
  for (auto it = thumbs().begin(); it != thumbs().end();) {
    if (std::find(alive.begin(), alive.end(), it->first) == alive.end()) {
      if (it->second.img) cairo_surface_destroy(it->second.img);
      it = thumbs().erase(it);
    } else {
      ++it;
    }
  }
}

bool TaskPreview::has_thumbnail(Window w) { return thumbs().count(w) && thumbs()[w].img; }

// --------------------------------------------------------------- window

TaskPreview::TaskPreview() : BufferedWindow(0, 0, 100, 100, "SDE Preview") {
  set_override();
  border(0);
  end();
}

TaskPreview::~TaskPreview() {
  Fl::remove_timeout(hide_timeout, this);
  Fl::remove_timeout(refresh_timeout, this);
}

void TaskPreview::layout() {
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  int n = int(items_.size());
  int col = preview_column_width(n, S(kThumbW), S(kMinW), S(kGap), mw - S(16) - 2 * S(kPad));
  cells_.clear();
  int th = col == S(kThumbW) ? S(kThumbH) : col * kThumbH / kThumbW;
  for (int i = 0; i < n; ++i) {
    Cell c;
    c.x = S(kPad) + i * (col + S(kGap));
    c.w = col;
    c.tx = c.x + S(4), c.ty = S(kPad) + S(kTitle) + S(2);
    c.tw = col - S(8), c.th = th;
    c.cs = S(20);
    c.cx = c.x + col - c.cs - S(2), c.cy = S(kPad) + (S(kTitle) - c.cs) / 2;
    cells_.push_back(c);
  }
  int W = 2 * S(kPad) + n * col + std::max(0, n - 1) * S(kGap);
  int H = S(kPad) + S(kTitle) + S(2) + th + S(4) + S(kPad);
  size(W, H);
}

void TaskPreview::place(int center_x) {
  auto *panel = Panel::instance;
  int mx, my, mw, mh;
  panel->monitor(mx, my, mw, mh);
  int margin = S(8), gap = S(6);
  int xx = std::max(mx + margin, std::min(center_x - w() / 2, mx + mw - w() - margin));
  int yy = panel->theme().panel_top ? panel->y() + panel->h() + gap : panel->y() - h() - gap;
  position(xx, yy);
}

void TaskPreview::show_items(const std::vector<Item> &items, int center_x, Window hovered) {
  Fl::remove_timeout(hide_timeout, this);
  bool same = items.size() == items_.size() &&
              std::equal(items.begin(), items.end(), items_.begin(),
                         [](const Item &a, const Item &b) { return a.win == b.win; });
  items_ = items;
  hovered_ = hovered;
  center_x_ = center_x;
  if (!same) hot_ = -1;
  // Fresh pictures of the mapped windows; minimized ones keep the last one.
  source_.clear();
  bool composited = compositor_running();
  for (const auto &it : items_) {
    bool fresh = (composited || !covers(it.win)) && capture(it.win);
    source_[it.win] = fresh ? "live" : has_thumbnail(it.win) ? "cached" : "icon";
  }
  layout();
  place(center_x);
  if (!shown()) {
    show();
    if (fl_xid(this)) {
      XStoreName(fl_display, fl_xid(this), label());
      x11::set_string(fl_xid(this), "_NET_WM_NAME", label());
      x11::set_opacity(fl_xid(this), Panel::instance->theme().popup_opacity);
      x11::set_surface(fl_xid(this), "popup");
    }
  } else {
    resize(x(), y(), w(), h());
  }
  if (fl_xid(this)) apply_rounded_shape(fl_xid(this), w(), h(), popup_style::radius());
  Fl::remove_timeout(refresh_timeout, this);
  Fl::add_timeout(1.0, refresh_timeout, this);
  redraw();
}

void TaskPreview::refresh_timeout(void *data) {
  auto *self = static_cast<TaskPreview *>(data);
  if (!self->showing()) return;
  bool any = false, composited = compositor_running();
  for (const auto &it : self->items_) {
    if (!composited && self->covers(it.win)) continue;
    if (capture(it.win)) {
      self->source_[it.win] = "live";
      any = true;
    }
  }
  if (any) self->redraw();
  Fl::repeat_timeout(1.0, refresh_timeout, data);
}

// Without a compositor a capture reads the screen: a window under the shown
// preview would picture the preview itself.
bool TaskPreview::covers(Window win) const {
  if (!const_cast<TaskPreview *>(this)->shown()) return false;
  XWindowAttributes a;
  int rx = 0, ry = 0;
  Window child;
  ErrorTrap trap;
  if (!XGetWindowAttributes(fl_display, win, &a) ||
      !XTranslateCoordinates(fl_display, win, a.root, 0, 0, &rx, &ry, &child))
    return true;
  return rx < x() + w() && rx + a.width > x() && ry < y() + h() && ry + a.height > y();
}

void TaskPreview::hide_soon() {
  Fl::remove_timeout(hide_timeout, this);
  Fl::add_timeout(0.25, hide_timeout, this);
}
void TaskPreview::cancel_hide() { Fl::remove_timeout(hide_timeout, this); }
void TaskPreview::hide_timeout(void *data) {
  auto *self = static_cast<TaskPreview *>(data);
  // The pointer moved onto the preview (its Enter may come before the
  // panel's Leave): keep it.
  int rx, ry;
  Fl::get_mouse(rx, ry);
  if (self->shown() && rx >= self->x() && rx < self->x() + self->w() && ry >= self->y() &&
      ry < self->y() + self->h())
    return;
  self->hide_now();
}

void TaskPreview::hide_now() {
  Fl::remove_timeout(hide_timeout, this);
  Fl::remove_timeout(refresh_timeout, this);
  items_.clear();
  cells_.clear();
  hovered_ = 0;
  hot_ = -1;
  if (shown()) hide();
  if (qa_dump_enabled()) qa_dump(this, "@hidden\t0\t0\t0\t0\t1\n");
}

int TaskPreview::item_at(int x, int y) const {
  for (size_t i = 0; i < cells_.size(); ++i)
    if (x >= cells_[i].x && x < cells_[i].x + cells_[i].w && y >= 0 && y < h()) return int(i);
  return -1;
}

bool TaskPreview::on_close(int i, int x, int y) const {
  if (i < 0 || i >= int(cells_.size())) return false;
  const Cell &c = cells_[size_t(i)];
  return x >= c.cx && x < c.cx + c.cs && y >= c.cy && y < c.cy + c.cs;
}

void TaskPreview::draw() {
  const Theme &t = Panel::instance->theme();
  std::string qa;
  {
    Painter p(w(), h());
    cairo_t *cr = p.cr();
    double r = popup_style::radius();
    p.fill_rect(0, 0, w(), h(), t.surface);
    // The edge of every panel popup (popup.cpp); the glass shader draws it
    // in Liquid Glass.
    if (t.glass && !t.liquid)
      p.glass_surface(0, 0, w(), h(), r, t.border, t.dark);
    else if (!t.liquid)
      p.surface_edge(0, 0, w(), h(), r, t.border, t.dark ? 1.0 : 0.0, t.dark);
    for (size_t i = 0; i < cells_.size() && i < items_.size(); ++i) {
      const Cell &c = cells_[i];
      const Item &it = items_[i];
      bool hot = int(i) == hot_;
      if (hot || it.win == hovered_)
        p.fill_rounded(c.x, S(kPad) - S(2), c.w, h() - 2 * S(kPad) + S(4), S(6),
                       hot ? t.hover : t.hover.alpha(t.hover.a * 0.5));
      // Thumbnail: the window picture fitted into the box, else the icon.
      auto th = thumbs().find(it.win);
      cairo_surface_t *img = th != thumbs().end() ? th->second.img : nullptr;
      int iw = 0, ih = 0;
      if (img) {
        PreviewSize fit = preview_fit(cairo_image_surface_get_width(img),
                                      cairo_image_surface_get_height(img), c.tw, c.th);
        iw = fit.w, ih = fit.h;
        double ix = c.tx + (c.tw - iw) / 2.0, iy = c.ty + (c.th - ih) / 2.0;
        cairo_save(cr);
        hover_path(cr, ix, iy, iw, ih, S(4));
        cairo_clip(cr);
        cairo_translate(cr, ix, iy);
        cairo_scale(cr, double(iw) / cairo_image_surface_get_width(img),
                    double(ih) / cairo_image_surface_get_height(img));
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        p.stroke_rounded(ix + 0.5, iy + 0.5, iw - 1, ih - 1, S(4), t.border);
      } else {
        p.fill_rounded(c.tx, c.ty, c.tw, c.th, S(6), t.surface_alt);
        int big = std::min({S(64), c.tw - S(16), c.th - S(16)});
        cairo_surface_t *icon =
            it.icon ? it.icon : IconLoader::get({"application-x-executable"}, big);
        if (icon) p.image(icon, c.tx + (c.tw - big) / 2.0, c.ty + (c.th - big) / 2.0, big);
        iw = c.tw, ih = c.th;
      }
      // Title row: icon and the ellipsized title; the close button on hover.
      int ts = S(popup_style::kIcon);
      if (it.icon) p.image(it.icon, c.x + S(4), S(kPad) + (S(kTitle) - ts) / 2.0, ts);
      if (hot) {
        bool hc = hot_close_;
        if (hc) p.fill_rounded(c.cx, c.cy, c.cs, c.cs, S(4), Color::hex(0xE81123));
        Color ink = hc ? Color::hex(0xFFFFFF) : t.text;
        double m = c.cs * 0.32;
        p.line(c.cx + m, c.cy + m, c.cx + c.cs - m, c.cy + c.cs - m, ink, Sd(1.4));
        p.line(c.cx + c.cs - m, c.cy + m, c.cx + m, c.cy + c.cs - m, ink, Sd(1.4));
      }
      std::string source = source_.count(it.win) ? source_[it.win] : "icon";
      auto num = [](int v) { return std::to_string(v); };
      qa += "preview:" + it.title + "\t" + num(c.x) + "\t0\t" + num(c.w) + "\t" + num(h()) +
            "\t1\n";
      qa += "close:" + it.title + "\t" + num(c.cx) + "\t" + num(c.cy) + "\t" + num(c.cs) + "\t" +
            num(c.cs) + "\t1\n";
      qa += "@thumb:" + source + ":" + num(iw) + "x" + num(ih) + ":" + it.title + "\t" +
            num(c.tx) + "\t" + num(c.ty) + "\t" + num(c.tw) + "\t" + num(c.th) + "\t1\n";
    }
  }
  text_font(t.font_size);
  for (size_t i = 0; i < cells_.size() && i < items_.size(); ++i) {
    const Cell &c = cells_[i];
    int tx = c.x + S(4) + (items_[i].icon ? S(popup_style::kIcon) + S(6) : 0);
    int right = int(i) == hot_ ? c.cx - S(4) : c.x + c.w - S(4);
    draw_text(items_[i].title, tx, S(kPad), right - tx, S(kTitle), t.text);
  }
  if (qa_dump_enabled()) qa_dump(this, qa);
}

int TaskPreview::handle(int event) {
  switch (event) {
    case FL_ENTER:
      cancel_hide();
      [[fallthrough]];
    case FL_MOVE: {
      cancel_hide();
      int i = item_at(Fl::event_x(), Fl::event_y());
      bool hc = on_close(i, Fl::event_x(), Fl::event_y());
      if (i != hot_ || hc != hot_close_) {
        hot_ = i;
        hot_close_ = hc;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hot_ = -1;
      hot_close_ = false;
      redraw();
      hide_soon();
      return 1;
    case FL_PUSH:
      return 1;
    case FL_RELEASE: {
      int i = item_at(Fl::event_x(), Fl::event_y());
      if (i < 0 || i >= int(items_.size())) return 1;
      Window win = items_[size_t(i)].win;
      if (Fl::event_button() == FL_MIDDLE_MOUSE || on_close(i, Fl::event_x(), Fl::event_y())) {
        x11::close_window(win);
        // The window goes away: drop it from the preview (or close it).
        items_.erase(items_.begin() + i);
        if (items_.empty()) {
          hide_now();
        } else {
          hot_ = -1;
          layout();
          place(center_x_);
          if (fl_xid(this)) apply_rounded_shape(fl_xid(this), w(), h(), popup_style::radius());
          redraw();
        }
        return 1;
      }
      hide_now();
      x11::activate_window(win);
      return 1;
    }
    default:
      break;
  }
  return BufferedWindow::handle(event);
}
