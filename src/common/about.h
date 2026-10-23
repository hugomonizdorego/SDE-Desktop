// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The common SDE "About" window. Every SDE program is part of the Selviana
// Desktop Environment and names the Selviana DE Project as its developer;
// the label "Developer" is translated, the name never is. The license,
// copyright lines, upstream credits and trademark notices come from the
// legal data (data/legal/components.json, see legal.h) unless AppInfo sets
// them.
//
//   sde::AppInfo info;
//   info.name = _("Gallery");
//   info.program = "sde-gallery";       // also the legal data component
//   info.version = SDE_VERSION;
//   info.icon = "sde-gallery";
//   info.description = _("Browse, organise and share your photos.");
//   info.credits = {sde::toolkit_credit()};
//   sde::show_about(info);
//
// The window has four sections: About (icon, name, version, description,
// "Part of Selviana Desktop Environment", developer, website), Credits,
// License (license name, the standard notice with its warranty disclaimer
// and the full text in a viewer) and Legal (copyright and trademarks).
// Keys: Ctrl+Tab / Ctrl+Shift+Tab or Alt+1..4 change the section, Left and
// Right move along the section buttons, Up/Down/Page Up/Page Down scroll,
// Enter or Escape close. SDE_ABOUT_QA_FILE=FILE logs the shown section and
// opened viewers for tests.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "paint.h"

namespace sde {

// Constant identity of every SDE program (not translated).
extern const char *const kDesktopName;    // "Selviana Desktop Environment"
extern const char *const kDeveloper;      // "Selviana DE Project"
extern const char *const kDeveloperId;    // AppStream developer id ("io.github.hugomonizdorego")
extern const char *const kCopyright;      // "Copyright © 2026 Selviana DE Project and contributors"
extern const char *const kProjectWebsite; // the SDE project page

// An upstream project a program comes from or adapts code of.
struct UpstreamCredit {
  std::string project;    // "mintupdate (Linux Mint Update Manager)"
  std::string url;
  std::string license;    // SPDX
  std::string copyright;  // "Copyright © 2007–2015 Clement Lefebvre"
  std::string relation;   // fork, port, adapted, build, reference
};

struct AppInfo {
  std::string name;         // "Gallery" (translated)
  std::string program;      // "sde-gallery"
  std::string version;
  std::string icon;         // freedesktop icon name of the program ("taca")
  std::vector<std::string> icon_fallbacks;  // older names of that icon ("org.debion.Niny")
  std::string description;  // one or two sentences (translated)
  std::string website = kProjectWebsite;
  // Component of data/legal/components.json; empty: |program|.
  std::string component;
  // SPDX license expression; empty: the component's license.
  std::string license;
  // Notice shown in the License section; empty: the license's standard one.
  std::string license_notice;
  // Copyright lines ("Copyright © ..."); empty: the component's lines.
  std::vector<std::string> copyright;
  // Extra trademark notices (keys of legal::trademark(), e.g. "debian");
  // the SDE notice and the component's notices are always shown.
  std::vector<std::string> trademarks;
  // Upstream projects; empty: the component's upstream projects.
  std::vector<UpstreamCredit> upstream;
  // Other credits, one line each (translated): libraries used, concepts.
  std::vector<std::string> credits;
  // Optional: draws the application icon in a size x size box when neither
  // |icon| nor |icon_fallbacks| is installed.
  std::function<void(Painter &, double x, double y, double size)> logo;
};

// "Developer: Selviana DE Project" (label translated).
std::string developer_line();
// "Part of Selviana Desktop Environment" (translated).
std::string part_of_line();
// Human readable license name, e.g. "GNU GPL version 3 or later".
std::string license_name(const std::string &spdx);
// Credit line for the SDE-FLTK toolkit and the FLTK authors.
std::string toolkit_credit();

// Modal About window centered over the program's first window.
void show_about(const AppInfo &info);

// Scrollable viewer with the full text of a license or notice file.
void show_license_viewer(const std::string &title, const std::string &text);
// The full license texts of a program (the License section's button).
void show_license(const AppInfo &info);
// Modal list of every SDE program and bundled component with its license
// and copyright (Control Center > About > Open-source licenses).
void show_open_source_licenses();

// For small dialogs without a menu: call from a window's handle(). F1, or a
// right click that no other widget used, opens a menu with "About <name>".
// Returns 1 when the event was used.
int handle_about_event(int event, const AppInfo &info);

// --version text in the GNU format, e.g.
//   taca (Selviana Desktop Environment) 2.0.0~alpha1
//   Copyright © 2026 Selviana DE Project and contributors
//   License GPL-3.0-or-later: GNU GPL version 3 or later <https://...>.
//   This is free software: you are free to change and redistribute it.
//   There is NO WARRANTY, to the extent permitted by law.
// The SDE release ("SDE 0.1 Alpha 52 (0.1.0~alpha52)") follows the first
// line when the program has its own version.
std::string version_line(const AppInfo &info);
std::string version_line(const std::string &program, const std::string &version);
// Prints version_line() to stdout; returns 0 for "return print_version(...)".
int print_version(const AppInfo &info);
int print_version(const std::string &program, const std::string &version);

}  // namespace sde
