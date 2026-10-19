// SPDX-License-Identifier: GPL-3.0-or-later
#include <FL/Fl.H>
#include <FL/x.H>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "build_config.h"
#include "i18n.h"
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
  Window w =
      XCreateSimpleWindow(fl_display, RootWindow(fl_display, fl_screen), -1, -1, 1, 1, 0, 0, 0);
  XSetSelectionOwner(fl_display, sel, w, CurrentTime);
  return XGetSelectionOwner(fl_display, sel) == w;
}

}  // namespace

int main(int argc, char **argv) {
  sde::init_i18n();
  std::string command;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--menu")) {
      command = "menu";
    } else if (!strcmp(argv[i], "--quick-settings") || !strcmp(argv[i], "--action-center")) {
      command = "quick";
    } else if (!strcmp(argv[i], "--section") && i + 1 < argc) {
      std::string section = argv[++i];
      if (section != "overview" && section != "network" && section != "bluetooth" &&
          section != "sound" && section != "media" && section != "tray" && section != "power")
        return 2;
      command = "quick:" + section;
    } else if (!strcmp(argv[i], "--calendar")) {
      command = "calendar";
    } else if (!strcmp(argv[i], "--version")) {
      printf("sde-panel %s (%s)\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    } else {
      printf("%s", _("Usage: sde-panel [--menu | --action-center | --calendar | --version]\n\n"
                     "  --menu            open or close the application menu of the running panel\n"
                     "  --action-center   open or close the Action Center\n"
                     "  --quick-settings  alias for --action-center\n"
                     "  --section NAME    open Action Center: network, bluetooth, sound, media, "
                     "tray, power\n"
                     "  --calendar        open or close the calendar\n"));
      return strcmp(argv[i], "--help") ? 2 : 0;
    }
  }
  // The panel's only text field is the menu search. An X input method such
  // as IBus or Fcitx can swallow its keys in FLTK 1.3, so the panel opens
  // its display without XIM. The variable is restored at once, so programs
  // started from the panel keep the user's input method.
  {
    const char *im = getenv("XMODIFIERS");
    std::string saved = im ? im : "";
    setenv("XMODIFIERS", "@im=none", 1);
    fl_open_display();
    if (im) setenv("XMODIFIERS", saved.c_str(), 1);
    else unsetenv("XMODIFIERS");
  }
  if (!command.empty()) {
    sde::x11::send_command(command);
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
