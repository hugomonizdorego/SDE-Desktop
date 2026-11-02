// SPDX-License-Identifier: GPL-3.0-or-later
// QA helper of tests/test_compositor_redraw.py (not installed): does the
// compositor running on $DISPLAY show what a window draws after it was
// mapped, and after it was resized while its program was still busy?
//
// The screen (the root window, which a compositor draws into) is read back.
// The compositor's wallpaper (_XROOTPMAP_ID) is a colour of its own and the
// X server draws the root window blue, so the probe first waits until the
// compositor draws the screen. Then, for a plain window and for a window
// manager frame with the program's window inside, each in 24 bit and with
// an alpha channel:
//
//  1. the window is mapped and drawn red only a moment later (a program
//     draws after its window was mapped), and red must show;
//  2. the window is made bigger and drawn nothing for 600 ms (the program
//     is busy), then green at its new size: green must show where the old
//     window was and in the part it grew by.
//
// With SDE 0.2.0 Alpha 3's compositor on Mesa's software OpenGL (Xvfb,
// virtual machines without 3D) the window kept showing what it had when it
// was mapped or resized: after the resize the old, smaller content with the
// rest of the frame see-through, until the compositor was reset.
//
// Exit status 0 when every case shows the new content, 1 otherwise, 77 when
// nothing composites the screen.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {
Display *dpy;
Window root;
const unsigned long kWallpaper = 0x203040, kRootBlue = 0x0000ff, kRed = 0xff0000, kGreen = 0x00ff00,
                    kTitle = 0x808080;

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

unsigned long screen_pixel(int x, int y) {
  XImage *im = XGetImage(dpy, root, x, y, 1, 1, AllPlanes, ZPixmap);
  if (!im) return 0xffffffff;
  unsigned long p = XGetPixel(im, 0, 0) & 0xffffff;
  XDestroyImage(im);
  return p;
}

// Waits until the screen at (x, y) is |color|; |last| gets what it was.
bool wait_for(int x, int y, unsigned long color, int ms, unsigned long *last) {
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  do {
    *last = screen_pixel(x, y);
    if (*last == color) return true;
    sleep_ms(50);
  } while (std::chrono::steady_clock::now() < until);
  return false;
}

Pixmap set_wallpaper() {
  int w = DisplayWidth(dpy, DefaultScreen(dpy)), h = DisplayHeight(dpy, DefaultScreen(dpy));
  Pixmap pm = XCreatePixmap(dpy, root, w, h, DefaultDepth(dpy, DefaultScreen(dpy)));
  GC gc = XCreateGC(dpy, pm, 0, nullptr);
  XSetForeground(dpy, gc, kWallpaper);
  XFillRectangle(dpy, pm, gc, 0, 0, w, h);
  XFreeGC(dpy, gc);
  for (const char *name : {"_XROOTPMAP_ID", "_XSETROOT_ID"})
    XChangeProperty(dpy, root, XInternAtom(dpy, name, False), XA_PIXMAP, 32, PropModeReplace,
                    reinterpret_cast<unsigned char *>(&pm), 1);
  XSetWindowBackground(dpy, root, kRootBlue);
  XClearWindow(dpy, root);
  XSync(dpy, False);
  return pm;
}

struct Case {
  bool frame, argb;
};

std::string name(const Case &c) {
  return std::string(c.frame ? "frame" : "window") + (c.argb ? " with alpha" : " 24 bit");
}

void fill(Window w, GC gc, unsigned long color, int width, int height) {
  XSetForeground(dpy, gc, color);
  XFillRectangle(dpy, w, gc, 0, 0, width, height);
  XSync(dpy, False);
}

// Runs one case; returns the failures ("" when it passed).
std::string run(const Case &c) {
  Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
  int depth = DefaultDepth(dpy, DefaultScreen(dpy));
  Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
  if (c.argb) {
    XVisualInfo vi;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vi)) return "no 32 bit visual";
    visual = vi.visual;
    depth = 32;
    cmap = XCreateColormap(dpy, root, visual, AllocNone);
  }
  XSetWindowAttributes a{};
  a.background_pixmap = None;  // like GTK and FLTK windows: nothing drawn for the program
  a.border_pixel = 0;
  a.colormap = cmap;
  a.event_mask = StructureNotifyMask;
  unsigned long mask = CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask;
  const int x = 60, y = 50, w0 = 220, h0 = 150, w1 = 460, h1 = 330, title = c.frame ? 24 : 0;
  const unsigned long opaque = c.argb ? 0xff000000UL : 0;
  Window top = XCreateWindow(dpy, root, x, y, w0, h0 + title, 0, depth, InputOutput, visual, mask, &a);
  Window client = top;
  if (c.frame) {
    a.event_mask = 0;
    client = XCreateWindow(dpy, top, 0, title, w0, h0, 0, depth, InputOutput, visual, mask, &a);
    XMapWindow(dpy, client);
  }
  XStoreName(dpy, top, "SDE compositor redraw probe");
  GC gc = XCreateGC(dpy, client, 0, nullptr), frame_gc = XCreateGC(dpy, top, 0, nullptr);
  XMapWindow(dpy, top);
  XEvent ev;
  do XNextEvent(dpy, &ev);
  while (ev.type != MapNotify || ev.xmap.window != top);

  std::string failures;
  unsigned long seen = 0;
  // 1. The program draws a moment after its window was mapped.
  sleep_ms(400);
  if (c.frame) fill(top, frame_gc, opaque | kTitle, w0, title);
  fill(client, gc, opaque | kRed, w0, h0);
  if (!wait_for(x + 40, y + title + 40, kRed, 5000, &seen)) {
    char buf[128];
    snprintf(buf, sizeof buf, "drawn after mapping: shows %06lx, not red; ", seen);
    failures += buf;
  }
  // 2. The window manager makes the window bigger; the program is busy.
  XResizeWindow(dpy, top, w1, h1 + title);
  if (c.frame) {
    XResizeWindow(dpy, client, w1, h1);
    fill(top, frame_gc, opaque | kTitle, w1, title);
  }
  XSync(dpy, False);
  sleep_ms(600);
  fill(client, gc, opaque | kGreen, w1, h1);
  struct {
    const char *where;
    int px, py;
  } points[] = {{"where the window was", x + 40, y + title + 40},
                {"where it grew", x + w1 - 40, y + title + h1 - 40}};
  for (auto &p : points) {
    if (!wait_for(p.px, p.py, kGreen, 5000, &seen)) {
      char buf[160];
      snprintf(buf, sizeof buf, "after the resize, %s: shows %06lx, not green; ", p.where, seen);
      failures += buf;
    }
  }
  XFreeGC(dpy, gc);
  XFreeGC(dpy, frame_gc);
  XDestroyWindow(dpy, top);
  if (c.argb) XFreeColormap(dpy, cmap);
  XSync(dpy, False);
  sleep_ms(300);
  return failures;
}
}  // namespace

int main() {
  dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    fprintf(stderr, "no display\n");
    return 2;
  }
  root = DefaultRootWindow(dpy);
  Pixmap wallpaper = set_wallpaper();
  unsigned long seen = 0;
  if (!wait_for(5, 5, kWallpaper, 60000, &seen)) {
    printf("nothing composites the screen (the corner shows %06lx)\n", seen);
    return 77;
  }
  int failed = 0;
  for (Case c : {Case{false, false}, Case{true, false}, Case{false, true}, Case{true, true}}) {
    std::string f = run(c);
    printf("%-22s %s\n", name(c).c_str(), f.empty() ? "OK" : ("FAIL: " + f).c_str());
    if (!f.empty()) ++failed;
  }
  for (const char *n : {"_XROOTPMAP_ID", "_XSETROOT_ID"}) XDeleteProperty(dpy, root, XInternAtom(dpy, n, False));
  XFreePixmap(dpy, wallpaper);
  XCloseDisplay(dpy);
  return failed ? 1 : 0;
}
