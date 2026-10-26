// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// How Debrun makes its changes. Two ways, both authorized by polkit (the
// SDE polkit agent asks for the password):
//
//   apt         debrun-helper through pkexec (helper.h): APT installs the
//               copy of the file the user saw, resolving its dependencies
//               from the configured package sources; the default.
//   packagekit  PackageKit's InstallFiles, RemovePackages and RefreshCache
//               over D-Bus (org.freedesktop.PackageKit), as Merkadu uses it.
//
// [debrun] backend=auto|apt|packagekit in sde.conf (or DEBRUN_BACKEND)
// chooses; auto takes apt when pkexec and the helper are installed, else
// PackageKit. Tests: DEBRUN_TEST_HELPER runs another helper program
// directly (without pkexec, so it gains nothing), DEBRUN_PACKAGEKIT_BUS=
// session talks to a PackageKit on the session bus.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sde {
namespace debrun {

struct Change;
struct Package;

struct Step {
  int percent = -1;      // of the whole change, -1 when unknown
  std::string phase;     // copy, check, wait, download, unpack, configure, remove, trigger, refresh, auth
  std::string package;   // the package dpkg works on, or ""
};

struct Outcome {
  bool ok = false;
  bool cancelled = false;  // the user cancelled (also the password dialog)
  std::string code;        // helper.h's error codes, "auth", "backend"
  std::string message;     // a sentence for the user
  std::string detail;      // what APT or PackageKit said
};

struct Callbacks {
  std::function<void(const Step &)> step;
  std::function<void(const std::string &)> log;
  std::function<void(const Outcome &)> done;  // exactly once
};

class Backend {
 public:
  virtual ~Backend() = default;
  // "APT" or "PackageKit", for the details.
  virtual std::string name() const = 0;
  // Installs |path| (the package |p| the user saw: its digest, name,
  // version, architecture); |removals| are the packages the user was shown
  // that the change removes.
  virtual void install(const Package &p, bool reinstall, bool downgrade, const std::vector<std::string> &removals,
                       Callbacks cb) = 0;
  // Removes the installed package |name| (and |also|, which depend on it).
  virtual void remove(const std::string &name, const std::vector<std::string> &also, Callbacks cb) = 0;
  // Downloads the package lists again (apt-get update).
  virtual void refresh(Callbacks cb) = 0;
  virtual bool running() const = 0;
};

std::unique_ptr<Backend> make_helper_backend();
std::unique_ptr<Backend> make_packagekit_backend();
// The backend of the configuration (see above).
std::unique_ptr<Backend> make_backend();

// A sentence for an error code of the helper (helper.h), with |detail|
// (what APT said) when there is no better one.
std::string error_message(const std::string &code, const std::string &detail);

}  // namespace debrun
}  // namespace sde
