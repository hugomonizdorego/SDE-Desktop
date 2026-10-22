// SPDX-License-Identifier: GPL-3.0-or-later
// Freedesktop.org Desktop Entry parsing (applications and autostart).
#pragma once

#include <string>
#include <vector>

namespace sde {

struct DesktopEntry {
  std::string id;          // desktop file ID, e.g. "org.gnome.Calculator.desktop"
  std::string path;        // absolute path of the .desktop file
  std::string type;        // Application, Link, Directory
  std::string name;        // localized Name
  std::string generic_name;
  std::string comment;
  std::string icon;
  std::string exec;
  std::string try_exec;
  std::string work_dir;    // Path=
  std::string url;         // URL= for Type=Link
  std::string startup_wm_class;  // StartupWMClass=, matches WM_CLASS of windows
  std::vector<std::string> keywords;
  std::vector<std::string> categories;
  std::vector<std::string> mime_types;  // MimeType=
  std::string exec_arg;                 // X-TerminalArgExec= (or X-ExecArg=)
  bool has_exec_arg = false;
  std::vector<std::string> only_show_in;
  std::vector<std::string> not_show_in;
  bool terminal = false;
  bool no_display = false;
  bool hidden = false;

  // Parses a .desktop file. Returns false when it has no [Desktop Entry].
  bool load(const std::string &file, const std::string &file_id = "");

  // True when the entry should be shown/started in the given desktop
  // (OnlyShowIn/NotShowIn/Hidden/TryExec rules).
  bool visible_in(const std::string &desktop) const;

  // Exec= expanded into an argument vector (no shell involved).
  // Field codes for files and URLs are expanded with |files| (may be empty).
  std::vector<std::string> command(const std::vector<std::string> &files = {}) const;

  // Main application category used by the SDE menu.
  std::string menu_category() const;
};

// All installed applications, de-duplicated by desktop file ID following the
// XDG data directory precedence. Hidden and NoDisplay entries are skipped.
std::vector<DesktopEntry> load_applications(const std::string &desktop = "SDE");
// Cheap fingerprint (directory mtimes) of every XDG applications directory:
// it changes when .desktop files are added, removed or replaced, so callers
// can poll it instead of parsing all entries again.
std::string applications_stamp();

// Finds an installed application by desktop file ID ("firefox.desktop").
bool find_application(const std::string &id, DesktopEntry &out);

// Splits a command line using the Desktop Entry quoting rules (which are a
// subset of POSIX shell quoting). No variables, globs or pipes are expanded.
std::vector<std::string> split_command(const std::string &cmd);

// Ordered list of menu categories as shown in the SDE application menu.
const std::vector<std::string> &menu_categories();

}  // namespace sde
