// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <thread>

#include "config.h"
#include "i18n.h"
#include "util.h"

namespace sde {

bool effects_capable(double *gib, int *threads) {
  double memory = 0;
  std::ifstream info("/proc/meminfo");
  std::string key;
  long long kib = 0;
  while (info >> key >> kib) {
    if (key == "MemTotal:") {
      memory = kib / 1024.0 / 1024.0;
      break;
    }
    info.ignore(256, '\n');
  }
  int cpus = int(std::thread::hardware_concurrency());
  if (cpus <= 0) cpus = int(sysconf(_SC_NPROCESSORS_ONLN));
  if (gib) *gib = memory;
  if (threads) *threads = cpus;
  // Eduka-Desktop's rule: at least 4 GB (reported as ~3.7 GiB) and 4 threads.
  return memory >= 3.5 && cpus >= 4;
}

const std::vector<std::pair<std::string, std::string>> &hover_effects() {
  static const std::vector<std::pair<std::string, std::string>> list = {
      {"none", _("None")},  {"wave", _("Wave")},   {"glow", _("Glow")},
      {"slide", _("Slide")}, {"pop", _("Pop")},    {"bounce", _("Bounce")}};
  return list;
}

const std::vector<std::pair<std::string, std::string>> &launch_effects() {
  static const std::vector<std::pair<std::string, std::string>> list = {
      {"none", _("None")},         {"zoom", _("Zoom in")},     {"zoom-out", _("Zoom out")},
      {"ripple", _("Ripple")},     {"bounce", _("Bounce")},    {"bubbles", _("Bubbles")},
      {"confetti", _("Confetti")}, {"fade", _("Fade")}};
  return list;
}

double ease_out(double t) {
  t = std::max(0.0, std::min(1.0, t));
  double u = 1.0 - t;
  return 1.0 - u * u * u;
}

Effects effects_from_config(const Config &cfg) {
  Effects e;
  std::string mode = lower(cfg.get("effects", "enabled", "auto"));
  bool low = cfg.get_bool("appearance", "low_resource", false);
  e.on = !low && (mode == "true" || (mode == "auto" && effects_capable()));
  e.hover = cfg.get("effects", "hover", "pop");
  e.launch = cfg.get("effects", "launch", "zoom");
  std::string speed = cfg.get("effects", "speed", "normal");
  e.duration = speed == "fast" ? 0.2 : speed == "slow" ? 0.5 : 0.32;
  std::string size = cfg.get("effects", "size", "normal");
  e.size = size == "small" ? 0.7 : size == "large" ? 1.35 : 1.0;
  e.random = cfg.get_bool("effects", "random", false);
  if (e.random && e.on) {
    static unsigned seed = unsigned(time(nullptr));
    const auto &all = launch_effects();
    e.launch = all[1 + rand_r(&seed) % (all.size() - 1)].first;
  }
  if (!e.on) e.hover = e.launch = "none";
  return e;
}

}  // namespace sde
