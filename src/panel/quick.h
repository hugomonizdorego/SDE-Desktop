// SPDX-License-Identifier: GPL-3.0-or-later
// Quick Settings: network, sound, brightness, battery and Do Not Disturb.
//
// SystemStatus collects the state from the services the system already has:
// NetworkManager (nmcli), PulseAudio or PipeWire (pactl), systemd-logind
// (brightness) and the kernel's power_supply class (battery). Missing
// services simply hide their section.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "popup.h"

struct NetworkState {
  bool available = false;     // NetworkManager is running
  bool has_wifi = false;      // a Wi-Fi device exists
  bool wifi_enabled = false;
  std::string kind;           // "wifi", "ethernet" or "" when offline
  std::string connection;     // connection or network name
  int signal = 0;             // 0-100 for Wi-Fi
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

class QuickSettings : public PanelPopup {
 public:
  QuickSettings();
  // Called by the panel when SystemStatus changed.
  void status_changed();
  void draw() override;
  int handle(int event) override;

 protected:
  void prepare() override;

 private:
  struct Text {
    int x, y, w, h;
    std::function<std::string()> text;
    int style;  // 0 normal, 1 heading, 2 dim
  };
  void rebuild();
  void sync();
  std::string structure() const;

  // Owns callback data of the current widgets.
  template <class T>
  T *keep(T *p) {
    keep_.emplace_back(std::shared_ptr<T>(p));
    return p;
  }
  std::vector<std::shared_ptr<void>> keep_;
  std::vector<Text> texts_;
  std::vector<int> separators_;  // y positions
  std::string structure_;
  class Fl_Slider *volume_ = nullptr;
  class Fl_Slider *brightness_ = nullptr;
  class Fl_Choice *sinks_ = nullptr;
  bool rebuild_pending_ = false;
};
