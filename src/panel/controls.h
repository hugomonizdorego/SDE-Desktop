// SPDX-License-Identifier: GPL-3.0-or-later
// Native service integrations. Eduka-Panel behavior reference: see ALPHA9.md.
#pragma once
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "dbus.h"
#include "status_model.h"
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
  // Removable drives (drives.cpp): UDisks2 over the system bus, lsblk and
  // udisksctl when UDisks2 is not reachable. Open mounts when needed and
  // shows the files in Liria; eject unmounts every partition and, with
  // |power_off|, powers the drive off (or ejects the medium).
  void refresh_drives();
  void drive_open(const std::string& name);
  void drive_eject(const std::string& name, bool power_off = true);
  // What is going on with a drive (by name): "mounting", "ejecting",
  // "busy", "removed", "error", with a sentence for the flyout.
  struct DriveTask {
    std::string state, text;
  };
  std::map<std::string, DriveTask> drive_tasks;
  // Free bytes of each mounted filesystem (by mount point).
  std::map<std::string, long long> drive_free;
  bool udisks() const { return udisks_; }
  // debion-network-check --quick --json (when installed): its verdict for
  // the current connection, "" until it answered. Run on connection
  // changes and when the Network flyout opens, never on a timer.
  std::string net_verdict;
  bool network_checker() const;
  void check_network();
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
  void start_drives();
  void udisks_refresh();
  void drives_found(std::vector<RemovableDrive> found);
  void drive_notify(const RemovableDrive& d);
  void drive_task(const std::string& name, const std::string& state, const std::string& text);
  void unmount_next(std::shared_ptr<std::vector<DrivePartition>> parts, size_t i,
                    RemovableDrive drive, bool power_off);
  void power_off(const RemovableDrive& drive, bool power_off);
  bool verdict_pending_ = false, verdict_again_ = false;
  bool udisks_ = false, drives_scanned_ = false, notify_listening_ = false;
  std::vector<std::string> known_drives_;
  std::map<uint32_t, std::string> drive_notifications_;  // notification id -> drive name
  std::function<void()> changed_;
  std::string discovery_owned_;
  bool drives_pending_ = false, profiles_pending_ = false;
  bool visible_ = false, audio_pending_ = false, devices_pending_ = false, bt_pending_ = false,
       bt_again_ = false;
  bool restore_wifi_ = false, restore_wwan_ = false;
  std::vector<std::string> restore_bluetooth_;
  static void tick(void*);
};
