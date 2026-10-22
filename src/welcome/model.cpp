// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "model.h"

#include <algorithm>
#include <sstream>

#include "config.h"
#include "i18n.h"
#include "util.h"

namespace sde {
namespace welcome {

const char *const kProjectUrl = "https://github.com/hugomonizdorego/SDE-Desktop";
const char *const kIssuesUrl = "https://github.com/hugomonizdorego/SDE-Desktop/issues";
const char *const kDonateUrl = "https://paypal.me/hugocenturion0311";
const char *const kDocsUrl = "https://github.com/hugomonizdorego/SDE-Desktop/tree/main/docs";

bool show_at_startup(const Config &cfg) { return cfg.get_bool("welcome", "show", true); }

void set_show_at_startup(Config &cfg, bool show) { cfg.set("welcome", "show", show ? "true" : "false"); }

bool ufw_conf_enabled(const std::string &ufw_conf) {
  std::istringstream in(ufw_conf);
  std::string line;
  bool enabled = false;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (starts_with(line, "ENABLED=")) {
      std::string v = lower(trim(line.substr(8)));
      if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
      enabled = v == "yes";
    }
  }
  return enabled;
}

std::string parse_ufw_status(const std::string &output) {
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    line = lower(trim(line));
    if (starts_with(line, "status:")) {
      std::string v = trim(line.substr(7));
      if (v == "active" || v == "inactive") return v;
    }
  }
  return "";
}

const std::vector<std::string> &codec_packages() {
  static const std::vector<std::string> list = {"gstreamer1.0-plugins-good", "gstreamer1.0-plugins-bad",
                                                "gstreamer1.0-plugins-ugly", "gstreamer1.0-libav"};
  return list;
}

std::vector<std::string> missing_packages(const std::vector<std::string> &wanted,
                                          const std::vector<std::string> &installed) {
  std::vector<std::string> out;
  for (const auto &w : wanted)
    if (std::find(installed.begin(), installed.end(), w) == installed.end()) out.push_back(w);
  return out;
}

namespace {
Tool first_installed(const std::vector<Tool> &candidates) {
  for (const auto &t : candidates)
    if (!t.argv.empty() && !find_program(t.argv[0]).empty()) return t;
  return Tool();
}
}  // namespace

std::vector<Tool> software_centers() {
  std::vector<Tool> all = {
      {"GNOME Software", {"gnome-software"}},
      {"KDE Discover", {"plasma-discover"}},
      {"Synaptic", {"synaptic-pkexec"}},
      {"Synaptic", {"synaptic"}},
  };
  std::vector<Tool> out;
  for (auto &t : all)
    if (!find_program(t.argv[0]).empty()) {
      bool dup = std::any_of(out.begin(), out.end(), [&](const Tool &o) { return o.name == t.name; });
      if (!dup) out.push_back(t);
    }
  return out;
}

Tool snapshot_tool() {
  return first_installed({{"Timeshift", {"timeshift-launcher"}}, {"Timeshift", {"timeshift-gtk"}}});
}

Tool firewall_tool() { return first_installed({{"Firewall Configuration (gufw)", {"gufw"}}}); }

const std::vector<Layout> &panel_layouts() {
  static const std::vector<Layout> list = {
      {"full", N_("Traditional"), N_("A full-width panel along the edge, like most desktops.")},
      {"short", N_("Centered"), N_("A shorter panel in the middle of the edge.")},
      {"floating", N_("Floating"), N_("A rounded panel that floats above the edge.")},
  };
  return list;
}

}  // namespace welcome
}  // namespace sde
