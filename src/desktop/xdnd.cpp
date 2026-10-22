// SPDX-License-Identifier: GPL-3.0-or-later
#include "xdnd.h"

#include <FL/Fl.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo-xlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sde {
namespace desk {

namespace {
const int kVersion = 5;
}

Atom Xdnd::atom(const char *name) { return XInternAtom(dpy_, name, False); }

Xdnd::Xdnd(Display *display, Window own, Hooks hooks) : dpy_(display), own_(own), hooks_(std::move(hooks)) {
  root_ = DefaultRootWindow(dpy_);
  a_aware_ = atom("XdndAware");
  a_proxy_ = atom("XdndProxy");
  a_enter_ = atom("XdndEnter");
  a_position_ = atom("XdndPosition");
  a_status_ = atom("XdndStatus");
  a_leave_ = atom("XdndLeave");
  a_drop_ = atom("XdndDrop");
  a_finished_ = atom("XdndFinished");
  a_selection_ = atom("XdndSelection");
  a_type_list_ = atom("XdndTypeList");
  a_action_list_ = atom("XdndActionList");
  a_action_desc_ = atom("XdndActionDescription");
  a_uri_list_ = atom("text/uri-list");
  a_utf8_ = atom("UTF8_STRING");
  a_text_plain_ = atom("text/plain");
  a_text_plain_utf8_ = atom("text/plain;charset=utf-8");
  a_targets_ = atom("TARGETS");
  a_data_ = atom("_SDE_XDND_DATA");
  long version = kVersion;  // SDE-FLTK announces version 4; the desktop speaks 5
  XChangeProperty(dpy_, own_, a_aware_, XA_ATOM, 32, PropModeReplace, (unsigned char *)&version, 1);
  // The window that owns XdndSelection and receives the targets' answers.
  XSetWindowAttributes wa;
  wa.override_redirect = True;
  src_win_ = XCreateWindow(dpy_, root_, -10, -10, 1, 1, 0, CopyFromParent, InputOnly, CopyFromParent,
                           CWOverrideRedirect, &wa);
  Fl::add_system_handler(system_handler, this);
}

Xdnd::~Xdnd() {
  Fl::remove_system_handler(system_handler);
  Fl::remove_timeout(deliver_drop, this);
  Fl::remove_timeout(drop_timeout, this);
  Fl::remove_timeout(source_timeout, this);
  if (icon_win_) XDestroyWindow(dpy_, icon_win_);
  if (icon_pixmap_) XFreePixmap(dpy_, icon_pixmap_);
  if (src_win_) XDestroyWindow(dpy_, src_win_);
}

unsigned Xdnd::modifiers_from_state(unsigned state) {
  unsigned m = 0;
  if (state & ShiftMask) m |= MOD_SHIFT;
  if (state & ControlMask) m |= MOD_CTRL;
  if (state & Mod1Mask) m |= MOD_ALT;
  return m;
}

DropAction Xdnd::action_of(Atom a) {
  if (!a) return DropAction::NoDrop;
  char *name = XGetAtomName(dpy_, a);
  if (!name) return DropAction::NoDrop;
  DropAction r = xdnd_action_from_name(name);
  XFree(name);
  return r;
}

Atom Xdnd::atom_of(DropAction a) {
  const char *n = xdnd_action_name(a);
  return *n ? atom(n) : None;
}

void Xdnd::send(Window to, Window window_field, Atom type, long d0, long d1, long d2, long d3, long d4) {
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.display = dpy_;
  e.xclient.window = window_field;
  e.xclient.message_type = type;
  e.xclient.format = 32;
  e.xclient.data.l[0] = d0;
  e.xclient.data.l[1] = d1;
  e.xclient.data.l[2] = d2;
  e.xclient.data.l[3] = d3;
  e.xclient.data.l[4] = d4;
  XSendEvent(dpy_, to, False, NoEventMask, &e);
  XFlush(dpy_);
}

int Xdnd::system_handler(void *event, void *data) {
  return static_cast<Xdnd *>(data)->handle(*static_cast<XEvent *>(event));
}

int Xdnd::handle(XEvent &e) {
  if (e.type == ClientMessage) {
    const XClientMessageEvent &m = e.xclient;
    if (m.window == own_) {
      if (m.message_type == a_enter_) return on_enter(m), 1;
      if (m.message_type == a_position_) return on_position(m), 1;
      if (m.message_type == a_drop_) return on_drop(m), 1;
      if (m.message_type == a_leave_) {
        if (Window(m.data.l[0]) == src_) {
          src_ = 0;
          if (hooks_.leave) hooks_.leave();
        }
        return 1;
      }
    }
    if (m.window == src_win_) {
      if (m.message_type == a_status_ && dragging_ && Window(m.data.l[0]) == target_) {
        waiting_status_ = false;
        status_accept_ = m.data.l[1] & 1;
        status_action_ = status_accept_ ? action_of(Atom(m.data.l[4])) : DropAction::NoDrop;
        if (status_accept_ && status_action_ == DropAction::NoDrop) status_action_ = DropAction::Copy;
        if (drop_pending_) {
          drop_pending_ = false;
          drop(ptime_);
        } else if (have_pending_) {
          have_pending_ = false;
          send_position();
        }
        return 1;
      }
      if (m.message_type == a_finished_ && dropped_) {
        bool accepted = target_version_ < 5 || (m.data.l[1] & 1);
        DropAction act = target_version_ >= 5 ? action_of(Atom(m.data.l[2])) : status_action_;
        if (accepted && act == DropAction::NoDrop) act = status_action_;
        finish_source(accepted, accepted ? act : DropAction::NoDrop);
        return 1;
      }
      return 1;
    }
    return 0;
  }
  if (e.type == SelectionRequest && e.xselectionrequest.selection == a_selection_ &&
      e.xselectionrequest.owner == src_win_) {
    answer_request(e.xselectionrequest);
    return 1;
  }
  if (e.type == SelectionNotify && e.xselection.requestor == own_ && e.xselection.selection == a_selection_ &&
      (e.xselection.property == a_data_ || e.xselection.property == None) && waiting_data_) {
    on_selection(e.xselection);
    return 1;
  }
  if (e.type == SelectionClear && e.xselectionclear.window == src_win_) return 1;
  return 0;
}

// ------------------------------------------------------------------ target

void Xdnd::on_enter(const XClientMessageEvent &m) {
  src_ = Window(m.data.l[0]);
  src_version_ = int((unsigned long)m.data.l[1] >> 24);
  src_types_.clear();
  if (m.data.l[1] & 1) {
    Atom type;
    int format;
    unsigned long count = 0, rest = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy_, src_, a_type_list_, 0, 0x8000, False, XA_ATOM, &type, &format, &count, &rest,
                           &data) == Success &&
        type == XA_ATOM && format == 32 && data) {
      for (unsigned long i = 0; i < count; ++i) src_types_.push_back(((Atom *)data)[i]);
    }
    if (data) XFree(data);
  }
  for (int i = 2; i < 5; ++i)
    if (m.data.l[i] && std::find(src_types_.begin(), src_types_.end(), Atom(m.data.l[i])) == src_types_.end())
      src_types_.push_back(Atom(m.data.l[i]));
  // The actions the source allows (all, when it does not say).
  src_actions_ = ActionSet();
  Atom type;
  int format;
  unsigned long count = 0, rest = 0;
  unsigned char *data = nullptr;
  if (XGetWindowProperty(dpy_, src_, a_action_list_, 0, 64, False, XA_ATOM, &type, &format, &count, &rest,
                         &data) == Success &&
      type == XA_ATOM && format == 32 && data && count) {
    ActionSet s;
    s.move = s.copy = s.link = s.ask = false;
    for (unsigned long i = 0; i < count; ++i) {
      DropAction a = action_of(((Atom *)data)[i]);
      if (a == DropAction::Move) s.move = true;
      else if (a == DropAction::Copy) s.copy = true;
      else if (a == DropAction::Link) s.link = true;
      else if (a == DropAction::Ask) s.ask = true;
    }
    if (s.move || s.copy || s.link) src_actions_ = s;
  }
  if (data) XFree(data);
  accepted_ = DropAction::NoDrop;
}

void Xdnd::on_position(const XClientMessageEvent &m) {
  Window source = Window(m.data.l[0]);
  if (source != src_) {
    // A position without an enter (or from a stale source): answer "no".
    send(source, own_, a_status_, long(own_), 0, 0, 0, None);
    return;
  }
  int x = int(((unsigned long)m.data.l[2] >> 16) & 0xFFFF), y = int((unsigned long)m.data.l[2] & 0xFFFF);
  DropAction suggested = action_of(Atom(m.data.l[4]));
  if (suggested == DropAction::NoDrop) suggested = DropAction::Copy;
  // Keyboard modifiers are read here, like GTK targets do: sources send only
  // the action they suggest.
  Window r, c;
  int rx, ry, wx, wy;
  unsigned mask = 0;
  XQueryPointer(dpy_, root_, &r, &c, &rx, &ry, &wx, &wy, &mask);
  bool files = std::find(src_types_.begin(), src_types_.end(), a_uri_list_) != src_types_.end();
  bool text = files;
  for (Atom t : src_types_)
    if (t == a_utf8_ || t == a_text_plain_ || t == a_text_plain_utf8_ || t == XA_STRING) text = true;
  DropAction a = DropAction::NoDrop;
  if (text && hooks_.over) a = hooks_.over(x, y, modifiers_from_state(mask), suggested, src_actions_, files);
  accepted_ = a;
  drop_x_ = x;
  drop_y_ = y;
  // Ask is answered as the source's suggestion, and the menu shown on drop
  // (the source does not know the final action until XdndFinished).
  DropAction reply = a == DropAction::Ask ? (src_actions_.ask ? DropAction::Ask : DropAction::Copy) : a;
  send(source, own_, a_status_, long(own_), a != DropAction::NoDrop ? 1 : 0, 0, 0,
       a != DropAction::NoDrop ? long(atom_of(reply)) : 0);
}

void Xdnd::on_drop(const XClientMessageEvent &m) {
  Window source = Window(m.data.l[0]);
  if (source != src_ || accepted_ == DropAction::NoDrop) {
    src_ = source;
    send_finished(false, DropAction::NoDrop);
    if (hooks_.leave) hooks_.leave();
    return;
  }
  Atom want = None;
  for (Atom pref : {a_uri_list_, a_utf8_, a_text_plain_utf8_, a_text_plain_, (Atom)XA_STRING})
    if (std::find(src_types_.begin(), src_types_.end(), pref) != src_types_.end()) {
      want = pref;
      break;
    }
  if (!want) {
    send_finished(false, DropAction::NoDrop);
    if (hooks_.leave) hooks_.leave();
    return;
  }
  drop_uri_list_ = want == a_uri_list_;
  waiting_data_ = true;
  unsigned long time = src_version_ >= 1 ? (unsigned long)m.data.l[2] : CurrentTime;
  XConvertSelection(dpy_, a_selection_, want, a_data_, own_, time);
  XFlush(dpy_);
  Fl::add_timeout(5.0, drop_timeout, this);
}

void Xdnd::on_selection(const XSelectionEvent &s) {
  waiting_data_ = false;
  Fl::remove_timeout(drop_timeout, this);
  drop_data_.clear();
  bool ok = false;
  if (s.property != None) {
    Atom type;
    int format;
    unsigned long count = 0, rest = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy_, own_, a_data_, 0, 0x1000000, True, AnyPropertyType, &type, &format, &count,
                           &rest, &data) == Success &&
        data && format == 8) {
      drop_data_.assign((const char *)data, count);
      if (type == XA_STRING) {
        // Latin-1 -> UTF-8.
        std::string utf8;
        for (unsigned char c : drop_data_) {
          if (c < 0x80) utf8 += char(c);
          else {
            utf8 += char(0xC0 | (c >> 6));
            utf8 += char(0x80 | (c & 0x3F));
          }
        }
        drop_data_ = utf8;
      }
      ok = true;
    }
    if (data) XFree(data);
  }
  if (!ok) {
    send_finished(false, DropAction::NoDrop);
    if (hooks_.leave) hooks_.leave();
    return;
  }
  // Outside X event dispatch, so that the drop may open menus or dialogs.
  Fl::add_timeout(0, deliver_drop, this);
}

void Xdnd::deliver_drop(void *data) {
  auto *self = static_cast<Xdnd *>(data);
  Window source = self->src_;
  std::string payload;
  payload.swap(self->drop_data_);
  DropAction done = DropAction::NoDrop;
  if (self->hooks_.drop)
    done = self->hooks_.drop(self->drop_x_, self->drop_y_, self->accepted_, payload, self->drop_uri_list_);
  self->src_ = source;
  self->send_finished(done != DropAction::NoDrop, done);
  if (self->hooks_.leave) self->hooks_.leave();
}

void Xdnd::drop_timeout(void *data) {
  auto *self = static_cast<Xdnd *>(data);
  if (!self->waiting_data_) return;
  self->waiting_data_ = false;
  self->send_finished(false, DropAction::NoDrop);
  if (self->hooks_.leave) self->hooks_.leave();
}

void Xdnd::send_finished(bool accepted, DropAction action) {
  if (!src_) return;
  if (src_version_ >= 5)
    send(src_, own_, a_finished_, long(own_), accepted ? 1 : 0, accepted ? long(atom_of(action)) : 0, 0, 0);
  else
    send(src_, own_, a_finished_, long(own_), 0, 0, 0, 0);
  src_ = 0;
}

// ------------------------------------------------------------------ source

int Xdnd::aware_version(Window w, Window *proxy) {
  *proxy = 0;
  Atom type;
  int format;
  unsigned long count = 0, rest = 0;
  unsigned char *data = nullptr;
  // XdndProxy: messages go to the proxy window (which must point to itself).
  if (XGetWindowProperty(dpy_, w, a_proxy_, 0, 1, False, XA_WINDOW, &type, &format, &count, &rest, &data) ==
          Success &&
      type == XA_WINDOW && data && count) {
    Window p = *(Window *)data;
    XFree(data);
    data = nullptr;
    if (XGetWindowProperty(dpy_, p, a_proxy_, 0, 1, False, XA_WINDOW, &type, &format, &count, &rest, &data) ==
            Success &&
        type == XA_WINDOW && data && count && *(Window *)data == p)
      *proxy = p;
  }
  if (data) XFree(data);
  data = nullptr;
  int version = 0;
  Window probe = *proxy ? *proxy : w;
  if (XGetWindowProperty(dpy_, probe, a_aware_, 0, 1, False, XA_ATOM, &type, &format, &count, &rest, &data) ==
          Success &&
      type == XA_ATOM && data && count)
    version = int(*(Atom *)data);
  if (data) XFree(data);
  return version;
}

Window Xdnd::find_target(int x, int y, int *version, Window *proxy) {
  *version = 0;
  *proxy = 0;
  Window root_ret, parent;
  Window *children = nullptr;
  unsigned n = 0;
  if (!XQueryTree(dpy_, root_, &root_ret, &parent, &children, &n)) return 0;
  Window top = 0;
  for (int i = int(n) - 1; i >= 0; --i) {  // topmost first
    Window c = children[i];
    if (c == icon_win_ || c == src_win_) continue;
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy_, c, &a) || a.map_state != IsViewable || a.c_class != InputOutput) continue;
    if (x >= a.x && y >= a.y && x < a.x + a.width + 2 * a.border_width &&
        y < a.y + a.height + 2 * a.border_width) {
      top = c;
      break;
    }
  }
  if (children) XFree(children);
  // Down from the top-level (often a window manager frame) to the first
  // XDND-aware window under the pointer.
  for (Window w = top; w;) {
    int v = aware_version(w, proxy);
    if (v) {
      *version = v;
      return w;
    }
    Window child = 0;
    int cx, cy;
    if (!XTranslateCoordinates(dpy_, root_, w, x, y, &cx, &cy, &child)) break;
    w = child;
  }
  return 0;
}

void Xdnd::show_icon(cairo_surface_t *icon, int size, int count) {
  int w = size + size / 3;
  icon_w_ = w;
  int screen = DefaultScreen(dpy_);
  XVisualInfo vi;
  bool argb = XMatchVisualInfo(dpy_, screen, 32, TrueColor, &vi);
  XSetWindowAttributes wa;
  wa.override_redirect = True;
  wa.save_under = True;
  unsigned long mask = CWOverrideRedirect | CWSaveUnder;
  Visual *visual = DefaultVisual(dpy_, screen);
  int depth = DefaultDepth(dpy_, screen);
  if (argb) {
    icon_colormap_ = XCreateColormap(dpy_, root_, vi.visual, AllocNone);
    wa.colormap = icon_colormap_;
    wa.border_pixel = 0;
    wa.background_pixel = 0;
    mask |= CWColormap | CWBorderPixel | CWBackPixel;
    visual = vi.visual;
    depth = 32;
  }
  icon_win_ = XCreateWindow(dpy_, root_, -w, -w, unsigned(w), unsigned(w), 0, depth, InputOutput, visual,
                            mask, &wa);
  Atom type = atom("_NET_WM_WINDOW_TYPE"), dnd = atom("_NET_WM_WINDOW_TYPE_DND");
  XChangeProperty(dpy_, icon_win_, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dnd, 1);
  // Never the target of the drop: an empty input region where supported is
  // not needed, find_target() skips this window.
  icon_pixmap_ = XCreatePixmap(dpy_, icon_win_, unsigned(w), unsigned(w), unsigned(depth));
  cairo_surface_t *s = cairo_xlib_surface_create(dpy_, icon_pixmap_, visual, w, w);
  cairo_t *cr = cairo_create(s);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  if (argb) cairo_set_source_rgba(cr, 0, 0, 0, 0);
  else cairo_set_source_rgb(cr, 0.11, 0.44, 0.54);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  if (icon) {
    double iw = cairo_image_surface_get_width(icon), ih = cairo_image_surface_get_height(icon);
    double sc = size / std::max(iw, ih);
    cairo_save(cr);
    cairo_translate(cr, (w - size) / 2.0, (w - size) / 2.0);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, icon, 0, 0);
    cairo_paint_with_alpha(cr, argb ? 0.8 : 1.0);
    cairo_restore(cr);
  }
  if (count > 1) {
    std::string label = std::to_string(count);
    double r = std::max(9.0, size / 4.5);
    double cx = w - r - 1, cy = r + 1;
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.85, 0.2, 0.2);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, r * 1.1);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, label.c_str(), &ext);
    cairo_move_to(cr, cx - ext.width / 2 - ext.x_bearing, cy - ext.height / 2 - ext.y_bearing);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_show_text(cr, label.c_str());
  }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  cairo_surface_destroy(s);
  XSetWindowBackgroundPixmap(dpy_, icon_win_, icon_pixmap_);
  XMapRaised(dpy_, icon_win_);
}

void Xdnd::begin_drag(const std::vector<std::string> &paths, cairo_surface_t *icon, int icon_size, int count,
                      unsigned long time) {
  if (dragging_) cancel();
  paths_ = paths;
  uri_list_ = format_uri_list(paths);
  dragging_ = true;
  dropped_ = false;
  target_ = target_proxy_ = 0;
  target_version_ = 0;
  waiting_status_ = have_pending_ = drop_pending_ = status_accept_ = false;
  status_action_ = DropAction::NoDrop;
  Atom types[] = {a_uri_list_, a_utf8_, a_text_plain_utf8_, a_text_plain_, XA_STRING};
  XChangeProperty(dpy_, src_win_, a_type_list_, XA_ATOM, 32, PropModeReplace, (unsigned char *)types, 5);
  Atom actions[] = {atom("XdndActionCopy"), atom("XdndActionMove"), atom("XdndActionLink"),
                    atom("XdndActionAsk")};
  XChangeProperty(dpy_, src_win_, a_action_list_, XA_ATOM, 32, PropModeReplace, (unsigned char *)actions, 4);
  const char desc[] = "Copy\0Move\0Link\0Ask\0";
  XChangeProperty(dpy_, src_win_, a_action_desc_, XA_STRING, 8, PropModeReplace, (const unsigned char *)desc,
                  sizeof desc - 1);
  XSetSelectionOwner(dpy_, a_selection_, src_win_, time ? time : CurrentTime);
  show_icon(icon, icon_size, count);
}

void Xdnd::send_position() {
  if (!target_) return;
  // The suggestion follows the GTK/Liria conventions: Ctrl copies, Shift
  // moves, Ctrl+Shift links, Alt asks; plain drags suggest a copy and the
  // target picks (file managers move within one file system).
  DropAction a = DropAction::Copy;
  if ((pmods_ & MOD_CTRL) && (pmods_ & MOD_SHIFT)) a = DropAction::Link;
  else if (pmods_ & MOD_SHIFT) a = DropAction::Move;
  else if (pmods_ & MOD_ALT) a = DropAction::Ask;
  send(target_proxy_ ? target_proxy_ : target_, target_, a_position_, long(src_win_), 0,
       long((px_ << 16) | (py_ & 0xFFFF)), long(ptime_), long(atom_of(a)));
  waiting_status_ = true;
}

void Xdnd::leave_target() {
  if (target_) send(target_proxy_ ? target_proxy_ : target_, target_, a_leave_, long(src_win_), 0, 0, 0, 0);
  target_ = target_proxy_ = 0;
  target_version_ = 0;
  waiting_status_ = have_pending_ = false;
  status_accept_ = false;
  status_action_ = DropAction::NoDrop;
}

bool Xdnd::motion(int x, int y, unsigned mods, unsigned long time) {
  if (!dragging_ || dropped_) return false;
  if (icon_win_) XMoveWindow(dpy_, icon_win_, x - icon_w_ / 2, y - icon_w_ / 2);
  px_ = x;
  py_ = y;
  pmods_ = mods;
  ptime_ = time;
  int version = 0;
  Window proxy = 0;
  Window t = find_target(x, y, &version, &proxy);
  if (t == own_) {
    if (target_) leave_target();
    return true;
  }
  if (t != target_) {
    leave_target();
    if (t && version >= 3) {
      target_ = t;
      target_proxy_ = proxy;
      target_version_ = std::min(version, kVersion);
      Atom types[] = {a_uri_list_, a_utf8_, a_text_plain_utf8_, a_text_plain_, XA_STRING};
      send(proxy ? proxy : t, t, a_enter_, long(src_win_), long(target_version_) << 24 | 1, long(types[0]),
           long(types[1]), long(types[2]));
    }
  }
  if (target_) {
    if (waiting_status_) have_pending_ = true;
    else send_position();
  }
  XFlush(dpy_);
  return false;
}

void Xdnd::drop(unsigned long time) {
  if (!dragging_ || dropped_) return;
  ptime_ = time;
  if (target_ && waiting_status_) {
    // Decide once the target answered the last position.
    drop_pending_ = true;
    Fl::add_timeout(2.0, source_timeout, this);
    return;
  }
  if (target_ && status_accept_) {
    dropped_ = true;
    send(target_proxy_ ? target_proxy_ : target_, target_, a_drop_, long(src_win_), 0, long(time), 0, 0);
    // Targets that never answer must not keep the drag alive.
    Fl::remove_timeout(source_timeout, this);
    Fl::add_timeout(30.0, source_timeout, this);
    if (icon_win_) XUnmapWindow(dpy_, icon_win_);
    return;
  }
  leave_target();
  finish_source(false, DropAction::NoDrop);
}

void Xdnd::source_timeout(void *data) {
  auto *self = static_cast<Xdnd *>(data);
  if (!self->dragging_) return;
  if (self->drop_pending_) {
    self->drop_pending_ = false;
    self->waiting_status_ = false;
    if (self->status_accept_) {
      self->drop(self->ptime_);
      return;
    }
    self->leave_target();
    self->finish_source(false, DropAction::NoDrop);
    return;
  }
  // No XdndFinished: assume the target did what it said.
  self->finish_source(self->status_accept_, self->status_action_);
}

void Xdnd::cancel() {
  if (!dragging_) return;
  leave_target();
  dragging_ = dropped_ = false;
  Fl::remove_timeout(source_timeout, this);
  if (icon_win_) XDestroyWindow(dpy_, icon_win_);
  if (icon_pixmap_) XFreePixmap(dpy_, icon_pixmap_);
  if (icon_colormap_) XFreeColormap(dpy_, icon_colormap_);
  icon_win_ = 0;
  icon_pixmap_ = 0;
  icon_colormap_ = 0;
  XFlush(dpy_);
}

void Xdnd::finish_source(bool accepted, DropAction action) {
  target_ = target_proxy_ = 0;
  cancel();
  if (hooks_.finished) hooks_.finished(accepted, action);
}

void Xdnd::answer_request(const XSelectionRequestEvent &r) {
  XEvent reply;
  memset(&reply, 0, sizeof reply);
  reply.xselection.type = SelectionNotify;
  reply.xselection.display = dpy_;
  reply.xselection.requestor = r.requestor;
  reply.xselection.selection = r.selection;
  reply.xselection.target = r.target;
  reply.xselection.time = r.time;
  Atom property = r.property ? r.property : r.target;
  reply.xselection.property = None;
  if (r.target == a_targets_) {
    Atom types[] = {a_targets_, a_uri_list_, a_utf8_, a_text_plain_utf8_, a_text_plain_, XA_STRING};
    XChangeProperty(dpy_, r.requestor, property, XA_ATOM, 32, PropModeReplace, (unsigned char *)types, 6);
    reply.xselection.property = property;
  } else if (r.target == a_uri_list_ || r.target == a_utf8_ || r.target == a_text_plain_utf8_ ||
             r.target == a_text_plain_ || r.target == XA_STRING) {
    // Every text type carries the URI list: SDE-FLTK programs (Niny,
    // Xanara ...) and GTK read file:// lines from plain text drops.
    XChangeProperty(dpy_, r.requestor, property, r.target, 8, PropModeReplace,
                    (const unsigned char *)uri_list_.data(), int(uri_list_.size()));
    reply.xselection.property = property;
  }
  XSendEvent(dpy_, r.requestor, False, NoEventMask, &reply);
  XFlush(dpy_);
}

}  // namespace desk
}  // namespace sde
