// SPDX-License-Identifier: GPL-3.0-or-later
// sde-tray-client: XEmbed system tray icons that misbehave, for the panel's
// tray tests (tests/test_tray.py, docs/PANEL.md). Never installed.
//
//   sde-tray-client [--name NAME] [--color RRGGBB] [--argb] [--frozen]
//                   [--die-after MS] [--storm N] [--redock N] [--reparent N]
//                   [--bogus] [--ready FILE]
//       An icon with a dot of COLOR, docked like GTK's: its background is
//       ParentRelative (the tray's colour shows around the dot). It docks
//       again when a new tray appears (MANAGER), as real programs do.
//       --argb       a 32-bit ARGB window: transparent around a translucent dot
//       --frozen     stops reading X events once embedded (a hung program)
//       --die-after  exits after MS milliseconds (the server destroys the icon)
//       --storm N    N icons created, docked and destroyed at once, then one stays
//       --redock N   N more dock requests for the same icon, flipping XEMBED_MAPPED
//       --reparent N N times: takes the icon back to the root window and docks again
//       --bogus      dock requests for None, the root window, a window that does not
//                    exist, the tray manager's window, the panel and a mapped
//                    application window of this program ("sde-tray-client victim")
//       --ready FILE written with the icon's window id once it is embedded
//   sde-tray-client --pixel X Y
//       Prints the colour of the screen at X,Y as RRGGBB.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

Display *dpy;
Window root;
Atom a_manager, a_selection, a_opcode, a_info, a_xembed;
std::string name = "sde-tray-client", ready_file;
unsigned long color = 0x2f9e44;
bool argb = false, frozen = false;
Visual *visual = nullptr;
int depth = 24;
Colormap colormap = None;
int size = 22;

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int ignore_errors(Display *, XErrorEvent *) { return 0; }

Window tray_owner() { return XGetSelectionOwner(dpy, a_selection); }

void set_info(Window w, bool mapped) {
  unsigned long values[] = {0, mapped ? 1UL : 0UL};
  XChangeProperty(dpy, w, a_info, a_info, 32, PropModeReplace, reinterpret_cast<unsigned char *>(values),
                  2);
}

Window make_icon() {
  XSetWindowAttributes sa;
  memset(&sa, 0, sizeof sa);
  unsigned long mask = CWEventMask | CWBorderPixel;
  sa.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask;
  sa.border_pixel = 0;
  if (argb) {
    sa.colormap = colormap;
    sa.background_pixel = 0;  // transparent
    mask |= CWColormap | CWBackPixel;
  } else {
    sa.background_pixmap = ParentRelative;  // like GTK without an ARGB tray visual
    mask |= CWBackPixmap;
  }
  Window w = XCreateWindow(dpy, root, 0, 0, unsigned(size), unsigned(size), 0, depth, InputOutput,
                           visual, mask, &sa);
  XStoreName(dpy, w, name.c_str());
  std::string cls = name;
  XClassHint hint{const_cast<char *>(name.c_str()), const_cast<char *>(cls.c_str())};
  XSetClassHint(dpy, w, &hint);
  set_info(w, true);
  return w;
}

void dock(Window icon, Window owner = 0) {
  if (!owner) owner = tray_owner();
  if (!owner) return;
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.window = owner;
  e.xclient.message_type = a_opcode;
  e.xclient.format = 32;
  e.xclient.data.l[0] = CurrentTime;
  e.xclient.data.l[1] = 0;  // SYSTEM_TRAY_REQUEST_DOCK
  e.xclient.data.l[2] = long(icon);
  XSendEvent(dpy, owner, False, NoEventMask, &e);
  XFlush(dpy);
}

// A dot of |color|: on the tray's own background (ParentRelative), or on
// transparent pixels at 80 % opacity for the ARGB icon.
void draw(Window w) {
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, w, &wa)) return;
  int W = wa.width, H = wa.height;
  if (argb) {
    std::vector<uint32_t> px(size_t(W) * size_t(H), 0);
    double cx = W / 2.0, cy = H / 2.0, r = std::min(W, H) * 0.4;
    unsigned a = 0xCC;
    uint32_t c = (a << 24) | ((((color >> 16) & 255) * a / 255) << 16) |
                 ((((color >> 8) & 255) * a / 255) << 8) | ((color & 255) * a / 255);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        if ((x + .5 - cx) * (x + .5 - cx) + (y + .5 - cy) * (y + .5 - cy) <= r * r) px[size_t(y) * W + x] = c;
    XImage *img = XCreateImage(dpy, visual, 32, ZPixmap, 0, reinterpret_cast<char *>(px.data()),
                               unsigned(W), unsigned(H), 32, 0);
    GC gc = XCreateGC(dpy, w, 0, nullptr);
    XPutImage(dpy, w, gc, img, 0, 0, 0, 0, unsigned(W), unsigned(H));
    img->data = nullptr;
    XDestroyImage(img);
    XFreeGC(dpy, gc);
  } else {
    XClearWindow(dpy, w);
    GC gc = XCreateGC(dpy, w, 0, nullptr);
    XSetForeground(dpy, gc, color);
    int d = std::min(W, H) * 4 / 5;
    XFillArc(dpy, w, gc, (W - d) / 2, (H - d) / 2, unsigned(d), unsigned(d), 0, 360 * 64);
    XFreeGC(dpy, gc);
  }
  XFlush(dpy);
}

void wait_events(double seconds, Window icon) {
  double end = now() + seconds;
  while (now() < end) {
    while (XPending(dpy)) {
      XEvent e;
      XNextEvent(dpy, &e);
      if (e.type == Expose && e.xexpose.window == icon && e.xexpose.count == 0) draw(icon);
    }
    pollfd p{ConnectionNumber(dpy), POLLIN, 0};
    poll(&p, 1, 10);
  }
}

// The window named |title| below the root window (the panel), or 0.
Window find_named(Window parent, const char *title) {
  Window r, p, *kids = nullptr;
  unsigned n = 0;
  Window found = 0;
  if (!XQueryTree(dpy, parent, &r, &p, &kids, &n)) return 0;
  for (unsigned i = 0; i < n && !found; ++i) {
    char *wn = nullptr;
    if (XFetchName(dpy, kids[i], &wn) && wn) {
      if (!strcmp(wn, title)) found = kids[i];
      XFree(wn);
    }
    if (!found) found = find_named(kids[i], title);
  }
  if (kids) XFree(kids);
  return found;
}

int pixel_mode(int x, int y) {
  XImage *img = XGetImage(dpy, root, x, y, 1, 1, AllPlanes, ZPixmap);
  if (!img) return 1;
  unsigned long p = XGetPixel(img, 0, 0);
  XDestroyImage(img);
  printf("%06lx\n", p & 0xffffff);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  int die_after = -1, storm = 0, redock = 0, reparent = 0;
  bool bogus = false;
  dpy = XOpenDisplay(nullptr);
  if (!dpy) return 1;
  root = DefaultRootWindow(dpy);
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--pixel" && i + 2 < argc) {
      int x = atoi(argv[i + 1]), y = atoi(argv[i + 2]);
      return pixel_mode(x, y);
    }
    if (a == "--name") name = next();
    else if (a == "--color") color = strtoul(next(), nullptr, 16);
    else if (a == "--argb") argb = true;
    else if (a == "--frozen") frozen = true;
    else if (a == "--die-after") die_after = atoi(next());
    else if (a == "--storm") storm = atoi(next());
    else if (a == "--redock") redock = atoi(next());
    else if (a == "--reparent") reparent = atoi(next());
    else if (a == "--bogus") bogus = true;
    else if (a == "--ready") ready_file = next();
    else {
      fprintf(stderr, "sde-tray-client: unknown option %s\n", a.c_str());
      return 2;
    }
  }
  XSetErrorHandler(ignore_errors);
  int screen = DefaultScreen(dpy);
  a_manager = XInternAtom(dpy, "MANAGER", False);
  a_selection = XInternAtom(dpy, ("_NET_SYSTEM_TRAY_S" + std::to_string(screen)).c_str(), False);
  a_opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
  a_info = XInternAtom(dpy, "_XEMBED_INFO", False);
  a_xembed = XInternAtom(dpy, "_XEMBED", False);
  visual = DefaultVisual(dpy, screen);
  depth = DefaultDepth(dpy, screen);
  if (argb) {
    XVisualInfo vi;
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vi)) {
      fprintf(stderr, "sde-tray-client: no 32-bit visual\n");
      return 1;
    }
    visual = vi.visual;
    depth = 32;
    colormap = XCreateColormap(dpy, root, visual, AllocNone);
  }
  XSelectInput(dpy, root, StructureNotifyMask);  // MANAGER announcements
  for (int t = 0; !tray_owner() && t < 500; ++t) usleep(10000);
  if (!tray_owner()) {
    fprintf(stderr, "sde-tray-client: no system tray\n");
    return 1;
  }

  for (int i = 0; i < storm; ++i) {
    Window w = make_icon();
    dock(w);
    if (i % 3 == 0) usleep(2000);
    XDestroyWindow(dpy, w);
    XFlush(dpy);
  }
  Window icon = make_icon();
  if (bogus) {
    Window victim = XCreateSimpleWindow(dpy, root, 40, 40, 160, 90, 0, 0, 0x8844aa);
    XStoreName(dpy, victim, "sde-tray-client victim");
    XMapWindow(dpy, victim);
    XSync(dpy, False);
    wait_events(0.4, icon);
    Window panel = find_named(root, "SDE Panel");
    for (Window w : {Window(None), root, Window(0x7ff0001), victim, tray_owner(), panel}) dock(w);
  }
  dock(icon);
  printf("docking 0x%lx\n", icon);
  fflush(stdout);

  bool embedded = false;
  double start = now();
  int redocks_left = redock, reparents_left = reparent;
  double next_action = 0;
  for (;;) {
    while (XPending(dpy)) {
      XEvent e;
      XNextEvent(dpy, &e);
      if (e.type == Expose && e.xexpose.window == icon && e.xexpose.count == 0) draw(icon);
      if (e.type == ConfigureNotify && e.xconfigure.window == icon) draw(icon);
      if (e.type == ReparentNotify && e.xreparent.window == icon) {
        embedded = e.xreparent.parent != root;
        printf("%s 0x%lx\n", embedded ? "embedded in" : "back on the root window", e.xreparent.parent);
        fflush(stdout);
        if (embedded && !ready_file.empty()) {
          FILE *f = fopen(ready_file.c_str(), "w");
          if (f) {
            fprintf(f, "0x%lx\n", icon);
            fclose(f);
          }
        }
        if (embedded && frozen && !redocks_left && !reparents_left) {
          printf("frozen\n");
          fflush(stdout);
          for (;;) pause();  // never reads another event
        }
      }
      // A new tray: dock again, like GTK and Qt.
      if (e.type == ClientMessage && e.xclient.window == root && e.xclient.message_type == a_manager &&
          Atom(e.xclient.data.l[1]) == a_selection)
        dock(icon, Window(e.xclient.data.l[2]));
    }
    double t = now();
    if ((redock || reparent) && !redocks_left && !reparents_left) {
      printf("done\n");  // the last request is sent
      fflush(stdout);
      redock = reparent = 0;
    }
    if (embedded && t >= next_action && redocks_left > 0) {
      set_info(icon, redocks_left % 2 == 1);  // shown again by the last request
      dock(icon);
      --redocks_left;
      next_action = t + 0.002;
    } else if (embedded && t >= next_action && reparents_left > 0) {
      XUnmapWindow(dpy, icon);
      XReparentWindow(dpy, icon, root, 0, 0);
      dock(icon);
      --reparents_left;
      embedded = false;
      next_action = t + 0.01;
    }
    if (die_after >= 0 && (t - start) * 1000 >= die_after) return 0;
    pollfd p{ConnectionNumber(dpy), POLLIN, 0};
    poll(&p, 1, 5);
  }
}
