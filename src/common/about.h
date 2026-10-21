// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The common SDE "About" window. Every SDE program names the DebION Dev-Team
// as its developer; the label "Developer" is translated, the name never is.
//
//   sde::AppInfo info;
//   info.name = _("Gallery");
//   info.program = "sde-gallery";
//   info.version = SDE_VERSION;
//   info.icon = "org.debion.Gallery";
//   info.description = _("Browse, organise and share your photos.");
//   info.credits = {sde::toolkit_credit()};
//   sde::show_about(info);
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "paint.h"

namespace sde {

// Constant identity of every SDE program (not translated).
extern const char *const kDeveloper;      // "DebION Dev-Team"
extern const char *const kDeveloperId;    // "org.debion" (AppStream developer id)
extern const char *const kCopyright;      // "© 2026 DebION Dev-Team"
extern const char *const kProjectWebsite; // the SDE project page

struct AppInfo {
  std::string name;         // "Gallery" (translated)
  std::string program;      // "sde-gallery"
  std::string version;
  std::string icon;         // freedesktop icon name, used when |logo| is empty
  std::string description;  // one or two sentences (translated)
  std::string website = kProjectWebsite;
  // SPDX license expression; shown with its full name when known.
  std::string license = "GPL-3.0-or-later";
  // Upstream projects and other credits, one line each (translated).
  std::vector<std::string> credits;
  // Optional: draws the application icon in a size x size box.
  std::function<void(Painter &, double x, double y, double size)> logo;
};

// "Developer: DebION Dev-Team" (label translated).
std::string developer_line();
// Human readable license name, e.g. "GNU GPL version 3 or later".
std::string license_name(const std::string &spdx);
// Credit line for the SDE-FLTK toolkit and the FLTK authors.
std::string toolkit_credit();

// Modal About window centered over the program's first window.
void show_about(const AppInfo &info);

// For small dialogs without a menu: call from a window's handle(). F1, or a
// right click that no other widget used, opens a menu with "About <name>".
// Returns 1 when the event was used.
int handle_about_event(int event, const AppInfo &info);

// "<program> <version> (DebION Dev-Team)" for --version output.
std::string version_line(const AppInfo &info);

}  // namespace sde
