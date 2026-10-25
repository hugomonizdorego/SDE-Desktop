// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Flatpak source, with libflatpak: apps are installed into the system
// installation (/var/lib/flatpak, for every user; Flatpak's system helper
// asks polkit, so the SDE polkit agent shows the password prompt) and
// removed from the installation they are in (also the user's own,
// ~/.local/share/flatpak). Transactions run on a worker thread each with
// their own FlatpakInstallation; progress is handed to the FLTK thread.
// "Add Flathub" adds the remote from its .flatpakrepo file and downloads
// its AppStream data, which the catalog (libappstream) then reads.
#include "backend.h"
#include "i18n.h"
#include "util.h"

#ifdef MERKADU_HAVE_FLATPAK

#include <flatpak.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#include "process.h"

namespace sde {
namespace merkadu {

namespace {

std::string str(const char *s) { return s ? s : ""; }

FlatpakInstallation *open_installation(const std::string &which, std::string *error) {
  GError *err = nullptr;
  FlatpakInstallation *inst = which == "user" ? flatpak_installation_new_user(nullptr, &err)
                                              : flatpak_installation_new_system(nullptr, &err);
  if (!inst && error) *error = err ? err->message : "cannot open the Flatpak installation";
  g_clear_error(&err);
  return inst;
}

// The exported icon of an installed app.
std::string exported_icon(const std::string &which, const std::string &id) {
  std::string base = which == "user" ? join_path(data_home(), "flatpak/exports/share/icons/hicolor")
                                     : std::string("/var/lib/flatpak/exports/share/icons/hicolor");
  for (const char *dir : {"scalable/apps/", "128x128/apps/", "256x256/apps/", "64x64/apps/", "48x48/apps/"})
    for (const char *ext : {".svg", ".png"}) {
      std::string p = base + "/" + dir + id + ext;
      if (file_exists(p)) return p;
    }
  return "";
}

std::vector<App> installed_apps(const std::string &which) {
  std::vector<App> out;
  std::string error;
  FlatpakInstallation *inst = open_installation(which, &error);
  if (!inst) return out;
  GError *err = nullptr;
  GPtrArray *refs = flatpak_installation_list_installed_refs_by_kind(inst, FLATPAK_REF_KIND_APP, nullptr, &err);
  g_clear_error(&err);
  // Updates need the remotes' summaries: without a network there are none.
  std::map<std::string, bool> updates;
  if (GPtrArray *upd = flatpak_installation_list_installed_refs_for_update(inst, nullptr, &err)) {
    for (guint i = 0; i < upd->len; ++i)
      updates[str(flatpak_ref_format_ref_cached(FLATPAK_REF(g_ptr_array_index(upd, i))))] = true;
    g_ptr_array_unref(upd);
  }
  g_clear_error(&err);
  for (guint i = 0; refs && i < refs->len; ++i) {
    FlatpakInstalledRef *r = FLATPAK_INSTALLED_REF(g_ptr_array_index(refs, i));
    FlatpakRef *ref = FLATPAK_REF(r);
    App a;
    std::string id = str(flatpak_ref_get_name(ref));
    a.appstream_id = id;
    a.id = normalize_id(id);
    a.name = str(flatpak_installed_ref_get_appdata_name(r));
    if (a.name.empty()) a.name = id;
    a.summary = str(flatpak_installed_ref_get_appdata_summary(r));
    a.icon = exported_icon(which, id);
    a.desktop_ids.push_back(id + ".desktop");
    Origin o;
    o.source = Source::Flatpak;
    o.id = str(flatpak_ref_format_ref_cached(ref));
    o.remote = str(flatpak_installed_ref_get_origin(r));
    o.installation = which;
    o.installed = true;
    o.installed_version = str(flatpak_installed_ref_get_appdata_version(r));
    o.installed_size = (long long)flatpak_installed_ref_get_installed_size(r);
    o.update = updates.count(o.id) > 0;
    a.origins.push_back(o);
    out.push_back(std::move(a));
  }
  if (refs) g_ptr_array_unref(refs);
  g_object_unref(inst);
  return out;
}

// What a running transaction reports, shared with its signal handlers.
struct Run {
  Job job;
  std::function<void(const Progress &)> progress;
  GCancellable *cancellable = nullptr;
  std::string error;
  int index = 0, total = 1;
  std::atomic<bool> done{false};
};

void progress_changed(FlatpakTransactionProgress *p, gpointer data) {
  auto *run = static_cast<Run *>(data);
  Progress pr;
  int op = flatpak_transaction_progress_get_progress(p);
  pr.percent = std::min(100, (run->index * 100 + op) / std::max(1, run->total));
  char *status = flatpak_transaction_progress_get_status(p);
  pr.status = run->job.action == Action::Remove ? std::string(_("Removing"))
              : op < 99 ? std::string(_("Downloading"))
                        : run->job.action == Action::Update ? std::string(_("Updating")) : std::string(_("Installing"));
  g_free(status);
  pr.cancellable = true;
  auto fn = run->progress;
  post([fn, pr] { fn(pr); });
}

void new_operation(FlatpakTransaction *t, FlatpakTransactionOperation *op, FlatpakTransactionProgress *p,
                   gpointer data) {
  auto *run = static_cast<Run *>(data);
  GList *ops = flatpak_transaction_get_operations(t);
  run->total = std::max(1, int(g_list_length(ops)));
  int i = 0;
  for (GList *l = ops; l; l = l->next, ++i)
    if (l->data == op) run->index = i;
  g_list_free_full(ops, g_object_unref);
  flatpak_transaction_progress_set_update_frequency(p, 300);
  g_signal_connect(p, "changed", G_CALLBACK(progress_changed), data);
}

gboolean operation_error(FlatpakTransaction *, FlatpakTransactionOperation *op, GError *error,
                         FlatpakTransactionErrorDetails details, gpointer data) {
  auto *run = static_cast<Run *>(data);
  if (error && !g_error_matches(error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED)) run->error = error->message;
  return FALSE;  // stop
}

gboolean ready(FlatpakTransaction *, gpointer) { return TRUE; }
gboolean add_new_remote(FlatpakTransaction *, FlatpakTransactionRemoteReason, const char *, const char *,
                        const char *, gpointer) {
  return FALSE;  // only remotes the administrator added
}

class FlatpakBackend : public Backend {
 public:
  Source source() const override { return Source::Flatpak; }

  void probe(std::function<void(SourceStatus)> done) override {
    bool refresh = !refreshed_;
    refreshed_ = true;
    background<SourceStatus>(
        [refresh] {
          SourceStatus st;
          std::string error;
          bool helper = file_exists("/usr/share/dbus-1/system-services/org.freedesktop.Flatpak.SystemHelper.service");
          st.installed = helper || !find_program("flatpak").empty();
          FlatpakInstallation *inst = open_installation("system", &error);
          if (!inst || !st.installed) {
            st.message = st.installed ? error : std::string(_("Flatpak is not installed"));
            if (inst) g_object_unref(inst);
            return st;
          }
          st.available = true;
          GError *err = nullptr;
          GPtrArray *remotes = flatpak_installation_list_remotes(inst, nullptr, &err);
          g_clear_error(&err);
          bool changed_any = false;
          for (guint i = 0; remotes && i < remotes->len; ++i) {
            FlatpakRemote *r = FLATPAK_REMOTE(g_ptr_array_index(remotes, i));
            if (flatpak_remote_get_disabled(r)) continue;
            std::string name = str(flatpak_remote_get_name(r));
            char *url = flatpak_remote_get_url(r);
            if (name == "flathub" || str(url).find("flathub.org") != std::string::npos) st.has_flathub = true;
            g_free(url);
            st.remotes.push_back(name);
            // The remotes' AppStream data: refreshed once a day.
            if (refresh) {
              GFile *stamp = flatpak_remote_get_appstream_timestamp(r, nullptr);
              GFileInfo *info = stamp ? g_file_query_info(stamp, G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NONE,
                                                          nullptr, nullptr)
                                      : nullptr;
              guint64 mtime = info ? g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED) : 0;
              if (info) g_object_unref(info);
              if (stamp) g_object_unref(stamp);
              if (guint64(std::time(nullptr)) - mtime > 86400) {
                gboolean changed = FALSE;
                if (flatpak_installation_update_appstream_full_sync(inst, name.c_str(), nullptr, nullptr, nullptr,
                                                                    &changed, nullptr, &err))
                  changed_any = changed_any || changed;
                g_clear_error(&err);
              }
            }
          }
          if (remotes) g_ptr_array_unref(remotes);
          g_object_unref(inst);
          st.message = changed_any ? "appstream-changed" : "";
          return st;
        },
        [done](SourceStatus st) {
          bool reload = st.message == "appstream-changed";
          if (reload) st.message.clear();
          done(st);
          if (reload) post([] { load_catalog_again(); });
        });
  }

  void load_installed(AppsFn done) override {
    background<std::vector<App>>(
        [] {
          std::vector<App> out = installed_apps("system");
          for (auto &a : installed_apps("user")) out.push_back(std::move(a));
          return out;
        },
        [done](std::vector<App> apps) { done(std::move(apps), ""); });
  }

  void run(const Job &job, std::function<void(const Progress &)> progress, DoneFn done) override {
    auto run = std::make_shared<Run>();
    run->job = job;
    run->progress = progress;
    run->cancellable = g_cancellable_new();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_[job.id] = run->cancellable;
    }
    int id = job.id;
    background<std::string>(
        [run] {
          // Targets are "installation:ref"; all of one job share an installation.
          std::string which = "system";
          std::vector<std::string> refs;
          for (auto &t : run->job.targets) {
            size_t colon = t.find(':');
            if (colon == std::string::npos) continue;
            which = t.substr(0, colon) == "user" ? "user" : "system";
            std::string ref = t.substr(colon + 1);
            if (!valid_flatpak_ref(ref)) return format(_("\"%s\" is not a Flatpak ref."), ref.c_str());
            refs.push_back(ref);
          }
          if (refs.empty()) return std::string(_("Nothing to do."));
          std::string error;
          FlatpakInstallation *inst = open_installation(which, &error);
          if (!inst) return error;
          GError *err = nullptr;
          FlatpakTransaction *t = flatpak_transaction_new_for_installation(inst, run->cancellable, &err);
          if (!t) {
            error = err ? err->message : "cannot start a Flatpak transaction";
            g_clear_error(&err);
            g_object_unref(inst);
            return error;
          }
          bool added = true;
          for (auto &ref : refs) {
            if (run->job.action == Action::Install)
              added = flatpak_transaction_add_install(t, run->job.remote.c_str(), ref.c_str(), nullptr, &err);
            else if (run->job.action == Action::Remove)
              added = flatpak_transaction_add_uninstall(t, ref.c_str(), &err);
            else
              added = flatpak_transaction_add_update(t, ref.c_str(), nullptr, nullptr, &err);
            if (!added) break;
          }
          if (added) {
            g_signal_connect(t, "new-operation", G_CALLBACK(new_operation), run.get());
            g_signal_connect(t, "operation-error", G_CALLBACK(operation_error), run.get());
            g_signal_connect(t, "ready", G_CALLBACK(ready), nullptr);
            g_signal_connect(t, "add-new-remote", G_CALLBACK(add_new_remote), nullptr);
            added = flatpak_transaction_run(t, run->cancellable, &err);
          }
          if (!added) error = !run->error.empty() ? run->error : err ? err->message : "Flatpak failed";
          g_clear_error(&err);
          g_object_unref(t);
          g_object_unref(inst);
          return error;
        },
        [this, id, run, done](std::string error) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.erase(id);
          }
          bool cancelled = g_cancellable_is_cancelled(run->cancellable);
          g_object_unref(run->cancellable);
          if (cancelled)
            done(false, true, "");
          else
            done(error.empty(), false, error);
        });
  }

  void cancel(int job_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = running_.find(job_id);
    if (it != running_.end()) g_cancellable_cancel(it->second);
  }

  void plan_remove(const std::vector<std::string> &packages,
                   std::function<void(std::vector<std::string>, long long, std::string)> done) override {
    done(packages, -1, "");
  }

  void history(std::function<void(std::vector<HistoryEntry>)> done) override {
    if (find_program("flatpak").empty()) {
      done({});
      return;
    }
    bool started = run_async({"env", "LC_ALL=C", "flatpak", "history",
                              "--columns=time,change,ref,installation,remote,user,tool"},
                             [done](const std::string &out, int) {
                               std::vector<HistoryEntry> h;
                               parse_flatpak_history(out, h);
                               done(h);
                             });
    if (!started) done({});
  }

  void setup(const std::string &what, DoneFn done) override {
    if (what != "flathub") {
      done(false, false, "unknown setup");
      return;
    }
    std::string url = env("MERKADU_FLATHUB_URL", "https://dl.flathub.org/repo/flathub.flatpakrepo");
    background<std::string>(
        [url] {
          int status = -1;
          std::string repo = run_sync({"curl", "-fsSL", "--max-time", "60", "--proto", "=https", url}, &status, 70000);
          if (status != 0 || repo.find("[Flatpak Repo]") == std::string::npos)
            return std::string(_("The Flathub repository file could not be downloaded."));
          GBytes *bytes = g_bytes_new(repo.data(), repo.size());
          GError *err = nullptr;
          FlatpakRemote *remote = flatpak_remote_new_from_file("flathub", bytes, &err);
          g_bytes_unref(bytes);
          std::string error;
          FlatpakInstallation *inst = remote ? open_installation("system", &error) : nullptr;
          if (remote && inst && flatpak_installation_add_remote(inst, remote, TRUE, nullptr, &err))
            flatpak_installation_update_appstream_full_sync(inst, "flathub", nullptr, nullptr, nullptr, nullptr,
                                                            nullptr, nullptr);
          if (err) error = err->message;
          g_clear_error(&err);
          if (remote) g_object_unref(remote);
          if (inst) g_object_unref(inst);
          return error;
        },
        [done](std::string error) { done(error.empty(), false, error); });
  }

 private:
  static void load_catalog_again();
  bool refreshed_ = false;
  std::mutex mutex_;
  std::map<int, GCancellable *> running_;
};

}  // namespace

}  // namespace merkadu
}  // namespace sde

#include "store.h"

namespace sde {
namespace merkadu {
namespace {
void FlatpakBackend::load_catalog_again() { Store::get().reload_catalog(); }
}  // namespace

std::unique_ptr<Backend> make_flatpak_backend() { return std::unique_ptr<Backend>(new FlatpakBackend()); }

}  // namespace merkadu
}  // namespace sde

#else  // without libflatpak

namespace sde {
namespace merkadu {

namespace {

class NoFlatpak : public Backend {
 public:
  Source source() const override { return Source::Flatpak; }
  void probe(std::function<void(SourceStatus)> done) override {
    SourceStatus st;
    st.message = _("This build of Merkadu has no Flatpak support");
    done(st);
  }
  void load_installed(AppsFn done) override { done({}, ""); }
  void run(const Job &, std::function<void(const Progress &)>, DoneFn done) override {
    done(false, false, _("This build of Merkadu has no Flatpak support"));
  }
};

}  // namespace

std::unique_ptr<Backend> make_flatpak_backend() { return std::unique_ptr<Backend>(new NoFlatpak()); }

}  // namespace merkadu
}  // namespace sde

#endif
