// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "pins.h"

#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <set>

#include "file_chooser_model.h"
#include "location.h"
#include "util.h"

namespace sde {
namespace liria {

namespace {

std::string take(gchar *s) {
  std::string out = s ? s : "";
  g_free(s);
  return out;
}

}  // namespace

std::string Pins::file_path() { return config_home() + "/sde/liria-quick-access"; }

std::vector<Pin> Pins::defaults() {
  std::vector<Pin> out;
  std::string home = home_dir();
  for (const char *key : {"DESKTOP", "DOWNLOAD", "DOCUMENTS", "PICTURES", "MUSIC", "VIDEOS"}) {
    std::string dir = chooser::normalize_path(user_dir(key));
    if (dir.empty() || dir == home || !dir_exists(dir)) continue;
    std::string uri = canonical(dir);
    if (std::none_of(out.begin(), out.end(), [&](const Pin &p) { return p.uri == uri; })) out.push_back({uri, ""});
  }
  return out;
}

std::string Pins::canonical(const std::string &in) {
  std::string s = trim(in);
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
  if (s.empty()) return "";
  if (s[0] == '~' && (s.size() == 1 || s[1] == '/')) s = home_dir() + s.substr(1);
  if (s[0] == '/') return path_to_uri(chooser::normalize_path(s));
  gchar *scheme = g_uri_parse_scheme(s.c_str());
  if (!scheme) return "";
  std::string sch = take(scheme);
  if (sch == "liria") return "";  // the Home page itself is no pin
  if (sch == "trash" || sch == "network" || sch == "recent") return sch + ":///";
  GFile *file = g_file_new_for_uri(s.c_str());
  std::string uri = take(g_file_get_uri(file));
  g_object_unref(file);
  if (uri.empty()) return "";
  // smb://nas/docs/ and smb://nas/docs are one place.
  if (sch != "file" && uri.size() > sch.size() + 3 && uri.back() != '/') uri += "/";
  return uri;
}

std::vector<Pin> Pins::parse(const std::string &text) {
  std::vector<Pin> out;
  std::set<std::string> seen;
  for (auto line : split(text, '\n', true)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t start = line.find_first_not_of(' ');
    if (start == std::string::npos) continue;
    line = line.substr(start);
    if (line.empty() || line[0] == '#') continue;
    std::string label;
    size_t tab = line.find('\t');
    if (tab != std::string::npos) {
      label = trim(line.substr(tab + 1));
      line = line.substr(0, tab);
    }
    std::string uri = canonical(line);
    if (uri.empty() || !seen.insert(uri).second) continue;
    if (!g_utf8_validate(label.c_str(), -1, nullptr)) label.clear();
    out.push_back({uri, label});
  }
  return out;
}

std::string Pins::format(const std::vector<Pin> &pins) {
  std::string out = "# Liria Quick access: one location per line, in card order (TAB, then a label of your own)\n";
  for (const auto &p : pins) {
    out += p.uri;
    if (!p.label.empty()) out += "\t" + p.label;
    out += "\n";
  }
  return out;
}

std::string Pins::file_stamp(const std::string &path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return "-";
  return std::to_string((long long)st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec) + ":" +
         std::to_string((long long)st.st_size) + ":" + std::to_string((unsigned long long)st.st_ino);
}

void Pins::load(const std::string &path) {
  path_ = path.empty() ? file_path() : path;
  stamp_ = file_stamp(path_);
  std::string text;
  custom_ = read_file(path_, text);
  items_ = custom_ ? parse(text) : defaults();
}

bool Pins::save() const {
  if (path_.empty()) return false;
  make_dirs(path_.substr(0, path_.rfind('/')));
  bool ok = write_file_atomic(path_, format(items_));
  const_cast<Pins *>(this)->stamp_ = file_stamp(path_);
  const_cast<Pins *>(this)->custom_ = ok || custom_;
  return ok;
}

int Pins::find(const std::string &uri_or_path) const {
  std::string uri = canonical(uri_or_path);
  if (uri.empty()) return -1;
  for (size_t i = 0; i < items_.size(); ++i)
    if (items_[i].uri == uri) return int(i);
  return -1;
}

bool Pins::add(const std::string &uri_or_path, int at) {
  std::string uri = canonical(uri_or_path);
  if (uri.empty() || find(uri) >= 0) return false;
  if (at < 0 || at > int(items_.size())) at = int(items_.size());
  items_.insert(items_.begin() + at, Pin{uri, ""});
  save();
  return true;
}

bool Pins::remove(const std::string &uri_or_path) {
  int i = find(uri_or_path);
  if (i < 0) return false;
  items_.erase(items_.begin() + i);
  save();
  return true;
}

bool Pins::move(int from, int to) {
  int n = int(items_.size());
  if (from < 0 || from >= n) return false;
  to = std::max(0, std::min(to, n - 1));
  if (from == to) return false;
  Pin p = items_[size_t(from)];
  items_.erase(items_.begin() + from);
  items_.insert(items_.begin() + to, p);
  save();
  return true;
}

bool Pins::rename(int index, const std::string &label) {
  if (index < 0 || index >= int(items_.size())) return false;
  std::string l = trim(label);
  if (items_[size_t(index)].label == l) return false;
  items_[size_t(index)].label = l;
  save();
  return true;
}

bool Pins::reset() {
  if (!path_.empty()) unlink(path_.c_str());
  custom_ = false;
  stamp_ = file_stamp(path_);
  std::vector<Pin> d = defaults();
  bool changed = !(d == items_);
  items_ = d;
  return changed;
}

}  // namespace liria
}  // namespace sde
