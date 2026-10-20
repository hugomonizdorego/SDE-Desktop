// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
namespace sde {
struct SuiteApp {
  std::string id, name, role, command, package, icon, upstream, url, license, version;
};
const std::vector<SuiteApp> &suite_apps();
// Developer and original-source links shared by native SDE tools.
void show_app_about(const std::string &id);
}  // namespace sde
