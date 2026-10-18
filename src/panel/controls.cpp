// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Hugo Moniz do Rego and SDE contributors.
// Independently implemented C++ adaptation of Eduka-Panel service behavior.
#include "controls.h"

#include <FL/Fl.H>

#include <algorithm>
#include <cctype>
#include <sstream>

#include "i18n.h"
#include "process.h"
#include "quick.h"
#include "util.h"
using namespace sde;
namespace {
std::vector<std::string> lines(const std::string& s) {
  std::vector<std::string> r;
  std::istringstream in(s);
  std::string l;
  while (std::getline(in, l)) r.push_back(l);
  return r;
}
std::vector<std::string> fields(const std::string& s) {
  std::vector<std::string> r(1);
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size())
      r.back() += s[++i];
    else if (s[i] == ':')
      r.emplace_back();
    else
      r.back() += s[i];
  }
  return r;
}
int percent(const std::string& s) {
  auto p = s.find('%');
  if (p == std::string::npos) return 0;
  auto a = p;
  while (a && isdigit(static_cast<unsigned char>(s[a - 1]))) --a;
  return std::max(0, std::min(150, atoi(s.substr(a, p - a).c_str())));
}
std::string prop(const dbus::Value& v, const char* key) {
  auto* p = v.get(key);
  return p ? p->str() : "";
}
bool flag(const dbus::Value& v, const char* key) {
  auto* p = v.get(key);
  return p && p->boolean();
}
bool success(DBusMessage* r) {
  return r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_METHOD_RETURN;
}
std::string error_text(DBusMessage* r) {
  auto args = dbus::read_args(r);
  return !args.empty() && args[0].is_string()
             ? args[0].str()
             : _("Request failed; check the service and permissions");
}
}  // namespace
std::vector<AudioStream> parse_audio_streams(const std::string& text) {
  std::vector<AudioStream> result;
  AudioStream* current = nullptr;
  for (const auto& line : lines(text)) {
    auto l = trim(line);
    if (starts_with(l, "Sink Input #")) {
      std::string id = l.substr(12);
      if (id.empty() ||
          !std::all_of(id.begin(), id.end(), [](unsigned char c) { return isdigit(c); }))
        continue;
      result.emplace_back();
      current = &result.back();
      current->id = unsigned(strtoul(id.c_str(), nullptr, 10));
    } else if (current) {
      if (starts_with(l, "Volume:"))
        current->volume = percent(l);
      else if (starts_with(l, "Mute:"))
        current->muted = l.find("yes") != std::string::npos;
      else if (starts_with(l, "Corked:"))
        current->corked = l.find("yes") != std::string::npos;
      else {
        auto eq = l.find(" = \"");
        if (eq == std::string::npos || l.back() != '"') continue;
        auto key = l.substr(0, eq), v = l.substr(eq + 4, l.size() - eq - 5);
        if (key == "application.name")
          current->app = v;
        else if (key == "media.name")
          current->title = v;
        else if (key == "application.icon_name")
          current->icon = v;
        else if (key == "application.process.id")
          current->pid = v;
        else if (key == "application.process.binary")
          current->binary = v;
      }
    }
  }
  for (auto& s : result) {
    if (s.app.empty()) s.app = s.binary.empty() ? _("Application") : s.binary;
    if (s.title == s.app || lower(s.title) == "playback" || lower(s.title) == "audio stream")
      s.title.clear();
  }
  return result;
}
std::string match_media_player(const AudioStream& s, const std::vector<MediaPlayer>& players) {
  for (const auto& p : players)
    if (!s.pid.empty() && s.pid == p.pid) return p.service;
  for (const auto& p : players) {
    auto name = lower(p.name), app = lower(s.app), bin = lower(s.binary);
    auto prefix = std::string("org.mpris.MediaPlayer2.");
    auto short_name = starts_with(p.service, prefix) ? p.service.substr(prefix.size()) : p.service;
    short_name = lower(short_name.substr(0, short_name.find('.')));
    if ((!name.empty() && !app.empty() &&
         (app == name || app.find(name) != std::string::npos ||
          name.find(app) != std::string::npos)) ||
        (!bin.empty() && bin == short_name))
      return p.service;
  }
  return "";
}
std::vector<NetworkDevice> parse_network_devices(const std::string& text) {
  std::vector<NetworkDevice> r;
  for (const auto& l : lines(text)) {
    auto f = fields(l);
    if (f.size() < 4 || (f[1] != "ethernet" && f[1] != "wifi" && f[1] != "gsm")) continue;
    r.push_back({f[0], f[1], f[2], f[3], "", "", ""});
  }
  return r;
}
PanelControls& PanelControls::get() {
  static PanelControls c;
  return c;
}
void PanelControls::changed() {
  if (changed_) changed_();
}
const BluetoothAdapter* PanelControls::adapter() const {
  for (const auto& a : adapters)
    if (a.path == adapter_path) return &a;
  return nullptr;
}
std::vector<MediaPlayer> PanelControls::players() const {
  std::vector<MediaPlayer> r;
  for (const auto& p : players_) r.push_back(p.second);
  std::stable_sort(r.begin(), r.end(), [](const MediaPlayer& a, const MediaPlayer& b) {
    return a.status == "Playing" && b.status != "Playing";
  });
  return r;
}
void PanelControls::start(std::function<void()> fn) {
  changed_ = std::move(fn);
  session_ = dbus::Bus::session();
  system_ = dbus::Bus::system();
  if (session_) {
    session_->add_signal_handler(
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
        [](DBusMessage* m) {
          return bool(dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged"));
        },
        [this](DBusMessage* m) {
          auto a = dbus::read_args(m);
          if (a.size() < 3 || !starts_with(a[0].str(), "org.mpris.MediaPlayer2.")) return;
          ++player_generation_[a[0].str()];
          players_.erase(a[0].str());
          if (!a[2].str().empty()) read_player(a[0].str(), a[2].str());
          changed();
        });
    session_->add_signal_handler(
        "type='signal',interface='org.freedesktop.DBus.Properties',path='/org/mpris/MediaPlayer2'",
        [](DBusMessage* m) {
          const char* p = dbus_message_get_path(m);
          return bool(dbus_message_is_signal(m, "org.freedesktop.DBus.Properties",
                                             "PropertiesChanged")) &&
                 p && std::string(p) == "/org/mpris/MediaPlayer2";
        },
        [this](DBusMessage* m) {
          const char* sender = dbus_message_get_sender(m);
          for (const auto& p : players_)
            if (sender && p.second.owner == sender) {
              read_player(p.first, p.second.owner);
              break;
            }
        });
  }
  if (system_) {
    system_->add_signal_handler(
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties'",
        [](DBusMessage* m) {
          const char* p = dbus_message_get_path(m);
          return bool(dbus_message_is_signal(m, "org.freedesktop.DBus.Properties",
                                             "PropertiesChanged")) &&
                 p && starts_with(p, "/org/bluez/");
        },
        [this](DBusMessage*) { refresh_bluetooth(); });
    for (const char* member : {"InterfacesAdded", "InterfacesRemoved"})
      system_->add_signal_handler(
          std::string("type='signal',sender='org.bluez',interface='org.freedesktop.DBus."
                      "ObjectManager',member='") +
              member + "'",
          [member](DBusMessage* m) {
            return bool(dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", member));
          },
          [this](DBusMessage*) { refresh_bluetooth(); });
    system_->add_signal_handler(
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
        [](DBusMessage* m) {
          auto a = dbus::read_args(m);
          return bool(dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) &&
                 !a.empty() && a[0].str() == "org.bluez";
        },
        [this](DBusMessage*) {
          adapters.clear();
          bluetooth.clear();
          refresh_bluetooth();
          changed();
        });
  }
  refresh();
  Fl::add_timeout(3, tick, this);
}
void PanelControls::tick(void* d) {
  auto* c = static_cast<PanelControls*>(d);
  if (c->visible_)
    c->refresh();
  else {
    c->refresh_media();
    c->refresh_bluetooth();
  }
  Fl::repeat_timeout(c->visible_ ? 3 : 15, tick, d);
}
void PanelControls::set_visible(bool on) {
  visible_ = on;
  if (on)
    refresh();
  else
    stop_scan();
}
void PanelControls::refresh() {
  refresh_media();
  refresh_audio_apps();
  refresh_bluetooth();
  refresh_devices();
}
void PanelControls::refresh_media() {
  if (!session_) return;
  session_->call_async(dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                    "org.freedesktop.DBus", "ListNames"),
                       [this](DBusMessage* r) {
                         auto a = dbus::read_args(r);
                         if (a.empty() || a[0].type != 'a') return;
                         int count = 0;
                         for (const auto& n : a[0].items) {
                           auto service = n.str();
                           if (!starts_with(service, "org.mpris.MediaPlayer2.") || ++count > 8)
                             continue;
                           auto* m = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "GetNameOwner");
                           dbus::Writer(m).str(service);
                           auto generation = player_generation_[service];
                           session_->call_async(m, [this, service, generation](DBusMessage* reply) {
                             if (player_generation_[service] != generation) return;
                             auto v = dbus::read_args(reply);
                             if (success(reply) && !v.empty()) read_player(service, v[0].str());
                           });
                         }
                       });
}
void PanelControls::read_player(const std::string& service, const std::string& owner) {
  if (!session_) return;
  auto generation = player_generation_[service];
  // Address the unique owner, so a replacement process never receives stale controls.
  dbus::get_all_properties(
      session_, owner, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
      [this, service, owner, generation](const dbus::Value& v) {
        if (player_generation_[service] != generation) return;
        if (v.type != 'a') {
          auto p = players_.find(service);
          if (p != players_.end() && p->second.owner == owner) {
            players_.erase(p);
            changed();
          }
          return;
        }
        auto& player = players_[service];
        player.service = service;
        player.owner = owner;
        player.status = prop(v, "PlaybackStatus");
        player.control = flag(v, "CanControl");
        player.play = flag(v, "CanPlay");
        player.pause = flag(v, "CanPause");
        player.next = flag(v, "CanGoNext");
        player.previous = flag(v, "CanGoPrevious");
        player.title.clear();
        player.artist.clear();
        if (auto* m = v.get("Metadata")) {
          player.title = prop(*m, "xesam:title");
          if (auto* a = m->get("xesam:artist"))
            for (const auto& artist : a->items) {
              if (!player.artist.empty()) player.artist += ", ";
              player.artist += artist.str();
            }
        }
        if (player.name.empty())
          player.name = service.substr(std::string("org.mpris.MediaPlayer2.").size());
        changed();
        dbus::get_all_properties(session_, owner, "/org/mpris/MediaPlayer2",
                                 "org.mpris.MediaPlayer2",
                                 [this, service, owner](const dbus::Value& root) {
                                   auto p = players_.find(service);
                                   if (p == players_.end() || p->second.owner != owner) return;
                                   if (root.type == 'a') {
                                     auto name = prop(root, "Identity");
                                     if (!name.empty()) p->second.name = name;
                                     p->second.raise = flag(root, "CanRaise");
                                     p->second.binary = prop(root, "DesktopEntry");
                                     changed();
                                   }
                                 });
        auto* m = dbus::method("org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "GetConnectionUnixProcessID");
        dbus::Writer(m).str(owner);
        session_->call_async(m, [this, service, owner](DBusMessage* r) {
          auto a = dbus::read_args(r);
          auto p = players_.find(service);
          if (p != players_.end() && p->second.owner == owner && !a.empty())
            p->second.pid = std::to_string(a[0].num());
        });
      });
}
void PanelControls::media_command(const std::string& service, const std::string& command) {
  auto p = players_.find(service);
  if (p == players_.end() || !session_) return;
  const auto& v = p->second;
  bool allowed = command == "Raise" ? v.raise
                                    : v.control && (command == "Next"       ? v.next
                                                    : command == "Previous" ? v.previous
                                                    : command == "PlayPause"
                                                        ? (v.status == "Playing" ? v.pause : v.play)
                                                        : false);
  if (!allowed) return;
  auto* m = dbus::method(
      v.owner, "/org/mpris/MediaPlayer2",
      command == "Raise" ? "org.mpris.MediaPlayer2" : "org.mpris.MediaPlayer2.Player", command);
  session_->call_async(m, [this, service](DBusMessage* r) {
    if (!success(r)) message = error_text(r);
    refresh_media();
    changed();
  });
}
void PanelControls::refresh_audio_apps() {
  if (audio_pending_) return;
  audio_pending_ = true;
  bool started =
      run_async({"pactl", "list", "sink-inputs"}, [this](const std::string& out, int code) {
        audio_pending_ = false;
        streams = code == 0 ? parse_audio_streams(out) : std::vector<AudioStream>{};
        refresh_microphone();
        changed();
      });
  if (!started) {
    audio_pending_ = false;
    streams.clear();
    mic_available = false;
    changed();
  }
}
void PanelControls::set_stream_volume(unsigned id, int value) {
  if (std::none_of(streams.begin(), streams.end(),
                   [&](const AudioStream& s) { return s.id == id; }))
    return;
  run_async({"pactl", "set-sink-input-volume", std::to_string(id),
             std::to_string(std::max(0, std::min(100, value))) + "%"},
            [this](const std::string&, int code) {
              if (code) message = _("Application volume could not be changed");
              refresh_audio_apps();
            });
}
void PanelControls::mute_stream(unsigned id, bool muted) {
  if (std::none_of(streams.begin(), streams.end(),
                   [&](const AudioStream& s) { return s.id == id; }))
    return;
  run_async({"pactl", "set-sink-input-mute", std::to_string(id), muted ? "1" : "0"},
            [this](const std::string&, int) { refresh_audio_apps(); });
}
void PanelControls::refresh_microphone() {
  run_async({"pactl", "get-source-volume", "@DEFAULT_SOURCE@"}, [this](const std::string& v,
                                                                       int code) {
    mic_available = code == 0;
    mic_volume = percent(v);
    if (code) {
      changed();
      return;
    }
    run_async({"pactl", "get-source-mute", "@DEFAULT_SOURCE@"}, [this](const std::string& m, int) {
      mic_muted = m.find("yes") != std::string::npos;
      changed();
    });
    run_async({"pactl", "get-default-source"},
              [this](const std::string& s, int) { default_source = trim(s); });
    run_async({"pactl", "list", "sources"}, [this](const std::string& s, int) {
      sources.clear();
      std::string name;
      for (const auto& line : lines(s)) {
        auto l = trim(line);
        if (starts_with(l, "Name:"))
          name = trim(l.substr(5));
        else if (starts_with(l, "Description:") && !name.empty()) {
          sources.emplace_back(name, trim(l.substr(12)));
          name.clear();
        }
      }
      changed();
    });
  });
}
void PanelControls::microphone_volume(int value) {
  run_async({"pactl", "set-source-volume", "@DEFAULT_SOURCE@",
             std::to_string(std::max(0, std::min(100, value))) + "%"},
            [this](const std::string&, int) { refresh_microphone(); });
}
void PanelControls::microphone_mute() {
  run_async({"pactl", "set-source-mute", "@DEFAULT_SOURCE@", "toggle"},
            [this](const std::string&, int) { refresh_microphone(); });
}
void PanelControls::microphone_source(const std::string& name) {
  if (std::none_of(sources.begin(), sources.end(), [&](const auto& s) { return s.first == name; }))
    return;
  run_async({"pactl", "set-default-source", name},
            [this](const std::string&, int) { refresh_microphone(); });
}
void PanelControls::refresh_bluetooth() {
  if (!system_) return;
  if (bt_pending_) {
    bt_again_ = true;
    return;
  }
  bt_pending_ = true;
  system_->call_async(
      dbus::method("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"),
      [this](DBusMessage* r) {
        bt_pending_ = false;
        bluetooth_available = success(r);
        auto a = dbus::read_args(r);
        adapters.clear();
        bluetooth.clear();
        if (success(r) && !a.empty())
          for (const auto& entry : a[0].items) {
            if (entry.items.size() != 2) continue;
            const auto& interfaces = entry.items[1];
            std::string path = entry.items[0].str();
            if (auto* p = interfaces.get("org.bluez.Adapter1"))
              adapters.push_back(
                  {path, prop(*p, "Alias"), flag(*p, "Powered"), flag(*p, "Discovering")});
            if (auto* p = interfaces.get("org.bluez.Device1"))
              bluetooth.push_back({path, prop(*p, "Adapter"), prop(*p, "Alias"),
                                   prop(*p, "Address"), flag(*p, "Paired"), flag(*p, "Connected"),
                                   flag(*p, "Trusted")});
          }
        if (!adapter() && !adapters.empty()) adapter_path = adapters.front().path;
        std::stable_sort(bluetooth.begin(), bluetooth.end(), [](const auto& a, const auto& b) {
          return a.connected != b.connected ? a.connected
                 : a.paired != b.paired     ? a.paired
                                            : a.name < b.name;
        });
        changed();
        if (bt_again_) {
          bt_again_ = false;
          refresh_bluetooth();
        }
      },
      2500);
}
void PanelControls::select_adapter(const std::string& path) {
  stop_scan();
  adapter_path = path;
  changed();
}
void PanelControls::bluez_call(const std::string& path, const std::string& iface,
                               const std::string& method, std::function<void(bool)> done) {
  if (!system_ || bluetooth_busy) return;
  bluetooth_busy = true;
  changed();
  system_->call_async(
      dbus::method("org.bluez", path, iface, method),
      [this, done](DBusMessage* r) {
        bluetooth_busy = false;
        bool ok = success(r);
        message = ok ? _("Bluetooth updated") : error_text(r);
        if (done) done(ok);
        refresh_bluetooth();
        changed();
      },
      20000);
}
void PanelControls::bluetooth_power(bool on) {
  const auto* a = adapter();
  if (!a || !system_ || bluetooth_busy) return;
  auto* m = dbus::method("org.bluez", a->path, "org.freedesktop.DBus.Properties", "Set");
  dbus::Writer(m).str("org.bluez.Adapter1").str("Powered").variant_bool(on);
  bluetooth_busy = true;
  system_->call_async(m, [this](DBusMessage* r) {
    bluetooth_busy = false;
    message = success(r) ? _("Bluetooth power updated") : error_text(r);
    refresh_bluetooth();
    changed();
  });
}
void PanelControls::bluetooth_scan() {
  auto* a = adapter();
  if (!a || !a->powered) return;
  std::string path = a->path;
  bluez_call(path, "org.bluez.Adapter1", "StartDiscovery", [this, path](bool ok) {
    if (ok) {
      discovery_owned_ = path;
      Fl::remove_timeout(scan_timeout, this);
      if (!visible_ || path != adapter_path)
        stop_scan();
      else
        Fl::add_timeout(10, scan_timeout, this);
    }
  });
}
void PanelControls::scan_timeout(void* d) { static_cast<PanelControls*>(d)->stop_scan(); }
void PanelControls::stop_scan() {
  Fl::remove_timeout(scan_timeout, this);
  if (discovery_owned_.empty() || !system_) return;
  auto path = discovery_owned_;
  discovery_owned_.clear();
  system_->call_async(dbus::method("org.bluez", path, "org.bluez.Adapter1", "StopDiscovery"),
                      [this](DBusMessage*) { refresh_bluetooth(); });
}
void PanelControls::bluetooth_device(const std::string& path) {
  auto p = std::find_if(bluetooth.begin(), bluetooth.end(),
                        [&](const auto& d) { return d.path == path; });
  if (p == bluetooth.end()) return;
  if (p->connected)
    bluez_call(path, "org.bluez.Device1", "Disconnect");
  else if (p->paired)
    bluez_call(path, "org.bluez.Device1", "Connect");
  else
    bluez_call(path, "org.bluez.Device1", "Pair", [this, path](bool ok) {
      if (ok)
        bluez_call(path, "org.bluez.Device1", "Connect");
      else
        message =
            _("Pairing needs an agent or confirmation. Open Bluetooth Settings for PIN/code "
              "devices.");
    });
}
void PanelControls::bluetooth_forget(const std::string& path) {
  auto p = std::find_if(bluetooth.begin(), bluetooth.end(),
                        [&](const auto& d) { return d.path == path; });
  if (p == bluetooth.end() || !system_ || bluetooth_busy) return;
  auto* m = dbus::method("org.bluez", p->adapter, "org.bluez.Adapter1", "RemoveDevice");
  dbus::Writer(m).path(path);
  system_->call_async(m, [this](DBusMessage* r) {
    if (!success(r)) message = error_text(r);
    refresh_bluetooth();
  });
}
void PanelControls::refresh_devices() {
  if (devices_pending_) return;
  devices_pending_ = true;
  if (!run_async({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"},
                 [this](const std::string& out, int code) {
                   devices_pending_ = false;
                   auto previous = std::move(devices);
                   devices = code == 0 ? parse_network_devices(out) : std::vector<NetworkDevice>{};
                   for (auto& d : devices)
                     for (const auto& old : previous)
                       if (d.device == old.device && d.state == old.state &&
                           d.connection == old.connection) {
                         d.ip = old.ip;
                         d.gateway = old.gateway;
                         d.dns = old.dns;
                         break;
                       }
                   changed();
                   if (visible_)
                     for (const auto& d : devices)
                       if (d.state == "connected") {
                         auto dev = d.device;
                         run_async({"nmcli", "-g", "IP4.ADDRESS,IP4.GATEWAY,IP4.DNS", "device",
                                    "show", dev},
                                   [this, dev](const std::string& s, int status) {
                                     if (status) return;
                                     auto v = lines(s);
                                     for (auto& d : devices)
                                       if (d.device == dev) {
                                         d.ip = v.empty() ? "" : v[0];
                                         d.gateway = v.size() > 1 ? v[1] : "";
                                         d.dns = v.size() > 2 ? v[2] : "";
                                       }
                                     changed();
                                   });
                       }
                 }))
    devices_pending_ = false;
  run_async({"setxkbmap", "-query"}, [this](const std::string& s, int code) {
    if (code) return;
    for (const auto& l : lines(s))
      if (starts_with(trim(l), "layout:")) {
        auto v = trim(l.substr(l.find(':') + 1));
        keyboard = v.substr(0, v.find(','));
        for (char& c : keyboard) c = char(toupper(static_cast<unsigned char>(c)));
        changed();
      }
  });
}
void PanelControls::network_device(const std::string& name, bool connect) {
  if (std::none_of(devices.begin(), devices.end(), [&](const auto& d) { return d.device == name; }))
    return;
  run_async({"nmcli", "device", connect ? "connect" : "disconnect", name},
            [this](const std::string& out, int code) {
              message = code ? (trim(out).empty() ? _("Network operation failed") : trim(out))
                             : _("Network updated");
              SystemStatus::get().refresh_network();
              refresh_devices();
              changed();
            });
}
void PanelControls::wireless_off(bool off) {
  if (off) {
    restore_wifi_ = SystemStatus::get().network.wifi_enabled;
    restore_bluetooth_.clear();
    for (const auto& a : adapters)
      if (a.powered) restore_bluetooth_.push_back(a.path);
    run_async({"nmcli", "radio", "wwan"}, [this](const std::string& s, int) {
      restore_wwan_ = trim(s) == "enabled";
      run_async({"nmcli", "radio", "all", "off"}, [this](const std::string&, int code) {
        if (code) message = _("Wireless radios could not be disabled");
        SystemStatus::get().refresh_network();
        changed();
      });
    });
    for (const auto& a : adapters)
      if (a.powered && system_) {
        auto* m = dbus::method("org.bluez", a.path, "org.freedesktop.DBus.Properties", "Set");
        dbus::Writer(m).str("org.bluez.Adapter1").str("Powered").variant_bool(false);
        system_->call_async(m, [this](DBusMessage*) { refresh_bluetooth(); });
      }
  } else {
    if (restore_wifi_) SystemStatus::get().set_wifi_enabled(true);
    if (restore_wwan_) run_async({"nmcli", "radio", "wwan", "on"}, nullptr);
    for (const auto& path : restore_bluetooth_)
      if (system_) {
        auto* m = dbus::method("org.bluez", path, "org.freedesktop.DBus.Properties", "Set");
        dbus::Writer(m).str("org.bluez.Adapter1").str("Powered").variant_bool(true);
        system_->call_async(m, [this](DBusMessage*) { refresh_bluetooth(); });
      }
  }
  airplane = off;
  changed();
}
