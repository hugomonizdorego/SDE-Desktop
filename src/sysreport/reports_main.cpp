// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE System Reports (sde-system-reports): crash reports of SDE programs
// from the session logs, with "Copy report" and "Report on GitHub", and a
// system information summary. After Linux Mint's System Reports concept.
#include <cstdio>
#include <string>

#include "about.h"
#include "build_config.h"
#include "i18n.h"
#include "pages.h"
#include "reports.h"
#include "shell.h"

using namespace sde;

int main(int argc, char **argv) {
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") {
      printf("sde-system-reports %s (%s) - %s\n", SDE_VERSION, SDE_VERSION_LABEL, kDeveloper);
      return 0;
    }
    if (a == "--summary") {
      fputs(reports::system_summary().c_str(), stdout);
      return 0;
    }
    if (a == "--list") {
      for (auto &g : reports::group(reports::collect()))
        printf("%dx\t%s\t%s\t%s\n", g.count, g.report.program.c_str(), g.report.signal.c_str(),
               g.report.source.c_str());
      return 0;
    }
    printf("Usage: sde-system-reports [--list | --summary] [--version]\n\n"
           "  --list     print the crash reports found in the session logs\n"
           "  --summary  print the system information summary\n");
    return a == "--help" ? 0 : 2;
  }
  shell::init();
  shell::Options o;
  o.title = _("System Reports");
  o.xclass = "sde-system-reports";
  o.about.name = _("System Reports");
  o.about.program = "sde-system-reports";
  o.about.version = SDE_VERSION;
  o.about.icon = "dialog-warning";
  o.about.description = _("Lists crash reports of SDE programs and helps send them to the developers.");
  o.about.credits = {_("Concept after Linux Mint's System Reports (no code used)"), toolkit_credit()};
  o.width = 820;
  o.height = 680;
  o.pages = {{"reports", _("System Reports"), {"dialog-warning"}, sysreport::build_reports_page}};
  return shell::run(std::move(o));
}
