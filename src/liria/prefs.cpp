// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "prefs.h"

#include <glib.h>

#include <algorithm>
#include <vector>

#include "util.h"

namespace sde {
namespace liria {

namespace {

bool get_bool(GKeyFile *k, const char *g, const char *key, bool fallback) {
  GError *e = nullptr;
  gboolean v = g_key_file_get_boolean(k, g, key, &e);
  if (e) {
    g_error_free(e);
    return fallback;
  }
  return v;
}

int get_int(GKeyFile *k, const char *g, const char *key, int fallback) {
  GError *e = nullptr;
  int v = g_key_file_get_integer(k, g, key, &e);
  if (e) {
    g_error_free(e);
    return fallback;
  }
  return v;
}

std::string get_str(GKeyFile *k, const char *g, const char *key, const std::string &fallback) {
  gchar *v = g_key_file_get_string(k, g, key, nullptr);
  if (!v) return fallback;
  std::string out = trim(v);
  g_free(v);
  return out;
}

const int kMaxViews = 500;

}  // namespace

std::string layout_id(Layout l) {
  return l == Layout::Details ? "details" : l == Layout::Compact ? "compact" : "icons";
}

Layout layout_from_id(const std::string &id) {
  if (id == "details" || id == "list") return Layout::Details;
  if (id == "compact") return Layout::Compact;
  return Layout::Icons;
}

int Prefs::clamp_icon_size(int px) { return std::max(32, std::min(256, px)); }

std::string Prefs::file_path() { return config_home() + "/sde/liria.conf"; }
std::string Prefs::views_path() { return state_home() + "/sde/liria-views"; }

void Prefs::load(const std::string &path, const std::string &views) {
  path_ = path.empty() ? file_path() : path;
  views_path_ = views.empty() ? views_path() : views;
  GKeyFile *k = g_key_file_new();
  if (g_key_file_load_from_file(k, path_.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr)) {
    layout = layout_from_id(get_str(k, "view", "layout", layout_id(layout)));
    icon_size = clamp_icon_size(get_int(k, "view", "icon_size", icon_size));
    show_hidden = get_bool(k, "view", "hidden", show_hidden);
    sort.key = chooser::sort_key_from_id(get_str(k, "view", "sort", chooser::sort_key_id(sort.key)));
    sort.descending = get_bool(k, "view", "descending", sort.descending);
    sort.folders_first = get_bool(k, "view", "folders_first", sort.folders_first);
    confirm_trash = get_bool(k, "view", "confirm_trash", confirm_trash);
    home_quick = get_bool(k, "home", "quick_access", home_quick);
    home_drives = get_bool(k, "home", "drives", home_drives);
    home_network = get_bool(k, "home", "network", home_network);
    home_recent = get_bool(k, "home", "recent", home_recent);
    network_shown = get_bool(k, "home", "network_shown", network_shown);
    recent_shown = get_bool(k, "home", "recent_shown", recent_shown);
    sidebar_shown = get_bool(k, "sidebar", "shown", sidebar_shown);
    sidebar_width = std::max(150, std::min(480, get_int(k, "sidebar", "width", sidebar_width)));
    side_pinned = get_bool(k, "sidebar", "pinned", side_pinned);
    side_drives = get_bool(k, "sidebar", "drives", side_drives);
    side_network = get_bool(k, "sidebar", "network", side_network);
    width = std::max(0, get_int(k, "window", "width", width));
    height = std::max(0, get_int(k, "window", "height", height));
    maximized = get_bool(k, "window", "maximized", maximized);
  }
  g_key_file_free(k);
  views_.clear();
  order_ = 0;
  std::string text;
  if (read_file(views_path_, text)) {
    for (const auto &line : split(text, '\n')) {
      if (line.empty() || line[0] == '#') continue;
      std::vector<std::string> f = split(line, '\t', true);
      if (f.size() < 5 || f[0].empty() || f[0][0] != '/') continue;
      FolderView v;
      v.layout = layout_from_id(f[1]);
      v.sort.key = chooser::sort_key_from_id(f[2]);
      v.sort.descending = f[3] == "1";
      v.icon_size = atoi(f[4].c_str());
      if (v.icon_size) v.icon_size = clamp_icon_size(v.icon_size);
      views_[f[0]] = {++order_, v};
    }
  }
}

bool Prefs::save() const {
  GKeyFile *k = g_key_file_new();
  g_key_file_load_from_file(k, path_.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
  g_key_file_set_string(k, "view", "layout", layout_id(layout).c_str());
  g_key_file_set_integer(k, "view", "icon_size", icon_size);
  g_key_file_set_boolean(k, "view", "hidden", show_hidden);
  g_key_file_set_string(k, "view", "sort", chooser::sort_key_id(sort.key).c_str());
  g_key_file_set_boolean(k, "view", "descending", sort.descending);
  g_key_file_set_boolean(k, "view", "folders_first", sort.folders_first);
  g_key_file_set_boolean(k, "view", "confirm_trash", confirm_trash);
  g_key_file_set_boolean(k, "home", "quick_access", home_quick);
  g_key_file_set_boolean(k, "home", "drives", home_drives);
  g_key_file_set_boolean(k, "home", "network", home_network);
  g_key_file_set_boolean(k, "home", "recent", home_recent);
  g_key_file_set_boolean(k, "home", "network_shown", network_shown);
  g_key_file_set_boolean(k, "home", "recent_shown", recent_shown);
  g_key_file_set_boolean(k, "sidebar", "shown", sidebar_shown);
  g_key_file_set_integer(k, "sidebar", "width", sidebar_width);
  g_key_file_set_boolean(k, "sidebar", "pinned", side_pinned);
  g_key_file_set_boolean(k, "sidebar", "drives", side_drives);
  g_key_file_set_boolean(k, "sidebar", "network", side_network);
  g_key_file_set_integer(k, "window", "width", width);
  g_key_file_set_integer(k, "window", "height", height);
  g_key_file_set_boolean(k, "window", "maximized", maximized);
  gsize n = 0;
  gchar *data = g_key_file_to_data(k, &n, nullptr);
  std::string text = data ? std::string(data, n) : std::string();
  g_free(data);
  g_key_file_free(k);
  make_dirs(path_.substr(0, path_.rfind('/')));
  std::string old;
  if (read_file(path_, old) && old == text) return true;
  return write_file_atomic(path_, text);
}

FolderView Prefs::view_of(const std::string &folder) const {
  auto it = views_.find(folder);
  if (it != views_.end()) return it->second.second;
  FolderView v;
  v.layout = layout;
  v.sort = sort;
  v.icon_size = 0;
  return v;
}

void Prefs::set_view(const std::string &folder, const FolderView &v) {
  if (folder.empty() || folder[0] != '/') return;
  views_[folder] = {++order_, v};
  if (int(views_.size()) > kMaxViews) {
    // The folders seen longest ago go first.
    auto oldest = std::min_element(views_.begin(), views_.end(),
                                   [](const auto &a, const auto &b) { return a.second.first < b.second.first; });
    views_.erase(oldest);
  }
  save_views();
}

bool Prefs::save_views() const {
  std::vector<std::pair<long, std::string>> order;
  for (const auto &kv : views_) order.push_back({kv.second.first, kv.first});
  std::sort(order.begin(), order.end());
  std::string text = "# Liria: the layout and sorting of folders (folder, layout, sort, descending, icon size)\n";
  for (const auto &o : order) {
    const FolderView &v = views_.at(o.second).second;
    if (o.second.find('\t') != std::string::npos || o.second.find('\n') != std::string::npos) continue;
    text += o.second + "\t" + layout_id(v.layout) + "\t" + chooser::sort_key_id(v.sort.key) + "\t" +
            (v.sort.descending ? "1" : "0") + "\t" + std::to_string(v.icon_size) + "\n";
  }
  make_dirs(views_path_.substr(0, views_path_.rfind('/')));
  return write_file_atomic(views_path_, text);
}

}  // namespace liria
}  // namespace sde
