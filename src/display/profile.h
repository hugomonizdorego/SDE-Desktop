// SPDX-License-Identifier: MIT
// Copyright (c) 2025 zac15987
// Copyright (c) 2026 SDE contributors
// C++ port of Profile/DisplaySetting in Core/Profile.cs and unique naming in
// Core/ProfileManager.cs, DisplayProfileManager c873b66ba54b6efb111620e736878c21eda22f51.
// https://github.com/zac15987/DisplayProfileManager — full notice: profile.LICENSE.
#pragma once
#include <string>
#include <vector>
namespace sde {
struct DisplayMode {
  std::string name;
  int width = 0, height = 0;
  double rate = 0;
};
struct Output {
  std::string name, readable_name, identity;
  bool connected = false, enabled = false, primary = false;
  int x = 0, y = 0, width = 0, height = 0, mm_width = 0, mm_height = 0;
  std::string current_mode, preferred_mode, rotation = "normal";
  double rate = 0;
  std::vector<std::string> modes, rotations;
  std::vector<DisplayMode> available;
};
struct DisplayProfile {
  std::string name;
  int scale = 100;  // X11 interface DPI is shared, unlike Windows per-monitor DPI.
  std::vector<Output> outputs;
};
std::string unique_profile_name(const std::string &base, const std::vector<std::string> &names);
}  // namespace sde
