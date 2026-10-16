// SPDX-License-Identifier: GPL-3.0-or-later
#include "tray.h"

#include <FL/Fl.H>
#include <FL/x.H>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "x11.h"

namespace {
constexpr long SYSTEM_TRAY_REQUEST_DOCK = 0;
constexpr long XEMBED_EMBEDDED_NOTIFY = 0;
constexpr long XEMBED_MAPPED = 1;
}  // namespace

Tray::Tray(Window panel) : panel_(panel) {}

Tray::~Tray() {
  for (Window w : std::vector<Window>(icons_)) undock(w, true);
  if (owner_) XDestroyWindow(fl_display, owner_);
}

bool Tray::start() {
  std::string name = "_NET_SYSTEM_TRAY_S" + std::to_string(fl_screen);
  selection_ = XInternAtom(fl_display, name.c_str(), False);
  if (XGetSelectionOwner(fl_display, selection_) != None) {
    fprintf(stderr, "sde-panel: another system tray is running; tray disabled\n");
    return false;
  }
  Window root = RootWindow(fl_display, fl_screen);
  owner_ = XCreateSimpleWindow(fl_display, root, -1, -1, 1, 1, 0, 0, 0);
  XSelectInput(fl_display, owner_, StructureNotifyMask);
  sde::x11::set_cardinals(owner_, "_NET_SYSTEM_TRAY_ORIENTATION", {0});
  sde::x11::set_cardinals(owner_, "_NET_SYSTEM_TRAY_ICON_SIZE", {icon_size_});
  XSetSelectionOwner(fl_display, selection_, owner_, CurrentTime);
  if (XGetSelectionOwner(fl_display, selection_) != owner_) return false;

  // Tell clients that a tray manager is now available.
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = root;
  e.xclient.message_type = sde::x11::atom("MANAGER");
  e.xclient.format = 32;
  e.xclient.data.l[0] = CurrentTime;
  e.xclient.data.l[1] = long(selection_);
  e.xclient.data.l[2] = long(owner_);
  XSendEvent(fl_display, root, False, StructureNotifyMask, &e);
  XFlush(fl_display);
  return true;
}

int Tray::width() const {
  if (icons_.empty()) return 0;
  return int(icons_.size()) * (icon_size_ + spacing_) + spacing_;
}

void Tray::place(int x, int height) {
  int y = (height - icon_size_) / 2;
  int cx = x + spacing_;
  for (Window w : icons_) {
    XMoveResizeWindow(fl_display, w, cx, y, unsigned(icon_size_), unsigned(icon_size_));
    cx += icon_size_ + spacing_;
  }
  XFlush(fl_display);
}

void Tray::dock(Window icon) {
  if (std::find(icons_.begin(), icons_.end(), icon) != icons_.end()) return;
  XWindowAttributes attr;
  if (!XGetWindowAttributes(fl_display, icon, &attr)) return;
  XSelectInput(fl_display, icon, StructureNotifyMask | PropertyChangeMask);
  // If the panel crashes, the X server puts the icon back on the root window
  // instead of destroying it.
  XAddToSaveSet(fl_display, icon);
  XReparentWindow(fl_display, icon, panel_, 0, 0);
  XResizeWindow(fl_display, icon, unsigned(icon_size_), unsigned(icon_size_));

  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = icon;
  e.xclient.message_type = sde::x11::atom("_XEMBED");
  e.xclient.format = 32;
  e.xclient.data.l[0] = CurrentTime;
  e.xclient.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
  e.xclient.data.l[3] = long(panel_);
  e.xclient.data.l[4] = 0;  // protocol version
  XSendEvent(fl_display, icon, False, NoEventMask, &e);

  // Map unless the icon explicitly asks to stay hidden.
  auto info = sde::x11::get_cardinals(icon, "_XEMBED_INFO", sde::x11::atom("_XEMBED_INFO"));
  if (info.size() < 2 || (info[1] & XEMBED_MAPPED)) XMapRaised(fl_display, icon);
  icons_.push_back(icon);
  XFlush(fl_display);
}

void Tray::undock(Window icon, bool reparent_to_root) {
  auto it = std::find(icons_.begin(), icons_.end(), icon);
  if (it == icons_.end()) return;
  icons_.erase(it);
  if (reparent_to_root) {
    XUnmapWindow(fl_display, icon);
    XReparentWindow(fl_display, icon, RootWindow(fl_display, fl_screen), 0, 0);
    XRemoveFromSaveSet(fl_display, icon);
  }
}

bool Tray::handle(const XEvent &e, bool &changed) {
  changed = false;
  if (!owner_) return false;
  if (e.type == ClientMessage && e.xclient.window == owner_ &&
      e.xclient.message_type == sde::x11::atom("_NET_SYSTEM_TRAY_OPCODE")) {
    if (e.xclient.data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
      dock(Window(e.xclient.data.l[2]));
      changed = true;
    }
    return true;
  }
  if (e.type == SelectionClear && e.xselectionclear.window == owner_) {
    for (Window w : std::vector<Window>(icons_)) undock(w, true);
    changed = true;
    return true;
  }
  Window w = 0;
  if (e.type == DestroyNotify) w = e.xdestroywindow.window;
  else if (e.type == ReparentNotify && e.xreparent.parent != panel_) w = e.xreparent.window;
  if (w && std::find(icons_.begin(), icons_.end(), w) != icons_.end()) {
    undock(w, false);
    changed = true;
    return true;
  }
  if (e.type == PropertyNotify &&
      std::find(icons_.begin(), icons_.end(), e.xproperty.window) != icons_.end()) {
    if (e.xproperty.atom == sde::x11::atom("_XEMBED_INFO")) {
      auto info = sde::x11::get_cardinals(e.xproperty.window, "_XEMBED_INFO",
                                          sde::x11::atom("_XEMBED_INFO"));
      if (info.size() >= 2) {
        if (info[1] & XEMBED_MAPPED) XMapRaised(fl_display, e.xproperty.window);
        else XUnmapWindow(fl_display, e.xproperty.window);
      }
    }
    return true;
  }
  return false;
}
