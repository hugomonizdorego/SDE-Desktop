// SPDX-License-Identifier: GPL-3.0-or-later
// Flyouts of the panel's Network, Drives and Touchpad icons (alpha50).
//
// Network: one compact header (connection, state, the LED colour and a "⋯"
// menu with Troubleshoot, Check internet connection, sign-in page, Airplane
// mode), then an on/off switch for Wi-Fi (radio) and for each wired device
// (connect/disconnect), then the Wi-Fi networks.
// Drives: every removable drive with its size and free space, Open and
// Eject ("Safely remove"), progress and which programs keep it busy.
// Touchpad: on/off, tap to click, natural scrolling, speed and a link to
// Control Center > Touchpad.
// The window title names the section ("Network", "Drives", "Touchpad").
#pragma once

#include <string>

#include "popup.h"

class StatusFlyout : public PanelPopup {
 public:
  StatusFlyout();
  static bool handles(const std::string &section);
  void select_section(const std::string &section);
  const std::string &section() const { return section_; }
  // Service data changed: rebuilds when the shown rows change.
  void status_changed();
  void draw() override;
  int handle(int event) override;

 protected:
  void prepare() override;
  void closed() override;

 private:
  void rebuild();
  std::string structure() const;
  static void rebuild_timeout(void *);
  void network_section(struct FlyBuilder &b);
  void drives_section(struct FlyBuilder &b);
  void touchpad_section(struct FlyBuilder &b);
  void network_menu();
  std::string section_ = "network";
  std::string structure_;
  class Fl_Scroll *body_ = nullptr;
  int header_ = 0, content_height_ = 0;
  bool rebuild_pending_ = false;
};
