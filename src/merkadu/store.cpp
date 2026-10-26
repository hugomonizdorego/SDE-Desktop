// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "store.h"

#include <FL/Fl.H>
#include <sys/utsname.h>

#include <algorithm>
#include <deque>
#include <mutex>

#include "desktop_entry.h"
#include "i18n.h"
#include "icons.h"
#include "spawn.h"
#include "util.h"

namespace sde {
namespace merkadu {

// ---------------------------------------------------------------- post

namespace {

std::mutex g_post_mutex;
std::deque<std::function<void()>> g_posted;

void drain_posted(void *) {
  for (;;) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(g_post_mutex);
      if (g_posted.empty()) return;
      fn = std::move(g_posted.front());
      g_posted.pop_front();
    }
    fn();
  }
}

// A one-line summary of what the catalog was made of, for the session log:
// the first thing to look at when a window shows fewer apps than expected.
void log_catalog(const CatalogData &d) {
  const CatalogSources &s = d.sources;
  std::string remotes;
  for (auto &r : s.flatpak_remotes) remotes += (remotes.empty() ? "" : ",") + r;
  fprintf(stderr,
          "merkadu: catalog: %zu apps (%d Debian, %d Flatpak) in %.1f s; APT catalog download %s, DEP-11 %d "
          "(%d read by AppStream, %zu linked by Merkadu), icon archives %d; Flatpak AppStream: %s%s%s\n",
          d.apps.size(), d.debian, d.flatpak, d.seconds, s.appstream_apt ? "on" : "off (appstream not installed)",
          s.dep11, s.dep11_linked, s.unlinked.size(), s.dep11_icons, remotes.empty() ? "none" : remotes.c_str(),
          d.error.empty() ? "" : "; error: ", d.error.c_str());
}

}  // namespace

void post(std::function<void()> fn) {
  bool wake;
  {
    std::lock_guard<std::mutex> lock(g_post_mutex);
    wake = g_posted.empty();
    g_posted.push_back(std::move(fn));
  }
  // One wake-up per batch: FLTK's awake queue is small.
  if (wake) Fl::awake(drain_posted, nullptr);
}

// --------------------------------------------------------------- labels

std::string action_progress_label(Action a) {
  switch (a) {
    case Action::Remove: return _("Removing");
    case Action::Update: return _("Updating");
    default: return _("Installing");
  }
}

std::string action_label(Action a) {
  switch (a) {
    case Action::Remove: return _("Remove");
    case Action::Update: return _("Update");
    default: return _("Install");
  }
}

std::string origin_line(const Origin &o) {
  switch (o.source) {
    case Source::Flatpak: {
      std::string id = o.flatpak_app_id();
      std::string line = o.remote.empty() ? format(_("Flatpak %s"), id.c_str())
                                          : format(_("Flatpak %s from %s"), id.c_str(), o.remote.c_str());
      if (o.installation == "user") line += " " + std::string(_("(your own installation)"));
      return line;
    }
    case Source::Snap:
      return o.remote.empty() ? format(_("Snap %s"), o.id.c_str())
                              : format(_("Snap %s (%s)"), o.id.c_str(), o.remote.c_str());
    default:
      return format(_("%s package %s"), source_label(Source::Debian).c_str(), o.id.c_str());
  }
}

// ---------------------------------------------------------------- store

Store &Store::get() {
  static Store store;
  return store;
}

void Store::start(bool catalog) {
  if (started_) return;
  started_ = true;
  settings_ = load_settings();
  categories_.load();
  for (int i = 0; i < kSourceCount; ++i) {
    Source s = Source(i);
    if (fake_mode())
      backends_[i] = make_fake_backend(s, fake_dir());
    else if (s == Source::Debian)
      backends_[i] = make_packagekit_backend();
    else if (s == Source::Flatpak)
      backends_[i] = make_flatpak_backend();
    else
      backends_[i] = make_snap_backend();
  }
  probe_sources();
  if (catalog)
    reload_catalog();
  else
    catalog_loaded_ = true;
  reload_all();
  // The lists of the repositories, Flathub and the Snap Store are
  // refreshed in the background while the cached ones are shown.
  if (catalog && settings_.refresh_on_start) refresh(false);
}

void Store::reload_catalog() {
  if (catalog_loading_) {
    catalog_again_ = true;  // a refresh changed the data meanwhile
    return;
  }
  catalog_loading_ = true;
  emit(Event::Catalog);
  auto done = [this](CatalogData data) {
    catalog_loading_ = false;
    if (!fake_mode()) log_catalog(data);
    catalog_apps_ = std::move(data.apps);
    catalog_error_ = data.error;
    catalog_sources_ = data.sources;
    catalog_loaded_ = true;
    rebuild();
    resolve_owners();
    emit(Event::Catalog);
    if (catalog_again_) {
      catalog_again_ = false;
      reload_catalog();
    }
  };
  if (fake_mode())
    load_fake_catalog(fake_dir(), done);
  else
    load_catalog(done);
}

void Store::probe_sources() {
  for (int i = 0; i < kSourceCount; ++i) {
    if (!backends_[i]) continue;
    backends_[i]->probe([this, i](SourceStatus st) {
      status_[i] = st;
      probed_[i] = true;
      emit(Event::Sources);
    });
  }
}

bool Store::all_installed_loaded() const {
  for (int i = 0; i < kSourceCount; ++i)
    if (!installed_loaded_[i]) return false;
  return true;
}

int Store::listen(std::function<void(Event)> fn) {
  int id = next_listener_++;
  listeners_[id] = std::move(fn);
  return id;
}

void Store::unlisten(int id) { listeners_.erase(id); }

void Store::emit(Event e) {
  // Listeners may (un)register others while they run.
  std::vector<std::function<void(Event)>> copy;
  for (auto &l : listeners_) copy.push_back(l.second);
  for (auto &fn : copy) fn(e);
}

void Store::reload_installed(Source s) {
  // What APT knows covers the installed packages and their updates, also
  // without PackageKit.
  if (s == Source::Debian && packages_available()) {
    load_packages();
    return;
  }
  Backend *b = backend(s);
  if (!b) return;
  b->load_installed([this, s](std::vector<App> apps, std::string error) {
    if (!error.empty()) fprintf(stderr, "merkadu: %s: %s\n", source_id(s), error.c_str());
    installed_[int(s)] = std::move(apps);
    installed_loaded_[int(s)] = true;
    rebuild();
    emit(Event::Catalog);
  });
}

void Store::reload_all() {
  for (int i = 0; i < kSourceCount; ++i) reload_installed(Source(i));
}

void Store::rebuild() {
  catalog_.clear();
  for (App a : catalog_apps_) {
    // Packages the catalog names no package for (metainfo of installed
    // software): the package that owns their desktop file.
    for (auto &o : a.origins)
      if (o.source == Source::Debian && o.id.empty())
        for (auto &d : a.desktop_ids) {
          auto it = owners_.find(d);
          if (it == owners_.end()) continue;
          o.id = it->second;
          if (std::find(a.pkgnames.begin(), a.pkgnames.end(), o.id) == a.pkgnames.end())
            a.pkgnames.push_back(o.id);
          break;
        }
    catalog_.add(std::move(a));
  }
  for (auto &a : store_apps_) catalog_.add(a);
  for (int s = 0; s < kSourceCount; ++s) {
    for (auto &a : installed_[s]) {
      if (s == int(Source::Debian) && starts_with(a.id, "deb:") && !a.origins.empty()) {
        // An installed package: it marks the app it belongs to, if any.
        App *target = catalog_.find_origin(Source::Debian, a.origins.front().id);
        if (!target) continue;
        App mark;
        mark.id = target->id;
        mark.origins = a.origins;
        catalog_.add(std::move(mark));
      } else {
        catalog_.add(a);
      }
    }
  }
  // Details fetched for the details page (sizes, versions).
  if (!details_.empty())
    for (auto &a : catalog_.apps())
      for (auto &o : a.origins) {
        auto it = details_.find(o.key());
        if (it != details_.end()) merge_origin(a, it->second);
      }
  // The package lists know the version, sizes and section of every
  // Debian app.
  if (!package_index_.empty())
    for (auto &a : catalog_.apps())
      for (auto &o : a.origins) {
        if (o.source != Source::Debian) continue;
        auto it = package_index_.find(o.id);
        if (it == package_index_.end()) continue;
        const Package &p = packages_[it->second];
        if (o.version.empty()) o.version = p.candidate_version;
        if (o.download_size < 0) o.download_size = p.download_size;
        if (o.installed_size < 0) o.installed_size = o.installed ? p.current_size : p.installed_size;
        if (a.section.empty()) a.section = p.section;
      }
  // Origins without anything to install (metainfo without a package).
  for (auto &a : catalog_.apps())
    a.origins.erase(std::remove_if(a.origins.begin(), a.origins.end(),
                                   [](const Origin &o) { return o.id.empty(); }),
                    a.origins.end());
  catalog_.remove_empty();
  catalog_.sort_origins(settings_.order);
  for (int s = 0; s < kSourceCount; ++s) catalog_counts_[s] = 0;
  for (auto &a : catalog_.apps())
    for (int s = 0; s < kSourceCount; ++s)
      if (a.has_source(Source(s))) ++catalog_counts_[s];
  ++generation_;
}

void Store::resolve_owners() {
  if (owners_asked_ || !backend(Source::Debian)) return;
  owners_asked_ = true;
  std::vector<std::string> paths;
  for (auto &a : catalog_apps_)
    for (auto &o : a.origins)
      if (o.source == Source::Debian && o.id.empty())
        for (auto &d : a.desktop_ids)
          if (ends_with(d, ".desktop") && d.find('/') == std::string::npos)
            paths.push_back("/usr/share/applications/" + d);
  if (paths.empty()) return;
  backend(Source::Debian)->owners(paths, [this](std::map<std::string, std::vector<std::string>> found) {
    for (auto &f : found)
      if (!f.second.empty()) owners_[basename_of(f.first)] = f.second.front();
    if (found.empty()) return;
    rebuild();
    emit(Event::Catalog);
  });
}

// ------------------------------------------------------------------ jobs

Job *Store::find_job(int id) {
  for (auto &j : jobs_)
    if (j.id == id) return &j;
  return nullptr;
}

const Job *Store::job(int id) const { return const_cast<Store *>(this)->find_job(id); }

const Job *Store::active_job(const std::string &app_id) const {
  for (auto &j : jobs_)
    if (j.active() && !j.app_id.empty() && j.app_id == app_id) return &j;
  return nullptr;
}

int Store::active_jobs() const {
  int n = 0;
  for (auto &j : jobs_) n += j.active();
  return n;
}

void Store::clear_finished() {
  jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return !j.active(); }), jobs_.end());
  emit(Event::Jobs);
}

int Store::enqueue(Job job) {
  if (job.targets.empty() || !backend(job.source)) return 0;
  job.id = next_job_++;
  job.state = Job::Queued;
  jobs_.push_back(job);
  emit(Event::Jobs);
  run_next();
  return job.id;
}

int Store::install(const std::string &app_id, Source source) {
  const App *a = app(app_id);
  const Origin *o = a ? a->origin(source) : nullptr;
  if (!o || o->installed || active_job(app_id)) return 0;
  Job j;
  j.action = Action::Install;
  j.source = source;
  j.app_id = a->id;
  j.title = a->name;
  j.icon = a->icon;
  j.remote = o->remote;
  if (source == Source::Debian) {
    if (!valid_package_name(o->id)) return 0;
    j.targets = {o->id};
  } else if (source == Source::Flatpak) {
    if (!valid_flatpak_ref(o->id)) return 0;
    j.targets = {std::string(o->installation.empty() ? "system" : o->installation) + ":" + o->id};
  } else {
    if (!valid_snap_name(o->id)) return 0;
    j.targets = {o->id};
    j.classic = o->classic;
  }
  return enqueue(j);
}

int Store::remove(const std::string &app_id, Source source) {
  const App *a = app(app_id);
  if (!a || active_job(app_id)) return 0;
  const Origin *o = nullptr;
  for (auto &x : a->origins)
    if (x.source == source && x.installed) o = &x;
  if (!o) return 0;
  if (source == Source::Debian && (settings_.protected_packages.count(o->id) || system_packages().count(o->id)))
    return 0;
  Job j;
  j.action = Action::Remove;
  j.source = source;
  j.app_id = a->id;
  j.title = a->name;
  j.icon = a->icon;
  if (source == Source::Flatpak)
    j.targets = {std::string(o->installation.empty() ? "system" : o->installation) + ":" + o->id};
  else
    j.targets = {o->id};
  return enqueue(j);
}

int Store::update(const std::string &app_id) {
  const App *a = app(app_id);
  if (!a || active_job(app_id)) return 0;
  for (auto &o : a->origins) {
    if (!o.installed || !o.update) continue;
    Job j;
    j.action = Action::Update;
    j.source = o.source;
    j.app_id = a->id;
    j.title = a->name;
    j.icon = a->icon;
    j.targets = {o.source == Source::Flatpak
                     ? std::string(o.installation.empty() ? "system" : o.installation) + ":" + o.id
                     : o.id};
    return enqueue(j);
  }
  return 0;
}

int Store::update_all() {
  int first = 0;
  // The system's packages (Debian apps too) are updated by Software
  // Updater, not here.
  for (Source s : {Source::Flatpak, Source::Snap}) {
    Job j;
    j.action = Action::Update;
    j.source = s;
    int count = 0;
    for (const App *a : updatable_apps()) {
      if (active_job(a->id)) continue;
      for (auto &o : a->origins)
        if (o.installed && o.update && o.source == s) {
          j.targets.push_back(o.source == Source::Flatpak
                                  ? std::string(o.installation.empty() ? "system" : o.installation) + ":" + o.id
                                  : o.id);
          if (!count++) {
            j.icon = a->icon;
            j.title = a->name;
            j.app_id = a->id;
          }
        }
    }
    if (count > 1) {
      j.title = format(_("%d apps from %s"), count, source_label(s).c_str());
      j.app_id.clear();
    }
    if (count) {
      int id = enqueue(j);
      if (!first) first = id;
    }
  }
  return first;
}

void Store::cancel(int job_id) {
  Job *j = find_job(job_id);
  if (!j) return;
  if (j->state == Job::Queued) {
    j->state = Job::Cancelled;
    j->finished = std::time(nullptr);
    emit(Event::Jobs);
    if (on_job_done) on_job_done(*j);
    return;
  }
  if (j->state == Job::Running && backend(j->source)) backend(j->source)->cancel(job_id);
}

void Store::run_next() {
  if (running_) return;
  Job *next = nullptr;
  for (auto &j : jobs_)
    if (j.state == Job::Queued) {
      // A job that needs another one that did not succeed is cancelled.
      if (j.needs) {
        const Job *dep = job(j.needs);
        if (dep && dep->active()) continue;
        if (!dep || dep->state != Job::Done) {
          j.state = Job::Cancelled;
          j.error = _("The change it needs did not succeed.");
          j.finished = std::time(nullptr);
          if (on_job_done) on_job_done(j);
          continue;
        }
      }
      next = &j;
      break;
    }
  if (!next) {
    emit(Event::Jobs);
    return;
  }
  running_ = true;
  next->state = Job::Running;
  next->status = action_progress_label(next->action);
  int id = next->id;
  Job copy = *next;
  emit(Event::Jobs);
  backend(copy.source)->run(
      copy,
      [this, id](const Progress &p) {
        Job *j = find_job(id);
        if (!j || j->state != Job::Running) return;
        j->percent = p.percent;
        if (!p.status.empty()) j->status = p.status;
        j->cancellable = p.cancellable;
        emit(Event::Progress);
      },
      [this, id](bool ok, bool cancelled, std::string error) { job_finished(id, ok, cancelled, error); });
}

void Store::job_finished(int id, bool ok, bool cancelled, const std::string &error) {
  running_ = false;
  Job *j = find_job(id);
  if (j) {
    j->state = ok ? Job::Done : cancelled ? Job::Cancelled : Job::Failed;
    j->percent = ok ? 100 : j->percent;
    j->error = error;
    j->cancellable = false;
    j->finished = std::time(nullptr);
    Source s = j->source;
    Job copy = *j;
    history_loaded_ = false;
    // New software may bring the icons that were missing.
    IconLoader::forget_missing();
    emit(Event::Jobs);
    if (on_job_done) on_job_done(copy);
    if (s == Source::Debian) {
      // The packages changed: APT's view too. A new flatpak or snapd
      // package changes what the other sources can do.
      if (packages_available() && !fake_mode()) apt::queue([] { apt::reload(); });
      probe_sources();
    }
    reload_installed(s);
    // A snap install may have added AppStream ids of other sources.
    if (s == Source::Snap) store_apps_.clear();
  }
  run_next();
}

// ----------------------------------------------------------- the stores

void Store::search_store(const std::string &query) {
  std::string q = trim(query);
  store_query_ = q;
  Backend *b = backend(Source::Snap);
  if (q.size() < 2 || !b || !status_[int(Source::Snap)].available) {
    store_searching_ = false;
    return;
  }
  store_searching_ = true;
  b->search(q, [this, q](std::vector<App> apps, std::string error) {
    if (q != store_query_) return;  // a newer search is running
    store_searching_ = false;
    for (auto &a : apps) store_apps_.push_back(std::move(a));
    rebuild();
    emit(Event::Search);
  });
}

void Store::browse_store(const std::string &category_id) {
  Backend *b = backend(Source::Snap);
  if (!b || !status_[int(Source::Snap)].available) return;
  for (auto &cat : snap_categories(category_id)) {
    if (!browsed_.insert(cat).second) continue;
    b->browse(cat, [this](std::vector<App> apps, std::string error) {
      if (apps.empty()) return;
      for (auto &a : apps) store_apps_.push_back(std::move(a));
      rebuild();
      emit(Event::Search);
    });
  }
}

void Store::fetch_details(const std::string &app_id) {
  const App *a = app(app_id);
  if (!a) return;
  // Is it in the Snap Store too?
  Backend *snap = backend(Source::Snap);
  if (snap && status_[int(Source::Snap)].available && !a->has_source(Source::Snap) && !a->appstream_id.empty() &&
      lookups_asked_.insert(a->id).second) {
    App copy = *a;
    snap->lookup(copy, [this](std::vector<App> apps, std::string error) {
      if (apps.empty()) return;
      for (auto &x : apps) store_apps_.push_back(std::move(x));
      rebuild();
      emit(Event::Catalog);
    });
  }
  for (auto &o : a->origins) {
    Backend *b = backend(o.source);
    if (!b || !details_asked_.insert(o.key()).second) continue;
    App copy = *a;
    b->details(copy, o, [this](Origin found) {
      details_[found.key()] = found;
      rebuild();
      emit(Event::Catalog);
    });
  }
}

// --------------------------------------------------------------- refresh

void Store::refresh(bool user) {
  if (refresh_.running) return;
  std::time_t succeeded = refresh_.succeeded;
  refresh_ = RefreshState();
  refresh_.succeeded = succeeded;
  refresh_.running = true;
  refresh_.user = user;
  refresh_.status = _("Refreshing the software lists\xE2\x80\xA6");
  refresh_catalog_ = false;
  int min_age = user ? 0 : settings_.refresh_minutes * 60;
  // A catalog APT never downloaded (appstream was installed after the last
  // refresh, or Merkadu's icons were turned on): refreshed whatever the
  // age of the lists.
  bool catalog_missing = false;
  if (!fake_mode()) {
    CatalogSources cs = catalog_sources();
    bool icons_wanted = file_exists("/etc/apt/apt.conf.d/60merkadu");
    catalog_missing = (cs.dep11 == 0 || (icons_wanted && cs.dep11_icons == 0)) &&
                      std::time(nullptr) - cs.lists_time > 120;
  }
  std::vector<Source> sources;
  for (int i = 0; i < kSourceCount; ++i)
    if (backends_[i]) sources.push_back(Source(i));
  refresh_pending_ = int(sources.size());
  emit(Event::Refresh);
  for (Source s : sources) {
    std::string label = source_label(s);
    bool force = user || (s == Source::Debian && catalog_missing);
    backend(s)->refresh(
        force, user, min_age,
        [this, label](const Progress &p) {
          if (!refresh_.running) return;
          refresh_.status = p.status.empty() ? label : format("%s: %s", label.c_str(), p.status.c_str());
          refresh_.percent = p.percent;
          emit(Event::Refresh);
        },
        [this, s, label](RefreshResult r) {
          if (!r.ok)
            refresh_.errors.push_back(format("%s: %s", label.c_str(), r.message.c_str()));
          else if (r.skipped && !r.message.empty())
            refresh_.notes.push_back(format("%s: %s", label.c_str(), r.message.c_str()));
          if (r.changed) {
            // New AppStream data (DEP-11 comes with the package lists).
            refresh_catalog_ = true;
            if (s == Source::Debian && packages_available() && !fake_mode()) apt::queue([] { apt::reload(); });
          }
          if (--refresh_pending_ > 0) {
            emit(Event::Refresh);
            return;
          }
          refresh_.running = false;
          refresh_.finished = std::time(nullptr);
          if (refresh_.errors.empty()) refresh_.succeeded = refresh_.finished;
          refresh_.status.clear();
          refresh_.percent = -1;
          if (refresh_catalog_) reload_catalog();
          reload_all();
          probe_sources();
          emit(Event::Refresh);
        });
  }
}

// ---------------------------------------------------------- All Software

void Store::load_packages() {
  if (!packages_available()) return;
  if (packages_loading_) {
    packages_again_ = true;
    return;
  }
  packages_loading_ = true;
  emit(Event::Packages);
  if (fake_mode()) {
    auto *snap = new apt::Snapshot(fake_package_snapshot());
    Fl::add_timeout(0.2, [](void *data) {
      std::unique_ptr<apt::Snapshot> s(static_cast<apt::Snapshot *>(data));
      Store::get().apply_packages(std::move(*s));
    }, snap);
    return;
  }
  apt::queue([this] {
    auto snap = std::make_shared<apt::Snapshot>(apt::snapshot());
    post([this, snap] { apply_packages(std::move(*snap)); });
  });
}

void Store::apply_packages(apt::Snapshot snap) {
  packages_loading_ = false;
  if (!snap.error.empty()) fprintf(stderr, "merkadu: package lists: %s\n", snap.error.c_str());
  packages_error_ = snap.error;
  packages_loaded_ = true;
  packages_ = std::move(snap.packages);
  package_index_.clear();
  for (size_t i = 0; i < packages_.size(); ++i) package_index_[packages_[i].name] = i;
  ++packages_generation_;
  // Marks of packages that are gone, or that are done now.
  for (auto &kv : std::map<std::string, Mark>(marks_.all())) {
    const Package *p = package(kv.first);
    if (!p || (kv.second == Mark::Install && p->installed) || (kv.second == Mark::Remove && !p->installed) ||
        (kv.second == Mark::Upgrade && !p->upgradable))
      marks_.mark(kv.first, Mark::Unmarked);
  }
  if (packages_.empty() && !packages_error_.empty()) {
    // Without APT's view, PackageKit says what is installed.
    packages_loaded_ = true;
    if (Backend *b = backend(Source::Debian))
      b->load_installed([this](std::vector<App> apps, std::string error) {
        installed_[int(Source::Debian)] = std::move(apps);
        installed_loaded_[int(Source::Debian)] = true;
        rebuild();
        emit(Event::Catalog);
      });
  } else {
    std::vector<App> installed;
    for (auto &p : packages_) {
      if (!p.installed) continue;
      App a;
      a.id = "deb:" + p.name;
      a.name = p.name;
      Origin o;
      o.source = Source::Debian;
      o.id = p.name;
      o.installed = true;
      o.installed_version = p.installed_version;
      o.update = p.upgradable;
      o.version = p.candidate_version;
      o.installed_size = p.current_size;
      a.origins.push_back(o);
      installed.push_back(std::move(a));
    }
    installed_[int(Source::Debian)] = std::move(installed);
    installed_loaded_[int(Source::Debian)] = true;
  }
  rebuild();
  emit(Event::Catalog);
  emit(Event::Packages);
  if (packages_again_) {
    packages_again_ = false;
    load_packages();
  }
}

const Package *Store::package(const std::string &name) const {
  auto it = package_index_.find(name);
  return it == package_index_.end() ? nullptr : &packages_[it->second];
}

const App *Store::app_of_package(const std::string &name) const {
  return const_cast<Catalog &>(catalog_).find_origin(Source::Debian, name);
}

void Store::package_details(const std::string &name, std::function<void(const apt::Details &)> done) {
  if (fake_mode()) {
    auto *d = new std::pair<apt::Details, std::function<void(const apt::Details &)>>(fake_package_details(name), done);
    Fl::add_timeout(0.1, [](void *data) {
      std::unique_ptr<std::pair<apt::Details, std::function<void(const apt::Details &)>>> p(
          static_cast<std::pair<apt::Details, std::function<void(const apt::Details &)>> *>(data));
      p->second(p->first);
    }, d);
    return;
  }
  apt::queue([name, done] {
    auto d = std::make_shared<apt::Details>(apt::details(name));
    post([d, done] { done(*d); });
  });
}

void Store::mark(const std::string &name, Mark m) {
  marks_.mark(name, m);
  emit(Event::Packages);
}

void Store::clear_marks() {
  marks_.clear();
  emit(Event::Packages);
}

void Store::plan_marks(std::function<void(const ChangePlan &)> done) {
  ChangeSet marks = marks_;
  if (fake_mode()) {
    auto *d = new std::pair<ChangePlan, std::function<void(const ChangePlan &)>>(fake_simulate(marks), done);
    Fl::add_timeout(0.2, [](void *data) {
      std::unique_ptr<std::pair<ChangePlan, std::function<void(const ChangePlan &)>>> p(
          static_cast<std::pair<ChangePlan, std::function<void(const ChangePlan &)>> *>(data));
      p->second(p->first);
    }, d);
    return;
  }
  apt::queue([marks, done] {
    auto plan = std::make_shared<ChangePlan>(apt::simulate(marks));
    post([plan, done] { done(*plan); });
  });
}

int Store::apply_marks() {
  int jobs = 0, previous = 0;
  auto add = [&](Action action, Mark mark) {
    std::vector<std::string> names = marks_.names(mark);
    if (names.empty()) return;
    Job j;
    j.action = action;
    j.source = Source::Debian;
    j.targets = names;
    j.icon = "package-x-generic";
    if (names.size() == 1) {
      const App *a = app_of_package(names[0]);
      j.title = a ? a->name : names[0];
      if (a) {
        j.icon = a->icon;
        j.app_id = a->id;
      }
    } else {
      j.title = format(_("%d packages"), int(names.size()));
    }
    // One after the other: the next waits for the one before it.
    j.needs = previous;
    int id = enqueue(j);
    if (id) {
      previous = id;
      ++jobs;
    }
  };
  add(Action::Remove, Mark::Remove);
  add(Action::Install, Mark::Install);
  add(Action::Update, Mark::Upgrade);
  marks_.clear();
  emit(Event::Packages);
  return jobs;
}

std::set<std::string> Store::system_packages() const {
  std::set<std::string> out;
  for (auto &p : packages_)
    if (p.system() && p.installed) out.insert(p.name);
  struct utsname u;
  if (uname(&u) == 0) {
    std::string kernel = running_kernel_package(u.release);
    if (!kernel.empty()) out.insert(kernel);
  }
  return out;
}

// --------------------------------------------------------------- history

void Store::load_history() {
  if (history_loading_) return;
  history_loading_ = true;
  auto pending = std::make_shared<int>(kSourceCount);
  auto all = std::make_shared<std::vector<HistoryEntry>>();
  for (int s = 0; s < kSourceCount; ++s) {
    auto finish = [this, pending, all](std::vector<HistoryEntry> h) {
      for (auto &e : h) all->push_back(std::move(e));
      if (--*pending) return;
      sort_history(*all);
      history_ = std::move(*all);
      history_loaded_ = true;
      history_loading_ = false;
      emit(Event::History);
    };
    if (backend(Source(s)))
      backend(Source(s))->history(finish);
    else
      finish({});
  }
}

std::string Store::item_title(Source source, const std::string &item) const {
  auto &cat = const_cast<Catalog &>(catalog_);
  if (const App *a = cat.find_origin(source, item)) return a->name;
  if (source == Source::Flatpak) {
    Origin o;
    o.id = item;
    return o.flatpak_app_id();
  }
  return item;
}

// ------------------------------------------------------------- queries

std::vector<const App *> Store::installed_apps() const {
  std::vector<const App *> out;
  for (auto &a : catalog_.apps())
    if (a.installed()) out.push_back(&a);
  sort_by_name(out);
  return out;
}

std::vector<const App *> Store::updatable_apps() const {
  std::vector<const App *> out;
  for (auto &a : catalog_.apps())
    if (a.has_update()) out.push_back(&a);
  sort_by_name(out);
  return out;
}

bool Store::launch(const std::string &app_id) {
  const App *a = app(app_id);
  if (!a) return false;
  const Origin *o = a->installed_origin();
  if (!o) return false;
  for (auto &d : a->desktop_ids) {
    DesktopEntry e;
    if (find_application(d, e)) return launch_entry(e);
  }
  if (o->source == Source::Flatpak) return spawn_detached({"flatpak", "run", o->flatpak_app_id()});
  if (o->source == Source::Snap) return spawn_detached({"snap", "run", o->id});
  return false;
}

}  // namespace merkadu
}  // namespace sde
