// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// debrun: installs Debian packages (.deb) downloaded from the websites of
// their makers (docs/DEBRUN.md, man debrun). Liria and the desktop open
// .deb files with it (debrun.desktop, NoDisplay: it is not in the Start
// menu or the Control Center).
//
//   debrun [FILE.deb | file:///path/FILE.deb]
//   debrun --version | --help
#include <FL/Fl.H>
#include <FL/x.H>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "config.h"
#include "crash.h"
#include "i18n.h"
#include "icons.h"
#include "theme.h"
#include "util.h"
#include "window.h"

using namespace sde;
using namespace sde::debrun;

namespace {

std::string usage() {
  return _("Debrun: install a Debian package downloaded from the website of its maker.\n\n"
           "Usage: debrun [FILE.deb]\n"
           "       debrun --version\n\n"
           "Debrun shows what the package contains and what it changes on the system, checks its\n"
           "dependencies with APT and installs it, with the packages it needs from the package\n"
           "sources, after an administrator's password.\n");
}

// "file:///home/u/x.deb" (what file managers may pass) -> "/home/u/x.deb".
std::string from_uri(const std::string &arg) {
  if (!starts_with(arg, "file://")) return arg;
  std::string rest = arg.substr(7), out;
  if (!rest.empty() && rest[0] != '/') rest = rest.substr(rest.find('/') == std::string::npos ? rest.size() : rest.find('/'));
  for (size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && isxdigit((unsigned char)rest[i + 1]) &&
        isxdigit((unsigned char)rest[i + 2])) {
      out += char(strtol(rest.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += rest[i];
    }
  }
  return out;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  sde::set_default_window_icon({"debrun", "package-x-generic"});
  install_crash_handler("debrun");
  std::string path;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") return print_version(about_info());
    if (a == "--help" || a == "-h") {
      printf("%s", usage().c_str());
      return 0;
    }
    if (starts_with(a, "-") || !path.empty()) {
      fprintf(stderr, "%s", usage().c_str());
      return 2;
    }
    path = from_uri(a);
  }
  // The real file: the helper wants an absolute path and opens no
  // symbolic link (a link to a .deb is followed here, as the user).
  if (!path.empty()) {
    if (char *real = realpath(path.c_str(), nullptr)) {
      path = real;
      free(real);
    } else if (path[0] != '/') {
      char cwd[4096];
      if (getcwd(cwd, sizeof cwd)) path = join_path(cwd, path);
    }
  }
  Config cfg;
  cfg.load();
  fl_open_display();
  Theme theme = Theme::from_config(cfg);
  theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  Fl::visible_focus(1);
  return run_window(path);
}
