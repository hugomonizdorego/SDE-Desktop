// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper (panel-smoke), never installed: a plain application window
// for the alpha51 taskbar previews. Usage: sde-preview-window TITLE [COLOR]
// Paints colored stripes (so a thumbnail is visibly the window), prints
// "focus" when it gets the input focus and exits on WM_DELETE_WINDOW
// (the preview's close button).
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
  const char *title = argc > 1 ? argv[1] : "SDE Preview QA";
  unsigned long base = argc > 2 ? strtoul(argv[2], nullptr, 16) : 0x2563EB;
  Display *d = XOpenDisplay(nullptr);
  if (!d) return 1;
  int screen = DefaultScreen(d);
  Window w = XCreateSimpleWindow(d, RootWindow(d, screen), 120, 80, 640, 400, 0, 0, 0xFFFFFF);
  XStoreName(d, w, title);
  Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
  XChangeProperty(d, w, XInternAtom(d, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(title), int(strlen(title)));
  // The same class groups windows in one preview.
  const char *cls = argc > 3 ? argv[3] : "SDE-Preview-QA";
  XClassHint hint{const_cast<char *>("sde-preview-qa"), const_cast<char *>(cls)};
  XSetClassHint(d, w, &hint);
  Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(d, w, &del, 1);
  XSelectInput(d, w, ExposureMask | FocusChangeMask | StructureNotifyMask);
  XMapWindow(d, w);
  GC gc = XCreateGC(d, w, 0, nullptr);
  for (;;) {
    XEvent e;
    XNextEvent(d, &e);
    if (e.type == Expose && e.xexpose.count == 0) {
      XWindowAttributes a;
      XGetWindowAttributes(d, w, &a);
      // A title bar band, then stripes in the window's color.
      XSetForeground(d, gc, 0x202833);
      XFillRectangle(d, w, gc, 0, 0, unsigned(a.width), 48);
      for (int i = 0; i < 8; ++i) {
        unsigned long c = i % 2 ? base : 0xF4F6F8;
        XSetForeground(d, gc, c);
        XFillRectangle(d, w, gc, 0, 48 + i * (a.height - 48) / 8, unsigned(a.width),
                       unsigned((a.height - 48) / 8 + 1));
      }
    } else if (e.type == FocusIn) {
      printf("focus %s\n", title);
      fflush(stdout);
    } else if (e.type == ClientMessage && Atom(e.xclient.data.l[0]) == del) {
      printf("closed %s\n", title);
      fflush(stdout);
      break;
    }
  }
  XCloseDisplay(d);
  return 0;
}
