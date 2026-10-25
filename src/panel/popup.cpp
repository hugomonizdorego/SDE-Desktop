// SPDX-License-Identifier: GPL-3.0-or-later
#include "popup.h"

#include <FL/Fl.H>
#include <FL/x.H>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Scroll.H>
#include <X11/extensions/shape.h>

#include <cmath>
#include <vector>

#include "menu_perf.h"
#include "paint.h"
#include "panel.h"
#include "popup_style.h"
#include "theme.h"
#include "x11.h"

using namespace sde;

PanelPopup::PanelPopup(const char *title) : BufferedWindow(0, 0, 100, 100, title) {
  set_override();
  border(0);
  resizable(nullptr);
}

void PanelPopup::open_centered(int center_x) { open(center_x, 2); }
void PanelPopup::open_at(int anchor_x, bool align_right) { open(anchor_x, align_right ? 1 : 0); }

void PanelPopup::open(int anchor_x, int align) {
  Panel *panel = Panel::instance;
  prepare();
  anchor_x_ = anchor_x;
  align_ = align;
  reposition();
  double shown_at = menu_perf::enabled() ? menu_perf::now_ms() : 0;
  show();
  apply_shape();
  if (shown_at > 0) menu_perf::note("show", menu_perf::now_ms() - shown_at);
  // FLTK omits WM titles for override-redirect popups. Keep a readable name
  // for window inspection and assistive tools as well as normal windows.
  if (label()) {
    XStoreName(fl_display, fl_xid(this), label());
    x11::set_string(fl_xid(this), "_NET_WM_NAME", label());
  }
  x11::set_opacity(fl_xid(this), panel->theme().popup_opacity);
  // A glass surface for the compositor, with the corners it is shaped with.
  x11::set_surface(fl_xid(this), !corner_popup()               ? "popup"
                                 : panel->theme().panel_top ? "popup-bottom"
                                                            : "popup-top");
  grabbing_ = !native_children() || !panel->native_popup_input();
  if (grabbing_) {
    // A grab of FLTK that is left over from a popup that went away without
    // closing would make Fl::grab() skip the X grabs for this one.
    if (Fl::grab() && Fl::grab() != this && !Fl::grab()->shown()) Fl::grab(nullptr);
    Fl::grab(this);
  }
  // Override-redirect menus are outside WM focus management. Claim keyboard
  // focus explicitly, including after a GTK4 client has closed its window.
  XSetInputFocus(fl_display, fl_xid(this), RevertToPointerRoot, CurrentTime);
  XFlush(fl_display);
  // A window manager key binding (Super) or a client that just closed can
  // still own the keyboard when the popup maps. Claim focus again shortly
  // after, unless the popup already has it.
  for (double delay : {0.06, 0.25}) Fl::add_timeout(delay, refocus, this);
  input_tries_ = 0;
  qa_mark_input(this, false);
  Fl::remove_timeout(ensure_input, this);
  Fl::add_timeout(0.1, ensure_input, this);
  panel->popup_opened(this);
  redraw();
  // Keyboard users start on the first control.
  Fl::add_timeout(
      0,
      [](void *d) {
        auto *self = static_cast<PanelPopup *>(d);
        if (!self->shown()) return;
        Fl_Widget *f = Fl::focus();
        if (f && (f == self || f->window() == self)) return;
        for (int i = 0; i < self->children(); ++i) {
          Fl_Widget *c = self->child(i);
          if (c->visible() && c->visible_focus() && c->take_focus()) return;
        }
        self->take_focus();
      },
      this);
}

bool PanelPopup::viewable() const {
  if (!const_cast<PanelPopup *>(this)->shown()) return false;
  XWindowAttributes attr;
  if (!XGetWindowAttributes(fl_display, fl_xid(this), &attr)) return false;
  return attr.map_state == IsViewable;
}

bool PanelPopup::has_input() const {
  if (!is_open()) return false;
  Window focused = 0;
  int revert = 0;
  XGetInputFocus(fl_display, &focused, &revert);
  return focused == fl_xid(this);
}

void PanelPopup::ensure_input(void *data) {
  auto *self = static_cast<PanelPopup *>(data);
  if (!self->is_open() || !fl_xid(self)) return;
  bool ok = true;
  Window xid = fl_xid(self);
  if (self->grabbing_ && Fl::grab() == self) {
    // Grabbing again is harmless when we hold the grab; it fails with
    // AlreadyGrabbed while another client (the screenshot tool) holds it.
    int pointer = XGrabPointer(fl_display, xid, True,
                               ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
                                   PointerMotionMask,
                               GrabModeAsync, GrabModeAsync, None, 0, CurrentTime);
    int keyboard = XGrabKeyboard(fl_display, xid, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    ok = pointer == GrabSuccess && keyboard == GrabSuccess;
  }
  Window focused = 0;
  int revert = 0;
  XGetInputFocus(fl_display, &focused, &revert);
  if (focused != xid) {
    XSetInputFocus(fl_display, xid, RevertToPointerRoot, CurrentTime);
    ok = false;
  }
  XFlush(fl_display);
  // Every 100 ms for 5 s, then every second while the popup stays open.
  ++self->input_tries_;
  if (!ok) Fl::repeat_timeout(self->input_tries_ < 50 ? 0.1 : 1.0, ensure_input, data);
  else qa_mark_input(self, true);
}

void PanelPopup::refocus(void *data) {
  auto *self = static_cast<PanelPopup *>(data);
  if (!self->is_open() || !fl_xid(self)) return;
  Window focused = 0;
  int revert = 0;
  XGetInputFocus(fl_display, &focused, &revert);
  if (focused != fl_xid(self)) {
    XSetInputFocus(fl_display, fl_xid(self), RevertToPointerRoot, CurrentTime);
    XFlush(fl_display);
  }
}

void PanelPopup::reposition() {
  auto *panel = Panel::instance;
  int mx, my, mw, mh;
  panel->monitor(mx, my, mw, mh);
  int gap = corner_popup() ? 0 : S(8), margin = corner_popup() ? 0 : S(8);
  int available = panel->theme().panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my;
  int ww = std::min(w(), mw - 2 * margin),
      hh = std::min(h(), std::max(1, available - gap - margin));
  int right = corner_popup() ? std::min(mx + mw, panel->x() + panel->w()) : anchor_x_;
  int xx = corner_popup() || align_ == 1 ? right - ww : align_ == 2 ? right - ww / 2 : right;
  xx = std::max(mx + margin, std::min(xx, mx + mw - ww - margin));
  int yy = panel->theme().panel_top ? panel->y() + panel->h() + gap : panel->y() - hh - gap;
  yy = std::max(my, std::min(yy, my + mh - hh));
  if (shown() && (xx != x() || yy != y() || ww != w() || hh != h())) {
    // A click inside the old rectangle (e.g. on "Show less", which shrinks
    // the Action Center) must not count as a click outside the new one.
    previous_x_ = x(), previous_y_ = y(), previous_w_ = w(), previous_h_ = h();
    previous_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
  }
  resize(xx, yy, ww, hh);
}

void PanelPopup::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  if (shown() && (w != shaped_w_ || h != shaped_h_)) apply_shape();
}

int PanelPopup::corner_radius() const {
  return std::max(0, std::min({popup_style::radius(), w() / 4, h() / 4}));
}

namespace {
// Corners to round: 1 top-left, 2 top-right, 4 bottom-right, 8 bottom-left.
int rounded_corners(bool corner_popup) {
  if (!corner_popup) return 15;
  // The Action Center sits on the panel: square on the panel's side.
  return Panel::instance && Panel::instance->theme().panel_top ? 4 | 8 : 1 | 2;
}
}  // namespace

void apply_rounded_shape(Window xid, int w, int h, int r, int corners) {
  int event, error;
  if (!xid || !XShapeQueryExtension(fl_display, &event, &error)) return;
  if (r <= 0) {
    XShapeCombineMask(fl_display, xid, ShapeBounding, 0, 0, None, ShapeSet);
    return;
  }
  // One rectangle per row inside the rounded outline (as the panel's shape).
  std::vector<XRectangle> rows;
  for (int row = 0; row < h; ++row) {
    bool top = row < r, bottom = row >= h - r;
    double dy = top ? r - row - 0.5 : bottom ? row - (h - r) + 0.5 : 0;
    int inset = dy > 0 ? int(std::ceil(r - std::sqrt(std::max(0.0, r * r - dy * dy)))) : 0;
    int left = (top && (corners & 1)) || (bottom && (corners & 8)) ? inset : 0;
    int right = (top && (corners & 2)) || (bottom && (corners & 4)) ? inset : 0;
    rows.push_back({short(left), short(row),
                    static_cast<unsigned short>(std::max(1, w - left - right)), 1});
  }
  XShapeCombineRectangles(fl_display, xid, ShapeBounding, 0, 0, rows.data(), int(rows.size()),
                          ShapeSet, YXBanded);
}

void PanelPopup::apply_shape() {
  if (!shown() || !fl_xid(this)) return;
  shaped_w_ = w(), shaped_h_ = h();
  apply_rounded_shape(fl_xid(this), w(), h(), corner_radius(), rounded_corners(corner_popup()));
}

void PanelPopup::stroke_outline(Painter &p, const Color &c) const {
  const Theme &t = Panel::instance->theme();
  // Liquid Glass: the compositor's glass shader draws the edge and its light.
  if (t.liquid) return;
  // 0.2.0 Alpha 2: the outline with a light rim inside it in dark mode (a
  // lifted surface), and in SDE Liquid Glass the glass light from the top.
  double r = corner_radius();
  int corners = rounded_corners(corner_popup());
  cairo_new_path(p.cr());
  if (t.glass)
    p.glass_surface(0, 0, w(), h(), r, c, t.dark, corners);
  else
    p.surface_edge(0, 0, w(), h(), r, c, t.dark ? 1.0 : 0.0, t.dark, corners);
}

std::string typed_key_text() {
  if (Fl::event_state() & (FL_CTRL | FL_ALT | FL_META)) return "";
  const char *text = Fl::event_text();
  if (text && *text) {
    // Control characters (Tab, Enter, Backspace...) and the space before
    // anything else type nothing.
    unsigned char c = static_cast<unsigned char>(text[0]);
    return c >= 0x21 && c != 0x7F ? std::string(text) : std::string();
  }
  int k = Fl::event_key();
  if (k <= 0x20 || k >= 0xFF || k == 0x7F || (k >= 0x80 && k < 0xA0)) return "";
  if (k >= 'a' && k <= 'z' && (Fl::event_state() & (FL_SHIFT | FL_CAPS_LOCK))) k -= 'a' - 'A';
  std::string out;
  if (k < 0x80) {
    out += char(k);
  } else {  // Latin-1 key symbols are their code points
    out += char(0xC0 | (k >> 6));
    out += char(0x80 | (k & 0x3F));
  }
  return out;
}

namespace {
// While a popup menu of a PanelPopup is open: where a typed character goes.
std::string *g_menu_typed = nullptr;
Fl_Event_Dispatch g_dispatch_before = nullptr;

int popup_menu_dispatch(int event, Fl_Window *w) {
  if (event == FL_KEYBOARD && g_menu_typed) {
    std::string text = typed_key_text();
    if (!text.empty()) {
      // FLTK's popup menus do nothing with letters (no item has an "&"
      // shortcut): close it as Escape does and keep the text — all of it:
      // keys typed quickly can arrive together, before the menu is gone.
      *g_menu_typed += text;
      Fl::e_keysym = FL_Escape;
    }
  }
  return g_dispatch_before ? g_dispatch_before(event, w) : Fl::handle_(event, w);
}
}  // namespace

// UI test hook: <qa>/<title>-popup.txt lists the items of the popup menu
// that is open ("item:<label>", "sub:<submenu>/<label>", the active flag
// as the sixth field) after an "@seq:<n>" line that grows with each one.
static void qa_dump_popup(Fl_Window *win, const Fl_Menu_Item *items) {
  if (!qa_dump_enabled() || !win->label()) return;
  static int seq = 0;
  std::string text = "@seq:" + std::to_string(++seq) + "\t0\t0\t0\t0\t1\n";
  std::string prefix;
  int depth = 0;
  for (const Fl_Menu_Item *m = items; m && (m->text || depth > 0); ++m) {
    if (!m->text) {  // the end of a submenu
      --depth;
      prefix.clear();
      continue;
    }
    std::string label = m->text;
    label.erase(std::remove(label.begin(), label.end(), '\t'), label.end());
    text += (depth ? "sub:" + prefix : "item:") + label + "\t0\t0\t0\t0\t" +
            std::to_string(m->active() ? 1 : 0) + "\n";
    if (m->flags & FL_SUBMENU) {
      ++depth;
      prefix = label + "/";
    }
  }
  std::string name = win->label();
  for (char &c : name)
    if (c == '/' || c == ' ') c = '-';
  std::string path = std::string(getenv("SDE_PANEL_QA_DIR")) + "/" + name + "-popup.txt";
  if (FILE *f = fopen((path + ".tmp").c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename((path + ".tmp").c_str(), path.c_str());
  }
}

const Fl_Menu_Item *PanelPopup::popup_menu(const Fl_Menu_Item *items, int x, int y,
                                           std::string *typed) {
  qa_dump_popup(this, items);
  std::string text;
  g_menu_typed = &text;
  g_dispatch_before = Fl::event_dispatch();
  Fl::event_dispatch(popup_menu_dispatch);
  const Fl_Menu_Item *m = items->popup(x, y);
  Fl::event_dispatch(g_dispatch_before);
  g_dispatch_before = nullptr;
  g_menu_typed = nullptr;
  if (typed) *typed = text;
  // The grabs and the focus come back (the chosen action may close the
  // popup right after, which releases them again).
  if (is_open() && fl_xid(this)) {
    if (grabbing_ && Fl::grab() != this) Fl::grab(this);
    XSetInputFocus(fl_display, fl_xid(this), RevertToPointerRoot, CurrentTime);
    XFlush(fl_display);
    input_tries_ = 0;
    Fl::remove_timeout(ensure_input, this);
    Fl::add_timeout(0.1, ensure_input, this);
  }
  return text.empty() ? m : nullptr;
}

bool PanelPopup::contains_root(int rx, int ry) const {
  if (rx >= x() && rx < x() + w() && ry >= y() && ry < y() + h()) return true;
  return std::chrono::steady_clock::now() < previous_until_ && rx >= previous_x_ &&
         rx < previous_x_ + previous_w_ && ry >= previous_y_ && ry < previous_y_ + previous_h_;
}

void PanelPopup::close() {
  if (!shown()) return;
  Fl::remove_timeout(refocus, this);
  Fl::remove_timeout(ensure_input, this);
  qa_mark_input(this, false);
  if (Fl::grab() == this) Fl::grab(nullptr);
  grabbing_ = false;
  // FLTK 1.3 hide() destroys this X window. Return native tray children to
  // the panel before that happens, while reporting this popup as closed.
  closing_ = true;
  closed();
  if (Panel::instance) Panel::instance->popup_closed(this);
  hide();
  closing_ = false;
}

int PanelPopup::handle(int event) {
  if (event == FL_PUSH) {
    int ex = Fl::event_x(), ey = Fl::event_y();
    if (ex < 0 || ey < 0 || ex >= w() || ey >= h()) {
      // Clicking outside closes the popup; the panel button that opened it
      // only closes it, another panel button opens its own popup at once.
      int rx = Fl::event_x_root(), ry = Fl::event_y_root();
      close();
      Panel::instance->switch_popup(rx, ry, this);
      return 1;
    }
  }
  if ((event == FL_KEYBOARD || event == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
    close();
    return 1;
  }
  return BufferedWindow::handle(event);
}

// ------------------------------------------------------------- UI test hook

bool qa_dump_enabled() {
  const char *dir = getenv("SDE_PANEL_QA_DIR");
  return dir && *dir;
}

namespace {
void qa_collect(Fl_Widget *w, std::string &out, bool scrolled) {
  if (!w->visible()) return;
  const char *name = w->tooltip() ? w->tooltip() : w->label();
  if (name && *name && w != w->window()) {
    std::string n = name;
    for (char &c : n)
      if (c == '\t' || c == '\n') c = ' ';
    char buf[96];
    snprintf(buf, sizeof buf, "\t%d\t%d\t%d\t%d\t%d\t%d\n", w->x(), w->y(), w->w(), w->h(),
             w->active_r() ? 1 : 0, scrolled ? 1 : 0);
    out += n + buf;
  }
  if (auto *g = w->as_group())
    for (int i = 0; i < g->children(); ++i)
      qa_collect(g->child(i), out, scrolled || dynamic_cast<Fl_Scroll *>(w));
}
}  // namespace

namespace {
std::string qa_path(Fl_Window *win, const char *suffix) {
  std::string name = win->label();
  for (char &c : name)
    if (c == '/' || c == ' ') c = '-';
  return std::string(getenv("SDE_PANEL_QA_DIR")) + "/" + name + suffix;
}
}  // namespace

void qa_mark_input(Fl_Window *win, bool has_input) {
  if (!qa_dump_enabled() || !win->label()) return;
  std::string path = qa_path(win, ".input");
  if (!has_input) {
    unlink(path.c_str());
  } else if (FILE *f = fopen(path.c_str(), "w")) {
    fclose(f);
  }
}

void qa_dump(Fl_Window *win, const std::string &extra) {
  if (!qa_dump_enabled() || !win->label()) return;
  std::string text = extra;
  for (int i = 0; i < win->children(); ++i) qa_collect(win->child(i), text, false);
  std::string path = qa_path(win, ".txt");
  std::string tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}
