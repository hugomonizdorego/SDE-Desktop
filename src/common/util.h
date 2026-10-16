// SPDX-License-Identifier: GPL-3.0-or-later
// Small string, file and XDG base directory helpers shared by all SDE programs.
#pragma once

#include <string>
#include <vector>

namespace sde {

std::string trim(const std::string &s);
std::string lower(std::string s);
std::vector<std::string> split(const std::string &s, char sep, bool keep_empty = false);
bool starts_with(const std::string &s, const std::string &prefix);
bool ends_with(const std::string &s, const std::string &suffix);
std::string join_path(const std::string &a, const std::string &b);
std::string basename_of(const std::string &path);

std::string env(const char *name, const std::string &fallback = "");
std::string home_dir();
std::string user_name();

// XDG Base Directory Specification.
std::string config_home();                 // ~/.config
std::string data_home();                   // ~/.local/share
std::string state_home();                  // ~/.local/state
std::string runtime_dir();                 // $XDG_RUNTIME_DIR or /tmp/sde-$UID
std::vector<std::string> data_dirs();      // data_home + $XDG_DATA_DIRS
std::vector<std::string> config_dirs();    // $XDG_CONFIG_DIRS (system only)

// XDG user directories (user-dirs.dirs), e.g. "DESKTOP" -> ~/Desktop.
std::string user_dir(const std::string &name);

bool file_exists(const std::string &path);
bool dir_exists(const std::string &path);
bool is_executable(const std::string &path);
bool read_file(const std::string &path, std::string &out);
bool write_file_atomic(const std::string &path, const std::string &content);
bool make_dirs(const std::string &path);
std::vector<std::string> list_dir(const std::string &path);

// Looks up a program in $PATH; returns the full path or "".
std::string find_program(const std::string &name);

// Human readable "key=value" from /etc/os-release, e.g. PRETTY_NAME.
std::string os_release(const std::string &key);

}  // namespace sde
