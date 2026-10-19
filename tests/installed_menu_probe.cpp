// SPDX-License-Identifier: GPL-3.0-or-later
// CI-only probe: inspect installed applications through the real Start Menu loader.
#include <cstdio>
#include "desktop_entry.h"

int main() {
  for (const auto &entry : sde::load_applications("SDE"))
    std::printf("%s\t%s\n", entry.id.c_str(), entry.menu_category().c_str());
}
