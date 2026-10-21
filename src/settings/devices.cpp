// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

#include "util.h"

namespace sde {

std::vector<AudioDevice> parse_pactl_devices(const std::string &text) {
  std::vector<AudioDevice> out;
  std::istringstream in(text);
  std::string line;
  AudioDevice cur;
  bool have = false;
  auto flush = [&] {
    if (have && !cur.name.empty() && !ends_with(cur.name, ".monitor")) {
      if (cur.description.empty()) cur.description = cur.name;
      out.push_back(cur);
    }
    cur = AudioDevice();
    have = false;
  };
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (starts_with(line, "Sink #") || starts_with(line, "Source #")) {
      flush();
      have = true;
    } else if (have && starts_with(t, "Name:") && cur.name.empty()) {
      cur.name = trim(t.substr(5));
    } else if (have && starts_with(t, "Description:") && cur.description.empty()) {
      cur.description = trim(t.substr(12));
    }
  }
  flush();
  return out;
}

int first_percent(const std::string &text) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') continue;
    size_t j = i;
    while (j > 0 && isdigit(static_cast<unsigned char>(text[j - 1]))) --j;
    if (j < i) return atoi(text.substr(j, i - j).c_str());
  }
  return -1;
}

bool parse_pactl_mute(const std::string &text) {
  return lower(text).find("mute: yes") != std::string::npos;
}

std::vector<std::string> split_nmcli(const std::string &line) {
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

std::vector<NetDevice> parse_nmcli_devices(const std::string &text) {
  std::vector<NetDevice> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    auto f = split_nmcli(trim(line));
    if (f.size() < 3 || f[0].empty()) continue;
    if (f[1] == "loopback" || f[1] == "wifi-p2p" || f[1] == "bridge" || f[1] == "tun") continue;
    NetDevice d;
    d.device = f[0];
    d.type = f[1];
    d.state = f[2];
    d.connection = f.size() > 3 ? f[3] : "";
    if (d.connection == "--") d.connection.clear();
    out.push_back(d);
  }
  return out;
}

std::vector<BtDevice> parse_bluetoothctl_devices(const std::string &text) {
  std::vector<BtDevice> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (!starts_with(t, "Device ")) continue;
    t = t.substr(7);
    size_t sp = t.find(' ');
    BtDevice d;
    d.address = t.substr(0, sp);
    d.name = sp == std::string::npos ? d.address : trim(t.substr(sp + 1));
    if (d.address.size() != 17) continue;
    out.push_back(d);
  }
  return out;
}

std::string bluetoothctl_field(const std::string &text, const std::string &key) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (starts_with(t, key + ":")) return trim(t.substr(key.size() + 1));
  }
  return "";
}

std::vector<std::string> with_timeout(std::vector<std::string> argv, int seconds) {
  if (find_program("timeout").empty()) return argv;
  argv.insert(argv.begin(), {"timeout", std::to_string(seconds)});
  return argv;
}

}  // namespace sde
