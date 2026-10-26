// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Snap source: snapd's REST API on /run/snapd.socket (snapd.h). Reads
// (installed snaps, the store's search and categories, changes) need no
// authorization; installing, removing and refreshing send
// "X-Allow-Interaction: true" so that snapd asks polkit, and the SDE polkit
// agent shows the password prompt. A change is followed by polling
// /v2/changes/ID on a worker thread.
//
// snapd keeps each user's snap data in ~/snap. Before the first snap is
// installed Merkadu sets experimental.hidden-snap-folder (setup
// "hide-snap-folder"), so that it is ~/.snap/data instead; docs/MERKADU.md
// explains why it cannot be moved out of the home folder.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "backend.h"
#include "i18n.h"
#include "snapd.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

constexpr int kInteractiveTimeout = 10 * 60 * 1000;  // polkit may wait for the user

std::string error_of(const HttpResponse &r) {
  if (r.status == 0) return r.error.empty() ? std::string(_("snapd did not answer.")) : r.error;
  std::string msg = snapd_error(r.body);
  std::string kind = snapd_error_kind(r.body);
  if (kind == "auth-cancelled") return "";
  if (kind == "login-required" || r.status == 401 || r.status == 403)
    return msg.empty() ? std::string(_("You are not allowed to do this, or the authentication failed.")) : msg;
  return msg.empty() ? format(_("snapd answered with error %d."), r.status) : msg;
}

std::vector<App> get_apps(const std::string &path, bool installed, std::string *error) {
  std::vector<App> out;
  HttpResponse r = http_unix(snapd_socket(), "GET", path);
  if (r.status != 200 || !parse_snaps(r.body, installed, out)) {
    // An empty search is answered with 404 "no snaps found".
    if (error && r.status != 404) *error = error_of(r);
    out.clear();
  }
  return out;
}

struct Running {
  std::atomic<bool> cancel{false};
  std::string change;  // set once snapd accepted it
  std::mutex mutex;
};

// Follows change |id| until it is ready; |progress| on the FLTK thread.
std::string follow(const std::string &id, std::shared_ptr<Running> run,
                   std::function<void(const Progress &)> progress, bool *cancelled) {
  bool abort_sent = false;
  for (;;) {
    if (run && run->cancel && !abort_sent) {
      http_unix(snapd_socket(), "POST", "/v2/changes/" + id, "{\"action\":\"abort\"}", true, 30000);
      abort_sent = true;
    }
    HttpResponse r = http_unix(snapd_socket(), "GET", "/v2/changes/" + id);
    SnapChange c;
    if (r.status != 200 || !parse_snap_change(r.body, c)) return error_of(r);
    if (progress) {
      Progress p;
      p.percent = c.percent;
      p.status = c.label;
      p.cancellable = !c.ready;
      post([progress, p] { progress(p); });
    }
    if (c.ready) {
      if (c.status == "Done") return "";
      if (abort_sent) {
        *cancelled = true;
        return "";
      }
      return c.error.empty() ? format(_("The change ended with status %s."), c.status.c_str()) : c.error;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }
}

class SnapBackend : public Backend {
 public:
  Source source() const override { return Source::Snap; }

  void probe(std::function<void(SourceStatus)> done) override {
    background<SourceStatus>(
        [] {
          SourceStatus st;
          std::string sock = snapd_socket();
          st.installed = file_exists(sock) || !find_program("snap").empty() || file_exists("/usr/lib/snapd/snapd");
          HttpResponse r = http_unix(sock, "GET", "/v2/system-info", "", false, 8000);
          if (r.status != 200) {
            st.message = st.installed ? std::string(_("snapd is not running")) : std::string(_("snapd is not installed"));
            return st;
          }
          st.available = st.installed = true;
          Json root;
          if (parse_json(r.body, root))
            if (const Json *res = root.get("result")) st.message = "snapd " + res->text("version");
          HttpResponse conf = http_unix(sock, "GET", "/v2/snaps/system/conf?keys=experimental.hidden-snap-folder");
          bool hidden = false;
          if (conf.status == 200 && parse_hidden_folder(conf.body, &hidden))
            st.hidden_snap_folder = hidden ? 1 : 0;
          else  // reading the configuration needs authorization (or the option was never set, 400):
                // snapd exports its enabled experimental features as files for snap-confine
            st.hidden_snap_folder = file_exists("/var/lib/snapd/features/hidden-snap-folder") ? 1 : 0;
          return st;
        },
        done);
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
        [done](std::pair<std::vector<App>, std::string> r) { done(std::move(r.first), r.second); });
  }

  void search(const std::string &query, AppsFn done) override {
    std::string path = "/v2/find?q=" + url_encode(query);
    background<std::vector<App>>([path] { return get_apps(path, false, nullptr); },
                                 [done](std::vector<App> apps) { done(std::move(apps), ""); });
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
        [done](std::vector<App> apps) { done(std::move(apps), ""); });
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
    for (auto &t : job.targets)
      if (!valid_snap_name(t)) {
        done(false, false, format(_("\"%s\" is not a snap name."), t.c_str()));
        return;
      }
    auto run = std::make_shared<Running>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_[job.id] = run;
    }
    int id = job.id;
    Job j = job;
    background<std::pair<std::string, bool>>(
        [j, run, progress] {
          std::string action = j.action == Action::Install ? "install" : j.action == Action::Remove ? "remove"
                                                                                                    : "refresh";
          HttpResponse r;
          if (j.targets.size() == 1) {
            std::string body = "{\"action\":" + json_quote(action);
            if (j.action == Action::Install) {
              // The channel of the store result ("latest/stable"); stable by default.
              std::string channel = j.remote.empty() ? "stable" : j.remote;
              body += ",\"channel\":" + json_quote(channel);
              if (j.classic) body += ",\"classic\":true";
            }
            body += "}";
            r = http_unix(snapd_socket(), "POST", "/v2/snaps/" + j.targets[0], body, true, kInteractiveTimeout);
          } else {
            std::string list;
            for (auto &t : j.targets) list += (list.empty() ? "" : ",") + json_quote(t);
            r = http_unix(snapd_socket(), "POST", "/v2/snaps", "{\"action\":" + json_quote(action) +
                                                                     ",\"snaps\":[" + list + "]}",
                          true, kInteractiveTimeout);
          }
          std::string change = r.status == 202 ? snapd_change_id(r.body) : "";
          if (change.empty()) {
            std::string err = error_of(r);
            return std::make_pair(err, err.empty());  // empty: the user cancelled the password prompt
          }
          bool cancelled = false;
          std::string err = follow(change, run, progress, &cancelled);
          return std::make_pair(err, cancelled);
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
          HttpResponse r = http_unix(snapd_socket(), "PUT", "/v2/snaps/system/conf",
                                     "{\"experimental.hidden-snap-folder\":true}", true, kInteractiveTimeout);
          std::string change = r.status == 202 ? snapd_change_id(r.body) : "";
          if (change.empty()) {
            std::string err = error_of(r);
            return std::make_pair(err, err.empty());
          }
          bool cancelled = false;
          std::string err = follow(change, nullptr, nullptr, &cancelled);
          return std::make_pair(err, cancelled);
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
