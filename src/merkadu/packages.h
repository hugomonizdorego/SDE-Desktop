// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's "All Software": every package of the configured repositories,
// the way Synaptic works with them, for experienced users. Packages are
// browsed by Debian section and by status (installed, with an update, not
// installed, marked), searched by name and description, and changes are
// marked first and applied together after a summary of everything that is
// installed, updated and removed with them.
//
// The list and the simulation of marked changes come from libapt-pkg
// (apt_cache.cpp); this file is the model around them and touches neither
// APT nor FLTK, so the tests can use it.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace sde {
namespace merkadu {

// One package of one architecture.
struct Package {
  std::string name;      // "gimp"; packages of other architectures "libc6:i386"
  std::string arch;      // "amd64", "all", "i386"
  std::string section;   // as the archive has it: "graphics", "contrib/games", "non-free-firmware/kernel"
  std::string summary;   // the short description
  std::string installed_version;  // "" when not installed
  std::string candidate_version;  // the version APT would install ("" when none)
  std::string origin;    // where the candidate comes from: "Debian testing/main"
  long long download_size = -1;   // of the candidate (bytes)
  long long installed_size = -1;  // of the candidate when installed (bytes)
  long long current_size = -1;    // of the installed version (bytes)
  int priority = 0;      // 1 required, 2 important, 3 standard, 4 optional, 5 extra (0 unknown)
  bool essential = false;
  bool installed = false;
  bool upgradable = false;  // the candidate is newer than the installed version
  bool automatic = false;   // installed as a dependency
  bool config_only = false; // removed, its configuration files are left
  bool broken = false;      // installed with unmet dependencies

  // Essential, Priority required: the system needs it.
  bool system() const { return essential || priority == 1; }
};

// --------------------------------------------------------------- sections

// "contrib/games" -> "games"; "" -> "".
std::string section_base(const std::string &section);
// "contrib/games" -> "contrib"; "games" -> "main".
std::string section_component(const std::string &section);
// The name of a Debian section for people ("libdevel" -> "Libraries -
// Development"); unknown sections keep their name, "" is "Other".
std::string section_title(const std::string &base);
// Icon names for packages of a section, best first.
std::vector<std::string> section_icon_names(const std::string &base);

// ----------------------------------------------------------------- marks

enum class Mark { Unmarked = 0, Install, Remove, Upgrade };

// The changes the user marked, by package name.
class ChangeSet {
 public:
  // Mark::Unmarked unmarks.
  void mark(const std::string &name, Mark m);
  Mark get(const std::string &name) const;
  void clear() { marks_.clear(); }
  bool empty() const { return marks_.empty(); }
  size_t size() const { return marks_.size(); }
  size_t count(Mark m) const;
  std::vector<std::string> names(Mark m) const;  // sorted
  const std::map<std::string, Mark> &all() const { return marks_; }

 private:
  std::map<std::string, Mark> marks_;
};

// What the marked changes do with everything they pull in (APT's
// simulation of them).
struct ChangePlan {
  bool valid = false;  // simulated
  std::vector<std::string> install;    // new packages
  std::vector<std::string> upgrade;
  std::vector<std::string> downgrade;
  std::vector<std::string> remove;
  std::set<std::string> marked;        // the packages the user marked (the rest comes with them)
  long long download = -1;             // bytes to download
  long long disk = 0;                  // bytes of disk space used (> 0) or freed (< 0)
  std::string error;                   // why they cannot be made (dependencies)
  size_t changes() const { return install.size() + upgrade.size() + downgrade.size() + remove.size(); }
};

// The removals of |plan| Merkadu refuses: packages the system needs
// (|system|: Essential, required, the running kernel) and the desktop's
// (|protected_packages|). Pairs of package and whether it is a system one.
std::vector<std::pair<std::string, bool>> refused_removals(const ChangePlan &plan,
                                                           const std::set<std::string> &system,
                                                           const std::set<std::string> &protected_packages);
// "2 to install, 1 to remove" (translated); "" for none.
std::string plan_counts(const ChangePlan &plan);

// ---------------------------------------------------------------- filters

enum class PackageStatus { All = 0, Installed, Upgradable, NotInstalled, Marked, ConfigOnly };
constexpr int kPackageStatusCount = 6;
const char *package_status_id(PackageStatus s);  // "all", "installed", ... (views, QA dumps)
bool package_status_from_id(const std::string &id, PackageStatus *out);
bool status_matches(const Package &p, PackageStatus s, const ChangeSet &marks);

struct PackageFilter {
  std::string section;  // a base section ("graphics"), "" for all
  PackageStatus status = PackageStatus::All;
  std::string query;    // words that must be in the name or the summary
};

// Indexes of the packages that pass |f|: by relevance when there is a
// query (name, then the start of the name, then the summary), else in the
// order of |packages| (sorted by name).
std::vector<size_t> filter_packages(const std::vector<Package> &packages, const PackageFilter &f,
                                    const ChangeSet &marks);
// How well |p| matches every word (0: it does not).
int package_match_score(const Package &p, const std::vector<std::string> &words);

struct SectionCount {
  std::string id;     // base section
  std::string title;
  size_t count = 0;
};
// The sections of the packages with |status| and how many each has, by
// title.
std::vector<SectionCount> section_counts(const std::vector<Package> &packages, PackageStatus status,
                                         const ChangeSet &marks);

// Packages sorted by name.
void sort_packages(std::vector<Package> &packages);

}  // namespace merkadu
}  // namespace sde
