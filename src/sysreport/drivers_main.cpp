// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Driver Manager (sde-drivers): recommends Debian's non-free firmware
// and driver packages for this computer and installs them with pkexec.
// The concept follows Linux Mint's Driver Manager; the code is SDE's own.
#include <cstdio>
#include <string>

#include "about.h"
#include "build_config.h"
#include "drivers.h"
#include "i18n.h"
#include "pages.h"
#include "shell.h"

using namespace sde;

int main(int argc, char **argv) {
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") {
      printf("sde-drivers %s (%s) - %s\n", SDE_VERSION, SDE_VERSION_LABEL, kDeveloper);
      return 0;
    }
    if (a == "--list") {
      // Text report for terminals and bug reports.
      drivers::Scan s = drivers::scan_system();
      for (auto &g : s.gpus) printf("graphics: %s\n", g.c_str());
      printf("sources: %s (non-free-firmware %s)\n", s.sources_file.c_str(),
             s.non_free_firmware ? "enabled" : "disabled");
      for (auto &r : s.items)
        printf("%s\t%s\t%s\t%s\n", r.package.c_str(), r.installed ? "installed" : "recommended",
               r.component.c_str(), r.device.c_str());
      if (!s.kernel_log_readable) printf("note: the kernel log could not be read\n");
      return 0;
    }
    printf("Usage: sde-drivers [--list] [--version]\n\n"
           "  --list     print the recommended packages instead of opening the window\n");
    return a == "--help" ? 0 : 2;
  }
  shell::init();
  shell::Options o;
  o.title = _("Driver Manager");
  o.xclass = "sde-drivers";
  o.about.name = _("Driver Manager");
  o.about.program = "sde-drivers";
  o.about.version = SDE_VERSION;
  o.about.icon = "jockey";
  o.about.description = _("Finds hardware that needs non-free drivers or firmware and installs "
                          "Debian's packages for it.");
  o.about.credits = {_("Concept after Linux Mint's Driver Manager (no code used)"), toolkit_credit()};
  o.width = 820;
  o.height = 680;
  o.pages = {{"drivers", _("Driver Manager"), {"jockey"}, sysreport::build_drivers_page}};
  return shell::run(std::move(o));
}
