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
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include "build_config.h"
#include "config.h"
#include "controls.h"
#include "dbus.h"
#include "desktop_entry.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "panel.h"
#include "process.h"
#include "sni.h"
#include "spawn.h"
#include "theme.h"
#include "tray.h"
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
  PanelControls::get().start([this] { this->changed(); });
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
                    run_async({"nmcli", "-t", "-f", "IN-USE,SIGNAL,SECURITY,SSID", "device", "wifi",
                               "list", "--rescan", rescan ? "yes" : "no"},
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
    bool hot = Fl::belowmouse() == this, on = on_ && on_();
    static const unsigned colors[] = {0x2563EB, 0x7C3AED, 0x047857, 0xB45309, 0xBE185D, 0x0E7490};
    bool multi =
        Panel::instance->config().get_bool("action_center", "multicolor", false) && color_ >= 0;
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
    {
      Painter p(window()->w(), window()->h());
      // Soft accent glow on the card: a two-stop gradient instead of a flat fill.
      p.fill_gradient(x(), y(), w(), h(), S(16),
                      t.surface_alt.mix(t.accent, t.dark ? 0.24 : 0.14), t.surface_alt, true);
      p.stroke_rounded(x(), y(), w(), h(), S(16), t.border);
      if (analog) {
        double cx = x() + S(48), cy = y() + h() / 2.0, r = S(32);
        p.circle(cx, cy, r, t.surface);
        for (int i = 0; i < 12; ++i) {
          double a = i * M_PI / 6;
          p.line(cx + std::sin(a) * (r - S(4)), cy - std::cos(a) * (r - S(4)),
                 cx + std::sin(a) * (r - S(7)), cy - std::cos(a) * (r - S(7)), t.text_dim, S(1));
        }
        double hour = (lt.tm_hour % 12 + lt.tm_min / 60.0) * M_PI / 6;
        double minute = lt.tm_min * M_PI / 30;
        p.line(cx, cy, cx + std::sin(hour) * r * 0.48, cy - std::cos(hour) * r * 0.48, t.text,
               S(3));
        p.line(cx, cy, cx + std::sin(minute) * r * 0.76, cy - std::cos(minute) * r * 0.76, t.accent,
               S(2));
        p.circle(cx, cy, S(3), t.accent);
      }
    }
    char date[128], clock[64];
    strftime(clock, sizeof clock, cfg.get_bool("panel", "clock_24h", true) ? "%H:%M" : "%I:%M %p",
             &lt);
    strftime(date, sizeof date, "%A, %e %B", &lt);
    // Time on the left, date and greeting stacked beside it.
    int left = x() + (analog ? S(96) : S(18));
    text_font(t.font_size + (analog ? 12 : 22), true);
    int cw = text_width(clock) + S(8);
    draw_text(clock, left, y(), cw, h(), t.text);
    int rx = left + cw + S(12), rw = x() + w() - rx - S(14);
    bool greet = cfg.get_bool("action_center", "show_greeting", true);
    int mid = y() + h() / 2;
    if (greet) {
      const char *greeting = lt.tm_hour < 12   ? _("Good morning")
                             : lt.tm_hour < 18 ? _("Good afternoon")
                                               : _("Good evening");
      text_font(t.font_size, true);
      draw_text(greeting, rx, mid - S(22), rw, S(20), t.accent_text);
    }
    text_font(t.font_size - 1);
    draw_text(date, rx, greet ? mid : mid - S(10), rw, S(20), t.text_dim);
  }
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
IconFn calendar_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    p.stroke_rounded(x + s * .1, y + s * .18, s * .8, s * .72, s * .12, c, s * .08);
    p.line(x + s * .12, y + s * .4, x + s * .88, y + s * .4, c, s * .08);
    for (int i = 0; i < 2; ++i)
      p.line(x + s * (.32 + i * .36), y + s * .06, x + s * (.32 + i * .36), y + s * .28, c,
             s * .08);
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

namespace {
// Callback ownership follows the widget, including during asynchronous rebuilds.
class CommandButton : public FlatButton {
 public:
  CommandButton(int x, int y, int w, int h, const std::string &name, std::function<void()> fn)
      : FlatButton(x, y, w, h, name.c_str()), fn_(std::move(fn)) {
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
}  // namespace
QuickSettings::QuickSettings() : PanelPopup("Action Center") { end(); }
void QuickSettings::select_section(const std::string &section) {
  static const std::vector<std::string> valid = {"overview", "network", "bluetooth", "sound",
                                                 "media",    "tray",    "power"};
  section_ = std::find(valid.begin(), valid.end(), section) != valid.end() ? section : "overview";
  if (shown()) status_changed();
}
void QuickSettings::closed() { PanelControls::get().stop_scan(); }
std::string QuickSettings::structure() const {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  const auto &cfg = Panel::instance->config();
  std::ostringstream out;
  out << section_ << s.network.available << s.network.has_wifi << s.network.wifi_enabled
      << s.network.connection;
  for (const auto &n : s.wifi) out << '|' << n.ssid << n.active << n.secure;
  out << s.audio.available << s.audio.default_sink << s.brightness.available << s.battery.present;
  for (const auto &sink : s.audio.sinks) out << '|' << sink.first << sink.second;
  out << s.message.empty() << c.message.empty();
  out << c.mic_available << c.mic_muted << c.default_source << c.bluetooth_available
      << c.bluetooth_busy << c.adapter_path << c.airplane;
  for (const auto &source : c.sources) out << '|' << source.first << source.second;
  for (const auto &a : c.adapters) out << '|' << a.path << a.name << a.powered << a.discovering;
  for (const auto &d : c.bluetooth) out << '|' << d.path << d.name << d.paired << d.connected;
  for (const auto &p : c.players())
    out << '|' << p.service << p.name << p.title << p.artist << p.status << p.control << p.play
        << p.pause << p.next << p.previous << p.raise;
  for (const auto &a : c.streams) out << '|' << a.id << a.app << a.title << a.muted;
  for (const auto &d : c.devices)
    out << '|' << d.device << d.state << d.connection << d.ip << d.gateway << d.dns;
  if (auto *h = Panel::instance->status_notifiers())
    for (auto *i : h->visible_items()) out << '|' << i->service << i->path << i->title << i->status;
  if (auto *tray = Panel::instance->legacy_tray()) out << tray->count();
  for (const char *key :
       {"show_clock", "show_greeting", "clock_style", "multicolor", "move_status", "show_media",
        "show_app_volume", "show_microphone", "show_tray", "tile_columns"})
    out << '|' << cfg.get("action_center", key);
  for (const char *key : {"style", "position", "height", "width_percent", "clock_24h"})
    out << '|' << cfg.get("panel", key);
  out << cfg.get("appearance", "accent") << cfg.get("appearance", "theme")
      << cfg.get("appearance", "scale") << cfg.get("notifications", "do_not_disturb");
  return out.str();
}
void QuickSettings::prepare() {
  SystemStatus::get().refresh_brightness();
  SystemStatus::get().message.clear();
  PanelControls::get().message.clear();
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
  self->rebuild();
  self->body_->scroll_to(0,
                         std::min(scroll, std::max(0, self->content_height_ - self->body_->h())));
  self->reposition();
  if (focus && self->body_->children()) self->body_->child(0)->take_focus();
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
  if (!shown() || !Panel::instance->status_in_center()) return;
  auto *tray = Panel::instance->legacy_tray();
  if (!tray) return;
  if (tray_box_ && fl_xid(this)) {
    tray->set_background(Panel::instance->theme().surface_alt.fl() >> 8);
    tray->place_in(fl_xid(this), tray_box_->x(), tray_box_->y(), tray_columns_,
                   body_->y() - tray_box_->y(), body_->h());
  } else
    tray->hide();
}
void QuickSettings::rebuild() {
  Fl::remove_timeout(rebuild_timeout, this);
  rebuild_pending_ = false;
  structure_ = structure();
  clear();
  resizable(nullptr);
  volume_ = brightness_ = microphone_ = nullptr;
  tray_box_ = nullptr;
  stream_sliders_.clear();
  const auto &t = Panel::instance->theme();
  const auto &cfg = Panel::instance->config();
  auto &s = SystemStatus::get();
  auto &c = PanelControls::get();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  int W = std::min(S(424), mw), pad = S(16), inner = W - 2 * pad - S(12), footer = S(58), y = S(10);
  auto *panel = Panel::instance;
  int available =
      std::max(S(120), t.panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my);
  begin();
  body_ = new Fl_Scroll(S(2), S(2), W - S(4), available - footer - S(4));
  body_->type(Fl_Scroll::VERTICAL_ALWAYS);
  body_->resizable(nullptr);
  body_->scrollbar_size(S(10));
  body_->box(FL_FLAT_BOX);
  body_->color(t.surface.fl());
  // Give Fl_Scroll a stable zero origin. Otherwise its padding is reported as
  // negative scroll and repeated service rebuilds accumulate blank space.
  new Fl_Box(body_->x(), body_->y(), 1, 1);
  auto label = [&](std::string text, int style = 0, int height = 24) {
    new ActionLabel(pad, y, inner, S(height), [text] { return text; }, style);
    y += S(height);
  };
  auto row = [&](std::string text, IconFn icon, std::function<void()> fn, bool enabled = true) {
    auto *b =
        new ActionButton(pad + S(8), y, inner - S(16), S(36), text, icon, fn, nullptr, -1, true);
    if (!enabled) b->deactivate();
    y += S(42);
    return b;
  };
  auto info = [&](std::string text) {
    new ActionLabel(pad + S(12), y, inner - S(24), S(24), [text] { return text; }, 2);
    y += S(24);
  };
  auto launch = [this](std::vector<std::string> cmd) {
    close();
    spawn_detached(cmd);
  };
  auto bt_icon = [](Painter &p, double x, double y, double z, const Color &ink) {
    glyph::bluetooth(p, x, y, z, ink);
  };
  auto audio_icon = [](Painter &p, double x, double y, double z, const Color &ink) {
    glyph::speaker(p, x, y, z, 3, false, ink);
  };
  auto media_icon = [](Painter &p, double x, double y, double z, const Color &ink) {
    glyph::media(p, x, y, z, ink, false);
  };
  auto network_icon = [](Painter &p, double x, double y, double z, const Color &ink) {
    const auto &n = SystemStatus::get().network;
    if (n.kind == "ethernet")
      glyph::ethernet(p, x, y, z, ink);
    else
      glyph::wifi(p, x, y, z, n.kind == "wifi" ? 4 : 0, ink, n.kind.empty());
  };
  auto start_card = [&](std::string title) {
    auto *card = new ActionCard(pad, y, inner);
    y += S(10);
    new ActionLabel(pad + S(12), y, inner - S(24), S(24), [title] { return title; }, 1);
    y += S(28);
    return card;
  };
  auto end_card = [&](ActionCard *card) {
    card->size(inner, y - card->y() + S(6));
    card->end();
    y += S(16);
  };
  label(_("Action Center"), 1, 28);
  y += S(4);
  if (section_ != "overview")
    row(
        _("Back to overview"),
        [](Painter &p, double x, double y, double z, const Color &ink) {
          p.line(x + z * .62, y + z * .2, x + z * .3, y + z * .5, ink, z * .1);
          p.line(x + z * .3, y + z * .5, x + z * .62, y + z * .8, ink, z * .1);
        },
        [this] { select_section("overview"); });
  if (!s.message.empty() || !c.message.empty()) {
    new ActionLabel(
        pad, y, inner, S(28),
        [] {
          const auto &m = PanelControls::get().message;
          return m.empty() ? SystemStatus::get().message : m;
        },
        2);
    y += S(30);
  }
  // Application tray only. System indicators are the quick tiles below, so
  // they are not repeated here (alpha9 showed every status icon twice).
  auto tray_card = [&] {
    auto *h = Panel::instance->status_notifiers();
    auto items = h ? h->visible_items() : std::vector<const SniHost::Item *>{};
    auto *tray = Panel::instance->legacy_tray();
    bool moved = cfg.get_bool("action_center", "move_status", true);
    bool legacy = tray && tray->count() && moved;
    if (!cfg.get_bool("action_center", "show_tray", true) && section_ != "tray") return;
    if (items.empty() && !legacy && section_ != "tray") return;
    if (section_ != "overview" && section_ != "tray" && !moved) return;
    auto *card = start_card(_("Tray applications"));
    int cell = S(36), columns = std::max(1, (inner - S(24)) / cell);
    if (items.empty() && !legacy) info(_("No applications are in the tray."));
    for (size_t i = 0; i < items.size(); ++i)
      new TrayIndicator(pad + S(12) + int(i % columns) * cell, y + int(i / columns) * cell, cell,
                        *items[i]);
    // XEmbed icons continue on the last SNI row when there is room for them.
    int last = int(items.size() % size_t(columns)), rows = int((items.size() + columns - 1) / columns);
    int tx = pad + S(12), ty = y + rows * cell;
    if (legacy) {
      int slot = tray->icon_size() + S(6), room = inner - S(24) - last * cell;
      if (rows && last && room >= slot) {
        tx += last * cell;
        ty -= cell;
        tray_columns_ = std::max(1, room / slot);
      } else {
        tray_columns_ = std::max(1, (inner - S(24)) / slot);
      }
      int h = tray->grid_height(tray_columns_);
      tray_box_ = new Fl_Box(tx, ty + std::max(0, (cell - h) / 2), inner - S(24) - (tx - pad - S(12)), h);
      y = std::max(ty + std::max(cell, h), y + rows * cell) + S(6);
    } else {
      y += rows * cell;
    }
    end_card(card);
  };
  auto player_card = [&](const MediaPlayer &p) {
    auto *card = start_card(p.name);
    info(p.title.empty() ? _("No track title") : p.title);
    if (!p.artist.empty()) info(p.artist);
    int gap = S(6), bw = (inner - S(24) - 2 * gap) / 3;
    std::vector<std::pair<std::string, std::string>> buttons = {
        {_("Previous"), "Previous"},
        {p.status == "Playing" ? _("Pause") : _("Play"), "PlayPause"},
        {_("Next"), "Next"}};
    bool allowed[] = {p.control && p.previous,
                      p.control && (p.status == "Playing" ? p.pause : p.play), p.control && p.next};
    for (int i = 0; i < 3; ++i) {
      auto cmd = buttons[i].second;
      auto *b = new CommandButton(
          pad + S(12) + i * (bw + gap), y, bw, S(34), buttons[i].first,
          [service = p.service, cmd] { PanelControls::get().media_command(service, cmd); });
      if (!allowed[i]) b->deactivate();
    }
    y += S(42);
    if (p.raise)
      row(_("Open player"), media_icon,
          [service = p.service] { PanelControls::get().media_command(service, "Raise"); });
    end_card(card);
  };
  // The moved panel tray stays at the top of every page, so it never jumps
  // when the page content below it changes.
  if (section_ != "overview") tray_card();
  if (section_ == "overview") {
    if (cfg.get_bool("action_center", "show_clock", true)) {
      new ClockCard(pad, y, inner, S(96));
      y += S(108);
    }
    tray_card();
    struct Tile {
      std::string title;
      IconFn icon;
      std::function<void()> click;
      std::function<bool()> on;
      StringFn sub;
    };
    std::vector<Tile> tiles = {
        {_("Network"), network_icon, [this] { select_section("network"); },
         [] { return !SystemStatus::get().network.kind.empty(); },
         [] {
           const auto &n = SystemStatus::get().network;
           return n.connection.empty() ? std::string(_("Offline")) : n.connection;
         }},
        {_("Bluetooth"), bt_icon, [this] { select_section("bluetooth"); },
         [] {
           auto *a = PanelControls::get().adapter();
           return a && a->powered;
         },
         [] {
           auto *a = PanelControls::get().adapter();
           return std::string(!a ? _("No adapter") : a->powered ? _("On") : _("Off"));
         }},
        {_("Wireless off"),
         [](Painter &p, double x, double y, double z, const Color &ink) {
           glyph::airplane(p, x, y, z, ink);
         },
         [] {
           auto &c = PanelControls::get();
           c.wireless_off(!c.airplane);
         },
         [] { return PanelControls::get().airplane; },
         [] { return std::string(_("Wi-Fi, WWAN, Bluetooth")); }},
        {_("Display"), display_icon(), [launch] { launch({"sde-display-settings"}); }, nullptr,
         [] { return std::string(_("Monitors and scale")); }},
        {_("Do Not Disturb"),
         [](Painter &p, double x, double y, double z, const Color &ink) {
           glyph::bell(p, x, y, z,
                       Panel::instance->config().get_bool("notifications", "do_not_disturb", false),
                       ink);
         },
         [] {
           auto *p = Panel::instance;
           p->set_do_not_disturb(!p->config().get_bool("notifications", "do_not_disturb", false));
         },
         [] {
           return Panel::instance->config().get_bool("notifications", "do_not_disturb", false);
         },
         [] { return std::string(_("Notifications")); }},
        {_("Dark Mode"),
         [](Painter &p, double x, double y, double z, const Color &ink) {
           glyph::sun(p, x, y, z, ink);
         },
         [] { Panel::instance->toggle_dark_mode(); }, [] { return Panel::instance->theme().dark; },
         [] { return std::string(_("Appearance")); }}};
    int columns = cfg.get_int("action_center", "tile_columns", 2) == 3 ? 3 : 2, gap = S(8),
        tw = (inner - (columns - 1) * gap) / columns, th = S(78);
    for (size_t i = 0; i < tiles.size(); ++i) {
      const auto &v = tiles[i];
      new ActionButton(pad + int(i % columns) * (tw + gap), y + int(i / columns) * (th + gap), tw,
                       th, v.title, v.icon, v.click, v.on, int(i), false, v.sub);
    }
    y += int((tiles.size() + columns - 1) / columns) * (th + gap) + S(10);
  }
  if (section_ == "network") {
    auto *card = start_card(_("Network"));
    info(s.network.available
             ? (s.network.connection.empty() ? _("Not connected") : s.network.connection)
             : _("NetworkManager is not running"));
    if (s.network.available && s.network.has_wifi) {
      row(s.network.wifi_enabled ? _("Turn Wi-Fi Off") : _("Turn Wi-Fi On"), network_icon, [] {
        auto &s = SystemStatus::get();
        s.set_wifi_enabled(!s.network.wifi_enabled);
      });
      if (s.network.wifi_enabled) {
        row(_("Scan Wi-Fi networks"), network_icon,
            [] { SystemStatus::get().refresh_network(true); });
        int count = 0;
        for (const auto &net : s.wifi) {
          if (++count > 16) break;
          auto *b = row(
              net.ssid + (net.active   ? " · Connected"
                          : net.secure ? " · Secured"
                                       : " · Open"),
              [net](Painter &p, double x, double y, double z, const Color &ink) {
                glyph::wifi(p, x, y, z, std::max(1, net.signal / 25), ink);
              },
              [net] {
                if (!net.active) SystemStatus::get().connect_wifi(net);
              });
          if (net.active) b->deactivate();
        }
      }
    }
    end_card(card);
    for (const auto &d : c.devices) {
      auto *card = start_card(d.device + " · " + d.kind);
      info(d.state + (d.connection.empty() ? "" : " · " + d.connection));
      if (!d.ip.empty()) info("IP: " + d.ip);
      if (!d.gateway.empty()) info("Gateway: " + d.gateway);
      if (!d.dns.empty()) info("DNS: " + d.dns);
      if (d.kind == "ethernet")
        row(d.state == "connected" ? _("Disconnect LAN") : _("Connect LAN"), network_icon,
            [d] { PanelControls::get().network_device(d.device, d.state != "connected"); });
      end_card(card);
    }
    if (!find_program("nm-connection-editor").empty())
      row(_("Network Connections..."), gear_icon(), [launch] { launch({"nm-connection-editor"}); });
  }
  if (section_ == "bluetooth") {
    auto *card = start_card(_("Bluetooth"));
    if (c.adapters.empty())
      info(c.bluetooth_available ? _("No Bluetooth adapter detected")
                                 : _("Bluetooth service is unavailable"));
    for (const auto &a : c.adapters)
      if (c.adapters.size() > 1)
        row(
            a.name + (a.path == c.adapter_path ? " · Selected" : ""), bt_icon,
            [path = a.path] { PanelControls::get().select_adapter(path); }, !c.bluetooth_busy);
    if (auto *a = c.adapter()) {
      info(a->name);
      row(
          a->powered ? _("Turn Bluetooth Off") : _("Turn Bluetooth On"), bt_icon,
          [on = !a->powered] { PanelControls::get().bluetooth_power(on); }, !c.bluetooth_busy);
      if (a->powered)
        row(
            a->discovering ? _("Stop scanning") : _("Scan for devices"), bt_icon,
            [] {
              auto &c = PanelControls::get();
              auto *a = c.adapter();
              if (a && a->discovering)
                c.stop_scan();
              else
                c.bluetooth_scan();
            },
            !c.bluetooth_busy);
      if (c.bluetooth_busy) info(_("Waiting for Bluetooth..."));
      if (a->powered)
        for (const auto &d : c.bluetooth)
          if (d.adapter == a->path) {
            info((d.name.empty() ? d.address : d.name) + " · " +
                 (d.connected ? _("Connected")
                  : d.paired  ? _("Paired")
                              : _("Available")));
            row(
                d.connected ? _("Disconnect")
                : d.paired  ? _("Connect")
                            : _("Pair and connect"),
                bt_icon, [path = d.path] { PanelControls::get().bluetooth_device(path); },
                !c.bluetooth_busy);
            if (d.paired && !d.connected)
              row(
                  _("Forget device"), bt_icon,
                  [path = d.path] {
                    if (fl_choice(_("Remove this Bluetooth pairing?"), _("Cancel"), _("Remove"),
                                  nullptr) == 1)
                      PanelControls::get().bluetooth_forget(path);
                  },
                  !c.bluetooth_busy);
          }
    }
    end_card(card);
    info(_("PIN/code pairing uses the system Bluetooth agent."));
    if (!find_program("blueman-manager").empty())
      row(_("Bluetooth Settings..."), gear_icon(), [launch] { launch({"blueman-manager"}); });
  }
  if (section_ == "overview" || section_ == "sound") {
    auto *card = start_card(_("Sound"));
    if (s.audio.available) {
      new ActionButton(
          pad + S(8), y, S(38), S(36), "",
          [](Painter &p, double x, double y, double z, const Color &ink) {
            glyph::speaker(p, x, y, z, 3, SystemStatus::get().audio.muted, ink);
          },
          [] { SystemStatus::get().toggle_mute(); }, [] { return SystemStatus::get().audio.muted; },
          -1, true);
      volume_ = new Slider(pad + S(54), y, inner - S(116), S(36));
      volume_->bounds(0, 100);
      volume_->value(s.audio.volume);
      volume_->tooltip(_("Output volume"));
      volume_->callback([](Fl_Widget *w, void *) {
        SystemStatus::get().set_volume(int(static_cast<Fl_Slider *>(w)->value()));
      });
      new ActionLabel(pad + inner - S(54), y, S(48), S(36),
                      [] { return std::to_string(SystemStatus::get().audio.volume) + "%"; });
      y += S(42);
      if (section_ == "sound" && !s.audio.sinks.empty()) {
        auto *choice = new Fl_Choice(pad + S(12), y, inner - S(24), S(30));
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
        y += S(38);
      }
      if (section_ == "overview")
        row(_("Sound, microphone and applications"), audio_icon,
            [this] { select_section("sound"); });
    } else
      info(_("Sound service is unavailable"));
    end_card(card);
    if (section_ == "sound" && c.mic_available &&
        cfg.get_bool("action_center", "show_microphone", true)) {
      card = start_card(_("Microphone"));
      row(
          c.mic_muted ? _("Unmute microphone") : _("Mute microphone"),
          [](Painter &p, double x, double y, double z, const Color &ink) {
            glyph::microphone(p, x, y, z, ink);
          },
          [] { PanelControls::get().microphone_mute(); });
      microphone_ = new Slider(pad + S(14), y, inner - S(28), S(36));
      microphone_->bounds(0, 100);
      microphone_->value(c.mic_volume);
      microphone_->tooltip(_("Microphone volume"));
      microphone_->callback([](Fl_Widget *w, void *) {
        PanelControls::get().microphone_volume(int(static_cast<Fl_Slider *>(w)->value()));
      });
      y += S(42);
      if (!c.sources.empty()) {
        auto *choice = new Fl_Choice(pad + S(12), y, inner - S(24), S(30));
        choice->textsize(t.font_size);
        choice->tooltip(_("Input device"));
        for (size_t i = 0; i < c.sources.size(); ++i) {
          choice->add(choice_label(c.sources[i].second).c_str());
          if (c.sources[i].first == c.default_source) choice->value(int(i));
        }
        choice->callback([](Fl_Widget *w, void *) {
          int i = static_cast<Fl_Choice *>(w)->value();
          const auto &v = PanelControls::get().sources;
          if (i >= 0 && i < int(v.size()))
            PanelControls::get().microphone_source(v[size_t(i)].first);
        });
        y += S(38);
      }
      end_card(card);
    }
  }
  if (section_ == "sound" || section_ == "media") {
    if (cfg.get_bool("action_center", "show_media", true)) {
      auto players = c.players();
      if (players.empty())
        label(_("No active media players"), 2);
      else
        for (const auto &p : players) player_card(p);
    }
    if (cfg.get_bool("action_center", "show_app_volume", true)) {
      auto *card = start_card(_("Application volume"));
      if (c.streams.empty()) info(_("No applications are playing sound"));
      int count = 0;
      for (const auto &a : c.streams) {
        if (++count > 32) break;
        info(a.app + (a.title.empty() ? "" : " · " + a.title));
        auto *slider = new Slider(pad + S(14), y, inner - S(94), S(34));
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
            pad + inner - S(72), y, S(60), S(34), "",
            [muted = a.muted](Painter &p, double x, double y, double z, const Color &ink) {
              glyph::speaker(p, x, y, z, 3, muted, ink);
            },
            [id = a.id, muted = !a.muted] { PanelControls::get().mute_stream(id, muted); },
            [muted = a.muted] { return muted; }, -1, true);
        mute->copy_tooltip(a.muted ? _("Unmute application") : _("Mute application"));
        y += S(40);
        auto player = match_media_player(a, c.players());
        if (!player.empty())
          row(_("Open player"), media_icon,
              [player] { PanelControls::get().media_command(player, "Raise"); });
      }
      end_card(card);
    }
  }
  if (section_ == "media") {
    auto *card = start_card(_("Installed media applications"));
    int count = 0;
    for (const auto &app : load_applications())
      if (std::find(app.categories.begin(), app.categories.end(), "AudioVideo") !=
          app.categories.end()) {
        row(app.name, media_icon, [this, app] {
          close();
          launch_entry(app);
        });
        if (++count == 8) break;
      }
    if (!count) info(_("Install a media player to play music or video."));
    end_card(card);
  }
  if (section_ == "overview" || section_ == "power") {
    if (s.brightness.available) {
      auto *card = start_card(_("Brightness"));
      brightness_ = new Slider(pad + S(14), y, inner - S(28), S(36));
      brightness_->bounds(1, 100);
      brightness_->value(s.brightness.percent());
      brightness_->tooltip(_("Brightness"));
      brightness_->callback([](Fl_Widget *w, void *) {
        SystemStatus::get().set_brightness(int(static_cast<Fl_Slider *>(w)->value()));
      });
      y += S(42);
      end_card(card);
    }
    if (s.battery.present)
      new ActionLabel(
          pad, y, inner, S(28),
          [] {
            const auto &b = SystemStatus::get().battery;
            return std::to_string(b.percent) + "% · " +
                   (b.charging ? _("Charging")
                    : b.full   ? _("Fully charged")
                               : _("On battery"));
          },
          2),
          y += S(34);
    if (section_ == "power")
      row(_("Power Settings"), gear_icon(), [launch] { launch({"sde-power-preferences"}); });
  }
  if (section_ == "overview") {
    auto players = c.players();
    if (cfg.get_bool("action_center", "show_media", true) && !players.empty())
      player_card(players.front());
  }

  content_height_ = y + S(8);
  new Fl_Box(pad, y, inner, S(8));
  body_->end();
  int H = std::min(available, content_height_ + footer + S(4));
  body_->size(W - S(4), H - footer - S(4));
  begin();
  int fy = H - footer + S(10), fb = S(36);
  new UserBadge(pad, fy, W - S(210), fb);
  std::vector<std::pair<std::string, std::function<void()>>> foot = {
      {_("Calendar"),
       [this] {
         close();
         Panel::instance->toggle_calendar();
       }},
      {_("Action Center Settings"),
       [launch] { launch({"sde-settings", "--page", "action-center"}); }},
      {_("Lock Screen"), [launch] { launch({"sde-lock"}); }},
      {_("Power Off / Log Out"), [launch] { launch({"sde-logout"}); }}};
  std::vector<IconFn> icons = {calendar_icon(), gear_icon(),
                               [](Painter &p, double x, double y, double z, const Color &ink) {
                                 glyph::lock(p, x, y, z, ink);
                               },
                               [](Painter &p, double x, double y, double z, const Color &ink) {
                                 glyph::power(p, x, y, z, ink);
                               }};
  for (size_t i = 0; i < foot.size(); ++i) {
    auto *b = new ActionButton(W - pad - int(foot.size() - i) * (fb + S(6)), fy, fb, fb, "",
                               icons[i], foot[i].second, nullptr, -1, true);
    b->copy_tooltip(foot[i].first.c_str());
  }
  end();
  resizable(nullptr);
  init_sizes();
  size(W, H);
  sync();
}
void QuickSettings::draw() {
  const auto &t = Panel::instance->theme();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.surface);
    if (t.glass)
      p.fill_rect(0, h() - S(58), w(), S(58), t.surface_alt.mix(t.surface, 0.4));
    p.stroke_rounded(0.5, 0.5, w() - 1, h() - 1, 0, t.border);
    p.line(S(16), h() - S(58), w() - S(16), h() - S(58), t.border);
  }
  draw_children();
  sync_native_tray();
}
int QuickSettings::handle(int event) {
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
