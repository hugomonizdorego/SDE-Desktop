// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu, the SDE software center: its data model. One App is one
// application whatever it comes from; its Origins are the sources that can
// install it (a Debian package through PackageKit, a Flatpak ref, a snap).
// Apps of different sources are merged by their AppStream id (snaps name it
// in their "common-ids").
//
// Also here: search ranking, the history of changes (APT's history.log,
// `flatpak history`, snapd changes), what an uninstall from the Start menu
// may remove, and the configuration file (merkadu.conf). Nothing here
// touches FLTK, D-Bus or the package system, so the tests can use it.
#pragma once

#include <ctime>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace sde {
namespace merkadu {

// ------------------------------------------------------------- sources

enum class Source { Debian = 0, Flatpak = 1, Snap = 2 };
constexpr int kSourceCount = 3;

const char *source_id(Source s);  // "debian", "flatpak", "snap"
bool source_from_id(const std::string &id, Source *out);
// Short name for badges: the distribution (os-release NAME without
// "GNU/Linux", e.g. "Debian"), "Flatpak" or "Snap".
std::string source_label(Source s);
// The name of the distribution as used by source_label() (tests set it).
void set_distribution_name(const std::string &name);
// "Debian package", "Flatpak app", "Snap" (translated).
std::string origin_kind_label(Source s);

// ---------------------------------------------------------------- apps

// One way to get an app.
struct Origin {
  Source source = Source::Debian;
  // Debian: binary package name. Flatpak: the full ref
  // ("app/org.gimp.GIMP/x86_64/stable"). Snap: the snap name.
  std::string id;
  // Debian: archive origin/suite. Flatpak: the remote ("flathub"). Snap:
  // the channel ("latest/stable").
  std::string remote;
  std::string version;            // available version ("" when unknown)
  std::string installed_version;
  bool installed = false;
  bool update = false;            // an update is available
  long long download_size = -1;   // bytes, -1 when unknown
  long long installed_size = -1;  // bytes, -1 when unknown
  std::string package_id;         // PackageKit package id, when known
  std::string installation;       // Flatpak: "system" or "user"
  std::string publisher;          // snap publisher, Flatpak developer
  bool verified = false;          // snap publisher validation
  bool classic = false;           // snap with classic confinement
  std::string license;

  // "flatpak:system:app/org.gimp.GIMP/x86_64/stable", "debian:gimp", "snap:gimp".
  std::string key() const;
  // Flatpak: the app id of the ref ("org.gimp.GIMP").
  std::string flatpak_app_id() const;
};

struct Screenshot {
  std::string thumbnail;  // URL or path of a small image (about 600 px wide)
  std::string image;      // URL or path of the full image
  std::string caption;
};

struct Release {
  std::string version;
  std::time_t timestamp = 0;
  std::string description;  // plain text
};

// One icon of an app: an icon name of the theme ("gimp"), a file (cached
// catalog icons, Flatpak exports), an https URL (catalog media, the Snap
// Store) or "snap-icon:NAME" (snapd).
struct IconRef {
  std::string ref;
  int size = 0;  // pixels (with the scale), 0 for icon names and unknown sizes
  bool operator==(const IconRef &o) const { return ref == o.ref && size == o.size; }
};

struct App {
  std::string id;           // normalize_id() of the AppStream id, or deb:/snap: names
  std::string appstream_id; // as published ("org.gimp.GIMP")
  std::string name, summary;
  std::string description;  // plain text: paragraphs separated by a blank line
  std::string icon;         // icon name, absolute path or https URL
  std::vector<IconRef> icons;  // every icon the sources have (see icon_candidates())
  std::string kind = "desktop-application";  // AppStream component kind
  std::vector<std::string> categories;       // freedesktop categories
  std::vector<std::string> keywords;
  std::string developer, license, homepage, bugtracker, help, donation;
  std::string section;  // Debian section, when known ("graphics", "contrib/net")
  std::vector<std::string> pkgnames;    // Debian binary packages of the app
  std::vector<std::string> desktop_ids; // launchables ("org.gimp.GIMP.desktop")
  std::vector<Screenshot> screenshots;
  std::vector<Release> releases;        // newest first
  std::vector<Origin> origins;          // in the preferred order (see sort_origins())

  bool installed() const;
  bool has_update() const;
  bool has_source(Source s) const;
  // The origin to show and act on: an installed one, else the first.
  const Origin *main_origin() const;
  const Origin *installed_origin() const;
  const Origin *origin(Source s) const;
  Origin *origin(Source s);
  std::time_t newest_release() const;
  bool is_app() const;  // a desktop or web application (not a font, add-on, tool)
};

// Adds |icon| to |app| (once); the first one also becomes app.icon.
void add_icon(App &app, const IconRef &icon);
// The icons of |app| to try for |px| pixels, best first: icon names (the
// theme's own drawing, when it has one), then files and URLs of at least
// |px| (smallest first), then smaller ones (largest first). Files before
// URLs of the same size.
std::vector<std::string> icon_candidates(const App &app, int px);
// Generic icon names for an app without an icon of its own: its kind (fonts,
// add-ons), its main freedesktop category, its Debian section, then a
// generic application.
std::vector<std::string> fallback_icon_names(const App &app);

// Lower case, without a ".desktop" suffix: the key apps are merged by.
std::string normalize_id(const std::string &appstream_id);
// Plain text of AppStream description markup (<p>, <ul>/<ol> lists with
// "• " and "1. " bullets, inline tags removed, entities decoded).
std::string description_text(const std::string &markup);

// The apps of every source, merged by id.
class Catalog {
 public:
  // Adds |app| or merges it into the app with the same id: its origins are
  // added (one per source, newer information replaces older for the same
  // source) and empty fields are filled.
  App &add(App app);
  App *find(const std::string &id);
  const App *find(const std::string &id) const;
  // The app that has an origin |id| of |source| (a package, ref or snap).
  App *find_origin(Source source, const std::string &id);
  // The app that has |desktop_id| ("gimp.desktop") as a launchable.
  App *find_desktop(const std::string &desktop_id);
  // Marks every origin of |source| as not installed (before the installed
  // apps of that source are added again).
  void reset_installed(Source source);
  // Removes origins of |source| that are neither installed nor in a
  // catalog (|keep|: Origin::key() values to keep), and apps left without
  // any origin.
  void prune(Source source, const std::set<std::string> &keep);
  // Removes the apps without any origin.
  void remove_empty();
  std::vector<App> &apps() { return apps_; }
  const std::vector<App> &apps() const { return apps_; }
  size_t size() const { return apps_.size(); }
  void clear();
  // Reorders every app's origins: installed first, then |priority|.
  void sort_origins(const std::vector<Source> &priority);

 private:
  void index(size_t i);
  std::vector<App> apps_;
  std::unordered_map<std::string, size_t> by_id_;
  std::unordered_map<std::string, size_t> by_origin_;   // "<source>:<id>"
  std::unordered_map<std::string, size_t> by_desktop_;  // normalized desktop id
};

// Merges an origin |o| into |app| (replacing the one of the same source and,
// for Flatpak, the same installation).
void merge_origin(App &app, const Origin &o);

// ---------------------------------------------------------------- search

// Lower-case words of a query (punctuation separates words).
std::vector<std::string> query_words(const std::string &query);
// How well |app| matches every word (0: it does not). Names count most,
// then ids and keywords, package names, the summary and the description.
int match_score(const App &app, const std::vector<std::string> &words);
// The matching apps, best first (ties by name). Fonts, add-ons and
// command-line tools only when |all_kinds|.
std::vector<const App *> search(const Catalog &catalog, const std::string &query, bool all_kinds = true);

// Apps sorted by name (locale-independent, case-insensitive).
void sort_by_name(std::vector<const App *> &apps);

// ------------------------------------------------------------- packages

// PackageKit package ids: "name;version;arch;data".
struct PackageId {
  std::string name, version, arch, data;
  bool valid = false;
  // True when |data| says the package is installed ("installed",
  // "installed:...", "auto:..." and "manual:..." of the APT backend).
  bool installed() const;
};
PackageId parse_package_id(const std::string &id);

// Debian package names as PackageKit and dpkg accept them, also with an
// architecture ("libc6:i386").
bool valid_package_name(const std::string &name);
bool valid_snap_name(const std::string &name);
// Flatpak refs (app|runtime/ID/ARCH/BRANCH).
bool valid_flatpak_ref(const std::string &ref);

// --------------------------------------------------------------- history

enum class Change { Install = 0, Update = 1, Remove = 2 };

struct HistoryEntry {
  std::time_t when = 0;
  Source source = Source::Debian;
  Change change = Change::Install;
  std::vector<std::string> items;  // packages, refs or snaps (no automatic ones)
  std::vector<std::string> versions;  // the version of each item ("" when unknown)
  int automatic = 0;               // dependencies changed with them (APT)
  std::string requested_by;        // user name (APT Requested-By, flatpak user)
  std::string tool;                // the command line or the program that did it
  bool failed = false;
  std::string error;
};

// /var/log/apt/history.log (one stanza per transaction; Install, Upgrade,
// Downgrade, Remove, Purge and Reinstall lines; "automatic" packages are
// counted, not listed). Entries are appended to |out|.
void parse_apt_history(const std::string &text, std::vector<HistoryEntry> &out);
// `flatpak history --columns=time,change,application,branch,installation,user,tool,version`
// (tab separated; header lines are skipped). Deploys of runtimes are left out.
void parse_flatpak_history(const std::string &text, std::vector<HistoryEntry> &out);
// snapd GET /v2/changes?select=all (JSON). Install, refresh and remove changes.
bool parse_snap_changes(const std::string &json, std::vector<HistoryEntry> &out);
// Newest first.
void sort_history(std::vector<HistoryEntry> &entries);
// "2026-10-20  10:15:42" (local time) -> time_t, 0 when invalid.
std::time_t parse_local_time(const std::string &stamp);
// RFC 3339 ("2026-10-20T08:15:42.123+02:00", "...Z") -> time_t, 0 when invalid.
std::time_t parse_rfc3339(const std::string &stamp);

// Day of a history group: "Today", "Yesterday", the weekday in the current
// week, "October 20" in the current year, else "October 20, 2025".
std::string day_label(std::time_t when, std::time_t now);
std::string time_label(std::time_t when);  // "10:15"
// Days since the epoch in local time (for grouping).
long local_day(std::time_t when);

// -------------------------------------------------------------- uninstall

// Where a launcher (.desktop file) comes from, without asking the package
// system (see classify_launcher()).
enum class LauncherKind {
  Package,      // a system file: ask dpkg which package owns it
  Flatpak,      // X-Flatpak=APPID
  Snap,         // X-SnapInstanceName=NAME or /var/lib/snapd/desktop/applications/NAME_*.desktop
  UserLauncher, // a launcher in the user's home that no package installed
  Invalid       // not a readable desktop file
};
struct Launcher {
  LauncherKind kind = LauncherKind::Invalid;
  std::string path;
  std::string id;         // flatpak app id or snap name
  std::string name, icon; // Name= and Icon= of the entry
  std::string flatpak_installation;  // "user" for ~/.local/share/flatpak exports
};
// |text| is the content of the desktop file at |path|; |home| the user's home.
Launcher classify_launcher(const std::string &path, const std::string &text, const std::string &home);
// Owners of |path| in `dpkg-query -S PATH` output ("pkg1, pkg2: /path";
// "diversion by" lines ignored).
std::vector<std::string> parse_dpkg_owners(const std::string &output, const std::string &path);
// Sum of `dpkg-query -W -f='${Installed-Size}\n'` lines (KiB) in bytes.
long long parse_installed_sizes(const std::string &output);
// `dpkg-query -W -f='${Package}\t${Installed-Size}\t${Essential}\t${Priority}\n'`:
// the space the packages take (bytes, -1 when unknown) and those the
// system needs (Essential: yes, Priority: required).
void parse_removal_query(const std::string &output, long long *freed, std::set<std::string> *system);

// Whether Merkadu may remove |package| when the package system would also
// remove |removed| (the result of a simulation; may contain |package|).
// |system_packages| are the packages the system cannot work without
// (Essential, Priority required, the running kernel): removing them is
// refused like removing the desktop.
struct RemovalPlan {
  bool allowed = false;
  // Why not: the package (or one removed with it) is protected.
  std::string protected_package;
  bool system = false;  // it is a system package (else part of the desktop)
  // Other packages that would go too (sorted, without |package|).
  std::vector<std::string> also_removed;
};
RemovalPlan plan_removal(const std::string &package, const std::vector<std::string> &removed,
                         const std::set<std::string> &protected_packages,
                         const std::set<std::string> &system_packages = {});
// Why Merkadu refuses the removal of |plan| (for |name|, the app or package
// asked for), one sentence.
std::string refusal_text(const RemovalPlan &plan, const std::string &name, const std::string &package);
// The packages Merkadu never removes: sde, sde-desktop, merkadu and the
// [Uninstall] protected= list of merkadu.conf.
std::set<std::string> default_protected_packages();
// System packages in `dpkg-query -W -f='${Package}\t${Essential}\t${Priority}\n'`
// output: Essential: yes or Priority: required.
std::set<std::string> parse_system_packages(const std::string &output);
// The package of the running kernel ("linux-image-" + release), "" when
// unknown.
std::string running_kernel_package(const std::string &release);

// --------------------------------------------------------------- config

#ifndef MERKADU_DATADIR
#define MERKADU_DATADIR "/usr/share/merkadu"
#endif
// Merkadu's data (categories.menu, merkadu.conf): $MERKADU_DATA_DIR, else
// MERKADU_DATADIR (/usr/share/merkadu).
std::string data_dir();

// merkadu.conf: a key file with [Featured] apps=, [Top] apps=, [New]
// count=, [Sources] order=, [Uninstall] protected= (lists separated by ';'
// or ','). Later files override keys of earlier ones.
class KeyFile {
 public:
  void parse(const std::string &text);  // adds to what is there
  std::string get(const std::string &section, const std::string &key, const std::string &fallback = "") const;
  std::vector<std::string> list(const std::string &section, const std::string &key) const;
  int get_int(const std::string &section, const std::string &key, int fallback) const;
  bool has(const std::string &section, const std::string &key) const;
  // The keys of |section| (lower case, sorted).
  std::vector<std::string> keys(const std::string &section) const;

 private:
  std::map<std::string, std::map<std::string, std::string>> values_;
};

struct Settings {
  std::vector<std::string> featured;  // [Highlights] (or [Featured]) apps: app references (see resolve_reference())
  std::vector<std::string> top;       // [Popular] (or [Top]) apps
  // [Recommended]: department id -> well-known apps of that department,
  // recommended to people who install apps of it.
  std::map<std::string, std::vector<std::string>> recommended;
  int new_count = 12;
  std::vector<Source> order = {Source::Debian, Source::Flatpak, Source::Snap};
  std::set<std::string> protected_packages;
  bool hidden_snap_folder = true;  // [Snap] hide-home-folder=
  std::string flathub_url = "https://dl.flathub.org/repo/flathub.flatpakrepo";
  bool setup_flathub = true;       // [Flatpak] setup-flathub=: offer Flathub when it is missing
  bool refresh_on_start = true;    // [Refresh] on-start=
  int refresh_minutes = 30;        // [Refresh] min-age=: lists younger than this are not refreshed on start
  static Settings from(const KeyFile &kf);
};
// Reads data_dir()/merkadu.conf (/usr/share/merkadu), then
// $XDG_CONFIG_DIRS/merkadu/merkadu.conf (/etc/xdg/merkadu) and
// /etc/merkadu/merkadu.conf; $MERKADU_CONFIG replaces all of them.
Settings load_settings();

// An app named in merkadu.conf: an AppStream id ("org.gimp.GIMP"),
// "deb:PACKAGE", "flatpak:APPID" or "snap:NAME"; alternatives separated by
// '|' are tried in order ("org.mozilla.firefox|deb:firefox-esr").
const App *resolve_reference(const Catalog &catalog, const std::string &ref);

// ------------------------------------------------------------- formatting

std::string format_size(long long bytes);  // "84.2 MB"
std::string format_count(long long n);     // "3,512"
// "4 apps", "1 app" (translated).
std::string apps_count(size_t n);
// Splits "a;b,c" lists.
std::vector<std::string> split_list(const std::string &s);
// Text without control characters, at most |max| bytes (UTF-8 safe).
std::string clean_text(const std::string &s, size_t max = 4096);

}  // namespace merkadu
}  // namespace sde
