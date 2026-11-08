// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// liria: the file manager of the Selviana Desktop Environment.
//
//   liria [--new-window] [--select] [URI|PATH...]
//
// One Liria runs per session: a second `liria` hands its locations to the
// running one over D-Bus (service.h), which opens them in a new window.
// The options of the earlier Liria are accepted: --no-desktop and
// --force-desktop (SDE draws its own desktop), --browser, -t/--tabs and
// -w/--new-window (a window with the locations as tabs, as always),
// -n/--no-default-window, -s/--select, -g/--geometry, -q/--quit.
//
// `sudo liria` (Open as administrator, in a terminal) runs with root's own
// home and preferences on a private D-Bus session, never the user's.
#include <FL/Fl.H>
#include <FL/platform.H>
#include <limits.h>
#include <pwd.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "about.h"
#include "config.h"
#include "glibloop.h"
#include "i18n.h"
#include "icons.h"
#include "location.h"
#include "service.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

using namespace sde;
using namespace sde::liria;

namespace {

AppInfo about_info() {
  AppInfo info;
  info.name = _("Liria");
  info.program = "liria";
#ifdef SDE_APP_VERSION
  info.version = SDE_APP_VERSION;
#endif
  info.icon = "liria";
  info.description = _("Browse, organize and share your files, drives and network places.");
  return info;
}

void usage() {
  printf("%s", _("Usage: liria [OPTION...] [URI|PATH...]\n"
                 "\n"
                 "Browse files, folders, drives and network places. The locations open as\n"
                 "tabs of a new window of the running Liria.\n"
                 "\n"
                 "  -s, --select            show the given files selected in their folders\n"
                 "  -w, --new-window        open a new window (what Liria always does)\n"
                 "  -t, --tabs              open the locations as tabs of one window (the default)\n"
                 "  -n, --no-default-window start without a window when no location is given\n"
                 "  -g, --geometry=WxH      the size of the window\n"
                 "  -q, --quit              quit the running Liria\n"
                 "      --no-desktop        accepted for compatibility (SDE draws the desktop)\n"
                 "  -h, --help              show this help\n"
                 "      --version           show the version\n"
                 "\n"
                 "Locations: folders and files, file://, trash:///, network:///, smb://,\n"
                 "\\\\server\\share, sftp://, ftp://, dav(s)://, nfs:// and computer names.\n"
                 "Preferences: ~/.config/sde/liria.conf; Quick access: ~/.config/sde/liria-quick-access.\n"));
}

// sudo liria: root's home and preferences, a private D-Bus session.
void root_session(int argc, char **argv) {
  if (geteuid() != 0 || !getenv("SUDO_UID") || getenv("SDE_LIRIA_ROOT_BUS")) return;
  struct passwd *root = getpwuid(0);
  if (!root || !root->pw_dir) {
    fprintf(stderr, "liria: %s\n", _("the home folder of root is not known"));
    exit(1);
  }
  setenv("HOME", root->pw_dir, 1);
  for (const char *v : {"XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR",
                        "DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE"})
    unsetenv(v);
  setenv("SDE_LIRIA_ROOT_BUS", "1", 1);
  std::string session = find_program("dbus-run-session");
  char self[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (session.empty() || n <= 0) {
    fprintf(stderr, "liria: %s\n", _("could not start a private D-Bus session for the administrator"));
    exit(1);
  }
  self[n] = 0;
  std::vector<char *> args{const_cast<char *>(session.c_str()), const_cast<char *>("--"), self};
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  args.push_back(nullptr);
  execv(args[0], args.data());
  fprintf(stderr, "liria: %s\n", _("could not start a private D-Bus session for the administrator"));
  exit(1);
}

}  // namespace

int main(int argc, char **argv) {
  double started = now_ms();
  init_i18n();
  root_session(argc, argv);
  std::vector<std::string> args;
  bool select = false, no_default = false, quit = false, service = false, only_files = false;
  int gw = 0, gh = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (!only_files && a.size() > 1 && a[0] == '-') {
      if (a == "--") {
        only_files = true;
      } else if (a == "--version" || a == "-v") {
        return print_version(about_info());
      } else if (a == "--help" || a == "-h" || a == "-?") {
        usage();
        return 0;
      } else if (a == "-s" || a == "--select") {
        select = true;
      } else if (a == "-n" || a == "--no-default-window") {
        no_default = true;
      } else if (a == "-q" || a == "--quit") {
        quit = true;
      } else if (a == "--service" || a == "--gapplication-service") {
        service = true;
      } else if (a == "-g" || a == "--geometry" || starts_with(a, "--geometry=") || starts_with(a, "-g=")) {
        std::string g = a.find('=') != std::string::npos ? a.substr(a.find('=') + 1) : (i + 1 < argc ? argv[++i] : "");
        if (sscanf(g.c_str(), "%dx%d", &gw, &gh) != 2) gw = gh = 0;
      } else if (a == "--no-desktop" || a == "--force-desktop" || a == "--browser" || a == "-t" || a == "--tabs" ||
                 a == "-w" || a == "--new-window") {
        // Accepted for compatibility.
      } else {
        fprintf(stderr, "liria: %s: %s\n", a.c_str(), _("unknown option (see --help)"));
        return 2;
      }
      continue;
    }
    args.push_back(a);
  }
  char cwd_buf[PATH_MAX];
  std::string cwd = getcwd(cwd_buf, sizeof cwd_buf) ? cwd_buf : home_dir();
  std::vector<std::string> locations, chosen;
  for (const auto &a : args) {
    std::string loc = location_of_argument(a, cwd);
    if (loc.empty()) continue;
    // A file given as a location is shown selected in its folder.
    if (select || (is_local(loc) && file_exists(loc) && !dir_exists(loc))) chosen.push_back(loc);
    else locations.push_back(loc);
  }
  if (quit) return ask_running_to_quit() ? 0 : 1;
  // A Liria runs already: it opens the window.
  if (!service && !(no_default && locations.empty() && chosen.empty()) && hand_off(locations, chosen)) return 0;

  fl_open_display();
  Config cfg;
  cfg.load();
  Theme::from_config(cfg).apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  set_default_window_icon({"liria", "system-file-manager", "folder"});
  Fl::visual(FL_RGB);
  App &app = App::get();
  app.started_ms = started;
  app.prefs.load();
  if (gw > 0 && gh > 0) {
    app.prefs.width = int(gw / ui_scale());
    app.prefs.height = int(gh / ui_scale());
  }
  app.pins.load();
  app.servers.load();
  start_glib_loop();
  app.start_services();
  if (!start_dbus_service()) {
    // Another Liria started at the same moment.
    if (!service && hand_off(locations, chosen)) return 0;
  }
  if (service || (no_default && locations.empty() && chosen.empty())) {
    app.service_mode = true;
    app.service_until = now_ms() + 15000;
    Fl::add_timeout(15.5, [](void *) {});
  } else {
    app.open_window(locations, chosen);
  }
  while (!app.should_quit()) Fl::wait(1e20);
  stop_glib_loop();
  return 0;
}
