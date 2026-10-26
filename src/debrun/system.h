// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The installed system as Debrun sees it, as the user and without a
// display (unit tested): dpkg's status file, the architectures dpkg
// accepts, how a .deb relates to what is installed (install, upgrade,
// reinstall, downgrade), whether its relations are met, and APT's
// simulation of the change (apt-get --simulate), which resolves the
// dependencies from the configured package sources exactly as the real
// installation will.
//
// Test hooks, read by the unprivileged program only: DEBRUN_DPKG_STATUS
// (another status file), DEBRUN_ARCHITECTURES ("amd64 i386": the native
// architecture, then the foreign ones) and DEBRUN_APT_GET (another
// apt-get, e.g. a stub that prints a simulation).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "deb.h"

namespace sde {
namespace debrun {

struct Installed {
  std::string name, version, arch, multi_arch;
  std::string want;   // dpkg selection: install, hold, deinstall, purge
  std::string state;  // installed, config-files, half-installed, unpacked ...
  bool essential = false, is_protected = false;
  std::vector<Relation> provides;
  // Unpacked or configured: its files are there.
  bool present() const { return state != "not-installed" && state != "config-files" && !state.empty(); }
  bool held() const { return want == "hold"; }
};

struct System {
  std::vector<Installed> packages;
  std::multimap<std::string, size_t> by_name;    // name -> index
  std::multimap<std::string, size_t> providers;  // provided name -> index
  std::string native_arch;
  std::vector<std::string> foreign_archs;
  bool loaded = false;  // the status file was read

  // The entries of |name| whose files are there (every architecture).
  std::vector<const Installed *> find(const std::string &name) const;
};

// Fills |out| from the text of dpkg's status file.
void parse_status(const std::string &text, System *out);
// Reads /var/lib/dpkg/status and asks dpkg for the architectures. False
// with *error when the status file cannot be read (not a Debian system).
bool load_system(System *out, std::string *error);
// "amd64" -> "64-bit PC (amd64)"; unknown names are returned as they are.
std::string arch_label(const std::string &arch);

// ----------------------------------------------- the package and the system

enum class Action { install, upgrade, reinstall, downgrade };

struct Fit {
  Action action = Action::install;
  const Installed *current = nullptr;  // the installed package of that name
  // Architecture: true when dpkg accepts it here (all, native, or a foreign
  // architecture added with dpkg --add-architecture).
  bool arch_ok = true;
  bool arch_foreign = false;  // a foreign architecture that is enabled
  std::string problem;  // why it cannot be installed here, "" when it can
};
Fit fit_package(const Package &p, const System &s);

// ------------------------------------------------------- APT's simulation

struct AptItem {
  std::string name, version, old_version, origin, arch;
};

struct AptPlan {
  bool ran = false;  // apt-get was run (and printed something we understand)
  bool ok = false;   // the change is possible
  std::vector<AptItem> install;  // Inst: new packages and upgrades (old_version set)
  std::vector<AptItem> remove;   // Remv
  std::vector<std::string> unmet;   // "pkg : Depends: x but it is not installable"
  std::vector<std::string> errors;  // the "E:" lines, without "E: "
  enum class Problem { none, unmet, held, essential, lock, downgrade, damaged, other };
  Problem problem = Problem::none;
};
// What apt-get -s printed (stdout and stderr together) and its exit code.
AptPlan parse_apt_output(const std::string &output, int status);
// The apt-get arguments of a simulation (the same options as the real
// change, see helper.cpp): "install ./file.deb", "remove NAME".
std::vector<std::string> simulate_install_argv(const std::string &deb, bool reinstall, bool downgrade);
std::vector<std::string> simulate_remove_argv(const std::string &name);
// apt-get (or DEBRUN_APT_GET).
std::string apt_get_program();
// Runs |argv| with LC_ALL=C.UTF-8 and returns what it printed on stdout and
// stderr; *status: its exit code (-1 when it could not run). Blocks: for
// worker threads.
std::string run_capture(const std::vector<std::string> &argv, int *status, int timeout_seconds = 120);

// apt-cache policy NAME: the versions the package sources offer.
struct Policy {
  std::string name;                  // the package it is about
  std::string installed, candidate;  // "" for "(none)"
  // The versions of the version table, highest first, each with its first
  // source ("http://deb.debian.org/debian testing/main amd64 Packages" or
  // "/var/lib/dpkg/status").
  std::vector<std::pair<std::string, std::string>> versions;
  // The highest version a package source offers (not only dpkg's status), "".
  std::string repo_version() const;
  std::string repo_source() const;
};
Policy parse_policy(const std::string &output);
// apt-cache (or DEBRUN_APT_CACHE).
std::string apt_cache_program();

// ------------------------------------------------------------ relations

struct DepCheck {
  std::string field;  // Pre-Depends, Depends, Recommends, Conflicts, Breaks
  Alternatives alts;
  enum class State { installed, provided, will_install, missing, conflict, removed, ok };
  State state = State::missing;
  std::string detail;  // the version installed or to be installed, the provider ...
};
// Each relation of |p| against the system and, when it ran, APT's plan.
std::vector<DepCheck> check_relations(const Package &p, const System &s, const AptPlan &plan);

}  // namespace debrun
}  // namespace sde
