// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper (panel-smoke), never installed. Mimics a screenshot tool (scrot -s, xfce4-screenshooter region mode):
// grabs the pointer and the keyboard on the root window, optionally waits
// for a click, then exits without ungrabbing (the server releases the grabs).
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <cstdio>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  Display *d = XOpenDisplay(NULL);
  if (!d) return 1;
  Window root = DefaultRootWindow(d);
  Cursor c = XCreateFontCursor(d, XC_crosshair);
  int pg = XGrabPointer(d, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                        GrabModeAsync, GrabModeAsync, None, c, CurrentTime);
  int kg = XGrabKeyboard(d, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
  printf("pointer grab %d keyboard grab %d\n", pg, kg);
  fflush(stdout);
  double wait = argc > 1 ? atof(argv[1]) : 1.0;
  if (argc > 2 && !strcmp(argv[2], "click")) {
    XEvent e;
    do XNextEvent(d, &e); while (e.type != ButtonRelease);
    printf("released\n");
  } else {
    usleep((useconds_t)(wait * 1e6));
  }
  XCloseDisplay(d);
  return 0;
}
