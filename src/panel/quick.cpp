// SPDX-License-Identifier: GPL-3.0-or-later
#include "quick.h"

#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Box.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cmath>
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

// =========================================================== Action Center
// Visual layout adapted from Eduka-Desktop: clock card, six tiles, grouped
// controls and a fixed session footer. All services remain native SDE helpers.
namespace {
using IconFn = std::function<void(Painter &, double, double, double, const Color &)>;
using StringFn = std::function<std::string()>;

class ActionLabel : public Fl_Widget {
 public:
  ActionLabel(int x, int y, int w, int h, StringFn text, int style = 0)
      : Fl_Widget(x, y, w, h), text_(std::move(text)), style_(style) {}
  void draw() override {
    const Theme &t = Panel::instance->theme();
    text_font(t.font_size + (style_ == 1 ? 1 : 0), style_ == 1);
    draw_text(text_(), x(), y(), w(), h(), style_ == 2 ? t.text_dim : t.text);
  }

 private:
  StringFn text_;
  int style_;
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
    }
    draw_children();
  }
};

class ActionButton : public DrawnButton {
 public:
  ActionButton(int x, int y, int w, int h, std::string title, IconFn icon, std::function<void()> action,
               std::function<bool()> on = nullptr, int color = -1, bool row = false, StringFn sub = nullptr)
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
    bool hot = Fl::belowmouse() == this, on = on_ && on_();
    static const unsigned colors[] = {0x2563EB, 0x7C3AED, 0x047857, 0xB45309, 0xBE185D, 0x0E7490};
    bool multi = Panel::instance->config().get_bool("action_center", "multicolor", false) && color_ >= 0;
    Color accent = multi ? Color::hex(colors[color_ % 6]) : t.accent;
    Color bg = on ? accent : t.surface_alt;
    if (multi && !on) bg = t.surface.mix(accent, t.dark ? 0.18 : 0.08);
    if (hot || value()) bg = bg.mix(on ? t.on_accent : t.text, value() ? 0.15 : 0.06);
    Color white = Color::hex(0xFFFFFF), black = Color::hex(0x101820);
    Color ink = on ? (white.contrast(accent) >= 4.5 ? white : black) : t.text;
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(12), bg);
      if (hot || on) p.stroke_rounded(x(), y(), w(), h(), S(12), on ? accent : t.border);
      double size = S(row_ ? 20 : 23);
      icon_(p, row_ ? x() + S(12) : x() + (w() - size) / 2.0, row_ ? y() + (h() - size) / 2.0 : y() + S(10),
            size, ink);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(12));
    }
    text_font(t.font_size - (row_ ? 0 : S(1)), true);
    if (!row_ && text_width(title_) > w() - S(8)) text_font(std::max(S(9), t.font_size - S(2)), true);
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

class ClockCard : public Fl_Widget {
 public:
  ClockCard(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { clear_visible_focus(); }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    const Config &cfg = Panel::instance->config();
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    bool analog = cfg.get("action_center", "clock_style", "digital") == "analog";
    int tx = x() + (analog ? S(108) : S(18)), tw = w() - (analog ? S(124) : S(36));
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(16), t.surface_alt.mix(t.accent, t.dark ? 0.08 : 0.04));
      p.stroke_rounded(x(), y(), w(), h(), S(16), t.border);
      if (analog) {
        double cx = x() + S(52), cy = y() + h() / 2.0, r = S(35);
        p.circle(cx, cy, r, t.surface);
        for (int i = 0; i < 12; ++i) {
          double a = i * M_PI / 6;
          p.line(cx + std::sin(a) * (r - S(4)), cy - std::cos(a) * (r - S(4)), cx + std::sin(a) * (r - S(7)),
                 cy - std::cos(a) * (r - S(7)), t.text_dim, S(1));
        }
        double hour = (lt.tm_hour % 12 + lt.tm_min / 60.0) * M_PI / 6;
        double minute = lt.tm_min * M_PI / 30;
        p.line(cx, cy, cx + std::sin(hour) * r * 0.48, cy - std::cos(hour) * r * 0.48, t.text, S(3));
        p.line(cx, cy, cx + std::sin(minute) * r * 0.76, cy - std::cos(minute) * r * 0.76, t.accent, S(2));
        p.circle(cx, cy, S(3), t.accent);
      }
    }
    char date[128], clock[64];
    strftime(clock, sizeof clock, cfg.get_bool("panel", "clock_24h", true) ? "%H:%M" : "%I:%M %p", &lt);
    strftime(date, sizeof date, "%A, %e %B", &lt);
    text_font(t.font_size + (analog ? 10 : 26), true);
    draw_text(clock, tx, y() + S(10), tw, S(44), t.text, analog ? 0 : 1);
    text_font(t.font_size - 1);
    draw_text(date, tx, y() + S(56), tw, S(22), t.text_dim, analog ? 0 : 1);
    if (cfg.get_bool("action_center", "show_greeting", true)) {
      const char *greeting = lt.tm_hour < 12   ? _("Good morning")
                             : lt.tm_hour < 18 ? _("Good afternoon")
                                               : _("Good evening");
      text_font(t.font_size, true);
      draw_text(greeting, tx, y() + S(80), tw, S(22), t.accent_text, analog ? 0 : 1);
    }
  }
};

IconFn gear_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) { glyph::gear(p, x, y, s, c); };
}
IconFn calendar_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.stroke_rounded(x + s * .1, y + s * .18, s * .8, s * .72, s * .12, c, s * .08);
    p.line(x + s * .12, y + s * .4, x + s * .88, y + s * .4, c, s * .08);
    for (int i = 0; i < 2; ++i)
      p.line(x + s * (.32 + i * .36), y + s * .06, x + s * (.32 + i * .36), y + s * .28, c, s * .08);
  };
}
IconFn display_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.stroke_rounded(x, y + s * .08, s, s * .64, s * .08, c, s * .08);
    p.line(x + s * .5, y + s * .72, x + s * .5, y + s * .95, c, s * .08);
    p.line(x + s * .25, y + s * .95, x + s * .75, y + s * .95, c, s * .08);
  };
}
}  // namespace

QuickSettings::QuickSettings() : PanelPopup("Action Center") { end(); }

std::string QuickSettings::structure() const {
  const auto &s = SystemStatus::get();
  const auto &cfg = Panel::instance->config();
  std::string sig = std::to_string(s.network.available) + std::to_string(s.network.has_wifi) +
                    std::to_string(s.network.wifi_enabled) + std::to_string(network_expanded_);
  for (auto &net : s.wifi) sig += "|" + net.ssid + (net.active ? "*" : "") + (net.secure ? "s" : "");
  sig += "#" + std::to_string(s.audio.available) + s.audio.default_sink;
  for (auto &sink : s.audio.sinks) sig += "|" + sink.first;
  sig += "#" + std::to_string(s.brightness.available) + std::to_string(s.battery.present);
  for (const char *key : {"show_clock", "show_greeting", "clock_style", "multicolor"})
    sig += "|" + cfg.get("action_center", key);
  for (const char *key : {"style", "position", "height", "width_percent", "clock_24h"})
    sig += "|" + cfg.get("panel", key);
  sig += cfg.get("appearance", "accent");
  sig += cfg.get("appearance", "theme") + cfg.get("appearance", "scale") +
         cfg.get("notifications", "do_not_disturb");
  return sig;
}

void QuickSettings::prepare() {
  SystemStatus::get().refresh_brightness();
  SystemStatus::get().message.clear();
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
  // Never delete a pressed button or slider while its callback is running.
  if (Fl::pushed() && Fl::pushed()->window() == self) {
    Fl::add_timeout(.1, rebuild_timeout, data);
    return;
  }
  int scroll = self->body_ ? self->body_->yposition() : 0;
  bool had_focus = Fl::focus() && Fl::focus()->window() == self;
  self->rebuild();
  if (self->body_)
    self->body_->scroll_to(0, std::min(scroll, std::max(0, self->content_height_ - self->body_->h())));
  auto *panel = Panel::instance;
  self->position(panel->x() + panel->w() - self->w(),
                 panel->theme().panel_top ? panel->y() + panel->h() + S(8) : panel->y() - self->h() - S(8));
  if (had_focus && self->body_->children()) self->body_->child(0)->take_focus();
}

void QuickSettings::sync() {
  const auto &s = SystemStatus::get();
  if (volume_ && Fl::pushed() != volume_) volume_->value(s.audio.volume);
  if (brightness_ && Fl::pushed() != brightness_) brightness_->value(s.brightness.percent());
  redraw();
}

void QuickSettings::rebuild() {
  Fl::remove_timeout(rebuild_timeout, this);
  rebuild_pending_ = false;
  structure_ = structure();
  clear();
  resizable(nullptr);
  volume_ = brightness_ = nullptr;
  sinks_ = nullptr;
  const auto &t = Panel::instance->theme();
  const auto &cfg = Panel::instance->config();
  auto &s = SystemStatus::get();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  int W = std::min(S(400), mw - S(16)), pad = S(16), inner = W - 2 * pad - S(12);
  int available = std::max(S(120), mh - Panel::instance->theme().panel_height - S(32));
  int footer = S(62), y = S(10);
  begin();
  body_ = new Fl_Scroll(S(2), S(2), W - S(4), available - footer - S(4));
  body_->type(Fl_Scroll::VERTICAL_ALWAYS);
  body_->resizable(nullptr);
  body_->scrollbar_size(S(10));
  body_->box(FL_FLAT_BOX);
  body_->color(t.surface.fl());
  auto label = [&](const std::string &text, int style = 0, int height = 22) {
    new ActionLabel(pad, y, inner, S(height), [text] { return text; }, style);
    y += S(height);
  };
  label(_("Action Center"), 1, 28);
  y += S(4);
  if (cfg.get_bool("action_center", "show_clock", true)) {
    new ClockCard(pad, y, inner, S(112));
    y += S(124);
  }
  struct Tile {
    std::string title;
    IconFn icon;
    std::function<void()> click;
    std::function<bool()> on;
    StringFn sub;
  };
  auto launch = [this](std::vector<std::string> cmd) {
    close();
    spawn_detached(cmd);
  };
  std::vector<Tile> tiles = {
      {_("Network"),
       [](Painter &p, double x, double y, double z, const Color &c) {
         const auto &n = SystemStatus::get().network;
         if (n.kind == "ethernet")
           glyph::ethernet(p, x, y, z, c);
         else
           glyph::wifi(p, x, y, z, n.kind == "wifi" ? 4 : 0, c, n.kind.empty());
       },
       [this] {
         network_expanded_ = !network_expanded_;
         status_changed();
       },
       [] { return !SystemStatus::get().network.kind.empty(); },
       [] {
         const auto &n = SystemStatus::get().network;
         return n.connection.empty() ? std::string(_("Offline")) : n.connection;
       }},
      {_("Display"), display_icon(), [launch] { launch({"sde-settings", "--page", "display"}); }, nullptr,
       [] { return std::string(_("Resolution")); }},
      {_("Do Not Disturb"),
       [](Painter &p, double x, double y, double z, const Color &c) {
         glyph::bell(p, x, y, z, Panel::instance->config().get_bool("notifications", "do_not_disturb", false),
                     c);
       },
       [] {
         auto *p = Panel::instance;
         p->set_do_not_disturb(!p->config().get_bool("notifications", "do_not_disturb", false));
       },
       [] { return Panel::instance->config().get_bool("notifications", "do_not_disturb", false); },
       [] {
         return std::string(Panel::instance->config().get_bool("notifications", "do_not_disturb", false)
                                ? _("On")
                                : _("Off"));
       }},
      {_("Dark Mode"),
       [](Painter &p, double x, double y, double z, const Color &c) { glyph::sun(p, x, y, z, c); },
       [] { Panel::instance->toggle_dark_mode(); }, [] { return Panel::instance->theme().dark; },
       [] { return std::string(Panel::instance->theme().dark ? _("On") : _("Off")); }},
      {_("Calendar"), calendar_icon(),
       [this] {
         close();
         Panel::instance->toggle_calendar();
       },
       nullptr, [] { return std::string(_("View month")); }},
      {_("Settings"), gear_icon(), [launch] { launch({"sde-settings"}); }, nullptr,
       [] { return std::string(_("Personalize")); }},
  };
  int gap = S(8), tw = (inner - 2 * gap) / 3, th = S(78);
  for (size_t i = 0; i < tiles.size(); ++i) {
    const auto &tile = tiles[i];
    new ActionButton(pad + int(i % 3) * (tw + gap), y + int(i / 3) * (th + gap), tw, th, tile.title,
                     tile.icon, tile.click, tile.on, int(i), false, tile.sub);
  }
  y += 2 * (th + gap) + S(4);

  if (network_expanded_) {
    int cy = y;
    auto *card = new ActionCard(pad, y, inner);
    y += S(10);
    new ActionLabel(
        pad + S(10), y, inner - S(20), S(22),
        [] {
          const auto &s = SystemStatus::get();
          if (!s.message.empty()) return s.message;
          return s.network.available
                     ? (s.network.connection.empty() ? std::string(_("Not connected")) : s.network.connection)
                     : std::string(_("Network service is not running"));
        },
        2);
    y += S(28);
    if (s.network.available && s.network.has_wifi) {
      new ActionButton(
          pad + S(8), y, inner - S(16), S(36),
          s.network.wifi_enabled ? _("Turn Wi-Fi Off") : _("Turn Wi-Fi On"),
          [](Painter &p, double x, double y, double z, const Color &c) { glyph::wifi(p, x, y, z, 4, c); },
          [] {
            auto &s = SystemStatus::get();
            s.set_wifi_enabled(!s.network.wifi_enabled);
          },
          nullptr, -1, true);
      y += S(42);
      if (s.network.wifi_enabled) {
        int count = 0;
        for (const auto &network : s.wifi) {
          if (++count > 8) break;
          new ActionButton(
              pad + S(8), y, inner - S(16), S(36), network.ssid,
              [network](Painter &p, double x, double y, double z, const Color &c) {
                glyph::wifi(p, x, y, z, std::max(1, network.signal / 25), c);
              },
              [network] {
                if (!network.active) SystemStatus::get().connect_wifi(network);
              },
              [network] { return network.active; }, -1, true);
          y += S(40);
        }
      }
    }
    if (!find_program("nm-connection-editor").empty()) {
      new ActionButton(
          pad + S(8), y, inner - S(16), S(36), _("Network Connections..."), gear_icon(),
          [launch] { launch({"nm-connection-editor"}); }, nullptr, -1, true);
      y += S(42);
    }
    card->size(inner, y - cy + S(6));
    card->end();
    y += S(16);
  }

  int cy = y;
  auto *sound = new ActionCard(pad, y, inner);
  y += S(10);
  new ActionLabel(pad + S(12), y, inner - S(24), S(22), [] { return std::string(_("Sound")); }, 1);
  y += S(28);
  if (s.audio.available) {
    new ActionButton(
        pad + S(8), y, S(38), S(36), "",
        [](Painter &p, double x, double y, double z, const Color &c) {
          const auto &a = SystemStatus::get().audio;
          glyph::speaker(p, x, y, z, 3, a.muted, c);
        },
        [] { SystemStatus::get().toggle_mute(); }, [] { return SystemStatus::get().audio.muted; }, -1, true);
    volume_ = new Slider(pad + S(54), y, inner - S(116), S(36));
    volume_->bounds(0, 100);
    volume_->value(s.audio.volume);
    volume_->tooltip(_("Volume"));
    volume_->callback([](Fl_Widget *w, void *) {
      SystemStatus::get().set_volume(int(static_cast<Fl_Slider *>(w)->value()));
      w->window()->redraw();
    });
    new ActionLabel(pad + inner - S(54), y, S(48), S(36),
                    [] { return std::to_string(SystemStatus::get().audio.volume) + "%"; });
    y += S(42);
    if (!s.audio.sinks.empty()) {
      sinks_ = new Fl_Choice(pad + S(12), y, inner - S(24), S(30));
      sinks_->color(t.surface.fl());
      sinks_->textcolor(t.text.fl());
      sinks_->textsize(t.font_size);
      sinks_->box(FL_FLAT_BOX);
      for (size_t i = 0; i < s.audio.sinks.size(); ++i) {
        std::string name;
        for (char c : s.audio.sinks[i].second) {
          if (c == '/' || c == '&' || c == '_' || c == '\\') name += '\\';
          name += c;
        }
        sinks_->add(name.c_str());
        if (s.audio.sinks[i].first == s.audio.default_sink) sinks_->value(int(i));
      }
      sinks_->tooltip(_("Output device"));
      sinks_->callback([](Fl_Widget *w, void *) {
        int i = static_cast<Fl_Choice *>(w)->value();
        const auto &s = SystemStatus::get().audio.sinks;
        if (i >= 0 && i < int(s.size())) SystemStatus::get().set_default_sink(s[size_t(i)].first);
      });
      y += S(38);
    }
  } else {
    new ActionLabel(
        pad + S(12), y, inner - S(24), S(26), [] { return std::string(_("Sound service is not running")); },
        2);
    y += S(28);
  }
  sound->size(inner, y - cy + S(6));
  sound->end();
  y += S(16);

  if (s.brightness.available) {
    int cy = y;
    auto *card = new ActionCard(pad, y, inner);
    y += S(10);
    new ActionLabel(pad + S(12), y, inner - S(24), S(22), [] { return std::string(_("Brightness")); }, 1);
    y += S(24);
    brightness_ = new Slider(pad + S(14), y, inner - S(80), S(36));
    brightness_->bounds(1, 100);
    brightness_->value(s.brightness.percent());
    brightness_->tooltip(_("Brightness"));
    brightness_->callback([](Fl_Widget *w, void *) {
      SystemStatus::get().set_brightness(int(static_cast<Fl_Slider *>(w)->value()));
      w->window()->redraw();
    });
    new ActionLabel(pad + inner - S(54), y, S(48), S(36),
                    [] { return std::to_string(SystemStatus::get().brightness.percent()) + "%"; });
    y += S(42);
    card->size(inner, y - cy + S(6));
    card->end();
    y += S(16);
  }
  if (s.battery.present) {
    new ActionLabel(
        pad, y, inner, S(24),
        [] {
          const auto &b = SystemStatus::get().battery;
          return std::to_string(b.percent) + "% · " +
                 (b.charging ? _("Charging")
                  : b.full   ? _("Fully charged")
                             : _("On battery"));
        },
        2);
    y += S(30);
  }
  content_height_ = y + S(8);
  new Fl_Box(pad, y, inner, S(8));
  body_->end();
  int H = std::min(available, content_height_ + footer + S(4));
  body_->size(W - S(4), H - footer - S(4));
  begin();
  int fy = H - footer + S(10), fb = S(40);
  new ActionLabel(pad, fy, W - 3 * fb - S(58), fb, [] { return user_name(); }, 2);
  struct Foot {
    std::string title;
    IconFn icon;
    std::vector<std::string> cmd;
  };
  std::vector<Foot> foot = {
      {_("Settings"), gear_icon(), {"sde-settings", "--page", "action-center"}},
      {_("Lock Screen"),
       [](Painter &p, double x, double y, double z, const Color &c) { glyph::lock(p, x, y, z, c); },
       {"sde-lock"}},
      {_("Power Off / Log Out"),
       [](Painter &p, double x, double y, double z, const Color &c) { glyph::power(p, x, y, z, c); },
       {"sde-logout"}},
  };
  for (size_t i = 0; i < foot.size(); ++i) {
    auto f = foot[i];
    auto *b = new ActionButton(
        W - pad - int(foot.size() - i) * (fb + S(6)), fy, fb, fb, "", f.icon, [launch, f] { launch(f.cmd); },
        nullptr, -1, true);
    b->copy_tooltip(f.title.c_str());
  }
  end();
  resizable(nullptr);
  init_sizes();
  size(W, H);
  redraw();
}

void QuickSettings::draw() {
  const auto &t = Panel::instance->theme();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.surface);
    p.stroke_rounded(0, 0, w(), h(), 0, t.border);
    p.line(S(16), h() - S(62), w() - S(16), h() - S(62), t.border);
  }
  draw_children();
}

int QuickSettings::handle(int event) { return PanelPopup::handle(event); }
