// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The software sources of Merkadu. A Backend installs, removes and updates
// software of one source and reports what is installed:
//
//   Debian   PackageKit over D-Bus (org.freedesktop.PackageKit), polkit
//            through the SDE polkit agent (packagekit.cpp)
//   Flatpak  libflatpak, the system installation (flatpak.cpp)
//   Snap     snapd's REST API on /run/snapd.socket (snap.cpp)
//
// MERKADU_FAKE=DIR replaces all three with fakes that read DIR/catalog.json
// (fake.cpp; the UI checks and screenshots use tests/fixtures/merkadu).
// Every callback runs on the FLTK thread.
#pragma once

#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "model.h"

namespace sde {
namespace merkadu {

enum class Action { Install = 0, Remove = 1, Update = 2 };

// A change the user asked for; the Store runs them one after the other.
struct Job {
  int id = 0;
  Action action = Action::Install;
  Source source = Source::Debian;
  std::string app_id;   // the app it is about ("" for several apps)
  std::string title;    // what the progress view shows ("GIMP", "3 apps")
  std::string icon;
  // Packages, Flatpak items ("system:app/org.gimp.GIMP/x86_64/stable") or
  // snap names.
  std::vector<std::string> targets;
  std::string remote;   // Flatpak remote to install from; snap channel
  bool classic = false; // a snap with classic confinement
  // Progress, updated while it runs.
  enum State { Queued, Running, Done, Failed, Cancelled };
  State state = Queued;
  int percent = -1;
  std::string status;
  std::string error;
  bool cancellable = false;
  std::time_t finished = 0;
  bool active() const { return state == Queued || state == Running; }
};

struct Progress {
  int percent = -1;  // -1: unknown
  std::string status;
  bool cancellable = false;
};

// What a source can do now (the Sources page).
struct SourceStatus {
  bool available = false;  // can install and remove
  bool installed = false;  // the software for it is installed (flatpak, snapd)
  std::string message;     // why not, or a short state ("PackageKit 1.3.3")
  std::vector<std::string> remotes;  // Flatpak remotes
  bool has_flathub = false;
  int hidden_snap_folder = -1;  // snapd's experimental.hidden-snap-folder (-1 unknown)
};

using AppsFn = std::function<void(std::vector<App> apps, std::string error)>;
using DoneFn = std::function<void(bool ok, bool cancelled, std::string error)>;

class Backend {
 public:
  virtual ~Backend() = default;
  virtual Source source() const = 0;
  // Checks the source (daemon, socket, installation) in the background.
  virtual void probe(std::function<void(SourceStatus)> done) = 0;
  // The installed software of the source (apps with installed origins;
  // updates are marked).
  virtual void load_installed(AppsFn done) = 0;
  // Search of a remote store (snapd); the catalog covers the others.
  virtual void search(const std::string &query, AppsFn done) { done({}, ""); }
  // Apps of a store category (snapd).
  virtual void browse(const std::string &category, AppsFn done) { done({}, ""); }
  // The store's version of a catalog app, found by its AppStream id
  // (snapd's common-id).
  virtual void lookup(const App &app, AppsFn done) { done({}, ""); }
  // Sizes and versions of |o| that the catalog does not have.
  virtual void details(const App &app, const Origin &o, std::function<void(Origin)> done) {}
  // Runs |job|: |progress| any number of times, then |done| once.
  virtual void run(const Job &job, std::function<void(const Progress &)> progress, DoneFn done) = 0;
  virtual void cancel(int job_id) {}
  // The source's history of changes.
  virtual void history(std::function<void(std::vector<HistoryEntry>)> done) { done({}); }
  // Debian: what removing |packages| would also remove, and the space it
  // frees (bytes, -1 unknown). Others: just the targets.
  virtual void plan_remove(const std::vector<std::string> &packages,
                           std::function<void(std::vector<std::string> removed, long long freed,
                                              std::string error)> done) {
    done(packages, -1, "");
  }
  // Debian: the packages that own each of |paths| (dpkg -S); paths no
  // package owns are left out.
  using OwnersFn = std::function<void(std::map<std::string, std::vector<std::string>>)>;
  virtual void owners(const std::vector<std::string> &paths, OwnersFn done) { done({}); }
  // Flatpak: adds the Flathub remote. Snap: hides ~/snap. Debian: nothing.
  virtual void setup(const std::string &what, DoneFn done) { done(false, false, "not supported"); }
};

// The catalog of AppStream data (Debian and Flatpak): loaded in the
// background, or from the fake catalog.
using CatalogFn = std::function<void(std::vector<App> apps, std::string error)>;
void load_catalog(CatalogFn done);

std::unique_ptr<Backend> make_packagekit_backend();
std::unique_ptr<Backend> make_flatpak_backend();
std::unique_ptr<Backend> make_snap_backend();
// The fakes of MERKADU_FAKE (all three sources share one fixture).
std::unique_ptr<Backend> make_fake_backend(Source source, const std::string &dir);
void load_fake_catalog(const std::string &dir, CatalogFn done);
bool fake_mode();
std::string fake_dir();

// Runs |fn| on the FLTK thread (from any thread; Fl::lock() must have been
// called once by main()).
void post(std::function<void()> fn);
// Runs |work| on a new thread and hands its result to |done| on the FLTK
// thread.
template <class T>
void background(std::function<T()> work, std::function<void(T)> done) {
  std::thread([work, done]() {
    auto result = std::make_shared<T>(work());
    post([done, result]() { done(std::move(*result)); });
  }).detach();
}

}  // namespace merkadu
}  // namespace sde
