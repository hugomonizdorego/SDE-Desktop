// SPDX-License-Identifier: GPL-3.0-or-later
// Interface animations for the SDE menu and launcher tiles.
//
// The effect set, the capability rule (4 GiB memory and 4 CPU threads) and
// the speed/size choices follow Eduka-Desktop's Effects page
// (https://github.com/hugomonizdorego/Eduka-Desktop); SDE draws them with its
// own cairo widgets and keeps its own look.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sde {

class Config;

struct Effects {
  bool on = false;          // effects are enabled and the computer can run them
  std::string hover;        // none, wave, glow, slide, pop, bounce
  std::string launch;       // none, zoom, zoom-out, ripple, bounce, bubbles, confetti, fade
  double duration = 0.32;   // seconds for one launch effect (speed choice)
  double size = 1.0;        // 0.7 small, 1.0 normal, 1.35 large
  bool random = false;      // pick a different launch effect each time
};

// [effects] in sde.conf. "auto" enables effects only on capable computers;
// [appearance] low_resource=true always turns them off.
Effects effects_from_config(const Config &cfg);

// Memory (GiB) and CPU threads, and whether they meet Eduka's minimum.
bool effects_capable(double *gib = nullptr, int *threads = nullptr);

// (id, label) pairs for Settings, in display order.
const std::vector<std::pair<std::string, std::string>> &hover_effects();
const std::vector<std::pair<std::string, std::string>> &launch_effects();

// Easing used by every SDE animation (ease-out cubic).
double ease_out(double t);

}  // namespace sde
