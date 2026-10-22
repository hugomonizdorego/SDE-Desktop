// SPDX-License-Identifier: GPL-3.0-or-later
// Removable drives for the panel (alpha50), the way GNOME and Cinnamon
// handle them: UDisks2 lists the drives and mounts, unmounts and powers
// them off (polkit lets the user of an active local session do that without
// a password); a new drive brings a notification with Open and Eject and,
// with [drives] automount=true, is mounted at once. Without UDisks2 on the
// system bus the panel falls back to lsblk, udevadm and udisksctl.
#include <FL/Fl.H>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <memory>

#include "controls.h"
#include "i18n.h"
#include "panel.h"
#include "process.h"
#include "quick.h"
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

// ------------------------------------------------- debion-network-check

namespace {
std::string network_checker_path() {
  std::string p = find_program("debion-network-check");
  if (p.empty() && access("/usr/sbin/debion-network-check", X_OK) == 0)
    p = "/usr/sbin/debion-network-check";
  return p;
}
}  // namespace

bool PanelControls::network_checker() const { return !network_checker_path().empty(); }

// Login (alpha52): the panel asks NetworkManager at start (nmcli, D-Bus
// GetAll) and runs the verdict as soon as it sees a connection; NetworkManager
// state and device signals, and resume from suspend, refresh it at once
// instead of on the next 15 s poll. A verdict that DNS fails is repaired
// without a prompt when polkit allows the safe repair (an administrator at
// the machine, see 50-debion-network-repair.rules), else a notification
// offers "Fix now".
void PanelControls::check_network(std::function<void()> done) {
  std::string checker = network_checker_path();
  if (checker.empty()) {
    if (done) done();
    return;
  }
  if (done) verdict_waiters_.push_back(std::move(done));
  if (net_verdict.empty()) {
    net_verdict = "checking";
    changed();
  }
  if (verdict_pending_) {
    verdict_again_ = true;
    return;
  }
  verdict_pending_ = true;
  std::string key = net_key_;
  bool started = run_async({"timeout", "25", checker, "--quick", "--json"}, [this, key](const std::string &out, int) {
    verdict_pending_ = false;
    if (key != net_key_ && !verdict_again_) {
      // Answered for a connection that is gone: ask again for this one.
      verdict_again_ = true;
    }
    if (!verdict_again_) {
      std::string v = parse_network_verdict(out);
      if (v.empty()) v = "unknown";  // timed out or failed: NetworkManager decides
      bool repairable = parse_json_bool(out, "repairable");
      if (v != net_verdict || repairable != net_repairable) {
        net_verdict = v;
        net_repairable = repairable;
        changed();
      }
      verdict_in(parse_json_bool(out, "auto_repair", true));
      auto waiters = std::move(verdict_waiters_);
      verdict_waiters_.clear();
      for (auto &w : waiters) w();
      return;
    }
    verdict_again_ = false;
    check_network();
  });
  if (!started) {
    verdict_pending_ = false;
    net_verdict = "unknown";
    changed();
  }
}

void PanelControls::net_retry(void *d) { static_cast<PanelControls *>(d)->check_network(); }

void PanelControls::verdict_in(bool auto_repair) {
  Fl::remove_timeout(net_retry, this);
  bool bad = net_verdict != "ok" && net_verdict != "unknown" && net_verdict != "checking";
  if (!bad || net_key_.empty()) {
    net_retries_ = 0;
    return;
  }
  if (net_repair_state == "ok") {
    // "Repaired" no longer holds.
    net_repair_state.clear();
    net_repair_text.clear();
    changed();
  }
  // Check again later: DHCP, the boot repair or the user may fix it.
  Fl::add_timeout(std::min(300, 20 << std::min(net_retries_, 4)), net_retry, this);
  ++net_retries_;
  if (!auto_repair || !network_verdict_fixable(net_verdict, net_repairable)) return;
  if (net_repair_state == "running") return;
  if (!net_auto_tried_) {
    net_auto_tried_ = true;
    // Only without a password prompt: nobody asked for it.
    std::string wrapper = find_program("debion-network-repair");
    if (wrapper.empty() && access("/usr/libexec/debion-network-repair", X_OK) == 0)
      wrapper = "/usr/libexec/debion-network-repair";
    if (!wrapper.empty() && !find_program("pkexec").empty() && !find_program("pkcheck").empty()) {
      std::string process = std::to_string(getpid());
      {
        // pid,start-time,uid: pkcheck's race-free form.
        std::string stat;
        size_t p = read_file("/proc/self/stat", stat) ? stat.rfind(')') : std::string::npos;
        if (p != std::string::npos && p + 2 < stat.size()) {
          auto f = split(stat.substr(p + 2), ' ');
          if (f.size() > 19) process += "," + f[19] + "," + std::to_string(getuid());
        }
      }
      run_async({"pkcheck", "--action-id", "org.debion.network-check.repair", "--process", process},
                [this](const std::string &, int status) {
                  if (status == 0)
                    repair_network(false);
                  else
                    offer_repair();
                });
      return;
    }
  }
  offer_repair();
}

// "Internet needs a quick repair" with "Fix now", once per connection.
void PanelControls::offer_repair() {
  if (net_offered_ || find_program("pkexec").empty()) return;
  dbus::Bus *bus = session_;
  if (!bus || !bus->name_has_owner(kNotify)) return;
  net_offered_ = true;
  DBusMessage *m = dbus::method(kNotify, kNotifyPath, kNotify, "Notify");
  {
    dbus::Writer w(m);
    std::string body = _(network_verdict_text(net_verdict).c_str());
    body += ". ";
    body += _("The repair gives the network settings back to NetworkManager and checks DNS.");
    w.str(_("Network")).u32(net_notification_).str("network-error")
        .str(_("Internet needs a quick repair")).str(body);
    w.strings({"fix", _("Fix now")});
    {
      dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
      dbus::Writer e(hints.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
      e.str("category").variant_str("network.error");
    }
    w.i32(0);
  }
  bus->call_async(m, [this](DBusMessage *r) {
    auto a = r ? dbus::read_args(r) : std::vector<dbus::Value>{};
    if (!a.empty()) net_notification_ = uint32_t(a[0].num());
  });
}

void PanelControls::repair_network(bool full) {
  if (net_repair_state == "running") return;
  // polkit's action names /usr/sbin/debion-network-check (the /usr/bin
  // link would need pkexec's generic authorization).
  std::string checker = network_checker_path();
  if (checker == "/usr/bin/debion-network-check" && access("/usr/sbin/debion-network-check", X_OK) == 0)
    checker = "/usr/sbin/debion-network-check";
  std::string wrapper = find_program("debion-network-repair");
  if (wrapper.empty() && access("/usr/libexec/debion-network-repair", X_OK) == 0)
    wrapper = "/usr/libexec/debion-network-repair";
  if (checker.empty() || find_program("pkexec").empty()) {
    net_repair_state = "failed";
    net_repair_text = _("The network repair (debion-network-check and pkexec) is not installed.");
    changed();
    return;
  }
  if (!full && wrapper.empty()) full = true;
  std::vector<std::string> argv = {"timeout", "300", "pkexec"};
  if (full) {
    argv.push_back(checker);
    argv.push_back("--fix");
  } else {
    argv.push_back(wrapper);
  }
  net_repair_state = "running";
  net_repair_text = _("Repairing the network connection...");
  changed();
  run_async(argv, [this, full](const std::string &out, int status) {
    std::string summary = network_repair_summary(out, status);
    if (status == 126 || status == 127) {
      net_repair_state = "failed";
      net_repair_text = _("The repair needs an administrator's password.");
    } else if (summary == "ok") {
      net_repair_state = "ok";
      net_repair_text = _("Repaired: names (DNS) resolve again.");
    } else if (summary == "done" && !full) {
      // The safe repair found nothing it may change: the full one may.
      net_repair_state = "";
      net_repair_text.clear();
      repair_network(true);
      return;
    } else {
      net_repair_state = status == 0 ? "ok" : "failed";
      net_repair_text = summary == "done" ? std::string(_("The repair finished.")) : summary;
    }
    changed();
    SystemStatus::get().refresh_network();
    refresh_devices();
    SystemStatus::get().check_connectivity(true);
    check_network();
  });
}

void PanelControls::troubleshoot_network() {
  auto &s = SystemStatus::get();
  if (!network_checker() || !s.network.connected()) {
    s.troubleshoot_network();
    return;
  }
  net_repair_state.clear();
  net_repair_text.clear();
  s.message = format(_("Checking the connection to %s..."), s.network.connection.c_str());
  s.changed_now();
  s.check_connectivity(true, [this](int) {
    check_network([this] {
      auto &st = SystemStatus::get();
      if (net_verdict == "ok" && !st.network.no_internet() && !st.network.portal()) {
        st.message = _("Internet access works.");
        st.changed_now();
      } else if (network_verdict_fixable(net_verdict, net_repairable)) {
        st.message = _(network_verdict_text(net_verdict).c_str());
        st.changed_now();
        repair_network(false);
      } else {
        // Proxy, clock, portal, a network without internet: reconnect and
        // let NetworkManager tell.
        st.troubleshoot_network();
      }
    });
  });
}

void PanelControls::network_state(const std::string &key, int connectivity) {
  if (key != net_key_) {
    net_key_ = key;
    net_connectivity_ = connectivity;
    net_verdict.clear();
    net_repairable = false;
    net_retries_ = 0;
    net_offered_ = false;
    net_auto_tried_ = false;
    if (net_repair_state != "running") {
      net_repair_state.clear();
      net_repair_text.clear();
    }
    Fl::remove_timeout(net_retry, this);
    if (!key.empty()) check_network();
    return;
  }
  if (connectivity != net_connectivity_) {
    net_connectivity_ = connectivity;
    // LIMITED -> FULL (DHCP finished) or FULL -> NONE: the verdict may
    // have changed although the connection did not.
    if (!key.empty()) check_network();
  }
}

void PanelControls::net_refresh(void *d) {
  auto *c = static_cast<PanelControls *>(d);
  c->net_refresh_pending_ = false;
  SystemStatus::get().refresh_network();
  c->refresh_devices();
}

void PanelControls::start_network() {
  if (!system_) return;
  auto later = [this] {
    if (net_refresh_pending_) return;
    net_refresh_pending_ = true;
    Fl::add_timeout(0.3, net_refresh, this);
  };
  // NetworkManager's State, ActiveConnections, WirelessEnabled...
  system_->add_signal_handler(
      "type='signal',path='/org/freedesktop/NetworkManager',interface='org.freedesktop.DBus."
      "Properties',member='PropertiesChanged'",
      [](DBusMessage *m) {
        const char *path = dbus_message_get_path(m);
        return dbus_message_is_signal(m, "org.freedesktop.DBus.Properties", "PropertiesChanged") &&
               path && std::string(path) == "/org/freedesktop/NetworkManager";
      },
      [later](DBusMessage *m) {
        auto a = dbus::read_args(m);
        if (a.size() < 2) return;
        for (const char *k : {"State", "ActiveConnections", "PrimaryConnection", "WirelessEnabled",
                              "NetworkingEnabled", "Devices"})
          if (a[1].get(k)) {
            later();
            return;
          }
      });
  // A cable plugged in, a device that NetworkManager starts or stops managing.
  system_->add_signal_handler(
      "type='signal',interface='org.freedesktop.NetworkManager.Device',member='StateChanged'",
      [](DBusMessage *m) {
        return bool(dbus_message_is_signal(m, "org.freedesktop.NetworkManager.Device", "StateChanged"));
      },
      [later](DBusMessage *) { later(); });
  // NetworkManager started after the panel (or restarted by a repair).
  system_->add_signal_handler(
      "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='"
      "org.freedesktop.NetworkManager'",
      [](DBusMessage *m) {
        auto a = dbus::read_args(m);
        return bool(dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) &&
               !a.empty() && a[0].str() == "org.freedesktop.NetworkManager";
      },
      [later](DBusMessage *) { later(); });
  // Resume: the connection and its verdict may be stale.
  system_->add_signal_handler(
      "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'",
      [](DBusMessage *m) {
        return bool(dbus_message_is_signal(m, "org.freedesktop.login1.Manager", "PrepareForSleep"));
      },
      [this](DBusMessage *m) {
        auto a = dbus::read_args(m);
        if (a.empty() || a[0].boolean()) return;
        Fl::add_timeout(3, [](void *d) {
          auto *c = static_cast<PanelControls *>(d);
          SystemStatus::get().refresh_network();
          c->refresh_devices();
          SystemStatus::get().check_connectivity(true);
          c->net_retries_ = 0;
          if (!c->net_key_.empty()) c->check_network();
        }, this);
      });
  if (session_)
    session_->add_signal_handler(
        "type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'",
        [](DBusMessage *m) {
          return bool(dbus_message_is_signal(m, "org.freedesktop.Notifications", "ActionInvoked"));
        },
        [this](DBusMessage *m) {
          auto a = dbus::read_args(m);
          if (a.size() < 2 || !net_notification_ || uint32_t(a[0].num()) != net_notification_) return;
          if (a[1].str() == "fix" || a[1].str() == "default") repair_network(true);
        });
}

void PanelControls::drives_later(void *d) { static_cast<PanelControls *>(d)->refresh_drives(); }

void PanelControls::start_drives() {
  udisks_ = system_ && system_->name_has_owner(kUDisks);
  if (system_) {
    // UDisks2 can start after the panel (D-Bus activation) or restart.
    system_->add_signal_handler(
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='"
        "org.freedesktop.UDisks2'",
        [](DBusMessage *m) {
          auto a = dbus::read_args(m);
          return bool(dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) &&
                 a.size() >= 3 && a[0].str() == kUDisks;
        },
        [this](DBusMessage *m) {
          auto a = dbus::read_args(m);
          udisks_ = !a[2].str().empty();
          // Drives already there when UDisks2 starts are not "new".
          drives_scanned_ = false;
          drives_pending_ = false;
          refresh_drives();
        });
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
  }
  if (udisks_) return;
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
      system_->call_async_any(m, [this, opened, name, label](DBusMessage *r) {
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
    system_->call_async_any(m, [this, parts, i, drive, power_off_drive, busy](DBusMessage *r) {
      std::string err = dbus_error(r);
      if (err.empty() || err.find("NotMounted") != std::string::npos) {
        unmount_next(parts, i + 1, drive, power_off_drive);
      } else if (err.find("DeviceBusy") != std::string::npos || err.find("Failed") != std::string::npos) {
        busy(err.find("DeviceBusy") != std::string::npos ? "" : dbus_error_text(r));
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
    system_->call_async_any(m, [done](DBusMessage *r) {
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
