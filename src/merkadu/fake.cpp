// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The fake sources of MERKADU_FAKE=DIR, for the UI checks and screenshots:
// no network, no root, no package system. DIR/catalog.json describes the
// apps (with their Debian, Flatpak and Snap origins, sizes and installed
// state), the history and the state of the sources; changes run on a timer
// and change the fake state. MERKADU_FAKE_FAIL=TARGET makes the changes of
// a package, ref or snap fail; MERKADU_FAKE_SECONDS sets their duration.
//
// catalog.json:
//   {"distribution": "Debian",
//    "sources": {"flatpak": {"available": true, "remotes": ["flathub"]},
//                "snap": {"available": true, "hidden_folder": false}},
//    "apps": [{"id": "org.example.Paintbox", "name": ..., "summary": ...,
//              "description": ..., "icon": "icons/paintbox.svg",
//              "categories": [...], "keywords": [...], "developer": ...,
//              "license": ..., "homepage": ..., "screenshots": [...],
//              "releases": [{"version": "3.2", "days": 3}],
//              "desktop": ["paintbox.desktop"], "store_only": false,
//              "origins": [{"source": "debian", "id": "paintbox",
//                           "version": "3.2-1", "download": 18200000,
//                           "size": 61000000, "installed": true,
//                           "update": false, "remote": "...",
//                           "publisher": ..., "verified": true}]}],
//    "removes": {"paintbox": ["paintbox", "paintbox-data"]},
//    "history": [{"days": 0, "time": "10:15", "source": "debian",
//                 "change": "install", "items": ["paintbox"], "by": "alex"}]}
#include <FL/Fl.H>

#include <algorithm>
#include <map>

#include "backend.h"
#include "i18n.h"
#include "json.h"
#include "util.h"

namespace sde {
namespace merkadu {

bool fake_mode() { return !fake_dir().empty(); }

std::string fake_dir() { return env("MERKADU_FAKE"); }

namespace {

std::string resolve(const std::string &dir, const std::string &path) {
  if (path.empty() || path[0] == '/' || starts_with(path, "https://") || path.find('/') == std::string::npos)
    return path;  // absolute, remote or an icon name
  return join_path(dir, path);
}

struct FakeWorld {
  bool loaded = false;
  std::string dir;
  std::vector<App> apps;
  std::vector<bool> store_only;
  std::map<std::string, std::vector<std::string>> removes;
  std::map<std::string, long long> sizes;  // package -> installed size
  std::vector<HistoryEntry> history;
  SourceStatus status[kSourceCount];
  std::string fail;
  double seconds = 2.5;

  void load(const std::string &d) {
    if (loaded) return;
    loaded = true;
    dir = d;
    fail = env("MERKADU_FAKE_FAIL");
    double s = std::atof(env("MERKADU_FAKE_SECONDS", "2.5").c_str());
    if (s > 0.05 && s < 600) seconds = s;
    std::string text;
    Json root;
    if (!read_file(join_path(dir, "catalog.json"), text) || !parse_json(text, root) || !root.is_object()) {
      fprintf(stderr, "merkadu: cannot read the fake catalog %s/catalog.json\n", dir.c_str());
      return;
    }
    if (!root.text("distribution").empty()) set_distribution_name(root.text("distribution"));
    std::time_t now = std::time(nullptr);
    for (auto &j : root.array("apps")) {
      App a;
      a.appstream_id = j.text("id");
      a.id = starts_with(a.appstream_id, "snap:") ? a.appstream_id : normalize_id(a.appstream_id);
      a.name = j.text("name");
      a.summary = j.text("summary");
      a.description = j.text("description");
      a.icon = resolve(dir, j.text("icon"));
      a.kind = j.text("kind", "desktop-application");
      a.categories = j.strings("categories");
      a.keywords = j.strings("keywords");
      a.developer = j.text("developer");
      a.license = j.text("license");
      a.homepage = j.text("homepage");
      a.bugtracker = j.text("bugtracker");
      a.desktop_ids = j.strings("desktop");
      for (auto &s : j.strings("screenshots")) {
        Screenshot shot;
        shot.image = shot.thumbnail = resolve(dir, s);
        a.screenshots.push_back(shot);
      }
      for (auto &r : j.array("releases")) {
        Release rel;
        rel.version = r.text("version");
        rel.timestamp = now - std::time_t(r.number("days", 30)) * 86400;
        rel.description = r.text("description");
        a.releases.push_back(rel);
      }
      for (auto &o : j.array("origins")) {
        Origin x;
        if (!source_from_id(o.text("source"), &x.source)) continue;
        x.id = o.text("id");
        x.remote = o.text("remote");
        x.version = o.text("version");
        x.installed = o.flag("installed");
        x.update = o.flag("update");
        x.installed_version = x.installed ? o.text("installed_version", x.version) : "";
        x.download_size = o.number("download", -1);
        x.installed_size = o.number("size", -1);
        x.publisher = o.text("publisher");
        x.verified = o.flag("verified");
        x.classic = o.flag("classic");
        x.license = a.license;
        x.installation = x.source == Source::Flatpak ? o.text("installation", "system") : "";
        if (x.source == Source::Debian) {
          a.pkgnames.push_back(x.id);
          if (x.installed_size >= 0) sizes[x.id] = x.installed_size;
        }
        a.origins.push_back(x);
      }
      store_only.push_back(j.flag("store_only"));
      apps.push_back(std::move(a));
    }
    if (const Json *r = root.get("removes"))
      for (auto &f : r->fields)
        for (auto &p : f.second.items) removes[f.first].push_back(p.str);
    if (const Json *sz = root.get("sizes"))
      for (auto &f : sz->fields) sizes[f.first] = f.second.integer;
    for (auto &h : root.array("history")) {
      HistoryEntry e;
      int hh = 12, mm = 0;
      std::sscanf(h.text("time", "12:00").c_str(), "%d:%d", &hh, &mm);
      struct tm tm = {};
      std::time_t day = now - std::time_t(h.number("days")) * 86400;
      localtime_r(&day, &tm);
      tm.tm_hour = hh;
      tm.tm_min = mm;
      tm.tm_sec = 0;
      tm.tm_isdst = -1;
      e.when = mktime(&tm);
      if (!source_from_id(h.text("source"), &e.source)) continue;
      std::string c = h.text("change");
      e.change = c == "remove" ? Change::Remove : c == "update" ? Change::Update : Change::Install;
      e.items = h.strings("items");
      e.versions.assign(e.items.size(), "");
      e.automatic = int(h.number("automatic"));
      e.requested_by = h.text("by");
      e.failed = h.flag("failed");
      e.error = h.text("error");
      history.push_back(e);
    }
    for (int i = 0; i < kSourceCount; ++i) {
      status[i].available = status[i].installed = true;
      status[i].message = i == 0 ? "PackageKit (test data)" : "test data";
    }
    if (const Json *src = root.get("sources")) {
      for (int i = 1; i < kSourceCount; ++i) {
        const Json *s = src->get(source_id(Source(i)));
        if (!s) continue;
        status[i].available = s->flag("available", true);
        status[i].installed = s->flag("installed", status[i].available);
        status[i].remotes = s->strings("remotes");
        status[i].has_flathub = std::find(status[i].remotes.begin(), status[i].remotes.end(), "flathub") !=
                                status[i].remotes.end();
        status[i].hidden_snap_folder = s->get("hidden_folder") ? (s->flag("hidden_folder") ? 1 : 0) : -1;
        if (!status[i].available) status[i].message = s->text("message", "not installed");
      }
    }
  }

  // The origin a job target names.
  Origin *origin_of(Source source, const std::string &target, App **app = nullptr) {
    std::string id = target, installation;
    if (source == Source::Flatpak) {
      size_t colon = target.find(':');
      if (colon != std::string::npos) {
        installation = target.substr(0, colon);
        id = target.substr(colon + 1);
      }
    }
    for (auto &a : apps)
      for (auto &o : a.origins)
        if (o.source == source && o.id == id && (installation.empty() || o.installation == installation)) {
          if (app) *app = &a;
          return &o;
        }
    return nullptr;
  }
};

FakeWorld &world() {
  static FakeWorld w;
  return w;
}

struct FakeJob {
  Job job;
  std::function<void(const Progress &)> progress;
  DoneFn done;
  double elapsed = 0;
  bool cancelled = false;
};

class FakeBackend : public Backend {
 public:
  FakeBackend(Source s, const std::string &dir) : source_(s) { world().load(dir); }
  Source source() const override { return source_; }

  void probe(std::function<void(SourceStatus)> done) override {
    SourceStatus st = world().status[int(source_)];
    Fl::add_timeout(0.05, call_later, new std::function<void()>([done, st] { done(st); }));
  }

  void load_installed(AppsFn done) override {
    std::vector<App> out;
    for (auto &a : world().apps)
      for (auto &o : a.origins)
        if (o.source == source_ && o.installed) {
          App copy = a;
          copy.origins = {o};
          out.push_back(copy);
        }
    Fl::add_timeout(0.1, call_later, new std::function<void()>([done, out] { done(out, ""); }));
  }

  void search(const std::string &query, AppsFn done) override {
    if (source_ != Source::Snap) return done({}, "");
    Catalog store;
    std::vector<App> out;
    for (size_t i = 0; i < world().apps.size(); ++i)
      if (world().store_only[i]) store.add(world().apps[i]);
    for (const App *a : merkadu::search(store, query)) out.push_back(*a);
    Fl::add_timeout(0.3, call_later, new std::function<void()>([done, out] { done(out, ""); }));
  }

  void browse(const std::string &category, AppsFn done) override {
    std::vector<App> out;
    for (size_t i = 0; i < world().apps.size(); ++i)
      if (world().store_only[i] && source_ == Source::Snap) out.push_back(world().apps[i]);
    Fl::add_timeout(0.3, call_later, new std::function<void()>([done, out] { done(out, ""); }));
  }

  void details(const App &app, const Origin &o, std::function<void(Origin)> done) override {}

  void lookup(const App &app, AppsFn done) override {
    std::vector<App> out;
    if (source_ == Source::Snap)
      for (auto &a : world().apps)
        if (a.id == app.id && a.has_source(Source::Snap)) {
          App copy = a;
          copy.origins = {*a.origin(Source::Snap)};
          copy.origins[0].installed = false;
          out.push_back(copy);
        }
    Fl::add_timeout(0.4, call_later, new std::function<void()>([done, out] { done(out, ""); }));
  }

  void run(const Job &job, std::function<void(const Progress &)> progress, DoneFn done) override {
    auto *fj = new FakeJob{job, std::move(progress), std::move(done)};
    jobs_[job.id] = fj;
    Fl::add_timeout(0.1, tick, new std::pair<FakeBackend *, int>(this, job.id));
  }

  void cancel(int job_id) override {
    auto it = jobs_.find(job_id);
    if (it != jobs_.end()) it->second->cancelled = true;
  }

  void history(std::function<void(std::vector<HistoryEntry>)> done) override {
    std::vector<HistoryEntry> out;
    for (auto &e : world().history)
      if (e.source == source_) out.push_back(e);
    Fl::add_timeout(0.2, call_later, new std::function<void()>([done, out] { done(out); }));
  }

  void plan_remove(const std::vector<std::string> &packages,
                   std::function<void(std::vector<std::string>, long long, std::string)> done) override {
    std::vector<std::string> removed;
    long long freed = 0;
    for (auto &p : packages) {
      auto it = world().removes.find(p);
      for (auto &r : it == world().removes.end() ? std::vector<std::string>{p} : it->second) {
        removed.push_back(r);
        auto sz = world().sizes.find(r);
        if (sz != world().sizes.end()) freed += sz->second;
      }
    }
    if (source_ != Source::Debian) {
      removed = packages;
      freed = -1;
      for (auto &p : packages)
        if (Origin *o = world().origin_of(source_, p)) freed = o->installed_size;
    }
    Fl::add_timeout(0.4, call_later, new std::function<void()>([done, removed, freed] { done(removed, freed, ""); }));
  }

  void owners(const std::vector<std::string> &paths, OwnersFn done) override {
    std::map<std::string, std::vector<std::string>> out;
    for (auto &p : paths) {
      std::string base = basename_of(p);
      for (auto &a : world().apps)
        if (std::find(a.desktop_ids.begin(), a.desktop_ids.end(), base) != a.desktop_ids.end())
          for (auto &o : a.origins)
            if (o.source == Source::Debian) out[p].push_back(o.id);
    }
    Fl::add_timeout(0.05, call_later, new std::function<void()>([done, out] { done(out); }));
  }

  void setup(const std::string &what, DoneFn done) override {
    SourceStatus &st = world().status[int(source_)];
    if (what == "flathub") {
      st.remotes.push_back("flathub");
      st.has_flathub = true;
    } else if (what == "hide-snap-folder") {
      st.hidden_snap_folder = 1;
    }
    Fl::add_timeout(0.5, call_later, new std::function<void()>([done] { done(true, false, ""); }));
  }

 private:
  static void call_later(void *data) {
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(data));
    (*fn)();
  }

  static void tick(void *data) {
    std::unique_ptr<std::pair<FakeBackend *, int>> arg(static_cast<std::pair<FakeBackend *, int> *>(data));
    FakeBackend *self = arg->first;
    auto it = self->jobs_.find(arg->second);
    if (it == self->jobs_.end()) return;
    FakeJob *fj = it->second;
    if (fj->cancelled) {
      DoneFn done = fj->done;
      self->jobs_.erase(it);
      delete fj;
      done(false, true, "");
      return;
    }
    fj->elapsed += 0.1;
    double f = std::min(1.0, fj->elapsed / world().seconds);
    Progress p;
    p.percent = int(f * 100);
    p.cancellable = f < 0.7;
    if (fj->job.action == Action::Remove)
      p.status = _("Removing");
    else
      p.status = f < 0.55 ? _("Downloading") : fj->job.action == Action::Update ? _("Updating") : _("Installing");
    fj->progress(p);
    if (f < 1.0) {
      Fl::add_timeout(0.1, tick, arg.release());
      return;
    }
    bool fail = false;
    const std::string &f_target = world().fail;  // a package, a ref ("system:REF" matches REF) or a snap
    if (!f_target.empty())
      for (auto &t : fj->job.targets) fail = fail || t == f_target || t.find(":" + f_target) != std::string::npos;
    std::string error;
    if (fail) {
      error = format(_("Simulated failure of %s (MERKADU_FAKE_FAIL)."), fj->job.title.c_str());
    } else {
      HistoryEntry e;
      e.when = std::time(nullptr);
      e.source = fj->job.source;
      e.change = fj->job.action == Action::Remove ? Change::Remove
                 : fj->job.action == Action::Update ? Change::Update
                                                    : Change::Install;
      e.requested_by = user_name();
      e.tool = "merkadu";
      for (auto &t : fj->job.targets) {
        App *app = nullptr;
        Origin *o = world().origin_of(fj->job.source, t, &app);
        if (!o) continue;
        if (fj->job.action == Action::Remove) {
          o->installed = false;
          o->update = false;
          o->installed_version.clear();
        } else {
          o->installed = true;
          o->update = false;
          o->installed_version = o->version;
        }
        e.items.push_back(o->id);
        e.versions.push_back(o->version);
      }
      world().history.push_back(e);
    }
    DoneFn done = fj->done;
    self->jobs_.erase(it);
    delete fj;
    done(!fail, false, error);
  }

  Source source_;
  std::map<int, FakeJob *> jobs_;
};

}  // namespace

std::unique_ptr<Backend> make_fake_backend(Source source, const std::string &dir) {
  return std::unique_ptr<Backend>(new FakeBackend(source, dir));
}

void load_fake_catalog(const std::string &dir, CatalogFn done) {
  world().load(dir);
  std::vector<App> out;
  for (size_t i = 0; i < world().apps.size(); ++i) {
    if (world().store_only[i]) continue;
    App a = world().apps[i];
    // What is installed comes from the backends.
    for (auto &o : a.origins) {
      o.installed = o.update = false;
      o.installed_version.clear();
    }
    // Snaps are not in the AppStream catalog; the store search finds them.
    a.origins.erase(std::remove_if(a.origins.begin(), a.origins.end(),
                                   [](const Origin &o) { return o.source == Source::Snap; }),
                    a.origins.end());
    if (!a.origins.empty()) out.push_back(std::move(a));
  }
  // Like the real catalog, it arrives a moment later.
  auto *fn = new std::function<void()>([done, out] { done(out, ""); });
  Fl::add_timeout(0.3, [](void *data) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(data));
    (*f)();
  }, fn);
}

}  // namespace merkadu
}  // namespace sde
