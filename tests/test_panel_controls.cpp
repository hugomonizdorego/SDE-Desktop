// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <iostream>

#include "../src/panel/controls.h"
#include "../src/panel/quick.h"
// The parser/backend unit executable does not start a desktop or system services.
SystemStatus& SystemStatus::get() {
  static SystemStatus s;
  return s;
}
void SystemStatus::refresh_network(bool) {}
void SystemStatus::set_wifi_enabled(bool on) { network.wifi_enabled = on; }
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
  std::cout << "Audio streams, escaped network fields and media matching passed\n";
}
