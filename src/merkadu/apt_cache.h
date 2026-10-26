// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The package system as APT sees it, read with libapt-pkg like Synaptic
// does: every package of the configured repositories with its section,
// versions, sizes and state; the details of one package (description,
// dependencies, versions and where they come from); and the simulation of
// marked changes (APT's dependency resolver on a pkgDepCache), which shows
// everything they install and remove before anything happens. Read-only:
// Merkadu changes the system through PackageKit, authorized by polkit.
//
// libapt-pkg is not thread-safe: everything here runs on one thread of its
// own (queue()), which keeps the cache open between requests.
#pragma once

#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "packages.h"

namespace sde {
namespace merkadu {
namespace apt {

// Merkadu was built with libapt-pkg.
bool available();

// Runs |work| on the APT thread, after the work queued before it.
void queue(std::function<void()> work);

struct Snapshot {
  std::vector<Package> packages;  // sorted by name
  std::string error;              // why the package lists could not be read
  std::time_t lists_time = 0;     // the newest package list (the last refresh)
  double seconds = 0;
};

struct Dependency {
  std::string type;  // "Depends", "Recommends", "Suggests", "Conflicts", "Breaks", "Replaces", "Enhances"
  std::string text;  // "libc6 (>= 2.38)", "default-dbus-session-bus | dbus-session-bus"
};

struct VersionInfo {
  std::string version;
  std::string origin;  // "Debian testing/main", "installed"
  int pin = 0;         // APT's priority
  bool installed = false, candidate = false;
};

struct Details {
  bool found = false;
  std::string name, version;
  std::string description;  // the long description, plain text
  std::string homepage, maintainer, source_package;
  std::vector<Dependency> dependencies;  // of the candidate (else the installed version)
  std::vector<std::string> required_by;  // installed packages that depend on it
  std::vector<std::string> provides;
  std::vector<VersionInfo> versions;
};

// These run on the APT thread only. The cache is opened on first use and
// again after reload() (a change or a refresh of the lists).
Snapshot snapshot();
Details details(const std::string &name);
ChangePlan simulate(const ChangeSet &marks);
void reload();

// The newest package list in APT's lists directory (any thread).
std::time_t lists_time();

}  // namespace apt
}  // namespace merkadu
}  // namespace sde
