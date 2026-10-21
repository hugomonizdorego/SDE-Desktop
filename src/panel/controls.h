// SPDX-License-Identifier: GPL-3.0-or-later
// Native service integrations. Eduka-Panel behavior reference: see ALPHA9.md.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dbus.h"
struct AudioStream {
  unsigned id = 0;
  std::string app, title, icon, pid, binary;
  int volume = 100;
  bool muted = false, corked = false;
};
struct MediaPlayer {
  std::string service, owner, name, title, artist, pid, binary, status = "Stopped";
  bool control = false, play = false, pause = false, next = false, previous = false, raise = false;
};
struct BluetoothAdapter {
  std::string path, name;
  bool powered = false, discovering = false;
};
struct BluetoothDevice {
  std::string path, adapter, name, address;
  bool paired = false, connected = false, trusted = false;
};
struct NetworkDevice {
  std::string device, kind, state, connection, ip, gateway, dns;
};
// A USB stick, memory card or external disk (not the system disk).
struct DrivePartition {
  std::string path, label, mountpoint, fstype;
};
struct RemovableDrive {
  std::string name, path, label, model, kind, icon;  // kind: usb, card, hdd, ssd
  long long size = 0;
  std::vector<DrivePartition> parts;
  std::string mountpoint() const;
};
// Parses `lsblk -P -b -o NAME,PATH,LABEL,SIZE,TYPE,TRAN,RM,HOTPLUG,ROTA,
// MOUNTPOINT,FSTYPE,MODEL,VENDOR,PKNAME` (rules from Eduka-Panel).
std::vector<RemovableDrive> parse_removable_drives(const std::string& text);
std::string human_size(long long bytes);
std::vector<AudioStream> parse_audio_streams(const std::string& text);
std::string match_media_player(const AudioStream&, const std::vector<MediaPlayer>&);
std::vector<NetworkDevice> parse_network_devices(const std::string& text);
class PanelControls {
 public:
  static PanelControls& get();
  void start(std::function<void()> changed);
  void set_visible(bool visible);
  void refresh();
  void refresh_media();
  void refresh_audio_apps();
  void refresh_bluetooth();
  void refresh_devices();
  void select_adapter(const std::string& path);
  void bluetooth_power(bool on);
  void bluetooth_scan();
  void stop_scan();
  void bluetooth_device(const std::string& path);
  void bluetooth_forget(const std::string& path);
  void media_command(const std::string& service, const std::string& command);
  void set_stream_volume(unsigned id, int volume);
  void mute_stream(unsigned id, bool muted);
  void microphone_volume(int percent);
  void microphone_mute();
  void microphone_source(const std::string& name);
  void network_device(const std::string& name, bool connect);
  void wireless_off(bool off);
  // Removable drives: open mounts when needed and shows the files in Liria;
  // eject unmounts every partition and, with |power_off|, powers it off.
  void refresh_drives();
  void drive_open(const std::string& name);
  void drive_eject(const std::string& name, bool power_off);
  // power-profiles-daemon (powerprofilesctl).
  void refresh_power_profiles();
  void set_power_profile(const std::string& profile);
  const BluetoothAdapter* adapter() const;
  std::vector<MediaPlayer> players() const;
  std::vector<BluetoothAdapter> adapters;
  std::vector<BluetoothDevice> bluetooth;
  std::vector<AudioStream> streams;
  std::vector<NetworkDevice> devices;
  std::vector<std::pair<std::string, std::string>> sources;
  std::vector<RemovableDrive> drives;
  std::vector<std::string> power_profiles;
  std::string power_profile;
  std::string adapter_path, message, keyboard = "US", default_source;
  int mic_volume = 0;
  bool mic_available = false, mic_muted = false, bluetooth_available = false,
       bluetooth_busy = false;
  bool airplane = false;

 private:
  void changed();
  void read_player(const std::string& service, const std::string& owner);
  void refresh_microphone();
  void bluez_call(const std::string& path, const std::string& iface, const std::string& method,
                  std::function<void(bool)> done = {});
  sde::dbus::Bus *session_ = nullptr, *system_ = nullptr;
  std::map<std::string, MediaPlayer> players_;
  std::map<std::string, unsigned> player_generation_;
  static void scan_timeout(void*);
  static void drives_later(void*);
  std::function<void()> changed_;
  std::string discovery_owned_;
  bool drives_pending_ = false, profiles_pending_ = false;
  bool visible_ = false, audio_pending_ = false, devices_pending_ = false, bt_pending_ = false,
       bt_again_ = false;
  bool restore_wifi_ = false, restore_wwan_ = false;
  std::vector<std::string> restore_bluetooth_;
  static void tick(void*);
};
