// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "library.h"

#include <dirent.h>
#include <glib.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <functional>

#include "util.h"

namespace gallery {

std::string month_key(time_t t) {
  struct tm tm {};
  localtime_r(&t, &tm);
  char buf[32];
  snprintf(buf, sizeof buf, "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
  return buf;
}

std::string month_label(time_t t) {
  struct tm tm {};
  localtime_r(&t, &tm);
  char buf[64];
  if (!strftime(buf, sizeof buf, "%B %Y", &tm)) return month_key(t);
  return buf;
}

static std::string fold(const std::string &s) {
  // ASCII case folding is enough for sorting and searching file names; UTF-8
  // bytes above 0x7f are compared as they are.
  std::string out = s;
  for (auto &c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Natural order: "img2" before "img10".
static bool natural_less(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    unsigned char ca = a[i], cb = b[j];
    if (std::isdigit(ca) && std::isdigit(cb)) {
      size_t i2 = i, j2 = j;
      while (i2 < a.size() && a[i2] == '0') ++i2;
      while (j2 < b.size() && b[j2] == '0') ++j2;
      size_t ie = i2, je = j2;
      while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
      while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
      if (ie - i2 != je - j2) return ie - i2 < je - j2;
      int c = a.compare(i2, ie - i2, b, j2, je - j2);
      if (c) return c < 0;
      i = ie;
      j = je;
      continue;
    }
    ca = std::tolower(ca);
    cb = std::tolower(cb);
    if (ca != cb) return ca < cb;
    ++i;
    ++j;
  }
  return a.size() - i < b.size() - j;
}

void sort_photos(std::vector<Photo> &photos, Sort sort) {
  auto by_name = [](const Photo &a, const Photo &b) {
    if (a.name != b.name) return natural_less(a.name, b.name);
    return a.path < b.path;
  };
  switch (sort) {
    case Sort::Newest:
      std::stable_sort(photos.begin(), photos.end(), [&](const Photo &a, const Photo &b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return by_name(a, b);
      });
      break;
    case Sort::Oldest:
      std::stable_sort(photos.begin(), photos.end(), [&](const Photo &a, const Photo &b) {
        if (a.mtime != b.mtime) return a.mtime < b.mtime;
        return by_name(a, b);
      });
      break;
    case Sort::Name:
      std::stable_sort(photos.begin(), photos.end(), by_name);
      break;
  }
}

std::vector<Group> group_photos(const std::vector<Photo> &photos, Sort sort) {
  std::vector<Group> groups;
  if (photos.empty()) return groups;
  if (sort == Sort::Name) {
    groups.push_back(Group{"", "", 0, photos.size()});
    return groups;
  }
  for (size_t i = 0; i < photos.size(); ++i) {
    std::string key = month_key(photos[i].mtime);
    if (groups.empty() || groups.back().key != key)
      groups.push_back(Group{key, month_label(photos[i].mtime), i, 0});
    ++groups.back().count;
  }
  return groups;
}

bool matches_search(const std::string &name, const std::string &query) {
  std::string q = sde::trim(query);
  if (q.empty()) return true;
  return fold(name).find(fold(q)) != std::string::npos;
}

std::string extension_of(const std::string &name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return "";
  return fold(name.substr(dot + 1));
}

bool is_image_name(const std::string &name, const std::set<std::string> &extra) {
  static const std::set<std::string> base{"jpg", "jpeg", "jpe", "jfif", "png", "gif",
                                          "bmp", "svg",  "svgz"};
  if (name.empty() || name[0] == '.') return false;
  std::string ext = extension_of(name);
  return base.count(ext) || extra.count(ext);
}

std::string cache_home() {
  std::string c = sde::env("XDG_CACHE_HOME");
  if (!c.empty() && c[0] == '/') return c;
  return sde::home_dir() + "/.cache";
}

std::string file_uri(const std::string &path) {
  // g_filename_to_uri escapes exactly like GIO, which the other thumbnailers
  // use, so cached thumbnails are shared with them.
  gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  if (!uri) return "file://" + path;
  std::string out(uri);
  g_free(uri);
  return out;
}

std::string md5_hex(const std::string &data) {
  gchar *sum = g_compute_checksum_for_data(G_CHECKSUM_MD5,
                                           reinterpret_cast<const guchar *>(data.data()), data.size());
  std::string out(sum ? sum : "");
  g_free(sum);
  return out;
}

std::string thumbnail_path(const std::string &path, bool large, const std::string &cache) {
  return cache + "/thumbnails/" + (large ? "large" : "normal") + "/" + md5_hex(file_uri(path)) +
         ".png";
}

bool in_thumbnail_cache(const std::string &path, const std::string &cache) {
  return sde::starts_with(path, cache + "/thumbnails/");
}

std::string encode_list(const std::vector<std::string> &items) {
  std::string out;
  for (const auto &item : items) {
    if (item.empty()) continue;
    if (!out.empty()) out += ';';
    for (char c : item) {
      if (c == '%') out += "%25";
      else if (c == ';') out += "%3B";
      else if (c == '#') out += "%23";
      else if (c == '\n') out += "%0A";
      else out += c;
    }
  }
  return out;
}

std::vector<std::string> decode_list(const std::string &value) {
  std::vector<std::string> out;
  for (const auto &part : sde::split(value, ';')) {
    std::string item;
    for (size_t i = 0; i < part.size(); ++i) {
      if (part[i] == '%' && i + 2 < part.size()) {
        int v = 0;
        bool ok = true;
        for (int k = 1; k <= 2; ++k) {
          char h = part[i + size_t(k)];
          v <<= 4;
          if (h >= '0' && h <= '9') v |= h - '0';
          else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
          else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
          else ok = false;
        }
        if (ok) {
          item += char(v);
          i += 2;
          continue;
        }
      }
      item += part[i];
    }
    item = sde::trim(item);
    if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
  }
  return out;
}

std::string pictures_dir() {
  std::string home = sde::home_dir();
  std::string dir = sde::user_dir("PICTURES");
  if (dir.empty() || dir == home || dir == home + "/") dir = home + "/Pictures";
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  return dir;
}

std::vector<Photo> scan_folders(const std::vector<std::string> &roots,
                                const std::set<std::string> &extra_formats, size_t limit) {
  std::vector<Photo> out;
  std::set<std::pair<dev_t, ino_t>> seen_dirs;
  std::set<std::string> seen_files;
  std::string cache = cache_home() + "/thumbnails";
  std::function<void(const std::string &, int)> walk = [&](const std::string &dir, int depth) {
    if (depth > 12 || out.size() >= limit || dir == cache) return;
    struct stat st {};
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
    if (!seen_dirs.insert({st.st_dev, st.st_ino}).second) return;  // loops, overlapping roots
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    while (struct dirent *e = readdir(d)) {
      std::string name = e->d_name;
      if (name.empty() || name[0] == '.') continue;
      std::string full = dir + "/" + name;
      struct stat fs {};
      if (stat(full.c_str(), &fs) != 0) continue;
      if (S_ISDIR(fs.st_mode)) {
        subdirs.push_back(full);
      } else if (S_ISREG(fs.st_mode) && is_image_name(name, extra_formats) && fs.st_size > 0) {
        if (!seen_files.insert(full).second) continue;
        Photo p;
        p.path = full;
        p.name = name;
        p.dir = dir;
        p.mtime = fs.st_mtime;
        p.size = fs.st_size;
        out.push_back(std::move(p));
        if (out.size() >= limit) break;
      }
    }
    closedir(d);
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto &s : subdirs) walk(s, depth + 1);
  };
  for (auto root : roots) {
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    walk(root, 0);
  }
  return out;
}

std::string human_size(long long bytes) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = double(bytes);
  int u = 0;
  while (v >= 1000 && u < 4) {
    v /= 1000;
    ++u;
  }
  char buf[32];
  if (u == 0) snprintf(buf, sizeof buf, "%lld B", bytes);
  else snprintf(buf, sizeof buf, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
  return buf;
}

}  // namespace gallery
