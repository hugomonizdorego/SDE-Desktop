// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The packagekit backend: PackageKit's D-Bus API on the system bus
// (DEBRUN_PACKAGEKIT_BUS=session in the tests). Each change is one
// transaction: CreateTransaction, SetHints ("interactive=true": polkit asks
// through the SDE agent; local files need org.freedesktop.packagekit.
// package-install-untrusted), the method, then its ErrorCode and Finished
// signals and the Percentage and Status properties. The file is hashed
// again first: what PackageKit installs is the file Debrun showed. The
// numbers and texts of PackageKit's enums come from Merkadu (pkenum.h).
#include <fcntl.h>
#include <unistd.h>

#include <map>
#include <memory>

#include "async.h"
#include "backend.h"
#include "dbus.h"
#include "deb.h"
#include "i18n.h"
#include "../merkadu/pkenum.h"
#include "sha256.h"
#include "util.h"

namespace sde {
namespace debrun {

namespace pk = merkadu::pk;

namespace {

const char *kService = "org.freedesktop.PackageKit";
const char *kPath = "/org/freedesktop/PackageKit";
const char *kTransaction = "org.freedesktop.PackageKit.Transaction";

dbus::Bus *bus() { return env("DEBRUN_PACKAGEKIT_BUS") == "session" ? dbus::Bus::session() : dbus::Bus::system(); }

struct Tx {
  std::string path;
  std::function<void(const std::string &id)> on_package;
  std::function<void(int percent, unsigned status)> on_progress;
  std::function<void(unsigned exit, unsigned code, const std::string &error)> on_finished;
  unsigned error_code = 0;
  std::string error;
  int percent = -1;
  unsigned status = 0;
  bool finished = false;
};

std::map<std::string, std::shared_ptr<Tx>> g_tx;

void finish_tx(const std::shared_ptr<Tx> &t, unsigned exit) {
  if (t->finished) return;
  t->finished = true;
  g_tx.erase(t->path);
  if (t->on_finished) t->on_finished(exit, t->error_code, t->error);
}

void watch() {
  static bool watching = false;
  dbus::Bus *b = bus();
  if (watching || !b) return;
  watching = true;
  auto ours = [](DBusMessage *m) {
    const char *path = dbus_message_get_path(m);
    return path && g_tx.count(path);
  };
  b->add_signal_handler(std::string("type='signal',sender='") + kService + "',interface='" + kTransaction + "'",
                        ours, [](DBusMessage *m) {
                          auto it = g_tx.find(dbus_message_get_path(m));
                          if (it == g_tx.end()) return;
                          std::shared_ptr<Tx> t = it->second;
                          std::string member = dbus_message_get_member(m) ? dbus_message_get_member(m) : "";
                          auto args = dbus::read_args(m);
                          if (member == "Package" && args.size() >= 2) {
                            if (t->on_package) t->on_package(args[1].str());
                          } else if (member == "ErrorCode" && args.size() >= 2) {
                            t->error_code = unsigned(args[0].num());
                            t->error = args[1].str();
                          } else if (member == "Finished" && !args.empty()) {
                            finish_tx(t, unsigned(args[0].num()));
                          } else if (member == "Destroy") {
                            finish_tx(t, pk::FINISHED_FAILED);
                          }
                        });
  b->add_signal_handler(std::string("type='signal',sender='") + kService +
                            "',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                        ours, [](DBusMessage *m) {
                          auto it = g_tx.find(dbus_message_get_path(m));
                          if (it == g_tx.end()) return;
                          std::shared_ptr<Tx> t = it->second;
                          auto args = dbus::read_args(m);
                          if (args.size() < 2) return;
                          if (const dbus::Value *v = args[1].get("Percentage")) {
                            unsigned p = unsigned(v->num());
                            t->percent = p > 100 ? -1 : int(p);
                          }
                          if (const dbus::Value *v = args[1].get("Status")) t->status = unsigned(v->num());
                          if (t->on_progress) t->on_progress(t->percent, t->status);
                        });
}

std::string locale() {
  for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"})
    if (!env(var).empty()) return env(var);
  return "C";
}

// CreateTransaction, SetHints, then |method| with the arguments |args|
// appends; on_finished always runs once.
void call(const std::string &method, std::function<void(DBusMessage *)> args, std::shared_ptr<Tx> t) {
  dbus::Bus *b = bus();
  if (!b) {
    t->error = _("PackageKit cannot be reached: there is no connection to the system bus.");
    finish_tx(t, pk::FINISHED_FAILED);
    return;
  }
  watch();
  b->call_async(
      dbus::method(kService, kPath, kService, "CreateTransaction"),
      [b, method, args, t](DBusMessage *reply) {
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
        dbus::Writer(hints).strings({"locale=" + locale(), "interactive=true", "background=false"});
        b->send(hints);
        DBusMessage *m = dbus::method(kService, t->path, kTransaction, method);
        args(m);
        b->call_async_any(
            m,
            [t](DBusMessage *rep) {
              if (rep && dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
                const char *name = dbus_message_get_error_name(rep);
                auto a = dbus::read_args(rep);
                t->error = !a.empty() ? a[0].str() : name ? name : "error";
                if (name && std::string(name).find("Refused") != std::string::npos)
                  t->error_code = pk::ERROR_NOT_AUTHORIZED;
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

void append_bool(DBusMessage *m, bool v) {
  dbus_bool_t b = v;
  if (m) dbus_message_append_args(m, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID);
}

std::string phase_of(unsigned status) {
  switch (status) {
    case pk::STATUS_DOWNLOAD: return "download";
    case pk::STATUS_INSTALL:
    case pk::STATUS_UPDATE: return "unpack";
    case pk::STATUS_REMOVE: return "remove";
    case pk::STATUS_WAITING_FOR_LOCK: return "wait";
    case pk::STATUS_WAITING_FOR_AUTH: return "auth";
    case pk::STATUS_REFRESH_CACHE: return "refresh";
    case pk::STATUS_CLEANUP:
    case pk::STATUS_COMMIT: return "configure";
    default: return "check";
  }
}

class PackageKitBackend : public Backend {
 public:
  std::string name() const override { return "PackageKit"; }
  bool running() const override { return running_; }

  void install(const Package &p, bool, bool, const std::vector<std::string> &, Callbacks cb) override {
    running_ = true;
    cb_ = std::move(cb);
    Step s;
    s.phase = "check";
    if (cb_.step) cb_.step(s);
    // The file must still be the one Debrun showed.
    std::string path = p.path, want = p.sha256;
    background<std::string>(
        [path] {
          int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
          if (fd < 0) return std::string();
          std::string h = sha256_fd_hex(fd);
          close(fd);
          return h;
        },
        [this, path, want](std::string &digest) {
          if (digest != want) {
            Outcome o;
            o.code = "changed";
            o.message = error_message("changed", "");
            done(o);
            return;
          }
          start("InstallFiles", [path](DBusMessage *m) {
            append_u64(m, 0);  // a local file is not "trusted": polkit asks
            dbus::Writer(m).strings({path});
          });
        });
  }

  void remove(const std::string &name, const std::vector<std::string> &, Callbacks cb) override {
    running_ = true;
    cb_ = std::move(cb);
    auto ids = std::make_shared<std::vector<std::string>>();
    auto t = std::make_shared<Tx>();
    t->on_package = [ids, name](const std::string &id) {
      if (id.substr(0, id.find(';')) == name) ids->push_back(id);
    };
    t->on_finished = [this, ids](unsigned exit, unsigned code, const std::string &error) {
      if (exit != pk::FINISHED_SUCCESS || ids->empty()) {
        Outcome o;
        o.code = "failed";
        o.message = ids->empty() ? std::string(_("The package is not installed.")) : pk::error_text(code, error);
        done(o);
        return;
      }
      std::vector<std::string> list = *ids;
      start("RemovePackages", [list](DBusMessage *m) {
        append_u64(m, 0);
        dbus::Writer(m).strings(list);
        append_bool(m, true);   // allow_deps: the user saw what goes with it
        append_bool(m, false);  // autoremove
      });
    };
    call("Resolve", [name](DBusMessage *m) {
      append_u64(m, pk::bit(pk::FILTER_INSTALLED));
      dbus::Writer(m).strings({name});
    }, t);
  }

  void refresh(Callbacks cb) override {
    running_ = true;
    cb_ = std::move(cb);
    start("RefreshCache", [](DBusMessage *m) { append_bool(m, false); });
  }

 private:
  void start(const std::string &method, std::function<void(DBusMessage *)> args) {
    auto t = std::make_shared<Tx>();
    t->on_progress = [this](int percent, unsigned status) {
      Step s;
      s.percent = percent;
      s.phase = phase_of(status);
      if (cb_.step) cb_.step(s);
    };
    t->on_finished = [this](unsigned exit, unsigned code, const std::string &error) {
      Outcome o;
      o.ok = exit == pk::FINISHED_SUCCESS;
      if (!o.ok) {
        o.cancelled = exit == pk::FINISHED_CANCELLED || pk::error_is_cancel(code);
        o.code = code == pk::ERROR_NOT_AUTHORIZED          ? "auth"
                 : code == pk::ERROR_CANNOT_GET_LOCK        ? "lock"
                 : code == pk::ERROR_DEP_RESOLUTION_FAILED  ? "unmet"
                 : code == pk::ERROR_NO_SPACE_ON_DEVICE     ? "space"
                                                            : "failed";
        o.message = pk::error_text(code, error);
        o.detail = error;
      }
      done(o);
    };
    call(method, std::move(args), t);
  }

  void done(const Outcome &o) {
    running_ = false;
    Callbacks cb = std::move(cb_);
    cb_ = Callbacks();
    if (cb.done) cb.done(o);
  }

  Callbacks cb_;
  bool running_ = false;
};

}  // namespace

std::unique_ptr<Backend> make_packagekit_backend() { return std::unique_ptr<Backend>(new PackageKitBackend()); }

}  // namespace debrun
}  // namespace sde
