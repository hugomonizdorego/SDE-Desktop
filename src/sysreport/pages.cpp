// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "pages.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <unistd.h>

#include <ctime>
#include <memory>
#include <sstream>

#include "build_config.h"
#include "i18n.h"
#include "reports.h"
#include "shell.h"
#include "ui.h"
#include "util.h"

namespace sde {
namespace sysreport {

namespace {

std::unique_ptr<std::vector<reports::Grouped>> g_reports;
std::string g_summary;

}  // namespace

void rescan_reports() {
  g_reports.reset();
  g_summary.clear();
}

void build_reports_page(Page *page) {
  page->heading(_("System Reports"), _("Crash reports of SDE programs and information for bug reports"));
  if (!g_reports) g_reports.reset(new std::vector<reports::Grouped>(reports::group(reports::collect())));
  if (g_summary.empty()) g_summary = reports::system_summary();
  const Theme &t = shell::theme();

  page->begin_card(_("This computer"));
  page->note(g_summary);
  std::string summary = g_summary;
  shell::button_row(page, _("Copy the system information for a forum post or bug report"), _("Copy"),
                    [summary] { shell::copy_text(summary); });
  shell::button_row(page, _("Report a problem that is not a crash"), _("New Issue"), [summary] {
    std::string body = "### What happened\n\n<!-- Describe the problem and how to repeat it -->\n\n"
                       "### System\n\n```\n" + summary + "```\n";
    shell::open_url(reports::issue_url("", body));
  });
  page->end_card();

  if (g_reports->empty()) {
    Color sc = t.success;
    shell::step(page, {"emblem-ok", "object-select", "dialog-information"}, _("No crash reports"),
                _("No SDE program has crashed in this or the previous session. When one does, its "
                  "report appears here with a backtrace you can send to the developers."),
                {{_("Scan Again"), [] { rescan_reports(); shell::refresh(); }},
                 {_("Open Log Folder"), [] { shell::open_url(state_home() + "/sde"); }}},
                _("All good"), &sc);
    return;
  }
  page->begin_card();
  page->note(_("Newest first. \"Report on GitHub\" opens a new issue with the backtrace and the "
               "system information filled in; check it before you send it. The full report is also "
               "copied to the clipboard."));
  shell::button_row(page, _("Session logs"), _("Open Log Folder"),
                    [] { shell::open_url(state_home() + "/sde"); });
  page->end_card();
  for (auto it = g_reports->rbegin(); it != g_reports->rend(); ++it) {
    const reports::CrashReport &r = it->report;
    std::string text;
    std::istringstream in(r.backtrace);
    std::string line;
    int n = 0;
    while (std::getline(in, line) && n < 3) {
      text += (text.empty() ? "" : "\n") + line;
      ++n;
    }
    if (text.empty()) text = _("The session restarted it. No backtrace was written (the program may "
                               "not use the SDE crash handler).");
    std::string status = format(ngettext("%d time", "%d times", it->count), it->count);
    if (!r.time.empty()) status += "  ·  " + format(_("last at %s"), r.time.c_str());
    status += "  ·  " + basename_of(r.source);
    std::string full = reports::report_text(r, g_summary);
    std::string title = reports::issue_title(r, SDE_VERSION);
    std::string bt = r.backtrace;
    Color sc = t.warning;
    shell::step(page, {"dialog-warning", "computer-fail", "dialog-error"},
                format(_("%s stopped: %s"), r.program.c_str(), r.signal.c_str()), text,
                {{_("Copy Report"), [full] { shell::copy_text(full); }},
                 {_("Report on GitHub"),
                  [full, title] {
                    shell::copy_text(full);
                    shell::open_url(reports::issue_url(title, full));
                  },
                  true}},
                status, &sc);
  }
}

}  // namespace sysreport
}  // namespace sde
