// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <cstdlib>
#include <sstream>

#include "util.h"

namespace sde {

static const char *kBuiltinDefaults = R"(
[appearance]
theme=light
accent=#167D9A
icon_theme=auto
font=Sans
font_size=10
wallpaper=auto
wallpaper_mode=zoom

[panel]
height=40
clock_24h=true
show_date=true
show_battery=true
pinned=org.mozilla.firefox.desktop;firefox.desktop;firefox-esr.desktop;chromium.desktop;pcmanfm.desktop;org.gnome.Nautilus.desktop;thunar.desktop;nemo.desktop;sde-settings.desktop

[desktop]
show_icons=true
show_home=true
show_trash=false

[session]
window_manager=openbox
polkit_agent=auto
)";

void Config::parse(const std::string &text, Map &into) {
  std::istringstream in(text);
  std::string line, section;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos || section.empty()) continue;
    into[section + "/" + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
}

std::string Config::user_path() { return config_home() + "/sde/sde.conf"; }

void Config::load() {
  system_.clear();
  user_.clear();
  parse(kBuiltinDefaults, system_);
  // $XDG_CONFIG_DIRS is ordered by preference, so apply it in reverse.
  auto dirs = config_dirs();
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
    std::string text;
    if (read_file(*it + "/sde/sde.conf", text)) parse(text, system_);
  }
  std::string text;
  if (read_file(user_path(), text)) parse(text, user_);
  merged_ = system_;
  for (auto &kv : user_) merged_[kv.first] = kv.second;
}

bool Config::save() const {
  std::map<std::string, std::map<std::string, std::string>> sections;
  for (auto &kv : user_) {
    size_t slash = kv.first.find('/');
    sections[kv.first.substr(0, slash)][kv.first.substr(slash + 1)] = kv.second;
  }
  std::string out = "# Selviana Desktop Environment user settings.\n"
                    "# Written by sde-settings. Remove a line to return to the default.\n";
  for (auto &s : sections) {
    out += "\n[" + s.first + "]\n";
    for (auto &kv : s.second) out += kv.first + "=" + kv.second + "\n";
  }
  return write_file_atomic(user_path(), out);
}

std::string Config::get(const std::string &section, const std::string &key,
                        const std::string &fallback) const {
  auto it = merged_.find(section + "/" + key);
  return it == merged_.end() ? fallback : it->second;
}

int Config::get_int(const std::string &section, const std::string &key, int fallback) const {
  std::string v = get(section, key);
  if (v.empty()) return fallback;
  char *end = nullptr;
  long n = strtol(v.c_str(), &end, 10);
  return (end && *end == '\0') ? int(n) : fallback;
}

bool Config::get_bool(const std::string &section, const std::string &key, bool fallback) const {
  std::string v = lower(get(section, key));
  if (v == "true" || v == "yes" || v == "1" || v == "on") return true;
  if (v == "false" || v == "no" || v == "0" || v == "off") return false;
  return fallback;
}

void Config::set(const std::string &section, const std::string &key, const std::string &value) {
  user_[section + "/" + key] = value;
  merged_[section + "/" + key] = value;
}

void Config::unset(const std::string &section, const std::string &key) {
  std::string k = section + "/" + key;
  user_.erase(k);
  auto it = system_.find(k);
  if (it == system_.end())
    merged_.erase(k);
  else
    merged_[k] = it->second;
}

}  // namespace sde
