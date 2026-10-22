// SPDX-License-Identifier: GPL-3.0-or-later
// Removable drives for the panel (alpha50), the way GNOME and Cinnamon
// handle them: UDisks2 lists the drives and mounts, unmounts and powers
// them off (polkit lets the user of an active local session do that without
// a password); a new drive brings a notification with Open and Eject and,
// with [drives] automount=true, is mounted at once. Without UDisks2 on the
// system bus the panel falls back to lsblk, udevadm and udisksctl.
#include <FL/Fl.H>
#include <sys/statvfs.h>

#include <algorithm>
#include <memory>

#include "controls.h"
#include "i18n.h"
#include "panel.h"
#include "process.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

namespace {
const char *kUDisks = "org.freedesktop.UDisks2";
const char *kUDisksPath = "/org/freedesktop/UDisks2";
const char *kNotify = "org.freedesktop.Notifications";
const char *kNotifyPath = "/org/freedesktop/Notifications";

std::string device_bytes(const dbus::Value *v) {
  if (!v) return "";
  std::string s;
  for (uint8_t b : v->bytes()) {
    if (!b) break;
    s += char(b);
  }
  return s;
}
std::string dbus_error(DBusMessage *r) {
  if (!r) return "org.freedesktop.DBus.Error.NoReply";
  if (dbus_message_get_type(r) != DBUS_MESSAGE_TYPE_ERROR) return "";
  const char *name = dbus_message_get_error_name(r);
  return name ? name : "error";
}
std::string dbus_error_text(DBusMessage *r) {
  auto a = r ? dbus::read_args(r) : std::vector<dbus::Value>{};
  return a.empty() ? std::string() : a[0].str();
}
// An a{sv} options argument with "auth.no_user_interaction" when asked.
void options(DBusMessage *m, bool no_interaction = false) {
  dbus::Writer w(m);
  dbus::Writer opts(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
  if (no_interaction) {
    dbus::Writer e(opts.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
    e.str("auth.no_user_interaction").variant_bool(true);
  }
}
}  // namespace

void PanelControls::drives_later(void *d) { static_cast<PanelControls *>(d)->refresh_drives(); }

void PanelControls::start_drives() {
  udisks_ = system_ && system_->name_has_owner(kUDisks);
  if (udisks_) {
    // Drives and filesystems come and go (InterfacesAdded/Removed); mount
    // points change with PropertiesChanged.
    for (const char *member : {"InterfacesAdded", "InterfacesRemoved"})
      system_->add_signal_handler(
          std::string("type='signal',sender='org.freedesktop.UDisks2',interface='org.freedesktop."
                      "DBus.ObjectManager',member='") +
              member + "'",
          [member](DBusMessage *m) {
            return bool(dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", member));
          },
          [this](DBusMessage *) {
            Fl::remove_timeout(drives_later, this);
            Fl::add_timeout(0.3, drives_later, this);
          });
    system_->add_signal_handler(
        "type='signal',sender='org.freedesktop.UDisks2',interface='org.freedesktop.DBus."
        "Properties',member='PropertiesChanged'",
        [](DBusMessage *m) {
          const char *p = dbus_message_get_path(m);
          return p && starts_with(p, "/org/freedesktop/UDisks2/");
        },
        [this](DBusMessage *) {
          Fl::remove_timeout(drives_later, this);
          Fl::add_timeout(0.3, drives_later, this);
        });
    return;
  }
  // New hardware shows up at once: udev reports block devices (USB drives,
  // memory cards) as they are plugged in or removed.
  stream_lines({"udevadm", "monitor", "--udev", "--subsystem-match=block"},
               [this](const std::string &line) {
                 if (line.find(" add ") == std::string::npos &&
                     line.find(" remove ") == std::string::npos &&
                     line.find(" change ") == std::string::npos)
                   return;
                 Fl::remove_timeout(drives_later, this);
                 Fl::add_timeout(0.8, drives_later, this);
               },
               nullptr);
}

void PanelControls::refresh_drives() {
  if (udisks_) {
    udisks_refresh();
    return;
  }
  if (drives_pending_) return;
  drives_pending_ = true;
  if (!run_async({"lsblk", "-P", "-b", "-o",
                  "NAME,PATH,LABEL,SIZE,TYPE,TRAN,RM,HOTPLUG,ROTA,MOUNTPOINT,FSTYPE,MODEL,VENDOR,"
                  "PKNAME"},
                 [this](const std::string &out, int code) {
                   drives_pending_ = false;
                   drives_found(code == 0 ? parse_removable_drives(out)
                                          : std::vector<RemovableDrive>{});
                 }))
    drives_pending_ = false;
}

void PanelControls::udisks_refresh() {
  if (drives_pending_ || !system_) return;
  drives_pending_ = true;
  DBusMessage *m =
      dbus::method(kUDisks, kUDisksPath, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
  system_->call_async(m, [this](DBusMessage *reply) {
    drives_pending_ = false;
    auto args = reply ? dbus::read_args(reply) : std::vector<dbus::Value>{};
    if (args.empty() || !dbus_error(reply).empty()) {
      // UDisks2 went away: fall back to lsblk.
      udisks_ = system_->name_has_owner(kUDisks);
      if (!udisks_) refresh_drives();
      return;
    }
    std::vector<UdisksBlock> blocks;
    std::vector<UdisksDrive> found;
    for (const auto &object : args[0].items) {
      if (object.items.size() != 2) continue;
      const std::string &path = object.items[0].s;
      const dbus::Value &ifaces = object.items[1];
      if (const dbus::Value *b = ifaces.get("org.freedesktop.UDisks2.Block")) {
        UdisksBlock k;
        k.object = path;
        k.device = device_bytes(b->get("PreferredDevice"));
        if (k.device.empty()) k.device = device_bytes(b->get("Device"));
        if (const auto *v = b->get("Drive")) k.drive = v->str();
        if (const auto *v = b->get("Size")) k.size = v->num();
        if (const auto *v = b->get("IdLabel")) k.label = v->str();
        if (const auto *v = b->get("IdType")) k.fstype = v->str();
        if (const auto *v = b->get("IdUsage")) k.usage = v->str();
        if (const auto *v = b->get("HintName")) k.hint_name = v->str();
        if (const auto *v = b->get("HintIgnore")) k.hint_ignore = v->boolean();
        if (const auto *v = b->get("HintSystem")) k.hint_system = v->boolean();
        if (const auto *v = b->get("HintAuto")) k.hint_auto = v->boolean();
        if (const auto *v = b->get("CryptoBackingDevice")) k.crypto_backing = v->str();
        if (const dbus::Value *fs = ifaces.get("org.freedesktop.UDisks2.Filesystem")) {
          k.filesystem = true;
          if (const auto *mps = fs->get("MountPoints"))
            for (const auto &mp : mps->items) {
              std::string s = device_bytes(&mp);
              if (!s.empty()) k.mount_points.push_back(s);
            }
        }
        k.partition = ifaces.get("org.freedesktop.UDisks2.Partition") != nullptr;
        k.partition_table = ifaces.get("org.freedesktop.UDisks2.PartitionTable") != nullptr;
        if (k.drive == "/") k.drive.clear();
        blocks.push_back(k);
      }
      if (const dbus::Value *d = ifaces.get("org.freedesktop.UDisks2.Drive")) {
        UdisksDrive k;
        k.object = path;
        if (const auto *v = d->get("Vendor")) k.vendor = v->str();
        if (const auto *v = d->get("Model")) k.model = v->str();
        if (const auto *v = d->get("ConnectionBus")) k.connection_bus = v->str();
        if (const auto *v = d->get("Media")) k.media = v->str();
        if (const auto *v = d->get("Size")) k.size = v->num();
        if (const auto *v = d->get("RotationRate")) k.rotation_rate = int(v->num());
        if (const auto *v = d->get("Removable")) k.removable = v->boolean();
        if (const auto *v = d->get("MediaRemovable")) k.media_removable = v->boolean();
        if (const auto *v = d->get("Ejectable")) k.ejectable = v->boolean();
        if (const auto *v = d->get("CanPowerOff")) k.can_power_off = v->boolean();
        found.push_back(k);
      }
    }
    // An unlocked LUKS volume belongs to the drive of its encrypted block.
    for (auto &b : blocks)
      if (b.drive.empty() && !b.crypto_backing.empty())
        for (const auto &o : blocks)
          if (o.object == b.crypto_backing) b.drive = o.drive;
    auto drives_list = udisks_removable_drives(blocks, found);
    // HintAuto of the filesystems: automount candidates.
    for (auto &d : drives_list)
      for (auto &p : d.parts)
        for (const auto &b : blocks)
          if (b.object == p.object) p.automount = b.hint_auto;
    drives_found(std::move(drives_list));
  });
}

void PanelControls::drives_found(std::vector<RemovableDrive> found) {
  bool same = found.size() == drives.size();
  for (size_t i = 0; same && i < found.size(); ++i) {
    same = found[i].path == drives[i].path && found[i].parts.size() == drives[i].parts.size();
    for (size_t k = 0; same && k < found[i].parts.size(); ++k)
      same = found[i].parts[k].mountpoint == drives[i].parts[k].mountpoint;
  }
  drive_free.clear();
  for (const auto &d : found)
    for (const auto &p : d.parts) {
      struct statvfs st;
      if (!p.mountpoint.empty() && statvfs(p.mountpoint.c_str(), &st) == 0)
        drive_free[p.mountpoint] = (long long)st.f_bavail * (long long)st.f_frsize;
    }
  std::vector<std::string> names;
  for (const auto &d : found) names.push_back(d.path);
  // A drive plugged in after the first scan: notification and automount.
  if (drives_scanned_) {
    const Config &cfg = Panel::instance ? Panel::instance->config() : Config();
    for (const auto &d : found) {
      if (std::find(known_drives_.begin(), known_drives_.end(), d.path) != known_drives_.end())
        continue;
      drive_tasks.erase(d.name);
      if (cfg.get_bool("drives", "notify", true)) drive_notify(d);
      bool automount = cfg.get_bool("drives", "automount", true);
      if (automount && !d.mounted())
        for (const auto &p : d.parts)
          if (p.automount || !udisks_) {
            // Mount without opening a window (GNOME opens nothing either;
            // the notification offers Open).
            if (udisks_ && !p.object.empty()) {
              DBusMessage *m = dbus::method(kUDisks, p.object, "org.freedesktop.UDisks2.Filesystem",
                                            "Mount");
              options(m, true);
              system_->call_async(m, [this](DBusMessage *) { refresh_drives(); });
            } else {
              run_async({"udisksctl", "mount", "-b", p.path, "--no-user-interaction"},
                        [this](const std::string &, int) { refresh_drives(); });
            }
            break;
          }
    }
  }
  // Tasks of drives that left are kept for the "can be removed" message
  // only while the flyout is open; others are dropped.
  for (auto it = drive_tasks.begin(); it != drive_tasks.end();) {
    bool present = std::any_of(found.begin(), found.end(),
                               [&](const RemovableDrive &d) { return d.name == it->first; });
    if (!present && it->second.state != "removed" && it->second.state != "busy")
      it = drive_tasks.erase(it);
    else
      ++it;
  }
  known_drives_ = names;
  drives_scanned_ = true;
  drives = std::move(found);
  if (!same) changed();
}

void PanelControls::drive_task(const std::string &name, const std::string &state,
                               const std::string &text) {
  if (state.empty())
    drive_tasks.erase(name);
  else
    drive_tasks[name] = {state, text};
  message = text;
  changed();
}

void PanelControls::drive_notify(const RemovableDrive &d) {
  dbus::Bus *bus = session_;
  if (!bus || !bus->name_has_owner(kNotify)) return;
  if (!notify_listening_) {
    notify_listening_ = true;
    bus->add_signal_handler(
        "type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'",
        [](DBusMessage *m) {
          return bool(dbus_message_is_signal(m, "org.freedesktop.Notifications", "ActionInvoked"));
        },
        [this](DBusMessage *m) {
          auto a = dbus::read_args(m);
          if (a.size() < 2) return;
          auto it = drive_notifications_.find(uint32_t(a[0].num()));
          if (it == drive_notifications_.end()) return;
          std::string name = it->second, key = a[1].str();
          drive_notifications_.erase(it);
          if (key == "open" || key == "default")
            drive_open(name);
          else if (key == "eject")
            drive_eject(name, true);
        });
  }
  static const std::map<std::string, const char *> titles = {
      {"usb", N_("USB drive connected")},
      {"card", N_("Memory card inserted")},
      {"hdd", N_("External disk connected")},
      {"ssd", N_("External disk connected")}};
  auto t = titles.find(d.kind);
  std::string title = _(t == titles.end() ? N_("Drive connected") : t->second);
  std::string body = d.label + " · " + human_size(d.size);
  DBusMessage *m = dbus::method(kNotify, kNotifyPath, kNotify, "Notify");
  {
    dbus::Writer w(m);
    w.str(_("Drives")).u32(0).str(d.icon).str(title).str(body);
    w.strings({"open", _("Open"), "eject", _("Eject")});
    {
      dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
      dbus::Writer e(hints.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      e.str("category").variant_str("device.added");
    }
    w.i32(8000);
  }
  std::string name = d.name;
  bus->call_async(m, [this, name](DBusMessage *r) {
    auto a = r ? dbus::read_args(r) : std::vector<dbus::Value>{};
    if (!a.empty()) drive_notifications_[uint32_t(a[0].num())] = name;
  });
}

void PanelControls::drive_open(const std::string &name) {
  for (const auto &d : drives) {
    if (d.name != name) continue;
    std::string mp = d.mountpoint();
    if (!mp.empty()) {
      spawn_detached({"liria", "--no-desktop", mp});
      return;
    }
    if (d.parts.empty()) return;
    std::string label = d.label;
    drive_task(name, "mounting", format(_("Opening %s..."), label.c_str()));
    auto opened = [this, name, label](const std::string &path) {
      if (path.empty()) {
        drive_task(name, "error", format(_("%s could not be opened."), label.c_str()));
      } else {
        spawn_detached({"liria", "--no-desktop", path});
        drive_task(name, "", "");
        message.clear();
      }
      refresh_drives();
    };
    const DrivePartition &p = d.parts.front();
    if (udisks_ && !p.object.empty()) {
      // Mount may ask for a password through the polkit agent when the
      // session is not an active local one.
      DBusMessage *m =
          dbus::method(kUDisks, p.object, "org.freedesktop.UDisks2.Filesystem", "Mount");
      options(m);
      system_->call_async(m, [this, opened, name, label](DBusMessage *r) {
        auto a = r ? dbus::read_args(r) : std::vector<dbus::Value>{};
        std::string err = dbus_error(r);
        if (err.empty() && !a.empty()) {
          opened(a[0].str());
          return;
        }
        if (err.find("NotAuthorized") != std::string::npos)
          drive_task(name, "error",
                     format(_("Not allowed to open %s: the session is not an active local "
                              "session (see loginctl)."),
                            label.c_str()));
        else
          drive_task(name, "error",
                     format(_("%s could not be opened. %s"), label.c_str(),
                            dbus_error_text(r).c_str()));
      }, 60000);
      return;
    }
    run_async({"udisksctl", "mount", "-b", p.path, "--no-user-interaction"},
              [opened](const std::string &out, int code) {
                size_t at = out.find(" at ");
                std::string path;
                if (code == 0 && at != std::string::npos) {
                  path = trim(out.substr(at + 4));
                  if (!path.empty() && path.back() == '.') path.pop_back();
                }
                opened(path);
              });
    return;
  }
}

void PanelControls::drive_eject(const std::string &name, bool power_off_drive) {
  for (const auto &d : drives) {
    if (d.name != name) continue;
    auto parts = std::make_shared<std::vector<DrivePartition>>();
    for (const auto &p : d.parts)
      if (!p.mountpoint.empty()) parts->push_back(p);
    drive_task(name, "ejecting", format(_("Ejecting %s..."), d.label.c_str()));
    unmount_next(parts, 0, d, power_off_drive);
    return;
  }
}

void PanelControls::unmount_next(std::shared_ptr<std::vector<DrivePartition>> parts, size_t i,
                                 RemovableDrive drive, bool power_off_drive) {
  if (i >= parts->size()) {
    power_off(drive, power_off_drive);
    return;
  }
  const DrivePartition p = (*parts)[i];
  // Still in use: name the programs that keep files open (fuser, lsof).
  auto busy = [this, drive, p](const std::string &detail) {
    std::string mp = p.mountpoint;
    std::string name = drive.name, label = drive.label;
    auto report = [this, name, label, detail](const std::vector<std::string> &programs) {
      std::string list;
      for (size_t k = 0; k < programs.size() && k < 4; ++k)
        list += (k ? ", " : "") + programs[k];
      if (!list.empty())
        drive_task(name, "busy",
                   format(_("%s is in use by %s. Close its files and try again."), label.c_str(),
                          list.c_str()));
      else
        drive_task(name, "busy",
                   format(_("%s is still in use. Close its files and try again."), label.c_str()) +
                       (detail.empty() ? "" : " (" + detail + ")"));
      refresh_drives();
    };
    if (!run_async({"fuser", "-vm", mp},
                   [report, mp](const std::string &out, int) {
                     auto programs = parse_busy_programs(out);
                     if (!programs.empty() || !run_async({"lsof", "-Fc", "--", mp},
                                                         [report](const std::string &o, int) {
                                                           report(parse_busy_programs(o));
                                                         }))
                       report(programs);
                   }))
      report({});
  };
  if (udisks_ && !p.object.empty()) {
    DBusMessage *m = dbus::method(kUDisks, p.object, "org.freedesktop.UDisks2.Filesystem", "Unmount");
    options(m);
    system_->call_async(m, [this, parts, i, drive, power_off_drive, busy](DBusMessage *r) {
      std::string err = dbus_error(r);
      if (err.empty() || err.find("NotMounted") != std::string::npos) {
        unmount_next(parts, i + 1, drive, power_off_drive);
      } else if (err.find("DeviceBusy") != std::string::npos) {
        busy("");
      } else {
        drive_task(drive.name, "error",
                   format(_("%s could not be ejected. %s"), drive.label.c_str(),
                          dbus_error_text(r).c_str()));
      }
    }, 60000);
    return;
  }
  run_async({"udisksctl", "unmount", "-b", p.path, "--no-user-interaction"},
            [this, parts, i, drive, power_off_drive, busy](const std::string &out, int code) {
              if (code == 0)
                unmount_next(parts, i + 1, drive, power_off_drive);
              else
                busy(trim(out).empty() ? "" : trim(out));
            });
}

void PanelControls::power_off(const RemovableDrive &drive, bool power_off_drive) {
  std::string name = drive.name, label = drive.label;
  auto done = [this, name, label](bool ok, const std::string &why) {
    // Unmounted is enough to pull the drive out, even when powering it off
    // failed (some card readers refuse).
    drive_task(name, "removed",
               ok ? format(_("%s can be removed safely."), label.c_str())
                  : format(_("%s is unmounted and can be removed. (%s)"), label.c_str(),
                           why.c_str()));
    refresh_drives();
  };
  if (!power_off_drive) {
    done(true, "");
    return;
  }
  if (udisks_ && !drive.object.empty() && (drive.can_power_off || drive.ejectable)) {
    // Cards in a reader: eject the medium; USB sticks and disks: power off
    // (spins a disk down and lets the hub cut its power).
    const char *method = drive.can_power_off ? "PowerOff" : "Eject";
    DBusMessage *m = dbus::method(kUDisks, drive.object, "org.freedesktop.UDisks2.Drive", method);
    options(m);
    system_->call_async(m, [done](DBusMessage *r) {
      std::string err = dbus_error(r);
      done(err.empty(), err.empty() ? "" : dbus_error_text(r));
    }, 60000);
    return;
  }
  if (!udisks_) {
    run_async({"udisksctl", "power-off", "-b", drive.path, "--no-user-interaction"},
              [done](const std::string &out, int code) { done(code == 0, trim(out)); });
    return;
  }
  done(true, "");
}
