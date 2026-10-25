// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Debian source: PackageKit's D-Bus API (org.freedesktop.PackageKit on
// the system bus, its APT backend on Debian). Every request is a
// transaction object: CreateTransaction, SetHints (the locale, and
// "interactive=true" so that polkit asks for the password through the SDE
// polkit agent), the method, then its signals (Package, Details, ErrorCode,
// Finished) and properties (Percentage, Status, AllowCancel). Everything
// runs on the FLTK loop through sde::dbus; nothing blocks the window.
//
// Also here: the catalog loader (libappstream on a worker thread), the APT
// history (/var/log/apt/history.log and its rotated .gz files) and dpkg
// queries (which package owns a launcher, installed sizes).
//
// MERKADU_PACKAGEKIT_BUS=session talks to a PackageKit on the session bus
// (the tests' fake service); MERKADU_APT_HISTORY replaces the log path.
#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

#include "appstream_catalog.h"
#include "backend.h"
#include "dbus.h"
#include "i18n.h"
#include "pkenum.h"
#include "process.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

const char *kService = "org.freedesktop.PackageKit";
const char *kPath = "/org/freedesktop/PackageKit";
const char *kTransaction = "org.freedesktop.PackageKit.Transaction";

dbus::Bus *pk_bus() {
  return env("MERKADU_PACKAGEKIT_BUS") == "session" ? dbus::Bus::session() : dbus::Bus::system();
}

// One PackageKit transaction and what it reports.
struct Tx {
  std::string path;
  std::function<void(unsigned info, const std::string &id, const std::string &summary)> on_package;
  std::function<void(const dbus::Value &details)> on_details;
  std::function<void(int percent, unsigned status, bool cancellable)> on_progress;
  // exit: pk::FINISHED_*; error/code: the last ErrorCode.
  std::function<void(unsigned exit, unsigned code, const std::string &error)> on_finished;
  unsigned error_code = 0;
  std::string error;
  int percent = -1;
  unsigned status = 0;
  bool cancellable = false;
  bool finished = false;
};

std::map<std::string, std::shared_ptr<Tx>> g_tx;
bool g_watching = false;

void finish_tx(const std::shared_ptr<Tx> &t, unsigned exit) {
  if (t->finished) return;
  t->finished = true;
  g_tx.erase(t->path);
  if (t->on_finished) t->on_finished(exit, t->error_code, t->error);
}

void watch_signals() {
  if (g_watching) return;
  dbus::Bus *bus = pk_bus();
  if (!bus) return;
  g_watching = true;
  auto ours = [](DBusMessage *m) {
    const char *path = dbus_message_get_path(m);
    return path && g_tx.count(path);
  };
  bus->add_signal_handler(std::string("type='signal',sender='") + kService + "',interface='" + kTransaction + "'",
                          ours, [](DBusMessage *m) {
                            auto it = g_tx.find(dbus_message_get_path(m));
                            if (it == g_tx.end()) return;
                            std::shared_ptr<Tx> t = it->second;
                            std::string member = dbus_message_get_member(m) ? dbus_message_get_member(m) : "";
                            auto args = dbus::read_args(m);
                            if (member == "Package" && args.size() >= 3) {
                              if (t->on_package) t->on_package(unsigned(args[0].num()), args[1].str(), args[2].str());
                            } else if (member == "Packages" && !args.empty()) {
                              for (auto &p : args[0].items)
                                if (p.items.size() >= 3 && t->on_package)
                                  t->on_package(unsigned(p.items[0].num()), p.items[1].str(), p.items[2].str());
                            } else if (member == "Details" && !args.empty()) {
                              if (t->on_details) t->on_details(args[0]);
                            } else if (member == "ErrorCode" && args.size() >= 2) {
                              t->error_code = unsigned(args[0].num());
                              t->error = args[1].str();
                            } else if (member == "Finished" && !args.empty()) {
                              finish_tx(t, unsigned(args[0].num()));
                            } else if (member == "Destroy") {
                              finish_tx(t, pk::FINISHED_FAILED);
                            }
                          });
  bus->add_signal_handler(std::string("type='signal',sender='") + kService +
                              "',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                          ours, [](DBusMessage *m) {
                            auto it = g_tx.find(dbus_message_get_path(m));
                            if (it == g_tx.end()) return;
                            std::shared_ptr<Tx> t = it->second;
                            auto args = dbus::read_args(m);
                            if (args.size() < 2) return;
                            const dbus::Value &changed = args[1];
                            bool any = false;
                            if (const dbus::Value *v = changed.get("Percentage")) {
                              unsigned p = unsigned(v->num());
                              t->percent = p > 100 ? -1 : int(p);  // 101: unknown
                              any = true;
                            }
                            if (const dbus::Value *v = changed.get("Status")) {
                              t->status = unsigned(v->num());
                              any = true;
                            }
                            if (const dbus::Value *v = changed.get("AllowCancel")) {
                              t->cancellable = v->boolean();
                              any = true;
                            }
                            if (any && t->on_progress) t->on_progress(t->percent, t->status, t->cancellable);
                          });
}

std::string pk_locale() {
  for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    std::string v = env(var);
    if (!v.empty()) return v;
  }
  return "C";
}

// Creates a transaction, sets its hints and calls |method| with the
// arguments |args| appends. |t| receives the signals; on_finished always
// runs once (also when PackageKit cannot be reached).
void pk_call(const std::string &method, std::function<void(DBusMessage *)> args, std::shared_ptr<Tx> t) {
  dbus::Bus *bus = pk_bus();
  if (!bus) {
    t->error = _("PackageKit cannot be reached: there is no connection to the system bus.");
    finish_tx(t, pk::FINISHED_FAILED);
    return;
  }
  watch_signals();
  DBusMessage *create = dbus::method(kService, kPath, kService, "CreateTransaction");
  bus->call_async(
      create,
      [bus, method, args, t](DBusMessage *reply) {
        auto r = reply ? dbus::read_args(reply) : std::vector<dbus::Value>();
        if (r.empty() || r[0].str().empty()) {
          t->error = _("PackageKit is not running or not installed.");
          t->finished = true;
          if (t->on_finished) t->on_finished(pk::FINISHED_FAILED, 0, t->error);
          return;
        }
        t->path = r[0].str();
        g_tx[t->path] = t;
        DBusMessage *hints = dbus::method(kService, t->path, kTransaction, "SetHints");
        {
          dbus::Writer w(hints);
          w.strings({"locale=" + pk_locale(), "interactive=true", "background=false"});
        }
        bus->send(hints);
        DBusMessage *call = dbus::method(kService, t->path, kTransaction, method);
        args(call);
        bus->call_async_any(
            call,
            [t](DBusMessage *rep) {
              if (rep && dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
                // Refused before it started (e.g. by polkit).
                const char *name = dbus_message_get_error_name(rep);
                auto a = dbus::read_args(rep);
                t->error = !a.empty() ? a[0].str() : name ? name : "error";
                if (name && std::string(name).find("Refused") != std::string::npos) t->error_code = pk::ERROR_NOT_AUTHORIZED;
                finish_tx(t, pk::FINISHED_FAILED);
              }
            },
            25000);
      },
      10000);
}

void append_u64(DBusMessage *m, uint64_t v) {
  if (m) dbus_message_append_args(m, DBUS_TYPE_UINT64, &v, DBUS_TYPE_INVALID);
}

void append_bool(DBusMessage *m, bool b) {
  dbus_bool_t v = b;
  if (m) dbus_message_append_args(m, DBUS_TYPE_BOOLEAN, &v, DBUS_TYPE_INVALID);
}

void append_strings(DBusMessage *m, const std::vector<std::string> &v) {
  dbus::Writer w(m);
  w.strings(v);
}

// Package ids for |names|: the installed ones, or the newest available.
void resolve(const std::vector<std::string> &names, bool installed,
             std::function<void(std::map<std::string, std::string> ids, std::string error)> done) {
  auto t = std::make_shared<Tx>();
  auto ids = std::make_shared<std::map<std::string, std::string>>();
  t->on_package = [ids, installed](unsigned info, const std::string &id, const std::string &) {
    PackageId p = parse_package_id(id);
    if (!p.valid || p.installed() != installed) return;
    (*ids)[p.name] = id;
  };
  t->on_finished = [ids, done](unsigned exit, unsigned code, const std::string &error) {
    done(*ids, exit == pk::FINISHED_SUCCESS ? "" : pk::error_text(code, error));
  };
  uint64_t filter = installed ? pk::bit(pk::FILTER_INSTALLED)
                              : pk::bit(pk::FILTER_NOT_INSTALLED) | pk::bit(pk::FILTER_NEWEST) |
                                    pk::bit(pk::FILTER_ARCH);
  pk_call("Resolve", [filter, names](DBusMessage *m) {
    append_u64(m, filter);
    append_strings(m, names);
  }, t);
}

// Reads a text file, gzip-compressed or not.
bool read_maybe_gz(const std::string &path, std::string &out) {
  gzFile f = gzopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[65536];
  int n;
  out.clear();
  while ((n = gzread(f, buf, sizeof buf)) > 0) {
    out.append(buf, size_t(n));
    if (out.size() > 256u * 1024 * 1024) break;
  }
  gzclose(f);
  return true;
}

class PackageKitBackend : public Backend {
 public:
  Source source() const override { return Source::Debian; }

  void probe(std::function<void(SourceStatus)> done) override {
    dbus::Bus *bus = pk_bus();
    if (!bus) {
      SourceStatus st;
      st.message = _("There is no connection to the system bus.");
      done(st);
      return;
    }
    dbus::get_all_properties(bus, kService, kPath, kService, [done](const dbus::Value &props) {
      SourceStatus st;
      if (!props.valid()) {
        st.message = _("PackageKit is not running or not installed.");
        done(st);
        return;
      }
      st.available = st.installed = true;
      std::string backend = props.get("BackendName") ? props.get("BackendName")->str() : "";
      int64_t ma = props.get("VersionMajor") ? props.get("VersionMajor")->num() : 0;
      int64_t mi = props.get("VersionMinor") ? props.get("VersionMinor")->num() : 0;
      int64_t mc = props.get("VersionMicro") ? props.get("VersionMicro")->num() : 0;
      st.message = format("PackageKit %lld.%lld.%lld", (long long)ma, (long long)mi, (long long)mc);
      if (!backend.empty()) st.message += " (" + backend + ")";
      done(st);
    });
  }

  void load_installed(AppsFn done) override {
    auto installed = std::make_shared<std::map<std::string, Origin>>();
    auto t = std::make_shared<Tx>();
    t->on_package = [installed](unsigned info, const std::string &id, const std::string &) {
      PackageId p = parse_package_id(id);
      if (!p.valid) return;
      Origin &o = (*installed)[p.name];
      o.source = Source::Debian;
      o.id = p.name;
      o.installed = true;
      o.installed_version = p.version;
      o.package_id = id;
    };
    t->on_finished = [this, installed, done](unsigned exit, unsigned code, const std::string &error) {
      if (exit != pk::FINISHED_SUCCESS) {
        done({}, pk::error_text(code, error));
        return;
      }
      load_updates(installed, done);
    };
    pk_call("GetPackages", [](DBusMessage *m) { append_u64(m, pk::bit(pk::FILTER_INSTALLED)); }, t);
  }

  // Marks the packages with updates, then hands everything over.
  void load_updates(std::shared_ptr<std::map<std::string, Origin>> installed, AppsFn done) {
    auto t = std::make_shared<Tx>();
    t->on_package = [this, installed](unsigned info, const std::string &id, const std::string &) {
      PackageId p = parse_package_id(id);
      if (!p.valid || info == pk::INFO_BLOCKED) return;
      auto it = installed->find(p.name);
      if (it == installed->end()) return;
      it->second.update = true;
      it->second.version = p.version;
      updates_[p.name] = id;
    };
    t->on_finished = [installed, done](unsigned, unsigned, const std::string &) {
      std::vector<App> apps;
      apps.reserve(installed->size());
      for (auto &p : *installed) {
        App a;
        a.id = "deb:" + p.first;
        a.name = p.first;
        a.origins.push_back(p.second);
        apps.push_back(std::move(a));
      }
      done(std::move(apps), "");
    };
    updates_.clear();
    pk_call("GetUpdates", [](DBusMessage *m) { append_u64(m, pk::bit(pk::FILTER_NONE)); }, t);
  }

  void details(const App &app, const Origin &o, std::function<void(Origin)> done) override {
    if (!valid_package_name(o.id)) return;
    auto fetch = [done](const std::string &id, Origin base) {
      auto t = std::make_shared<Tx>();
      auto found = std::make_shared<Origin>(base);
      t->on_details = [found](const dbus::Value &d) {
        long long size = d.get("size") ? d.get("size")->num() : -1;
        long long dl = d.get("download-size") ? d.get("download-size")->num() : -1;
        if (const dbus::Value *lic = d.get("license"))
          if (!lic->str().empty() && lic->str() != "unknown") found->license = lic->str();
        if (found->installed) {
          if (size > 0) found->installed_size = size;
        } else if (dl > 0) {
          found->download_size = dl;
          if (size > 0) found->installed_size = size;
        } else if (size > 0) {
          found->download_size = size;
        }
      };
      t->on_finished = [found, done](unsigned exit, unsigned, const std::string &) {
        if (exit == pk::FINISHED_SUCCESS) done(*found);
      };
      pk_call("GetDetails", [id](DBusMessage *m) { append_strings(m, {id}); }, t);
    };
    if (!o.package_id.empty()) {
      fetch(o.package_id, o);
      return;
    }
    Origin base = o;
    resolve({o.id}, false, [fetch, base](std::map<std::string, std::string> ids, std::string) {
      auto it = ids.find(base.id);
      if (it == ids.end()) return;
      Origin withv = base;
      withv.version = parse_package_id(it->second).version;
      withv.package_id = it->second;
      fetch(it->second, withv);
    });
  }

  void run(const Job &job, std::function<void(const Progress &)> progress, DoneFn done) override {
    for (auto &p : job.targets)
      if (!valid_package_name(p)) {
        done(false, false, format(_("\"%s\" is not a package name."), p.c_str()));
        return;
      }
    int id = job.id;
    Action action = job.action;
    auto finish = [this, id, done](unsigned exit, unsigned code, const std::string &error) {
      running_.erase(id);
      if (exit == pk::FINISHED_SUCCESS) done(true, false, "");
      else if (exit == pk::FINISHED_CANCELLED || pk::error_is_cancel(code)) done(false, true, "");
      else done(false, false, pk::error_text(code, error));
    };
    auto start = [this, id, action, progress, finish](std::vector<std::string> ids) {
      auto t = std::make_shared<Tx>();
      t->on_progress = [progress](int percent, unsigned status, bool cancellable) {
        Progress p;
        p.percent = percent;
        p.status = pk::status_text(status);
        p.cancellable = cancellable;
        progress(p);
      };
      t->on_finished = finish;
      running_[id] = t;
      if (action == Action::Remove) {
        pk_call("RemovePackages", [ids](DBusMessage *m) {
          append_u64(m, 0);
          append_strings(m, ids);
          append_bool(m, true);   // allow_deps: the user saw what goes with it
          append_bool(m, false);  // autoremove: leave other dependencies alone
        }, t);
      } else {
        std::string method = action == Action::Update ? "UpdatePackages" : "InstallPackages";
        pk_call(method, [ids](DBusMessage *m) {
          append_u64(m, pk::bit(pk::FLAG_ONLY_TRUSTED));
          append_strings(m, ids);
        }, t);
      }
    };
    // Package ids: the installed ones to remove, the new versions to
    // update (known from GetUpdates), the newest available to install.
    if (action == Action::Update) {
      std::vector<std::string> ids;
      for (auto &p : job.targets) {
        auto it = updates_.find(p);
        if (it != updates_.end()) ids.push_back(it->second);
      }
      if (ids.empty()) {
        done(false, false, _("There is no update for it any more."));
        return;
      }
      start(ids);
      return;
    }
    std::vector<std::string> names = job.targets;
    resolve(names, action == Action::Remove,
            [names, start, done](std::map<std::string, std::string> found, std::string error) {
              std::vector<std::string> ids;
              for (auto &n : names) {
                auto it = found.find(n);
                if (it == found.end()) {
                  done(false, false, error.empty() ? format(_("The package %s was not found."), n.c_str()) : error);
                  return;
                }
                ids.push_back(it->second);
              }
              start(ids);
            });
  }

  void cancel(int job_id) override {
    auto it = running_.find(job_id);
    if (it == running_.end() || it->second->path.empty()) return;
    pk_bus()->call_async(dbus::method(kService, it->second->path, kTransaction, "Cancel"), nullptr);
  }

  void plan_remove(const std::vector<std::string> &packages,
                   std::function<void(std::vector<std::string>, long long, std::string)> done) override {
    std::vector<std::string> names = packages;
    resolve(names, true, [names, done](std::map<std::string, std::string> found, std::string error) {
      std::vector<std::string> ids;
      for (auto &n : names)
        if (found.count(n)) ids.push_back(found[n]);
      if (ids.empty()) {
        done({}, -1, error.empty() ? std::string(_("It is not installed.")) : error);
        return;
      }
      auto removed = std::make_shared<std::vector<std::string>>();
      auto t = std::make_shared<Tx>();
      t->on_package = [removed](unsigned info, const std::string &id, const std::string &) {
        PackageId p = parse_package_id(id);
        if (p.valid && pk::info_removes(info) &&
            std::find(removed->begin(), removed->end(), p.name) == removed->end())
          removed->push_back(p.name);
      };
      t->on_finished = [removed, names, done](unsigned exit, unsigned code, const std::string &error) {
        if (exit != pk::FINISHED_SUCCESS) {
          done({}, -1, pk::error_text(code, error));
          return;
        }
        if (removed->empty()) *removed = names;
        // The space they take: dpkg's Installed-Size of each (KiB).
        std::vector<std::string> argv = {"dpkg-query", "-W", "-f=${Installed-Size}\\n"};
        for (auto &p : *removed) argv.push_back(p);
        auto list = *removed;
        if (!run_async(argv, [list, done](const std::string &out, int) { done(list, parse_installed_sizes(out), ""); }))
          done(list, -1, "");
      };
      pk_call("RemovePackages", [ids](DBusMessage *m) {
        append_u64(m, pk::bit(pk::FLAG_SIMULATE));
        append_strings(m, ids);
        append_bool(m, true);
        append_bool(m, false);
      }, t);
    });
  }

  void owners(const std::vector<std::string> &paths, OwnersFn done) override {
    if (paths.empty()) {
      done({});
      return;
    }
    std::vector<std::string> argv = {"dpkg-query", "-S", "--"};
    for (auto &p : paths) argv.push_back(p);
    std::vector<std::string> list = paths;
    bool started = run_async(argv, [list, done](const std::string &out, int) {
      std::map<std::string, std::vector<std::string>> found;
      for (auto &p : list) {
        auto pkgs = parse_dpkg_owners(out, p);
        if (!pkgs.empty()) found[p] = pkgs;
      }
      done(found);
    });
    if (!started) done({});
  }

  void history(std::function<void(std::vector<HistoryEntry>)> done) override {
    background<std::vector<HistoryEntry>>(
        [] {
          std::vector<HistoryEntry> out;
          std::string log = env("MERKADU_APT_HISTORY", "/var/log/apt/history.log");
          // Rotated logs: history.log.1.gz (newest) ... history.log.12.gz.
          for (int i = 12; i >= 1; --i) {
            std::string text;
            if (read_maybe_gz(log + "." + std::to_string(i) + ".gz", text) ||
                read_maybe_gz(log + "." + std::to_string(i), text))
              parse_apt_history(text, out);
          }
          std::string text;
          if (read_maybe_gz(log, text)) parse_apt_history(text, out);
          return out;
        },
        done);
  }

 private:
  std::map<int, std::shared_ptr<Tx>> running_;
  std::map<std::string, std::string> updates_;  // package -> package id of its update
};

}  // namespace

std::unique_ptr<Backend> make_packagekit_backend() { return std::unique_ptr<Backend>(new PackageKitBackend()); }

void load_catalog(CatalogFn done) {
  background<CatalogData>([] { return load_appstream(); },
                          [done](CatalogData data) { done(std::move(data.apps), data.error); });
}

}  // namespace merkadu
}  // namespace sde
