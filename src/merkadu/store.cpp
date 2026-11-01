// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "store.h"

#include <FL/Fl.H>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "desktop_entry.h"
#include "i18n.h"
#include "icons.h"
#include "json.h"
#include "process.h"
#include "spawn.h"
#include "util.h"
#include "watchdog.h"

namespace sde {
namespace merkadu {

// ---------------------------------------------------------------- post

namespace {

// Results of the worker threads for the FLTK thread. Never destroyed:
// workers may still post while the program ends.
struct Posted {
  std::function<void()> fn;
  const char *what = nullptr;
};
struct PostQueue {
  std::mutex mutex;
  std::deque<Posted> items;
  bool scheduled = false;  // a wake-up or a continuation is pending
  std::chrono::steady_clock::time_point since;
};

PostQueue &post_queue() {
  static PostQueue *q = new PostQueue;
  return *q;
}

void drain_posted(void *) {
  PostQueue &q = post_queue();
  auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    Posted p;
    {
      std::lock_guard<std::mutex> lock(q.mutex);
      if (q.items.empty()) {
        q.scheduled = false;
        return;
      }
      p = std::move(q.items.front());
      q.items.pop_front();
    }
    {
      watchdog::Busy busy(p.what ? p.what : "handling the result of a worker thread");
      p.fn();
    }
    // A burst of results (a refresh brings many) must not keep the window
    // from drawing: the rest comes after the next redraw.
    if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(20)) {
      Fl::add_timeout(0.0, drain_posted);
      return;
    }
  }
}

// FLTK's ring of wake-ups can be full (Fl::awake() fails): a slow timer
// picks up what is waiting then.
void posted_safety(void *) {
  PostQueue &q = post_queue();
  bool stuck;
  {
    std::lock_guard<std::mutex> lock(q.mutex);
    stuck = !q.items.empty() && std::chrono::steady_clock::now() - q.since > std::chrono::milliseconds(500);
    if (stuck) q.scheduled = true;
  }
  if (stuck) drain_posted(nullptr);
  Fl::repeat_timeout(0.5, posted_safety);
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

void post(std::function<void()> fn, const char *what) {
  PostQueue &q = post_queue();
  bool wake;
  {
    std::lock_guard<std::mutex> lock(q.mutex);
    wake = !q.scheduled;
    q.scheduled = true;
    if (q.items.empty()) q.since = std::chrono::steady_clock::now();
    q.items.push_back(Posted{std::move(fn), what});
  }
  // One wake-up per batch: FLTK's awake queue is small.
  if (wake) Fl::awake(drain_posted, nullptr);
}

// --------------------------------------------------------------- labels

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
  Fl::add_timeout(0.5, posted_safety);
  settings_ = load_settings();
  categories_.load();
  for (int i = 0; i < kSourceCount; ++i) {
    Source s = Source(i);
    // MERKADU_FAKE_SNAPD: the real Snap source, talking to a test's fake
    // snapd (MERKADU_SNAPD_SOCKET), with the other sources fake.
    if (fake_mode() && !(s == Source::Snap && !env("MERKADU_FAKE_SNAPD").empty()))
      backends_[i] = make_fake_backend(s, fake_dir());
    else if (s == Source::Debian)
      backends_[i] = make_packagekit_backend();
    else if (s == Source::Flatpak)
      backends_[i] = make_flatpak_backend();
    else
      backends_[i] = make_snap_backend();
  }
  probe_sources();
  if (catalog) {
    load_popular(false);  // what was kept (downloaded once Flathub is known)
    reload_catalog();
  } else {
    catalog_loaded_ = true;
  }
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
    catalog_apps_ = std::make_shared<const std::vector<App>>(std::move(data.apps));
    catalog_error_ = data.error;
    catalog_sources_ = data.sources;
    owners_asked_ = false;
    rebuild();
    resolve_owners();
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
      bool was = status_[i].available;
      status_[i] = st;
      probed_[i] = true;
      emit(Event::Sources);
      // snapd answers (at start, or after Enable Snap Support): what the
      // store has, and the installed snaps.
      if (Source(i) == Source::Snap && st.available && !was) {
        reload_installed(Source::Snap);
        load_snap_store();
      }
      // Flathub is set up: its popular apps for the highlights.
      if (Source(i) == Source::Flatpak && st.has_flathub) load_popular(false);
    });
  }
}

void Store::add_store_apps(std::vector<App> apps, bool replace) {
  bool added = false;
  for (auto &a : apps) {
    const Origin *o = a.origin(Source::Snap);
    if (!o) continue;
    if (!store_snaps_.insert(o->id).second) {
      // Known already: a lookup's answer has the catalog app's id.
      if (!replace) continue;
      std::string name = o->id;
      store_apps_.erase(std::remove_if(store_apps_.begin(), store_apps_.end(),
                                       [&name](const App &x) {
                                         const Origin *xo = x.origin(Source::Snap);
                                         return xo && xo->id == name;
                                       }),
                        store_apps_.end());
    }
    store_apps_.push_back(std::move(a));
    added = true;
  }
  if (added) rebuild();
}

void Store::load_snap_store() {
  Backend *b = backend(Source::Snap);
  if (!b || !status_[int(Source::Snap)].available || snap_store_loading_) return;
  snap_store_loading_ = true;
  browsed_.clear();
  b->featured([this](std::vector<App> apps, std::string error) {
    if (!error.empty()) watchdog::log("Snap Store: " + error);
    featured_snaps_.clear();
    for (auto &a : apps) featured_snaps_.push_back(a.id);
    add_store_apps(std::move(apps));
    emit(Event::Search);
    // The snaps of every department, one after the other (each is a
    // request snapd sends on to the store).
    auto cats = std::make_shared<std::vector<std::string>>();
    for (const Category *c : categories_.sorted())
      for (auto &cat : snap_categories(c->id))
        if (browsed_.insert(cat).second) cats->push_back(cat);
    browse_next(cats, 0);
  });
}

void Store::browse_next(std::shared_ptr<std::vector<std::string>> cats, size_t i) {
  Backend *b = backend(Source::Snap);
  if (!b || i >= cats->size()) {
    snap_store_loading_ = false;
    return;
  }
  b->browse((*cats)[i], [this, cats, i](std::vector<App> apps, std::string) {
    if (!apps.empty()) {
      add_store_apps(std::move(apps));
      emit(Event::Search);
    }
    browse_next(cats, i + 1);
  });
}

namespace {

std::string popular_cache() {
  std::string base = env("XDG_CACHE_HOME");
  if (base.empty()) base = join_path(home_dir(), ".cache");
  return join_path(base, "merkadu/flathub-collections.json");
}

}  // namespace

void Store::load_popular(bool force) {
  if (popular_loading_) return;
  std::string cached;
  struct stat st;
  std::string file = popular_cache();
  if (popular_.empty() && read_file(file, cached)) popular_ = parse_flathub_collection(cached);
  if (fake_source(Source::Flatpak)) {
    // The test data's collection (no network).
    std::string text;
    if (read_file(join_path(fake_dir(), "flathub-popular.json"), text)) popular_ = parse_flathub_collection(text);
    return;
  }
  bool fresh = stat(file.c_str(), &st) == 0 && std::time(nullptr) - st.st_mtime < 24 * 3600;
  if ((!force && fresh) || !status_[int(Source::Flatpak)].has_flathub || settings_.popular_urls.empty()) return;
  popular_loading_ = true;
  // One collection after the other, with curl (no network: the cached
  // ones stay).
  auto urls = std::make_shared<std::vector<std::string>>(settings_.popular_urls);
  auto found = std::make_shared<std::vector<std::string>>();
  auto step = std::make_shared<std::function<void(size_t)>>();
  std::weak_ptr<std::function<void(size_t)>> weak = step;  // kept alive by the download that runs
  *step = [this, urls, found, weak, file](size_t i) {
    auto step = weak.lock();
    if (!step) return;
    if (i >= urls->size()) {
      popular_loading_ = false;
      // A Flathub's answers without any id are no news (offline, an API
      // that changed): the cached collections stay.
      if (found->empty()) return;
      popular_ = *found;
      std::string json = "[";
      for (size_t k = 0; k < found->size(); ++k) json += (k ? "," : "") + json_quote((*found)[k]);
      make_dirs(file.substr(0, file.rfind('/')));
      write_file_atomic(file, json + "]\n");
      emit(Event::Catalog);
      return;
    }
    bool started = run_async({"curl", "-fsSL", "--max-time", "20", "--connect-timeout", "8", "--proto", "=https",
                              (*urls)[i]},
                             [found, step, i](const std::string &out, int status) {
                               if (status == 0)
                                 for (auto &id : parse_flathub_collection(out))
                                   if (std::find(found->begin(), found->end(), id) == found->end())
                                     found->push_back(id);
                               (*step)(i + 1);
                             });
    if (!started) (*step)(urls->size());
  };
  (*step)(0);
}

std::vector<Highlight> Store::highlights(size_t count) const {
  HighlightSources src;
  src.distribution = settings_.featured;
  src.fresh = fresh_;
  src.popular = popular_;
  src.featured_snaps = featured_snaps_;
  // The tests and screenshots see the same highlights every day
  // (MERKADU_FAKE_DAY picks another).
  long day = fake_mode() ? std::atol(env("MERKADU_FAKE_DAY").c_str()) : local_day(std::time(nullptr));
  return choose_highlights(catalog_, src, count, day);
}

void Store::watch_snapd(void *) {
  Store &st = get();
  if (st.status_[int(Source::Snap)].available || std::time(nullptr) > st.snapd_watch_until_) return;
  st.probe_sources();
  Fl::repeat_timeout(3.0, watch_snapd);
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
    if (!error.empty()) watchdog::log(std::string(source_id(s)) + ": " + error);
    installed_[int(s)] = std::make_shared<const std::vector<App>>(std::move(apps));
    installed_loaded_[int(s)] = true;
    rebuild();
  });
}

void Store::reload_all() {
  for (int i = 0; i < kSourceCount; ++i) reload_installed(Source(i));
}

void Store::rebuild() {
  // Several changes in one turn of the event loop make one merge.
  if (rebuild_scheduled_) return;
  rebuild_scheduled_ = true;
  Fl::add_timeout(0.0, run_rebuild);
}

void Store::run_rebuild(void *) {
  Store &st = get();
  st.rebuild_scheduled_ = false;
  if (st.rebuild_running_) {
    st.rebuild_again_ = true;  // merged again when the current one is done
    return;
  }
  // Nothing to show before the catalog is read (the uninstall dialog has
  // no catalog: catalog_loaded_ is set and catalog_apps_ stays empty).
  if (!st.catalog_apps_ && !st.catalog_loaded_) return;
  st.rebuild_running_ = true;
  auto in = std::make_shared<CatalogInputs>();
  in->catalog = st.catalog_apps_;
  for (int s = 0; s < kSourceCount; ++s) in->installed[s] = st.installed_[s];
  in->store = st.store_apps_;
  in->details = st.details_;
  in->owners = st.owners_;
  in->packages = st.package_list_;
  in->order = st.settings_.order;
  in->categories = &st.categories_;  // read-only after start()
  in->now = std::time(nullptr);
  in->fresh_count = size_t(std::max(12, st.settings_.new_count * 2));
  watchdog::Busy busy("starting the merge of the catalog");
  std::thread([in]() {
    auto result = std::make_shared<CatalogResult>(build_catalog(*in));
    post([result]() { get().adopt(result); }, "the merged catalog arrived");
  }).detach();
}

void Store::adopt(std::shared_ptr<CatalogResult> result) {
  watchdog::Busy busy("taking over the merged catalog", long(result->catalog.size()));
  // The old catalog is freed on a worker: thousands of apps take a while.
  auto old = std::make_shared<Catalog>(std::move(catalog_));
  catalog_ = std::move(result->catalog);
  departments_ = std::move(result->departments);
  fresh_ = std::move(result->fresh);
  flatpak_rows_ = std::move(result->flatpak_rows);
  snap_rows_ = std::move(result->snap_rows);
  dropped_debian_ = result->dropped_debian;
  for (int s = 0; s < kSourceCount; ++s) catalog_counts_[s] = result->counts[s];
  std::thread([old = std::move(old)]() mutable { old.reset(); }).detach();
  rebuild_running_ = false;
  if (catalog_apps_) catalog_loaded_ = true;
  ++generation_;
  emit(Event::Catalog);
  if (rebuild_again_) {
    rebuild_again_ = false;
    rebuild();
  }
}

std::vector<const App *> Store::apps_in(const Category &c) const {
  std::vector<const App *> out;
  if (!departments_.built()) return categories_.apps_in(c, catalog_);
  for (size_t i : departments_.apps(c.id))
    if (i < catalog_.apps().size()) out.push_back(&catalog_.apps()[i]);
  return out;
}

size_t Store::count_in(const Category &c) const {
  return departments_.built() ? departments_.apps(c.id).size() : categories_.apps_in(c, catalog_).size();
}

const Category *Store::department_of(const App &a) const {
  const auto &apps = catalog_.apps();
  if (departments_.built() && !apps.empty() && &a >= &apps.front() && &a <= &apps.back())
    return departments_.department(size_t(&a - &apps.front()));
  return categories_.category_of(a);
}

std::vector<const App *> Store::fresh_apps(size_t max) const {
  std::vector<const App *> out;
  for (auto &id : fresh_) {
    if (out.size() >= max) break;
    const App *a = catalog_.find(id);
    if (a && !a->installed()) out.push_back(a);
  }
  return out;
}

void Store::resolve_owners() {
  if (owners_asked_ || !backend(Source::Debian) || !catalog_apps_) return;
  owners_asked_ = true;
  // Launchers of installed software whose metainfo names no package: the
  // package that owns the launcher, if one does (dpkg -S).
  std::vector<std::string> paths;
  for (auto &a : *catalog_apps_) {
    if (!a.needs_owner) continue;
    for (auto &d : a.desktop_ids) {
      std::string p = "/usr/share/applications/" + d;
      if (ends_with(d, ".desktop") && d.find('/') == std::string::npos && file_exists(p)) paths.push_back(p);
    }
  }
  if (paths.empty()) return;
  backend(Source::Debian)->owners(paths, [this](std::map<std::string, std::vector<std::string>> found) {
    for (auto &f : found)
      if (!f.second.empty()) owners_[basename_of(f.first)] = f.second.front();
    if (!found.empty()) rebuild();
  });
}

// ------------------------------------------------------------------ jobs

const Job *Store::job(int id) const { return queue_.find(id); }

const Job *Store::active_job(const std::string &app_id) const {
  for (auto &j : queue_.jobs())
    if (j.active() && !j.app_id.empty() && j.app_id == app_id) return &j;
  return nullptr;
}

int Store::active_jobs() const { return queue_.active(); }

void Store::clear_finished() {
  queue_.clear_finished();
  emit(Event::Jobs);
}

int Store::enqueue(Job job) {
  if (job.targets.empty() || !backend(job.source)) return 0;
  int id = queue_.add(job);
  emit(Event::Jobs);
  run_next();
  return id;
}

int Store::install(const std::string &app_id, Source source) {
  const App *a = app(app_id);
  Job j;
  if (!a || active_job(app_id) || !make_install_job(*a, source, j)) return 0;
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
  const Job *before = queue_.find(job_id);
  if (!before) return;
  Source source = before->source;
  if (queue_.cancel(job_id, std::time(nullptr))) {
    // Running: the source stops it and reports it cancelled.
    if (backend(source)) backend(source)->cancel(job_id);
    emit(Event::Progress);
    return;
  }
  const Job *j = queue_.find(job_id);
  if (j && j->state == Job::Cancelled) {
    Job copy = *j;
    emit(Event::Jobs);
    if (on_job_done) on_job_done(copy);
    run_next();  // what waited for it is cancelled too
  }
}

void Store::pause(int job_id) {
  const Job *j = queue_.find(job_id);
  if (!j) return;
  Source source = j->source;
  if (queue_.pause(job_id)) {
    // Stopped by the source; job_finished() keeps it in the queue, paused.
    if (backend(source)) backend(source)->cancel(job_id);
    emit(Event::Progress);
    return;
  }
  emit(Event::Jobs);
}

void Store::resume(int job_id) {
  if (!queue_.resume(job_id)) return;
  emit(Event::Jobs);
  run_next();
}

void Store::run_next() {
  std::vector<int> cancelled;
  Job *next = queue_.next(std::time(nullptr), &cancelled);
  for (int id : cancelled)
    if (const Job *c = queue_.find(id))
      if (on_job_done) on_job_done(*c);
  if (!next) {
    emit(Event::Jobs);
    return;
  }
  int id = next->id;
  queue_.start(id);
  Job copy = *queue_.find(id);
  emit(Event::Jobs);
  backend(copy.source)->run(
      copy,
      [this, id](const Progress &p) {
        queue_.progress(id, p);
        emit(Event::Progress);
      },
      [this, id](bool ok, bool cancelled, std::string error) { job_finished(id, ok, cancelled, error); });
}

void Store::job_finished(int id, bool ok, bool cancelled, const std::string &error) {
  queue_.finished(id, ok, cancelled, error, std::time(nullptr));
  const Job *j = queue_.find(id);
  if (j && j->state == Job::Paused) {
    // Paused: it waits in the queue; the next change may run meanwhile.
    emit(Event::Jobs);
    run_next();
    return;
  }
  if (j) {
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
    if (s == Source::Flatpak && runtimes_loaded_) load_runtimes();
    // The snapd package (Enable Snap Support, or installed in All
    // Software): snapd starts by itself in a moment; it is asked until it
    // answers, without restarting Merkadu.
    if (s == Source::Debian && copy.state == Job::Done && copy.action == Action::Install &&
        std::find(copy.targets.begin(), copy.targets.end(), "snapd") != copy.targets.end() &&
        !status_[int(Source::Snap)].available) {
      snapd_watch_until_ = std::time(nullptr) + 180;
      Fl::remove_timeout(watch_snapd);
      Fl::add_timeout(3.0, watch_snapd);
    }
    // The snapd snap (Enable Snap Support) makes the source available.
    if (s == Source::Snap) probe_sources();
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
    add_store_apps(std::move(apps));
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
      add_store_apps(std::move(apps));
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
      if (!apps.empty()) add_store_apps(std::move(apps), true);
    });
  }
  for (auto &o : a->origins) {
    Backend *b = backend(o.source);
    if (!b || !details_asked_.insert(o.key()).second) continue;
    App copy = *a;
    b->details(copy, o, [this](Origin found) {
      details_[found.key()] = found;
      rebuild();
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
          // The Snap Store's featured snaps and departments, again; with
          // Refresh, Flathub's popular apps too.
          load_snap_store();
          if (refresh_.user) load_popular(true);
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
    auto *list = new std::shared_ptr<PackageList>(std::make_shared<PackageList>());
    apt::Snapshot snap = fake_package_snapshot();
    (*list)->packages = std::move(snap.packages);
    (*list)->error = snap.error;
    (*list)->prepare();
    Fl::add_timeout(0.2, [](void *data) {
      std::unique_ptr<std::shared_ptr<PackageList>> l(static_cast<std::shared_ptr<PackageList> *>(data));
      Store::get().apply_packages(*l);
    }, list);
    return;
  }
  // Read, indexed and turned into installed apps on the APT thread: the
  // window only swaps them in.
  apt::queue([this] {
    apt::Snapshot snap = apt::snapshot();
    auto list = std::make_shared<PackageList>();
    list->packages = std::move(snap.packages);
    list->error = snap.error;
    list->prepare();
    post([this, list] { apply_packages(list); }, "the package lists arrived");
  });
}

void Store::apply_packages(std::shared_ptr<PackageList> list) {
  watchdog::Busy busy("taking over the package lists", long(list->packages.size()));
  packages_loading_ = false;
  if (!list->error.empty()) watchdog::log("package lists: " + list->error);
  packages_error_ = list->error;
  packages_loaded_ = true;
  // The lists replaced are freed on a worker (90,000 packages take a while).
  std::shared_ptr<const PackageList> old = std::move(package_list_);
  std::shared_ptr<const std::vector<App>> old_installed = std::move(installed_[int(Source::Debian)]);
  if (old || old_installed)
    std::thread([old = std::move(old), old_installed = std::move(old_installed)]() mutable {
      old_installed.reset();
      old.reset();
    }).detach();
  package_list_ = list;
  ++packages_generation_;
  // Marks of packages that are gone, or that are done now.
  for (auto &kv : std::map<std::string, Mark>(marks_.all())) {
    const Package *p = package(kv.first);
    if (!p || (kv.second == Mark::Install && p->installed) || (kv.second == Mark::Remove && !p->installed) ||
        (kv.second == Mark::Upgrade && !p->upgradable))
      marks_.mark(kv.first, Mark::Unmarked);
  }
  if (list->packages.empty() && !packages_error_.empty()) {
    // Without APT's view, PackageKit says what is installed.
    if (Backend *b = backend(Source::Debian))
      b->load_installed([this](std::vector<App> apps, std::string error) {
        installed_[int(Source::Debian)] = std::make_shared<const std::vector<App>>(std::move(apps));
        installed_loaded_[int(Source::Debian)] = true;
        rebuild();
      });
  } else {
    installed_[int(Source::Debian)] = std::shared_ptr<const std::vector<App>>(list, &list->installed);
    installed_loaded_[int(Source::Debian)] = true;
  }
  rebuild();
  emit(Event::Packages);
  if (packages_again_) {
    packages_again_ = false;
    load_packages();
  }
}

const std::vector<Package> &Store::packages() const {
  static const std::vector<Package> *none = new std::vector<Package>();
  return package_list_ ? package_list_->packages : *none;
}

const std::vector<Package> &Store::rows(Source source, bool runtimes) const {
  if (source == Source::Debian) return packages();
  if (source == Source::Snap) return snap_rows_;
  if (!runtimes || runtime_rows_.empty()) return flatpak_rows_;
  int gen = rows_generation(Source::Flatpak);
  if (flatpak_all_generation_ != gen) {
    flatpak_all_ = flatpak_rows_;
    flatpak_all_.insert(flatpak_all_.end(), runtime_rows_.begin(), runtime_rows_.end());
    flatpak_all_generation_ = gen;
  }
  return flatpak_all_;
}

const Package *Store::row(Source source, const std::string &name) const {
  if (source == Source::Debian) return package(name);
  for (auto &p : rows(source, true))
    if (p.name == name) return &p;
  return nullptr;
}

int Store::rows_generation(Source source) const {
  if (source == Source::Debian) return packages_generation_;
  return generation_ * 4096 + (source == Source::Flatpak ? runtimes_generation_ % 4096 : 0);
}

void Store::load_runtimes() {
  Backend *b = backend(Source::Flatpak);
  if (!b || runtimes_loading_) return;
  runtimes_loading_ = true;
  b->list_runtimes([this](std::vector<Package> rows) {
    runtimes_loading_ = false;
    runtimes_loaded_ = true;
    runtime_rows_ = std::move(rows);
    ++runtimes_generation_;
    emit(Event::Packages);
  });
}

const Package *Store::package(const std::string &name) const {
  return package_list_ ? package_list_->find(name) : nullptr;
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
  // Asked by every app page and package pane: worked out once per list.
  if (system_generation_ == packages_generation_) return system_cache_;
  std::set<std::string> out;
  for (auto &p : packages())
    if (p.system() && p.installed) out.insert(p.name);
  struct utsname u;
  if (uname(&u) == 0) {
    std::string kernel = running_kernel_package(u.release);
    if (!kernel.empty()) out.insert(kernel);
  }
  system_cache_ = out;
  system_generation_ = packages_generation_;
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
