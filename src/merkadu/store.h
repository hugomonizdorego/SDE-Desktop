// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's state on the FLTK thread: the catalog of every source, what is
// installed, the queue of changes and the history. The window and the
// uninstall dialog read it and listen to its events; the backends feed it.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "backend.h"
#include "categories.h"
#include "model.h"

namespace sde {
namespace merkadu {

enum class Event {
  Catalog,   // apps were added or their installed state changed
  Jobs,      // a change was queued, started or finished
  Progress,  // a running change made progress
  History,   // the history was loaded
  Sources,   // a source's status changed
  Search,    // results of a store search arrived
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
  bool installed_loaded(Source s) const { return installed_loaded_[int(s)]; }
  bool all_installed_loaded() const;
  const std::string &catalog_error() const { return catalog_error_; }
  const SourceStatus &status(Source s) const { return status_[int(s)]; }
  Backend *backend(Source s) { return backends_[int(s)].get(); }
  void probe_sources();

  int listen(std::function<void(Event)> fn);
  void unlisten(int id);

  // Changes. Returns the job id (0 when the request is invalid).
  int install(const std::string &app_id, Source source);
  int remove(const std::string &app_id, Source source);
  int update(const std::string &app_id);
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

 private:
  Store() = default;
  void emit(Event e);
  void rebuild();
  void run_next();
  void job_finished(int id, bool ok, bool cancelled, const std::string &error);
  void resolve_owners();
  Job *find_job(int id);

  Settings settings_;
  Categories categories_;
  Catalog catalog_;
  std::unique_ptr<Backend> backends_[kSourceCount];
  SourceStatus status_[kSourceCount];
  // What each part contributed; rebuild() merges them into catalog_.
  std::vector<App> catalog_apps_;
  std::vector<App> installed_[kSourceCount];
  std::vector<App> store_apps_;  // snap search and category results
  std::map<std::string, Origin> details_;  // Origin::key() -> fetched details
  std::set<std::string> details_asked_;
  std::set<std::string> lookups_asked_;
  std::map<std::string, std::string> owners_;  // desktop id -> package
  bool owners_asked_ = false;
  bool catalog_loaded_ = false;
  bool installed_loaded_[kSourceCount] = {false, false, false};
  std::string catalog_error_;
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
