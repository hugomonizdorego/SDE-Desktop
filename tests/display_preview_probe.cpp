// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only process: exercise rollback including parent death on private Xvfb.
#include <unistd.h>

#include "../src/display/backend.h"
int main(int argc, char** argv) {
  auto outputs = sde::query_outputs();
  if (outputs.empty()) return 2;
  bool primary = false;
  for (const auto& o : outputs) primary |= o.enabled && o.primary;
  if (!primary)
    for (auto& o : outputs)
      if (o.enabled) {
        o.primary = true;
        break;
      }
  sde::DisplayPreview preview(outputs, 1);
  if (!preview.ready()) return 3;
  std::string mode = argc > 1 ? argv[1] : "revert";
  if (mode == "keep") return preview.keep() ? 0 : 4;
  if (mode == "crash") _exit(0);
  if (mode == "timeout") {
    usleep(1500000);
    return preview.keep() ? 5 : 0;
  }
  preview.revert();
  return 0;
}
