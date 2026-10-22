// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <iostream>

#include "../src/panel/controls.h"
#include "../src/panel/panel.h"
#include "../src/panel/quick.h"
// drives.cpp reads [drives] keys through the panel; there is none here.
Panel* Panel::instance = nullptr;
// The parser/backend unit executable does not start a desktop or system services.
SystemStatus& SystemStatus::get() {
  static SystemStatus s;
  return s;
}
void SystemStatus::refresh_network(bool) {}
void SystemStatus::set_wifi_enabled(bool on) { network.wifi_enabled = on; }
void SystemStatus::changed() {}
void SystemStatus::check_connectivity(bool, std::function<void(int)> done) {
  if (done) done(network.connectivity);
}
void SystemStatus::troubleshoot_network() {}
int main() {
  auto a = parse_audio_streams(R"(Sink Input #0
    Mute: yes
    Corked: no
    Volume: front-left: 42598 / 65% / -11 dB
    application.name = "Music Player"
    application.process.id = "42"
    application.process.binary = "music"
    media.name = "Alpha song"
Sink Input #7
    Mute: no
    Volume: front-left: 0 / 0% / -inf dB
    application.name = "Browser"
    media.name = "Audio Stream"
Sink Input #oops
)");
  assert(a.size() == 2 && a[0].id == 0 && a[0].volume == 65 && a[0].muted && !a[0].corked);
  assert(a[0].app == "Music Player" && a[0].pid == "42" && a[0].title == "Alpha song");
  assert(a[1].volume == 0 && !a[1].muted && a[1].title.empty());
  assert(parse_audio_streams("unavailable\n").empty());
  // Removable drives: the system disk and loop devices are left out; a USB
  // stick with a mounted partition and a memory card are found.
  auto drives = parse_removable_drives(
      "NAME=\"sda\" PATH=\"/dev/sda\" LABEL=\"\" SIZE=\"500107862016\" TYPE=\"disk\" TRAN=\"sata\" RM=\"0\" HOTPLUG=\"0\" ROTA=\"0\" MOUNTPOINT=\"\" FSTYPE=\"\" MODEL=\"Internal SSD\" VENDOR=\"ATA\" PKNAME=\"\"\n"
      "NAME=\"sda1\" PATH=\"/dev/sda1\" LABEL=\"\" SIZE=\"500000000000\" TYPE=\"part\" TRAN=\"\" RM=\"0\" HOTPLUG=\"0\" ROTA=\"0\" MOUNTPOINT=\"/\" FSTYPE=\"ext4\" MODEL=\"\" VENDOR=\"\" PKNAME=\"sda\"\n"
      "NAME=\"sdb\" PATH=\"/dev/sdb\" LABEL=\"\" SIZE=\"16008609792\" TYPE=\"disk\" TRAN=\"usb\" RM=\"1\" HOTPLUG=\"1\" ROTA=\"0\" MOUNTPOINT=\"\" FSTYPE=\"\" MODEL=\"Flash\\x20Disk\" VENDOR=\"Kingston\" PKNAME=\"\"\n"
      "NAME=\"sdb1\" PATH=\"/dev/sdb1\" LABEL=\"DATA\" SIZE=\"16000000000\" TYPE=\"part\" TRAN=\"\" RM=\"1\" HOTPLUG=\"1\" ROTA=\"0\" MOUNTPOINT=\"/media/user/DATA\" FSTYPE=\"vfat\" MODEL=\"\" VENDOR=\"\" PKNAME=\"sdb\"\n"
      "NAME=\"mmcblk0\" PATH=\"/dev/mmcblk0\" LABEL=\"\" SIZE=\"31914983424\" TYPE=\"disk\" TRAN=\"\" RM=\"0\" HOTPLUG=\"0\" ROTA=\"0\" MOUNTPOINT=\"\" FSTYPE=\"\" MODEL=\"\" VENDOR=\"\" PKNAME=\"\"\n"
      "NAME=\"mmcblk0p1\" PATH=\"/dev/mmcblk0p1\" LABEL=\"\" SIZE=\"31914000000\" TYPE=\"part\" TRAN=\"\" RM=\"0\" HOTPLUG=\"0\" ROTA=\"0\" MOUNTPOINT=\"\" FSTYPE=\"exfat\" MODEL=\"\" VENDOR=\"\" PKNAME=\"mmcblk0\"\n"
      "NAME=\"loop0\" PATH=\"/dev/loop0\" LABEL=\"\" SIZE=\"1000\" TYPE=\"loop\" TRAN=\"\" RM=\"0\" HOTPLUG=\"0\" ROTA=\"0\" MOUNTPOINT=\"/snap/x\" FSTYPE=\"squashfs\" MODEL=\"\" VENDOR=\"\" PKNAME=\"\"\n");
  assert(drives.size() == 2);
  assert(drives[0].name == "sdb" && drives[0].kind == "usb" && drives[0].label == "DATA");
  assert(drives[0].model == "Kingston Flash Disk" && drives[0].mountpoint() == "/media/user/DATA");
  assert(drives[1].name == "mmcblk0" && drives[1].kind == "card" && drives[1].mountpoint().empty());
  assert(human_size(16008609792LL) == "16.0 GB" && human_size(512) == "512 B");
  MediaPlayer p;
  p.service = "org.mpris.MediaPlayer2.music.instance1";
  p.name = "Music Player";
  p.pid = "42";
  assert(match_media_player(a[0], {p}) == p.service);
  p.pid = "100";
  assert(match_media_player(a[0], {p}) == p.service);
  p.name = "Different";
  assert(match_media_player(a[0], {p}) == p.service);
  p.service = "x";
  assert(match_media_player(a[0], {p}).empty());
  auto n = parse_network_devices(
      "eth0:ethernet:connected:School\\: "
      "LAN\nwlan0:wifi:disconnected:\nlo:loopback:connected:lo\nbroken\n");
  assert(n.size() == 2 && n[0].device == "eth0" && n[0].connection == "School: LAN" &&
         n[1].kind == "wifi");
  // The Action Center's media card: the playing player (the most recently
  // started one), else the most recently active paused one.
  {
    MediaPlayer music, video, browser;
    music.service = "org.mpris.MediaPlayer2.music";
    music.title = "Song";
    music.status = "Paused";
    music.active_ms = 500;
    video.service = "org.mpris.MediaPlayer2.vlc";
    video.title = "Holiday.mp4";
    video.status = "Paused";
    video.active_ms = 900;
    browser.service = "org.mpris.MediaPlayer2.firefox.instance_1_23";
    browser.status = "Stopped";
    browser.active_ms = 2000;
    if (choose_player({}) != -1) return 11;
    if (choose_player({music, video, browser}) != 1) return 12;  // most recent paused
    browser.status = "Playing";
    browser.title = "A video - YouTube";
    if (choose_player({music, video, browser}) != 2) return 13;  // playing wins
    music.status = "Playing";
    music.active_ms = 3000;
    if (choose_player({music, video, browser}) != 0) return 14;  // most recently started
    // A titled player beats an empty one of the same state.
    MediaPlayer empty;
    empty.status = "Paused";
    empty.active_ms = 99999;
    video.active_ms = 1;
    if (choose_player({empty, video}) != 1) return 15;
    // Position: moves while playing at the rate, clamped to the length.
    MediaPlayer t;
    t.status = "Playing";
    t.position_us = 10000000;
    t.position_ms = 1000;
    t.length_us = 12000000;
    if (t.position_now(2000) != 11000000 || t.position_now(9000) != 12000000) return 16;
    t.status = "Paused";
    if (t.position_now(9000) != 10000000) return 17;
    if (format_media_time(187000000) != "3:07" || format_media_time(3723000000LL) != "1:02:03" ||
        format_media_time(-5) != "0:00")
      return 18;
  }
  std::cout << "Audio streams, escaped network fields, media matching and player choice passed\n";
}
