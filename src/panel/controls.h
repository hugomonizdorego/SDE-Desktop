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
  // Metadata: xesam:album, mpris:artUrl, mpris:trackid and mpris:length.
  std::string album, art_url, track_id;
  long long length_us = 0;
  // Position (microseconds) as read at |position_ms| (steady clock), and
  // the playback rate, so the seek bar moves between reads.
  long long position_us = 0, position_ms = 0;
  double rate = 1.0;
  bool seek = false;
  // When the player last started playing or changed track (steady ms);
  // picks the most recently active player.
  long long active_ms = 0;
  // Interpolated position now, clamped to the track length.
  long long position_now(long long now_ms) const;
};
// The player the Action Center shows: one that is Playing (the most
// recently started), else the most recently active paused one, else any.
// Returns -1 for an empty list.
int choose_player(const std::vector<MediaPlayer>& players);
// "3:07" or "1:02:03".
std::string format_media_time(long long us);
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
  // MPRIS SetPosition (absolute, microseconds); needs CanSeek and a track id.
  void media_seek(const std::string& service, long long position_us);
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
  // "checking" until the first answer for a connection (the LED is not
  // green before it), "unknown" when the check failed.
  std::string net_verdict;
  bool network_checker() const;
  // |done| runs once the verdict is in.
  void check_network(std::function<void()> done = {});
  // --quick --json said a repair would change something.
  bool net_repairable = false;
  // The panel's view of the network (kind|connection, "" offline, and
  // NetworkManager's connectivity): checks again on a new connection, a
  // connectivity change and, while the verdict is bad, after 20 s, 40 s...
  // (at most every 5 minutes). Offers the repair once per connection.
  void network_state(const std::string& key, int connectivity);
  // Repairs the network through pkexec: the safe repair
  // (/usr/libexec/debion-network-repair) when polkit allows it without a
  // password, else (or with |full|) debion-network-check --fix.
  // |interactive| false (the automatic repair after login) never asks for
  // a password: when the safe repair is not enough it offers "Fix now".
  void repair_network(bool full, bool interactive = true);
  // Flyout "Troubleshoot": NetworkManager's check, then the verdict; DNS
  // problems are repaired, others reconnect.
  void troubleshoot_network();
  // "running", "ok", "failed" (or ""), and the sentence the flyout shows.
  std::string net_repair_state, net_repair_text;
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
  // Network (drives.cpp, "debion-network-check").
  void start_network();
  void verdict_in(bool auto_repair);
  void offer_repair();
  static void net_retry(void*);
  static void net_refresh(void*);
  std::vector<std::function<void()>> verdict_waiters_;
  std::string net_key_;
  int net_connectivity_ = -1, net_retries_ = 0;
  bool net_offered_ = false, net_auto_tried_ = false, net_refresh_pending_ = false;
  uint32_t net_notification_ = 0;
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
