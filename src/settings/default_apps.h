// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Default Applications (alpha 52), without FLTK:
//  * the categories (Web browser, Mail, Music ...) and their MIME types;
//  * installed applications and the MIME types they declare
//    (/usr/share/applications, ~/.local/share/applications and the Flatpak
//    export folders);
//  * mimeapps.list reading and writing that keeps comments, unknown
//    sections and the order of everything it does not change, and the
//    freedesktop.org lookup of the current default (user files first, then
//    the system and SDE defaults, /etc/xdg/sde-mimeapps.list).
// The terminal has no MIME type: it is [applications] terminal in sde.conf.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace sde {

struct AppCategory {
  std::string id;     // "browser", "mail", ...
  std::string title;  // untranslated (N_ strings)
  std::vector<std::string> icons;
  // Every type the category sets; the first one decides which application
  // is shown as the current default.
  std::vector<std::string> types;
  // An application is offered when it declares one of these types (empty:
  // |types|), or the desktop category below.
  std::vector<std::string> match;
  std::string desktop_category;  // "TerminalEmulator" for the terminal
};

const std::vector<AppCategory> &app_categories();
const AppCategory *find_category(const std::string &id);

struct MimeApp {
  std::string id;  // desktop file ID, "firefox-esr.desktop"
  std::string name, icon, path;
  std::vector<std::string> mime_types, categories;
  bool hidden = false;  // NoDisplay=true: never offered, but still a valid default
  bool supports(const std::string &mime) const;
};

// The applications folders searched, in XDG precedence order (user first),
// including the Flatpak export folders when $XDG_DATA_DIRS lacks them.
std::vector<std::string> application_dirs();
// Installed applications (first ID wins, subfolders give "a-b.desktop"
// IDs, Hidden=true entries mask the ones below), sorted by name.
std::vector<MimeApp> scan_mime_apps(const std::vector<std::string> &dirs);

// Applications offered for |cat|, sorted by name.
std::vector<const MimeApp *> apps_for_category(const AppCategory &cat, const std::vector<MimeApp> &apps);
// Applications that declare |mime|, sorted by name.
std::vector<const MimeApp *> apps_for_type(const std::string &mime, const std::vector<MimeApp> &apps);
// Every MIME type some installed application declares, sorted.
std::vector<std::string> known_mime_types(const std::vector<MimeApp> &apps);

// One mimeapps.list file.
class MimeAppsList {
 public:
  void parse(const std::string &text);
  std::string format() const;
  bool load(const std::string &path);  // false when missing (empty list)
  bool save(const std::string &path) const;

  // Desktop file IDs listed for |mime| in |section| ("Default
  // Applications", "Added Associations", "Removed Associations").
  std::vector<std::string> get(const std::string &section, const std::string &mime) const;
  bool has(const std::string &section, const std::string &mime) const;
  // [Default Applications] mime=app; and |app| no longer removed for |mime|.
  void set_default(const std::string &mime, const std::string &app);
  // Removes mime from [Default Applications].
  void unset_default(const std::string &mime);
  bool changed() const { return changed_; }

 private:
  struct Line {
    std::string text;     // the original line (or the rebuilt one)
    std::string key;      // empty for comments, blanks and headers
    std::string section;  // section it belongs to
    bool header = false;
  };
  std::vector<Line> lines_;
  bool changed_ = false;
  void set_list(const std::string &section, const std::string &mime, const std::vector<std::string> &apps);
  void remove_key(const std::string &section, const std::string &mime);
};

// Files consulted for defaults, most important first: for each config
// folder ($XDG_CONFIG_HOME, then $XDG_CONFIG_DIRS) "<desktop>-mimeapps.list"
// for each name in $XDG_CURRENT_DESKTOP (lower case) and then
// "mimeapps.list"; then the same in the applications data folders.
std::vector<std::string> mimeapps_paths(const std::string &config_home, const std::vector<std::string> &config_dirs,
                                        const std::vector<std::string> &data_dirs, const std::string &desktops);
std::vector<std::string> mimeapps_paths();  // from the environment

// The default for |mime|: Default Applications entries that are installed
// and not removed, in file order; then Added Associations; then the first
// installed application (by name) that declares the type. "" when none.
std::string resolve_default(const std::string &mime, const std::vector<MimeAppsList> &lists,
                            const std::vector<MimeApp> &apps);

// Makes |app| the default for |cat| in |user|: every type of the category
// that the application declares (and always the first type); types it does
// not declare lose their user default so that the system one applies.
void apply_category(MimeAppsList &user, const AppCategory &cat, const MimeApp &app);
// Removes the user defaults of |cat| (the system and SDE defaults apply).
void reset_category(MimeAppsList &user, const AppCategory &cat);

}  // namespace sde
