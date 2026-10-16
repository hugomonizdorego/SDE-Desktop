// SPDX-License-Identifier: GPL-3.0-or-later
#include <FL/Fl.H>
#include <FL/x.H>

#include <cstdio>
#include <cstring>
#include <string>

#include "build_config.h"
#include "panel.h"
#include "x11.h"

namespace {

int ignore_x_errors(Display *, XErrorEvent *e) {
  // Client windows can disappear at any time; BadWindow/BadMatch for them
  // are expected and harmless.
  if (e->error_code != BadWindow && e->error_code != BadMatch && e->error_code != BadDrawable)
    fprintf(stderr, "sde-panel: X error %d (request %d)\n", e->error_code, e->request_code);
  return 0;
}

// Only one panel per screen: owning this selection marks the running one.
bool claim_instance() {
  Atom sel = XInternAtom(fl_display, ("_SDE_PANEL_S" + std::to_string(fl_screen)).c_str(), False);
  if (XGetSelectionOwner(fl_display, sel) != None) return false;
  Window w = XCreateSimpleWindow(fl_display, RootWindow(fl_display, fl_screen), -1, -1, 1, 1, 0, 0, 0);
  XSetSelectionOwner(fl_display, sel, w, CurrentTime);
  return XGetSelectionOwner(fl_display, sel) == w;
}

}  // namespace

int main(int argc, char **argv) {
  bool open_menu = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--menu")) {
      open_menu = true;
    } else if (!strcmp(argv[i], "--version")) {
      printf("sde-panel %s (%s)\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    } else {
      printf("Usage: sde-panel [--menu | --version]\n\n"
             "  --menu     open or close the application menu of the running panel\n");
      return strcmp(argv[i], "--help") ? 2 : 0;
    }
  }
  fl_open_display();
  if (open_menu) {
    sde::x11::send_command("menu");
    return 0;
  }
  if (!claim_instance()) {
    fprintf(stderr, "sde-panel: a panel is already running on this screen\n");
    return 1;
  }
  XSetErrorHandler(ignore_x_errors);

  Panel *panel = new Panel();
  panel->show();
  panel->setup();
  Fl::add_handler(Panel::x_event_handler);
  return Fl::run();
}
