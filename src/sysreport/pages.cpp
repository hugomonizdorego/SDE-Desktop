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
#include "drivers.h"
#include "i18n.h"
#include "reports.h"
#include "shell.h"
#include "ui.h"
#include "util.h"

namespace sde {
namespace sysreport {

namespace {

std::unique_ptr<drivers::Scan> g_scan;
std::unique_ptr<std::vector<reports::Grouped>> g_reports;
std::string g_summary;

std::vector<std::string> icons_for(const drivers::Recommendation &r) {
  const std::string &p = r.package;
  std::string d = lower(r.device);
  if (p.find("nvidia") != std::string::npos || p.find("graphics") != std::string::npos)
    return {"video-display", "preferences-desktop-display", "computer"};
  if (p.find("microcode") != std::string::npos) return {"cpu", "computer", "application-x-firmware"};
  if (d.find("bluetooth") != std::string::npos) return {"bluetooth", "preferences-system-bluetooth", "network-wireless"};
  if (d.find("wi-fi") != std::string::npos) return {"network-wireless", "network-wireless-symbolic"};
  if (d.find("ethernet") != std::string::npos) return {"network-wired", "network-wireless"};
  if (d.find("audio") != std::string::npos) return {"audio-card", "audio-speakers", "multimedia-volume-control"};
  return {"application-x-firmware", "drive-harddisk", "computer"};
}

std::string component_label(const std::string &c) {
  if (c == "non-free-firmware") return _("non-free firmware");
  if (c == "non-free") return _("non-free");
  if (c == "contrib") return _("contrib");
  return _("free software (main)");
}

// Rewrites the Debian sources file with |components| added (one
// authorization: copy the new file in place, then apt-get update). A copy
// of the old file is kept in ~/.local/state/sde/apt-backup/.
void enable_components(const std::vector<std::string> &components) {
  bool deb822 = false;
  std::string path = drivers::main_sources_file(&deb822);
  std::string text;
  if (!read_file(path, text)) {
    fl_alert(_("Cannot read %s."), path.c_str());
    return;
  }
  std::string updated = drivers::enable_components(text, deb822, components);
  if (updated == text) {
    fl_message(_("%s has no Debian entry to change. Add the components in a terminal, for example "
                 "with \"sudo apt edit-sources\"."),
               path.c_str());
    return;
  }
  std::string list;
  for (auto &c : components) list += (list.empty() ? "" : ", ") + c;
  if (fl_choice(_("Add %s to the Debian entries of %s and refresh the package lists?\n\n"
                  "Only these words are added; other repositories and comments stay as they are. A "
                  "copy of the current file is saved in ~/.local/state/sde/apt-backup."),
                _("Cancel"), _("Enable"), nullptr, list.c_str(), path.c_str()) != 1)
    return;
  std::string backup_dir = state_home() + "/sde/apt-backup";
  make_dirs(backup_dir);
  char stamp[32];
  time_t now = time(nullptr);
  strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
  write_file_atomic(join_path(backup_dir, basename_of(path) + "." + stamp), text);
  std::string tmp = join_path(runtime_dir(), "sde-drivers-" + std::to_string(getpid()) + "-" +
                                                 basename_of(path));
  if (!write_file_atomic(tmp, updated)) {
    fl_alert(_("Cannot write %s."), tmp.c_str());
    return;
  }
  // The paths are passed as arguments, never pasted into the script.
  shell::run_admin({"/bin/sh", "-c", "install -m 0644 -- \"$1\" \"$2\" && apt-get update", "sh", tmp, path},
                   _("Enabling the software sources and refreshing the package lists..."),
                   [tmp](const std::string &out, int status) {
                     unlink(tmp.c_str());
                     rescan_drivers();
                     if (status != 0 && status != 126 && status != 127) {
                       std::string tail = out.size() > 1500 ? out.substr(out.size() - 1500) : out;
                       fl_alert(_("Refreshing the package lists failed (status %d).\n\n%s"), status,
                                tail.c_str());
                     }
                   });
}

}  // namespace

void rescan_drivers() { g_scan.reset(); }
void rescan_reports() {
  g_reports.reset();
  g_summary.clear();
}

void build_drivers_page(Page *page) {
  page->heading(_("Driver Manager"), _("Drivers and firmware for the hardware of this computer"));
  if (!g_scan) g_scan.reset(new drivers::Scan(drivers::scan_system()));
  const drivers::Scan &s = *g_scan;
  const Theme &t = shell::theme();

  bool need_firmware = false, need_nonfree = false;
  for (auto &r : s.items) {
    if (r.installed) continue;
    if (r.component == "non-free-firmware") need_firmware = true;
    if (r.component == "non-free" || r.component == "contrib") need_nonfree = true;
  }

  // Software sources.
  {
    std::vector<shell::Action> actions;
    std::string text = format(_("Debian keeps firmware that is not free software in the "
                                "\"non-free-firmware\" component and drivers such as NVIDIA's in "
                                "\"non-free\" and \"contrib\". They must be enabled in %s before "
                                "these packages can be installed."),
                              s.sources_file.c_str());
    std::string status;
    if (s.non_free_firmware && (s.contrib_non_free || !need_nonfree)) {
      status = _("Enabled");
    } else {
      std::vector<std::string> comps;
      if (!s.non_free_firmware) comps.push_back("non-free-firmware");
      if (!s.contrib_non_free && need_nonfree) {
        comps.push_back("contrib");
        comps.push_back("non-free");
      }
      std::string list;
      for (auto &c : comps) list += (list.empty() ? "" : ", ") + c;
      status = format(_("Not enabled: %s"), list.c_str());
      actions.push_back({_("Enable"), [comps] { enable_components(comps); }, true});
    }
    bool ok = s.non_free_firmware && (s.contrib_non_free || !need_nonfree);
    Color sc = ok ? t.success : t.warning;
    shell::step(page, {"software-properties", "preferences-system", "system-software-install"},
                _("Software sources"), text, actions, status, &sc);
  }

  if (!s.gpus.empty()) {
    std::string list;
    for (auto &g : s.gpus) list += (list.empty() ? "" : "\n") + g;
    page->begin_card(_("Graphics"));
    page->note(list);
    page->end_card();
  }

  std::vector<std::string> missing;
  for (auto &r : s.items)
    if (!r.installed) missing.push_back(r.package);

  if (s.items.empty()) {
    Color sc = t.success;
    shell::step(page, {"emblem-ok", "object-select", "dialog-information"},
                _("No additional drivers are needed"),
                _("The free drivers of Debian support every device that SDE found. Devices that "
                  "need firmware later (for example a USB Wi-Fi stick) show up here after they are "
                  "plugged in: scan again."),
                {{_("Scan Again"), [] { rescan_drivers(); shell::refresh(); }}}, _("Everything is set up"),
                &sc);
  } else {
    page->begin_card(missing.empty() ? _("Everything recommended is installed")
                                     : format(ngettext("%d recommended package", "%d recommended packages",
                                                       missing.size()),
                                              int(missing.size())));
    if (!missing.empty()) {
      std::string list;
      for (auto &m : missing) list += (list.empty() ? "" : " ") + m;
      shell::button_row(page, list, _("Install All"),
                        [missing] { shell::install_packages(missing, ""); },
                        _("Restart the computer afterwards so that the new firmware and drivers are "
                          "loaded."),
                        true);
    }
    shell::button_row(page, _("Look for devices again"), _("Scan Again"), [] {
      rescan_drivers();
      shell::refresh();
    });
    page->end_card();
    for (auto &r : s.items) {
      std::vector<shell::Action> actions;
      if (!r.installed) {
        std::string pkg = r.package;
        actions.push_back({_("Install"), [pkg] { shell::install_packages({pkg}, ""); }, false});
      }
      Color sc = r.installed ? t.success : t.accent_text;
      std::string status = r.installed ? _("Installed")
                                       : format(_("Recommended (%s)"), component_label(r.component).c_str());
      shell::step(page, icons_for(r), r.package + "  -  " + r.device, r.reason, actions, status, &sc);
    }
  }

  if (!s.kernel_log_readable)
    page->note(_("The kernel log could not be read, so missing firmware may not be listed. Members "
                 "of the \"adm\" group can read it (sudo adduser $USER adm, then log in again)."));
  if (!s.isenkram)
    page->note(_("Tip: install isenkram-cli for more suggestions based on the devices' "
                 "modaliases."));
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
