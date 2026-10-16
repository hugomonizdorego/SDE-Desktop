// SPDX-License-Identifier: GPL-3.0-or-later
#include "quick.h"

#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "build_config.h"
#include "config.h"
#include "dbus.h"
#include "glyphs.h"
#include "i18n.h"
#include "panel.h"
#include "process.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"
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
}

// pactl reports every volume, mute and device change. The sound server may
// start after the panel or restart, so the stream is retried.
void SystemStatus::start_audio_stream() {
  audio_stream_ = stream_lines(
      {"pactl", "subscribe"},
      [this](const std::string &line) {
        if (line.find("sink") == std::string::npos && line.find("server") == std::string::npos) return;
        if (audio_pending_) return;
        audio_pending_ = true;
        Fl::add_timeout(0.1, [](void *) {
          SystemStatus::get().audio_pending_ = false;
          SystemStatus::get().refresh_audio();
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
      double hours = b.charging ? (atof(full.c_str()) - atof(now.c_str())) / r : atof(now.c_str()) / r;
      if (hours > 0 && hours < 48) b.minutes = int(hours * 60);
    }
    break;
  }
  bool diff = b.present != battery.present || b.percent != battery.percent ||
              b.charging != battery.charging || b.full != battery.full || b.minutes != battery.minutes;
  battery = b;
  if (diff) changed();
}

void SystemStatus::refresh_brightness() {
  BrightnessState s;
  for (auto &dev : list_dir("/sys/class/backlight")) {
    std::string base = "/sys/class/backlight/" + dev, v, m;
    if (!read_file(base + "/brightness", v) || !read_file(base + "/max_brightness", m)) continue;
    s.available = atoi(m.c_str()) > 0;
    s.device = dev;
    s.value = atoi(v.c_str());
    s.max = atoi(m.c_str());
    if (s.available) break;
  }
  bool diff = s.available != brightness.available || s.value != brightness.value;
  brightness = s;
  if (diff) changed();
}

void SystemStatus::refresh_network(bool rescan) {
  bool started = run_async({"nmcli", "-t", "-f", "TYPE,STATE,CONNECTION", "device"},
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
    run_async({"nmcli", "radio", "wifi"}, [this, n, rescan](const std::string &out2, int) mutable {
      n.wifi_enabled = trim(out2) == "enabled";
      network.available = n.available;
      network.has_wifi = n.has_wifi;
      network.wifi_enabled = n.wifi_enabled;
      network.kind = n.kind;
      network.connection = n.connection;
      if (!n.has_wifi || !n.wifi_enabled) {
        wifi.clear();
        changed();
        return;
      }
      run_async({"nmcli", "-t", "-f", "IN-USE,SIGNAL,SECURITY,SSID", "device", "wifi", "list", "--rescan",
                 rescan ? "yes" : "no"},
                [this](const std::string &out3, int) {
                  parse_wifi_list(out3);
                  changed();
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
    auto same = std::find_if(list.begin(), list.end(), [&](const WifiNetwork &o) { return o.ssid == w.ssid; });
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
  bool started = run_async({"pactl", "get-sink-volume", "@DEFAULT_SINK@"}, [this](const std::string &vol, int st) {
    if (st != 0) {
      if (audio.available) {
        audio = AudioState();
        changed();
      }
      return;
    }
    int v = first_percent(vol);
    run_async({"pactl", "get-sink-mute", "@DEFAULT_SINK@"}, [this, v](const std::string &mute, int) {
      bool muted = mute.find("yes") != std::string::npos;
      run_async({"pactl", "get-default-sink"}, [this, v, muted](const std::string &def, int) {
        run_async({"pactl", "list", "sinks"}, [this, v, muted, def](const std::string &list, int) {
          AudioState a;
          a.available = true;
          a.volume = std::max(0, v);
          a.muted = muted;
          a.default_sink = trim(def);
          std::string name;
          for (auto &line : lines_of(list)) {
            std::string l = trim(line);
            if (starts_with(l, "Name:")) name = trim(l.substr(5));
            else if (starts_with(l, "Description:") && !name.empty()) {
              a.sinks.push_back({name, trim(l.substr(12))});
              name.clear();
            }
          }
          bool diff = a.volume != audio.volume || a.muted != audio.muted ||
                      a.default_sink != audio.default_sink || a.sinks.size() != audio.sinks.size() ||
                      !audio.available;
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
  run_async({"pactl", "set-default-sink", name}, [](const std::string &, int) {
    SystemStatus::get().refresh_audio();
  });
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
  run_async({"nmcli", "device", "wifi", "connect", ssid}, [this, ssid](const std::string &out, int st) {
    if (st == 0) {
      message = format(_("Connected to %s"), ssid.c_str());
      refresh_network();
      return;
    }
    // Unknown secured network: ask for the password and try again.
    const char *pw = fl_password("%s", "", format(_("Password for the Wi-Fi network \"%s\":"), ssid.c_str()).c_str());
    if (!pw || !*pw) {
      message = trim(out).empty() ? _("Connection failed") : trim(out);
      changed();
      return;
    }
    std::string password = pw;
    run_async({"nmcli", "device", "wifi", "connect", ssid, "password", password},
              [this, ssid](const std::string &out2, int st2) {
                message = st2 == 0 ? format(_("Connected to %s"), ssid.c_str())
                                   : (trim(out2).empty() ? std::string(_("Connection failed")) : trim(out2));
                refresh_network();
              });
  });
}

void SystemStatus::set_brightness(int percent) {
  if (!brightness.available) return;
  percent = std::max(1, std::min(100, percent));
  int value = std::max(1, brightness.max * percent / 100);
  brightness.value = value;
  // systemd-logind lets the session's user change the backlight.
  dbus::Bus *bus = dbus::Bus::system();
  if (bus) {
    DBusMessage *m = dbus::method("org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                                  "org.freedesktop.login1.Session", "SetBrightness");
    dbus::Writer(m).str("backlight").str(brightness.device).u32(uint32_t(value));
    std::string dev = brightness.device;
    bus->call_async(m, [percent](DBusMessage *reply) {
      if (!reply) run_async({"brightnessctl", "set", std::to_string(percent) + "%"}, nullptr);
    });
  } else {
    run_async({"brightnessctl", "set", std::to_string(percent) + "%"}, nullptr);
  }
}

// =========================================================== QuickSettings

QuickSettings::QuickSettings() : PanelPopup("Quick Settings") { end(); }

std::string QuickSettings::structure() const {
  const SystemStatus &s = SystemStatus::get();
  std::string sig = std::to_string(s.network.available) + std::to_string(s.network.has_wifi) +
                    std::to_string(s.network.wifi_enabled) + "|" + s.network.connection;
  for (auto &w : s.wifi) sig += "|" + w.ssid + (w.active ? "*" : "") + (w.secure ? "s" : "");
  sig += "#" + std::to_string(s.audio.available) + s.audio.default_sink;
  for (auto &k : s.audio.sinks) sig += "|" + k.first;
  sig += "#" + std::to_string(s.brightness.available) + std::to_string(s.battery.present);
  sig += "#" + Panel::instance->config().get("notifications", "do_not_disturb", "false");
  return sig;
}

void QuickSettings::prepare() {
  SystemStatus::get().refresh_brightness();
  SystemStatus::get().message.clear();
  rebuild();
}

void QuickSettings::status_changed() {
  if (!shown()) return;
  std::string sig = structure();
  if (sig == structure_) {
    sync();
    return;
  }
  // Do not rebuild under the user's fingers.
  Fl_Widget *p = Fl::pushed();
  if (p && p->window() == this) {
    rebuild_pending_ = true;
    return;
  }
  Fl_Widget *focus = Fl::focus();
  bool had_focus = focus && focus->window() == this;
  rebuild();
  int hh = h();
  // Keep the popup attached to the panel when its height changed.
  Panel *panel = Panel::instance;
  if (!panel->theme().panel_top) position(x(), panel->y() - hh - S(8));
  if (had_focus && children()) child(0)->take_focus();
}

void QuickSettings::sync() {
  const SystemStatus &s = SystemStatus::get();
  if (volume_ && Fl::pushed() != volume_) volume_->value(s.audio.volume);
  if (brightness_ && Fl::pushed() != brightness_) brightness_->value(s.brightness.percent());
  redraw();
}

void QuickSettings::rebuild() {
  structure_ = structure();
  rebuild_pending_ = false;
  clear();
  keep_.clear();
  resizable(nullptr);
  texts_.clear();
  separators_.clear();
  volume_ = brightness_ = nullptr;
  sinks_ = nullptr;
  begin();
  SystemStatus &st = SystemStatus::get();
  const Theme &t = Panel::instance->theme();
  int W = S(360), pad = S(16), y = pad, inner = W - 2 * pad;
  const Config &cfg = Panel::instance->config();

  // ---- toggle tiles
  struct TileSpec {
    std::string title;
    std::function<std::string()> sub;
    std::function<bool()> on;
    std::function<void(Painter &, double, double, double, const Color &)> icon;
    std::function<void()> click;
  };
  std::vector<TileSpec> tiles;
  if (st.network.available && st.network.has_wifi) {
    tiles.push_back({_("Wi-Fi"),
                     [] {
                       const auto &n = SystemStatus::get().network;
                       if (!n.wifi_enabled) return std::string(_("Off"));
                       return n.kind == "wifi" ? n.connection : std::string(_("Not connected"));
                     },
                     [] { return SystemStatus::get().network.wifi_enabled; },
                     [](Painter &p, double x, double yy, double s, const Color &c) {
                       const auto &n = SystemStatus::get().network;
                       glyph::wifi(p, x, yy, s, n.wifi_enabled ? 4 : 0, c, !n.wifi_enabled);
                     },
                     [] { SystemStatus::get().set_wifi_enabled(!SystemStatus::get().network.wifi_enabled); }});
  }
  bool dnd = cfg.get_bool("notifications", "do_not_disturb", false);
  tiles.push_back({_("Do Not Disturb"), [dnd] { return std::string(dnd ? _("On") : _("Off")); },
                   [dnd] { return dnd; },
                   [dnd](Painter &p, double x, double yy, double s, const Color &c) { glyph::bell(p, x, yy, s, dnd, c); },
                   [dnd] { Panel::instance->set_do_not_disturb(!dnd); }});
  int gap = S(10), tw = (inner - gap) / 2, th = S(60);
  for (size_t i = 0; i < tiles.size(); ++i) {
    TileSpec spec = tiles[i];
    int tx = pad + int(i % 2) * (tw + gap);
    int ty = y + int(i / 2) * (th + gap);
    auto *b = new DrawnButton(
        tx, ty, tw, th,
        [spec, tx, ty, tw, th, &t](Painter &p, bool hover, bool down) {
          bool on = spec.on();
          Color bg = on ? t.accent : (hover ? t.hover : t.surface_alt);
          if (down) bg = bg.mix(t.text, 0.12);
          p.fill_rounded(tx, ty, tw, th, Sd(12), bg);
          spec.icon(p, tx + Sd(12), ty + (th - Sd(22)) / 2.0, Sd(22), on ? t.on_accent : t.text);
        },
        [spec, tx, ty, tw, th, &t](bool) {
          bool on = spec.on();
          text_font(t.font_size, true);
          draw_text(spec.title, tx + S(44), ty + S(10), tw - S(52), S(20), on ? t.on_accent : t.text);
          text_font(t.font_size - 1);
          draw_text(spec.sub(), tx + S(44), ty + S(30), tw - S(52), S(18), on ? t.on_accent : t.text_dim);
        },
        Sd(12));
    b->callback([](Fl_Widget *, void *d) { (*static_cast<std::function<void()> *>(d))(); },
                keep(new std::function<void()>(spec.click)));
  }
  y += int((tiles.size() + 1) / 2) * (th + gap) + S(4);

  // ---- network
  separators_.push_back(y);
  y += S(12);
  texts_.push_back({pad, y, inner, S(22), [] { return std::string(_("Network")); }, 1});
  y += S(24);
  texts_.push_back({pad, y, inner, S(20),
                    [] {
                      const SystemStatus &s = SystemStatus::get();
                      if (!s.message.empty()) return s.message;
                      if (!s.network.available) return std::string(_("Network service is not running"));
                      if (s.network.kind == "ethernet")
                        return format(_("Wired connection: %s"), s.network.connection.c_str());
                      if (s.network.kind == "wifi") return format(_("Connected to %s"), s.network.connection.c_str());
                      return std::string(_("Not connected"));
                    },
                    2});
  y += S(26);
  if (st.network.available && st.network.wifi_enabled) {
    int shown_rows = 0;
    for (auto &net : st.wifi) {
      if (shown_rows++ >= 6) break;
      WifiNetwork w = net;
      int ry = y, rh = S(36);
      auto *row = new DrawnButton(
          pad, ry, inner, rh,
          [w, ry, rh, pad, inner, &t](Painter &p, bool hover, bool) {
            if (hover || w.active) p.fill_rounded(pad, ry, inner, rh, Sd(8), w.active ? t.accent.alpha(0.14) : t.hover);
            int bars = w.signal >= 75 ? 4 : w.signal >= 50 ? 3 : w.signal >= 25 ? 2 : 1;
            glyph::wifi(p, pad + Sd(8), ry + (rh - Sd(18)) / 2.0, Sd(18), bars, t.text);
            if (w.secure) glyph::lock(p, pad + inner - Sd(26), ry + (rh - Sd(14)) / 2.0, Sd(14), t.text_dim);
            if (w.active) glyph::check(p, pad + inner - Sd(46), ry + (rh - Sd(16)) / 2.0, Sd(16), t.accent_text);
          },
          [w, ry, rh, pad, inner, &t](bool) {
            text_font(t.font_size, w.active);
            draw_text(w.ssid, pad + S(36), ry, inner - S(92), rh, t.text);
          },
          Sd(8));
      row->callback(
          [](Fl_Widget *, void *d) {
            auto *net2 = static_cast<WifiNetwork *>(d);
            if (!net2->active) SystemStatus::get().connect_wifi(*net2);
          },
          keep(new WifiNetwork(w)));
      y += rh + S(2);
    }
    if (st.wifi.empty()) {
      texts_.push_back({pad, y, inner, S(20), [] { return std::string(_("Searching for networks...")); }, 2});
      y += S(24);
    }
  }
  if (st.network.available && !find_program("nm-connection-editor").empty()) {
    auto *more = new FlatButton(pad, y + S(4), inner, S(32), _("Network Connections..."));
    more->callback([](Fl_Widget *w, void *) {
      static_cast<QuickSettings *>(w->window())->close();
      spawn_detached({"nm-connection-editor"});
    });
    y += S(40);
  }
  y += S(8);

  // ---- sound
  separators_.push_back(y);
  y += S(12);
  texts_.push_back({pad, y, inner, S(22), [] { return std::string(_("Sound")); }, 1});
  y += S(26);
  if (st.audio.available) {
    int bh = S(36);
    auto *mute = new DrawnButton(
        pad, y, bh, bh,
        [pad, y, bh, &t](Painter &p, bool hover, bool) {
          const AudioState &a = SystemStatus::get().audio;
          p.fill_rounded(pad, y, bh, bh, bh / 2.0, a.muted ? t.accent : (hover ? t.hover : t.surface_alt));
          int level = a.volume == 0 ? 0 : a.volume < 34 ? 1 : a.volume < 67 ? 2 : 3;
          glyph::speaker(p, pad + Sd(8), y + Sd(8), bh - Sd(16), level, a.muted, a.muted ? t.on_accent : t.text);
        },
        nullptr, bh / 2.0);
    mute->tooltip(_("Mute"));
    mute->callback([](Fl_Widget *, void *) { SystemStatus::get().toggle_mute(); });
    volume_ = new Slider(pad + bh + S(10), y, inner - bh - S(64), bh);
    volume_->bounds(0, 100);
    volume_->value(st.audio.volume);
    volume_->callback([](Fl_Widget *w, void *) {
      SystemStatus::get().set_volume(int(static_cast<Fl_Slider *>(w)->value()));
      w->window()->redraw();
    });
    texts_.push_back({pad + inner - S(48), y, S(48), bh,
                      [] { return std::to_string(SystemStatus::get().audio.volume) + "%"; }, 3});
    y += bh + S(8);
    if (st.audio.sinks.size() > 1) {
      sinks_ = new Fl_Choice(pad, y, inner, S(32));
      sinks_->box(FL_FLAT_BOX);
      sinks_->down_box(FL_FLAT_BOX);
      sinks_->color(t.surface_alt.fl());
      sinks_->textsize(t.font_size);
      sinks_->textcolor(t.text.fl());
      for (size_t i = 0; i < st.audio.sinks.size(); ++i) {
        std::string label;
        for (char c : st.audio.sinks[i].second) {
          if (c == '/' || c == '&' || c == '_' || c == '\\') label += '\\';
          label += c;
        }
        sinks_->add(label.c_str());
        if (st.audio.sinks[i].first == st.audio.default_sink) sinks_->value(int(i));
      }
      sinks_->tooltip(_("Output device"));
      sinks_->callback([](Fl_Widget *w, void *) {
        int v = static_cast<Fl_Choice *>(w)->value();
        const auto &sinks = SystemStatus::get().audio.sinks;
        if (v >= 0 && v < int(sinks.size())) SystemStatus::get().set_default_sink(sinks[size_t(v)].first);
      });
      y += S(40);
    }
  } else {
    texts_.push_back({pad, y, inner, S(20), [] { return std::string(_("Sound service is not running")); }, 2});
    y += S(26);
  }
  y += S(4);

  // ---- brightness
  if (st.brightness.available) {
    separators_.push_back(y);
    y += S(12);
    texts_.push_back({pad, y, inner, S(22), [] { return std::string(_("Brightness")); }, 1});
    y += S(26);
    int bh = S(36);
    new DrawnButton(pad, y, bh, bh, [pad, y, bh, &t](Painter &p, bool, bool) {
      glyph::sun(p, pad + Sd(7), y + Sd(7), bh - Sd(14), t.text);
    });
    child(children() - 1)->clear_visible_focus();
    brightness_ = new Slider(pad + bh + S(10), y, inner - bh - S(64), bh);
    brightness_->bounds(1, 100);
    brightness_->value(st.brightness.percent());
    brightness_->callback([](Fl_Widget *w, void *) {
      SystemStatus::get().set_brightness(int(static_cast<Fl_Slider *>(w)->value()));
      w->window()->redraw();
    });
    texts_.push_back({pad + inner - S(48), y, S(48), bh,
                      [] { return std::to_string(SystemStatus::get().brightness.percent()) + "%"; }, 3});
    y += bh + S(12);
  }

  // ---- battery
  if (st.battery.present) {
    separators_.push_back(y);
    y += S(12);
    int bh = S(30);
    auto *icon = new DrawnButton(pad, y, bh, bh, [pad, y, bh, &t](Painter &p, bool, bool) {
      const BatteryState &b = SystemStatus::get().battery;
      Color level = b.percent <= 15 && !b.charging ? Color::hex(0xD64545) : t.text;
      if (b.charging) level = Color::hex(0x2E9E5B);
      glyph::battery(p, pad, y, bh, b.percent, b.charging, t.text, level);
    });
    icon->clear_visible_focus();
    texts_.push_back({pad + bh + S(10), y, inner - bh - S(10), bh,
                      [] {
                        const BatteryState &b = SystemStatus::get().battery;
                        std::string s = std::to_string(b.percent) + "%";
                        if (b.full) s += " · " + std::string(_("Fully charged"));
                        else if (b.charging) s += " · " + std::string(_("Charging"));
                        if (b.minutes > 0 && !b.full) {
                          std::string left = format(_("%d h %02d min"), b.minutes / 60, b.minutes % 60);
                          s += " · " + format(b.charging ? _("%s until full") : _("%s left"), left.c_str());
                        }
                        return s;
                      },
                      0});
    y += bh + S(12);
  }

  // ---- footer
  separators_.push_back(y);
  y += S(12);
  int fb = S(38);
  struct Foot {
    const char *tip;
    std::function<void(Painter &, double, double, double, const Color &)> icon;
    std::vector<std::string> cmd;
  };
  std::vector<Foot> foot = {
      {_("Settings"), [](Painter &p, double x, double yy, double s, const Color &c) { glyph::gear(p, x, yy, s, c); },
       {"sde-settings"}},
      {_("Lock Screen"), [](Painter &p, double x, double yy, double s, const Color &c) { glyph::lock(p, x, yy, s, c); },
       {"xdg-screensaver", "lock"}},
      {_("Power Off / Log Out"),
       [](Painter &p, double x, double yy, double s, const Color &c) { glyph::power(p, x, yy, s, c); },
       {"sde-logout"}},
  };
  for (size_t i = 0; i < foot.size(); ++i) {
    int bx = pad + inner - int(foot.size() - i) * (fb + S(8)) + S(8);
    Foot f = foot[i];
    auto *b = new DrawnButton(
        bx, y, fb, fb,
        [f, bx, y, fb, &t](Painter &p, bool hover, bool) {
          p.fill_rounded(bx, y, fb, fb, fb / 2.0, hover ? t.hover : t.surface_alt);
          f.icon(p, bx + Sd(10), y + Sd(10), fb - Sd(20), t.text);
        },
        nullptr, fb / 2.0);
    b->tooltip(f.tip);
    b->callback(
        [](Fl_Widget *w, void *d) {
          auto *cmd = static_cast<std::vector<std::string> *>(d);
          static_cast<QuickSettings *>(w->window())->close();
          spawn_detached(*cmd);
        },
        keep(new std::vector<std::string>(f.cmd)));
  }
  texts_.push_back({pad, y, inner - 3 * (fb + S(8)), fb, [] { return std::string("SDE " SDE_VERSION_LABEL); }, 2});
  y += fb + pad;
  end();
  size(W, y);
  redraw();
}

void QuickSettings::draw() {
  const Theme &t = Panel::instance->theme();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.surface);
    p.stroke_rounded(0.5, 0.5, w() - 1, h() - 1, 0, t.border);
    for (int y : separators_) p.line(S(16), y + 0.5, w() - S(16), y + 0.5, t.border);
  }
  draw_children();
  for (auto &tx : texts_) {
    text_font(tx.style == 1 ? t.font_size + 1 : t.font_size, tx.style == 1);
    draw_text(tx.text(), tx.x, tx.y, tx.w, tx.h, tx.style == 2 ? t.text_dim : t.text, tx.style == 3 ? 2 : 0);
  }
}

int QuickSettings::handle(int event) {
  int r = PanelPopup::handle(event);
  if (event == FL_RELEASE && rebuild_pending_) Fl::add_timeout(0, [](void *d) {
      static_cast<QuickSettings *>(d)->status_changed();
    }, this);
  return r;
}
