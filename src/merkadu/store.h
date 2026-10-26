// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's state on the FLTK thread: the catalog of every source, what is
// installed, every package of the repositories (All Software), the changes
// marked there, the queue of changes, the refreshes of the software lists
// and the history. The window and the uninstall dialog read it and listen
// to its events; the backends feed it.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "appstream_catalog.h"
#include "apt_cache.h"
#include "backend.h"
#include "categories.h"
#include "model.h"
#include "packages.h"

namespace sde {
namespace merkadu {

enum class Event {
  Catalog,   // apps were added or their installed state changed
  Jobs,      // a change was queued, started or finished
  Progress,  // a running change made progress
  History,   // the history was loaded
  Sources,   // a source's status changed
  Search,    // results of a store search arrived
  Refresh,   // the refresh of the software lists started, progressed or ended
  Packages,  // the package lists (All Software) or the marks changed
};

// The refresh of the software lists of every source.
struct RefreshState {
  bool running = false;
  bool user = false;         // asked for with the Refresh button
  std::string status;        // what it does now ("Downloading the package lists")
  int percent = -1;
  std::time_t finished = 0;  // when the last one ended
  // What failed, by source ("Debian: Cannot download ..."); notes are
  // things that were skipped on purpose (no network, not authorized).
  std::vector<std::string> errors, notes;
};

class Store {
 public:
  static Store &get();

  // Loads the settings and departments and starts loading the catalog,
  // what is installed and the sources' status. |catalog|: false for the
  // uninstall dialog, which only needs what is installed.
  void start(bool catalog = true);
  const Settings &settings() const { return settings_; }
  const Categories &categories() const { return categories_; }
  Catalog &catalog() { return catalog_; }
  const App *app(const std::string &id) const { return catalog_.find(id); }
  bool catalog_loaded() const { return catalog_loaded_; }
  bool catalog_loading() const { return catalog_loading_; }
  bool installed_loaded(Source s) const { return installed_loaded_[int(s)]; }
  bool all_installed_loaded() const;
  const std::string &catalog_error() const { return catalog_error_; }
  // Where the catalog came from (what is missing when it is empty).
  const CatalogSources &catalog_sources() const { return catalog_sources_; }
  // Apps of each source in the catalog.
  int catalog_count(Source s) const { return catalog_counts_[int(s)]; }
  const SourceStatus &status(Source s) const { return status_[int(s)]; }
  bool probed(Source s) const { return probed_[int(s)]; }
  Backend *backend(Source s) { return backends_[int(s)].get(); }
  void probe_sources();

  int listen(std::function<void(Event)> fn);
  void unlisten(int id);

  // Changes. Returns the job id (0 when the request is invalid).
  int install(const std::string &app_id, Source source);
  int remove(const std::string &app_id, Source source);
  int update(const std::string &app_id);
  // Updates every Flatpak and Snap app (the system's packages are Software
  // Updater's).
  int update_all();
  int enqueue(Job job);
  void cancel(int job_id);
  const std::vector<Job> &jobs() const { return jobs_; }
  const Job *job(int id) const;
  // The queued or running job of an app (nullptr when none).
  const Job *active_job(const std::string &app_id) const;
  int active_jobs() const;
  void clear_finished();
  // Called when a job ends (the uninstall dialog exits with it).
  std::function<void(const Job &)> on_job_done;

  // Snap Store search and categories, merged into the catalog.
  void search_store(const std::string &query);
  void browse_store(const std::string &category_id);
  const std::string &store_query() const { return store_query_; }
  bool store_searching() const { return store_searching_; }
  // Sizes and versions for the details page (once per origin).
  void fetch_details(const std::string &app_id);

  const std::vector<HistoryEntry> &history() const { return history_; }
  bool history_loaded() const { return history_loaded_; }
  void load_history();

  // Starts an installed app (its desktop file, else flatpak/snap run).
  bool launch(const std::string &app_id);
  // Apps with installed origins, with updates.
  std::vector<const App *> installed_apps() const;
  std::vector<const App *> updatable_apps() const;
  // The title of an item of the history (an app name when known).
  std::string item_title(Source source, const std::string &item) const;

  // Reloads what is installed (after a change, or by the user).
  void reload_installed(Source s);
  void reload_all();
  // Loads the AppStream catalog again (after a Flatpak remote was added).
  void reload_catalog();
  // Changes whenever the catalog was rebuilt (for caches of the pages).
  int generation() const { return generation_; }

  // Refreshes the software lists of every source: PackageKit's refresh of
  // the package lists (APT downloads the AppStream catalog with them), the
  // AppStream data of the Flatpak remotes and the Snap Store's updates,
  // then loads everything again. |user|: the Refresh button (always, and
  // polkit may ask); else only lists older than [Refresh] min-age.
  void refresh(bool user);
  const RefreshState &refresh_state() const { return refresh_; }

  // All Software: every package APT knows (libapt-pkg).
  bool packages_available() const { return apt::available() && !fake_mode(); }
  bool packages_loaded() const { return packages_loaded_; }
  bool packages_loading() const { return packages_loading_; }
  const std::string &packages_error() const { return packages_error_; }
  const std::vector<Package> &packages() const { return packages_; }
  const Package *package(const std::string &name) const;
  void load_packages();
  // Changes whenever the package list was loaded again.
  int packages_generation() const { return packages_generation_; }
  // The app (catalog) of a package, nullptr when it is not one.
  const App *app_of_package(const std::string &name) const;
  // The details of a package (description, dependencies, versions).
  void package_details(const std::string &name, std::function<void(const apt::Details &)> done);
  // The marked changes and what they do with their dependencies.
  const ChangeSet &marks() const { return marks_; }
  void mark(const std::string &name, Mark m);
  void clear_marks();
  void plan_marks(std::function<void(const ChangePlan &)> done);
  // Queues the marked changes (removals, installations, updates) and
  // clears the marks; the number of jobs.
  int apply_marks();
  // The packages the system needs (Essential, Priority required, the
  // running kernel): never removed by Merkadu.
  std::set<std::string> system_packages() const;

 private:
  Store() = default;
  void emit(Event e);
  void rebuild();
  void run_next();
  void job_finished(int id, bool ok, bool cancelled, const std::string &error);
  void resolve_owners();
  void apply_packages(apt::Snapshot snap);
  void refresh_step_done(const std::string &source, bool ok, bool cancelled, const std::string &error);
  Job *find_job(int id);

  Settings settings_;
  Categories categories_;
  Catalog catalog_;
  std::unique_ptr<Backend> backends_[kSourceCount];
  SourceStatus status_[kSourceCount];
  bool probed_[kSourceCount] = {false, false, false};
  // What each part contributed; rebuild() merges them into catalog_.
  std::vector<App> catalog_apps_;
  std::vector<App> installed_[kSourceCount];
  std::vector<App> store_apps_;  // snap search and category results
  std::map<std::string, Origin> details_;  // Origin::key() -> fetched details
  std::set<std::string> details_asked_;
  std::set<std::string> lookups_asked_;
  std::map<std::string, std::string> owners_;  // desktop id -> package
  bool owners_asked_ = false;
  bool catalog_loaded_ = false, catalog_loading_ = false, catalog_again_ = false;
  bool installed_loaded_[kSourceCount] = {false, false, false};
  std::string catalog_error_;
  CatalogSources catalog_sources_;
  int catalog_counts_[kSourceCount] = {0, 0, 0};
  std::vector<Job> jobs_;
  int next_job_ = 1;
  bool running_ = false;
  std::vector<HistoryEntry> history_;
  bool history_loaded_ = false;
  bool history_loading_ = false;
  std::string store_query_;
  bool store_searching_ = false;
  std::set<std::string> browsed_;
  std::map<int, std::function<void(Event)>> listeners_;
  int next_listener_ = 1;
  bool started_ = false;
  int generation_ = 0;
  RefreshState refresh_;
  int refresh_pending_ = 0;
  bool refresh_catalog_ = false;  // a refresh changed what the catalog reads
  std::vector<Package> packages_;
  std::map<std::string, size_t> package_index_;
  bool packages_loaded_ = false, packages_loading_ = false, packages_again_ = false;
  std::string packages_error_;
  int packages_generation_ = 0;
  ChangeSet marks_;
};

// Short verb for a job: "Installing", "Removing", "Updating" (translated).
std::string action_progress_label(Action a);
// "Install", "Remove", "Update".
std::string action_label(Action a);
// A one-line description of where an origin comes from: "Debian package
// gimp", "Flatpak org.gimp.GIMP from flathub", "Snap gimp (latest/stable)".
std::string origin_line(const Origin &o);

}  // namespace merkadu
}  // namespace sde
