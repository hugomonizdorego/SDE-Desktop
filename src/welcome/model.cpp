// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "model.h"

#include <algorithm>
#include <cctype>
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

bool show_at_startup(const Config &cfg) { return cfg.get_bool("welcome", "show", false); }

void set_show_at_startup(Config &cfg, bool show) { cfg.set("welcome", "show", show ? "true" : "false"); }

// ---- live session

namespace {

std::vector<std::string> words_of(const std::string &s) {
  std::istringstream in(s);
  std::vector<std::string> out;
  std::string w;
  while (in >> w) out.push_back(w);
  return out;
}

struct Mount {
  std::string source, target, type, options;
};

// /proc/mounts escapes spaces and tabs as \040 and \011.
std::string unescape_mount(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
      out += char(std::stoi(s.substr(i + 1, 3), nullptr, 8));
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::vector<Mount> parse_mounts(const std::string &text) {
  std::vector<Mount> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    auto w = words_of(line);
    if (w.size() < 4) continue;
    out.push_back({unescape_mount(w[0]), unescape_mount(w[1]), w[2], w[3]});
  }
  return out;
}

// Value of "key=value" in a comma separated option list ("" when missing).
std::string mount_option(const std::string &options, const std::string &key) {
  for (auto &o : split(options, ','))
    if (starts_with(o, key + "=")) return o.substr(key.size() + 1);
  return "";
}

bool under(const std::string &path, const std::string &dir) {
  if (dir == "/") return true;
  return path == dir || starts_with(path, dir + "/");
}

std::string root_reason(const std::string &mounts_text) {
  auto mounts = parse_mounts(mounts_text);
  const Mount *root = nullptr;
  for (const auto &m : mounts)
    if (m.target == "/") root = &m;  // the last mount on / is the visible one
  if (!root) return "";
  std::vector<const Mount *> squash;
  for (const auto &m : mounts)
    if (m.type == "squashfs") squash.push_back(&m);
  if (root->type == "squashfs") return "root file system: squashfs";
  if (root->type == "overlay") {
    // lowerdir=/run/live/rootfs/filesystem.squashfs/ (several joined by ':').
    for (auto &dir : split(mount_option(root->options, "lowerdir"), ':')) {
      std::string d = dir;
      while (d.size() > 1 && d.back() == '/') d.pop_back();
      for (const Mount *s : squash)
        if (under(d, s->target)) return "root file system: overlay on squashfs (" + s->target + ")";
    }
    return "";
  }
  if ((root->type == "tmpfs" || root->type == "rootfs") && !squash.empty())
    return "root file system: " + root->type + " with a squashfs image";
  return "";
}

std::string live_path(const std::string &path) { return env("SDE_LIVE_ROOT") + path; }

}  // namespace

LiveProbe probe_live_session() {
  LiveProbe p;
  read_file(env("SDE_PROC_CMDLINE", "/proc/cmdline"), p.cmdline);
  read_file(env("SDE_PROC_MOUNTS", "/proc/mounts"), p.mounts);
  p.run_live = dir_exists(live_path("/run/live"));
  p.live_medium = dir_exists(live_path("/lib/live/mount/medium")) || dir_exists(live_path("/run/live/medium"));
  std::string etc_live = live_path("/etc/live");
  for (auto &name : list_dir(etc_live))
    if (starts_with(name, "config")) p.live_config = true;
  p.user = user_name();
  std::string desktop = env("SDE_DESKTOP_DIR");
  if (desktop.empty()) desktop = env("SDE_PROC_CMDLINE").empty() ? user_dir("DESKTOP") : "";
  if (!desktop.empty()) {
    for (auto &name : list_dir(desktop)) {
      std::string n = lower(name);
      if (!ends_with(n, ".desktop")) continue;
      if (n.find("calamares") != std::string::npos || n.find("install-debian") != std::string::npos ||
          n.find("debian-installer") != std::string::npos || n.find("install-debion") != std::string::npos)
        p.installer_launcher = true;
    }
  }
  return p;
}

std::string live_reason(const LiveProbe &p) {
  auto words = words_of(p.cmdline);
  bool toram = false;
  std::string live_word;
  for (const auto &w : words) {
    if (w == "boot=live" || w == "boot=casper" || w == "rd.live.image" || starts_with(w, "live-media=") ||
        starts_with(w, "live-media-path=") || w == "casper") {
      if (live_word.empty()) live_word = w;
    }
    if (w == "toram" || starts_with(w, "toram=")) toram = true;
  }
  if (!live_word.empty()) return "kernel command line: " + live_word + (toram ? " toram" : "");
  if (p.run_live) return "/run/live exists";
  if (p.live_medium) return "live medium mounted";
  std::string root = root_reason(p.mounts);
  if (!root.empty()) return root;
  if ((p.user == "user" || p.user == "live") && p.live_config) return "live user with live-config";
  if (p.installer_launcher) return "installer launcher on the desktop";
  return "";
}

bool is_live_session(const LiveProbe &probe) { return !live_reason(probe).empty(); }

bool is_live_session() { return is_live_session(probe_live_session()); }

std::string done_stamp_path() { return join_path(config_home(), "sde/welcome-done"); }

bool welcome_done() {
  if (file_exists(done_stamp_path())) return true;
  std::string conf;
  if (!read_file(Config::user_path(), conf)) return false;
  for (auto &line : split(conf, '\n'))
    if (trim(line) == "[welcome]") return true;
  return false;
}

bool mark_welcome_done() {
  std::string path = done_stamp_path();
  if (file_exists(path)) return true;
  make_dirs(join_path(config_home(), "sde"));
  return write_file_atomic(path, "SDE Welcome was shown to this user. Delete this file to see it at the next login.\n");
}

bool should_autostart(bool live, bool done, bool show_setting) {
  if (live) return false;
  if (!done) return true;
  return show_setting;
}

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
