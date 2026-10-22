// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Welcome model: the start-up preference, firewall and codec status and
// the tools the First Steps open. Display-free, unit tested.
#pragma once

#include <string>
#include <vector>

namespace sde {
class Config;
namespace welcome {

extern const char *const kProjectUrl;  // SDE on GitHub
extern const char *const kIssuesUrl;
extern const char *const kDonateUrl;   // PayPal
extern const char *const kDocsUrl;     // docs/ on GitHub

// sde.conf [welcome] show (default true): open SDE Welcome at login.
bool show_at_startup(const Config &cfg);
void set_show_at_startup(Config &cfg, bool show);

// Firewall (ufw). /etc/ufw/ufw.conf is world readable: ENABLED=yes means
// ufw starts at boot. "ufw status" output (root only) -> "active",
// "inactive" or "" when unknown.
bool ufw_conf_enabled(const std::string &ufw_conf);
std::string parse_ufw_status(const std::string &output);

// Debian's GStreamer codec packages (all in main): good, bad, ugly, libav.
const std::vector<std::string> &codec_packages();
// |wanted| minus |installed|, in |wanted|'s order.
std::vector<std::string> missing_packages(const std::vector<std::string> &wanted,
                                          const std::vector<std::string> &installed);

// A program the First Steps start, first installed candidate wins.
struct Tool {
  std::string name;               // "GNOME Software"
  std::vector<std::string> argv;  // "gnome-software"
};
// Software centers in order of preference (installed ones only).
std::vector<Tool> software_centers();
// Timeshift's launcher (asks for the password itself), or an empty argv.
Tool snapshot_tool();
// A firewall front end (gufw), or an empty argv.
Tool firewall_tool();

// Panel layouts offered by the First Steps ([panel] style).
struct Layout {
  const char *id;      // full, short, floating
  const char *label;
  const char *description;
};
const std::vector<Layout> &panel_layouts();

}  // namespace welcome
}  // namespace sde
