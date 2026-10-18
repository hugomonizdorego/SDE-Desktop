// SPDX-License-Identifier: MIT
// Copyright (c) 2025 zac15987
// Copyright (c) 2026 SDE contributors
// Port of ProfileManager.GetUniqueProfileName; see profile.h and profile.LICENSE.
#include "profile.h"

#include <algorithm>

#include "util.h"
namespace sde {
std::string unique_profile_name(const std::string &base, const std::vector<std::string> &names) {
  auto has = [&](const std::string &name) {
    return std::any_of(names.begin(), names.end(),
                       [&](const std::string &s) { return lower(s) == lower(name); });
  };
  if (!has(base)) return base;
  int counter = 1;
  std::string unique;
  do {
    unique = base + " (" + std::to_string(counter++) + ")";
  } while (has(unique));
  return unique;
}
}  // namespace sde
