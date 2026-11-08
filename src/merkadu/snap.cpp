// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Snap source: snapd's REST API on /run/snapd.socket (snapd.h). Reads
// (installed snaps, the store's search, categories and featured snaps,
// changes) need no authorization; installing, removing and refreshing send
// "X-Allow-Interaction: true" so that snapd asks polkit, and the SDE polkit
// agent shows the password prompt. A change is followed by polling
// /v2/changes/ID on a worker thread (snap_change()).
//
// The store's answers are kept in $XDG_CACHE_HOME/merkadu/snapd, so that
// Home, the departments and All Software still show snaps offline.
//
// snapd keeps each user's snap data in ~/snap. Before the first snap is
// installed Merkadu sets experimental.hidden-snap-folder (setup
// "hide-snap-folder"), so that it is ~/.snap/data instead; docs/MERKADU.md
// explains why it cannot be moved out of the home folder.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#include "backend.h"
#include "i18n.h"
#include "snapd.h"
#include "storage.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

// Changes wait this long for snapd to be ready: after Enable Snap Support
// (snapd starts and seeds itself) longer than otherwise.
constexpr int kStartWait = 180;
constexpr int kBusyWait = 60;

std::string cache_file(const std::string &path) {
  std::string base = env("XDG_CACHE_HOME");
  if (base.empty()) base = join_path(home_dir(), ".cache");
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : path) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char name[32];
  std::snprintf(name, sizeof name, "%016llx.json", static_cast<unsigned long long>(h));
  return join_path(base, std::string("merkadu/snapd/") + name);
}

// The snaps of a store query: snapd's answer, kept for offline use; the
// last answer kept when snapd cannot reach the store.
std::vector<App> get_apps(const std::string &path, bool installed, std::string *error) {
  std::vector<App> out;
  HttpResponse r = http_unix(snapd_socket(), "GET", path);
  bool store = starts_with(path, "/v2/find");
  if (r.status == 200 && parse_snaps(r.body, installed, out)) {
    if (store) {
      std::string file = cache_file(path);
      make_dirs(file.substr(0, file.rfind('/')));
      write_file_atomic(file, r.body);
    }
    return out;
  }
  out.clear();
  // An empty search is answered with 404 "no snaps found".
  if (r.status == 404) return out;
  std::string cached;
  if (store && read_file(cache_file(path), cached) && parse_snaps(cached, installed, out)) return out;
  if (error) *error = snapd_error_text(r);
  return out;
}

// The socket of a test's fake snapd: only it says whether snapd is there.
bool test_socket() { return !env("MERKADU_SNAPD_SOCKET").empty(); }

struct Running {
  std::atomic<bool> cancel{false};
};

// A snapd change of |run|, its progress on the FLTK thread.
std::pair<std::string, bool> follow_change(const std::string &method, const std::string &path,
                                           const std::string &body, std::shared_ptr<Running> run,
                                           std::function<void(const Progress &)> progress, int wait_s) {
  auto report = [progress](const SnapProgress &sp) {
    if (!progress) return;
    Progress p;
    p.percent = sp.percent;
    p.status = sp.status;
    // snapd undoes what an aborted change did; a download it stops is
    // continued from its partial file when the change runs again.
    p.cancellable = !sp.aborting;
    p.phase = sp.waiting ? JobPhase::Waiting
              : sp.kind == "download-snap" ? JobPhase::Downloading
              : sp.kind.empty() || sp.kind == "prerequisites" || sp.kind == "validate-snap" ? JobPhase::Preparing
                                                                                            : JobPhase::Applying;
    post([progress, p] { progress(p); });
  };
  SnapChangeResult r =
      snap_change(snapd_socket(), method, path, body, run ? &run->cancel : nullptr, report, wait_s);
  return {r.error, r.cancelled};
}

class SnapBackend : public Backend {
 public:
  Source source() const override { return Source::Snap; }

  void probe(std::function<void(SourceStatus)> done) override {
    background<SourceStatus>(
        [] {
          SourceStatus st;
          std::string sock = snapd_socket();
          st.installed = file_exists(sock) ||
                         (!test_socket() && (!find_program("snap").empty() || file_exists("/usr/lib/snapd/snapd")));
          HttpResponse r = http_unix(sock, "GET", "/v2/system-info", "", false, 8000);
          if (r.status != 200) {
            st.message = st.installed ? std::string(_("snapd is not running")) : std::string(_("snapd is not installed"));
            return st;
          }
          st.available = st.installed = true;
          Json root;
          if (parse_json(r.body, root))
            if (const Json *res = root.get("result")) {
              st.message = "snapd " + res->text("version");
              // Without AppArmor (Debian turns it on by default) snaps are
              // only partly isolated.
              std::string confinement = res->text("confinement");
              if (confinement == "partial") st.message += ", " + std::string(_("partial confinement (AppArmor is off)"));
            }
          HttpResponse conf = http_unix(sock, "GET", "/v2/snaps/system/conf?keys=experimental.hidden-snap-folder");
          bool hidden = false;
          if (conf.status == 200 && parse_hidden_folder(conf.body, &hidden))
            st.hidden_snap_folder = hidden ? 1 : 0;
          else  // reading the configuration needs authorization (or the option was never set, 400):
                // snapd exports its enabled experimental features as files for snap-confine
            st.hidden_snap_folder = file_exists("/var/lib/snapd/features/hidden-snap-folder") ? 1 : 0;
          return st;
        },
        done, "snapd answered");
  }

  void load_installed(AppsFn done) override {
    background<std::pair<std::vector<App>, std::string>>(
        [] {
          std::string error;
          std::vector<App> apps = get_apps("/v2/snaps", true, &error);
          // Refreshes the store has for them.
          std::vector<App> refresh = get_apps("/v2/find?select=refresh", false, nullptr);
          for (auto &a : apps)
            for (auto &r : refresh)
              if (!a.origins.empty() && !r.origins.empty() && a.origins[0].id == r.origins[0].id) {
                a.origins[0].update = true;
                a.origins[0].version = r.origins[0].version;
              }
          return std::make_pair(std::move(apps), error);
        },
        [done](std::pair<std::vector<App>, std::string> r) { done(std::move(r.first), r.second); },
        "the installed snaps arrived");
  }

  // Every revision snapd keeps: the active one of each snap and the old
  // ones it keeps to go back to (refresh.retain), which Clean Up removes.
  void storage(StorageFn done) override {
    using Result = std::pair<std::vector<StorageEntry>, bool>;
    background<Result>(
        [] {
          Result r;
          HttpResponse resp = http_unix(snapd_socket(), "GET", "/v2/snaps?select=all", "", false, 15000);
          std::vector<SnapRevision> revs;
          if (resp.status != 200 || !parse_snap_revisions(resp.body, revs)) return r;
          r.second = true;
          for (auto &rev : revs) {
            StorageEntry e;
            e.source = Source::Snap;
            e.id = rev.name;
            e.name = rev.title;
            e.kind = rev.active ? "app" : "revision";
            e.version = rev.version + " (" + rev.revision + ")";
            e.size = rev.size;
            e.unused = !rev.active;
            e.target = rev.active ? rev.name : rev.name + "@" + rev.revision;
            r.first.push_back(std::move(e));
          }
          return r;
        },
        [done](Result r) { done(std::move(r.first), r.second); }, "snapd's revisions arrived");
  }

  void search(const std::string &query, AppsFn done) override {
    std::string path = "/v2/find?q=" + url_encode(query);
    background<std::vector<App>>([path] { return get_apps(path, false, nullptr); },
                                 [done](std::vector<App> apps) { done(std::move(apps), ""); },
                                 "the Snap Store answered a search");
  }

  void browse(const std::string &category, AppsFn done) override {
    std::string cat = url_encode(category);
    background<std::vector<App>>(
        [cat] {
          std::vector<App> apps = get_apps("/v2/find?category=" + cat, false, nullptr);
          // Older snapd: sections.
          if (apps.empty()) apps = get_apps("/v2/find?section=" + cat, false, nullptr);
          return apps;
        },
        [done](std::vector<App> apps) { done(std::move(apps), ""); }, "the Snap Store answered a department");
  }

  void featured(AppsFn done) override {
    background<std::pair<std::vector<App>, std::string>>(
        [] {
          std::string error;
          std::vector<App> apps;
          for (const char *path : {"/v2/find?category=featured", "/v2/find?section=featured"}) {
            apps = get_apps(path, false, &error);
            if (!apps.empty()) break;
          }
          return std::make_pair(std::move(apps), error);
        },
        [done](std::pair<std::vector<App>, std::string> r) { done(std::move(r.first), r.second); },
        "the Snap Store's featured snaps arrived");
  }

  void lookup(const App &app, AppsFn done) override {
    std::string path = "/v2/find?common-id=" + url_encode(app.appstream_id);
    background<std::vector<App>>([path] { return get_apps(path, false, nullptr); },
                                 [done](std::vector<App> apps) { done(std::move(apps), ""); });
  }

  void details(const App &app, const Origin &o, std::function<void(Origin)> done) override {
    if (o.installed || !valid_snap_name(o.id)) return;
    std::string path = "/v2/find?name=" + url_encode(o.id);
    background<std::vector<App>>([path] { return get_apps(path, false, nullptr); },
                                 [done](std::vector<App> apps) {
                                   for (auto &a : apps)
                                     if (!a.origins.empty()) done(a.origins[0]);
                                 });
  }

  void run(const Job &job, std::function<void(const Progress &)> progress, DoneFn done) override {
    // "NAME@REVISION": one old revision of a snap (Clean Up), removed alone.
    for (auto &t : job.targets) {
      std::string name = t.substr(0, t.find('@'));
      std::string rev = t.find('@') == std::string::npos ? "" : t.substr(t.find('@') + 1);
      bool rev_ok = rev.empty() || (job.action == Action::Remove && job.targets.size() == 1 &&
                                    std::all_of(rev.begin(), rev.end(), [](char c) {
                                      return (c >= '0' && c <= '9') || c == 'x';
                                    }));
      if (!valid_snap_name(name) || !rev_ok) {
        done(false, false, format(_("\"%s\" is not a snap name."), t.c_str()));
        return;
      }
    }
    auto run = std::make_shared<Running>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_[job.id] = run;
    }
    {
      // It can be stopped before snapd accepts it.
      Progress p;
      p.status = _("Preparing");
      p.cancellable = true;
      p.phase = JobPhase::Preparing;
      progress(p);
    }
    int id = job.id;
    Job j = job;
    background<std::pair<std::string, bool>>(
        [j, run, progress] {
          std::string action = j.action == Action::Install ? "install" : j.action == Action::Remove ? "remove"
                                                                                                    : "refresh";
          std::string method = "POST", path, body;
          if (j.targets.size() == 1) {
            std::string name = j.targets[0].substr(0, j.targets[0].find('@'));
            path = "/v2/snaps/" + name;
            body = "{\"action\":" + json_quote(action);
            if (name != j.targets[0]) body += ",\"revision\":" + json_quote(j.targets[0].substr(name.size() + 1));
            if (j.action == Action::Install) {
              // The channel of the store result ("latest/stable"); stable by default.
              std::string channel = j.remote.empty() ? "stable" : j.remote;
              body += ",\"channel\":" + json_quote(channel);
              if (j.classic) body += ",\"classic\":true";
            }
            body += "}";
          } else {
            std::string list;
            for (auto &t : j.targets) list += (list.empty() ? "" : ",") + json_quote(t);
            path = "/v2/snaps";
            body = "{\"action\":" + json_quote(action) + ",\"snaps\":[" + list + "]}";
          }
          // Right after Enable Snap Support snapd starts and seeds itself;
          // otherwise it may be busy with a change of its own.
          return follow_change(method, path, body, run, progress, j.needs ? kStartWait : kBusyWait);
        },
        [this, id, done](std::pair<std::string, bool> result) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.erase(id);
          }
          if (result.second)
            done(false, true, "");
          else
            done(result.first.empty(), false, result.first);
        });
  }

  void cancel(int job_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = running_.find(job_id);
    if (it != running_.end()) it->second->cancel = true;
  }

  void history(std::function<void(std::vector<HistoryEntry>)> done) override {
    background<std::vector<HistoryEntry>>(
        [] {
          std::vector<HistoryEntry> out;
          HttpResponse r = http_unix(snapd_socket(), "GET", "/v2/changes?select=all", "", false, 15000);
          if (r.status == 200) parse_snap_changes(r.body, out);
          return out;
        },
        done);
  }

  void setup(const std::string &what, DoneFn done) override {
    if (what != "hide-snap-folder") {
      done(false, false, "unknown setup");
      return;
    }
    background<std::pair<std::string, bool>>(
        [] {
          return follow_change("PUT", "/v2/snaps/system/conf", "{\"experimental.hidden-snap-folder\":true}", nullptr,
                               nullptr, kBusyWait);
        },
        [done](std::pair<std::string, bool> r) { done(r.first.empty() && !r.second, r.second, r.first); });
  }

 private:
  std::mutex mutex_;
  std::map<int, std::shared_ptr<Running>> running_;
};

}  // namespace

std::unique_ptr<Backend> make_snap_backend() { return std::unique_ptr<Backend>(new SnapBackend()); }

}  // namespace merkadu
}  // namespace sde
