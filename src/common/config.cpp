// SPDX-License-Identifier: GPL-3.0-or-later
// C++ facade: parsing, configuration layers and atomic writes live in Rust.
#include "config.h"

#include "rust_config.h"
#include "util.h"

namespace sde {

Config::Config() : impl_(sde_config_new()) {}
Config::~Config() { sde_config_free(impl_); }
Config::Config(const Config &other) : impl_(sde_config_clone(other.impl_)) {}
Config &Config::operator=(const Config &other) {
  if (this != &other) {
    auto *next = sde_config_clone(other.impl_);
    sde_config_free(impl_);
    impl_ = next;
  }
  return *this;
}

std::string Config::user_path() { return config_home() + "/sde/sde.conf"; }

void Config::load() {
  std::string dirs;
  for (const auto &dir : config_dirs()) {
    if (!dirs.empty()) dirs += ':';
    dirs += dir;
  }
  sde_config_load(impl_, user_path().c_str(), dirs.c_str());
}

bool Config::save() const { return sde_config_save(impl_, user_path().c_str()); }

std::string Config::get(const std::string &section, const std::string &key,
                        const std::string &fallback) const {
  char *value = sde_config_get(impl_, section.c_str(), key.c_str(), fallback.c_str());
  std::string result(value);
  sde_config_string_free(value);
  return result;
}

int Config::get_int(const std::string &section, const std::string &key, int fallback) const {
  return sde_config_get_int(impl_, section.c_str(), key.c_str(), fallback);
}

bool Config::get_bool(const std::string &section, const std::string &key, bool fallback) const {
  return sde_config_get_bool(impl_, section.c_str(), key.c_str(), fallback);
}

void Config::set(const std::string &section, const std::string &key, const std::string &value) {
  sde_config_set(impl_, section.c_str(), key.c_str(), value.c_str());
}

void Config::unset(const std::string &section, const std::string &key) {
  sde_config_unset(impl_, section.c_str(), key.c_str());
}

}  // namespace sde
