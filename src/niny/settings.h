// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny settings (~/.config/sde/niny.conf), session and recent files
// (~/.local/state/niny).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace niny {

// A small INI document: [section] key=value, '#' and ';' comments.
class Ini {
 public:
  void parse(const std::string &text);
  std::string serialize() const;
  std::string get(const std::string &section, const std::string &key, const std::string &fallback = "") const;
  int get_int(const std::string &section, const std::string &key, int fallback, int lo, int hi) const;
  bool get_bool(const std::string &section, const std::string &key, bool fallback) const;
  void set(const std::string &section, const std::string &key, const std::string &value);
  bool has(const std::string &section, const std::string &key) const;
  const std::map<std::string, std::map<std::string, std::string>> &data() const { return data_; }

 private:
  std::map<std::string, std::map<std::string, std::string>> data_;
};

struct Settings {
  // Editor
  std::string font = "Monospace";
  int font_size = 11;  // points, before zoom
  int zoom = 0;        // pixel steps from Ctrl +/-
  int tab_width = 4;
  bool insert_spaces = true;
  bool detect_indent = true;
  bool auto_indent = true;
  bool auto_close = true;
  bool word_wrap = false;
  int right_margin = 0;  // column of the margin line; 0 hides it
  // View
  bool line_numbers = true;
  bool highlight_line = true;
  bool bracket_match = true;
  bool show_sidebar = true;
  int sidebar_width = 230;
  bool status_bar = true;
  std::string color_scheme = "auto";  // auto, light, dark
  // Files
  bool restore_session = true;
  bool backups = true;
  bool hot_exit = true;  // keep unsaved changes for the next start instead of asking
  bool watch_files = true;
  bool trim_trailing = false;
  bool final_newline = false;
  std::string default_encoding = "UTF-8";
  std::string default_eol = "LF";

  void from_ini(const Ini &ini);
  void to_ini(Ini &ini) const;
  bool load(const std::string &path);
  bool save(const std::string &path) const;
};

struct SessionEntry {
  std::string path;    // may be empty for an untitled document with a backup
  std::string backup;  // backup id holding unsaved text, if any
  int cursor = 0;
  int top_line = 1;
  std::string language;  // manual language choice, if any
};

struct Session {
  std::vector<SessionEntry> entries;
  int active = 0;
  std::string folder;  // folder shown in the file browser
  std::string serialize() const;
  void parse(const std::string &text);
};

// Most recent first, unique, at most |max| entries.
void push_recent(std::vector<std::string> &recent, const std::string &path, size_t max = 20);
std::string serialize_list(const std::vector<std::string> &items);
std::vector<std::string> parse_list(const std::string &text);

std::string settings_path();   // ~/.config/sde/niny.conf
std::string state_dir();       // ~/.local/state/niny
std::string backup_dir();      // ~/.local/state/niny/backups

}  // namespace niny
