// SPDX-License-Identifier: GPL-3.0-or-later
// SDE Update data model: parsers for the helper protocols in data/update
// (check.py, history.py, sources.py, transaction.py) and formatting helpers.
// Nothing here touches FLTK, the network or the package system.
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
  bool selected = true;
};

struct UpdateList {
  bool valid = false;
  int protocol = 0;
  std::vector<Update> updates;
  std::vector<std::string> held;
  long long total_size() const;
  int count(Kind kind) const;
};

// Accepts protocol 2 (and the six-field protocol 1 of SDE Update 1.0).
UpdateList parse_updates(const std::string &output);

// Case-insensitive search over names, packages and summary.
bool matches(const Update &u, const std::string &query);

// Binary package names of the selected updates. |all| is set when every
// update is selected (then the whole system can be upgraded at once).
std::vector<std::string> selected_packages(const std::vector<Update> &updates, bool *all);

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

}  // namespace update
}  // namespace sde
