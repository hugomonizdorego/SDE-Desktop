// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// merkadu: the SDE software center (docs/MERKADU.md, man merkadu).
//
//   merkadu [--page home|installed|updates|packages|history|progress|sources]
//           [--search TEXT] [--category ID] [--details APPID] [--package NAME]
//   merkadu --self-test [--expect REF,...] [--expect-flatpak N] [--list]
//   merkadu appstream://APPID
//   merkadu --uninstall FILE.desktop   (exit 0 removed, 1 failed, 2 cancelled)
//   merkadu --version | --help
#include <FL/Fl.H>
#include <FL/x.H>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "about.h"
#include "config.h"
#include "crash.h"
#include "i18n.h"
#include "icons.h"
#include "store.h"
#include "theme.h"
#include "util.h"
#include "window.h"

using namespace sde;
using namespace sde::merkadu;

namespace {

std::string usage() {
  return _("Merkadu: find, install, update and remove software from the Debian archive, Flatpak and Snap.\n\n"
           "Usage: merkadu [--page NAME] [--search TEXT] [--category ID] [--details APPID] [--package NAME]\n"
           "       merkadu appstream://APPID\n"
           "       merkadu --uninstall FILE.desktop\n"
           "       merkadu --version\n\n"
           "  --page NAME       open a page: home, installed, updates, packages (All Software),\n"
           "                    history, progress or sources\n"
           "  --search TEXT     search for apps\n"
           "  --category ID     open a department (games, graphics/photography, ...)\n"
           "  --details APPID   open the page of an app (its AppStream id, e.g. org.gimp.GIMP)\n"
           "  --package NAME    open the page of a package in All Software\n"
           "  --self-test       load the catalogs and package lists without a window and report\n"
           "                    (--expect REF,... --expect-flatpak N --list)\n"
           "  --uninstall FILE  uninstall the app of a launcher (the absolute path of its .desktop\n"
           "                    file), asking first; exits with 0 when it was removed, 1 when it\n"
           "                    failed or cannot be removed, 2 when the user cancelled\n"
           "  --version         print the version and exit\n");
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  if (argc > 1 && std::strcmp(argv[1], "--self-test") == 0)
    return run_self_test(std::vector<std::string>(argv + 2, argv + argc));
  sde::set_default_window_icon({"merkadu", "system-software-install"});  // title bar and taskbar
  install_crash_handler("merkadu");
  View view;
  std::string uninstall;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](const char *name, std::string &out) {
      std::string eq = std::string(name) + "=";
      if (a == name && i + 1 < argc) {
        out = argv[++i];
        return true;
      }
      if (starts_with(a, eq)) {
        out = a.substr(eq.size());
        return true;
      }
      return false;
    };
    std::string v;
    if (value("--uninstall", v)) {
      uninstall = v;
    } else if (value("--page", v)) {
      if (!view_from_id(v, &view.kind) || view.kind == View::Details || view.kind == View::Category ||
          view.kind == View::Search || view.kind == View::Package) {
        fprintf(stderr, "merkadu: unknown page \"%s\"\n", v.c_str());
        return 2;
      }
    } else if (value("--search", v)) {
      view = View{View::Search, trim(v)};
    } else if (value("--category", v)) {
      view = View{View::Category, v};
    } else if (value("--details", v)) {
      view = View{View::Details, normalize_id(v)};
    } else if (value("--package", v)) {
      view = View{View::Package, trim(v)};
    } else if (starts_with(a, "appstream:")) {
      // appstream://org.gimp.GIMP, the links of app websites (MimeType of
      // merkadu.desktop).
      std::string id = a.substr(10);
      while (!id.empty() && id[0] == '/') id.erase(0, 1);
      while (!id.empty() && id.back() == '/') id.pop_back();
      view = View{View::Details, normalize_id(id)};
    } else if (a == "--version") {
      return print_version(about_info());
    } else if (a == "--help" || a == "-h") {
      printf("%s", usage().c_str());
      return 0;
    } else {
      fprintf(stderr, "%s", usage().c_str());
      return 2;
    }
  }
  if (!uninstall.empty() && uninstall[0] != '/') {
    fprintf(stderr, "merkadu: --uninstall needs the absolute path of a .desktop file\n");
    return 1;
  }
  if (view.kind == View::Search && view.arg.empty()) view.kind = View::Explore;

  Config cfg;
  cfg.load();
  fl_open_display();
  Theme theme = Theme::from_config(cfg);
  theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  Fl::visible_focus(1);
  // Worker threads (catalog, Flatpak, snapd) hand their results to the
  // event loop with Fl::awake().
  Fl::lock();
  if (!uninstall.empty()) return run_uninstall(uninstall);
  Store::get().start();
  return run_window(view);
}
