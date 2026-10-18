// SPDX-License-Identifier: GPL-3.0-or-later
#include "popup.h"

#include <FL/Fl.H>
#include <FL/x.H>

#include <algorithm>

#include "panel.h"
#include "theme.h"
#include "x11.h"

using namespace sde;

PanelPopup::PanelPopup(const char *title) : BufferedWindow(0, 0, 100, 100, title) {
  set_override();
  border(0);
  resizable(nullptr);
}

void PanelPopup::open_at(int anchor_x, bool align_right) {
  Panel *panel = Panel::instance;
  prepare();
  int mx, my, mw, mh;
  panel->monitor(mx, my, mw, mh);
  int gap = S(8);
  int x = align_right ? anchor_x - w() : anchor_x;
  x = std::max(mx + gap, std::min(x, mx + mw - w() - gap));
  int y;
  if (panel->theme().panel_top) y = panel->y() + panel->h() + gap;
  else y = panel->y() - h() - gap;
  // Never larger than the space next to the panel.
  int avail = mh - panel->h() - 2 * gap;
  int hh = std::min(h(), avail);
  if (!panel->theme().panel_top) y = panel->y() - hh - gap;
  resize(x, y, w(), hh);
  show();
  // FLTK omits WM titles for override-redirect popups. Keep a readable name
  // for window inspection and assistive tools as well as normal windows.
  if (label()) {
    XStoreName(fl_display, fl_xid(this), label());
    x11::set_string(fl_xid(this), "_NET_WM_NAME", label());
  }
  Fl::grab(this);
  // Override-redirect menus are outside WM focus management. Claim keyboard
  // focus explicitly, including after a GTK4 client has closed its window.
  XSetInputFocus(fl_display, fl_xid(this), RevertToPointerRoot, CurrentTime);
  XFlush(fl_display);
  panel->popup_opened(this);
  redraw();
  // Keyboard users start on the first control.
  Fl::add_timeout(0, [](void *d) {
    auto *self = static_cast<PanelPopup *>(d);
    if (!self->shown()) return;
    Fl_Widget *f = Fl::focus();
    if (f && (f == self || f->window() == self)) return;
    for (int i = 0; i < self->children(); ++i) {
      Fl_Widget *c = self->child(i);
      if (c->visible() && c->visible_focus() && c->take_focus()) return;
    }
    self->take_focus();
  }, this);
}

void PanelPopup::close() {
  if (!shown()) return;
  if (Fl::grab() == this) Fl::grab(nullptr);
  hide();
  closed();
  if (Panel::instance) Panel::instance->popup_closed(this);
}

int PanelPopup::handle(int event) {
  if (event == FL_PUSH) {
    int ex = Fl::event_x(), ey = Fl::event_y();
    if (ex < 0 || ey < 0 || ex >= w() || ey >= h()) {
      // Clicking outside (including the panel button that opened the
      // popup) only closes it.
      close();
      return 1;
    }
  }
  if ((event == FL_KEYBOARD || event == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
    close();
    return 1;
  }
  return BufferedWindow::handle(event);
}
