// SPDX-License-Identifier: GPL-3.0-or-later
#include "tray.h"

#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#if SDE_HAVE_XCOMPOSITE && SDE_HAVE_XDAMAGE
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <cairo-xlib.h>
#define SDE_TRAY_COMPOSITE 1
#endif

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "status_model.h"
#include "x11.h"

namespace {
constexpr long SYSTEM_TRAY_REQUEST_DOCK = 0;
constexpr long XEMBED_EMBEDDED_NOTIFY = 0;
constexpr long XEMBED_MAPPED = 1;
// At most this many icons; dock requests handled per batch.
constexpr size_t kMaxIcons = 64, kBatch = 16, kMaxRequests = 256;

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// SDE_TRAY_DEBUG=1: what the tray does, on stderr (the session log).
bool debug() {
  static const bool on = [] {
    const char *v = getenv("SDE_TRAY_DEBUG");
    return v && *v && *v != '0';
  }();
  return on;
}
#define TRAY_LOG(...)                                \
  do {                                               \
    if (debug()) {                                   \
      fprintf(stderr, "sde-panel: tray: " __VA_ARGS__); \
      fputc('\n', stderr);                           \
    }                                                \
  } while (0)

// Requests on the windows of tray programs can fail at any time (the window
// is gone, its program quit): their errors are counted here, not reported.
int g_trapped = 0;
int trap_errors(Display *, XErrorEvent *) {
  ++g_trapped;
  return 0;
}
class ErrorTrap {
 public:
  ErrorTrap() {
    XSync(fl_display, False);  // earlier errors go to the usual handler
    old_ = XSetErrorHandler(trap_errors);
    before_ = g_trapped;
  }
  ~ErrorTrap() {
    XSync(fl_display, False);
    XSetErrorHandler(old_);
  }
  bool failed() {
    XSync(fl_display, False);
    return g_trapped != before_;
  }

 private:
  XErrorHandler old_;
  int before_ = 0;
};

std::string icon_class(Window icon) {
  XClassHint hint{nullptr, nullptr};
  if (!XGetClassHint(fl_display, icon, &hint)) return "";
  std::string v = hint.res_name ? hint.res_name : "";
  if (hint.res_class) v += std::string(" ") + hint.res_class;
  if (hint.res_name) XFree(hint.res_name);
  if (hint.res_class) XFree(hint.res_class);
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return char(tolower(c)); });
  return v;
}

// The XEMBED_MAPPED flag of _XEMBED_INFO; |present| tells whether the
// window has the property at all (every XEmbed client sets it).
bool embed_mapped(Window w, bool *present = nullptr) {
  auto info = sde::x11::get_cardinals(w, "_XEMBED_INFO", sde::x11::atom("_XEMBED_INFO"));
  if (present) *present = info.size() >= 2;
  return info.size() < 2 || (info[1] & XEMBED_MAPPED);
}

bool in_client_list(Window w) {
  for (Window c : sde::x11::get_windows(RootWindow(fl_display, fl_screen), "_NET_CLIENT_LIST"))
    if (c == w) return true;
  return false;
}
}  // namespace

Tray::Tray(Window panel, std::function<void()> changed) : changed_(std::move(changed)), panel_(panel) {}

Tray::~Tray() {
  Fl::remove_timeout(process_requests, this);
  Fl::remove_timeout(remap_check, this);
  while (!icons_.empty()) undock(icons_.size() - 1, true);
  if (host_) XDestroyWindow(fl_display, host_);
  if (owner_) XDestroyWindow(fl_display, owner_);
  XFlush(fl_display);
}

void Tray::set_icon_size(int size) {
  icon_size_ = size;
  spacing_ = std::max(4, size / 4);
  if (owner_) sde::x11::set_cardinals(owner_, "_NET_SYSTEM_TRAY_ICON_SIZE", {icon_size_});
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
  XWindowAttributes pa;
  XGetWindowAttributes(fl_display, panel_, &pa);
  host_depth_ = pa.depth;
  // The theme's colour comes with the first set_background(); until then the
  // host is never shown black (pixel 0) but in the panel's own background.
  host_ = XCreateSimpleWindow(fl_display, panel_, 0, 0, 1, 1, 0, 0, background_);
  XStoreName(fl_display, host_, "SDE System Tray");
  XSelectInput(fl_display, host_, StructureNotifyMask | SubstructureNotifyMask | ExposureMask);
  sde::x11::set_cardinals(owner_, "_NET_SYSTEM_TRAY_ORIENTATION", {0});
  sde::x11::set_cardinals(owner_, "_NET_SYSTEM_TRAY_ICON_SIZE", {icon_size_});
  // The panel's visual: GTK and Qt make their icons with it (ParentRelative
  // backgrounds, no alpha channel the X server would drop).
  {
    unsigned long visual = XVisualIDFromVisual(pa.visual);
    XChangeProperty(fl_display, owner_, sde::x11::atom("_NET_SYSTEM_TRAY_VISUAL"), XA_VISUALID, 32,
                    PropModeReplace, reinterpret_cast<unsigned char *>(&visual), 1);
  }
#ifdef SDE_TRAY_COMPOSITE
  int event = 0, error = 0, major = 0, minor = 2;
  composite_ = XCompositeQueryExtension(fl_display, &event, &error) &&
               XCompositeQueryVersion(fl_display, &major, &minor) && (major > 0 || minor >= 2) &&
               XDamageQueryExtension(fl_display, &damage_event_, &error);
#endif
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

int Tray::count() const {
  int n = 0;
  for (const auto &i : icons_) n += i.want_mapped ? 1 : 0;
  return n;
}

int Tray::width() const {
  int n = count();
  if (!n) return 0;
  return n * (icon_size_ + spacing_) + spacing_;
}

int Tray::grid_height(int columns) const {
  int n = count();
  if (!n) return 0;
  columns = std::max(1, columns);
  return ((n + columns - 1) / columns) * (icon_size_ + spacing_) + spacing_;
}

void Tray::hide() {
  if (host_ && host_mapped_) XUnmapWindow(fl_display, host_);
  host_mapped_ = false;
}

void Tray::park() {
  if (!host_) return;
  hide();
  if (parent_ != panel_) {
    XReparentWindow(fl_display, host_, panel_, 0, 0);
    parent_ = panel_;
    host_x_ = host_y_ = 0;
  }
}

void Tray::place(int x, int height) {
  place_in(panel_, x, (height - icon_size_ - 2 * spacing_) / 2, std::max(1, count()));
}

void Tray::place_in(Window parent, int x, int y, int columns, int clip_top, int clip_height) {
  if (!host_ || !count()) {
    hide();
    return;
  }
  columns = std::max(1, columns);
  columns_ = columns;
  int n = count();
  int width = std::min(columns, n) * (icon_size_ + spacing_) + spacing_, height = grid_height(columns);
  // Only what changed is sent: the Tray flyout places the host on every
  // redraw, and every request on an icon wakes its program.
  if (parent_ != parent) {
    XReparentWindow(fl_display, host_, parent, x, y);
    parent_ = parent;
    host_mapped_ = false;
    host_x_ = x, host_y_ = y;
  }
  if (x != host_x_ || y != host_y_ || width != host_w_ || height != host_h_) {
    XMoveResizeWindow(fl_display, host_, x, y, unsigned(width), unsigned(height));
    host_x_ = x, host_y_ = y, host_w_ = width, host_h_ = height;
  }
  int top = std::max(0, clip_top), bottom = std::min(height, clip_top + clip_height);
  if (bottom <= top) {
    hide();
    return;
  }
  XRectangle rect{0, short(top), static_cast<unsigned short>(width),
                  static_cast<unsigned short>(bottom - top)};
  XShapeCombineRectangles(fl_display, host_, ShapeBounding, 0, 0, &rect, 1, ShapeSet, Unsorted);
  XShapeCombineRectangles(fl_display, host_, ShapeInput, 0, 0, &rect, 1, ShapeSet, Unsorted);
  int slot = 0;
  for (auto &icon : icons_) {
    if (!icon.want_mapped) continue;
    int ix = spacing_ + (slot % columns) * (icon_size_ + spacing_),
        iy = spacing_ + (slot / columns) * (icon_size_ + spacing_);
    ++slot;
    if (ix != icon.x || iy != icon.y || icon.size != icon_size_) {
      XMoveResizeWindow(fl_display, icon.win, ix, iy, unsigned(icon_size_), unsigned(icon_size_));
      icon.x = ix, icon.y = iy, icon.size = icon_size_;
#ifdef SDE_TRAY_COMPOSITE
      // A resized window gets a new pixmap.
      if (icon.composited && icon.pixmap) {
        XFreePixmap(fl_display, icon.pixmap);
        icon.pixmap = 0;
      }
#endif
    }
  }
  if (!host_mapped_) {
    XMapRaised(fl_display, host_);
    host_mapped_ = true;
  }
  XFlush(fl_display);
}

Tray::Icon *Tray::find(Window w) {
  for (auto &i : icons_)
    if (i.win == w) return &i;
  return nullptr;
}

bool Tray::has_input_method() const {
  for (const auto &icon : icons_) {
    const std::string &c = icon.cls;
    for (const auto &part : {c.substr(0, c.find(' ')), c.substr(c.find(' ') + 1)})
      if (keyboard_applet(part) == KeyboardApplet::INPUT_METHOD) return true;
  }
  return false;
}

bool Tray::hidden(Window icon) const {
  if (hide_layout_applets_) {
    std::string c = icon_class(icon);
    for (const auto &part : {c.substr(0, c.find(' ')), c.substr(c.find(' ') + 1)})
      if (keyboard_applet(part) == KeyboardApplet::LAYOUT) return true;
  }
  if (hidden_.empty()) return false;
  XClassHint hint{nullptr, nullptr};
  if (!XGetClassHint(fl_display, icon, &hint)) return false;
  bool match = false;
  for (const char *part : {hint.res_name, hint.res_class}) {
    if (!part) continue;
    std::string v(part);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return char(tolower(c)); });
    for (const auto &h : hidden_)
      if (v == h) match = true;
  }
  if (hint.res_name) XFree(hint.res_name);
  if (hint.res_class) XFree(hint.res_class);
  return match;
}

void Tray::changed() {
  if (changed_) changed_();
}

// Dock requests wait for the next turn of the event loop and are handled
// a few at a time: a program that creates and destroys icons in a loop
// leaves mostly windows that are already gone, and the panel keeps drawing.
void Tray::process_requests(void *data) {
  auto *self = static_cast<Tray *>(data);
  bool any = false;
  size_t handled = 0;
  while (!self->requests_.empty() && handled < kBatch) {
    Window w = self->requests_.front();
    self->requests_.erase(self->requests_.begin());
    ++handled;
    any = self->dock(w) || any;
  }
  if (!self->requests_.empty()) Fl::repeat_timeout(0.02, process_requests, data);
  if (any) self->changed();
}

bool Tray::dock(Window icon) {
  Window root = RootWindow(fl_display, fl_screen);
  if (!icon || icon == root || icon == owner_ || icon == host_ || icon == panel_ || fl_find(icon)) {
    TRAY_LOG("refused 0x%lx: not a tray icon", icon);
    return false;
  }
  double t = now();
  auto &recent = recent_[icon];
  if (t - recent.first > 3.0) recent = {t, 0};
  if (++recent.second > 8) {
    if (recent.second == 9) TRAY_LOG("0x%lx docks again and again: ignored for a while", icon);
    return false;
  }
  if (recent_.size() > 512)
    for (auto it = recent_.begin(); it != recent_.end();)
      it = t - it->second.first > 3.0 ? recent_.erase(it) : std::next(it);

  ErrorTrap trap;
  Window r = 0, parent = 0, *kids = nullptr;
  unsigned n = 0;
  if (!XQueryTree(fl_display, icon, &r, &parent, &kids, &n)) return false;
  if (kids) XFree(kids);
  if (Icon *known = find(icon)) {
    // The same window again: still in the host, it only asks to be shown;
    // elsewhere it is a new icon that got the id of one that is gone.
    if (parent == host_) {
      apply_mapped(*known);
      return true;
    }
    undock(size_t(known - icons_.data()), false);
  }
  if (icons_.size() >= kMaxIcons) {
    TRAY_LOG("refused 0x%lx: %zu icons already", icon, icons_.size());
    return false;
  }
  XWindowAttributes attr;
  if (!XGetWindowAttributes(fl_display, icon, &attr) || attr.c_class != InputOutput || attr.depth == 0)
    return false;
  bool has_info = false;
  bool mapped = embed_mapped(icon, &has_info);
  // Tray icons are top-level windows that are not on the screen yet (or
  // override-redirect ones, as Qt makes them). A window inside another one
  // is only taken when it is an XEmbed client (a tray icon that a window
  // manager framed after a tray crashed); a normal application window never.
  if (parent != root ? !has_info
                     : (!has_info && attr.map_state != IsUnmapped && !attr.override_redirect) ||
                           (!has_info && in_client_list(icon))) {
    TRAY_LOG("refused 0x%lx: an application window", icon);
    return false;
  }
  if (trap.failed()) return false;
  // The application keeps running (nm-applet is also the Wi-Fi secret agent);
  // only its second icon next to the native indicator is left out.
  if (hidden(icon)) return false;

  Icon ic;
  ic.win = icon;
  ic.cls = icon_class(icon);
  ic.depth = attr.depth;
  ic.want_mapped = mapped;
  XSelectInput(fl_display, icon, StructureNotifyMask | PropertyChangeMask);
  // If the panel crashes, the X server puts the icon back on the root window
  // instead of destroying it, and leaves it unmapped there (XFixes, as GTK's
  // own sockets do): with a plain save-set it was mapped, and the window
  // manager framed it in the middle of the screen until the program noticed.
  XFixesChangeSaveSet(fl_display, icon, SetModeInsert, SaveSetRoot, SaveSetUnmap);
  if (attr.map_state != IsUnmapped) ++ic.own_unmaps;  // the reparent unmaps it
  XReparentWindow(fl_display, icon, host_, 0, 0);
  XResizeWindow(fl_display, icon, unsigned(icon_size_), unsigned(icon_size_));
  ic.size = icon_size_;

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
  if (trap.failed()) {
    // Gone while it was being docked.
    XRemoveFromSaveSet(fl_display, icon);
    TRAY_LOG("0x%lx went away while docking", icon);
    return false;
  }
  icons_.push_back(ic);
  Icon &added = icons_.back();
#ifdef SDE_TRAY_COMPOSITE
  if (composite_ && attr.depth != host_depth_) composite_setup(added, attr.visual);
#endif
  if (added.want_mapped) XMapRaised(fl_display, icon);
  TRAY_LOG("docked 0x%lx (%s, depth %d%s)", icon, added.cls.c_str(), added.depth,
           added.composited ? ", drawn with its alpha" : "");
  XFlush(fl_display);
  return true;
}

void Tray::undock(size_t index, bool give_back) {
  if (index >= icons_.size()) return;
  Icon icon = icons_[index];
  icons_.erase(icons_.begin() + long(index));
  gone_.push_back({icon.win, now()});
  if (gone_.size() > 64) gone_.erase(gone_.begin());
  TRAY_LOG("undocked 0x%lx%s", icon.win, give_back ? " (given back)" : "");
  ErrorTrap trap;
  composite_release(icon);
  if (give_back) {
    // Back to the root window, unmapped, for the next tray.
    XUnmapWindow(fl_display, icon.win);
    XReparentWindow(fl_display, icon.win, RootWindow(fl_display, fl_screen), 0, 0);
    XRemoveFromSaveSet(fl_display, icon.win);
  } else {
    XSelectInput(fl_display, icon.win, NoEventMask);
  }
}

// Icons that something else unmapped while they should be shown are mapped
// again, three times in ten seconds at most (no fight with a window manager).
void Tray::remap_check(void *data) {
  auto *self = static_cast<Tray *>(data);
  double t = now();
  for (auto &icon : self->icons_) {
    if (!icon.remap_pending) continue;
    icon.remap_pending = false;
    ErrorTrap trap;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(fl_display, icon.win, &attr) || attr.map_state != IsUnmapped ||
        !embed_mapped(icon.win))
      continue;
    if (t - icon.remap_since > 10) icon.remap_since = t, icon.remaps = 0;
    if (++icon.remaps > 3) continue;
    XMapRaised(fl_display, icon.win);
    TRAY_LOG("0x%lx was unmapped by someone else: shown again", icon.win);
  }
}

// Shows or hides an icon as its _XEMBED_INFO asks.
void Tray::apply_mapped(Icon &icon) {
  icon.want_mapped = embed_mapped(icon.win);
  ErrorTrap trap;
  if (icon.want_mapped) {
    XMapRaised(fl_display, icon.win);
  } else {
    ++icon.own_unmaps;
    XUnmapWindow(fl_display, icon.win);
  }
  icon.x = icon.y = -1;  // placed again
}

void Tray::composite_setup(Icon &icon, Visual *visual) {
#ifdef SDE_TRAY_COMPOSITE
  // The X server copies a window of another depth into its parent without
  // its alpha channel: transparent pixels became black. Redirected, the
  // icon's pixels stay off screen and the tray draws them with alpha.
  XCompositeRedirectWindow(fl_display, icon.win, CompositeRedirectManual);
  icon.damage = XDamageCreate(fl_display, icon.win, XDamageReportNonEmpty);
  icon.composited = true;
  (void)visual;
#else
  (void)icon;
  (void)visual;
#endif
}

void Tray::composite_release(Icon &icon) {
#ifdef SDE_TRAY_COMPOSITE
  if (!icon.composited) return;
  if (icon.damage) XDamageDestroy(fl_display, icon.damage);
  if (icon.pixmap) XFreePixmap(fl_display, icon.pixmap);
  XCompositeUnredirectWindow(fl_display, icon.win, CompositeRedirectManual);
  icon.damage = icon.pixmap = 0;
  icon.composited = false;
#else
  (void)icon;
#endif
}

// The ARGB icons over the host's background, with their alpha channel.
void Tray::paint_composited() {
#ifdef SDE_TRAY_COMPOSITE
  if (!host_ || !host_mapped_ || host_w_ <= 0 || host_h_ <= 0) return;
  bool any = false;
  for (auto &i : icons_) any = any || (i.composited && i.want_mapped && i.x >= 0);
  if (!any) return;
  ErrorTrap trap;
  XWindowAttributes ha;
  if (!XGetWindowAttributes(fl_display, host_, &ha)) return;
  cairo_surface_t *dst = cairo_xlib_surface_create(fl_display, host_, ha.visual, host_w_, host_h_);
  cairo_t *cr = cairo_create(dst);
  for (auto &i : icons_) {
    if (!i.composited || !i.want_mapped || i.x < 0) continue;
    XWindowAttributes ia;
    if (!XGetWindowAttributes(fl_display, i.win, &ia) || ia.map_state != IsViewable) continue;
    if (!i.pixmap) i.pixmap = XCompositeNameWindowPixmap(fl_display, i.win);
    if (!i.pixmap) continue;
    XClearArea(fl_display, host_, i.x, i.y, unsigned(ia.width), unsigned(ia.height), False);
    cairo_surface_t *src = cairo_xlib_surface_create(fl_display, i.pixmap, ia.visual, ia.width, ia.height);
    cairo_set_source_surface(cr, src, i.x, i.y);
    cairo_rectangle(cr, i.x, i.y, ia.width, ia.height);
    cairo_fill(cr);
    cairo_surface_destroy(src);
  }
  cairo_destroy(cr);
  cairo_surface_flush(dst);
  cairo_surface_destroy(dst);
  XFlush(fl_display);
#endif
}

bool Tray::handle(const XEvent &e) {
  if (!owner_) return false;
  if (e.type == ClientMessage && e.xclient.window == owner_) {
    if (e.xclient.message_type == sde::x11::atom("_NET_SYSTEM_TRAY_OPCODE") &&
        e.xclient.data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
      Window w = Window(e.xclient.data.l[2]);
      if (std::find(requests_.begin(), requests_.end(), w) == requests_.end() &&
          requests_.size() < kMaxRequests)
        requests_.push_back(w);
      if (!Fl::has_timeout(process_requests, this)) Fl::add_timeout(0.0, process_requests, this);
    }
    // Balloon messages (BEGIN_MESSAGE, CANCEL_MESSAGE) are not shown.
    return true;
  }
  if (e.type == SelectionClear && e.xselectionclear.window == owner_) {
    // Another tray took over: it gets the icons.
    while (!icons_.empty()) undock(icons_.size() - 1, true);
    requests_.clear();
    changed();
    return true;
  }
#ifdef SDE_TRAY_COMPOSITE
  if (composite_ && e.type == damage_event_ + XDamageNotify) {
    auto &d = reinterpret_cast<const XDamageNotifyEvent &>(e);
    for (auto &i : icons_)
      if (i.damage == d.damage) {
        XDamageSubtract(fl_display, i.damage, None, None);
        paint_composited();
        return true;
      }
    return false;
  }
#endif
  if (e.type == Expose && e.xexpose.window == host_) {
    if (e.xexpose.count == 0) paint_composited();
    return true;
  }
  // The window an event is about, and the one it was reported on.
  Window about = 0, on = e.xany.window;
  switch (e.type) {
    case DestroyNotify: about = e.xdestroywindow.window; break;
    case ReparentNotify: about = e.xreparent.window; break;
    case UnmapNotify: about = e.xunmap.window; break;
    case MapNotify: about = e.xmap.window; break;
    case ConfigureNotify: about = e.xconfigure.window; break;
    case PropertyNotify: about = e.xproperty.window; break;
    default: return false;
  }
  if (about == host_) return on == host_;  // the host's own events
  Icon *icon = find(about);
  if (!icon) {
    // Events about icons that left a moment ago (each comes twice, on the
    // icon and on the host) stay the tray's: FLTK would probe its input
    // method for every DestroyNotify of a foreign window (see tray.h).
    if (on == host_) return true;
    double t = now();
    gone_.erase(std::remove_if(gone_.begin(), gone_.end(),
                               [t](const std::pair<Window, double> &g) { return t - g.second > 5; }),
                gone_.end());
    return on == about && e.type != PropertyNotify &&
           std::any_of(gone_.begin(), gone_.end(),
                       [about](const std::pair<Window, double> &g) { return g.first == about; });
  }
  size_t index = size_t(icon - icons_.data());
  switch (e.type) {
    case DestroyNotify:
      undock(index, false);
      changed();
      break;
    case ReparentNotify:
      // Taken away by its program (or another tray).
      if (e.xreparent.parent != host_) {
        undock(index, false);
        changed();
      }
      break;
    case UnmapNotify:
      if (on != about) break;  // the same event, reported on the host
      if (icon->own_unmaps > 0) {
        --icon->own_unmaps;
      } else if (icon->want_mapped) {
        // Unmapped by something else (a window manager that framed it after
        // a tray crashed lets go of it): looked at again in a moment, when a
        // program that is quitting has destroyed it.
        icon->remap_pending = true;
        if (!Fl::has_timeout(remap_check, this)) Fl::add_timeout(0.3, remap_check, this);
      }
      break;
    case MapNotify:
#ifdef SDE_TRAY_COMPOSITE
      if (icon->composited && icon->pixmap) {
        XFreePixmap(fl_display, icon->pixmap);
        icon->pixmap = 0;
      }
#endif
      break;
    case PropertyNotify:
      if (e.xproperty.atom == sde::x11::atom("_XEMBED_INFO")) {
        bool was = icon->want_mapped;
        apply_mapped(*icon);
        if (was != icon->want_mapped) changed();
      }
      break;
    default:
      break;
  }
  return true;
}

void Tray::set_background(unsigned long pixel) {
  if (background_set_ && pixel == background_) return;
  background_ = pixel;
  background_set_ = true;
  if (!host_) return;  // start() creates the host in this colour
  ErrorTrap trap;
  XSetWindowBackground(fl_display, host_, pixel);
  // The host and the icons (whose background is the host's: ParentRelative)
  // are painted again in the new colour; their programs redraw on Expose.
  // Up to 0.2.0 Alpha 3.1 they kept the old one after a theme change.
  if (host_mapped_) {
    XClearArea(fl_display, host_, 0, 0, 0, 0, True);
    for (auto &i : icons_)
      if (!i.composited) XClearArea(fl_display, i.win, 0, 0, 0, 0, True);
  }
}

std::string Tray::qa_lines() const {
  std::string out;
  for (const auto &i : icons_) {
    std::string cls = i.cls.substr(0, i.cls.find(' '));
    out += "@tray-icon:" + cls + ":" + std::to_string(i.depth) + ":" +
           (i.want_mapped ? "shown" : "hidden") + "\t" + std::to_string(host_x_ + i.x) + "\t" +
           std::to_string(host_y_ + i.y) + "\t" + std::to_string(i.size) + "\t" +
           std::to_string(i.size) + "\t" + (host_mapped_ ? "1" : "0") + "\n";
  }
  out += "@tray-count:" + std::to_string(count()) + "\t0\t0\t0\t0\t1\n";
  return out;
}
