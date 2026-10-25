// SPDX-License-Identifier: GPL-3.0-or-later
#include "quick.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include "account.h"
#include "avatar_view.h"
#include "build_config.h"
#include "calendar.h"
#include "clockface.h"
#include "timezone.h"
#include "config.h"
#include "agenda.h"
#include "alarm_popup.h"
#include "alarms.h"
#include "art.h"
#include "controls.h"
#include "panel_widgets.h"
#include "dbus.h"
#include "desktop_entry.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "panel.h"
#include "popup_style.h"
#include "power.h"
#include "process.h"
#include "sni.h"
#include "status_model.h"
#include "spawn.h"
#include "theme.h"
#include "tray.h"
#include "tray_icons.h"
#include "util.h"
#include "widgets.h"
#include "nightlight.h"
#include "weather.h"
#include "x11.h"

using namespace sde;

namespace {

// Splits one line of "nmcli -t" output; ':' inside values is escaped as "\:".
std::vector<std::string> nm_fields(const std::string &line) {
  std::vector<std::string> out(1);
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size()) {
      out.back() += line[++i];
    } else if (line[i] == ':') {
      out.emplace_back();
    } else {
      out.back() += line[i];
    }
  }
  return out;
}

std::vector<std::string> lines_of(const std::string &s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line))
    if (!trim(line).empty()) out.push_back(line);
  return out;
}

// First "NN%" in pactl output.
int first_percent(const std::string &s) {
  size_t p = s.find('%');
  if (p == std::string::npos) return -1;
  size_t b = p;
  while (b > 0 && isdigit(static_cast<unsigned char>(s[b - 1]))) --b;
  return b < p ? atoi(s.substr(b, p - b).c_str()) : -1;
}

}  // namespace

namespace {
std::string &settings_help();
}  // namespace

// ============================================================ SystemStatus

SystemStatus &SystemStatus::get() {
  static SystemStatus s;
  return s;
}

void SystemStatus::changed() {
  if (changed_) changed_();
}

void SystemStatus::start(std::function<void()> changed) {
  changed_ = std::move(changed);
  start_upower();
  refresh_battery();
  refresh_brightness();
  refresh_network();
  refresh_audio();
  if (!Panel::safe_mode) PanelControls::get().start([this] { this->changed(); });
  // Periodic refresh for things that have no change notification.
  struct Tick {
    static void network(void *) {
      SystemStatus::get().refresh_network();
      Fl::repeat_timeout(15, network);
    }
    static void battery(void *) {
      SystemStatus::get().refresh_battery();
      Fl::repeat_timeout(30, battery);
    }
  };
  Fl::add_timeout(15, Tick::network);
  Fl::add_timeout(30, Tick::battery);
  start_audio_stream();
  start_connectivity();
  // The Action Center's weather reports a fetch through the usual change.
  weather::Service::get().set_listener([this] { this->changed(); });
  // "Sync time zone" reports its progress the same way.
  tzsync::Sync::get().set_listener([this] {
    this->changed();
    if (Panel::instance && tzsync::Sync::get().state() == tzsync::Sync::DONE)
      Panel::instance->time_zone_changed();
  });
  if (!Panel::safe_mode)
    nightlight::Controller::get().start(
        []() -> const Config * { return Panel::instance ? &Panel::instance->config() : nullptr; });
  run_async({"sde-settings", "--help"},
            [](const std::string &out, int) { settings_help() = out; });
}

// ------------------------------------------------- internet connectivity

namespace {
const char *kNM = "org.freedesktop.NetworkManager";
const char *kNMPath = "/org/freedesktop/NetworkManager";
int parse_connectivity(const std::string &s) {
  std::string v = lower(trim(s));
  return v == "full" ? NetworkState::FULL
         : v == "limited" ? NetworkState::LIMITED
         : v == "portal"  ? NetworkState::PORTAL
         : v == "none"    ? NetworkState::NONE
                          : NetworkState::UNKNOWN;
}
long long mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

std::string NetworkState::status_text() const {
  if (!available) return _("NetworkManager is not running");
  if (!connected()) return _("Not connected");
  switch (connectivity) {
    case FULL:
      return _("Connected · Internet access");
    case PORTAL:
      return _("Sign in to network");
    case LIMITED:
    case NONE:
      return _("Connected, no internet");
    default:
      return _("Connected");
  }
}

void SystemStatus::start_connectivity() {
  if (Panel::safe_mode) return;
  if (dbus::Bus *bus = dbus::Bus::system())
    bus->add_signal_handler(
        "type='signal',path='/org/freedesktop/NetworkManager',interface='org.freedesktop.DBus."
        "Properties',member='PropertiesChanged'",
        [](DBusMessage *m) {
          const char *path = dbus_message_get_path(m);
          return dbus_message_is_signal(m, "org.freedesktop.DBus.Properties", "PropertiesChanged") &&
                 path && std::string(path) == kNMPath;
        },
        [this](DBusMessage *m) {
          auto a = dbus::read_args(m);
          if (a.size() < 2 || a[0].str() != kNM) return;
          if (const auto *u = a[1].get("ConnectivityCheckUri")) network.check_uri = u->str();
          if (const auto *e = a[1].get("ConnectivityCheckEnabled")) network.check_enabled = e->boolean();
          if (const auto *e = a[1].get("ConnectivityCheckAvailable"))
            network.check_enabled = network.check_enabled && e->boolean();
          if (const auto *c = a[1].get("Connectivity")) {
            nm_dbus_ = true;
            set_connectivity(int(c->num()));
          }
        });
  refresh_connectivity();
}

void SystemStatus::set_connectivity(int value) {
  if (value < NetworkState::UNKNOWN || value > NetworkState::FULL) value = NetworkState::UNKNOWN;
  if (value == network.connectivity) return;
  network.connectivity = value;
  changed();
}

// NetworkManager's Connectivity property over D-Bus; nmcli when NM is not
// reachable on the system bus.
void SystemStatus::refresh_connectivity() {
  auto fallback = [this] {
    nm_dbus_ = false;
    run_async({"nmcli", "-t", "networking", "connectivity"}, [this](const std::string &out, int st) {
      if (st == 0) set_connectivity(parse_connectivity(out));
    });
  };
  dbus::Bus *bus = Panel::safe_mode ? nullptr : dbus::Bus::system();
  if (!bus) {
    fallback();
    return;
  }
  dbus::get_all_properties(bus, kNM, kNMPath, kNM, [this, fallback](const dbus::Value &v) {
    const dbus::Value *c = v.valid() ? v.get("Connectivity") : nullptr;
    if (!c) {
      fallback();
      return;
    }
    nm_dbus_ = true;
    if (const auto *u = v.get("ConnectivityCheckUri")) network.check_uri = u->str();
    bool enabled = true;
    if (const auto *e = v.get("ConnectivityCheckEnabled")) enabled = e->boolean();
    if (const auto *e = v.get("ConnectivityCheckAvailable")) enabled = enabled && e->boolean();
    network.check_enabled = enabled;
    set_connectivity(int(c->num()));
  });
}

void SystemStatus::check_connectivity(bool force, std::function<void(int)> done) {
  long long now = mono_ms();
  if (!force && now - last_check_ms_ < 5000) {
    if (done) done(network.connectivity);
    return;
  }
  last_check_ms_ = now;
  auto finish = [this, done](int value) {
    set_connectivity(value);
    if (done) done(network.connectivity);
  };
  auto by_nmcli = [finish] {
    run_async({"nmcli", "-t", "networking", "connectivity", "check"},
              [finish](const std::string &out, int st) {
                finish(st == 0 ? parse_connectivity(out) : SystemStatus::get().network.connectivity);
              });
  };
  dbus::Bus *bus = Panel::safe_mode ? nullptr : dbus::Bus::system();
  if (!bus || !nm_dbus_) {
    by_nmcli();
    return;
  }
  DBusMessage *m = dbus::method(kNM, kNMPath, kNM, "CheckConnectivity");
  // NetworkManager answers after its check, which can take a while.
  bus->call_async(m, [finish, by_nmcli](DBusMessage *reply) {
    auto a = dbus::read_args(reply);
    if (a.empty())
      by_nmcli();
    else
      finish(int(a[0].num()));
  }, 30000);
}

void SystemStatus::troubleshoot_network() {
  if (!network.connected()) {
    message = _("Not connected: choose a Wi-Fi network or plug in the network cable.");
    changed();
    refresh_network(true);
    return;
  }
  std::string name = network.connection;
  message = format(_("Checking the connection to %s..."), name.c_str());
  changed();
  check_connectivity(true, [this, name](int value) {
    if (value == NetworkState::FULL) {
      message = _("Internet access works.");
      changed();
      return;
    }
    if (value == NetworkState::PORTAL) {
      message = _("This network needs a sign-in: open the sign-in page.");
      changed();
      return;
    }
    // Reconnecting renews the address (DHCP) and the DNS servers.
    message = format(_("Reconnecting to %s..."), name.c_str());
    changed();
    run_async({"nmcli", "connection", "up", "id", name}, [this, name](const std::string &, int st) {
      refresh_network();
      Fl::add_timeout(st == 0 ? 3 : 0.5, [](void *) {
        SystemStatus::get().check_connectivity(true, [](int v) {
          auto &s = SystemStatus::get();
          s.message = v == NetworkState::FULL ? _("Internet access works again.")
                      : v == NetworkState::PORTAL
                          ? _("This network needs a sign-in: open the sign-in page.")
                          : _("Still no internet. The network itself does not reach the internet: "
                              "restart the router or modem, or check the DNS settings.");
          s.changed();
        });
      });
      if (st != 0) {
        message = format(_("Could not reconnect to %s."), name.c_str());
        changed();
      }
    });
  });
}

void SystemStatus::open_portal() {
  std::string uri = network.check_uri.empty() ? "http://nmcheck.gnome.org/check_network_status.txt"
                                              : network.check_uri;
  // Any plain http page is redirected to the portal's sign-in form.
  open_uri(uri);
  Fl::add_timeout(20, [](void *) { SystemStatus::get().check_connectivity(true); });
}

std::string SystemStatus::network_signature() const {
  std::ostringstream o;
  o << network.available << network.has_wifi << network.wifi_enabled << network.kind << '|'
    << network.connection << '|' << network.signal;
  for (const auto &w : wifi) o << '|' << w.ssid << w.signal << w.secure << w.active;
  return o.str();
}

// pactl reports every volume, mute and device change. The sound server may
// start after the panel or restart, so the stream is retried.
void SystemStatus::start_audio_stream() {
  audio_stream_ = stream_lines(
      {"pactl", "subscribe"},
      [this](const std::string &line) {
        if (line.find("sink") == std::string::npos && line.find("server") == std::string::npos &&
            line.find("source") == std::string::npos)
          return;
        if (audio_pending_) return;
        audio_pending_ = true;
        Fl::add_timeout(0.1, [](void *) {
          SystemStatus::get().audio_pending_ = false;
          SystemStatus::get().refresh_audio();
          PanelControls::get().refresh_audio_apps();
        });
      },
      [] {
        SystemStatus::get().audio_stream_ = 0;
        Fl::add_timeout(10, [](void *) {
          SystemStatus::get().refresh_audio();
          SystemStatus::get().start_audio_stream();
        });
      });
}

// ------------------------------------------------------------- battery

namespace {
const char *kUPower = "org.freedesktop.UPower";
const char *kUPowerPath = "/org/freedesktop/UPower";
const char *kDisplayDevice = "/org/freedesktop/UPower/devices/DisplayDevice";
// The low and critical levels of the icon: 20 % and 10 %, or sde-power's
// warnings ([power] battery_low, battery_critical) when they are higher.
int power_level(const char *key, int fallback) {
  return Panel::instance ? Panel::instance->config().get_int("power", key, fallback) : fallback;
}
int low_level() { return battery_low_level(power_level("battery_low", 10)); }
int critical_level() { return battery_critical_level(power_level("battery_critical", 5)); }
void upower_changed(void *) { SystemStatus::get().refresh_battery(); }
}  // namespace

void SystemStatus::refresh_battery() {
  // While UPower reports a battery it is asked (asynchronously, see
  // read_upower()); otherwise the kernel's power_supply class is read and
  // UPower is asked whether it is there by now.
  if (upower_) {
    read_upower();
    return;
  }
  BatteryState b;
  BatteryReading r;
  read_sysfs_battery(b, r);
  apply_battery(b, r);
  read_upower();
}

// The first battery of the power_supply class (alpha51), and whether a mains
// adapter is online. SDE_POWER_SUPPLY_DIR replaces the class directory in
// tests.
void SystemStatus::read_sysfs_battery(BatteryState &b, BatteryReading &r) {
  const char *dir_env = getenv("SDE_POWER_SUPPLY_DIR");
  std::string dir = dir_env && *dir_env ? dir_env : "/sys/class/power_supply";
  bool ac = false;
  std::string status;
  for (auto &dev : list_dir(dir)) {
    std::string base = dir + "/" + dev, type, cap, scope, online;
    if (!read_file(base + "/type", type)) continue;
    type = trim(type);
    if (read_file(base + "/scope", scope) && trim(scope) == "Device") continue;  // mice etc.
    if (type == "Mains" || starts_with(type, "USB")) {
      ac = ac || (read_file(base + "/online", online) && trim(online) == "1");
      continue;
    }
    if (type != "Battery" || b.present || !read_file(base + "/capacity", cap)) continue;
    b.present = true;
    b.percent = std::max(0, std::min(100, atoi(cap.c_str())));
    read_file(base + "/status", status);
    std::string vendor, model;
    read_file(base + "/manufacturer", vendor);
    read_file(base + "/model_name", model);
    b.model = battery_model_name(vendor, model);
    // Energy (µWh) and power (µW), or charge (µAh) and current (µA).
    std::string now, full, rate;
    bool energy = read_file(base + "/energy_now", now) && read_file(base + "/energy_full", full);
    bool charge = !energy && read_file(base + "/charge_now", now) &&
                  read_file(base + "/charge_full", full);
    if (energy || charge) {
      r.energy = atof(now.c_str()) / 1e6;
      r.energy_full = atof(full.c_str()) / 1e6;
      if (read_file(base + (energy ? "/power_now" : "/current_now"), rate))
        r.rate = std::fabs(atof(rate.c_str())) / 1e6;
    }
  }
  if (!b.present) return;
  b.source = "sysfs";
  int state = battery_state_from_sysfs(status, b.percent, ac);
  b.phase = battery_phase(state, b.percent, 0, state == 2, low_level(), critical_level());
  r.phase = b.phase;
  r.percent = b.percent;
}

// UPower's DisplayDevice (the batteries combined), on the system bus.
void SystemStatus::read_upower() {
  dbus::Bus *bus = Panel::safe_mode ? nullptr : dbus::Bus::system();
  if (!bus || upower_pending_) return;
  upower_pending_ = true;
  // OnBattery decides an unknown state.
  dbus::get_all_properties(bus, kUPower, kUPowerPath, kUPower, [this, bus](const dbus::Value &d) {
    if (!d.valid()) {
      upower_pending_ = false;
      if (upower_) {  // UPower went away: the power_supply class
        upower_ = false;
        refresh_battery();
      }
      return;
    }
    const auto *ob = d.get("OnBattery");
    bool on_battery = ob && ob->boolean();
    dbus::get_all_properties(
        bus, kUPower, kDisplayDevice, "org.freedesktop.UPower.Device",
        [this, on_battery](const dbus::Value &v) {
          upower_pending_ = false;
          auto num = [&v](const char *key) {
            const auto *p = v.valid() ? v.get(key) : nullptr;
            return !p ? 0.0 : p->inner().type == 'd' ? p->inner().d : double(p->num());
          };
          const auto *present = v.valid() ? v.get("IsPresent") : nullptr;
          // Type 2: a battery (UPSes and peripherals are not the computer's).
          if (!present || !present->boolean() || int(num("Type")) != 2) {
            if (upower_) {
              upower_ = false;
              refresh_battery();
            }
            return;
          }
          upower_ = true;
          BatteryState b, sysfs;
          BatteryReading r, unused;
          read_sysfs_battery(sysfs, unused);  // the model name
          b.present = true;
          b.model = sysfs.model;
          b.source = "upower";
          double percent = std::max(0.0, std::min(100.0, num("Percentage")));
          b.percent = int(percent + 0.5);
          b.phase = battery_phase(int(num("State")), percent, int(num("WarningLevel")), on_battery,
                                  low_level(), critical_level());
          r.phase = b.phase;
          r.percent = percent;
          r.energy = num("Energy");
          r.energy_full = num("EnergyFull");
          r.rate = std::fabs(num("EnergyRate"));
          r.time_to_empty = (long long)num("TimeToEmpty");
          r.time_to_full = (long long)num("TimeToFull");
          apply_battery(b, r);
        });
  });
}

void SystemStatus::apply_battery(BatteryState b, const BatteryReading &reading) {
  BatteryReading r = reading;
  r.t = mono_ms() / 1000.0;
  // Readings from another source come in other units.
  if (b.source != battery.source) estimator_.reset();
  if (b.present) {
    estimator_.update(r);
    b.minutes = estimator_.minutes();
    b.calculating = estimator_.calculating();
  }
  b.charging = b.phase == BatteryPhase::CHARGING;
  b.full = b.phase == BatteryPhase::FULL;
  bool diff = b.present != battery.present || b.percent != battery.percent ||
              b.charging != battery.charging || b.full != battery.full ||
              b.minutes != battery.minutes || b.model != battery.model ||
              b.phase != battery.phase || b.calculating != battery.calculating ||
              b.source != battery.source;
  battery = b;
  if (diff) changed();
}

void SystemStatus::start_upower() {
  dbus::Bus *bus = Panel::safe_mode ? nullptr : dbus::Bus::system();
  if (!bus) return;
  // UPower's changes come in bursts: one read for each.
  for (const char *path : {kUPowerPath, kDisplayDevice})
    bus->add_signal_handler(
        std::string("type='signal',interface='org.freedesktop.DBus.Properties',member='"
                    "PropertiesChanged',path='") +
            path + "'",
        [path](DBusMessage *m) {
          const char *p = dbus_message_get_path(m);
          return dbus_message_is_signal(m, "org.freedesktop.DBus.Properties",
                                        "PropertiesChanged") &&
                 p && std::string(p) == path;
        },
        [](DBusMessage *) {
          Fl::remove_timeout(upower_changed);
          Fl::add_timeout(0.2, upower_changed);
        });
  // UPower starting or going away.
  bus->add_signal_handler(
      "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='"
      "org.freedesktop.UPower'",
      [](DBusMessage *m) {
        auto a = dbus::read_args(m);
        return bool(dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) &&
               !a.empty() && a[0].str() == kUPower;
      },
      [this](DBusMessage *m) {
        auto a = dbus::read_args(m);
        if (a.size() >= 3 && a[2].str().empty()) upower_ = false;
        refresh_battery();
      });
}

// The backlight class directory; SDE_BACKLIGHT_DIR replaces it in tests.
static std::string backlight_dir() {
  const char *dir = getenv("SDE_BACKLIGHT_DIR");
  return dir && *dir ? dir : "/sys/class/backlight";
}

void SystemStatus::refresh_brightness() {
  BrightnessState s;
  // The same backlight as sde-power and Settings (firmware before platform
  // before raw interfaces).
  power::Backlight bl;
  if (power::primary_backlight(bl, backlight_dir())) {
    s.available = true;
    s.device = bl.name;
    s.value = bl.value;
    s.max = bl.max;
  }
  // No built-in panel: an external monitor through DDC/CI (ddcutil), probed
  // once in the background because it takes a few seconds.
  // [brightness] external_monitor (older name: [action_center]
  // external_brightness) allows the DDC/CI monitor.
  bool external =
      Panel::instance &&
      Panel::instance->config().get_bool(
          "brightness", "external_monitor",
          Panel::instance->config().get_bool("action_center", "external_brightness", true));
  if (!s.available && ddc_.available && external) s = ddc_;
  if (!s.available && !ddc_probed_ && !Panel::safe_mode && external &&
      !find_program("ddcutil").empty()) {
    ddc_probed_ = true;
    run_async({"ddcutil", "--brief", "getvcp", "10"}, [this](const std::string &out, int status) {
      // "VCP 10 C 50 100": current and maximum.
      int cur = 0, max = 0;
      if (status == 0 && sscanf(out.c_str(), "VCP %*s C %d %d", &cur, &max) == 2 && max > 0) {
        ddc_.available = true;
        ddc_.device = "ddc";
        ddc_.value = cur;
        ddc_.max = max;
        refresh_brightness();
      }
    });
  }
  bool diff = s.available != brightness.available || s.value != brightness.value;
  brightness = s;
  if (diff) changed();
}

void SystemStatus::refresh_network(bool rescan) {
  bool started = run_async(
      {"nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device"},
      [this, rescan](const std::string &out, int status) {
        NetworkState n;
        n.available = status == 0;
        for (auto &line : lines_of(out)) {
          auto f = nm_fields(line);
          if (f.size() < 3) continue;
          if (f[0] == "wifi") n.has_wifi = true;
          bool connected = f[1] == "connected" || starts_with(f[1], "connected");
          if (connected && n.kind.empty() && (f[0] == "wifi" || f[0] == "ethernet")) {
            n.kind = f[0];
            n.connection = f[2];
          }
        }
        if (!n.available) {
          network = n;
          wifi.clear();
          changed();
          return;
        }
        run_async({"nmcli", "radio", "wifi"},
                  [this, n, rescan](const std::string &out2, int) mutable {
                    n.wifi_enabled = trim(out2) == "enabled";
                    std::string before = network_signature();
                    std::string now = n.kind + "|" + n.connection;
                    if (now != last_connection_) {
                      last_connection_ = now;
                      // A new connection: ask NetworkManager whether it reaches the internet.
                      if (!n.kind.empty())
                        Fl::add_timeout(2, [](void *) { SystemStatus::get().check_connectivity(true); });
                      else
                        set_connectivity(NetworkState::UNKNOWN);
                    }
                    if (!nm_dbus_) refresh_connectivity();
                    network.available = n.available;
                    network.has_wifi = n.has_wifi;
                    network.wifi_enabled = n.wifi_enabled;
                    network.kind = n.kind;
                    network.connection = n.connection;
                    if (!n.has_wifi || !n.wifi_enabled) {
                      wifi.clear();
                      if (network_signature() != before) changed();
                      return;
                    }
                    run_async({"nmcli", "-t", "-f", "IN-USE,SIGNAL,SECURITY,SSID", "device", "wifi",
                               "list", "--rescan", rescan ? "yes" : "no"},
                              [this, before](const std::string &out3, int) {
                                parse_wifi_list(out3);
                                if (network_signature() != before) changed();
                              });
                  });
      });
  if (!started && network.available) {
    network = NetworkState();
    changed();
  }
}

void SystemStatus::parse_wifi_list(const std::string &out) {
  std::vector<WifiNetwork> list;
  for (auto &line : lines_of(out)) {
    auto f = nm_fields(line);
    if (f.size() < 4 || f[3].empty()) continue;
    WifiNetwork w;
    w.active = f[0] == "*";
    w.signal = atoi(f[1].c_str());
    w.secure = !f[2].empty() && f[2] != "--";
    w.ssid = f[3];
    auto same = std::find_if(list.begin(), list.end(),
                             [&](const WifiNetwork &o) { return o.ssid == w.ssid; });
    if (same != list.end()) {
      same->signal = std::max(same->signal, w.signal);
      same->active = same->active || w.active;
      continue;
    }
    list.push_back(w);
    if (w.active && network.kind == "wifi") network.signal = w.signal;
  }
  std::stable_sort(list.begin(), list.end(), [](const WifiNetwork &a, const WifiNetwork &b) {
    if (a.active != b.active) return a.active;
    return a.signal > b.signal;
  });
  for (auto &w : list)
    if (w.active) network.signal = w.signal;
  wifi = list;
}

void SystemStatus::refresh_audio() {
  bool started = run_async(
      {"pactl", "get-sink-volume", "@DEFAULT_SINK@"}, [this](const std::string &vol, int st) {
        if (st != 0) {
          if (audio.available) {
            audio = AudioState();
            changed();
          }
          return;
        }
        int v = first_percent(vol);
        run_async({"pactl", "get-sink-mute", "@DEFAULT_SINK@"}, [this, v](const std::string &mute,
                                                                          int) {
          bool muted = mute.find("yes") != std::string::npos;
          run_async({"pactl", "get-default-sink"}, [this, v, muted](const std::string &def, int) {
            run_async({"pactl", "list", "sinks"},
                      [this, v, muted, def](const std::string &list, int) {
                        AudioState a;
                        a.available = true;
                        a.volume = std::max(0, v);
                        a.muted = muted;
                        a.default_sink = trim(def);
                        std::string name;
                        for (auto &line : lines_of(list)) {
                          std::string l = trim(line);
                          if (starts_with(l, "Name:"))
                            name = trim(l.substr(5));
                          else if (starts_with(l, "Description:") && !name.empty()) {
                            a.sinks.push_back({name, trim(l.substr(12))});
                            name.clear();
                          }
                        }
                        bool diff = a.volume != audio.volume || a.muted != audio.muted ||
                                    a.default_sink != audio.default_sink ||
                                    a.sinks.size() != audio.sinks.size() || !audio.available;
                        audio = a;
                        if (diff) changed();
                      });
          });
        });
      });
  if (!started && audio.available) {
    audio = AudioState();
    changed();
  }
}

void SystemStatus::set_volume(int percent) {
  percent = std::max(0, std::min(150, percent));
  audio.volume = percent;
  run_async({"pactl", "set-sink-volume", "@DEFAULT_SINK@", std::to_string(percent) + "%"}, nullptr);
  if (audio.muted && percent > 0) toggle_mute();
}

void SystemStatus::toggle_mute() {
  audio.muted = !audio.muted;
  run_async({"pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle"}, nullptr);
  changed();
}

void SystemStatus::set_default_sink(const std::string &name) {
  run_async({"pactl", "set-default-sink", name},
            [](const std::string &, int) { SystemStatus::get().refresh_audio(); });
}

void SystemStatus::set_wifi_enabled(bool on) {
  network.wifi_enabled = on;
  if (!on) wifi.clear();
  changed();
  run_async({"nmcli", "radio", "wifi", on ? "on" : "off"}, [on](const std::string &, int) {
    // Give NetworkManager a moment to find networks.
    Fl::add_timeout(on ? 3 : 0.5, [](void *) { SystemStatus::get().refresh_network(true); });
  });
}

void SystemStatus::connect_wifi(const WifiNetwork &net) {
  std::string ssid = net.ssid;
  message = format(_("Connecting to %s..."), ssid.c_str());
  changed();
  run_async(
      {"nmcli", "device", "wifi", "connect", ssid}, [this, ssid](const std::string &out, int st) {
        if (st == 0) {
          message = format(_("Connected to %s"), ssid.c_str());
          refresh_network();
          PanelControls::get().refresh_devices();
          return;
        }
        // Unknown secured network: ask for the password and try again.
        const char *pw = fl_password(
            "%s", "", format(_("Password for the Wi-Fi network \"%s\":"), ssid.c_str()).c_str());
        if (!pw || !*pw) {
          message = trim(out).empty() ? _("Connection failed") : trim(out);
          changed();
          return;
        }
        std::string password = pw;
        run_async({"nmcli", "device", "wifi", "connect", ssid, "password", password},
                  [this, ssid](const std::string &out2, int st2) {
                    message = st2 == 0 ? format(_("Connected to %s"), ssid.c_str())
                                       : (trim(out2).empty() ? std::string(_("Connection failed"))
                                                             : trim(out2));
                    refresh_network();
                  });
      });
}

void SystemStatus::set_brightness(int percent) {
  if (!brightness.available) return;
  percent = std::max(1, std::min(100, percent));
  int value = power::percent_to_raw(percent, brightness.max);
  if (value == brightness.value) return;
  brightness.value = value;
  if (brightness.device == "ddc") {
    // ddcutil is slow: send the last value once the slider rests.
    ddc_.value = value;
    Fl::remove_timeout(ddc_apply, this);
    Fl::add_timeout(0.3, ddc_apply, this);
    return;
  }
  // A backlight the user may write to (udev rule, or the test fixture).
  std::string file = backlight_dir() + "/" + brightness.device + "/brightness";
  if (access(file.c_str(), W_OK) == 0) {
    if (FILE *f = fopen(file.c_str(), "w")) {
      bool ok = fprintf(f, "%d\n", value) > 0;
      if (fclose(f) == 0 && ok) return;
    }
  }
  // systemd-logind lets the session's user change the backlight.
  dbus::Bus *bus = dbus::Bus::system();
  if (bus) {
    DBusMessage *m = dbus::method("org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                                  "org.freedesktop.login1.Session", "SetBrightness");
    dbus::Writer(m).str("backlight").str(brightness.device).u32(uint32_t(value));
    bus->call_async(m, [percent](DBusMessage *reply) {
      if (!reply) run_async({"brightnessctl", "set", std::to_string(percent) + "%"}, nullptr);
    });
  } else {
    run_async({"brightnessctl", "set", std::to_string(percent) + "%"}, nullptr);
  }
}

void SystemStatus::reset_external_brightness() {
  ddc_ = BrightnessState();
  ddc_probed_ = false;
  refresh_brightness();
  changed();
}

void SystemStatus::ddc_apply(void *data) {
  auto *self = static_cast<SystemStatus *>(data);
  run_async({"ddcutil", "--noverify", "setvcp", "10", std::to_string(self->ddc_.value)}, nullptr);
}

void SystemStatus::step_brightness(int delta) {
  refresh_brightness();
  if (!brightness.available) return;
  // Finer steps near the dark end, as the brightness keys of sde-power.
  int now = brightness.percent();
  set_brightness(delta % 5 == 0 ? power::step_percent(now, delta / 5, 5) : now + delta);
  changed();
}

// =========================================================== Action Center
// The Action Center (corner popup) and the status icon flyouts share these
// widgets and section builders. Visual language adapted from Eduka-Desktop:
// rounded cards, accent tiles and a fixed session footer.
namespace {
using IconFn = std::function<void(Painter &, double, double, double, const Color &)>;
using StringFn = std::function<std::string()>;

// Runs |fn| from the event loop, after the widget whose callback asked for
// it is done (a rebuild or another popup may delete that widget).
void later(std::function<void()> fn) {
  auto *heap = new std::function<void()>(std::move(fn));
  Fl::add_timeout(0, [](void *d) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(d));
    (*f)();
  }, heap);
}

class ActionLabel : public Fl_Widget {
 public:
  ActionLabel(int x, int y, int w, int h, StringFn text, int style = 0)
      : Fl_Widget(x, y, w, h), text_(std::move(text)), style_(style) {}
  void draw() override {
    const Theme &t = Panel::instance->theme();
    int size = t.font_size + (style_ == 3 ? 12 : style_ == 5 ? -1 : 0);
    text_font(size, style_ == 1 || style_ == 3 || style_ == 5);
    draw_text(text_(), x(), y(), w(), h(), style_ == 2 || style_ == 5 ? t.text_dim : t.text,
              style_ == 4 ? 2 : 0);
  }

 private:
  StringFn text_;
  int style_;  // 0 text, 1 title, 2 dim, 3 large, 4 right aligned, 5 section caption
};

// A group of rows. alpha51: in the flyouts a group is plain text rows with
// a thin separator above it (no card); the Action Center keeps soft cards.
class ActionCard : public Fl_Group {
 public:
  ActionCard(int x, int y, int w, bool flat = false, bool rule = false)
      : Fl_Group(x, y, w, S(40)), flat_(flat), rule_(rule) {
    box(FL_NO_BOX);
    resizable(nullptr);
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    {
      Painter p(window()->w(), window()->h());
      if (flat_) {
        if (rule_) p.line(x(), y() + 0.5, x() + w(), y() + 0.5, t.border);
      } else {
        p.fill_rounded(x(), y(), w(), h(), S(8), t.surface_alt);
        if (t.glass) p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(8), t.border);
      }
    }
    draw_children();
  }

 private:
  bool flat_, rule_;
};

class ActionButton : public DrawnButton {
 public:
  ActionButton(int x, int y, int w, int h, std::string title, IconFn icon,
               std::function<void()> action, std::function<bool()> on = nullptr, int color = -1,
               bool row = false, StringFn sub = nullptr)
      : DrawnButton(x, y, w, h, nullptr),
        title_(std::move(title)),
        icon_(std::move(icon)),
        action_(std::move(action)),
        on_(std::move(on)),
        sub_(std::move(sub)),
        color_(color),
        row_(row) {
    copy_tooltip(title_.c_str());
    callback([](Fl_Widget *w, void *) {
      auto *b = static_cast<ActionButton *>(w);
      b->action_();
    });
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    bool hot = Fl::belowmouse() == this && active_r(), on = on_ && on_();
    static const unsigned colors[] = {0x2563EB, 0x7C3AED, 0x047857, 0xB45309, 0xBE185D, 0x0E7490};
    bool multi =
        Panel::instance->config().get_bool("action_center", "multicolor", false) && color_ >= 0;
    Color accent = multi ? Color::hex(colors[color_ % 6]) : t.accent;
    Color white = Color::hex(0xFFFFFF), black = Color::hex(0x101820);
    Color ink, bg;
    bool fill = true;
    if (row_) {
      // alpha51: a plain text row; hover and "on" are a soft tint, the icon
      // takes the accent when on.
      bg = on ? accent.alpha(t.dark ? 0.24 : 0.14) : t.hover;
      fill = on || hot || value();
      if (value()) bg = bg.mix(t.text, 0.08);
      ink = t.text;
    } else {
      bg = on ? accent : t.surface_alt;
      if (multi && !on) bg = t.surface.mix(accent, t.dark ? 0.18 : 0.08);
      if (hot || value()) bg = bg.mix(on ? t.on_accent : t.text, value() ? 0.15 : 0.06);
      ink = on ? (white.contrast(accent) >= 4.5 ? white : black) : t.text;
    }
    if (!active_r()) ink = t.text_dim;
    int radius = row_ ? S(6) : S(8);
    {
      Painter p(window()->w(), window()->h());
      if (fill) p.fill_rounded(x(), y(), w(), h(), radius, bg);
      if (!row_ && (hot || on)) p.stroke_rounded(x(), y(), w(), h(), radius, on ? accent : t.border);
      double size = S(row_ ? popup_style::kIcon : 20);
      Color icon_ink = row_ && on && active_r() ? t.accent_text : ink;
      icon_(p, row_ && !title_.empty() ? x() + S(8) : x() + (w() - size) / 2.0,
            row_ ? y() + (h() - size) / 2.0 : y() + S(8), size, icon_ink);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), radius);
    }
    text_font(t.font_size - (row_ ? 0 : S(1)), !row_);
    if (!row_ && text_width(title_) > w() - S(8))
      text_font(std::max(S(9), t.font_size - S(2)), true);
    int tx = row_ ? x() + S(32) : x() + S(4), tw = row_ ? w() - S(40) : w() - S(8);
    draw_text(title_, tx, row_ ? y() : y() + S(30), tw, row_ ? h() : S(18), ink, row_ ? 0 : 1);
    if (sub_ && !row_) {
      text_font(std::max(S(9), t.font_size - 2));
      draw_text(sub_(), tx, y() + S(46), tw, S(16), on ? ink : t.text_dim, 1);
    }
  }

 private:
  std::string title_;
  IconFn icon_;
  std::function<void()> action_;
  std::function<bool()> on_;
  StringFn sub_;
  int color_;
  bool row_;
};

// Icon-only button; |tip| names it.
ActionButton *icon_button(int x, int y, int w, int h, const std::string &tip, IconFn icon,
                          std::function<void()> action) {
  auto *b = new ActionButton(x, y, w, h, "", std::move(icon), std::move(action), nullptr, -1, true);
  b->copy_tooltip(tip.c_str());
  return b;
}

// Quick tile of the Action Center: a rounded toggle with its name below.
// With |more|, the right part of the toggle shows a chevron that opens the
// tile's flyout (Wi-Fi networks, Bluetooth devices).
class QuickTile : public DrawnButton {
 public:
  QuickTile(int x, int y, int w, int h, std::string title, IconFn icon, std::function<void()> click,
            std::function<bool()> on, StringFn caption, std::function<void()> more, int color)
      : DrawnButton(x, y, w, h, nullptr),
        title_(std::move(title)),
        icon_(std::move(icon)),
        click_(std::move(click)),
        on_(std::move(on)),
        caption_(std::move(caption)),
        more_(std::move(more)),
        color_(color) {
    copy_tooltip(title_.c_str());
    callback([](Fl_Widget *w, void *) {
      auto *t = static_cast<QuickTile *>(w);
      bool key = Fl::event() == FL_KEYBOARD || Fl::event() == FL_SHORTCUT;
      if (t->more_ && !key && Fl::event_x() >= t->split())
        t->more_();
      else if (t->click_)
        t->click_();
    });
  }
  int button_h() const { return h() - S(20); }
  int split() const { return x() + w() * 62 / 100; }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    bool on = on_ && on_(), hot = Fl::belowmouse() == this && active_r();
    static const unsigned colors[] = {0x2563EB, 0x7C3AED, 0x047857, 0xB45309, 0xBE185D, 0x0E7490,
                                      0x4F46E5, 0x0F766E};
    bool multi = Panel::instance->config().get_bool("action_center", "multicolor", false);
    Color accent = multi ? Color::hex(colors[color_ % 8]) : t.accent;
    Color bg = on ? accent : t.surface_alt;
    if (multi && !on) bg = t.surface.mix(accent, t.dark ? 0.18 : 0.08);
    if (hot || value()) bg = bg.mix(on ? t.on_accent : t.text, value() ? 0.15 : 0.07);
    Color white = Color::hex(0xFFFFFF), black = Color::hex(0x101820);
    Color ink = on ? (white.contrast(accent) >= 4.5 ? white : black) : t.text;
    if (!active_r()) ink = t.text_dim;
    int bh = button_h();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), bh, S(8), bg);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, bh - 1, S(8),
                       on ? accent.mix(t.text, 0.1) : t.border);
      double size = S(popup_style::kIcon);
      double ix = more_ ? x() + (split() - x() - size) / 2.0 : x() + (w() - size) / 2.0;
      icon_(p, ix, y() + (bh - size) / 2.0, size, ink);
      if (more_) {
        p.line(split() + 0.5, y() + S(8), split() + 0.5, y() + bh - S(8), ink.alpha(0.35));
        double cs = S(10);
        glyph::chevron(p, split() + (x() + w() - split() - cs) / 2.0, y() + (bh - cs) / 2.0, cs,
                       true, ink);
      }
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), bh, S(8));
    }
    std::string caption = caption_ ? caption_() : title_;
    text_font(std::max(S(9), t.font_size - S(2)));
    draw_text(caption, x() - S(2), y() + bh + S(1), w() + S(4), S(18), t.text, 1);
  }

 private:
  std::string title_;
  IconFn icon_;
  std::function<void()> click_;
  std::function<bool()> on_;
  StringFn caption_;
  std::function<void()> more_;
  int color_;
};

// Weather: the network reaches the internet (or NetworkManager is not there
// to tell; then a failed request decides).
bool weather_online() {
  const auto &n = SystemStatus::get().network;
  if (!n.available) return true;
  return n.connected() && !n.no_internet() && !n.portal();
}
weather::Settings weather_settings() {
  return weather::Settings::from_config(Panel::instance->config());
}
const weather::Report *weather_report() {
  return weather::Service::get().current(weather_settings(), weather_online());
}
// Seconds since the panel started, for animations.
double anim_seconds() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}


// The Action Center's clock hero (alpha52): a large clock in the [clock]
// style over the animated sky, the weather when it is known (sun, clouds,
// rain, snow, thunder, fog) or just the day or night sky (sunrise and sunset
// at the weather location, else 06:00-18:00), the date line ("Thursday,
// 22 October 2026") and the temperature, condition, place and wind. Its
// buttons (Action Center settings, show more/less) sit in the corner.
class ClockHero : public Fl_Group {
 public:
  ClockHero(int x, int y, int w, int h, const weather::Report *wx, const weather::Settings &ws,
            bool animate)
      : Fl_Group(x, y, w, h), ws_(ws), animate_(animate) {
    box(FL_NO_BOX);
    resizable(nullptr);
    clear_visible_focus();
    const auto &cfg = Panel::instance->config();
    clock_ = clockface::Settings::from_config(cfg);
    show_clock_ = cfg.get_bool("action_center", "show_clock", true);
    greet_ = cfg.get_bool("action_center", "show_greeting", true);
    clockface::DayInput in;
    in.prefer_clock = clockface::fixture_active();
    if (wx) {
      wx_ = *wx;
      weather_ = true;
      in.have_report = true;
      in.report_day = wx->is_day;
    }
    const weather::Location *loc = wx && wx->location.valid
                                       ? &wx->location
                                       : weather::Service::get().known_location();
    if (loc) {
      in.have_location = true;
      in.lat = loc->lat;
      in.lon = loc->lon;
    }
    day_ = clockface::is_daytime(in, clockface::now());
    if (!weather_) {
      // No weather: the plain day or night sky.
      wx_ = weather::Report();
      wx_.code = 0;
    }
    wx_.is_day = day_;
    std::string tip = std::string(_("Clock")) + ": " + clockface::style_name(clock_.style);
    if (weather_) {
      tip = std::string(_("Weather")) + ": " + _(weather::from_wmo(wx_.code).text) + ", " +
            weather::format_temperature(wx_.temp_c, ws_.temp);
      if (!wx_.location.city.empty()) tip += ", " + wx_.location.city;
    }
    copy_tooltip(tip.c_str());
  }
  bool weather() const { return weather_; }
  bool day() const { return day_; }
  const weather::Report &report() const { return wx_; }
  const clockface::Settings &clock() const { return clock_; }
  // 0.2 Alpha 2: a right click on the clock (not on its buttons) offers
  // the style, seconds and the blinking colon.
  int handle(int event) override {
    if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      for (int i = 0; i < children(); ++i)
        if (Fl::event_inside(child(i))) return Fl_Group::handle(event);
      Panel::instance->clock_options_menu();  // may rebuild the Action Center
      return 1;
    }
    return Fl_Group::handle(event);
  }
  // Repaints for the clock: every second for seconds and blinking colons.
  bool ticks_seconds() const {
    return show_clock_ && (clockface::ticks_seconds(clock_) ||
                           (clock_.style == clockface::Style::Analog && clock_.seconds));
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    struct tm lt = clockface::local_now();
    Color ink = Color::hex(0xFFFFFF), soft = Color::hex(0xE6EDF6), shade = Color::hex(0x0B1424);
    auto shadowed = [&shade](const std::string &s, int x, int y, int w, int h, const Color &c,
                             int align = 0) {
      sde::draw_text(s, x + 1, y + 1, w, h, shade.alpha(.55), align);
      sde::draw_text(s, x, y, w, h, c, align);
    };
    int pad = S(16), H = h();
    bool small = H < S(124);
    // The clock size follows the hero.
    int big = std::max(S(26), std::min(S(58), int(H * (small ? .36 : .34))));
    int date_h = S(20), info_h = S(20);
    bool analog = show_clock_ && clock_.style == clockface::Style::Analog;
    double ar = analog ? std::min(H / 2.0 - S(12), double(S(56))) : 0;
    double acx = x() + pad + ar, acy = y() + H / 2.0;
    int left = analog ? int(acx + ar + S(14)) : x() + pad;
    int right = x() + w() - S(12);
    bool colon = clockface::colon_visible(clock_);
    std::string suffix;
    std::string digits = clockface::time_digits(lt, clock_.h24, clock_.seconds, &suffix);
    // Vertical layout: date line, clock, weather line.
    int date_y = y() + S(small ? 8 : 12);
    int info_y = y() + H - S(small ? 8 : 12) - info_h;
    int clock_y = date_y + date_h, clock_h = std::max(S(20), info_y - clock_y);
    if (analog || !show_clock_) {
      // Text beside the face: date, then the weather lines.
      clock_y = clock_h = 0;
    }
    // A low hero (the calendar view on small screens): the clock on the
    // left over the full height, the date and weather beside it.
    bool flat = H < S(100) && clock_h > 0;
    if (flat) {
      clock_y = y();
      clock_h = H;
      big = std::min(S(44), int(H * .56));
    }
    {
      Painter p(window()->w(), window()->h());
      weather::draw_scene(p, x(), y(), w(), H, S(14), wx_, animate_ ? anim_seconds() : 2.0);
      if (analog)
        clockface::draw_analog(p, acx, acy, ar, lt, Color::hex(0xFFFFFF, .16), soft.alpha(.85),
                               ink, t.accent.mix(ink, .25), clock_.seconds, true);
      else if (show_clock_ && clock_.style == clockface::Style::Led) {
        double lh = big * .82, lw = clockface::led_width(digits, lh);
        double ly = clock_y + (clock_h - lh) / 2.0;
        // Themed LED colour, light enough for the sky.
        clockface::draw_led(p, digits, left, ly, lh, t.accent.mix(ink, .55), colon, true);
        led_right_ = int(left + lw);
      }
      p.flush();
    }
    text_font(t.font_size, true);
    std::string date = clockface::date_line(lt);
    int text_w = right - left;
    if (analog || !show_clock_) {
      // Date and time stacked beside the analog face (or alone).
      int mid = y() + H / 2;
      int line = S(small ? 20 : 24);
      int top = mid - (weather_ ? line * 3 / 2 : line);
      if (!analog && show_clock_ == false) top = date_y;
      text_font(t.font_size + (small ? 1 : 3), true);
      size_t comma = date.find(", ");
      if (text_width(date) > text_w && comma != std::string::npos && !small) {
        // "Thursday," over "22 October 2026" beside the face.
        top -= line / 2;
        shadowed(date.substr(0, comma + 1), left, top, text_w, line, ink);
        top += line;
        shadowed(date.substr(comma + 2), left, top, text_w, line, ink);
      } else {
        shadowed(date, left, top, text_w, line, ink);
      }
      top += line;
      if (analog) {
        text_font(t.font_size, false);
        shadowed(clockface::format_time(lt, clock_.h24, false), left, top, text_w, line, soft);
        top += line;
      }
      if (weather_) {
        info_line(left, top, text_w, line, shadowed, ink, soft);
      } else if (greet_) {
        text_font(t.font_size, false);
        shadowed(greeting(lt), left, top, text_w, line, soft);
      }
      draw_children();
      return;
    }
    if (!flat) {
      text_font(t.font_size + (small ? 0 : 1), true);
      shadowed(date, left, date_y, text_w, date_h, ink);
    }
    int clock_right = led_right_;
    // The clock.
    if (clock_.style == clockface::Style::Led) {
      if (!suffix.empty()) {
        text_font(t.font_size + 1, true);
        shadowed(suffix, led_right_ + S(8), clock_y, S(60), clock_h, soft);
      }
    } else {
      text_font(big, true);
      std::string time_text = clockface::format_time(lt, clock_.h24, clock_.seconds);
      std::string main = time_text, ampm;
      size_t sp = time_text.find(' ');
      if (sp != std::string::npos) {
        main = time_text.substr(0, sp);
        ampm = time_text.substr(sp + 1);
      }
      int tw = text_width(main);
      // Shadow, then the digits; the blinking colon keeps its place.
      clockface::draw_blink_text(main, left + 1, clock_y + 1, tw, clock_h, shade.alpha(.45),
                                 colon);
      clockface::draw_blink_text(main, left, clock_y, tw, clock_h, ink, colon);
      clock_right = left + tw;
      if (!ampm.empty()) {
        text_font(t.font_size + 2, true);
        shadowed(ampm, left + tw + S(8), clock_y, S(60), clock_h, soft);
        clock_right += S(8) + text_width(ampm);
      }
    }
    if (flat) {
      int rx = clock_right + S(16), rw = std::max(0, right - S(76) - rx);
      int mid = y() + H / 2;
      text_font(t.font_size, true);
      shadowed(date, rx, mid - S(22), rw, S(20), ink);
      if (weather_)
        info_line(rx, mid, rw, S(20), shadowed, ink, soft);
      else if (greet_) {
        text_font(t.font_size, false);
        shadowed(greeting(lt), rx, mid, rw, S(20), soft);
      }
      draw_children();
      return;
    }
    // The weather line shares the bottom row with the corner buttons.
    int info_w = std::max(0, text_w - S(76));
    if (weather_)
      info_line(left, info_y, info_w, info_h, shadowed, ink, soft);
    else if (greet_) {
      text_font(t.font_size, false);
      shadowed(greeting(lt), left, info_y, info_w, info_h, soft);
    }
    draw_children();
  }

 private:
  static std::string greeting(const struct tm &lt) {
    return lt.tm_hour < 5 || lt.tm_hour >= 18 ? _("Good evening")
           : lt.tm_hour < 12                  ? _("Good morning")
                                              : _("Good afternoon");
  }
  // "21°C Rain · Jakarta · 12 km/h"
  template <class F>
  void info_line(int x, int y, int w, int h, F &shadowed, const Color &ink, const Color &soft) {
    const Theme &t = Panel::instance->theme();
    std::string temp = weather::format_temperature(wx_.temp_c, ws_.temp);
    text_font(t.font_size + 2, true);
    int tw = text_width(temp) + S(8);
    shadowed(temp, x, y, std::min(tw, w), h, ink);
    std::string rest = _(weather::from_wmo(wx_.code).text);
    std::string place = wx_.location.city;
    if (!place.empty()) rest += " \xc2\xb7 " + place;
    std::string wind = weather::format_wind(wx_.wind_kmh, ws_.wind);
    text_font(t.font_size, false);
    if (text_width(rest + " \xc2\xb7 " + wind) <= w - tw) rest += " \xc2\xb7 " + wind;
    shadowed(rest, x + tw, y, std::max(0, w - tw), h, soft);
  }

  weather::Settings ws_;
  weather::Report wx_;
  clockface::Settings clock_;
  bool animate_, weather_ = false, day_ = true, show_clock_ = true, greet_ = true;
  int led_right_ = 0;
};

// Round button on the hero's sky: white icon on a translucent disc.
class HeroButton : public DrawnButton {
 public:
  HeroButton(int x, int y, int s, const std::string &tip, IconFn icon, std::function<void()> fn)
      : DrawnButton(x, y, s, s, nullptr), icon_(std::move(icon)), fn_(std::move(fn)) {
    copy_tooltip(tip.c_str());
    callback([](Fl_Widget *w, void *) { static_cast<HeroButton *>(w)->fn_(); });
  }
  void draw() override {
    bool hot = Fl::belowmouse() == this;
    Painter p(window()->w(), window()->h());
    p.circle(x() + w() / 2.0, y() + h() / 2.0, w() / 2.0,
             Color::hex(0x0B1424, value() ? .55 : hot ? .45 : .30));
    double z = S(16);
    icon_(p, x() + (w() - z) / 2.0, y() + (h() - z) / 2.0, z, Color::hex(0xFFFFFF));
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), w() / 2);
  }

 private:
  IconFn icon_;
  std::function<void()> fn_;
};

// The user in the footer: the account picture (or the initials) and the
// name (src/panel/account.h); a click opens Users & Avatar.
class UserBadge : public Fl_Widget {
 public:
  UserBadge(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { tooltip(_("Account settings")); }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    const std::string &name = panel_account::name();
    cairo_surface_t *pic = panel_account::picture(h());
    {
      Painter p(window()->w(), window()->h());
      if (hot_) p.fill_rounded(x() - S(4), y() - S(2), w() + S(4), h() + S(4), h() / 2.0 + S(2), t.hover);
      avatar::paint_circle(p, pic, x(), y(), h(), t.accent);
    }
    if (!pic) avatar::draw_initials(name, x(), y(), h(), t.on_accent);
    text_font(t.font_size, true);
    draw_text(name, x() + h() + S(10), y(), w() - h() - S(10), h(), t.text);
  }
  int handle(int e) override {
    switch (e) {
      case FL_ENTER:
      case FL_LEAVE:
        hot_ = e == FL_ENTER;
        redraw();
        return 1;
      case FL_PUSH:
        return 1;
      case FL_RELEASE:
        if (Fl::event_inside(this)) {
          if (auto *popup = dynamic_cast<PanelPopup *>(window())) popup->close();
          spawn_detached({"sde-settings", "--page", "users"});
        }
        return 1;
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  bool hot_ = false;
};

IconFn gear_icon() {
  return
      [](Painter &p, double x, double y, double s, const Color &c) { glyph::gear(p, x, y, s, c); };
}
IconFn display_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.stroke_rounded(x, y + s * .08, s, s * .64, s * .08, c, s * .08);
    p.line(x + s * .5, y + s * .72, x + s * .5, y + s * .95, c, s * .08);
    p.line(x + s * .25, y + s * .95, x + s * .75, y + s * .95, c, s * .08);
  };
}
IconFn camera_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.stroke_rounded(x + s * .04, y + s * .26, s * .92, s * .62, s * .12, c, s * .08);
    p.line(x + s * .32, y + s * .26, x + s * .4, y + s * .12, c, s * .08);
    p.line(x + s * .4, y + s * .12, x + s * .6, y + s * .12, c, s * .08);
    p.line(x + s * .6, y + s * .12, x + s * .68, y + s * .26, c, s * .08);
    p.set(c);
    cairo_set_line_width(p.cr(), s * .08);
    cairo_new_sub_path(p.cr());
    cairo_arc(p.cr(), x + s * .5, y + s * .57, s * .17, 0, 2 * M_PI);
    cairo_stroke(p.cr());
  };
}
IconFn moon_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    tray_icon::moon(p, x, y, s, c);
  };
}
// Up (expanded, click to collapse) or down chevron.
IconFn fold_icon(bool up) {
  return [up](Painter &p, double x, double y, double s, const Color &c) {
    double a = up ? .62 : .38, b = up ? .38 : .62;
    p.line(x + s * .25, y + s * a, x + s * .5, y + s * b, c, s * .1);
    p.line(x + s * .5, y + s * b, x + s * .75, y + s * a, c, s * .1);
  };
}
IconFn chevron_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    glyph::chevron(p, x + s * .2, y + s * .2, s * .6, true, c);
  };
}
IconFn sun_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) { glyph::sun(p, x, y, s, c); };
}
IconFn alarm_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    double cx = x + s * .5, cy = y + s * .55, r = s * .36;
    p.set(c);
    cairo_set_line_width(p.cr(), s * .08);
    cairo_new_sub_path(p.cr());
    cairo_arc(p.cr(), cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(p.cr());
    p.line(cx, cy, cx, cy - r * .6, c, s * .08);
    p.line(cx, cy, cx + r * .45, cy + r * .2, c, s * .08);
    p.line(x + s * .08, y + s * .24, x + s * .26, y + s * .08, c, s * .1);
    p.line(x + s * .92, y + s * .24, x + s * .74, y + s * .08, c, s * .1);
  };
}
IconFn plus_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.line(x + s * .5, y + s * .2, x + s * .5, y + s * .8, c, s * .1);
    p.line(x + s * .2, y + s * .5, x + s * .8, y + s * .5, c, s * .1);
  };
}
IconFn cross_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.line(x + s * .28, y + s * .28, x + s * .72, y + s * .72, c, s * .09);
    p.line(x + s * .72, y + s * .28, x + s * .28, y + s * .72, c, s * .09);
  };
}
IconFn power_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    glyph::power(p, x, y, s, c);
  };
}
// Media transport: 0 previous, 1 play, 2 pause, 3 next.
IconFn transport_icon(int kind) {
  return [kind](Painter &p, double x, double y, double s, const Color &c) {
    cairo_t *cr = p.cr();
    p.set(c);
    auto tri = [&](double x0, double w, bool right) {
      cairo_new_path(cr);
      if (right) {
        cairo_move_to(cr, x0, y + s * .2);
        cairo_line_to(cr, x0 + w, y + s * .5);
        cairo_line_to(cr, x0, y + s * .8);
      } else {
        cairo_move_to(cr, x0 + w, y + s * .2);
        cairo_line_to(cr, x0, y + s * .5);
        cairo_line_to(cr, x0 + w, y + s * .8);
      }
      cairo_close_path(cr);
      cairo_fill(cr);
    };
    switch (kind) {
      case 0:
        p.fill_rounded(x + s * .18, y + s * .2, s * .1, s * .6, s * .03, c);
        tri(x + s * .3, s * .5, false);
        break;
      case 1: tri(x + s * .28, s * .52, true); break;
      case 2:
        p.fill_rounded(x + s * .26, y + s * .2, s * .16, s * .6, s * .04, c);
        p.fill_rounded(x + s * .58, y + s * .2, s * .16, s * .6, s * .04, c);
        break;
      default:
        tri(x + s * .2, s * .5, true);
        p.fill_rounded(x + s * .72, y + s * .2, s * .1, s * .6, s * .03, c);
        break;
    }
  };
}
IconFn battery_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    glyph::battery(p, x, y, s, 60, false, c, c);
  };
}

// Callback ownership follows the widget, including during asynchronous rebuilds.
class CommandButton : public FlatButton {
 public:
  CommandButton(int x, int y, int w, int h, const std::string &name, std::function<void()> fn,
                bool primary = false)
      : FlatButton(x, y, w, h, name.c_str(), primary), fn_(std::move(fn)) {
    copy_label(name.c_str());
    copy_tooltip(name.c_str());
    callback([](Fl_Widget *w, void *) { static_cast<CommandButton *>(w)->fn_(); });
  }

 private:
  std::function<void()> fn_;
};
class TrayIndicator : public DrawnButton {
 public:
  TrayIndicator(int x, int y, int size, const SniHost::Item &item)
      : DrawnButton(x, y, size, size, nullptr), service_(item.service), path_(item.path) {
    copy_tooltip((item.title.empty() ? item.id : item.title).c_str());
    callback([](Fl_Widget *w, void *) { static_cast<TrayIndicator *>(w)->activate(1); });
  }
  const SniHost::Item *item() const {
    auto *h = Panel::instance->status_notifiers();
    if (!h) return nullptr;
    for (auto *i : h->visible_items())
      if (i->service == service_ && i->path == path_) return i;
    return nullptr;
  }
  void activate(int button) {
    auto *i = item();
    auto *h = Panel::instance->status_notifiers();
    if (!i || !h) return;
    int x = Fl::event_x_root(), y = Fl::event_y_root();
    if (button == 3)
      h->context_menu(*i, x, y);
    else if (button == 2)
      h->secondary_activate(*i, x, y);
    else
      h->activate(*i, x, y);
  }
  int handle(int e) override {
    if (e == FL_PUSH && Fl::event_button() != 1) {
      activate(Fl::event_button());
      return 1;
    }
    if (e == FL_MOUSEWHEEL) {
      if (auto *i = item()) Panel::instance->status_notifiers()->scroll(*i, -Fl::event_dy() * 120);
      return 1;
    }
    return DrawnButton::handle(e);
  }
  void draw() override {
    const auto &t = Panel::instance->theme();
    Painter p(window()->w(), window()->h());
    if (Fl::belowmouse() == this) p.fill_rounded(x(), y(), w(), h(), S(6), t.hover);
    auto *i = item();
    auto *surface = i ? Panel::instance->status_notifiers()->icon(*i, S(22)) : nullptr;
    if (surface)
      p.image(surface, x() + (w() - S(22)) / 2, y() + (h() - S(22)) / 2, S(22));
    else
      glyph::gear(p, x() + S(6), y() + S(6), S(20), t.text_dim);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(6));
  }

 private:
  std::string service_, path_;
};
std::string choice_label(const std::string &s) {
  std::string r;
  for (char c : s) {
    if (c == '/' || c == '&' || c == '_' || c == '\\') r += '\\';
    r += c;
  }
  return r;
}
// First installed program of |names|, or "".
std::string first_program(std::initializer_list<const char *> names) {
  for (const char *n : names)
    if (!find_program(n).empty()) return n;
  return "";
}
// The pages of sde-settings, read once from "sde-settings --help", so that a
// flyout's settings button opens its own page as soon as the Control Center has it.
std::string &settings_help() {
  static std::string text;
  return text;
}
bool settings_page(const std::string &page) {
  const std::string &h = settings_help();
  // The page list (and accepted aliases) follows the last "--page NAME",
  // up to the next option.
  size_t at = h.rfind("--page NAME");
  if (at == std::string::npos) return false;
  size_t end = h.find("\n  --", at + 11);
  std::string list = h.substr(at + 11, end == std::string::npos ? std::string::npos : end - at - 11);
  std::string word;
  for (size_t i = 0; i <= list.size(); ++i) {
    char c = i < list.size() ? list[i] : ' ';
    if (isalnum(static_cast<unsigned char>(c)) || c == '-') {
      word += c;
    } else {
      if (word == page) return true;
      word.clear();
    }
  }
  return false;
}
// The Control Center page |page|, else |fallback| when installed, else the
// Control Center overview.
std::vector<std::string> settings_for(const std::string &page, std::vector<std::string> fallback) {
  if (settings_page(page)) return {"sde-settings", "--page", page};
  if (!fallback.empty() && !fallback[0].empty() && !find_program(fallback[0]).empty())
    return fallback;
  return {"sde-settings"};
}
void bt_icon(Painter &p, double x, double y, double z, const Color &ink) {
  glyph::bluetooth(p, x, y, z, ink);
}
void media_icon(Painter &p, double x, double y, double z, const Color &ink) {
  glyph::media(p, x, y, z, ink, false);
}
void network_icon(Painter &p, double x, double y, double z, const Color &ink) {
  const auto &n = SystemStatus::get().network;
  if (n.kind == "ethernet")
    glyph::ethernet(p, x, y, z, ink);
  else
    glyph::wifi(p, x, y, z, n.kind == "wifi" ? 4 : 0, ink, n.kind.empty());
}
}  // namespace

// Layout state of the scrolled body while a page is built.
// alpha51: flyouts (|flat|) use plain text rows grouped under a dim caption
// with a thin separator between groups; the Action Center keeps cards.
struct SectionBuilder {
  QuickSettings *qs;
  int pad, inner, y;
  bool flat = false;
  int groups = 0;
  // Horizontal inset of rows inside a group.
  int inset() const { return flat ? 0 : S(6); }
  void label(std::string text, int style = 0, int height = 24) {
    new ActionLabel(pad, y, inner, S(height), [text] { return text; }, style);
    y += S(height);
  }
  ActionButton *row(std::string text, IconFn icon, std::function<void()> fn, bool enabled = true,
                    std::function<bool()> on = nullptr) {
    auto *b = new ActionButton(pad + inset(), y, inner - 2 * inset(), S(popup_style::kRow), text,
                               icon, fn, on, -1, true);
    if (!enabled) b->deactivate();
    y += S(popup_style::kRow) + S(2);
    return b;
  }
  void info(std::string text) {
    new ActionLabel(pad + inset() + S(8), y, inner - 2 * inset() - S(16), S(22),
                    [text] { return text; }, 2);
    y += S(22);
  }
  ActionCard *start_card(std::string title) {
    auto *card = new ActionCard(pad, y, inner, flat, flat && groups > 0);
    ++groups;
    y += flat ? (groups > 1 ? S(6) : 0) : S(6);
    if (!title.empty()) {
      new ActionLabel(pad + inset() + S(8), y, inner - 2 * inset() - S(16),
                      S(popup_style::kSection), [title] { return title; }, 5);
      y += S(popup_style::kSection);
    }
    return card;
  }
  void end_card(ActionCard *card) {
    card->size(inner, y - card->y() + (flat ? 0 : S(4)));
    card->end();
    y += flat ? S(4) : S(8);
  }
  // Closes the popup and starts |cmd|.
  std::function<void()> launch(std::vector<std::string> cmd) {
    QuickSettings *q = qs;
    return [q, cmd] {
      q->close();
      spawn_detached(cmd);
    };
  }
  // Settings link at the end of a flyout.
  void settings_row(const std::string &title, std::vector<std::string> cmd) {
    if (flat) {
      // A separator, then the settings link as a plain row.
      auto *card = new ActionCard(pad, y, inner, true, groups > 0);
      ++groups;
      y += S(6);
      row(title, gear_icon(), launch(std::move(cmd)));
      card->size(inner, y - card->y());
      card->end();
      return;
    }
    y += S(2);
    row(title, gear_icon(), launch(std::move(cmd)));
  }
};

bool QuickSettings::valid_section(const std::string &section) {
  static const std::vector<std::string> valid = {"network", "bluetooth", "sound",  "media",
                                                 "tray",    "power",     "drives", "keyboard"};
  return std::find(valid.begin(), valid.end(), section) != valid.end();
}
std::string QuickSettings::section_title(const std::string &section) {
  if (section == "network") return _("Network");
  if (section == "bluetooth") return _("Bluetooth");
  if (section == "sound") return _("Sound");
  if (section == "media") return _("Media");
  if (section == "tray") return _("Tray");
  if (section == "power") return _("Power");
  if (section == "drives") return _("Drives");
  if (section == "keyboard") return _("Keyboard");
  return _("Action Center");
}

QuickSettings::QuickSettings(Mode mode)
    : PanelPopup(mode == FLYOUT ? "Flyout" : "Action Center"), mode_(mode) {
  end();
  if (mode_ == ACTION_CENTER) section_ = "overview";
}
void QuickSettings::select_section(const std::string &section) {
  if (mode_ == ACTION_CENTER) return;
  section_ = valid_section(section) ? section : "sound";
  // The window title names the flyout for window lists and UI tests.
  // Window titles stay untranslated, like "Action Center".
  static const std::map<std::string, const char *> names = {
      {"network", "Network"}, {"bluetooth", "Bluetooth"}, {"sound", "Sound"},
      {"media", "Media"},     {"tray", "Tray"},           {"power", "Power"},
      {"drives", "Drives"},   {"keyboard", "Keyboard"}};
  copy_label(names.at(section_));
  if (shown()) status_changed();
}
// While a popup with the brightness slider is open, follow changes made
// elsewhere (brightness keys, the power manager).
static void poll_brightness(void *data) {
  auto *q = static_cast<QuickSettings *>(data);
  if (!q->is_open()) return;
  SystemStatus::get().refresh_brightness();
  Fl::repeat_timeout(1.5, poll_brightness, data);
}

void QuickSettings::weather_tick(void *data) {
  auto *q = static_cast<QuickSettings *>(data);
  if (!q->is_open() || !q->clock_) return;
  q->weather_frame_ = true;
  q->clock_->redraw();
  if (q->weather_fps_ > 1) {
    Fl::repeat_timeout(1.0 / q->weather_fps_, weather_tick, data);
    return;
  }
  // A still sky: the clock's next second.
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count() %
            1000;
  Fl::add_timeout((1000 - ms) / 1000.0 + 0.02, weather_tick, data);
}
// The seek bar and times follow a playing track once a second.
void QuickSettings::media_tick(void *data) {
  auto *q = static_cast<QuickSettings *>(data);
  if (!q->is_open()) return;
  if (q->media_slider_ && Fl::pushed() != q->media_slider_) {
    for (const auto &p : PanelControls::get().players())
      if (p.service == q->media_service_) {
        q->media_slider_->value(double(p.position_now(mono_ms()) / 1000000));
        // The slider and the time labels below it.
        q->damage_rect(q->media_slider_->x(), q->media_slider_->y(), q->media_slider_->w(),
                       q->media_slider_->h() + S(20));
        break;
      }
  }
  Fl::repeat_timeout(1.0, media_tick, data);
}

void QuickSettings::closed() {
  Fl::remove_timeout(poll_brightness, this);
  Fl::remove_timeout(weather_tick, this);
  Fl::remove_timeout(media_tick, this);
  brightness_open_ = false;
  PanelControls::get().stop_scan();
  calendar_view_ = false;
  media_view_ = false;
}
bool QuickSettings::expanded() const {
  return calendar_view_ || Panel::instance->config().get_bool("action_center", "expanded", true);
}
void QuickSettings::toggle_expanded() {
  bool next = !expanded();
  calendar_view_ = false;
  Panel::instance->set_setting("action_center", "expanded", next ? "true" : "false");
  status_changed();
}
std::string QuickSettings::structure() const {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  const auto &cfg = Panel::instance->config();
  std::ostringstream out;
  out << Alarms::get().revision() << '|' << agenda_year_ << '-' << agenda_month_ << '-'
      << agenda_day_ << agenda_form_ << agenda_edit_ << agenda_error_ << power_open_
      << brightness_open_
      << s.network.connectivity << s.network.check_enabled << '|';
  out << int(mode_) << section_ << calendar_view_ << s.network.available << s.network.has_wifi
      << s.network.wifi_enabled << s.network.connection << s.network.kind;
  for (const auto &n : s.wifi) out << '|' << n.ssid << n.active << n.secure;
  out << s.audio.available << s.audio.default_sink << s.brightness.available << s.battery.present;
  for (const auto &sink : s.audio.sinks) out << '|' << sink.first << sink.second;
  out << s.message.empty() << c.message.empty();
  out << c.mic_available << c.mic_muted << c.default_source << c.bluetooth_available
      << c.bluetooth_busy << c.adapter_path << c.airplane << c.keyboard;
  for (const auto &source : c.sources) out << '|' << source.first << source.second;
  for (const auto &a : c.adapters) out << '|' << a.path << a.name << a.powered << a.discovering;
  for (const auto &d : c.bluetooth) out << '|' << d.path << d.name << d.paired << d.connected;
  for (const auto &p : c.players())
    out << '|' << p.service << p.name << p.title << p.artist << p.status << p.control << p.play
        << p.pause << p.next << p.previous << p.raise << p.album << p.art_url << p.length_us
        << p.seek;
  if (mode_ == ACTION_CENTER) {
    // The weather picture (condition, temperature, place) and its settings.
    if (const auto *w = weather_report())
      out << "|wx" << w->code << w->is_day << std::lround(w->temp_c * 10) << w->location.city;
    for (const char *key : {"enabled", "unit", "wind_unit", "animation", "location"})
      out << '|' << cfg.get("weather", key);
    for (const char *key : {"style", "seconds", "blink", "format"})
      out << '|' << cfg.get("clock", key);
    out << "|players" << c.players().size();
    for (const char *key : {"enabled", "schedule", "from", "to"})
      out << '|' << cfg.get("night_light", key);
    out << '|' << cfg.get("power", "idle_dim_battery") << cfg.get("brightness", "external_monitor");
    const auto &tz = tzsync::Sync::get();
    out << "|tz" << int(tz.state()) << tz.message() << cfg.get("clock", "auto_timezone")
        << tzsync::current_zone() << weather_online();
  }
  for (const auto &a : c.streams) out << '|' << a.id << a.app << a.title << a.muted;
  for (const auto &d : c.drives) out << '|' << d.path << d.mountpoint();
  for (const auto &p : c.power_profiles) out << '|' << p;
  out << c.power_profile;
  for (const auto &d : c.devices)
    out << '|' << d.device << d.state << d.connection << d.ip << d.gateway << d.dns;
  if (mode_ == FLYOUT && section_ == "power")  // the state line names the estimate
    out << "|bat" << s.battery.percent << int(s.battery.phase) << s.battery.minutes
        << s.battery.calculating << s.battery.model;
  if (mode_ == FLYOUT && section_ == "tray") {
    if (auto *h = Panel::instance->status_notifiers())
      for (auto *i : h->visible_items()) out << '|' << i->service << i->path << i->title << i->status;
    if (auto *tray = Panel::instance->legacy_tray()) out << tray->count();
  }
  for (const char *key :
       {"show_clock", "show_greeting", "clock_style", "multicolor", "move_status", "show_media",
        "show_app_volume", "show_microphone", "show_tray", "tile_columns", "expanded",
        "show_calendar", "show_notifications", "show_drives"})
    out << '|' << cfg.get("action_center", key);
  for (const char *key : {"style", "position", "height", "width_percent", "clock_24h",
                          "clock_seconds", "clock_blink"})
    out << '|' << cfg.get("panel", key);
  out << cfg.get("appearance", "accent") << cfg.get("appearance", "theme")
      << cfg.get("appearance", "style") << cfg.get("appearance", "scale")
      << cfg.get("notifications", "do_not_disturb") << cfg.get("input", "keyboard_layout")
      << cfg.get("input", "keyboard_layout2");
  return out.str();
}
void QuickSettings::prepare() {
  SystemStatus::get().refresh_brightness();
  Fl::remove_timeout(poll_brightness, this);
  Fl::add_timeout(1.5, poll_brightness, this);
  SystemStatus::get().message.clear();
  PanelControls::get().message.clear();
  scroll_to_calendar_ = calendar_view_;
  power_open_ = false;
  if (mode_ == ACTION_CENTER) weather::Service::get().request(weather_settings(), weather_online());
  if (mode_ == ACTION_CENTER) {
    // The agenda starts on today.
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    agenda_year_ = shown_year_ = lt.tm_year + 1900;
    agenda_month_ = shown_month_ = lt.tm_mon + 1;
    agenda_day_ = lt.tm_mday;
    agenda_form_ = false;
    agenda_edit_ = 0;
    agenda_error_.clear();
  }
  // Opening the Network flyout asks NetworkManager for a fresh check.
  if (mode_ == FLYOUT && section_ == "network") SystemStatus::get().check_connectivity();
  rebuild();
}
void QuickSettings::status_changed() {
  if (!shown()) return;
  if (structure() == structure_) {
    sync();
    return;
  }
  if (!rebuild_pending_) {
    rebuild_pending_ = true;
    Fl::add_timeout(0, rebuild_timeout, this);
  }
}
void QuickSettings::rebuild_timeout(void *data) {
  auto *self = static_cast<QuickSettings *>(data);
  if (!self->shown()) {
    self->rebuild_pending_ = false;
    return;
  }
  if (Fl::pushed() && Fl::pushed()->window() == self) {
    Fl::add_timeout(.1, rebuild_timeout, data);
    return;
  }
  auto *sc = self->scroll_;
  int scroll = sc ? std::max(0, sc->yposition()) : 0;
  bool focus = Fl::focus() && Fl::focus()->window() == self;
  std::string focus_name = focused_name(self);
  self->rebuild();
  // Flyouts keep their scroll position (the Action Center never scrolls).
  sc = self->scroll_;
  if (sc) sc->scroll_to(0, std::min(scroll, std::max(0, self->content_height_ - sc->h())));
  if (sc && self->reveal_brightness_ && self->brightness_card_) {
    // Just opened: bring the settings card into view (its top first).
    auto *c = self->brightness_card_;
    int y = sc->yposition();
    int bottom = c->y() + c->h() - (sc->y() + sc->h());
    int top = c->y() - sc->y() - S(8);
    if (bottom > 0) y += std::min(bottom, std::max(0, top));
    sc->scroll_to(0, std::max(0, std::min(y, self->content_height_ - sc->h())));
  }
  self->reveal_brightness_ = false;
  self->reposition();
  if (!focus_name.empty())
    restore_focus(self, focus_name);
  else if (focus && self->body_ && self->body_->children())
    self->body_->child(0)->take_focus();
  else if (focus && self->children())
    self->child(0)->take_focus();
}
void QuickSettings::sync() {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  if (volume_ && Fl::pushed() != volume_) volume_->value(s.audio.volume);
  if (brightness_ && Fl::pushed() != brightness_) brightness_->value(s.brightness.percent());
  if (microphone_ && Fl::pushed() != microphone_) microphone_->value(c.mic_volume);
  for (const auto &a : c.streams) {
    auto i = stream_sliders_.find(a.id);
    if (i != stream_sliders_.end() && Fl::pushed() != i->second) i->second->value(a.volume);
  }
  redraw();
}
void QuickSettings::sync_native_tray() {
  if (!shown() || Panel::instance->tray_host() != this) return;
  auto *tray = Panel::instance->legacy_tray();
  if (!tray) return;
  if (tray_box_ && fl_xid(this)) {
    tray->set_background(Panel::instance->theme().surface_alt.fl() >> 8);
    tray->place_in(fl_xid(this), tray_box_->x(), tray_box_->y(), tray_columns_,
                   body_->y() - tray_box_->y(), body_->h());
  } else
    tray->hide();
}

// ------------------------------------------------------------ shared rows

void QuickSettings::volume_row(SectionBuilder &b, bool more) {
  const auto &s = SystemStatus::get();
  int x = b.pad + b.inset(), right = b.pad + b.inner - b.inset(), rh = S(popup_style::kRow);
  auto *mute = new ActionButton(
      x, b.y, S(32), rh, "",
      [](Painter &p, double x, double y, double z, const Color &ink) {
        const auto &a = SystemStatus::get().audio;
        glyph::speaker(p, x, y, z, a.volume > 66 ? 3 : a.volume > 33 ? 2 : a.volume > 0 ? 1 : 0,
                       a.muted, ink);
      },
      [] { SystemStatus::get().toggle_mute(); }, [] { return SystemStatus::get().audio.muted; },
      -1, true);
  mute->copy_tooltip(_("Mute"));
  int more_w = more ? S(32) : 0;
  int pct = S(40);
  volume_ = new Slider(x + S(36), b.y, right - more_w - pct - (x + S(36)) - S(4), rh);
  volume_->bounds(0, 100);
  volume_->value(s.audio.volume);
  volume_->tooltip(_("Output volume"));
  volume_->callback([](Fl_Widget *w, void *) {
    SystemStatus::get().set_volume(int(static_cast<Fl_Slider *>(w)->value()));
  });
  new ActionLabel(
      right - more_w - pct, b.y, pct, rh,
      [] { return std::to_string(SystemStatus::get().audio.volume) + "%"; }, 4);
  if (more)
    icon_button(right - more_w + S(2), b.y, more_w - S(2), rh,
                _("Sound output and applications"), chevron_icon(),
                [] { later([] { Panel::instance->open_flyout("sound"); }); });
  b.y += rh + S(4);
}

namespace {
// Settings changed by sliders are written once the slider rests.
std::map<std::pair<std::string, std::string>, std::string> &pending_settings() {
  static std::map<std::pair<std::string, std::string>, std::string> m;
  return m;
}
void flush_settings(void *) {
  auto m = std::move(pending_settings());
  pending_settings().clear();
  for (const auto &kv : m) Panel::instance->set_setting(kv.first.first, kv.first.second, kv.second);
  nightlight::Controller::get().update();
}
void save_later(const std::string &section, const std::string &key, const std::string &value) {
  pending_settings()[{section, key}] = value;
  Fl::remove_timeout(flush_settings);
  Fl::add_timeout(0.5, flush_settings);
}
void save_now(const std::string &section, const std::string &key, const std::string &value) {
  pending_settings().erase({section, key});
  Panel::instance->set_setting(section, key, value);
}
}  // namespace

void QuickSettings::set_brightness_menu(bool open) {
  brightness_open_ = open;
  reveal_brightness_ = open;
  status_changed();
}

void QuickSettings::brightness_row(SectionBuilder &b) {
  const auto &s = SystemStatus::get();
  int x = b.pad + b.inset(), right = b.pad + b.inner - b.inset(), pct = S(40), more_w = S(32),
      rh = S(popup_style::kRow);
  auto *icon = new ActionButton(x, b.y, S(32), rh, "", sun_icon(), [] {}, nullptr, -1, true);
  icon->clear_visible_focus();
  // The gear at the end opens brightness and Night Light settings.
  bool open = brightness_open_;
  auto *more = new ActionButton(
      right - more_w + S(2), b.y, more_w - S(2), rh, "", gear_icon(),
      [this, open] { later([this, open] { set_brightness_menu(!open); }); },
      [this] { return brightness_open_; }, -1, true);
  more->copy_tooltip(_("Brightness settings"));
  right -= more_w;
  brightness_ = new Slider(x + S(36), b.y, right - pct - (x + S(36)) - S(4), rh);
  brightness_->bounds(1, 100);
  brightness_->value(s.brightness.percent());
  brightness_->tooltip(_("Brightness"));
  brightness_->callback([](Fl_Widget *w, void *) {
    SystemStatus::get().set_brightness(int(static_cast<Fl_Slider *>(w)->value()));
  });
  new ActionLabel(
      right - pct, b.y, pct, rh,
      [] { return std::to_string(SystemStatus::get().brightness.percent()) + "%"; }, 4);
  b.y += rh + S(4);
}

void QuickSettings::brightness_settings(SectionBuilder &b) {
  const auto &cfg = Panel::instance->config();
  const auto &t = Panel::instance->theme();
  int pad = b.pad, inner = b.inner;
  auto *card = b.start_card(_("Brightness and Night Light"));
  brightness_card_ = card;
  icon_button(pad + inner - S(46), card->y() + S(6), S(34), S(30), _("Close brightness settings"),
              cross_icon(), [this] { later([this] { set_brightness_menu(false); }); });
  auto refresh = [this] { later([this] { status_changed(); }); };
  // Night Light: warm colors through the XRandR gamma ramps.
  auto nl = nightlight::Settings::from_config(cfg);
  b.row(_("Night Light"), moon_icon(),
        [refresh] {
          bool on = !nightlight::Settings::from_config(Panel::instance->config()).enabled;
          save_now("night_light", "enabled", on ? "true" : "false");
          nightlight::Controller::get().update();
          refresh();
        },
        true, [] { return nightlight::Settings::from_config(Panel::instance->config()).enabled; });
  int x = pad + S(14), w = inner - S(28), lw = S(92), vw = S(64);
  new ActionLabel(x, b.y, lw, S(30), [] { return std::string(_("Warmth")); }, 2);
  auto *warm = new Slider(x + lw, b.y, w - lw - vw, S(30));
  // Right is warmer.
  warm->bounds(6500, 2500);
  warm->step(100);
  warm->value(nl.temperature);
  warm->tooltip(_("Night Light warmth"));
  if (!nl.enabled) warm->deactivate();
  warm->callback([](Fl_Widget *wd, void *) {
    int k = int(static_cast<Fl_Slider *>(wd)->value());
    save_later("night_light", "temperature", std::to_string(k));
    wd->window()->redraw();
  });
  new ActionLabel(
      x + w - vw, b.y, vw, S(30),
      [warm] { return std::to_string(int(warm->value())) + " K"; }, 4);
  b.y += S(34);
  {
    new ActionLabel(x, b.y, lw, S(30), [] { return std::string(_("Schedule")); }, 2);
    auto *sched = new Fl_Choice(x + lw, b.y, w - lw, S(30));
    sched->tooltip(_("Night Light schedule"));
    sched->textsize(t.font_size);
    sched->add(choice_label(_("Sunset to sunrise")).c_str());
    sched->add(choice_label(_("Always on")).c_str());
    sched->add(choice_label(_("Custom hours")).c_str());
    sched->value(nl.schedule == "always" ? 1 : nl.schedule == "custom" ? 2 : 0);
    if (!nl.enabled) sched->deactivate();
    sched->callback([](Fl_Widget *wd, void *d) {
      static const char *names[] = {"sunset", "always", "custom"};
      int i = std::max(0, std::min(2, static_cast<Fl_Choice *>(wd)->value()));
      save_now("night_light", "schedule", names[i]);
      nightlight::Controller::get().update();
      later([d] { static_cast<QuickSettings *>(d)->status_changed(); });
    }, this);
    b.y += S(34);
  }
  if (nl.schedule == "custom") {
    int half = (w - lw - S(8)) / 2;
    new ActionLabel(x, b.y, lw, S(30), [] { return std::string(_("From – to")); }, 2);
    for (int which = 0; which < 2; ++which) {
      auto *c = new Fl_Choice(x + lw + which * (half + S(8)), b.y, half, S(30));
      c->tooltip(which ? _("Night Light ends") : _("Night Light starts"));
      c->textsize(t.font_size);
      int cur = which ? nl.to : nl.from, sel = 0;
      for (int i = 0; i < 48; ++i) {
        c->add(nightlight::format_time(i * 30).c_str());
        if (std::abs(i * 30 - cur) < 15) sel = i;
      }
      c->value(sel);
      if (!nl.enabled) c->deactivate();
      c->callback(
          [](Fl_Widget *wd, void *data) {
            bool end = data != nullptr;
            int i = static_cast<Fl_Choice *>(wd)->value();
            save_now("night_light", end ? "to" : "from", nightlight::format_time(i * 30));
            nightlight::Controller::get().update();
          },
          which ? reinterpret_cast<void *>(1) : nullptr);
    }
    b.y += S(34);
  } else if (nl.enabled && nl.schedule == "sunset") {
    int from, to;
    double lat = 0, lon = 0;
    bool have = false;
    if (const auto *r = weather::Service::get().current(weather_settings(), true))
      if (r->location.valid) {
        have = true;
        lat = r->location.lat;
        lon = r->location.lon;
      }
    nightlight::window_for(nl, time(nullptr), have, lat, lon, from, to);
    b.info(format(_("On from %s to %s"), nightlight::format_time(from).c_str(),
                  nightlight::format_time(to).c_str()));
  }
  b.y += S(4);
  // Dimming by sde-power ([power] idle_dim_battery and dim_level).
  bool dim = cfg.get_int("power", "idle_dim_battery", 120) > 0;
  b.row(_("Auto-dim on battery"), battery_icon(),
        [refresh] {
          const auto &c = Panel::instance->config();
          int now = c.get_int("power", "idle_dim_battery", 120);
          if (now > 0) {
            save_now("brightness", "auto_dim_seconds", std::to_string(now));
            save_now("power", "idle_dim_battery", "0");
            save_now("brightness", "auto_dim_battery", "false");
          } else {
            int back = std::max(30, c.get_int("brightness", "auto_dim_seconds", 120));
            save_now("power", "idle_dim_battery", std::to_string(back));
            save_now("brightness", "auto_dim_battery", "true");
          }
          refresh();
        },
        true, [] { return Panel::instance->config().get_int("power", "idle_dim_battery", 120) > 0; });
  new ActionLabel(x, b.y, lw, S(30), [] { return std::string(_("Dim level")); }, 2);
  auto *level = new Slider(x + lw, b.y, w - lw - vw, S(30));
  level->bounds(10, 90);
  level->step(5);
  level->value(std::max(10, std::min(90, cfg.get_int("power", "dim_level", 30))));
  level->tooltip(_("Dim level"));
  if (!dim) level->deactivate();
  level->callback([](Fl_Widget *wd, void *) {
    save_later("power", "dim_level", std::to_string(int(static_cast<Fl_Slider *>(wd)->value())));
    wd->window()->redraw();
  });
  new ActionLabel(
      x + w - vw, b.y, vw, S(30),
      [level] { return std::to_string(int(level->value())) + "%"; }, 4);
  b.y += S(34);
  // External monitors through DDC/CI.
  bool ddc = !find_program("ddcutil").empty();
  auto *ext = b.row(ddc ? _("External monitor (DDC/CI)") : _("External monitor: install ddcutil"),
                    display_icon(),
                    [refresh] {
                      const auto &c = Panel::instance->config();
                      bool on = !c.get_bool("brightness", "external_monitor",
                                            c.get_bool("action_center", "external_brightness", true));
                      save_now("brightness", "external_monitor", on ? "true" : "false");
                      SystemStatus::get().reset_external_brightness();
                      refresh();
                    },
                    ddc, [ddc] {
                      const auto &c = Panel::instance->config();
                      return ddc && c.get_bool("brightness", "external_monitor",
                                        c.get_bool("action_center", "external_brightness", true));
                    });
  ext->copy_tooltip(_("External monitor brightness"));
  b.row(_("Display settings…"), gear_icon(), b.launch({"sde-settings", "--page", "display"}));
  b.end_card(card);
}

void QuickSettings::player_card(SectionBuilder &b, const MediaPlayer &p, bool mini) {
  const auto &t = Panel::instance->theme();
  int pad = b.pad, inner = b.inner;
  auto *card = b.start_card("");
  // |mini| (alpha52, small screens): the transport buttons beside the
  // title and no time labels, about 90 px instead of 150.
  int x = pad + S(12), w = inner - S(24), art = mini ? S(40) : S(48);
  int row_y = b.y;
  // Album art (mpris:artUrl) or a note on an accent tile.
  std::string url = p.art_url;
  struct Cover : Fl_Widget {
    std::string url;
    Cover(int X, int Y, int W, int H, std::string u) : Fl_Widget(X, Y, W, H), url(std::move(u)) {}
    void draw() override {
      const Theme &th = Panel::instance->theme();
      Painter pt(window()->w(), window()->h());
      cairo_surface_t *img = url.empty() ? nullptr : art::get(url, w(), [] {
        SystemStatus::get().changed_now();
      });
      if (img) {
        cairo_t *cr = pt.cr();
        cairo_save(cr);
        pt.rounded_rect(x(), y(), w(), h(), S(10));
        cairo_clip(cr);
        cairo_set_source_surface(cr, img, x(), y());
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        pt.fill_rounded(x(), y(), w(), h(), S(10), th.accent);
        double g = w() * .5;
        glyph::media(pt, x() + (w() - g) / 2, y() + (h() - g) / 2, g, th.on_accent);
      }
    }
  };
  auto *cv = new Cover(x, b.y, art, art, url);
  cv->clear_visible_focus();
  // Title, then the artist; a video without one shows the application.
  std::string title = p.title.empty() ? p.name : p.title;
  std::string artist = !p.artist.empty() ? p.artist : p.title.empty() ? std::string() : p.name;
  if (!p.album.empty() && !p.artist.empty()) artist += " — " + p.album;
  int tx = x + art + S(12), tw = w - art - S(12) - (mini ? 3 * S(34) + S(4) : 0);
  int ty = mini ? b.y - S(1) : b.y + S(3);
  auto *tl = new ActionLabel(tx, ty, tw, S(22), [title] { return title; }, 1);
  tl->copy_tooltip((std::string(_("Now playing")) + ": " + title).c_str());
  auto *al = new ActionLabel(tx, ty + S(mini ? 20 : 22), tw, S(20), [artist] { return artist; }, 2);
  if (!artist.empty()) al->copy_tooltip((std::string(_("Artist")) + ": " + artist).c_str());
  b.y += art + S(mini ? 4 : 8);
  // Seek bar with the elapsed and total time.
  std::string service = p.service;
  media_service_ = service;
  auto *bar = new Slider(x - S(4), b.y, w + S(8), S(24));
  bar->bounds(0, std::max<long long>(1, p.length_us / 1000000));
  bar->value(double(p.position_now(mono_ms()) / 1000000));
  bar->tooltip(_("Playback position"));
  bar->when(FL_WHEN_RELEASE);
  bar->callback([](Fl_Widget *wd, void *d) {
    // The player this card shows (kept on the popup across rebuilds).
    auto *q = static_cast<QuickSettings *>(d);
    auto *sl = static_cast<Fl_Slider *>(wd);
    PanelControls::get().media_seek(q->media_service_, (long long)(sl->value() * 1000000.0));
  }, this);
  if (!(p.control && p.seek && p.length_us > 0)) bar->deactivate();
  media_slider_ = bar;
  b.y += S(24);
  auto time_of = [service](bool total) {
    return [service, total] {
      for (const auto &pl : PanelControls::get().players())
        if (pl.service == service)
          return pl.length_us > 0 ? format_media_time(total ? pl.length_us
                                                            : pl.position_now(mono_ms()))
                                  : std::string(total ? "" : "");
      return std::string();
    };
  };
  if (!mini) {
    new ActionLabel(x, b.y, w / 2, S(18), time_of(false), 2);
    new ActionLabel(x + w / 2, b.y, w - w / 2, S(18),
                    [f = time_of(true)] { return f(); }, 4);
    b.y += S(20);
  }
  // Only previous, play/pause and next.
  int bw = mini ? S(34) : S(40), gap = mini ? S(1) : S(12),
      cx = mini ? x + w - 3 * bw - 2 * gap : x + (w - 3 * bw - 2 * gap) / 2;
  int by = mini ? row_y + (art - S(32)) / 2 : b.y;
  bool playing = p.status == "Playing";
  struct B {
    const char *tip, *cmd;
    int icon;
    bool on;
  } buttons[] = {{N_("Previous"), "Previous", 0, p.control && p.previous},
                 {playing ? N_("Pause") : N_("Play"), "PlayPause", playing ? 2 : 1,
                  p.control && (playing ? p.pause : p.play)},
                 {N_("Next"), "Next", 3, p.control && p.next}};
  for (int i = 0; i < 3; ++i) {
    std::string cmd = buttons[i].cmd;
    auto *btn = new ActionButton(cx + i * (bw + gap), by, bw, S(32), "",
                                 transport_icon(buttons[i].icon),
                                 [service, cmd] { PanelControls::get().media_command(service, cmd); },
                                 i == 1 ? std::function<bool()>([] { return true; }) : nullptr, -1,
                                 true);
    btn->copy_tooltip(_(buttons[i].tip));
    if (!buttons[i].on) btn->deactivate();
  }
  (void)t;
  if (!mini) b.y += S(34);
  b.end_card(card);
}

// ---------------------------------------------------------- flyout pages

void QuickSettings::sound_section(SectionBuilder &b) {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  const auto &t = Panel::instance->theme();
  const auto &cfg = Panel::instance->config();
  int pad = b.pad, inner = b.inner;
  auto *card = b.start_card(_("Output"));
  if (s.audio.available) {
    volume_row(b, false);
    if (!s.audio.sinks.empty()) {
      auto *choice = new Fl_Choice(pad + b.inset() + S(4), b.y, inner - 2 * b.inset() - S(8), S(28));
      choice->textsize(t.font_size);
      choice->tooltip(_("Output device"));
      for (size_t i = 0; i < s.audio.sinks.size(); ++i) {
        choice->add(choice_label(s.audio.sinks[i].second).c_str());
        if (s.audio.sinks[i].first == s.audio.default_sink) choice->value(int(i));
      }
      choice->callback([](Fl_Widget *w, void *) {
        int i = static_cast<Fl_Choice *>(w)->value();
        const auto &v = SystemStatus::get().audio.sinks;
        if (i >= 0 && i < int(v.size())) SystemStatus::get().set_default_sink(v[size_t(i)].first);
      });
      b.y += S(32);
    }
  } else {
    b.info(_("Sound service is unavailable"));
  }
  b.end_card(card);
  if (c.mic_available && cfg.get_bool("action_center", "show_microphone", true)) {
    card = b.start_card(_("Microphone"));
    b.row(
        c.mic_muted ? _("Unmute microphone") : _("Mute microphone"),
        [](Painter &p, double x, double y, double z, const Color &ink) {
          glyph::microphone(p, x, y, z, ink);
        },
        [] { PanelControls::get().microphone_mute(); });
    microphone_ = new Slider(pad + b.inset() + S(4), b.y, inner - 2 * b.inset() - S(8), S(28));
    microphone_->bounds(0, 100);
    microphone_->value(c.mic_volume);
    microphone_->tooltip(_("Microphone volume"));
    microphone_->callback([](Fl_Widget *w, void *) {
      PanelControls::get().microphone_volume(int(static_cast<Fl_Slider *>(w)->value()));
    });
    b.y += S(32);
    if (!c.sources.empty()) {
      auto *choice = new Fl_Choice(pad + b.inset() + S(4), b.y, inner - 2 * b.inset() - S(8), S(28));
      choice->textsize(t.font_size);
      choice->tooltip(_("Input device"));
      for (size_t i = 0; i < c.sources.size(); ++i) {
        choice->add(choice_label(c.sources[i].second).c_str());
        if (c.sources[i].first == c.default_source) choice->value(int(i));
      }
      choice->callback([](Fl_Widget *w, void *) {
        int i = static_cast<Fl_Choice *>(w)->value();
        const auto &v = PanelControls::get().sources;
        if (i >= 0 && i < int(v.size())) PanelControls::get().microphone_source(v[size_t(i)].first);
      });
      b.y += S(32);
    }
    b.end_card(card);
  }
  if (s.audio.available && cfg.get_bool("action_center", "show_app_volume", true)) {
    card = b.start_card(_("Applications"));
    if (c.streams.empty()) b.info(_("No applications are playing sound"));
    int count = 0;
    for (const auto &a : c.streams) {
      if (++count > 32) break;
      b.info(a.app + (a.title.empty() ? "" : " · " + a.title));
      auto *slider = new Slider(pad + b.inset() + S(4), b.y, inner - 2 * b.inset() - S(48), S(28));
      slider->bounds(0, 100);
      slider->value(a.volume);
      slider->copy_tooltip(a.app.c_str());
      slider->callback(
          [](Fl_Widget *w, void *id) {
            PanelControls::get().set_stream_volume(unsigned(reinterpret_cast<uintptr_t>(id)),
                                                   int(static_cast<Fl_Slider *>(w)->value()));
          },
          reinterpret_cast<void *>(uintptr_t(a.id)));
      stream_sliders_[a.id] = slider;
      auto *mute = new ActionButton(
          pad + inner - b.inset() - S(36), b.y, S(36), S(28), "",
          [muted = a.muted](Painter &p, double x, double y, double z, const Color &ink) {
            glyph::speaker(p, x, y, z, 3, muted, ink);
          },
          [id = a.id, muted = !a.muted] { PanelControls::get().mute_stream(id, muted); },
          [muted = a.muted] { return muted; }, -1, true);
      mute->copy_tooltip(a.muted ? _("Unmute application") : _("Mute application"));
      b.y += S(32);
      auto player = match_media_player(a, c.players());
      if (!player.empty())
        b.row(_("Open player"), media_icon,
              [player] { PanelControls::get().media_command(player, "Raise"); });
    }
    b.end_card(card);
  }
  std::string mixer = first_program({"pavucontrol", "pwvucontrol"});
  b.settings_row(_("Sound settings"), settings_for("sound", {mixer}));
}

void QuickSettings::network_section(SectionBuilder &b) {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  const auto &n = s.network;
  auto *card = b.start_card(_("Status"));
  if (n.connected()) b.info(n.connection);
  {
    // Named for window inspection; the text follows NetworkManager live.
    auto *state = new ActionLabel(b.pad + S(12), b.y, b.inner - S(24), S(26),
                                  [] { return SystemStatus::get().network.status_text(); }, 1);
    state->copy_label(n.status_text().c_str());
    state->labeltype(FL_NO_LABEL);
    b.y += S(26);
  }
  if (n.no_internet()) {
    b.info(_("The network has no internet access."));
    b.info(_("Troubleshoot renews the address and DNS."));
  } else if (n.portal()) {
    b.info(_("Sign in to this network to go online."));
  } else if (n.connected() && n.connectivity == NetworkState::UNKNOWN && !n.check_enabled) {
    b.info(_("NetworkManager does not check internet access."));
  }
  if (n.portal())
    b.row(_("Open sign-in page"), network_icon, [this] {
      close();
      SystemStatus::get().open_portal();
    });
  if (n.connected()) {
    b.row(_("Troubleshoot"),
          [](Painter &p, double x, double y, double z, const Color &ink) {
            glyph::gear(p, x, y, z, ink);
          },
          [] { PanelControls::get().troubleshoot_network(); });
    b.row(_("Check internet connection"), network_icon,
          [] {
            auto &s = SystemStatus::get();
            s.message = _("Checking the internet connection...");
            s.check_connectivity(true, [](int) {
              auto &st = SystemStatus::get();
              st.message = st.network.status_text();
              st.changed_now();
            });
          });
  }
  if (s.network.available && s.network.has_wifi) {
    b.row(s.network.wifi_enabled ? _("Turn Wi-Fi Off") : _("Turn Wi-Fi On"), network_icon, [] {
      auto &s = SystemStatus::get();
      s.set_wifi_enabled(!s.network.wifi_enabled);
    });
  }
  b.row(
      _("Airplane mode"),
      [](Painter &p, double x, double y, double z, const Color &ink) {
        glyph::airplane(p, x, y, z, ink);
      },
      [] {
        auto &c = PanelControls::get();
        c.wireless_off(!c.airplane);
      },
      true, [] { return PanelControls::get().airplane; });
  b.end_card(card);
  if (s.network.available && s.network.has_wifi && s.network.wifi_enabled) {
    card = b.start_card(_("Wi-Fi networks"));
    b.row(_("Scan Wi-Fi networks"), network_icon, [] { SystemStatus::get().refresh_network(true); });
    int count = 0;
    for (const auto &net : s.wifi) {
      if (++count > 16) break;
      auto *row = b.row(
          net.ssid + (net.active   ? " · " + std::string(_("Connected"))
                      : net.secure ? " · " + std::string(_("Secured"))
                                   : " · " + std::string(_("Open"))),
          [net](Painter &p, double x, double y, double z, const Color &ink) {
            glyph::wifi(p, x, y, z, std::max(1, net.signal / 25), ink);
          },
          [net] {
            if (!net.active) SystemStatus::get().connect_wifi(net);
          });
      if (net.active) row->deactivate();
    }
    if (s.wifi.empty()) b.info(_("No Wi-Fi networks found"));
    b.end_card(card);
  }
  for (const auto &d : c.devices) {
    card = b.start_card(d.device + " · " + d.kind);
    b.info(d.state + (d.connection.empty() ? "" : " · " + d.connection));
    if (!d.ip.empty()) b.info("IP: " + d.ip);
    if (!d.gateway.empty()) b.info(std::string(_("Gateway")) + ": " + d.gateway);
    if (!d.dns.empty()) b.info("DNS: " + d.dns);
    if (d.kind == "ethernet")
      // "connected (externally)" counts as connected too.
      b.row(d.state.rfind("connected", 0) == 0 ? _("Disconnect LAN") : _("Connect LAN"), network_icon,
            [d] { PanelControls::get().network_device(d.device, d.state.rfind("connected", 0) != 0); });
    b.end_card(card);
  }
  b.settings_row(_("Network settings"), settings_for("network", {"nm-connection-editor"}));
}

void QuickSettings::bluetooth_section(SectionBuilder &b) {
  const auto &c = PanelControls::get();
  auto *card = b.start_card(_("Adapter"));
  if (c.adapters.empty())
    b.info(c.bluetooth_available ? _("No Bluetooth adapter detected")
                                 : _("Bluetooth service is unavailable"));
  for (const auto &a : c.adapters)
    if (c.adapters.size() > 1)
      b.row(
          a.name + (a.path == c.adapter_path ? " · " + std::string(_("Selected")) : ""), bt_icon,
          [path = a.path] { PanelControls::get().select_adapter(path); }, !c.bluetooth_busy);
  const BluetoothAdapter *adapter = c.adapter();
  if (adapter) {
    b.info(adapter->name);
    b.row(
        adapter->powered ? _("Turn Bluetooth Off") : _("Turn Bluetooth On"), bt_icon,
        [on = !adapter->powered] { PanelControls::get().bluetooth_power(on); }, !c.bluetooth_busy,
        [] {
          auto *a = PanelControls::get().adapter();
          return a && a->powered;
        });
    if (adapter->powered)
      b.row(
          adapter->discovering ? _("Stop scanning") : _("Scan for devices"), bt_icon,
          [] {
            auto &c = PanelControls::get();
            auto *a = c.adapter();
            if (a && a->discovering)
              c.stop_scan();
            else
              c.bluetooth_scan();
          },
          !c.bluetooth_busy);
    if (c.bluetooth_busy) b.info(_("Waiting for Bluetooth..."));
  }
  b.end_card(card);
  if (adapter && adapter->powered) {
    card = b.start_card(_("Devices"));
    bool any = false;
    for (const auto &d : c.bluetooth)
      if (d.adapter == adapter->path) {
        any = true;
        b.info((d.name.empty() ? d.address : d.name) + " · " +
               (d.connected ? _("Connected")
                : d.paired  ? _("Paired")
                            : _("Available")));
        b.row(
            d.connected ? _("Disconnect")
            : d.paired  ? _("Connect")
                        : _("Pair and connect"),
            bt_icon, [path = d.path] { PanelControls::get().bluetooth_device(path); },
            !c.bluetooth_busy);
        if (d.paired && !d.connected)
          b.row(
              _("Forget device"), bt_icon,
              [path = d.path] {
                if (fl_choice(_("Remove this Bluetooth pairing?"), _("Cancel"), _("Remove"),
                              nullptr) == 1)
                  PanelControls::get().bluetooth_forget(path);
              },
              !c.bluetooth_busy);
      }
    if (!any) b.info(_("No devices yet: scan to find nearby devices."));
    b.end_card(card);
  }
  b.info(_("PIN/code pairing uses the system Bluetooth agent."));
  b.settings_row(_("Bluetooth settings"), settings_for("bluetooth", {"blueman-manager"}));
}

void QuickSettings::media_section(SectionBuilder &b) {
  const auto &c = PanelControls::get();
  auto players = c.players();
  for (const auto &p : players) player_card(b, p);
  if (!players.empty()) return;
  b.label(_("No active media players"), 2);
  auto *card = b.start_card(_("Media applications"));
  int count = 0;
  for (const auto &app : load_applications())
    if (std::find(app.categories.begin(), app.categories.end(), "AudioVideo") !=
        app.categories.end()) {
      b.row(app.name, media_icon, [this, app] {
        close();
        launch_entry(app);
      });
      if (++count == 6) break;
    }
  if (!count) b.info(_("Install a media player to play music or video."));
  b.end_card(card);
}

namespace {
// The battery flyout's first line: battery icon, then "45% LGES AP23A8L" in
// bold (alpha51, after the Cinnamon power applet). Named for UI tests.
class BatteryHeader : public Fl_Widget {
 public:
  BatteryHeader(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    clear_visible_focus();
    copy_label(text().c_str());
    labeltype(FL_NO_LABEL);
  }
  static std::string text() {
    const auto &b = SystemStatus::get().battery;
    if (!b.present) return _("No battery");
    std::string s = std::to_string(b.percent) + "%";
    if (!b.model.empty()) s += " " + b.model;
    return s;
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    const auto &b = SystemStatus::get().battery;
    double size = S(popup_style::kIcon);
    {
      Painter p(window()->w(), window()->h());
      // 0.2 Alpha 2: the colours of the panel icon (green on mains power,
      // amber when low, red when critically low).
      Color level = b.phase == BatteryPhase::LOW        ? t.warning
                    : b.phase == BatteryPhase::CRITICAL ? t.error
                    : b.phase == BatteryPhase::CHARGING || b.phase == BatteryPhase::FULL
                        ? t.success
                        : t.text;
      if (b.present)
        glyph::battery(p, x(), y() + (h() - size) / 2.0, size, b.percent, b.charging, t.text,
                       level);
      else
        glyph::power(p, x(), y() + (h() - size) / 2.0, size, t.text);
    }
    text_font(t.font_size, true);
    draw_text(text(), x() + S(24), y(), w() - S(24), h(), t.text);
  }
};

// A plain text row of a flyout: optional dot before the text marking the
// current choice (shown bold), hover tint, keyboard activation.
class TextRow : public Fl_Button {
 public:
  TextRow(int x, int y, int w, int h, std::string text, std::function<void()> action,
          bool dot = false, bool current = false)
      : Fl_Button(x, y, w, h), text_(std::move(text)), action_(std::move(action)), dot_(dot),
        current_(current) {
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
    copy_tooltip(text_.c_str());
    callback([](Fl_Widget *w, void *) {
      auto fn = static_cast<TextRow *>(w)->action_;
      if (fn) later(fn);  // the action may rebuild the flyout
    });
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    bool hot = Fl::belowmouse() == this && active_r();
    {
      Painter p(window()->w(), window()->h());
      if (hot || value()) p.fill_rounded(x(), y(), w(), h(), S(6), t.hover);
      if (dot_ && current_) p.circle(x() + S(10), y() + h() / 2.0, Sd(3), t.text);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(6));
    }
    text_font(t.font_size, current_);
    draw_text(text_, x() + (dot_ ? S(22) : S(8)), y(), w() - S(30), h(),
              active_r() ? t.text : t.text_dim);
  }
  int handle(int event) override {
    if (event == FL_ENTER || event == FL_LEAVE || event == FL_FOCUS || event == FL_UNFOCUS) {
      redraw();
      return 1;
    }
    if (event == FL_KEYBOARD && Fl::focus() == this &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter ||
         Fl::event_key() == ' ')) {
      do_callback();
      return 1;
    }
    return Fl_Button::handle(event);
  }

 private:
  std::string text_;
  std::function<void()> action_;
  bool dot_, current_;
};

// A thin separator line across the body.
class Rule : public Fl_Widget {
 public:
  Rule(int x, int y, int w) : Fl_Widget(x, y, w, 1) { clear_visible_focus(); }
  void draw() override {
    Painter p(window()->w(), window()->h());
    p.line(x(), y() + 0.5, x() + w(), y() + 0.5, Panel::instance->theme().border);
  }
};
}  // namespace

// The battery flyout (alpha51), in the spirit of the Cinnamon power applet:
// battery and model, the state with the remaining time, a brightness
// slider, the power profiles as text rows (a dot marks the active one) and
// a Power Settings link. No cards, no tiles.
void QuickSettings::power_section(SectionBuilder &b) {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  int x = b.pad, w = b.inner, rh = S(popup_style::kRow);
  new BatteryHeader(x + S(8), b.y, w - S(16), S(28));
  b.y += S(28);
  {
    // 0.2 Alpha 2: low and critical, "Calculating…" while the estimate
    // is not known yet, plugged in without charging.
    // The label calls this while drawing, with its font set: a line that
    // does not fit (75 %, long estimates) takes the short duration.
    int line_w = w - S(16);
    auto *state = new ActionLabel(
        x + S(8), b.y, line_w, S(24),
        [line_w] {
          const auto &bt = SystemStatus::get().battery;
          if (!bt.present) return std::string(_("The computer runs on mains power."));
          std::string line = battery_phase_line(bt.phase, bt.minutes, bt.calculating);
          std::string time = battery_time_label(bt.phase, bt.minutes, bt.calculating);
          if (text_width(line) <= line_w || time.empty() || bt.calculating) return line;
          return battery_label(bt.phase) + " - " + time;
        },
        0);
    // Named for UI tests with the text it shows.
    state->copy_tooltip(s.battery.present
                            ? battery_phase_line(s.battery.phase, s.battery.minutes,
                                                 s.battery.calculating)
                                  .c_str()
                            : _("The computer runs on mains power."));
    b.y += S(24);
  }
  b.y += S(8);
  if (s.brightness.available) {
    auto *icon = new ActionButton(x, b.y, S(32), rh, "", sun_icon(), [] {}, nullptr, -1, true);
    icon->clear_visible_focus();
    brightness_ = new Slider(x + S(36), b.y, w - S(36) - S(4), rh);
    brightness_->bounds(1, 100);
    brightness_->value(s.brightness.percent());
    brightness_->tooltip(_("Brightness"));
    brightness_->callback([](Fl_Widget *wd, void *) {
      SystemStatus::get().set_brightness(int(static_cast<Fl_Slider *>(wd)->value()));
    });
    b.y += rh + S(6);
  }
  // power-profiles-daemon modes (powerprofilesctl), when the service runs.
  if (!c.power_profiles.empty()) {
    static const std::map<std::string, const char *> names = {{"power-saver", N_("Power Saver")},
                                                              {"balanced", N_("Balanced")},
                                                              {"performance", N_("Performance")}};
    for (const auto &id : ordered_power_profiles(c.power_profiles)) {
      std::string label = names.count(id) ? _(names.at(id)) : id;
      new TextRow(x, b.y, w, rh, label, [id] { PanelControls::get().set_power_profile(id); }, true,
                  id == c.power_profile);
      b.y += rh;
    }
    b.y += S(4);
  }
  new Rule(x + S(8), b.y, w - S(16));
  b.y += S(5);
  std::string prefs = first_program({"sde-power-preferences", "xfce4-power-manager-settings"});
  std::vector<std::string> cmd =
      settings_page("power") || prefs.empty()
          ? std::vector<std::string>{"sde-settings", "--page", "power"}
          : std::vector<std::string>{prefs};
  new TextRow(x, b.y, w, rh, _("Power Settings"), b.launch(cmd));
  b.y += rh;
}

void QuickSettings::drives_section(SectionBuilder &b) {
  const auto &c = PanelControls::get();
  int pad = b.pad, inner = b.inner;
  if (c.drives.empty()) b.label(_("No removable drives are plugged in."), 2);
  int gap = S(6), bw = (inner - S(24) - gap) / 2;
  static const std::map<std::string, std::string> kinds = {
      {"usb", _("USB flash drive")}, {"card", _("Memory card")},
      {"hdd", _("External hard disk")}, {"ssd", _("External SSD")}};
  for (const auto &d : c.drives) {
    auto *card = b.start_card(d.label);
    auto kind = kinds.count(d.kind) ? kinds.at(d.kind) : std::string(_("Drive"));
    b.info(kind + " · " + human_size(d.size) +
           (d.mountpoint().empty() ? "" : " · " + std::string(_("Mounted"))));
    new CommandButton(pad + S(12), b.y, bw, S(32), _("Show Files"), [this, name = d.name] {
      close();
      PanelControls::get().drive_open(name);
    });
    new CommandButton(pad + S(12) + bw + gap, b.y, bw, S(32), _("Eject"),
                      [name = d.name] { PanelControls::get().drive_eject(name, true); });
    b.y += S(40);
    b.end_card(card);
  }
}

void QuickSettings::keyboard_section(SectionBuilder &b) {
  const auto &c = PanelControls::get();
  const auto &cfg = Panel::instance->config();
  auto *card = b.start_card(_("Keyboard layout"));
  b.info(std::string(_("Current layout")) + ": " + c.keyboard);
  std::vector<std::string> layouts;
  for (const char *key : {"keyboard_layout", "keyboard_layout2"}) {
    std::string v = trim(cfg.get("input", key));
    if (!v.empty() && std::find(layouts.begin(), layouts.end(), v) == layouts.end())
      layouts.push_back(v);
  }
  if (layouts.size() > 1)
    for (const auto &l : layouts) {
      std::string upper = l;
      for (char &ch : upper) ch = char(toupper(static_cast<unsigned char>(ch)));
      bool current = upper == c.keyboard;
      auto *row = b.row(
          upper + (current ? " · " + std::string(_("In use")) : ""),
          [](Painter &p, double x, double y, double z, const Color &ink) {
            glyph::keyboard(p, x, y, z, ink);
          },
          [layouts, l, upper] {
            // The chosen layout first; Alt+Shift still switches to the other.
            std::string other;
            for (const auto &o : layouts)
              if (o != l) other = o;
            run_async({"setxkbmap", "-layout", l + "," + other, "-option", "", "-option",
                       "grp:alt_shift_toggle"},
                      nullptr);
            PanelControls::get().keyboard = upper;
            later([l, other] {
              Panel::instance->set_setting("input", "keyboard_layout", l);
              Panel::instance->set_setting("input", "keyboard_layout2", other);
            });
          },
          !current);
      (void)row;
    }
  else
    b.info(_("Add a second layout in Keyboard settings to switch here."));
  b.end_card(card);
  b.settings_row(_("Keyboard settings"), settings_for("input", {"sde-settings", "--page", "input"}));
}

void QuickSettings::tray_section(SectionBuilder &b) {
  const auto &cfg = Panel::instance->config();
  int pad = b.pad, inner = b.inner;
  auto *h = Panel::instance->status_notifiers();
  auto items = h ? h->visible_items() : std::vector<const SniHost::Item *>{};
  auto *tray = Panel::instance->legacy_tray();
  // [action_center] move_status: the panel's tray icons gather in this
  // flyout while it is open; without it they stay on the panel.
  bool moved = cfg.get_bool("action_center", "move_status", true);
  bool legacy = tray && tray->count() && moved;
  auto *card = b.start_card(_("Tray applications"));
  int cell = S(36), columns = std::max(1, (inner - S(8)) / cell);
  if (items.empty() && !legacy) b.info(_("No applications are in the tray."));
  for (size_t i = 0; i < items.size(); ++i)
    new TrayIndicator(pad + S(4) + int(i % columns) * cell, b.y + int(i / columns) * cell, cell,
                      *items[i]);
  // XEmbed icons continue on the last StatusNotifier row when there is room.
  int last = int(items.size() % size_t(columns)),
      rows = int((items.size() + columns - 1) / columns);
  int tx = pad + S(4), ty = b.y + rows * cell;
  if (legacy) {
    int slot = tray->icon_size() + S(6), room = inner - S(8) - last * cell;
    if (rows && last && room >= slot) {
      tx += last * cell;
      ty -= cell;
      tray_columns_ = std::max(1, room / slot);
    } else {
      tray_columns_ = std::max(1, (inner - S(8)) / slot);
    }
    int th = tray->grid_height(tray_columns_);
    tray_box_ = new Fl_Box(tx, ty + std::max(0, (cell - th) / 2),
                           inner - S(8) - (tx - pad - S(4)), th);
    b.y = std::max(ty + std::max(cell, th), b.y + rows * cell) + S(6);
  } else {
    b.y += rows * cell;
  }
  b.end_card(card);
  b.settings_row(_("Panel settings"), settings_for("panel", {"sde-settings", "--page", "panel"}));
}

void QuickSettings::build_flyout(SectionBuilder &b) {
  if (section_ == "sound")
    sound_section(b);
  else if (section_ == "network")
    network_section(b);
  else if (section_ == "bluetooth")
    bluetooth_section(b);
  else if (section_ == "media")
    media_section(b);
  else if (section_ == "power")
    power_section(b);
  else if (section_ == "drives")
    drives_section(b);
  else if (section_ == "keyboard")
    keyboard_section(b);
  else if (section_ == "tray")
    tray_section(b);
}

// ---------------------------------------------------------- Action Center

// ------------------------------------------------------- Action Center parts
// alpha52: the Action Center is built from parts placed in one or two
// columns under the clock hero; see rebuild_center().

void QuickSettings::tiles_part(SectionBuilder &b, bool all) {
  const auto &cfg = Panel::instance->config();
  const auto &s = SystemStatus::get();
  int pad = b.pad, inner = b.inner;
  auto open_flyout = [](const char *section) {
    return [section] { later([section] { Panel::instance->open_flyout(section); }); };
  };
  // Quick tiles: toggles first (the compact row), then shortcuts.
  struct Tile {
    std::string title;
    IconFn icon;
    std::function<void()> click;
    std::function<bool()> on;
    StringFn caption;
    std::function<void()> more;
  };
  std::vector<Tile> tiles;
  if (s.network.has_wifi)
    tiles.push_back({_("Wi-Fi"), network_icon,
                     [] {
                       auto &s = SystemStatus::get();
                       s.set_wifi_enabled(!s.network.wifi_enabled);
                     },
                     [] { return SystemStatus::get().network.wifi_enabled; },
                     [] {
                       const auto &n = SystemStatus::get().network;
                       if (n.kind == "wifi" && n.no_internet()) return std::string(_("No internet"));
                       if (n.kind == "wifi" && n.portal()) return std::string(_("Sign in"));
                       return n.kind == "wifi" && !n.connection.empty() ? n.connection
                                                                        : std::string(_("Wi-Fi"));
                     },
                     open_flyout("network")});
  else
    tiles.push_back({_("Network"), network_icon, open_flyout("network"),
                     [] { return !SystemStatus::get().network.kind.empty(); },
                     [] {
                       const auto &n = SystemStatus::get().network;
                       if (n.no_internet()) return std::string(_("No internet"));
                       if (n.portal()) return std::string(_("Sign in"));
                       return n.connection.empty() ? std::string(_("Offline")) : n.connection;
                     },
                     nullptr});
  tiles.push_back({_("Bluetooth"), bt_icon,
                   [] {
                     auto &c = PanelControls::get();
                     if (auto *a = c.adapter())
                       c.bluetooth_power(!a->powered);
                     else
                       later([] { Panel::instance->open_flyout("bluetooth"); });
                   },
                   [] {
                     auto *a = PanelControls::get().adapter();
                     return a && a->powered;
                   },
                   nullptr, open_flyout("bluetooth")});
  tiles.push_back({_("Airplane mode"),
                   [](Painter &p, double x, double y, double z, const Color &ink) {
                     glyph::airplane(p, x, y, z, ink);
                   },
                   [] {
                     auto &c = PanelControls::get();
                     c.wireless_off(!c.airplane);
                   },
                   [] { return PanelControls::get().airplane; }, nullptr, nullptr});
  tiles.push_back({_("Do Not Disturb"), moon_icon(),
                   [] {
                     auto *p = Panel::instance;
                     p->set_do_not_disturb(
                         !p->config().get_bool("notifications", "do_not_disturb", false));
                   },
                   [] {
                     return Panel::instance->config().get_bool("notifications", "do_not_disturb",
                                                               false);
                   },
                   nullptr, nullptr});
  tiles.push_back({_("Dark Mode"),
                   [](Painter &p, double x, double y, double z, const Color &ink) {
                     glyph::sun(p, x, y, z, ink);
                   },
                   [] { Panel::instance->toggle_dark_mode(); },
                   [] { return Panel::instance->theme().dark; }, nullptr, nullptr});
  tiles.push_back({_("Display"), display_icon(),
                   b.launch(settings_for("display", {"sde-display-settings"})), nullptr, nullptr,
                   nullptr});
  std::string shooter = first_program({"scrot", "gnome-screenshot", "xfce4-screenshooter",
                                       "flameshot", "spectacle"});
  if (!shooter.empty()) {
    tiles.push_back(
        {_("Screenshot"), camera_icon(),
         [this, shooter] {
           close();
           // Give the compositor a moment to remove the Action Center first.
           std::string cmd =
               shooter == "scrot"
                   ? "mkdir -p \"$HOME/Pictures\" && scrot \"$HOME/Pictures/Screenshot-%Y%m%d-%H%M%S.png\""
               : shooter == "flameshot" ? "flameshot gui"
               : shooter == "gnome-screenshot" ? "gnome-screenshot -i"
                                               : shooter;
           spawn_detached({"sh", "-c", "sleep 0.5; " + cmd});
         },
         nullptr, nullptr, nullptr});
  }
  // The Control Center has its button in the footer; the clock's alarms get a tile.
  tiles.push_back({_("Alarms"), alarm_icon(),
                   [] { later([] { Panel::instance->open_alarms(); }); }, nullptr,
                   [] {
                     long long next = Alarms::get().next_alarm();
                     if (!next) return std::string(_("Alarms"));
                     time_t t = time_t(next);
                     struct tm lt;
                     localtime_r(&t, &lt);
                     return clockface::format_time(lt, clockface::use_24h(Panel::instance->config()),
                                                   false);
                   },
                   nullptr});
  int columns = cfg.get_int("action_center", "tile_columns", 4) == 3 ? 3 : 4, gap = S(6),
      tw = (inner - (columns - 1) * gap) / columns, th = S(popup_style::kTile);
  // Compact: one row of toggles.
  size_t count = all ? tiles.size() : std::min(tiles.size(), size_t(columns));
  for (size_t i = 0; i < count; ++i) {
    const auto &v = tiles[i];
    new QuickTile(pad + int(i % columns) * (tw + gap), b.y + int(i / columns) * (th + gap), tw, th,
                  v.title, v.icon, v.click, v.on, v.caption, v.more, int(i));
  }
  b.y += int((count + columns - 1) / columns) * (th + gap) + S(2);
}

void QuickSettings::sliders_part(SectionBuilder &b, bool settings) {
  const auto &s = SystemStatus::get();
  if (s.audio.available || s.brightness.available) {
    auto *card = b.start_card("");
    if (s.audio.available) volume_row(b, true);
    if (s.brightness.available) brightness_row(b);
    b.y -= S(2);
    b.end_card(card);
  }
  if (brightness_open_ && settings) brightness_settings(b);
}

bool QuickSettings::media_part(SectionBuilder &b, bool mini) {
  if (!Panel::instance->config().get_bool("action_center", "show_media", true)) return false;
  auto players = PanelControls::get().players();
  if (players.empty()) return false;
  player_card(b, players.front(), mini);
  return true;
}

void QuickSettings::calendar_part(SectionBuilder &b, int row, bool tz, bool agenda,
                                  bool calendar) {
  int pad = b.pad, inner = b.inner;
  if (calendar || tz) {
    auto *card = b.start_card("");
    if (calendar) {
      auto *cal = new MonthCalendar(pad + S(8), b.y - S(2), inner - S(16), row);
      calendar_ = cal;
      if (shown_year_) cal->show_month(shown_year_, shown_month_);
      cal->select(agenda_year_, agenda_month_, agenda_day_);
      cal->marked = [](int y, int m, int d) {
        return agenda::has_items(Alarms::get().items(), y, m, d);
      };
      cal->moved = [this](int y, int m) {
        shown_year_ = y;
        shown_month_ = m;
      };
      cal->picked = [this](int y, int m, int d) { agenda_pick(y, m, d); };
      b.y += calendar_->h() - S(2);
    }
    if (tz) timezone_rows(b);
    b.end_card(card);
  }
  if (agenda) agenda_card(b);
  // alpha52: the notifications moved to the panel's notifications button
  // (notify_center.h); the Action Center keeps toggles, sliders, media,
  // calendar and power.
}

// ------------------------------------------------------------- time zone

// Under the calendar: "Time zone · Asia/Jakarta · UTC+7" with Sync, the
// progress or outcome of the last sync, and the Automatic time zone switch.
void QuickSettings::timezone_rows(SectionBuilder &b) {
  const auto &cfg = Panel::instance->config();
  auto &tz = tzsync::Sync::get();
  int pad = b.pad, inner = b.inner, bw = S(78);
  bool online = weather_online();
  b.y += S(4);
  // The labels name the rows for the QA dump (they are not drawn).
  auto *zone = new ActionLabel(pad + S(12), b.y, inner - S(32) - bw, S(32), [] {
    return std::string(_("Time zone")) + " \xc2\xb7 " + tzsync::current_label();
  });
  zone->copy_label(("Time zone: " + tzsync::current_label()).c_str());
  auto *sync = new CommandButton(pad + inner - S(12) - bw, b.y, bw, S(32),
                                 tz.busy() ? _("Syncing") : _("Sync"),
                                 [] { tzsync::Sync::get().start(weather_online()); });
  sync->copy_tooltip(online ? _("Sync time zone")
                            : _("Sync time zone: connect to the internet to detect the time zone"));
  if (!online || tz.busy()) sync->deactivate();
  b.y += S(34);
  if (!tz.message().empty()) {
    auto *status = new ActionLabel(pad + S(12), b.y, inner - S(24), S(24),
                                   [] { return tzsync::Sync::get().message(); }, 2);
    status->copy_label(("Time zone status: " + tz.message()).c_str());
    b.y += S(24);
  }
  auto *automatic = new Switch(pad + S(12), b.y, inner - S(24), S(32));
  automatic->copy_label(_("Automatic time zone"));
  automatic->copy_tooltip(_("Automatic time zone"));
  automatic->value(cfg.get_bool("clock", "auto_timezone", false) ? 1 : 0);
  automatic->callback([](Fl_Widget *w, void *) {
    bool on = static_cast<Switch *>(w)->value() != 0;
    later([on] {
      Panel::instance->set_setting("clock", "auto_timezone", on ? "true" : "false");
      if (on) tzsync::Sync::get().automatic(true, weather_online());
    });
  });
  b.y += S(36);
}

// ---------------------------------------------------------------- agenda

namespace {
const int kRemind[] = {-1, 0, 5, 15, 30, 60, 1440};
std::string remind_label(int minutes) {
  switch (minutes) {
    case -1:
      return _("No reminder");
    case 0:
      return _("At start time");
    case 60:
      return _("1 hour before");
    case 1440:
      return _("1 day before");
    default:
      return format(_("%d minutes before"), minutes);
  }
}
int remind_index(int minutes) {
  for (int i = 0; i < 7; ++i)
    if (kRemind[i] == minutes) return i;
  return 0;
}
}  // namespace

void QuickSettings::agenda_pick(int year, int month, int day) {
  agenda_year_ = year;
  agenda_month_ = month;
  agenda_day_ = day;
  shown_year_ = year;
  shown_month_ = month;
  agenda_error_.clear();
  status_changed();
}

void QuickSettings::agenda_edit(int id) {
  const agenda::Item *it = id ? Alarms::get().item(id) : nullptr;
  agenda_edit_ = it ? id : 0;
  agenda_form_ = true;
  agenda_error_.clear();
  draft_title_ = it ? it->title : "";
  draft_note_ = it ? it->note : "";
  draft_time_.clear();
  if (it && it->timed()) draft_time_ = format("%02d:%02d", it->hour, it->minute);
  draft_remind_ = it ? remind_index(it->remind) : 0;
  status_changed();
}

void QuickSettings::agenda_cancel() {
  agenda_form_ = false;
  agenda_edit_ = 0;
  agenda_error_.clear();
  status_changed();
}

void QuickSettings::agenda_save() {
  agenda::Item item;
  if (const agenda::Item *old = agenda_edit_ ? Alarms::get().item(agenda_edit_) : nullptr)
    item = *old;
  item.id = agenda_edit_;
  item.year = agenda_year_;
  item.month = agenda_month_;
  item.day = agenda_day_;
  item.title = trim(draft_title_);
  item.note = trim(draft_note_);
  item.remind = kRemind[std::max(0, std::min(6, draft_remind_))];
  item.hour = -1;
  item.minute = 0;
  if (item.title.empty()) {
    agenda_error_ = _("Give the agenda item a title.");
  } else if (!trim(draft_time_).empty() &&
             !agenda::parse_time(draft_time_, item.hour, item.minute)) {
    agenda_error_ = _("Write the time as HH:MM, for example 14:30.");
  } else {
    Alarms::get().save_item(item);
    agenda_form_ = false;
    agenda_edit_ = 0;
    agenda_error_.clear();
    draft_title_.clear();
    draft_time_.clear();
    draft_note_.clear();
    draft_remind_ = 0;
  }
  status_changed();
}

void QuickSettings::agenda_card(SectionBuilder &b) {
  const auto &t = Panel::instance->theme();
  int pad = b.pad, inner = b.inner;
  auto *card = b.start_card("");
  struct tm day {};
  day.tm_year = agenda_year_ - 1900;
  day.tm_mon = agenda_month_ - 1;
  day.tm_mday = agenda_day_;
  day.tm_hour = 12;
  day.tm_isdst = -1;
  mktime(&day);
  char date[96];
  strftime(date, sizeof date, "%A, %e %B", &day);
  std::string title = std::string(_("Agenda")) + " · " + trim(date);
  new ActionLabel(pad + S(12), b.y - S(4), inner - S(70), S(32), [title] { return title; }, 1);
  if (!agenda_form_)
    icon_button(pad + inner - S(46), b.y - S(4), S(34), S(32), _("Add agenda item"), plus_icon(),
                [this] { later([this] { agenda_edit(0); }); });
  b.y += S(32);
  auto items = agenda::items_on(Alarms::get().items(), agenda_year_, agenda_month_, agenda_day_);
  // Tight layouts (small screens) leave out the empty-day hint.
  if (items.empty() && !agenda_form_ && !agenda_tight_)
    b.info(_("Nothing planned. Add an item with +."));
  for (const auto &it : items) {
    std::string text =
        (it.timed() ? format("%02d:%02d", it.hour, it.minute) : std::string(_("All day"))) +
        " · " + it.title;
    int id = it.id;
    auto *row = new ActionButton(
        pad + S(8), b.y, inner - S(58), S(36), text,
        [](Painter &p, double x, double y, double z, const Color &ink) {
          p.circle(x + z / 2, y + z / 2, z * .22, ink);
        },
        [this, id] { later([this, id] { agenda_edit(id); }); }, nullptr, -1, true);
    row->copy_tooltip(format(_("Edit agenda item %s"), it.title.c_str()).c_str());
    icon_button(pad + inner - S(46), b.y + S(1), S(34), S(34),
                format(_("Delete agenda item %s"), it.title.c_str()), cross_icon(),
                [id] { later([id] { Alarms::get().remove_item(id); }); });
    b.y += S(38);
    std::string extra = it.note;
    if (it.remind >= 0)
      extra += std::string(extra.empty() ? "" : " · ") + std::string(_("Reminder")) + ": " +
               remind_label(it.remind);
    for (char &c : extra)
      if (c == '\n') c = ' ';
    if (!extra.empty() && !agenda_tight_) {
      new ActionLabel(pad + S(50), b.y - S(4), inner - S(62), S(22), [extra] { return extra; }, 2);
      b.y += S(20);
    }
  }
  if (agenda_form_) {
    b.y += S(4);
    new ActionLabel(
        pad + S(12), b.y, inner - S(24), S(24),
        [this] { return std::string(agenda_edit_ ? _("Edit agenda item") : _("New agenda item")); },
        0);
    b.y += S(26);
    int x = pad + S(12), w = inner - S(24), gap = S(8);
    auto *title_in = new TextField(x, b.y, w, S(34), _("Agenda title"), _("Title"),
                                   [this](const std::string &v) { draft_title_ = v; });
    title_in->value(draft_title_.c_str());
    b.y += S(40);
    int half = (w - gap) / 2;
    auto *time_in = new TextField(x, b.y, half, S(34), _("Agenda time"), _("Time (optional)"),
                                  [this](const std::string &v) { draft_time_ = v; });
    time_in->value(draft_time_.c_str());
    auto *remind = new Fl_Choice(x + half + gap, b.y + S(1), w - half - gap, S(32));
    remind->tooltip(_("Agenda reminder"));
    remind->textsize(t.font_size);
    for (int i = 0; i < 7; ++i) remind->add(choice_label(remind_label(kRemind[i])).c_str());
    remind->value(std::max(0, std::min(6, draft_remind_)));
    remind->callback(
        [](Fl_Widget *w, void *d) {
          auto *q = static_cast<QuickSettings *>(d);
          q->draft_remind_ = static_cast<Fl_Choice *>(w)->value();
          q->status_changed();
        },
        this);
    b.y += S(40);
    auto *note_in = new TextField(x, b.y, w, S(34), _("Agenda note"), _("Note (optional)"),
                                  [this](const std::string &v) { draft_note_ = v; });
    note_in->value(draft_note_.c_str());
    b.y += S(40);
    if (!agenda_error_.empty()) b.info(agenda_error_);
    if (draft_remind_ > 0 && trim(draft_time_).empty())
      b.info(_("Without a time, reminders count from 09:00."));
    int bw = (w - gap) / 2;
    new CommandButton(
        x, b.y, bw, S(34), _("Save item"), [this] { later([this] { agenda_save(); }); }, true);
    new CommandButton(x + bw + gap, b.y, w - bw - gap, S(34), _("Cancel"),
                      [this] { later([this] { agenda_cancel(); }); });
    b.y += S(40);
  }
  b.end_card(card);
}

// ---------------------------------------------------------------- footer

void QuickSettings::set_power_menu(bool open) {
  power_open_ = open;
  status_changed();
}

// The session row: the user, Control Center, Lock and Power. Power opens a
// row above it with Sleep, Log Out, Restart and Shut Down (the sde-logout
// actions); Power again closes it.
void QuickSettings::footer(int W, int H, int pad) {
  int fy = H - footer_ + (power_open_ ? S(44) : 0) + S(8), fb = S(32);
  auto launch = [this](std::vector<std::string> cmd) {
    return [this, cmd] {
      close();
      spawn_detached(cmd);
    };
  };
  if (power_open_) {
    struct Choice {
      std::string name;
      const char *action;
    };
    std::vector<Choice> choices = {{_("Sleep"), "suspend"},
                                   {_("Log Out"), "logout"},
                                   {_("Restart"), "reboot"},
                                   {_("Shut Down"), "poweroff"}};
    int gap = S(6), n = int(choices.size()), bw = (W - 2 * pad - (n - 1) * gap) / n;
    for (int i = 0; i < n; ++i)
      new CommandButton(pad + i * (bw + gap), H - footer_ + S(8), bw, fb,
                        choices[size_t(i)].name,
                        launch({"sde-logout", "--action", choices[size_t(i)].action}));
  }
  bool open = power_open_;
  std::vector<std::pair<std::string, std::function<void()>>> foot = {
      {_("Control Center"), launch({"sde-settings"})},
      {_("Lock Screen"), launch({"sde-logout", "--action", "lock"})},
      {open ? _("Close power options") : _("Power"),
       [this, open] { later([this, open] { set_power_menu(!open); }); }}};
  std::vector<IconFn> icons = {gear_icon(),
                               [](Painter &p, double x, double y, double z, const Color &ink) {
                                 glyph::lock(p, x, y, z, ink);
                               },
                               power_icon()};
  new UserBadge(pad, fy, W - 2 * pad - int(foot.size()) * (fb + S(6)) - S(8), fb);
  for (size_t i = 0; i < foot.size(); ++i) {
    auto *b = new ActionButton(W - pad - int(foot.size() - i) * (fb + S(6)) + S(6), fy, fb, fb, "",
                               icons[i], foot[i].second,
                               i == 2 ? std::function<bool()>([open] { return open; }) : nullptr,
                               -1, true);
    b->copy_tooltip(foot[i].first.c_str());
  }
}

// --------------------------------------------------------------- rebuild

void QuickSettings::rebuild() {
  Fl::remove_timeout(rebuild_timeout, this);
  rebuild_pending_ = false;
  structure_ = structure();
  Fl::remove_timeout(weather_tick, this);
  Fl::remove_timeout(media_tick, this);
  if (mode_ == ACTION_CENTER) {
    rebuild_center();
    return;
  }
  clear();
  resizable(nullptr);
  volume_ = brightness_ = microphone_ = nullptr;
  tray_box_ = nullptr;
  calendar_ = nullptr;
  brightness_card_ = nullptr;
  clock_ = nullptr;
  media_slider_ = nullptr;
  media_service_.clear();
  stream_sliders_.clear();
  center_plan_.clear();
  const auto &t = Panel::instance->theme();
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  // alpha51: compact widths (popup_style.h); the battery flyout is a little
  // wider for "45% <battery model>" and the remaining time on one line.
  int fly_w = section_ == "power" ? popup_style::kBatteryW : popup_style::kFlyoutW;
  int W = std::min(S(fly_w), mw - S(16));
  int pad = S(popup_style::kPad), scrollbar = S(6), inner = W - 2 * pad - scrollbar;
  auto *panel = Panel::instance;
  int available =
      std::max(S(120), t.panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my);
  available = std::max(S(120), available - S(16));
  // The battery flyout has no title bar: its first line names the battery.
  bool titled = section_ != "power";
  header_ = titled ? S(popup_style::kHeader) : S(4);
  footer_ = 0;
  footer_w_ = 0;

  begin();
  scroll_ = new Fl_Scroll(S(2), header_, W - S(4), std::max(S(40), available - header_));
  body_ = scroll_;
  scroll_->type(Fl_Scroll::VERTICAL);
  scroll_->resizable(nullptr);
  scroll_->scrollbar_size(scrollbar);
  scroll_->box(FL_FLAT_BOX);
  scroll_->color(t.surface.fl());
  // Give Fl_Scroll a stable zero origin. Otherwise its padding is reported as
  // negative scroll and repeated service rebuilds accumulate blank space.
  new Fl_Box(scroll_->x(), scroll_->y(), 1, 1);
  SectionBuilder b{this, pad, inner, header_ + S(6)};
  b.flat = true;
  if (!s.message.empty() || !c.message.empty()) {
    new ActionLabel(
        pad, b.y, inner, S(28),
        [] {
          const auto &m = PanelControls::get().message;
          return m.empty() ? SystemStatus::get().message : m;
        },
        2);
    b.y += S(30);
  }
  build_flyout(b);
  content_height_ = b.y - header_ + S(4);
  new Fl_Box(pad, b.y, inner, S(4));
  scroll_->end();
  int H = std::min(available, header_ + content_height_);
  scroll_->size(W - S(4), H - header_);
  body_x_ = scroll_->x(), body_y_ = scroll_->y(), body_w_ = scroll_->w(), body_h_ = scroll_->h();
  begin();
  // Created after the body, so keyboard focus starts on the first control.
  std::string title = section_title(section_);
  if (titled)
    new ActionLabel(pad + S(8), S(4), W - 2 * pad - S(16), header_ - S(4),
                    [title] { return title; }, 1);
  end();
  resizable(nullptr);
  init_sizes();
  size(W, H);
  if (media_slider_) Fl::add_timeout(1.0, media_tick, this);
  sync();
}

// One layout of the Action Center: which parts, in one or two columns.
struct QuickSettings::CenterPlan {
  std::string name;
  bool two = false;        // two columns: calendar beside the controls
  bool tiles_all = false;  // every tile, else the row of toggles
  int media = 0;           // the player card: 0 hidden, 1 left, 2 right column, 3 right (mini)
  bool calendar = false, tz = false, agenda = false, notif = false;
  int cal_row = 29;        // calendar week row height at 100 %
  bool bright_right = false;  // the brightness settings card in the right column
  bool focus = false;  // calendar view: time zone and agenda in place of the controls
};


// Builds the hero, the parts of |plan| and the footer; false when they need
// more than |limit| pixels (the widgets are then built anyway, the caller
// clears them to try the next plan). |W| and |H| return the window size.
bool QuickSettings::build_center_plan(const CenterPlan &plan, int limit, int &W, int &H) {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  int pad = S(popup_style::kPad), gap = S(10), sp = S(10);
  // Anchored at the panel's right end: no wider than the room left of it.
  auto *panel = Panel::instance;
  int room = std::min(mw, panel->x() + panel->w() - mx);
  W = plan.two ? std::min(S(2 * popup_style::kCenterW), room - S(8))
               : std::min(S(popup_style::kCenterW), room);
  int colw = plan.two ? (W - 2 * pad - gap) / 2 : W - 2 * pad;
  if (plan.two && colw < S(296)) return false;
  header_ = 0;
  footer_ = power_open_ ? S(92) : S(48);
  agenda_tight_ = plan.cal_row <= 22 || plan.focus;
  // The calendar view on a small screen takes a lower hero.
  int hero_min = S(plan.focus ? 72 : popup_style::kHeroMin);

  // Left column: the controls.
  begin();
  auto *left = new Fl_Group(pad, 0, colw, S(10));
  left->resizable(nullptr);
  SectionBuilder bl{this, pad, colw, 0};
  if (!s.message.empty() || !c.message.empty()) {
    new ActionLabel(
        pad, bl.y, colw, S(28),
        [] {
          const auto &m = PanelControls::get().message;
          return m.empty() ? SystemStatus::get().message : m;
        },
        2);
    bl.y += S(30);
  }
  if (plan.focus) {
    // The calendar view on a small screen: time zone and agenda under the
    // hero, the month beside them.
    // With 22 px week rows the agenda fits under the month instead.
    calendar_part(bl, plan.cal_row, plan.tz, plan.agenda && plan.cal_row != 22, false);
  } else {
    tiles_part(bl, plan.tiles_all);
    sliders_part(bl, !plan.bright_right);
    if (plan.media == 1) media_part(bl);
  }
  if (!plan.two) {
    calendar_part(bl, plan.cal_row, plan.tz, plan.agenda, plan.calendar);
  }
  left->end();
  int lh = bl.y;
  // Right column: the calendar, agenda and time zone.
  Fl_Group *right = nullptr;
  int rh = 0;
  if (plan.two) {
    begin();
    right = new Fl_Group(pad + colw + gap, 0, colw, S(10));
    right->resizable(nullptr);
    SectionBuilder br{this, pad + colw + gap, colw, 0};
    if (plan.bright_right && brightness_open_) brightness_settings(br);
    if (plan.media >= 2) media_part(br, plan.media == 3);
    if (plan.focus)
      calendar_part(br, plan.cal_row, false, plan.agenda && plan.cal_row == 22, plan.calendar);
    else
      calendar_part(br, plan.cal_row, plan.tz, plan.agenda, plan.calendar);
    right->end();
    rh = br.y;
  }
  // The hero takes what is left, between its smallest and largest size;
  // over both columns when the right one fits under it too.
  int top = pad, bottom = S(4) + footer_;
  // Two columns: the wide hero when it gets as much height as the hero
  // above the left column alone would; else the calendar runs up beside it.
  bool wide = !plan.two;
  // Beside the hero, the right column runs from the top down past the
  // footer, which then stays under the left column.
  int right_bottom = S(8);
  if (plan.two) {
    int wide_hero = limit - bottom - top - sp - std::max(lh, rh);
    int narrow_hero = std::min(limit - bottom - top - sp - lh, S(popup_style::kHeroMax));
    wide = wide_hero >= hero_min &&
           (wide_hero >= narrow_hero || top + rh + right_bottom > limit);
  }
  int column = wide ? std::max(lh, rh) : lh;
  int hero = std::min(S(popup_style::kHeroMax), limit - bottom - top - sp - column);
  bool fits = hero >= hero_min && (wide || top + rh + right_bottom <= limit);
  hero = std::max(hero_min, hero);
  int body_top = top + hero + sp;
  left->resize(pad, body_top, colw, std::max(1, lh));
  if (right) right->resize(pad + colw + gap, wide ? body_top : top, colw, std::max(1, rh));
  H = body_top + lh + bottom;
  if (right) H = std::max(H, wide ? body_top + rh + bottom : top + rh + right_bottom);
  footer_w_ = right && !wide ? pad + colw + pad / 2 : W;
  // Everything above the footer (QA dump; nothing scrolls).
  body_x_ = 0, body_y_ = 0, body_w_ = W, body_h_ = H - footer_;

  // The clock hero, with its buttons in the corner.
  begin();
  weather::Settings wset = weather_settings();
  const weather::Report *wx = wset.enabled ? weather_report() : nullptr;
  auto *h = new ClockHero(pad, top, wide ? W - 2 * pad : colw, hero, wx, wset, wset.animate);
  clock_ = h;
  int bs = S(30), by = top + hero - bs - S(8), bx = pad + h->w() - S(8) - bs;
  bool open = expanded();
  new HeroButton(bx, by, bs, open ? _("Show less") : _("Show more"), fold_icon(open),
                 [this] { later([this] { toggle_expanded(); }); });
  new HeroButton(bx - bs - S(6), by, bs, _("Action Center settings"), gear_icon(),
                 [this] {
                   close();
                   spawn_detached(settings_for("action-center", {}));
                 });
  h->end();
  footer(footer_w_, H, pad);
  end();
  return fits;
}

void QuickSettings::rebuild_center() {
  const auto &t = Panel::instance->theme();
  const auto &cfg = Panel::instance->config();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  auto *panel = Panel::instance;
  // The room between the panel and the far screen edge.
  int available =
      std::max(S(200), (t.panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my) - S(8));
  bool exp = expanded();
  bool media = cfg.get_bool("action_center", "show_media", true) &&
               !PanelControls::get().players().empty();
  int m = media ? 1 : 0;
  // From the richest layout to the plainest. Expanded: the whole month and
  // agenda, the time zone, every tile; two columns when one is too tall;
  // smaller calendar rows; then what matters least goes first.
  std::vector<CenterPlan> plans;
  if (exp) {
    plans.push_back({"expanded", false, true, m, true, true, true, false, 29});
    plans.push_back({"expanded-two", true, true, m, true, true, true, false, 29});
    if (media) plans.push_back({"expanded-two", true, true, 2, true, true, true, false, 29});
    plans.push_back({"expanded-two", true, true, m, true, true, true, false, 24});
    if (media) plans.push_back({"expanded-two", true, true, 2, true, true, true, false, 24});
    plans.push_back({"expanded-two", true, true, m, true, false, true, false, 24});
    if (media) plans.push_back({"expanded-two", true, true, 2, true, false, true, false, 24});
    plans.push_back({"expanded-two", true, false, m, true, false, true, false, 24});
    // Small screens: the small player above the month.
    if (media) plans.push_back({"expanded-two", true, false, 3, true, false, true, false, 24});
  }
  if (exp || calendar_view_ || agenda_form_) {
    // The calendar and agenda (or the agenda form) were asked for: they
    // stay, the media card goes (it is in the compact view). The time zone
    // row too, with week rows of 22 px and a one-line agenda.
    plans.push_back({"calendar-two", true, false, 0, true, true, true, false, 22});
    if (calendar_view_ || agenda_form_) {
      CenterPlan focus{"calendar-focus", true, false, 0, true, true, true, false, 24};
      focus.focus = true;
      plans.push_back(focus);
      focus.cal_row = 22;
      plans.push_back(focus);
      focus.cal_row = 24;
      focus.agenda = false;
      plans.push_back(focus);
      focus.agenda = true;
      focus.tz = false;
      plans.push_back(focus);
    }
    plans.push_back({"calendar-two", true, false, 0, true, true, false, false, 22});
    plans.push_back({"calendar-two", true, false, 0, true, false, true, false, 24});
    plans.push_back({"calendar-two", true, false, 0, true, false, false, false, 24});
    plans.push_back({"calendar", false, false, 0, true, false, false, false, 24});
    // Only reached first from the media icon (media_view_ below).
    if (media) plans.push_back({"expanded-two", true, false, 3, true, false, false, false, 24});
    if (media) plans.push_back({"expanded-two", true, false, 2, true, false, false, false, 24});
    if (agenda_form_) {
      plans.push_back({"agenda-two", true, false, 0, false, false, true, false, 24});
      plans.push_back({"agenda", false, false, 0, false, false, true, false, 24});
    }
  }
  plans.push_back({"compact", false, false, m, false, false, false, false, 24});
  if (media) {
    plans.push_back({"compact-two", true, false, 2, false, false, false, false, 24});
    plans.push_back({"compact", false, false, 0, false, false, false, false, 24});
  }
  if (brightness_open_) {
    // The brightness gear was just pressed: its card comes first, beside
    // the controls when there is no room under them.
    std::vector<CenterPlan> first;
    first.push_back({"brightness", false, false, m, false, false, false, false, 24});
    first.push_back({"brightness", false, false, 0, false, false, false, false, 24});
    CenterPlan two{"brightness-two", true, false, 0, false, false, false, false, 24};
    two.bright_right = true;
    first.push_back(two);
    plans.insert(plans.begin() + (exp ? std::min<long>(4, long(plans.size())) : 0), first.begin(),
                 first.end());
  }
  if (calendar_view_ && !exp) {
    // The clock asked for the calendar in compact mode: try it first.
    std::stable_partition(plans.begin(), plans.end(),
                          [](const CenterPlan &p) { return p.calendar; });
  }
  if (agenda_form_) {
    // Adding or editing an agenda item: the form stays in view.
    std::stable_partition(plans.begin(), plans.end(),
                          [](const CenterPlan &p) { return p.agenda; });
  }
  if (media_view_ && media) {
    // The media status icon (sde-panel --section media): the player first.
    std::stable_partition(plans.begin(), plans.end(),
                          [](const CenterPlan &p) { return p.media != 0; });
  }
  auto reset = [this] {
    clear();
    resizable(nullptr);
    volume_ = brightness_ = microphone_ = nullptr;
    tray_box_ = nullptr;
    calendar_ = nullptr;
    brightness_card_ = nullptr;
    clock_ = nullptr;
    media_slider_ = nullptr;
    media_service_.clear();
    stream_sliders_.clear();
    body_ = nullptr;
    scroll_ = nullptr;
  };
  // A comfortable height first (one column stays short on tall screens),
  // then all the room the screen has.
  int comfort = std::min(available, S(popup_style::kCenterMaxH));
  int W = 0, H = 0;
  bool done = false;
  std::vector<int> limits = {comfort};
  if (available > comfort) limits.push_back(available);
  for (int limit : limits) {
    for (const auto &plan : plans) {
      reset();
      if (build_center_plan(plan, limit, W, H) && H <= limit) {
        center_plan_ = plan.name + (plan.media == 2 ? "+media-right" : "") +
                       (plan.media == 3 ? "+mini-media" : "") +
                       (plan.calendar && plan.cal_row != 29 ? "+small-calendar" : "") +
                       (plan.tz ? "+timezone" : "");
        done = true;
        break;
      }
    }
    if (done) break;
  }
  if (!done) {
    // Nothing fits (a tiny screen): the plainest layout, cut at the edge.
    reset();
    build_center_plan(plans.back(), available, W, H);
    H = std::min(H, available);
    center_plan_ = plans.back().name + "+clipped";
  }
  body_ = nullptr;
  content_height_ = H;
  resizable(nullptr);
  init_sizes();
  size(W, H);
  // The hero's sky moves at [weather] fps (at most 24); still pictures
  // (reduced motion) only repaint the clock once a second when it shows
  // seconds or a blinking colon, else with the panel clock's minute.
  weather::Settings wset = weather_settings();
  auto *hero = static_cast<ClockHero *>(clock_);
  weather_fps_ = wset.animate ? std::min(24, wset.fps) : (hero && hero->ticks_seconds() ? 1 : 0);
  if (weather_fps_) Fl::add_timeout(1.0 / weather_fps_, weather_tick, this);
  if (media_slider_) Fl::add_timeout(1.0, media_tick, this);
  scroll_to_calendar_ = false;
  sync();
}

void QuickSettings::draw() {
  // A weather animation frame repaints only the clock card (the buffered
  // window clips to it); skip the bookkeeping below.
  bool frame = weather_frame_ && damage() == FL_DAMAGE_CHILD;
  weather_frame_ = false;
  if (frame) {
    draw_children();
    return;
  }
  const auto &t = Panel::instance->theme();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.surface);
    if (t.glass) {
      p.fill_rect(0, 0, w(), header_, t.surface_alt.mix(t.surface, 0.55));
      if (footer_) p.fill_rect(0, h() - footer_, footer_w_ ? footer_w_ : w(), footer_, t.surface_alt.mix(t.surface, 0.4));
    }
    stroke_outline(p, t.border);
    if (header_ > S(8))
      p.line(S(popup_style::kPad), header_ - 0.5, w() - S(popup_style::kPad), header_ - 0.5,
             t.border);
    if (footer_)
      p.line(S(popup_style::kPad), h() - footer_ + 0.5,
             (footer_w_ ? footer_w_ : w()) - S(popup_style::kPad), h() - footer_ + 0.5, t.border);
  }
  draw_children();
  sync_native_tray();
  if (qa_dump_enabled()) {
    std::string extra = "@body\t" + std::to_string(body_x_) + "\t" + std::to_string(body_y_) +
                        "\t" + std::to_string(body_w_) + "\t" + std::to_string(body_h_) + "\t1\n";
    if (auto *hero = mode_ == ACTION_CENTER ? static_cast<ClockHero *>(clock_) : nullptr) {
      auto rect = [hero] {
        return "\t" + std::to_string(hero->x()) + "\t" + std::to_string(hero->y()) + "\t" +
               std::to_string(hero->w()) + "\t" + std::to_string(hero->h()) + "\t1\n";
      };
      // "@weather-rain-day" etc. over the hero while the weather shows;
      // "@sky-night", the clock style and the layout plan always.
      if (hero->weather())
        extra += std::string("@weather-") +
                 weather::kind_name(weather::from_wmo(hero->report().code).kind) +
                 (hero->day() ? "-day" : "-night") + rect();
      extra += std::string("@sky-") + (hero->day() ? "day" : "night") + rect();
      extra += std::string("@clock-") + clockface::style_name(hero->clock().style) + rect();
      // 0.2 Alpha 2: the clock's options.
      if (hero->clock().seconds) extra += "@clock-seconds" + rect();
      if (hero->clock().blink) extra += "@clock-blink" + rect();
      extra += "@plan-" + center_plan_ + "\t0\t0\t" + std::to_string(w()) + "\t" +
               std::to_string(h()) + "\t1\n";
    }
    qa_dump(this, extra);
  }
}

int QuickSettings::handle(int event) {
  bool in_body = scroll_ ? Fl::event_inside(scroll_) : mode_ == ACTION_CENTER;
  if (event == FL_MOUSEWHEEL && in_body && Fl::event_dy()) {
    // The wheel anywhere on the volume or brightness row (icon, slider,
    // percentage) moves that slider.
    for (Fl_Slider *sl : {volume_, brightness_}) {
      // Only on the card itself: the margins and the scroll bar scroll.
      Fl_Widget *card = sl ? sl->parent() : nullptr;
      if (!sl || !sl->visible_r() || Fl::event_y() < sl->y() || Fl::event_y() >= sl->y() + sl->h() ||
          !card || Fl::event_x() < card->x() || Fl::event_x() >= std::min(card->x() + card->w(), w() - S(20)))
        continue;
      sl->value(sl->clamp(sl->value() - Fl::event_dy() * 5));
      sl->do_callback();
      redraw();
      return 1;
    }
  }
  if (event == FL_MOUSEWHEEL && scroll_ && Fl::event_inside(scroll_)) {
    // Route wheel gestures explicitly through this override-redirect window;
    // keep application-tray and slider wheel controls local to their widgets.
    auto *target = Fl::belowmouse();
    // belowmouse can still name a previous control after an async rebuild.
    // Forward wheel events only when the pointer is actually inside it.
    if (target && Fl::event_inside(target) &&
        (dynamic_cast<Fl_Slider *>(target) || dynamic_cast<TrayIndicator *>(target) ||
         dynamic_cast<Fl_Choice *>(target)))
      return PanelPopup::handle(event);
    int limit = std::max(0, content_height_ - scroll_->h());
    scroll_->scroll_to(0, std::max(0, std::min(limit, scroll_->yposition() + Fl::event_dy() * S(36))));
    redraw();
    return 1;
  }
  return PanelPopup::handle(event);
}
