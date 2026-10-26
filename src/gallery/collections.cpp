// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "collections.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#include "album.h"
#include "util.h"

namespace gallery {

namespace {

const char kExt[] = ".album";

std::string lower_ascii(std::string s) {
  for (auto &c : s) c = char(tolower(static_cast<unsigned char>(c)));
  return s;
}

bool exists_path(const std::string &p) {
  struct stat st {};
  return stat(p.c_str(), &st) == 0;
}

}  // namespace

std::string albums_dir() {
  const char *xdg = getenv("XDG_DATA_HOME");
  std::string base = xdg && *xdg == '/' ? xdg : sde::home_dir() + "/.local/share";
  return base + "/sde/gallery/albums";
}

std::vector<UserAlbum> list_user_albums(const std::string &dir) {
  std::vector<UserAlbum> out;
  DIR *d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent *e = readdir(d)) {
    std::string n = e->d_name;
    size_t el = sizeof kExt - 1;
    if (n.size() <= el || n.compare(n.size() - el, el, kExt) != 0 || n[0] == '.') continue;
    UserAlbum a;
    a.name = n.substr(0, n.size() - el);
    a.file = dir + "/" + n;
    a.count = read_album_list(a.file).size();
    out.push_back(std::move(a));
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const UserAlbum &a, const UserAlbum &b) {
    std::string la = lower_ascii(a.name), lb = lower_ascii(b.name);
    return la != lb ? la < lb : a.name < b.name;
  });
  return out;
}

bool valid_album_name(const std::string &name, std::string *clean) {
  size_t b = name.find_first_not_of(" \t"), e = name.find_last_not_of(" \t");
  std::string n = b == std::string::npos ? "" : name.substr(b, e - b + 1);
  if (clean) *clean = n;
  if (n.empty() || n == "." || n == ".." || n.size() > 200) return false;
  for (unsigned char c : n)
    if (c == '/' || c < 0x20 || c == 0x7F) return false;
  return true;
}

bool create_album(const std::string &dir, const std::string &name, std::string *file, std::string *error) {
  std::string n;
  if (!valid_album_name(name, &n)) {
    if (error) *error = "invalid name";
    return false;
  }
  std::string f = dir + "/" + n + kExt;
  for (const auto &a : list_user_albums(dir))
    if (lower_ascii(a.name) == lower_ascii(n)) {
      if (error) *error = "exists";
      return false;
    }
  if (!sde::make_dirs(dir) || !write_album_list(f, {})) {
    if (error) *error = "cannot write";
    return false;
  }
  if (file) *file = f;
  return true;
}

bool rename_album(const std::string &file, const std::string &new_name, std::string *new_file,
                  std::string *error) {
  std::string n;
  if (!valid_album_name(new_name, &n)) {
    if (error) *error = "invalid name";
    return false;
  }
  std::string dir = file.substr(0, file.rfind('/'));
  std::string f = dir + "/" + n + kExt;
  if (f != file) {
    for (const auto &a : list_user_albums(dir))
      if (a.file != file && lower_ascii(a.name) == lower_ascii(n)) {
        if (error) *error = "exists";
        return false;
      }
    if (rename(file.c_str(), f.c_str()) != 0) {
      if (error) *error = "cannot rename";
      return false;
    }
  }
  if (new_file) *new_file = f;
  return true;
}

bool delete_album(const std::string &file) { return unlink(file.c_str()) == 0; }

int add_to_album(const std::string &file, const std::vector<std::string> &paths) {
  auto list = read_album_list(file);
  int added = 0;
  for (const auto &p : paths) {
    if (p.empty() || p[0] != '/' || std::find(list.begin(), list.end(), p) != list.end()) continue;
    list.push_back(p);
    ++added;
  }
  if (added && !write_album_list(file, list)) return -1;
  return added;
}

int remove_from_album(const std::string &file, const std::vector<std::string> &paths) {
  auto list = read_album_list(file);
  size_t before = list.size();
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const std::string &p) { return std::find(paths.begin(), paths.end(), p) != paths.end(); }),
             list.end());
  int removed = int(before - list.size());
  if (removed && !write_album_list(file, list)) return -1;
  return removed;
}

void album_path_changed(const std::string &dir, const std::string &from, const std::string &to) {
  for (const auto &a : list_user_albums(dir)) {
    auto list = read_album_list(a.file);
    bool changed = false;
    for (auto it = list.begin(); it != list.end();) {
      if (*it == from) {
        changed = true;
        if (to.empty()) {
          it = list.erase(it);
          continue;
        }
        *it = to;
      }
      ++it;
    }
    if (changed) write_album_list(a.file, list);
  }
}

std::string unique_destination(const std::string &dir, const std::string &name,
                               const std::function<bool(const std::string &)> &exists,
                               const std::vector<std::string> &planned, const std::string &ext) {
  std::string stem = name, suffix;
  size_t dot = name.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    stem = name.substr(0, dot);
    suffix = name.substr(dot);
  }
  if (!ext.empty()) suffix = "." + ext;
  auto taken = [&](const std::string &p) {
    if (std::find(planned.begin(), planned.end(), p) != planned.end()) return true;
    return exists ? exists(p) : exists_path(p);
  };
  std::string base = dir == "/" ? "" : dir;
  std::string candidate = base + "/" + stem + suffix;
  for (int i = 2; taken(candidate) && i < 100000; ++i) candidate = base + "/" + stem + " (" + std::to_string(i) + ")" + suffix;
  return candidate;
}

std::vector<CopyItem> plan_copy(const std::vector<std::string> &sources, const std::string &dir,
                                const std::function<bool(const std::string &)> &exists, const std::string &ext) {
  std::vector<CopyItem> out;
  std::vector<std::string> planned;
  for (const auto &s : sources) {
    std::string d = unique_destination(dir, sde::basename_of(s), exists, planned, ext);
    planned.push_back(d);
    out.push_back({s, d});
  }
  return out;
}

void fit_size(int w, int h, int max_edge, int &ow, int &oh) {
  ow = w;
  oh = h;
  if (max_edge <= 0 || w <= 0 || h <= 0 || (w <= max_edge && h <= max_edge)) return;
  double k = double(max_edge) / std::max(w, h);
  ow = std::max(1, int(std::lround(w * k)));
  oh = std::max(1, int(std::lround(h * k)));
}

// ------------------------------------------------------------ batch rename

std::string rename_one(const std::string &pattern, const RenameSource &src, int number, int width) {
  std::string name = sde::basename_of(src.path), stem = name;
  size_t dot = name.rfind('.');
  if (dot != std::string::npos && dot > 0) stem = name.substr(0, dot);
  std::string dir = src.path.substr(0, src.path.rfind('/'));
  struct tm tm {};
  time_t when = src.taken;
  localtime_r(&when, &tm);
  char date[32], clock[32];
  strftime(date, sizeof date, "%Y-%m-%d", &tm);
  strftime(clock, sizeof clock, "%H.%M.%S", &tm);
  char num[32];
  snprintf(num, sizeof num, "%0*d", std::max(1, width), number);
  std::string out;
  for (size_t i = 0; i < pattern.size();) {
    auto token = [&](const char *t) {
      size_t n = strlen(t);
      if (pattern.compare(i, n, t) != 0) return false;
      i += n;
      return true;
    };
    if (token("{name}")) out += stem;
    else if (token("{n}")) out += num;
    else if (token("{date}")) out += date;
    else if (token("{time}")) out += clock;
    else if (token("{camera}")) out += src.camera;
    else if (token("{folder}")) out += sde::basename_of(dir);
    else out += pattern[i++];
  }
  return sde::trim(out);
}

bool plan_rename(const std::vector<RenameSource> &files, const std::string &pattern, int start,
                 const std::function<bool(const std::string &)> &exists, std::vector<RenameItem> &out,
                 std::string *error) {
  out.clear();
  int last = start + int(files.size()) - 1;
  int width = int(std::to_string(std::max(1, last)).size());
  std::vector<std::string> sources;
  for (const auto &f : files) sources.push_back(f.path);
  std::vector<std::string> planned;
  for (size_t i = 0; i < files.size(); ++i) {
    const std::string &path = files[i].path;
    std::string stem = rename_one(pattern, files[i], start + int(i), width);
    if (stem.empty() || stem == "." || stem == ".." || stem.find('/') != std::string::npos) {
      if (error) *error = stem.find('/') != std::string::npos ? "slash" : "empty";
      out.clear();
      return false;
    }
    std::string name = sde::basename_of(path), ext;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) ext = name.substr(dot);
    std::string dir = path.substr(0, path.rfind('/'));
    // A name is free when no other file has it (the photos being renamed
    // give theirs up) and no other photo of this batch takes it.
    auto taken = [&](const std::string &p) {
      if (std::find(planned.begin(), planned.end(), p) != planned.end()) return true;
      if (p == path) return false;
      if (std::find(sources.begin(), sources.end(), p) != sources.end()) return false;
      return exists && exists(p);
    };
    std::string dest = dir + "/" + stem + ext;
    for (int k = 2; taken(dest); ++k) dest = dir + "/" + stem + " (" + std::to_string(k) + ")" + ext;
    planned.push_back(dest);
    if (dest != path) out.push_back({path, dest});
  }
  return true;
}

size_t apply_rename(const std::vector<RenameItem> &items, std::string *error) {
  // First every photo gets a temporary name, then its new one: "a" -> "b"
  // and "b" -> "a" works.
  std::vector<std::string> temps;
  size_t moved = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    std::string dir = items[i].from.substr(0, items[i].from.rfind('/'));
    std::string tmp = dir + "/.sde-rename-" + std::to_string(getpid()) + "-" + std::to_string(i);
    if (rename(items[i].from.c_str(), tmp.c_str()) != 0) {
      if (error) *error = strerror(errno);
      // Put back what was moved already.
      for (size_t k = 0; k < temps.size(); ++k) rename(temps[k].c_str(), items[k].from.c_str());
      return 0;
    }
    temps.push_back(tmp);
  }
  for (size_t i = 0; i < items.size(); ++i) {
    // Never replace a file that appeared meanwhile.
    if (exists_path(items[i].to) || rename(temps[i].c_str(), items[i].to.c_str()) != 0) {
      if (error) *error = exists_path(items[i].to) ? "exists" : strerror(errno);
      rename(temps[i].c_str(), items[i].from.c_str());
      continue;
    }
    ++moved;
  }
  return moved;
}

}  // namespace gallery
