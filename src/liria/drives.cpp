// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "drives.h"

#include <gio/gio.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <map>
#include <set>

#include "dbus.h"
#include "file_chooser_model.h"
#include "i18n.h"
#include "location.h"
#include "util.h"

namespace sde {
namespace liria {

namespace {

std::string take(gchar *s) {
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

std::vector<std::string> icon_names(GIcon *icon) {
  std::vector<std::string> out;
  if (!icon) return out;
  if (G_IS_THEMED_ICON(icon)) {
    const gchar *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    for (int i = 0; names && names[i]; ++i) out.push_back(names[i]);
  } else if (G_IS_FILE_ICON(icon)) {
    GFile *f = g_file_icon_get_file(G_FILE_ICON(icon));
    std::string p = take(g_file_get_path(f));
    if (!p.empty()) out.push_back(p);
  }
  return out;
}

void usage(Drive &d) {
  if (d.path.empty() || d.network) return;
  struct statvfs v {};
  if (statvfs(d.path.c_str(), &v) == 0 && v.f_blocks > 0) {
    d.total = (long long)v.f_blocks * (long long)v.f_frsize;
    d.free = (long long)v.f_bavail * (long long)v.f_frsize;
  }
}

std::string error_text(GError *e, const std::string &uri) {
  if (!e) return "";
  std::string host;
  if (!uri.empty()) {
    gchar *h = nullptr;
    if (g_uri_split(uri.c_str(), G_URI_FLAGS_NON_DNS, nullptr, nullptr, &h, nullptr, nullptr, nullptr, nullptr, nullptr))
      host = take(h);
  }
  if (e->domain == G_IO_ERROR) {
    switch (e->code) {
      case G_IO_ERROR_FAILED_HANDLED:
      case G_IO_ERROR_CANCELLED:
        return "";
      case G_IO_ERROR_HOST_NOT_FOUND:
        return host.empty() ? std::string(_("The computer was not found on the network."))
                            : format(_("No computer called “%s” was found on the network."), host.c_str());
      case G_IO_ERROR_PERMISSION_DENIED:
        return _("Access was denied: the user name or the password was not accepted, or this account may not use it.");
      case G_IO_ERROR_TIMED_OUT:
        return host.empty() ? std::string(_("The server does not answer."))
                            : format(_("“%s” does not answer."), host.c_str());
      case G_IO_ERROR_NOT_FOUND:
        return _("There is no such share or folder on the server.");
      case G_IO_ERROR_NOT_SUPPORTED:
        return format(_("This computer cannot open such places. They need the package %s; ask your administrator "
                        "to install it."),
                      "gvfs-backends");
      case G_IO_ERROR_BUSY:
        return _("The drive is in use. Close the files and programs that use it, then try again.");
      default:
        break;
    }
  }
  return e->message ? e->message : _("The operation failed.");
}

// One GMountOperation with Liria's dialogs behind it.
struct OpCtx {
  MountQuestions q;
  GMountOperation *op = nullptr;
  Drives::Done done;
  std::string uri;
  GFile *file = nullptr;  // mount_location
};

void reply_password(GMountOperation *op, const SignIn &s) {
  if (!s.ok) {
    g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
  } else {
    g_mount_operation_set_anonymous(op, s.anonymous);
    if (!s.anonymous) {
      g_mount_operation_set_username(op, s.user.c_str());
      if (!s.domain.empty()) g_mount_operation_set_domain(op, s.domain.c_str());
      g_mount_operation_set_password(op, s.password.c_str());
      g_mount_operation_set_password_save(op, s.remember ? G_PASSWORD_SAVE_PERMANENTLY : G_PASSWORD_SAVE_NEVER);
    }
    g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
  }
  g_object_unref(op);
}

void on_ask_password(GMountOperation *op, const gchar *message, const gchar *user, const gchar *domain,
                     GAskPasswordFlags flags, gpointer data) {
  auto *ctx = static_cast<OpCtx *>(data);
  if (!ctx->q.password) {
    g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
    return;
  }
  g_object_ref(op);
  ctx->q.password(message ? message : "", user ? user : "", domain ? domain : "", int(flags),
                  Drives::keyring_available(), [op](const SignIn &s) { reply_password(op, s); });
}

void ask_choices(GMountOperation *op, const gchar *message, const gchar *const *choices, OpCtx *ctx) {
  std::vector<std::string> list;
  for (int i = 0; choices && choices[i]; ++i) list.push_back(choices[i]);
  if (!ctx->q.question || list.empty()) {
    g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
    return;
  }
  g_object_ref(op);
  ctx->q.question(message ? message : "", list, [op](int choice) {
    if (choice < 0) {
      g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
    } else {
      g_mount_operation_set_choice(op, choice);
      g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
    }
    g_object_unref(op);
  });
}

void on_ask_question(GMountOperation *op, const gchar *message, const gchar **choices, gpointer data) {
  ask_choices(op, message, choices, static_cast<OpCtx *>(data));
}

void on_show_processes(GMountOperation *op, const gchar *message, GArray *, const gchar **choices, gpointer data) {
  ask_choices(op, message, choices, static_cast<OpCtx *>(data));
}

OpCtx *new_ctx(MountQuestions q, Drives::Done done) {
  auto *ctx = new OpCtx;
  ctx->q = std::move(q);
  ctx->done = std::move(done);
  ctx->op = g_mount_operation_new();
  g_signal_connect(ctx->op, "ask-password", G_CALLBACK(on_ask_password), ctx);
  g_signal_connect(ctx->op, "ask-question", G_CALLBACK(on_ask_question), ctx);
  g_signal_connect(ctx->op, "show-processes", G_CALLBACK(on_show_processes), ctx);
  return ctx;
}

void finish_ctx(OpCtx *ctx, bool ok, const std::string &error, const std::string &path) {
  Drives::Done done = std::move(ctx->done);
  if (ctx->op) {
    g_signal_handlers_disconnect_by_data(ctx->op, ctx);
    g_object_unref(ctx->op);
  }
  if (ctx->file) g_object_unref(ctx->file);
  delete ctx;
  if (done) done(ok, error, path);
}

}  // namespace

struct Drives::Impl {
  GVolumeMonitor *monitor = nullptr;
  std::vector<gulong> handlers;
  std::map<std::string, GVolume *> volumes;
  std::map<std::string, GMount *> mounts;
  void clear() {
    for (auto &kv : volumes) g_object_unref(kv.second);
    for (auto &kv : mounts) g_object_unref(kv.second);
    volumes.clear();
    mounts.clear();
  }
};

Drives::Drives() : impl_(new Impl) { refresh(); }

Drives::~Drives() {
  if (impl_->monitor) {
    for (gulong h : impl_->handlers) g_signal_handler_disconnect(impl_->monitor, h);
    g_object_unref(impl_->monitor);
  }
  impl_->clear();
  delete impl_;
}

namespace {
void monitor_changed(GVolumeMonitor *, gpointer, gpointer data) {
  auto *self = static_cast<Drives *>(data);
  self->refresh();
}
}  // namespace

void Drives::start(std::function<void()> changed) {
  changed_ = std::move(changed);
  if (impl_->monitor) return;
  impl_->monitor = g_volume_monitor_get();
  monitor_ = impl_->monitor;
  for (const char *sig : {"volume-added", "volume-removed", "volume-changed", "mount-added", "mount-removed",
                          "mount-changed", "drive-connected", "drive-disconnected", "drive-changed"})
    impl_->handlers.push_back(g_signal_connect(impl_->monitor, sig, G_CALLBACK(monitor_changed), this));
  refresh();
}

void Drives::refresh() {
  std::vector<Drive> drives, network;
  Drive root;
  root.id = "root";
  root.name = _("System");
  root.path = "/";
  root.uri = "file:///";
  root.icons = {"drive-harddisk-system", "drive-harddisk", "drive-harddisk-solidstate"};
  root.mounted = true;
  root.system = true;
  usage(root);
  drives.push_back(root);
  impl_->clear();
  std::set<std::string> seen_paths{"/"};
  if (impl_->monitor) {
    GList *vols = g_volume_monitor_get_volumes(impl_->monitor);
    for (GList *l = vols; l; l = l->next) {
      GVolume *v = G_VOLUME(l->data);
      Drive d;
      std::string key = take(g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UUID));
      if (key.empty()) key = take(g_volume_get_identifier(v, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
      if (key.empty()) key = take(g_volume_get_name(v));
      d.id = "volume:" + key;
      d.name = take(g_volume_get_name(v));
      GIcon *icon = g_volume_get_icon(v);
      d.icons = icon_names(icon);
      if (icon) g_object_unref(icon);
      d.icons.push_back("drive-removable-media");
      d.icons.push_back("drive-harddisk");
      GDrive *drv = g_volume_get_drive(v);
      d.removable = drv && (g_drive_is_removable(drv) || g_drive_is_media_removable(drv));
      if (drv) g_object_unref(drv);
      d.can_eject = g_volume_can_eject(v);
      GMount *m = g_volume_get_mount(v);
      if (m && g_mount_is_shadowed(m)) {
        g_object_unref(m);
        m = nullptr;
      }
      if (m) {
        GFile *r = g_mount_get_root(m);
        d.path = take(g_file_get_path(r));
        d.uri = take(g_file_get_uri(r));
        g_object_unref(r);
        d.mounted = true;
        d.can_unmount = g_mount_can_unmount(m);
        d.can_eject = d.can_eject || g_mount_can_eject(m);
        impl_->mounts[d.id] = m;
      } else {
        d.can_mount = g_volume_can_mount(v);
      }
      if (d.path == "/") {
        if (m) {
          g_object_unref(m);
          impl_->mounts.erase(d.id);
        }
        continue;
      }
      impl_->volumes[d.id] = G_VOLUME(g_object_ref(v));
      if (!d.path.empty()) seen_paths.insert(d.path);
      usage(d);
      drives.push_back(d);
    }
    g_list_free_full(vols, g_object_unref);
    GList *mounts = g_volume_monitor_get_mounts(impl_->monitor);
    for (GList *l = mounts; l; l = l->next) {
      GMount *m = G_MOUNT(l->data);
      if (g_mount_is_shadowed(m)) continue;
      GVolume *v = g_mount_get_volume(m);
      if (v) {
        g_object_unref(v);
        continue;  // listed with its volume
      }
      GFile *r = g_mount_get_root(m);
      Drive d;
      d.uri = take(g_file_get_uri(r));
      d.path = take(g_file_get_path(r));
      g_object_unref(r);
      d.id = "mount:" + d.uri;
      d.name = take(g_mount_get_name(m));
      GIcon *icon = g_mount_get_icon(m);
      d.icons = icon_names(icon);
      if (icon) g_object_unref(icon);
      d.mounted = true;
      d.can_unmount = g_mount_can_unmount(m);
      d.can_eject = g_mount_can_eject(m);
      d.network = !starts_with(d.uri, "file:");
      impl_->mounts[d.id] = G_MOUNT(g_object_ref(m));
      if (d.network) {
        d.icons.push_back("folder-remote");
        d.icons.push_back("network-server");
        network.push_back(d);
      } else {
        if (seen_paths.count(d.path)) continue;
        seen_paths.insert(d.path);
        d.icons.push_back("drive-harddisk");
        usage(d);
        drives.push_back(d);
      }
    }
    g_list_free_full(mounts, g_object_unref);
  } else {
    // Before the volume monitor: what /proc/self/mountinfo says.
    for (auto &p : chooser::mounted_drives()) {
      if (!seen_paths.insert(p.path).second) continue;
      Drive d;
      d.id = "mount:" + path_to_uri(p.path);
      d.name = p.label;
      d.path = p.path;
      d.uri = path_to_uri(p.path);
      d.mounted = true;
      d.removable = p.removable;
      d.network = p.icon == "network";
      d.icons = d.removable ? std::vector<std::string>{"drive-removable-media-usb", "drive-removable-media"}
                            : std::vector<std::string>{"drive-harddisk"};
      usage(d);
      (d.network ? network : drives).push_back(d);
    }
    for (auto &p : chooser::network_places()) {
      Drive d;
      d.id = "mount:" + path_to_uri(p.path);
      d.name = p.label;
      d.path = p.path;
      d.uri = gvfs_uri_of(p.path);
      d.mounted = true;
      d.network = true;
      d.icons = {"folder-remote", "network-server"};
      network.push_back(d);
    }
  }
  // System first, then fixed disks, then removable drives, by name.
  std::stable_sort(drives.begin() + 1, drives.end(), [](const Drive &a, const Drive &b) {
    if (a.removable != b.removable) return !a.removable;
    return a.name < b.name;
  });
  std::stable_sort(network.begin(), network.end(), [](const Drive &a, const Drive &b) { return a.name < b.name; });
  drives_ = std::move(drives);
  network_ = std::move(network);
  if (changed_) changed_();
}

std::vector<Drive> Drives::drives() const { return drives_; }
std::vector<Drive> Drives::network() const { return network_; }

const Drive *Drives::drive_of(const std::string &path) const {
  const Drive *best = nullptr;
  for (const auto *list : {&drives_, &network_})
    for (const auto &d : *list) {
      if (d.path.empty()) continue;
      if ((path == d.path || starts_with(path, d.path == "/" ? "/" : d.path + "/")) &&
          (!best || d.path.size() > best->path.size()))
        best = &d;
    }
  return best;
}

std::vector<std::string> Drives::mount_points() const {
  std::vector<std::string> out;
  for (const auto *list : {&drives_, &network_})
    for (const auto &d : *list)
      if (!d.path.empty() && d.path != "/") out.push_back(d.path);
  return out;
}

void Drives::mount(const std::string &id, MountQuestions q, Done done) {
  auto it = impl_->volumes.find(id);
  if (it == impl_->volumes.end()) {
    if (done) done(false, _("The drive is not there any more."), "");
    return;
  }
  OpCtx *ctx = new_ctx(std::move(q), std::move(done));
  g_volume_mount(it->second, G_MOUNT_MOUNT_NONE, ctx->op, nullptr,
                 [](GObject *src, GAsyncResult *res, gpointer data) {
                   auto *c = static_cast<OpCtx *>(data);
                   GError *e = nullptr;
                   bool ok = g_volume_mount_finish(G_VOLUME(src), res, &e);
                   std::string path;
                   if (ok) {
                     if (GMount *m = g_volume_get_mount(G_VOLUME(src))) {
                       GFile *r = g_mount_get_root(m);
                       path = take(g_file_get_path(r));
                       g_object_unref(r);
                       g_object_unref(m);
                     }
                   }
                   std::string err = error_text(e, "");
                   if (e) g_error_free(e);
                   finish_ctx(c, ok, err, path);
                 },
                 ctx);
}

void Drives::unmount(const std::string &id, Done done) {
  auto it = impl_->mounts.find(id);
  if (it == impl_->mounts.end()) {
    if (done) done(false, _("The drive is not mounted."), "");
    return;
  }
  OpCtx *ctx = new_ctx(MountQuestions(), std::move(done));
  g_mount_unmount_with_operation(it->second, G_MOUNT_UNMOUNT_NONE, ctx->op, nullptr,
                                 [](GObject *src, GAsyncResult *res, gpointer data) {
                                   GError *e = nullptr;
                                   bool ok = g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &e);
                                   std::string err = error_text(e, "");
                                   if (e) g_error_free(e);
                                   finish_ctx(static_cast<OpCtx *>(data), ok, err, "");
                                 },
                                 ctx);
}

void Drives::eject(const std::string &id, Done done) {
  auto mit = impl_->mounts.find(id);
  auto vit = impl_->volumes.find(id);
  OpCtx *ctx = new_ctx(MountQuestions(), std::move(done));
  auto cb_mount = [](GObject *src, GAsyncResult *res, gpointer data) {
    GError *e = nullptr;
    bool ok = g_mount_eject_with_operation_finish(G_MOUNT(src), res, &e);
    std::string err = error_text(e, "");
    if (e) g_error_free(e);
    finish_ctx(static_cast<OpCtx *>(data), ok, err, "");
  };
  auto cb_volume = [](GObject *src, GAsyncResult *res, gpointer data) {
    GError *e = nullptr;
    bool ok = g_volume_eject_with_operation_finish(G_VOLUME(src), res, &e);
    std::string err = error_text(e, "");
    if (e) g_error_free(e);
    finish_ctx(static_cast<OpCtx *>(data), ok, err, "");
  };
  if (mit != impl_->mounts.end() && g_mount_can_eject(mit->second)) {
    g_mount_eject_with_operation(mit->second, G_MOUNT_UNMOUNT_NONE, ctx->op, nullptr, cb_mount, ctx);
  } else if (vit != impl_->volumes.end() && g_volume_can_eject(vit->second)) {
    g_volume_eject_with_operation(vit->second, G_MOUNT_UNMOUNT_NONE, ctx->op, nullptr, cb_volume, ctx);
  } else if (mit != impl_->mounts.end()) {
    g_mount_unmount_with_operation(mit->second, G_MOUNT_UNMOUNT_NONE, ctx->op, nullptr,
                                   [](GObject *src, GAsyncResult *res, gpointer data) {
                                     GError *e = nullptr;
                                     bool ok = g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &e);
                                     std::string err = error_text(e, "");
                                     if (e) g_error_free(e);
                                     finish_ctx(static_cast<OpCtx *>(data), ok, err, "");
                                   },
                                   ctx);
  } else {
    finish_ctx(ctx, false, _("This drive cannot be ejected."), "");
  }
}

void Drives::mount_location(const std::string &uri, MountQuestions q, Done done) {
  OpCtx *ctx = new_ctx(std::move(q), std::move(done));
  ctx->uri = uri;
  ctx->file = g_file_new_for_uri(uri.c_str());
  g_file_mount_enclosing_volume(ctx->file, G_MOUNT_MOUNT_NONE, ctx->op, nullptr,
                                [](GObject *src, GAsyncResult *res, gpointer data) {
                                  auto *c = static_cast<OpCtx *>(data);
                                  GError *e = nullptr;
                                  bool ok = g_file_mount_enclosing_volume_finish(G_FILE(src), res, &e);
                                  if (!ok && e && e->domain == G_IO_ERROR && e->code == G_IO_ERROR_ALREADY_MOUNTED)
                                    ok = true;
                                  std::string path = ok ? take(g_file_get_path(G_FILE(src))) : std::string();
                                  std::string err = ok ? std::string() : error_text(e, c->uri);
                                  if (e) g_error_free(e);
                                  finish_ctx(c, ok, err, path);
                                },
                                ctx);
}

bool Drives::scheme_supported(const std::string &scheme) {
  const gchar *const *schemes = g_vfs_get_supported_uri_schemes(g_vfs_get_default());
  for (int i = 0; schemes && schemes[i]; ++i)
    if (scheme == schemes[i]) return true;
  return false;
}

std::string Drives::package_for(const std::string &scheme) {
  if (scheme == "trash" || scheme == "network") return "gvfs";
  return "gvfs-backends";
}

bool Drives::keyring_available() {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return false;
  if (bus->name_has_owner("org.freedesktop.secrets")) return true;
  DBusMessage *m = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                "ListActivatableNames");
  DBusMessage *r = m ? bus->call(m, 500) : nullptr;
  bool found = false;
  if (r) {
    for (const auto &v : dbus::read_args(r))
      for (const auto &item : v.items)
        if (item.str() == "org.freedesktop.secrets") found = true;
    dbus_message_unref(r);
  }
  return found;
}

std::string local_path_of_uri(const std::string &uri) {
  if (uri.empty()) return "";
  if (uri[0] == '/') return uri;
  GFile *f = g_file_new_for_uri(uri.c_str());
  std::string p = take(g_file_get_path(f));
  g_object_unref(f);
  return p;
}

std::string uri_of_local_path(const std::string &path) {
  std::string uri = gvfs_uri_of(path);
  if (!uri.empty()) return uri;
  return path_to_uri(path);
}

}  // namespace liria
}  // namespace sde
