// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "codec.h"
#include "util.h"

namespace niny {

namespace {

std::string strip(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

// Values are one line; newlines and backslashes are escaped.
std::string escape(const std::string &v) {
  std::string o;
  for (char c : v) {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}
std::string unescape(const std::string &v) {
  std::string o;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char d = v[++i];
      o += d == 'n' ? '\n' : d;
    } else {
      o += v[i];
    }
  }
  return o;
}

}  // namespace

void Ini::parse(const std::string &text) {
  data_.clear();
  std::string section;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::string t = strip(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    if (t[0] == '[') {
      size_t e = t.find(']');
      section = strip(t.substr(1, e == std::string::npos ? std::string::npos : e - 1));
      continue;
    }
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    data_[section][strip(t.substr(0, eq))] = unescape(strip(t.substr(eq + 1)));
  }
}

std::string Ini::serialize() const {
  std::string out = "# Niny Text Editor settings\n";
  for (auto &sec : data_) {
    out += "\n[" + sec.first + "]\n";
    for (auto &kv : sec.second) out += kv.first + "=" + escape(kv.second) + "\n";
  }
  return out;
}

std::string Ini::get(const std::string &section, const std::string &key, const std::string &fallback) const {
  auto s = data_.find(section);
  if (s == data_.end()) return fallback;
  auto k = s->second.find(key);
  return k == s->second.end() ? fallback : k->second;
}

bool Ini::has(const std::string &section, const std::string &key) const {
  auto s = data_.find(section);
  return s != data_.end() && s->second.count(key);
}

int Ini::get_int(const std::string &section, const std::string &key, int fallback, int lo, int hi) const {
  std::string v = get(section, key);
  if (v.empty()) return fallback;
  char *end = nullptr;
  long n = strtol(v.c_str(), &end, 10);
  if (!end || *end) return fallback;
  return int(std::max<long>(lo, std::min<long>(hi, n)));
}

bool Ini::get_bool(const std::string &section, const std::string &key, bool fallback) const {
  std::string v = sde::lower(get(section, key));
  if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
  if (v == "false" || v == "0" || v == "no" || v == "off") return false;
  return fallback;
}

void Ini::set(const std::string &section, const std::string &key, const std::string &value) {
  data_[section][key] = value;
}

void Settings::from_ini(const Ini &ini) {
  Settings d;
  font = ini.get("editor", "font", d.font);
  if (font.empty()) font = d.font;
  font_size = ini.get_int("editor", "font_size", d.font_size, 6, 48);
  zoom = ini.get_int("editor", "zoom", d.zoom, -8, 40);
  tab_width = ini.get_int("editor", "tab_width", d.tab_width, 1, 16);
  insert_spaces = ini.get_bool("editor", "insert_spaces", d.insert_spaces);
  detect_indent = ini.get_bool("editor", "detect_indent", d.detect_indent);
  auto_indent = ini.get_bool("editor", "auto_indent", d.auto_indent);
  auto_close = ini.get_bool("editor", "auto_close_brackets", d.auto_close);
  word_wrap = ini.get_bool("editor", "word_wrap", d.word_wrap);
  right_margin = ini.get_int("editor", "right_margin", d.right_margin, 0, 400);
  line_numbers = ini.get_bool("view", "line_numbers", d.line_numbers);
  highlight_line = ini.get_bool("view", "highlight_current_line", d.highlight_line);
  bracket_match = ini.get_bool("view", "bracket_matching", d.bracket_match);
  show_sidebar = ini.get_bool("view", "side_panel", d.show_sidebar);
  sidebar_width = ini.get_int("view", "side_panel_width", d.sidebar_width, 140, 600);
  status_bar = ini.get_bool("view", "status_bar", d.status_bar);
  color_scheme = sde::lower(ini.get("view", "color_scheme", d.color_scheme));
  if (color_scheme != "light" && color_scheme != "dark") color_scheme = "auto";
  restore_session = ini.get_bool("files", "restore_session", d.restore_session);
  backups = ini.get_bool("files", "backups", d.backups);
  hot_exit = ini.get_bool("files", "keep_unsaved_on_quit", d.hot_exit);
  watch_files = ini.get_bool("files", "detect_external_changes", d.watch_files);
  trim_trailing = ini.get_bool("files", "trim_trailing_whitespace", d.trim_trailing);
  final_newline = ini.get_bool("files", "ensure_final_newline", d.final_newline);
  Encoding e;
  default_encoding = ini.get("files", "default_encoding", d.default_encoding);
  if (!encoding_from_name(default_encoding, e)) default_encoding = d.default_encoding;
  else default_encoding = encoding_name(e);
  Eol eol;
  default_eol = ini.get("files", "default_line_ending", d.default_eol);
  if (!eol_from_name(default_eol, eol)) default_eol = d.default_eol;
}

void Settings::to_ini(Ini &ini) const {
  auto b = [](bool v) { return std::string(v ? "true" : "false"); };
  ini.set("editor", "font", font);
  ini.set("editor", "font_size", std::to_string(font_size));
  ini.set("editor", "zoom", std::to_string(zoom));
  ini.set("editor", "tab_width", std::to_string(tab_width));
  ini.set("editor", "insert_spaces", b(insert_spaces));
  ini.set("editor", "detect_indent", b(detect_indent));
  ini.set("editor", "auto_indent", b(auto_indent));
  ini.set("editor", "auto_close_brackets", b(auto_close));
  ini.set("editor", "word_wrap", b(word_wrap));
  ini.set("editor", "right_margin", std::to_string(right_margin));
  ini.set("view", "line_numbers", b(line_numbers));
  ini.set("view", "highlight_current_line", b(highlight_line));
  ini.set("view", "bracket_matching", b(bracket_match));
  ini.set("view", "side_panel", b(show_sidebar));
  ini.set("view", "side_panel_width", std::to_string(sidebar_width));
  ini.set("view", "status_bar", b(status_bar));
  ini.set("view", "color_scheme", color_scheme);
  ini.set("files", "restore_session", b(restore_session));
  ini.set("files", "backups", b(backups));
  ini.set("files", "keep_unsaved_on_quit", b(hot_exit));
  ini.set("files", "detect_external_changes", b(watch_files));
  ini.set("files", "trim_trailing_whitespace", b(trim_trailing));
  ini.set("files", "ensure_final_newline", b(final_newline));
  ini.set("files", "default_encoding", default_encoding);
  ini.set("files", "default_line_ending", default_eol);
}

bool Settings::load(const std::string &path) {
  std::string text;
  if (!sde::read_file(path, text)) return false;
  Ini ini;
  ini.parse(text);
  from_ini(ini);
  return true;
}

bool Settings::save(const std::string &path) const {
  Ini ini;
  // Keep keys written by newer versions.
  std::string old;
  if (sde::read_file(path, old)) ini.parse(old);
  to_ini(ini);
  size_t slash = path.rfind('/');
  if (slash != std::string::npos) sde::make_dirs(path.substr(0, slash));
  return sde::write_file_atomic(path, ini.serialize());
}

std::string Session::serialize() const {
  Ini ini;
  ini.set("session", "active", std::to_string(active));
  ini.set("session", "count", std::to_string(entries.size()));
  ini.set("session", "folder", folder);
  for (size_t i = 0; i < entries.size(); ++i) {
    std::string s = "doc" + std::to_string(i);
    const auto &e = entries[i];
    ini.set(s, "path", e.path);
    ini.set(s, "backup", e.backup);
    ini.set(s, "cursor", std::to_string(e.cursor));
    ini.set(s, "top_line", std::to_string(e.top_line));
    ini.set(s, "language", e.language);
  }
  return ini.serialize();
}

void Session::parse(const std::string &text) {
  Ini ini;
  ini.parse(text);
  entries.clear();
  int count = ini.get_int("session", "count", 0, 0, 1000);
  for (int i = 0; i < count; ++i) {
    std::string s = "doc" + std::to_string(i);
    SessionEntry e;
    e.path = ini.get(s, "path");
    e.backup = ini.get(s, "backup");
    e.cursor = ini.get_int(s, "cursor", 0, 0, 2147483647);
    e.top_line = ini.get_int(s, "top_line", 1, 1, 2147483647);
    e.language = ini.get(s, "language");
    if (!e.path.empty() || !e.backup.empty()) entries.push_back(e);
  }
  active = ini.get_int("session", "active", 0, 0, 1000);
  if (active >= int(entries.size())) active = entries.empty() ? 0 : int(entries.size()) - 1;
  folder = ini.get("session", "folder");
}

void push_recent(std::vector<std::string> &recent, const std::string &path, size_t max) {
  if (path.empty()) return;
  recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
  recent.insert(recent.begin(), path);
  if (recent.size() > max) recent.resize(max);
}

std::string serialize_list(const std::vector<std::string> &items) {
  std::string out;
  for (auto &i : items)
    if (i.find('\n') == std::string::npos) out += i + "\n";
  return out;
}

std::vector<std::string> parse_list(const std::string &text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line))
    if (!line.empty()) out.push_back(line);
  return out;
}

std::string settings_path() { return sde::config_home() + "/sde/niny.conf"; }
std::string state_dir() { return sde::state_home() + "/niny"; }
std::string backup_dir() { return state_dir() + "/backups"; }

}  // namespace niny
