// SPDX-License-Identifier: GPL-3.0-or-later
#include "popup.h"

#include <FL/Fl.H>
#include <FL/x.H>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>

#include "panel.h"
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
  show();
  // FLTK omits WM titles for override-redirect popups. Keep a readable name
  // for window inspection and assistive tools as well as normal windows.
  if (label()) {
    XStoreName(fl_display, fl_xid(this), label());
    x11::set_string(fl_xid(this), "_NET_WM_NAME", label());
  }
  x11::set_opacity(fl_xid(this), panel->theme().popup_opacity);
  if (!native_children() || !panel->native_popup_input()) Fl::grab(this);
  // Override-redirect menus are outside WM focus management. Claim keyboard
  // focus explicitly, including after a GTK4 client has closed its window.
  XSetInputFocus(fl_display, fl_xid(this), RevertToPointerRoot, CurrentTime);
  XFlush(fl_display);
  // A window manager key binding (Super) or a client that just closed can
  // still own the keyboard when the popup maps. Claim focus again shortly
  // after, unless the popup already has it.
  for (double delay : {0.06, 0.25}) Fl::add_timeout(delay, refocus, this);
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
  resize(xx, yy, ww, hh);
}

void PanelPopup::close() {
  if (!shown()) return;
  Fl::remove_timeout(refocus, this);
  if (Fl::grab() == this) Fl::grab(nullptr);
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

void qa_dump(Fl_Window *win, const std::string &extra) {
  if (!qa_dump_enabled() || !win->label()) return;
  std::string text = extra;
  for (int i = 0; i < win->children(); ++i) qa_collect(win->child(i), text, false);
  std::string name = win->label();
  for (char &c : name)
    if (c == '/' || c == ' ') c = '-';
  std::string path = std::string(getenv("SDE_PANEL_QA_DIR")) + "/" + name + ".txt";
  std::string tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}
