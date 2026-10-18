// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
namespace sde {
struct SuiteApp {
  std::string id, name, role, command, package, icon, upstream, url, license;
};
const std::vector<SuiteApp> &suite_apps();
}  // namespace sde
