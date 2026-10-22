// SPDX-License-Identifier: GPL-3.0-or-later
// The development log of Settings > About: build-history.json, generated
// from CHANGELOG.md by tools/build-info.py and installed in /usr/share/sde.
//
//   {"format": 1, "version": "0.1.0~alpha50",
//    "build": {"commit": "e89a262", "number": "210", "date": "2026-10-22"},
//    "entries": [{"title": "Alpha16 patch", "date": "2026-10-21",
//                 "summary": "themes, ...", "items": ["...", ...]}, ...]}
#pragma once

#include <string>
#include <vector>

namespace sde {

struct BuildEntry {
  std::string title, date, summary;
  std::vector<std::string> items;
};

struct BuildHistory {
  std::string version, commit, number, date;
  std::vector<BuildEntry> entries;
};

// Parses the JSON text; false when it is not a build history.
bool parse_build_history(const std::string &json, BuildHistory &out);

// The installed file: $XDG_DATA_DIRS/sde/build-history.json, the SDE data
// directory, or build-history.json next to the build tree of the running
// program (development builds). "" when there is none.
std::string build_history_path();

}  // namespace sde
