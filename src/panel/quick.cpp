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

#include "build_config.h"
#include "calendar.h"
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
#include "power.h"
#include "process.h"
#include "sni.h"
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

void SystemStatus::refresh_battery() {
  BatteryState b;
  for (auto &dev : list_dir("/sys/class/power_supply")) {
    std::string base = "/sys/class/power_supply/" + dev, type, cap, status, scope;
    if (!read_file(base + "/type", type) || trim(type) != "Battery") continue;
    if (read_file(base + "/scope", scope) && trim(scope) == "Device") continue;  // mice etc.
    if (!read_file(base + "/capacity", cap)) continue;
    b.present = true;
    b.percent = std::max(0, std::min(100, atoi(cap.c_str())));
    read_file(base + "/status", status);
    status = trim(status);
    b.charging = status == "Charging";
    b.full = status == "Full" || (status == "Not charging" && b.percent >= 95);
    // Remaining time from energy (µWh) / power (µW), or charge / current.
    std::string now, full, rate;
    bool ok = read_file(base + "/energy_now", now) && read_file(base + "/energy_full", full) &&
              read_file(base + "/power_now", rate);
    if (!ok)
      ok = read_file(base + "/charge_now", now) && read_file(base + "/charge_full", full) &&
           read_file(base + "/current_now", rate);
    double r = ok ? atof(rate.c_str()) : 0;
    if (r > 0) {
      double hours =
          b.charging ? (atof(full.c_str()) - atof(now.c_str())) / r : atof(now.c_str()) / r;
      if (hours > 0 && hours < 48) b.minutes = int(hours * 60);
    }
    break;
  }
  bool diff = b.present != battery.present || b.percent != battery.percent ||
              b.charging != battery.charging || b.full != battery.full ||
              b.minutes != battery.minutes;
  battery = b;
  if (diff) changed();
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
    int size = t.font_size + (style_ == 1 ? 1 : style_ == 3 ? 12 : 0);
    text_font(size, style_ == 1 || style_ == 3);
    draw_text(text_(), x(), y(), w(), h(), style_ == 2 ? t.text_dim : t.text, style_ == 4 ? 2 : 0);
  }

 private:
  StringFn text_;
  int style_;  // 0 text, 1 title, 2 dim, 3 large, 4 right aligned
};

class ActionCard : public Fl_Group {
 public:
  ActionCard(int x, int y, int w) : Fl_Group(x, y, w, S(40)) {
    box(FL_NO_BOX);
    resizable(nullptr);
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(14), t.surface_alt);
      if (t.glass) p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(14), t.border);
    }
    draw_children();
  }
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
    Color bg = on ? accent : t.surface_alt;
    if (multi && !on) bg = t.surface.mix(accent, t.dark ? 0.18 : 0.08);
    if (hot || value()) bg = bg.mix(on ? t.on_accent : t.text, value() ? 0.15 : 0.06);
    Color white = Color::hex(0xFFFFFF), black = Color::hex(0x101820);
    Color ink = on ? (white.contrast(accent) >= 4.5 ? white : black) : t.text;
    if (!active_r()) ink = t.text_dim;
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(12), bg);
      if (hot || on) p.stroke_rounded(x(), y(), w(), h(), S(12), on ? accent : t.border);
      double size = S(row_ ? 20 : 23);
      icon_(p, row_ && !title_.empty() ? x() + S(12) : x() + (w() - size) / 2.0,
            row_ ? y() + (h() - size) / 2.0 : y() + S(10), size, ink);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(12));
    }
    text_font(t.font_size - (row_ ? 0 : S(1)), true);
    if (!row_ && text_width(title_) > w() - S(8))
      text_font(std::max(S(9), t.font_size - S(2)), true);
    int tx = row_ ? x() + S(42) : x() + S(4), tw = row_ ? w() - S(52) : w() - S(8);
    draw_text(title_, tx, row_ ? y() : y() + S(36), tw, row_ ? h() : S(20), ink, row_ ? 0 : 1);
    if (sub_ && !row_) {
      text_font(std::max(S(9), t.font_size - 2));
      draw_text(sub_(), tx, y() + S(55), tw, S(16), on ? ink : t.text_dim, 1);
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
  int button_h() const { return h() - S(22); }
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
      p.fill_rounded(x(), y(), w(), bh, S(12), bg);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, bh - 1, S(12),
                       on ? accent.mix(t.text, 0.1) : t.border);
      double size = S(20);
      double ix = more_ ? x() + (split() - x() - size) / 2.0 : x() + (w() - size) / 2.0;
      icon_(p, ix, y() + (bh - size) / 2.0, size, ink);
      if (more_) {
        p.line(split() + 0.5, y() + S(10), split() + 0.5, y() + bh - S(10), ink.alpha(0.35));
        double cs = S(12);
        glyph::chevron(p, split() + (x() + w() - split() - cs) / 2.0, y() + (bh - cs) / 2.0, cs,
                       true, ink);
      }
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), bh, S(12));
    }
    std::string caption = caption_ ? caption_() : title_;
    text_font(std::max(S(9), t.font_size - S(2)));
    draw_text(caption, x() - S(2), y() + bh + S(2), w() + S(4), S(20), t.text, 1);
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

class ClockCard : public Fl_Widget {
 public:
  ClockCard(int x, int y, int w, int h, bool flat = false, const weather::Report *wx = nullptr,
            bool fahrenheit = false, bool animate = true)
      : Fl_Widget(x, y, w, h), flat_(flat), animate_(animate), fahrenheit_(fahrenheit) {
    clear_visible_focus();
    if (wx) {
      wx_ = *wx;
      sky_ = true;
      // Named for UI tests and shown as the tooltip.
      std::string tip = std::string(_("Weather")) + ": " +
                        _(weather::from_wmo(wx_.code).text) + ", " +
                        weather::format_temperature(wx_.temp_c, fahrenheit_);
      if (!wx_.location.city.empty()) tip += ", " + wx_.location.city;
      copy_tooltip(tip.c_str());
    }
  }
  bool sky() const { return sky_; }
  const weather::Report &report() const { return wx_; }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    const Config &cfg = Panel::instance->config();
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    bool analog = cfg.get("action_center", "clock_style", "digital") == "analog";
    double r = std::min(double(S(32)), h() / 2.0 - S(4));
    double cx = x() + (flat_ && !sky_ ? 0 : S(16)) + r, cy = y() + h() / 2.0;
    {
      Painter p(window()->w(), window()->h());
      if (sky_) {
        // A still picture at reduced motion (always the same moment).
        weather::draw_scene(p, x(), y(), w(), h(), S(14), wx_, animate_ ? anim_seconds() : 2.0);
      } else if (!flat_) {
        // Soft accent glow on the card: a two-stop gradient instead of a flat fill.
        p.fill_gradient(x(), y(), w(), h(), S(16),
                        t.surface_alt.mix(t.accent, t.dark ? 0.24 : 0.14), t.surface_alt, true);
        p.stroke_rounded(x(), y(), w(), h(), S(16), t.border);
      }
      if (analog) {
        p.circle(cx, cy, r, flat_ ? t.surface_alt : t.surface);
        for (int i = 0; i < 12; ++i) {
          double a = i * M_PI / 6;
          p.line(cx + std::sin(a) * (r - r / 8), cy - std::cos(a) * (r - r / 8),
                 cx + std::sin(a) * (r - r / 4.5), cy - std::cos(a) * (r - r / 4.5), t.text_dim,
                 S(1));
        }
        double hour = (lt.tm_hour % 12 + lt.tm_min / 60.0) * M_PI / 6;
        double minute = lt.tm_min * M_PI / 30;
        p.line(cx, cy, cx + std::sin(hour) * r * 0.48, cy - std::cos(hour) * r * 0.48, t.text,
               S(3));
        p.line(cx, cy, cx + std::sin(minute) * r * 0.76, cy - std::cos(minute) * r * 0.76,
               t.accent, S(2));
        p.circle(cx, cy, S(3), t.accent);
      }
    }
    char date[128], clock[64];
    strftime(clock, sizeof clock, cfg.get_bool("panel", "clock_24h", true) ? "%H:%M" : "%I:%M %p",
             &lt);
    strftime(date, sizeof date, "%A, %e %B", &lt);
    // Time on the left, greeting and date stacked beside it.
    int left = analog ? int(cx + r + S(14)) : x() + (flat_ && !sky_ ? 0 : S(16));
    text_font(t.font_size + (analog ? 10 : flat_ ? 16 : 22), true);
    int cw = text_width(clock) + S(8);
    if (sky_) {
      // White text on the sky (the scene shades its left side).
      Color ink = Color::hex(0xFFFFFF), soft = Color::hex(0xE6EDF6),
            shade = Color::hex(0x0B1424);
      // Text with a soft shadow, readable over clouds and snow.
      auto draw_text = [&shade](const std::string &s, int x, int y, int w, int h,
                                const Color &c) {
        sde::draw_text(s, x + 1, y + 1, w, h, shade);
        sde::draw_text(s, x, y, w, h, c);
      };
      // The analog face already tells the time: room for the weather.
      if (!analog) draw_text(clock, left, y(), cw, h(), ink);
      int rx = analog ? left : left + cw + S(8), rw = x() + w() - rx - S(8);
      int mid = y() + h() / 2;
      std::string cond = _(weather::from_wmo(wx_.code).text);
      std::string temp = weather::format_temperature(wx_.temp_c, fahrenheit_);
      text_font(t.font_size + 1, true);
      int tw = text_width(temp) + S(6);
      draw_text(temp, rx, mid - S(27), std::min(tw, rw), S(20), ink);
      text_font(t.font_size, true);
      draw_text(cond, rx + tw, mid - S(27), std::max(0, rw - tw), S(20), ink);
      text_font(t.font_size - 1);
      draw_text(trim(date), rx, mid - S(8), rw, S(18), soft);
      std::string place = wx_.location.city;
      if (place.empty() && wx_.location.valid) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.2f, %.2f", wx_.location.lat, wx_.location.lon);
        place = buf;
      }
      draw_text(place, rx, mid + S(9), rw, S(18), soft);
      return;
    }
    draw_text(clock, left, y(), cw, h(), t.text);
    int rx = left + cw + S(10), rw = x() + w() - rx - S(4);
    bool greet = cfg.get_bool("action_center", "show_greeting", true);
    int mid = y() + h() / 2;
    if (greet) {
      const char *greeting = lt.tm_hour < 12   ? _("Good morning")
                             : lt.tm_hour < 18 ? _("Good afternoon")
                                               : _("Good evening");
      text_font(t.font_size, true);
      draw_text(greeting, rx, mid - S(20), rw, S(20), t.accent_text);
    }
    text_font(t.font_size - 1);
    draw_text(trim(date), rx, greet ? mid : mid - S(10), rw, S(20), t.text_dim);
  }

 private:
  bool flat_, animate_, fahrenheit_;
  bool sky_ = false;
  weather::Report wx_;
};

// Initial in an accent circle, then the user name.
class UserBadge : public Fl_Widget {
 public:
  UserBadge(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    const Theme &t = Panel::instance->theme();
    std::string name = user_name();
    std::string initial = name.empty() ? "?" : std::string(1, char(toupper((unsigned char)name[0])));
    {
      Painter p(window()->w(), window()->h());
      p.fill_gradient(x(), y(), h(), h(), h() / 2.0, t.accent.mix(Color::hex(0xFFFFFF), 0.2),
                      t.accent.mix(Color::hex(0x000000), 0.1));
    }
    text_font(t.font_size, true);
    draw_text(initial, x(), y(), h(), h(), t.on_accent, 1);
    draw_text(name, x() + h() + S(10), y(), w() - h() - S(10), h(), t.text);
  }
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
IconFn back_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    glyph::chevron(p, x + s * .2, y + s * .2, s * .6, false, c);
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
// flyout's settings button opens its own page as soon as SDE Settings has it.
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
// The SDE Settings page |page|, else |fallback| when installed, else the
// SDE Settings overview.
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
struct SectionBuilder {
  QuickSettings *qs;
  int pad, inner, y;
  void label(std::string text, int style = 0, int height = 24) {
    new ActionLabel(pad, y, inner, S(height), [text] { return text; }, style);
    y += S(height);
  }
  ActionButton *row(std::string text, IconFn icon, std::function<void()> fn, bool enabled = true,
                    std::function<bool()> on = nullptr) {
    auto *b = new ActionButton(pad + S(8), y, inner - S(16), S(36), text, icon, fn, on, -1, true);
    if (!enabled) b->deactivate();
    y += S(42);
    return b;
  }
  void info(std::string text) {
    new ActionLabel(pad + S(12), y, inner - S(24), S(24), [text] { return text; }, 2);
    y += S(24);
  }
  ActionCard *start_card(std::string title) {
    auto *card = new ActionCard(pad, y, inner);
    y += S(10);
    if (!title.empty()) {
      new ActionLabel(pad + S(12), y, inner - S(24), S(24), [title] { return title; }, 1);
      y += S(28);
    } else {
      y += S(2);
    }
    return card;
  }
  void end_card(ActionCard *card) {
    card->size(inner, y - card->y() + S(6));
    card->end();
    y += S(12);
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
  Fl::repeat_timeout(1.0 / q->weather_fps_, weather_tick, data);
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
    for (const char *key : {"enabled", "unit", "animation", "location"})
      out << '|' << cfg.get("weather", key);
    for (const char *key : {"enabled", "schedule", "from", "to"})
      out << '|' << cfg.get("night_light", key);
    out << '|' << cfg.get("power", "idle_dim_battery") << cfg.get("brightness", "external_monitor");
  }
  for (const auto &a : c.streams) out << '|' << a.id << a.app << a.title << a.muted;
  for (const auto &d : c.drives) out << '|' << d.path << d.mountpoint();
  for (const auto &p : c.power_profiles) out << '|' << p;
  out << c.power_profile;
  for (const auto &d : c.devices)
    out << '|' << d.device << d.state << d.connection << d.ip << d.gateway << d.dns;
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
  for (const char *key : {"style", "position", "height", "width_percent", "clock_24h"})
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
  int scroll = self->body_ ? std::max(0, self->body_->yposition()) : 0;
  bool focus = Fl::focus() && Fl::focus()->window() == self;
  std::string focus_name = focused_name(self);
  self->rebuild();
  if (!self->scroll_to_calendar_)
    self->body_->scroll_to(
        0, std::min(scroll, std::max(0, self->content_height_ - self->body_->h())));
  if (self->reveal_brightness_ && self->brightness_card_) {
    // Just opened: bring the settings card into view (its top first).
    auto *c = self->brightness_card_;
    int y = self->body_->yposition();
    int bottom = c->y() + c->h() - (self->body_->y() + self->body_->h());
    int top = c->y() - self->body_->y() - S(8);
    if (bottom > 0) y += std::min(bottom, std::max(0, top));
    self->body_->scroll_to(0, std::max(0, std::min(y, self->content_height_ - self->body_->h())));
  }
  self->reveal_brightness_ = false;
  self->reposition();
  if (!focus_name.empty())
    restore_focus(self, focus_name);
  else if (focus && self->body_->children())
    self->body_->child(0)->take_focus();
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
  int x = b.pad + S(8), right = b.pad + b.inner - S(8);
  auto *mute = new ActionButton(
      x, b.y, S(38), S(36), "",
      [](Painter &p, double x, double y, double z, const Color &ink) {
        const auto &a = SystemStatus::get().audio;
        glyph::speaker(p, x, y, z, a.volume > 66 ? 3 : a.volume > 33 ? 2 : a.volume > 0 ? 1 : 0,
                       a.muted, ink);
      },
      [] { SystemStatus::get().toggle_mute(); }, [] { return SystemStatus::get().audio.muted; },
      -1, true);
  mute->copy_tooltip(_("Mute"));
  int more_w = more ? S(36) : 0;
  int pct = S(44);
  volume_ = new Slider(x + S(46), b.y, right - more_w - pct - (x + S(46)) - S(4), S(36));
  volume_->bounds(0, 100);
  volume_->value(s.audio.volume);
  volume_->tooltip(_("Output volume"));
  volume_->callback([](Fl_Widget *w, void *) {
    SystemStatus::get().set_volume(int(static_cast<Fl_Slider *>(w)->value()));
  });
  new ActionLabel(
      right - more_w - pct, b.y, pct, S(36),
      [] { return std::to_string(SystemStatus::get().audio.volume) + "%"; }, 4);
  if (more)
    icon_button(right - more_w + S(2), b.y, more_w - S(2), S(36),
                _("Sound output and applications"), chevron_icon(),
                [] { later([] { Panel::instance->open_flyout("sound"); }); });
  b.y += S(42);
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
  int x = b.pad + S(8), right = b.pad + b.inner - S(8), pct = S(44), more_w = S(36);
  auto *icon = new ActionButton(x, b.y, S(38), S(36), "", sun_icon(), [] {}, nullptr, -1, true);
  icon->clear_visible_focus();
  // The gear at the end opens brightness and Night Light settings.
  bool open = brightness_open_;
  auto *more = new ActionButton(
      right - more_w + S(2), b.y, more_w - S(2), S(36), "", gear_icon(),
      [this, open] { later([this, open] { set_brightness_menu(!open); }); },
      [this] { return brightness_open_; }, -1, true);
  more->copy_tooltip(_("Brightness settings"));
  right -= more_w;
  brightness_ = new Slider(x + S(46), b.y, right - pct - (x + S(46)) - S(4), S(36));
  brightness_->bounds(1, 100);
  brightness_->value(s.brightness.percent());
  brightness_->tooltip(_("Brightness"));
  brightness_->callback([](Fl_Widget *w, void *) {
    SystemStatus::get().set_brightness(int(static_cast<Fl_Slider *>(w)->value()));
  });
  new ActionLabel(
      right - pct, b.y, pct, S(36),
      [] { return std::to_string(SystemStatus::get().brightness.percent()) + "%"; }, 4);
  b.y += S(42);
}

void QuickSettings::brightness_settings(SectionBuilder &b) {
  const auto &cfg = Panel::instance->config();
  const auto &t = Panel::instance->theme();
  int pad = b.pad, inner = b.inner;
  auto *card = b.start_card(_("Brightness and Night Light"));
  brightness_card_ = card;
  icon_button(pad + inner - S(46), card->y() + S(6), S(34), S(32), _("Close brightness settings"),
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
  int x = pad + S(12), w = inner - S(24), lw = S(92), vw = S(64);
  new ActionLabel(x, b.y, lw, S(32), [] { return std::string(_("Warmth")); }, 2);
  auto *warm = new Slider(x + lw, b.y, w - lw - vw, S(32));
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
      x + w - vw, b.y, vw, S(32),
      [warm] { return std::to_string(int(warm->value())) + " K"; }, 4);
  b.y += S(38);
  {
    new ActionLabel(x, b.y, lw, S(32), [] { return std::string(_("Schedule")); }, 2);
    auto *sched = new Fl_Choice(x + lw, b.y, w - lw, S(32));
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
    b.y += S(38);
  }
  if (nl.schedule == "custom") {
    int half = (w - lw - S(8)) / 2;
    new ActionLabel(x, b.y, lw, S(32), [] { return std::string(_("From – to")); }, 2);
    for (int which = 0; which < 2; ++which) {
      auto *c = new Fl_Choice(x + lw + which * (half + S(8)), b.y, half, S(32));
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
    b.y += S(38);
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
  new ActionLabel(x, b.y, lw, S(32), [] { return std::string(_("Dim level")); }, 2);
  auto *level = new Slider(x + lw, b.y, w - lw - vw, S(32));
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
      x + w - vw, b.y, vw, S(32),
      [level] { return std::to_string(int(level->value())) + "%"; }, 4);
  b.y += S(38);
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

void QuickSettings::player_card(SectionBuilder &b, const MediaPlayer &p) {
  const auto &t = Panel::instance->theme();
  int pad = b.pad, inner = b.inner;
  auto *card = b.start_card("");
  int x = pad + S(12), w = inner - S(24), art = S(56);
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
        pt.fill_gradient(x(), y(), w(), h(), S(10), th.accent.mix(Color::hex(0xFFFFFF), 0.15),
                         th.accent.mix(Color::hex(0x000000), 0.15), true);
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
  int tx = x + art + S(12), tw = w - art - S(12);
  auto *tl = new ActionLabel(tx, b.y + S(6), tw, S(22), [title] { return title; }, 1);
  tl->copy_tooltip((std::string(_("Now playing")) + ": " + title).c_str());
  auto *al = new ActionLabel(tx, b.y + S(30), tw, S(20), [artist] { return artist; }, 2);
  if (!artist.empty()) al->copy_tooltip((std::string(_("Artist")) + ": " + artist).c_str());
  b.y += art + S(8);
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
  new ActionLabel(x, b.y, w / 2, S(18), time_of(false), 2);
  new ActionLabel(x + w / 2, b.y, w - w / 2, S(18),
                  [f = time_of(true)] { return f(); }, 4);
  b.y += S(20);
  // Only previous, play/pause and next.
  int bw = S(44), gap = S(14), cx = x + (w - 3 * bw - 2 * gap) / 2;
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
    auto *btn = new ActionButton(cx + i * (bw + gap), b.y, bw, S(40), "",
                                 transport_icon(buttons[i].icon),
                                 [service, cmd] { PanelControls::get().media_command(service, cmd); },
                                 i == 1 ? std::function<bool()>([] { return true; }) : nullptr, -1,
                                 true);
    btn->copy_tooltip(_(buttons[i].tip));
    if (!buttons[i].on) btn->deactivate();
  }
  (void)t;
  b.y += S(46);
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
      auto *choice = new Fl_Choice(pad + S(12), b.y, inner - S(24), S(30));
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
      b.y += S(38);
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
    microphone_ = new Slider(pad + S(14), b.y, inner - S(28), S(36));
    microphone_->bounds(0, 100);
    microphone_->value(c.mic_volume);
    microphone_->tooltip(_("Microphone volume"));
    microphone_->callback([](Fl_Widget *w, void *) {
      PanelControls::get().microphone_volume(int(static_cast<Fl_Slider *>(w)->value()));
    });
    b.y += S(42);
    if (!c.sources.empty()) {
      auto *choice = new Fl_Choice(pad + S(12), b.y, inner - S(24), S(30));
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
      b.y += S(38);
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
      auto *slider = new Slider(pad + S(14), b.y, inner - S(84), S(34));
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
          pad + inner - S(62), b.y, S(50), S(34), "",
          [muted = a.muted](Painter &p, double x, double y, double z, const Color &ink) {
            glyph::speaker(p, x, y, z, 3, muted, ink);
          },
          [id = a.id, muted = !a.muted] { PanelControls::get().mute_stream(id, muted); },
          [muted = a.muted] { return muted; }, -1, true);
      mute->copy_tooltip(a.muted ? _("Unmute application") : _("Mute application"));
      b.y += S(40);
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
          [] { SystemStatus::get().troubleshoot_network(); });
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
      b.row(d.state == "connected" ? _("Disconnect LAN") : _("Connect LAN"), network_icon,
            [d] { PanelControls::get().network_device(d.device, d.state != "connected"); });
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

void QuickSettings::power_section(SectionBuilder &b) {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  int pad = b.pad, inner = b.inner;
  auto *card = b.start_card(_("Battery"));
  if (s.battery.present) {
    new ActionLabel(
        pad + S(12), b.y, inner - S(24), S(40),
        [] { return std::to_string(SystemStatus::get().battery.percent) + "%"; }, 3);
    b.y += S(40);
    new ActionLabel(
        pad + S(12), b.y, inner - S(24), S(24),
        [] {
          const auto &b = SystemStatus::get().battery;
          std::string text = b.charging ? _("Charging") : b.full ? _("Fully charged") : _("On battery");
          if (b.minutes > 0 && !b.full)
            text += " · " + format(b.charging ? _("%d h %02d min until full")
                                              : _("%d h %02d min remaining"),
                                   b.minutes / 60, b.minutes % 60);
          return text;
        },
        2);
    b.y += S(26);
  } else {
    b.info(_("No battery: the computer runs on mains power."));
  }
  b.end_card(card);
  // power-profiles-daemon modes, when the service is installed.
  if (!c.power_profiles.empty()) {
    card = b.start_card(_("Power mode"));
    int n = int(c.power_profiles.size()), gap = S(6), bw = (inner - S(24) - (n - 1) * gap) / n;
    static const std::map<std::string, std::string> names = {
        {"power-saver", _("Power Saver")}, {"balanced", _("Balanced")},
        {"performance", _("Performance")}};
    for (int i = 0; i < n; ++i) {
      const auto &id = c.power_profiles[size_t(i)];
      std::string label = names.count(id) ? names.at(id) : id;
      if (id == c.power_profile) label = "✓ " + label;
      new CommandButton(pad + S(12) + i * (bw + gap), b.y, bw, S(34), label,
                        [id] { PanelControls::get().set_power_profile(id); });
    }
    b.y += S(42);
    b.end_card(card);
  }
  if (s.brightness.available) {
    card = b.start_card(_("Brightness"));
    brightness_row(b);
    b.end_card(card);
  }
  std::string prefs = first_program({"sde-power-preferences", "xfce4-power-manager-settings"});
  b.settings_row(_("Power settings"),
                 settings_for("power", prefs.empty() ? std::vector<std::string>{"sde-settings",
                                                                                "--page", "system"}
                                                     : std::vector<std::string>{prefs}));
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
  int cell = S(36), columns = std::max(1, (inner - S(24)) / cell);
  if (items.empty() && !legacy) b.info(_("No applications are in the tray."));
  for (size_t i = 0; i < items.size(); ++i)
    new TrayIndicator(pad + S(12) + int(i % columns) * cell, b.y + int(i / columns) * cell, cell,
                      *items[i]);
  // XEmbed icons continue on the last StatusNotifier row when there is room.
  int last = int(items.size() % size_t(columns)),
      rows = int((items.size() + columns - 1) / columns);
  int tx = pad + S(12), ty = b.y + rows * cell;
  if (legacy) {
    int slot = tray->icon_size() + S(6), room = inner - S(24) - last * cell;
    if (rows && last && room >= slot) {
      tx += last * cell;
      ty -= cell;
      tray_columns_ = std::max(1, room / slot);
    } else {
      tray_columns_ = std::max(1, (inner - S(24)) / slot);
    }
    int th = tray->grid_height(tray_columns_);
    tray_box_ = new Fl_Box(tx, ty + std::max(0, (cell - th) / 2),
                           inner - S(24) - (tx - pad - S(12)), th);
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

void QuickSettings::build_center(SectionBuilder &b) {
  const auto &cfg = Panel::instance->config();
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  int pad = b.pad, inner = b.inner;
  auto open_flyout = [](const char *section) {
    return [section] { later([section] { Panel::instance->open_flyout(section); }); };
  };
  // Quick tiles: toggles first, then shortcuts.
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
  // SDE Settings has its button in the footer; the clock's alarms get a tile.
  tiles.push_back({_("Alarms"), alarm_icon(),
                   [] { later([] { Panel::instance->open_alarms(); }); }, nullptr,
                   [] {
                     long long next = Alarms::get().next_alarm();
                     if (!next) return std::string(_("Alarms"));
                     time_t t = time_t(next);
                     struct tm lt;
                     localtime_r(&t, &lt);
                     char buf[16];
                     bool h24 = Panel::instance->config().get_bool("panel", "clock_24h", true);
                     strftime(buf, sizeof buf, h24 ? "%H:%M" : "%I:%M %p", &lt);
                     return std::string(buf);
                   },
                   nullptr});
  int columns = cfg.get_int("action_center", "tile_columns", 4) == 3 ? 3 : 4, gap = S(8),
      tw = (inner - (columns - 1) * gap) / columns, th = S(74);
  for (size_t i = 0; i < tiles.size(); ++i) {
    const auto &v = tiles[i];
    new QuickTile(pad + int(i % columns) * (tw + gap), b.y + int(i / columns) * (th + gap), tw, th,
                  v.title, v.icon, v.click, v.on, v.caption, v.more, int(i));
  }
  b.y += int((tiles.size() + columns - 1) / columns) * (th + gap) + S(6);

  // Sliders.
  if (s.audio.available || s.brightness.available) {
    auto *card = b.start_card("");
    if (s.audio.available) volume_row(b, true);
    if (s.brightness.available) brightness_row(b);
    b.y -= S(4);
    b.end_card(card);
  }
  if (brightness_open_) brightness_settings(b);
  if (!expanded()) return;

  if (cfg.get_bool("action_center", "show_media", true)) {
    auto players = c.players();
    if (!players.empty()) player_card(b, players.front());
  }
  // Calendar card; the panel clock always shows it.
  {
    bool open = calendar_view_ || cfg.get_bool("action_center", "show_calendar", true);
    auto *card = b.start_card("");
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char date[128];
    strftime(date, sizeof date, "%A, %e %B %Y", &lt);
    std::string today = trim(date);
    new ActionLabel(pad + S(12), b.y - S(4), inner - S(70), S(32), [today] { return today; }, 1);
    icon_button(pad + inner - S(46), b.y - S(4), S(34), S(32),
                open ? _("Hide calendar") : _("Show calendar"), fold_icon(open), [this, open] {
                  calendar_view_ = false;
                  Panel::instance->set_setting("action_center", "show_calendar",
                                               open ? "false" : "true");
                  status_changed();
                });
    b.y += S(32);
    if (open) {
      auto *cal = new MonthCalendar(pad + S(12), b.y, inner - S(24));
      calendar_ = cal;
      if (shown_year_) cal->show_month(shown_year_, shown_month_);
      cal->select(agenda_year_, agenda_month_, agenda_day_);
      cal->marked = [](int y, int m, int d) { return agenda::has_items(Alarms::get().items(), y, m, d); };
      cal->moved = [this](int y, int m) {
        shown_year_ = y;
        shown_month_ = m;
      };
      cal->picked = [this](int y, int m, int d) { agenda_pick(y, m, d); };
      b.y += calendar_->h();
    }
    b.end_card(card);
    if (open) agenda_card(b);
  }
  if (cfg.get_bool("action_center", "show_notifications", true)) {
    auto *card = b.start_card(_("Notifications"));
    icon_button(pad + inner - S(46), card->y() + S(6), S(34), S(32), _("Notification settings"),
                gear_icon(), b.launch({"sde-settings", "--page", "notifications"}));
    b.info(cfg.get_bool("notifications", "do_not_disturb", false)
               ? _("Do Not Disturb is on: notifications are silenced.")
               : _("No new notifications"));
    b.end_card(card);
  }
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
  if (items.empty() && !agenda_form_) b.info(_("Nothing planned. Add an item with +."));
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
    if (!extra.empty()) {
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

// The session row: the user, SDE Settings, Lock and Power. Power opens a
// row above it with Sleep, Log Out, Restart and Shut Down (the sde-logout
// actions); Power again closes it.
void QuickSettings::footer(int W, int H, int pad) {
  int fy = H - S(58) + S(11), fb = S(36);
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
      new CommandButton(pad + i * (bw + gap), H - footer_ + S(10), bw, fb,
                        choices[size_t(i)].name,
                        launch({"sde-logout", "--action", choices[size_t(i)].action}));
  }
  bool open = power_open_;
  std::vector<std::pair<std::string, std::function<void()>>> foot = {
      {_("SDE Settings"), launch({"sde-settings"})},
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
  clear();
  resizable(nullptr);
  volume_ = brightness_ = microphone_ = nullptr;
  tray_box_ = nullptr;
  calendar_ = nullptr;
  brightness_card_ = nullptr;
  clock_ = nullptr;
  media_slider_ = nullptr;
  media_service_.clear();
  Fl::remove_timeout(weather_tick, this);
  Fl::remove_timeout(media_tick, this);
  // The weather picture behind the clock, while online.
  const weather::Report *wx = nullptr;
  weather::Settings wset = weather_settings();
  if (mode_ == ACTION_CENTER && Panel::instance->config().get_bool("action_center", "show_clock", true))
    wx = weather_report();
  stream_sliders_.clear();
  const auto &t = Panel::instance->theme();
  const auto &cfg = Panel::instance->config();
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  bool fly = mode_ == FLYOUT;
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  int W = fly ? std::min(S(360), mw - S(16)) : std::min(S(400), mw);
  int pad = S(14), scrollbar = S(8), inner = W - 2 * pad - scrollbar;
  auto *panel = Panel::instance;
  int available =
      std::max(S(120), t.panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my);
  if (fly) available = std::max(S(120), available - S(16));
  header_ = fly ? S(48) : wx ? S(92) : S(70);
  footer_ = fly ? 0 : power_open_ ? S(104) : S(58);

  begin();
  body_ = new Fl_Scroll(S(2), header_, W - S(4), std::max(S(40), available - header_ - footer_));
  body_->type(Fl_Scroll::VERTICAL);
  body_->resizable(nullptr);
  body_->scrollbar_size(scrollbar);
  body_->box(FL_FLAT_BOX);
  body_->color(t.surface.fl());
  // Give Fl_Scroll a stable zero origin. Otherwise its padding is reported as
  // negative scroll and repeated service rebuilds accumulate blank space.
  new Fl_Box(body_->x(), body_->y(), 1, 1);
  SectionBuilder b{this, pad, inner, header_ + S(4)};
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
  if (fly)
    build_flyout(b);
  else
    build_center(b);

  content_height_ = b.y - header_ + S(4);
  new Fl_Box(pad, b.y, inner, S(4));
  body_->end();
  int H = std::min(available, header_ + content_height_ + footer_);
  body_->size(W - S(4), H - header_ - footer_);
  begin();
  // Created after the body, so keyboard focus starts on the first control.
  // Fixed header: the flyout's title, or the Action Center's clock and the
  // compact/expanded switch.
  if (fly) {
    std::string title = section_title(section_);
    new ActionLabel(pad + S(4), S(8), W - 2 * pad, S(34), [title] { return title; }, 1);
  } else {
    int toggle = S(36);
    if (wx)
      clock_ = new ClockCard(pad, S(8), W - 2 * pad - 2 * toggle - S(14), header_ - S(16), true,
                             wx, wset.fahrenheit, wset.animate);
    else if (cfg.get_bool("action_center", "show_clock", true))
      clock_ = new ClockCard(pad + S(4), S(8), W - 2 * pad - 2 * toggle - S(18), header_ - S(14),
                             true);
    else
      new ActionLabel(pad + S(4), S(8), W - 2 * pad - 2 * toggle - S(6), header_ - S(14),
                      [] { return std::string(_("Action Center")); }, 1);
    bool open = expanded();
    icon_button(W - pad - 2 * toggle - S(6), (header_ - toggle) / 2, toggle, toggle,
                _("Action Center settings"), gear_icon(),
                b.launch(settings_for("action-center", {})));
    icon_button(W - pad - toggle, (header_ - toggle) / 2, toggle, toggle,
                open ? _("Show less") : _("Show more"), fold_icon(open),
                [this] { toggle_expanded(); });
  }
  if (!fly) footer(W, H, pad);
  end();
  resizable(nullptr);
  init_sizes();
  size(W, H);
  if (scroll_to_calendar_ && calendar_) {
    // Show the whole calendar card when the clock opened the Action Center;
    // on a small screen at least the month grid from its top.
    int top = calendar_->y() - body_->y() - S(52);
    int bottom = calendar_->y() + calendar_->h() + S(12) - body_->y() - body_->h();
    int scroll = bottom <= top ? bottom : std::min(top + S(52), bottom);
    body_->scroll_to(0, std::max(0, std::min(scroll, content_height_ - body_->h())));
    scroll_to_calendar_ = false;
  }
  weather_fps_ = wset.fps;
  if (wx && wset.animate) Fl::add_timeout(1.0 / wset.fps, weather_tick, this);
  if (media_slider_) Fl::add_timeout(1.0, media_tick, this);
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
      if (footer_) p.fill_rect(0, h() - footer_, w(), footer_, t.surface_alt.mix(t.surface, 0.4));
    }
    p.stroke_rounded(0.5, 0.5, w() - 1, h() - 1, 0, t.border);
    p.line(S(14), header_ - 0.5, w() - S(14), header_ - 0.5, t.border);
    if (footer_) p.line(S(14), h() - footer_ + 0.5, w() - S(14), h() - footer_ + 0.5, t.border);
  }
  draw_children();
  sync_native_tray();
  if (qa_dump_enabled() && body_) {
    std::string extra = "@body\t" + std::to_string(body_->x()) + "\t" +
                        std::to_string(body_->y()) + "\t" + std::to_string(body_->w()) + "\t" +
                        std::to_string(body_->h()) + "\t1\n";
    // "@weather-rain-day" etc. over the clock card while the sky shows.
    if (auto *cc = dynamic_cast<ClockCard *>(clock_))
      if (cc->sky())
        extra += std::string("@weather-") + weather::kind_name(weather::from_wmo(cc->report().code).kind) +
                 (cc->report().is_day ? "-day" : "-night") + "\t" + std::to_string(cc->x()) +
                 "\t" + std::to_string(cc->y()) + "\t" + std::to_string(cc->w()) + "\t" +
                 std::to_string(cc->h()) + "\t1\n";
    qa_dump(this, extra);
  }
}

int QuickSettings::handle(int event) {
  if (event == FL_MOUSEWHEEL && body_ && Fl::event_inside(body_) && Fl::event_dy()) {
    // The wheel anywhere on the volume or brightness row (icon, slider,
    // percentage) moves that slider.
    for (Fl_Slider *sl : {volume_, brightness_}) {
      // Only on the card itself: the margins and the scroll bar scroll.
      if (!sl || !sl->visible_r() || Fl::event_y() < sl->y() || Fl::event_y() >= sl->y() + sl->h() ||
          Fl::event_x() < S(14) || Fl::event_x() >= w() - S(22))
        continue;
      sl->value(sl->clamp(sl->value() - Fl::event_dy() * 5));
      sl->do_callback();
      redraw();
      return 1;
    }
  }
  if (event == FL_MOUSEWHEEL && body_ && Fl::event_inside(body_)) {
    // Route wheel gestures explicitly through this override-redirect window;
    // keep application-tray and slider wheel controls local to their widgets.
    auto *target = Fl::belowmouse();
    // belowmouse can still name a previous control after an async rebuild.
    // Forward wheel events only when the pointer is actually inside it.
    if (target && Fl::event_inside(target) &&
        (dynamic_cast<Fl_Slider *>(target) || dynamic_cast<TrayIndicator *>(target) ||
         dynamic_cast<Fl_Choice *>(target)))
      return PanelPopup::handle(event);
    int limit = std::max(0, content_height_ - body_->h());
    body_->scroll_to(0, std::max(0, std::min(limit, body_->yposition() + Fl::event_dy() * S(36))));
    redraw();
    return 1;
  }
  return PanelPopup::handle(event);
}
