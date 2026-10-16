// SPDX-License-Identifier: GPL-3.0-or-later
// SDE configuration: a small INI file read in three layers.
//
//   1. built-in defaults (this file's .cpp)
//   2. system defaults, $XDG_CONFIG_DIRS/sde/sde.conf (e.g. /etc/xdg/sde/sde.conf)
//   3. the user's file, ~/.config/sde/sde.conf
//
// Only layer 3 is ever written, so distributions can ship their own defaults
// and users can always return to them by deleting a key.
#pragma once

#include <map>
#include <string>

namespace sde {

class Config {
 public:
  void load();
  bool save() const;

  std::string get(const std::string &section, const std::string &key,
                  const std::string &fallback = "") const;
  int get_int(const std::string &section, const std::string &key, int fallback) const;
  bool get_bool(const std::string &section, const std::string &key, bool fallback) const;

  void set(const std::string &section, const std::string &key, const std::string &value);
  void unset(const std::string &section, const std::string &key);

  static std::string user_path();

 private:
  using Map = std::map<std::string, std::string>;  // "section/key" -> value
  static void parse(const std::string &text, Map &into);
  Map system_;  // built-in and system defaults
  Map merged_;
  Map user_;
};

}  // namespace sde
