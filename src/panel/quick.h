// SPDX-License-Identifier: GPL-3.0-or-later
// Quick Settings: network, sound, brightness, battery and Do Not Disturb.
//
// SystemStatus collects the state from the services the system already has:
// NetworkManager (nmcli), PulseAudio or PipeWire (pactl), systemd-logind
// (brightness) and the kernel's power_supply class (battery). Missing
// services simply hide their section.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "popup.h"

struct NetworkState {
  bool available = false;  // NetworkManager is running
  bool has_wifi = false;   // a Wi-Fi device exists
  bool wifi_enabled = false;
  std::string kind;        // "wifi", "ethernet" or "" when offline
  std::string connection;  // connection or network name
  int signal = 0;          // 0-100 for Wi-Fi
};

struct WifiNetwork {
  std::string ssid;
  int signal = 0;
  bool secure = false;
  bool active = false;
};

struct AudioState {
  bool available = false;
  int volume = 0;  // percent
  bool muted = false;
  std::string default_sink;
  std::vector<std::pair<std::string, std::string>> sinks;  // name, description
};

struct BrightnessState {
  bool available = false;
  std::string device;  // /sys/class/backlight/<device>
  int value = 0, max = 0;
  int percent() const { return max > 0 ? value * 100 / max : 0; }
};

struct BatteryState {
  bool present = false;
  int percent = 0;
  bool charging = false;
  bool full = false;
  int minutes = -1;  // time to empty or full, -1 if unknown
};

class SystemStatus {
 public:
  static SystemStatus &get();

  // Starts monitoring; |changed| runs whenever something changes.
  void start(std::function<void()> changed);

  NetworkState network;
  std::vector<WifiNetwork> wifi;
  AudioState audio;
  BrightnessState brightness;
  BatteryState battery;
  std::string message;  // last result of a user action ("Connected to ...")

  void refresh_network(bool rescan = false);
  void refresh_audio();
  void refresh_battery();
  void refresh_brightness();

  void set_volume(int percent);
  void toggle_mute();
  void set_default_sink(const std::string &name);
  void set_wifi_enabled(bool on);
  void connect_wifi(const WifiNetwork &net);
  void set_brightness(int percent);

 private:
  void changed();
  void start_audio_stream();
  void parse_wifi_list(const std::string &out);
  std::function<void()> changed_;
  int audio_stream_ = 0;
  bool audio_pending_ = false;
};

// The Action Center (corner popup: header clock, quick tiles, sliders, media,
// calendar and notifications, compact or expanded) and, in flyout mode, the
// small popup of one panel status icon (Sound, Network, Bluetooth, Power,
// Media, Drives, Keyboard, Tray) anchored above that icon. Both share the
// section builders below; the window title names the flyout's section.
class QuickSettings : public PanelPopup {
 public:
  enum Mode { ACTION_CENTER, FLYOUT };
  explicit QuickSettings(Mode mode = ACTION_CENTER);
  // Flyout: network, bluetooth, sound, media, tray, power, drives, keyboard.
  void select_section(const std::string &section);
  const std::string &section() const { return section_; }
  bool flyout() const { return mode_ == FLYOUT; }
  // Action Center: show the calendar card for this opening (the panel
  // clock), even in compact mode.
  void set_calendar_view(bool on) { calendar_view_ = on; }
  bool calendar_view() const { return calendar_view_; }
  // Window title of a flyout section ("Sound", "Network", ...).
  static std::string section_title(const std::string &section);
  static bool valid_section(const std::string &section);
  // Called by the panel when SystemStatus changed.
  void status_changed();
  void draw() override;
  int handle(int event) override;

 protected:
  void prepare() override;
  void closed() override;
  bool corner_popup() const override { return mode_ == ACTION_CENTER; }
  bool native_children() const override { return true; }

 private:
  void rebuild();
  // Section builders; |b| tracks the layout of the scrolled body.
  void build_center(struct SectionBuilder &b);
  void build_flyout(struct SectionBuilder &b);
  void volume_row(struct SectionBuilder &b, bool more);
  void brightness_row(struct SectionBuilder &b);
  void sound_section(struct SectionBuilder &b);
  void network_section(struct SectionBuilder &b);
  void bluetooth_section(struct SectionBuilder &b);
  void media_section(struct SectionBuilder &b);
  void power_section(struct SectionBuilder &b);
  void drives_section(struct SectionBuilder &b);
  void keyboard_section(struct SectionBuilder &b);
  void tray_section(struct SectionBuilder &b);
  void player_card(struct SectionBuilder &b, const struct MediaPlayer &p);
  void sync();
  void sync_native_tray();
  bool expanded() const;
  void toggle_expanded();
  std::string structure() const;
  static void rebuild_timeout(void *);
  Mode mode_;
  class Fl_Scroll *body_ = nullptr;
  class Fl_Slider *volume_ = nullptr;
  class Fl_Slider *brightness_ = nullptr;
  class Fl_Slider *microphone_ = nullptr;
  class Fl_Widget *tray_box_ = nullptr;
  class Fl_Widget *calendar_ = nullptr;
  std::map<unsigned, class Fl_Slider *> stream_sliders_;
  int tray_columns_ = 1;
  std::string structure_;
  int content_height_ = 0, header_ = 0, footer_ = 0;
  std::string section_ = "overview";
  bool rebuild_pending_ = false;
  bool calendar_view_ = false, scroll_to_calendar_ = false;
};
