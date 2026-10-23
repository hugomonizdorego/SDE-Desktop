// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// XSETTINGS model for sde-xsettingsd: parsing the settings file and
// encoding the _XSETTINGS_SETTINGS property (freedesktop XSETTINGS
// specification 0.5). Kept free of Xlib so it can be unit tested.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sde {
namespace xsettings {

struct Setting {
  enum Type : uint8_t { INTEGER = 0, STRING = 1, COLOR = 2 } type = INTEGER;
  int32_t integer = 0;
  std::string string;
  uint16_t color[4] = {0, 0, 0, 0xFFFF};  // red, green, blue, alpha
  uint32_t serial = 0;                    // serial of the last change
  bool operator==(const Setting &o) const;
};

using Settings = std::map<std::string, Setting>;

// Parses "Name value" lines: value is an integer, a "quoted string" (with \"
// and \\ escapes) or a color (r, g, b[, a]) with 16-bit components.
// Empty lines and lines starting with # are ignored. Malformed lines are
// reported in |errors| ("line N: ...") and skipped.
Settings parse(const std::string &text, std::vector<std::string> *errors = nullptr);

// Copies |next| over |current|, giving settings whose value changed (or that
// are new) the serial |serial|; settings missing from |next| are removed.
// Returns true when anything changed.
bool merge(Settings &current, const Settings &next, uint32_t serial);

// The _XSETTINGS_SETTINGS property contents in the machine's byte order.
std::vector<uint8_t> encode(const Settings &settings, uint32_t serial);

}  // namespace xsettings
}  // namespace sde
