// SPDX-License-Identifier: GPL-3.0-or-later
#include "tray.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/extensions/shape.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include "indicator_policy.h"

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
  if (host_) XDestroyWindow(fl_display, host_);
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
  parent_ = panel_;
  host_ = XCreateSimpleWindow(fl_display, panel_, 0, 0, 1, 1, 0, 0, 0);
  XStoreName(fl_display, host_, "SDE System Tray");
  XSelectInput(fl_display, host_, StructureNotifyMask | SubstructureNotifyMask | SubstructureRedirectMask);
  sde::x11::set_cardinals(owner_, "_NET_SYSTEM_TRAY_ORIENTATION", {0});
  sde::x11::set_cardinals(owner_, "_NET_SYSTEM_TRAY_ICON_SIZE", {icon_size_});
  unsigned long visual = XVisualIDFromVisual(DefaultVisual(fl_display, fl_screen));
  XChangeProperty(fl_display, owner_, sde::x11::atom("_NET_SYSTEM_TRAY_VISUAL"), XA_VISUALID,
                  32, PropModeReplace, reinterpret_cast<unsigned char *>(&visual), 1);
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
  if (!count()) return 0;
  return count() * (icon_size_ + spacing_) + spacing_;
}

int Tray::grid_height(int columns) const {
  if (!count()) return 0;
  columns = std::max(1, columns);
  return ((count() + columns - 1) / columns) * (icon_size_ + spacing_) + spacing_;
}
void Tray::hide() {
  if (host_) XUnmapWindow(fl_display, host_);
}
void Tray::place(int x, int height) {
  place_in(panel_, x, (height - icon_size_ - 2 * spacing_) / 2, std::max(1, count()));
}
void Tray::place_in(Window parent, int x, int y, int columns, int clip_top, int clip_height) {
  const auto visible = visible_icons();
  for (Window icon : icons_)
    if (std::find(visible.begin(), visible.end(), icon) == visible.end()) XUnmapWindow(fl_display, icon);
  if (!host_ || visible.empty()) {
    hide();
    return;
  }
  columns = std::max(1, columns);
  if (parent_ != parent) {
    XReparentWindow(fl_display, host_, parent, x, y);
    parent_ = parent;
  }
  int width = std::min(columns, count()) * (icon_size_ + spacing_) + spacing_,
      height = grid_height(columns);
  XMoveResizeWindow(fl_display, host_, x, y, unsigned(width), unsigned(height));
  int top = std::max(0, clip_top), bottom = std::min(height, clip_top + clip_height);
  if (bottom <= top) {
    hide();
    return;
  }
  XRectangle rect{0, short(top), static_cast<unsigned short>(width),
                  static_cast<unsigned short>(bottom - top)};
  XShapeCombineRectangles(fl_display, host_, ShapeBounding, 0, 0, &rect, 1, ShapeSet, Unsorted);
  XShapeCombineRectangles(fl_display, host_, ShapeInput, 0, 0, &rect, 1, ShapeSet, Unsorted);
  for (size_t i = 0; i < visible.size(); ++i) {
    XMoveResizeWindow(fl_display, visible[i],
                      spacing_ + int(i % size_t(columns)) * (icon_size_ + spacing_),
                      spacing_ + int(i / size_t(columns)) * (icon_size_ + spacing_),
                      unsigned(icon_size_), unsigned(icon_size_));
    XMapRaised(fl_display, visible[i]);
  }
  XMapRaised(fl_display, host_);
  XFlush(fl_display);
}

std::vector<Window> Tray::visible_icons() const {
  std::vector<Window> result;
  for (Window icon : icons_) {
    auto it = states_.find(icon);
    if (it != states_.end() && it->second.mapped && !(it->second.role & native_roles_)) result.push_back(icon);
  }
  return result;
}
bool Tray::has_input_indicator() const {
  for (const auto &entry : states_)
    if (entry.second.mapped && entry.second.role == indicator::Input) return true;
  return false;
}
void Tray::update_identity(Window icon) {
  XClassHint hint{};
  if (XGetClassHint(fl_display, icon, &hint)) {
    unsigned role = unsigned(indicator::role(hint.res_class ? hint.res_class : ""));
    if (!role) role = unsigned(indicator::role(hint.res_name ? hint.res_name : ""));
    states_[icon].role = role;
    if (hint.res_class) XFree(hint.res_class);
    if (hint.res_name) XFree(hint.res_name);
  }
}

void Tray::dock(Window icon) {
  if (!host_ || !icon || icon == host_ || icon == owner_ || icon == panel_ ||
      icon == RootWindow(fl_display, fl_screen)) return;
  if (std::find(icons_.begin(), icons_.end(), icon) != icons_.end()) return;
  XWindowAttributes attr;
  if (!XGetWindowAttributes(fl_display, icon, &attr)) return;
  XSelectInput(fl_display, icon, StructureNotifyMask | PropertyChangeMask);
  // If the panel crashes, the X server puts the icon back on the root window
  // instead of destroying it.
  XAddToSaveSet(fl_display, icon);
  XReparentWindow(fl_display, icon, host_, 0, 0);
  XResizeWindow(fl_display, icon, unsigned(icon_size_), unsigned(icon_size_));

  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = icon;
  e.xclient.message_type = sde::x11::atom("_XEMBED");
  e.xclient.format = 32;
  e.xclient.data.l[0] = CurrentTime;
  e.xclient.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
  e.xclient.data.l[3] = long(host_);
  e.xclient.data.l[4] = 0;  // protocol version
  XSendEvent(fl_display, icon, False, NoEventMask, &e);

  // Map unless the icon explicitly asks to stay hidden.
  auto info = sde::x11::get_cardinals(icon, "_XEMBED_INFO", sde::x11::atom("_XEMBED_INFO"));
  states_[icon].mapped = info.size() < 2 || (info[1] & XEMBED_MAPPED);
  update_identity(icon);
  icons_.push_back(icon);
  XFlush(fl_display);
}

void Tray::undock(Window icon, bool reparent_to_root) {
  auto it = std::find(icons_.begin(), icons_.end(), icon);
  if (it == icons_.end()) return;
  icons_.erase(it);
  states_.erase(icon);
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
  if ((e.type == ConfigureRequest || e.type == MapRequest || e.type == ConfigureNotify) &&
      states_.count(e.type == ConfigureRequest ? e.xconfigurerequest.window :
                    e.type == MapRequest ? e.xmaprequest.window : e.xconfigure.window)) {
    Window icon = e.type == ConfigureRequest ? e.xconfigurerequest.window :
                  e.type == MapRequest ? e.xmaprequest.window : e.xconfigure.window;
    if (e.type != ConfigureNotify || e.xconfigure.width != icon_size_ || e.xconfigure.height != icon_size_)
      XResizeWindow(fl_display, icon, unsigned(icon_size_), unsigned(icon_size_));
    if (e.type == MapRequest) changed = true;
    return true;
  }
  Window w = 0;
  if (e.type == DestroyNotify)
    w = e.xdestroywindow.window;
  else if (e.type == ReparentNotify && e.xreparent.parent != host_)
    w = e.xreparent.window;
  if (w && std::find(icons_.begin(), icons_.end(), w) != icons_.end()) {
    undock(w, false);
    changed = true;
    return true;
  }
  if (e.type == PropertyNotify &&
      std::find(icons_.begin(), icons_.end(), e.xproperty.window) != icons_.end()) {
    if (e.xproperty.atom == sde::x11::atom("_XEMBED_INFO")) {
      auto info = sde::x11::get_cardinals(e.xproperty.window, "_XEMBED_INFO", sde::x11::atom("_XEMBED_INFO"));
      states_[e.xproperty.window].mapped = info.size() < 2 || (info[1] & XEMBED_MAPPED);
      changed = true;
    } else if (e.xproperty.atom == XA_WM_CLASS) {
      update_identity(e.xproperty.window);
      changed = true;
    }
    return true;
  }
  return false;
}

void Tray::set_background(unsigned long pixel) {
  if (host_ && background_ != pixel) {
    background_ = pixel;
    XSetWindowBackground(fl_display, host_, pixel);
    XClearWindow(fl_display, host_);
  }
}
