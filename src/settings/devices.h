// SPDX-License-Identifier: GPL-3.0-or-later
// Parsers for the command-line tools behind the Sound, Network and
// Bluetooth pages (pactl, nmcli, bluetoothctl). No FLTK; unit tested.
#pragma once

#include <string>
#include <vector>

namespace sde {

struct AudioDevice {
  std::string name;         // "alsa_output.pci-0000_00_1f.3.analog-stereo"
  std::string description;  // "Built-in Audio Analog Stereo"
};
// "pactl list sinks" / "pactl list sources". Monitor sources are skipped.
std::vector<AudioDevice> parse_pactl_devices(const std::string &text);
// First "NN%" in pactl output; -1 when there is none.
int first_percent(const std::string &text);
// "Mute: yes" -> true.
bool parse_pactl_mute(const std::string &text);

struct NetDevice {
  std::string device, type, state, connection;
};
// Splits one "nmcli -t" line; ':' inside values is escaped as "\:".
std::vector<std::string> split_nmcli(const std::string &line);
// "nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status"; loopback and
// unmanaged helper devices (lo, p2p) are skipped.
std::vector<NetDevice> parse_nmcli_devices(const std::string &text);

struct BtDevice {
  std::string address, name;
  bool connected = false;
};
// "bluetoothctl devices [Paired|Connected]": "Device AA:BB:.. Name".
std::vector<BtDevice> parse_bluetoothctl_devices(const std::string &text);
// Value of "Key: value" in "bluetoothctl show" (e.g. "Powered").
std::string bluetoothctl_field(const std::string &text, const std::string &key);

// Prefixes |argv| with "timeout SECONDS" when coreutils' timeout exists, so
// a helper that waits for a missing daemon cannot hang.
std::vector<std::string> with_timeout(std::vector<std::string> argv, int seconds = 4);

}  // namespace sde
