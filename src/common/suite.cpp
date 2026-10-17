// SPDX-License-Identifier: GPL-3.0-or-later
#include "suite.h"
#include "suite_data.h"
#include "util.h"
namespace sde {
const std::vector<SuiteApp> &suite_apps() {
  static const std::vector<SuiteApp> apps = [] {
    std::vector<SuiteApp> result;
    for (const auto &line : split(kSuiteManifest, '\n')) {
      if (line.empty() || line[0] == '#') continue;
      auto f = split(line, '\t');
      if (f.size() == 9) result.push_back({f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]});
    }
    return result;
  }();
  return apps;
}
}  // namespace sde
