// SPDX-License-Identifier: GPL-3.0-or-later
// Fl_X::before_map (SDE-FLTK) is declared with the FLTK internals.
#define FL_INTERNALS 1
#include <FL/Fl.H>
#include <FL/x.H>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "about.h"
#include "build_config.h"
#include "crash.h"
#include "i18n.h"
#include "panel.h"
#include "quick.h"
#include "status_flyout.h"
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
  sde::install_crash_handler("sde-panel");
  std::string command;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--menu")) {
      command = "menu";
    } else if (!strcmp(argv[i], "--brightness") && i + 1 < argc) {
      // Brightness keys: --brightness +5 or -5 (percent).
      command = std::string("brightness:") + argv[++i];
    } else if (!strcmp(argv[i], "--search")) {
      command = "search";
      if (i + 1 < argc && argv[i + 1][0] != '-') command += std::string(":") + argv[++i];
    } else if (!strcmp(argv[i], "--quick-settings") || !strcmp(argv[i], "--action-center")) {
      command = "quick";
    } else if (!strcmp(argv[i], "--section") && i + 1 < argc) {
      std::string section = argv[++i];
      if (section != "overview" && !QuickSettings::valid_section(section) &&
          !StatusFlyout::handles(section))
        return 2;
      command = "quick:" + section;
    } else if (!strcmp(argv[i], "--show-desktop")) {
      // A plain EWMH request: works without a running panel too.
      fl_open_display();
      sde::x11::set_showing_desktop(!sde::x11::showing_desktop());
      XSync(fl_display, False);  // processed before this process exits (see send_command)
      return 0;
    } else if (!strcmp(argv[i], "--safe")) {
      Panel::safe_mode = true;
    } else if (!strcmp(argv[i], "--calendar")) {
      command = "calendar";
    } else if (!strcmp(argv[i], "--alarms")) {
      command = "alarms";
    } else if (!strcmp(argv[i], "--new-alarm")) {
      command = "alarms:new";
    } else if (!strcmp(argv[i], "--version")) {
      return sde::print_version("sde-panel", SDE_VERSION);
    } else {
      printf("%s", _("Usage: sde-panel [--menu | --search [TEXT] | --action-center | --calendar | --alarms | --version]\n\n"
                     "  --menu            open or close the application menu of the running panel\n"
                     "  --search [TEXT]   open or close the panel search (with TEXT typed)\n"
                     "  --brightness +N|-N  change the screen brightness by N percent\n"
                     "  --action-center   open or close the Action Center\n"
                     "  --quick-settings  alias for --action-center\n"
                     "  --section NAME    open or close the flyout of a status icon: network,\n"
                     "                    bluetooth, sound, power, drives, keyboard, touchpad, tray\n"
                     "                    (overview opens the Action Center)\n"
                     "  --calendar        open or close the Action Center with its calendar\n"
                     "                    and agenda\n"
                     "  --alarms          open or close the clock's alarms\n"
                     "  --new-alarm       open the alarms with a new alarm\n"
                     "  --show-desktop    minimize every window, or bring them back\n"
                     "  --safe            start without tray icons and device status (after crashes)\n"));
      return strcmp(argv[i], "--help") ? 2 : 0;
    }
  }
  // The panel's text fields are the search and the menu's group names. An X input method such
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
  static Panel *the_panel = panel;
  Fl_X::before_map = [](Fl_Window *win, Window xid) {
    if (win == the_panel) Panel::prepare_xid(xid);
  };
  panel->show();
  panel->setup();
  Fl::add_handler(Panel::x_event_handler);
  return Fl::run();
}
