// SPDX-License-Identifier: GPL-3.0-or-later
// Identify system applets by provider identity, never arbitrary icon/title text.
#pragma once
#include <cctype>
#include <string>

namespace indicator {
enum Role : unsigned { Unknown = 0, Battery = 1, Bluetooth = 2, Network = 4, Sound = 8, Input = 16 };
inline std::string identity(const std::string &name) {
  std::string out;
  for (unsigned char c : name)
    if (std::isalnum(c)) out += char(std::tolower(c));
  return out;
}
inline Role role(const std::string &name) {
  const auto id = identity(name);
  for (const char *s : {"sdepowermanager", "matepowermanager", "xfce4powermanager",
                        "indicatorpower", "ayatanaindicatorpower", "orggnomesettingsdaemonpower"})
    if (id == s) return Battery;
  for (const char *s : {"blueman", "bluemanapplet", "bluemantray", "indicatorbluetooth", "ayatanaindicatorbluetooth"})
    if (id == s) return Bluetooth;
  for (const char *s : {"nmapplet", "networkmanagerapplet", "indicatornetwork", "ayatanaindicatornetwork"})
    if (id == s) return Network;
  for (const char *s : {"pasystray", "volumeicon", "matevolumecontrolstatusicon", "indicatorsound", "ayatanaindicatorsound"})
    if (id == s) return Sound;
  for (const char *s : {"ibus", "ibusuigtk3", "ibuspanel", "fcitx", "fcitx5", "fcitxui"})
    if (id == s) return Input;
  return Unknown;
}
}  // namespace indicator
