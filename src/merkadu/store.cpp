// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "store.h"

#include <FL/Fl.H>

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
}

void Store::reload_catalog() {
  auto done = [this](std::vector<App> apps, std::string error) {
    catalog_apps_ = std::move(apps);
    catalog_error_ = error;
    catalog_loaded_ = true;
    rebuild();
    resolve_owners();
    emit(Event::Catalog);
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
  Backend *b = backend(s);
  if (!b) return;
  b->load_installed([this, s](std::vector<App> apps, std::string error) {
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
  // Origins without anything to install (metainfo without a package).
  for (auto &a : catalog_.apps())
    a.origins.erase(std::remove_if(a.origins.begin(), a.origins.end(),
                                   [](const Origin &o) { return o.id.empty(); }),
                    a.origins.end());
  catalog_.remove_empty();
  catalog_.sort_origins(settings_.order);
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
    if (j.active() && j.app_id == app_id) return &j;
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
  if (source == Source::Debian && settings_.protected_packages.count(o->id)) return 0;
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
  for (int s = 0; s < kSourceCount; ++s) {
    Job j;
    j.action = Action::Update;
    j.source = Source(s);
    int count = 0;
    for (const App *a : updatable_apps()) {
      if (active_job(a->id)) continue;
      for (auto &o : a->origins)
        if (o.installed && o.update && o.source == Source(s)) {
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
      j.title = format(_("%d apps from %s"), count, source_label(Source(s)).c_str());
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
      next = &j;
      break;
    }
  if (!next) return;
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
