// SPDX-License-Identifier: GPL-3.0-or-later
// Software Updater (sde-update) data model: parsers for the helper protocols
// in data/update (check.py, history.py, sources.py, transaction.py), the
// update groups and formatting helpers. Nothing here touches FLTK, the
// network or the package system. The Software Updater updates the operating
// system (Debian packages); applications from Flathub and the Snap Store are
// updated in Merkadu (docs/MERKADU.md).
#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace sde {
namespace update {

enum class Kind { Security = 0, Kernel = 1, Software = 2, Backport = 3 };
constexpr int kKindCount = 4;

Kind kind_from_id(const std::string &id);  // "security", "kernel", "backport", else Software
const char *kind_title(Kind kind);         // translated group title
const char *kind_badge(Kind kind);         // translated short badge text

// The groups of the Software Updater, in display order. See classify().
enum class Group { Critical = 0, Security = 1, Main = 2 };
constexpr int kGroupCount = 3;
const char *group_title(Group group);  // "Critical updates", ... (translated)
const char *group_badge(Group group);  // "Critical", "Security", "Main"
const char *group_id(Group group);     // critical, security, main

// One update, grouped by source package (see data/update/model.py).
struct Update {
  Kind kind = Kind::Software;
  std::string name;          // display name (usually the source package)
  std::string main_package;  // the binary package that represents the group
  std::string source;
  std::string old_version, new_version;
  long long size = 0;  // download size in bytes
  std::vector<std::string> packages;
  std::string origin, archive, site, summary, description;
  // Since Alpha 51 (optional fields of the U record).
  std::string priority, section, urgency;  // Debian control fields, changelog urgency
  std::string app_name, icon;              // from the package's desktop file
  Group group = Group::Main;  // set by parse_updates()
  bool reboot = false;        // installing it needs a restart (see needs_reboot())
  bool selected = true;
  // What the window shows: the application name, else the short description
  // (capitalized), else the package name.
  std::string title() const;
};

// Critical: Priority required/important, the kernel, libc, systemd, apt,
// dpkg, boot loader, firmware and microcode, urgency critical/high/emergency,
// or anything that needs a restart. Security: the Debian security archive
// (Debian-Security label / *-security suite). Main: everything else from the
// package archive (including backports).
Group classify(const Update &u);
bool needs_reboot(const Update &u);

struct UpdateList {
  bool valid = false;
  int protocol = 0;
  std::vector<Update> updates;
  std::vector<std::string> held;
  long long total_size() const;
  int count(Kind kind) const;
  int count(Group group) const;
  long long size(Group group) const;           // all updates of the group
  long long selected_size() const;             // what Install Now downloads
  int selected_count() const;
  bool needs_reboot() const;                   // a selected update needs a restart
};

// Accepts protocol 2 (and the six-field protocol 1 of SDE Update 1.0).
// Updates are classified and sorted by group, then title.
UpdateList parse_updates(const std::string &output);
// Sorts by group then title.
void sort_updates(std::vector<Update> &updates);

// Case-insensitive search over names, packages and summary.
bool matches(const Update &u, const std::string &query);

// Binary package names of the selected updates. |all| is set when every
// update is selected (then the whole system can be upgraded at once).
std::vector<std::string> selected_packages(const std::vector<Update> &updates, bool *all);
// The transaction.py argument lists that install the selection: {"upgrade"}
// (everything) or {"upgrade", pkg...} (apt-get install --only-upgrade).
// Empty when nothing is selected.
std::vector<std::vector<std::string>> install_commands(const std::vector<Update> &updates);

struct Change {
  std::string action, package, arch, old_version, new_version;
  bool automatic = false;
};

struct HistoryEntry {
  std::string start, end, commandline, requested_by, error;
  std::vector<Change> changes;
  // "Upgraded 3 · Installed 1" (translated).
  std::string summary() const;
  int count(const std::string &action) const;
};

bool parse_history(const std::string &output, std::vector<HistoryEntry> &out);
bool matches(const HistoryEntry &e, const std::string &query);

struct Source {
  std::string file;
  int line = 0;
  std::string format;  // one-line or deb822
  bool enabled = true;
  std::string types, uris, suites, components, signed_by;
};

bool parse_sources(const std::string &output, std::vector<Source> &out);

// State of a running transaction, fed line by line from transaction.py.
struct Progress {
  bool started = false, finished = false;
  std::string action;   // refresh, upgrade, autoremove, clean, repair
  std::string backend;  // packagekit or apt
  bool cancellable = false, cancel_requested = false;
  std::string status;  // e.g. "Downloading packages"
  int percent = -1;
  std::string item_info, item_name, item_version;
  int items_seen = 0;
  std::vector<std::string> seen;  // package names reported so far
  std::vector<std::string> log, errors;
  int exit_code = -1;
  std::string outcome;  // ok, nothing, failed, cancelled, denied (or "" while running)
  bool ok() const { return finished && (outcome == "ok" || outcome == "nothing"); }
};

void apply_transaction_line(Progress &p, const std::string &line);
// Readable status for the user, e.g. "Downloading packages" -> translated.
std::string status_text(const std::string &status);

// Package names that `apt-get -s autoremove` would remove ("Remv name [ver]").
std::vector<std::string> parse_autoremove(const std::string &simulation);

// Options for the automatic check interval, in hours (0 = never).
const std::vector<int> &interval_hours();
std::string interval_label(int hours);

std::string format_size(long long bytes);              // "84.2 MB"
std::string format_age(std::time_t then, std::time_t now);  // "2 hours ago"
// "2026-09-17 02:20:17" -> "Sep 17, 2026 · 02:20".
std::string format_date(const std::string &stamp);

// Debian package names as accepted by the helpers.
bool valid_package_name(const std::string &name);

// Remind Me Later: choices in hours and their labels.
const std::vector<int> &remind_hours();
std::string remind_label(int hours);

}  // namespace update
}  // namespace sde
