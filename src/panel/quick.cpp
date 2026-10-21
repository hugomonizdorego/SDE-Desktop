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
#include <map>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include "build_config.h"
#include "calendar.h"
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
#include "tray_icons.h"
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

class ClockCard : public Fl_Widget {
 public:
  ClockCard(int x, int y, int w, int h, bool flat = false)
      : Fl_Widget(x, y, w, h), flat_(flat) {
    clear_visible_focus();
  }
  void draw() override {
    const Theme &t = Panel::instance->theme();
    const Config &cfg = Panel::instance->config();
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    bool analog = cfg.get("action_center", "clock_style", "digital") == "analog";
    double r = std::min(double(S(32)), h() / 2.0 - S(4));
    double cx = x() + (flat_ ? 0 : S(16)) + r, cy = y() + h() / 2.0;
    {
      Painter p(window()->w(), window()->h());
      if (!flat_) {
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
    int left = analog ? int(cx + r + S(14)) : x() + (flat_ ? 0 : S(18));
    text_font(t.font_size + (analog ? 10 : flat_ ? 16 : 22), true);
    int cw = text_width(clock) + S(8);
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
  bool flat_;
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
IconFn power_icon() {
  return [](Painter &p, double x, double y, double s, const Color &c) {
    glyph::power(p, x, y, s, c);
  };
}

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
// First installed program of |names|, or "".
std::string first_program(std::initializer_list<const char *> names) {
  for (const char *n : names)
    if (!find_program(n).empty()) return n;
  return "";
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
void QuickSettings::closed() {
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
        << p.pause << p.next << p.previous << p.raise;
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
  SystemStatus::get().message.clear();
  PanelControls::get().message.clear();
  scroll_to_calendar_ = calendar_view_;
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
  if (!self->scroll_to_calendar_)
    self->body_->scroll_to(
        0, std::min(scroll, std::max(0, self->content_height_ - self->body_->h())));
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

void QuickSettings::brightness_row(SectionBuilder &b) {
  const auto &s = SystemStatus::get();
  int x = b.pad + S(8), right = b.pad + b.inner - S(8), pct = S(44);
  auto *icon = new ActionButton(x, b.y, S(38), S(36), "", sun_icon(), [] {}, nullptr, -1, true);
  icon->clear_visible_focus();
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

void QuickSettings::player_card(SectionBuilder &b, const MediaPlayer &p) {
  int pad = b.pad, inner = b.inner;
  auto *card = b.start_card(p.name);
  b.info(p.title.empty() ? _("No track title") : p.title);
  if (!p.artist.empty()) b.info(p.artist);
  b.y += S(4);
  int gap = S(6), bw = (inner - S(24) - 2 * gap) / 3;
  std::vector<std::pair<std::string, std::string>> buttons = {
      {_("Previous"), "Previous"},
      {p.status == "Playing" ? _("Pause") : _("Play"), "PlayPause"},
      {_("Next"), "Next"}};
  bool allowed[] = {p.control && p.previous,
                    p.control && (p.status == "Playing" ? p.pause : p.play), p.control && p.next};
  for (int i = 0; i < 3; ++i) {
    auto cmd = buttons[i].second;
    auto *button = new CommandButton(
        pad + S(12) + i * (bw + gap), b.y, bw, S(34), buttons[i].first,
        [service = p.service, cmd] { PanelControls::get().media_command(service, cmd); });
    if (!allowed[i]) button->deactivate();
  }
  b.y += S(42);
  if (p.raise)
    b.row(_("Open player"), media_icon,
          [service = p.service] { PanelControls::get().media_command(service, "Raise"); });
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
  if (!mixer.empty()) b.settings_row(_("Sound settings"), {mixer});
}

void QuickSettings::network_section(SectionBuilder &b) {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  auto *card = b.start_card(_("Status"));
  b.info(s.network.available
             ? (s.network.connection.empty() ? _("Not connected") : s.network.connection)
             : _("NetworkManager is not running"));
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
  if (!find_program("nm-connection-editor").empty())
    b.settings_row(_("Network settings"), {"nm-connection-editor"});
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
  if (!find_program("blueman-manager").empty())
    b.settings_row(_("Bluetooth settings"), {"blueman-manager"});
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
  b.settings_row(_("Power settings"), prefs.empty() ? std::vector<std::string>{"sde-settings",
                                                                               "--page", "system"}
                                                    : std::vector<std::string>{prefs});
  b.row(_("Shut down or log out"), power_icon(), b.launch({"sde-logout"}));
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
  b.settings_row(_("Keyboard settings"), {"sde-settings", "--page", "input"});
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
  b.settings_row(_("Panel settings"), {"sde-settings", "--page", "panel"});
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
                       return n.kind == "wifi" && !n.connection.empty() ? n.connection
                                                                        : std::string(_("Wi-Fi"));
                     },
                     open_flyout("network")});
  else
    tiles.push_back({_("Network"), network_icon, open_flyout("network"),
                     [] { return !SystemStatus::get().network.kind.empty(); },
                     [] {
                       const auto &n = SystemStatus::get().network;
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
  tiles.push_back({_("Display"), display_icon(), b.launch({"sde-display-settings"}), nullptr,
                   nullptr, nullptr});
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
  tiles.push_back({_("Settings"), gear_icon(), b.launch({"sde-settings"}), nullptr, nullptr,
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
      calendar_ = new MonthCalendar(pad + S(12), b.y, inner - S(24));
      b.y += calendar_->h();
    }
    b.end_card(card);
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
  header_ = fly ? S(48) : S(70);
  footer_ = fly ? 0 : S(58);

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
    if (cfg.get_bool("action_center", "show_clock", true))
      new ClockCard(pad + S(4), S(8), W - 2 * pad - toggle - S(12), header_ - S(14), true);
    else
      new ActionLabel(pad + S(4), S(8), W - 2 * pad - toggle, header_ - S(14),
                      [] { return std::string(_("Action Center")); }, 1);
    bool open = expanded();
    icon_button(W - pad - toggle, (header_ - toggle) / 2, toggle, toggle,
                open ? _("Show less") : _("Show more"), fold_icon(open),
                [this] { toggle_expanded(); });
  }
  if (!fly) {
    int fy = H - footer_ + S(11), fb = S(36);
    std::vector<std::pair<std::string, std::function<void()>>> foot = {
        {_("Action Center Settings"), b.launch({"sde-settings", "--page", "action-center"})},
        {_("Lock Screen"), b.launch({"sde-lock"})},
        {_("Power Off / Log Out"), b.launch({"sde-logout"})}};
    std::vector<IconFn> icons = {gear_icon(),
                                 [](Painter &p, double x, double y, double z, const Color &ink) {
                                   glyph::lock(p, x, y, z, ink);
                                 },
                                 power_icon()};
    new UserBadge(pad, fy, W - 2 * pad - int(foot.size()) * (fb + S(6)) - S(8), fb);
    for (size_t i = 0; i < foot.size(); ++i)
      icon_button(W - pad - int(foot.size() - i) * (fb + S(6)) + S(6), fy, fb, fb, foot[i].first,
                  icons[i], foot[i].second);
  }
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
  sync();
}

void QuickSettings::draw() {
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
  if (qa_dump_enabled() && body_)
    qa_dump(this, "@body\t" + std::to_string(body_->x()) + "\t" + std::to_string(body_->y()) +
                      "\t" + std::to_string(body_->w()) + "\t" + std::to_string(body_->h()) +
                      "\t1\n");
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
