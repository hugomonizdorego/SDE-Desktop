// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "album.h"

#include <dirent.h>
#include <gio/gio.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "config.h"
#include "util.h"

namespace gallery {

namespace {

std::string strip_slash(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

}  // namespace

std::string album_id(const Album &a) {
  switch (a.kind) {
    case AlbumKind::All: return "all";
    case AlbumKind::Favorites: return "favorites";
    case AlbumKind::Recent: return "recent";
    case AlbumKind::Screenshots: return "screenshots";
    case AlbumKind::Folder: return "folder:" + a.path;
    case AlbumKind::Device: return "device:" + a.path;
    case AlbumKind::Directory: return "dir:" + a.path;
    case AlbumKind::List: return "list:" + a.path;
  }
  return "all";
}

bool parse_album(const std::string &id_in, Album &out) {
  std::string id = sde::trim(id_in);
  static const std::pair<const char *, AlbumKind> simple[] = {{"all", AlbumKind::All},
                                                             {"favorites", AlbumKind::Favorites},
                                                             {"recent", AlbumKind::Recent},
                                                             {"screenshots", AlbumKind::Screenshots}};
  for (const auto &s : simple)
    if (id == s.first) {
      out = Album{s.second, ""};
      return true;
    }
  static const std::pair<const char *, AlbumKind> prefixed[] = {{"folder:", AlbumKind::Folder},
                                                               {"device:", AlbumKind::Device},
                                                               {"dir:", AlbumKind::Directory},
                                                               {"list:", AlbumKind::List}};
  for (const auto &p : prefixed)
    if (sde::starts_with(id, p.first)) {
      std::string path = strip_slash(id.substr(strlen(p.first)));
      if (path.empty() || path[0] != '/') return false;
      out = Album{p.second, path};
      return true;
    }
  if (!id.empty() && id[0] == '/') {
    out = Album{AlbumKind::Folder, strip_slash(id)};
    return true;
  }
  return false;
}

std::string sort_name(Sort s) {
  return s == Sort::Oldest ? "oldest" : s == Sort::Name ? "name" : "newest";
}

bool parse_sort(const std::string &name, Sort &out) {
  if (name == "newest") out = Sort::Newest;
  else if (name == "oldest") out = Sort::Oldest;
  else if (name == "name") out = Sort::Name;
  else return false;
  return true;
}

LibrarySettings LibrarySettings::load(const sde::Config &cfg) {
  LibrarySettings s;
  s.pictures = pictures_dir();
  for (auto &f : decode_list(cfg.get("gallery", "folders", ""))) {
    f = strip_slash(f);
    if (f != s.pictures && !f.empty() && f[0] == '/') s.user_folders.push_back(f);
  }
  for (auto &f : decode_list(cfg.get("gallery", "favorites", ""))) s.favorites.insert(f);
  parse_sort(cfg.get("gallery", "sort", "newest"), s.sort);
  return s;
}

std::vector<std::string> LibrarySettings::roots() const {
  std::vector<std::string> r{pictures};
  for (const auto &f : user_folders) r.push_back(f);
  return r;
}

bool path_under(const std::string &path, const std::string &dir) {
  if (dir == "/") return path.size() > 1 && path[0] == '/';
  return path.size() > dir.size() + 1 && sde::starts_with(path, dir) && path[dir.size()] == '/';
}

std::vector<std::string> album_roots(const Album &a, const LibrarySettings &s, bool *recursive) {
  if (recursive) *recursive = a.kind != AlbumKind::Directory;
  switch (a.kind) {
    case AlbumKind::All:
    case AlbumKind::Favorites:
    case AlbumKind::Recent: return s.roots();
    case AlbumKind::Screenshots: return {s.screenshots()};
    case AlbumKind::Folder:
    case AlbumKind::Device:
    case AlbumKind::Directory: return {a.path};
    case AlbumKind::List: return {};
  }
  return {};
}

bool album_contains(const Album &a, const Photo &p, const LibrarySettings &s, time_t now) {
  switch (a.kind) {
    case AlbumKind::All: return true;
    case AlbumKind::Favorites: return s.favorites.count(p.path) != 0;
    case AlbumKind::Recent: return p.mtime >= now - time_t(s.recent_days) * 24 * 3600;
    case AlbumKind::Screenshots: return path_under(p.path, s.screenshots());
    case AlbumKind::Folder:
    case AlbumKind::Device: return path_under(p.path, a.path);
    case AlbumKind::Directory: return p.dir == a.path;
    case AlbumKind::List: return true;
  }
  return true;
}

std::vector<Photo> album_photos(const std::vector<Photo> &photos, const Album &a,
                                const LibrarySettings &s, Sort sort, time_t now,
                                const std::string &query) {
  std::vector<Photo> out;
  for (const auto &p : photos)
    if (album_contains(a, p, s, now) && matches_search(p.name, query)) out.push_back(p);
  // A list keeps its own order.
  if (a.kind != AlbumKind::List) sort_photos(out, sort);
  return out;
}

std::vector<Photo> scan_directory(const std::string &dir_in, const std::set<std::string> &extra) {
  std::vector<Photo> out;
  std::string dir = strip_slash(dir_in);
  DIR *d = opendir(dir.c_str());
  if (!d) return out;
  while (struct dirent *e = readdir(d)) {
    std::string name = e->d_name;
    if (!is_image_name(name, extra)) continue;
    std::string full = dir == "/" ? "/" + name : dir + "/" + name;
    struct stat st {};
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
    Photo p;
    p.path = full;
    p.name = name;
    p.dir = dir;
    p.mtime = st.st_mtime;
    p.size = st.st_size;
    out.push_back(std::move(p));
  }
  closedir(d);
  return out;
}

std::vector<std::string> read_album_list(const std::string &file) {
  std::string text;
  std::vector<std::string> out;
  if (!sde::read_file(file, text)) return out;
  for (auto &line : sde::split(text, '\n'))
    if (!line.empty() && line[0] == '/') out.push_back(line);
  return out;
}

bool write_album_list(const std::string &file, const std::vector<std::string> &paths) {
  std::string text;
  for (const auto &p : paths)
    if (p.find('\n') == std::string::npos) text += p + "\n";
  std::string dir = file.substr(0, file.rfind('/'));
  if (!dir.empty()) sde::make_dirs(dir);
  return sde::write_file_atomic(file, text);
}

std::vector<Photo> photos_for_paths(const std::vector<std::string> &paths) {
  std::vector<Photo> out;
  for (const auto &path : paths) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    Photo p;
    p.path = path;
    p.name = sde::basename_of(path);
    p.dir = path.substr(0, path.rfind('/'));
    if (p.dir.empty()) p.dir = "/";
    p.mtime = st.st_mtime;
    p.size = st.st_size;
    out.push_back(std::move(p));
  }
  return out;
}

int index_of(const std::vector<Photo> &photos, const std::string &path) {
  for (size_t i = 0; i < photos.size(); ++i)
    if (photos[i].path == path) return int(i);
  return -1;
}

std::string encode_events(const std::vector<GalleryEvent> &events) {
  std::string out;
  for (const auto &e : events) {
    if (e.verb.empty() || e.path.empty() || e.path.find('\n') != std::string::npos ||
        e.verb.find_first_of("\t\n") != std::string::npos)
      continue;
    out += e.verb + "\t" + e.path + "\n";
  }
  return out;
}

std::vector<GalleryEvent> decode_events(const std::string &text) {
  std::vector<GalleryEvent> out;
  for (const auto &line : sde::split(text, '\n')) {
    size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
    GalleryEvent e{line.substr(0, tab), line.substr(tab + 1)};
    if (e.path[0] != '/') continue;
    out.push_back(std::move(e));
  }
  return out;
}

bool move_to_trash(const std::string &path, std::string *error) {
  GFile *file = g_file_new_for_path(path.c_str());
  GError *err = nullptr;
  bool ok = g_file_trash(file, nullptr, &err);
  if (!ok && error) *error = err ? err->message : "unknown error";
  if (err) g_error_free(err);
  g_object_unref(file);
  return ok;
}

}  // namespace gallery
