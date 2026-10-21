// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Folder tree, sorting and file kinds.
#include "model.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

#include "i18n.h"
#include "util.h"

namespace taca {

namespace {

std::string ext_of(const std::string &name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return "";
  return sde::lower(name.substr(dot + 1));
}

}  // namespace

int Tree::ensure_dir(const std::string &path) {
  int found = find(path);
  if (found >= 0) {
    nodes_[size_t(found)].dir = true;
    return found;
  }
  size_t slash = path.rfind('/');
  int parent = slash == std::string::npos ? 0 : ensure_dir(path.substr(0, slash));
  Node n;
  n.path = path;
  n.name = slash == std::string::npos ? path : path.substr(slash + 1);
  n.dir = true;
  n.parent = parent;
  nodes_.push_back(n);
  int index = int(nodes_.size() - 1);
  index_[path] = index;
  nodes_[size_t(parent)].children.push_back(index);
  return index;
}

void Tree::build(const std::vector<Entry> &entries) {
  nodes_.clear();
  Node root;
  root.dir = true;
  nodes_.push_back(root);
  index_.clear();
  index_[""] = 0;
  for (const auto &e : entries) {
    if (e.path.empty()) continue;
    if (e.dir) {
      int i = ensure_dir(e.path);
      auto &n = nodes_[size_t(i)];
      n.mtime = e.mtime;
      n.unsafe = e.unsafe;
      continue;
    }
    size_t slash = e.path.rfind('/');
    int parent = slash == std::string::npos ? 0 : ensure_dir(e.path.substr(0, slash));
    int existing = find(e.path);
    Node n;
    n.path = e.path;
    n.name = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
    n.size = e.size;
    n.packed = e.packed;
    n.mtime = e.mtime;
    n.symlink = e.symlink;
    n.encrypted = e.encrypted;
    n.unsafe = e.unsafe;
    n.parent = parent;
    if (existing >= 0 && !nodes_[size_t(existing)].dir) {
      // The same name twice (tar appends): the last one wins.
      n.children = nodes_[size_t(existing)].children;
      nodes_[size_t(existing)] = n;
      continue;
    }
    if (existing >= 0) continue;  // a folder of that name exists
    nodes_.push_back(n);
    int index = int(nodes_.size() - 1);
    index_[n.path] = index;
    nodes_[size_t(parent)].children.push_back(index);
  }
  // Sum sizes bottom-up: children always have larger indexes than parents.
  for (size_t i = nodes_.size(); i-- > 1;) {
    Node &n = nodes_[i];
    if (!n.dir) n.files = 1;
    Node &p = nodes_[size_t(n.parent)];
    if (n.size > 0) p.size += n.size;
    p.files += n.files;
    if (n.packed >= 0) p.packed = (p.packed < 0 ? 0 : p.packed) + n.packed;
    if (n.mtime > p.mtime && p.dir && p.mtime == 0) p.mtime = n.mtime;
  }
}

int Tree::find(const std::string &path) const {
  auto it = index_.find(path);
  return it == index_.end() ? -1 : it->second;
}

void Tree::sort(std::vector<int> &v, SortKey key, bool descending) const {
  std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
    const Node &x = nodes_[size_t(a)], &y = nodes_[size_t(b)];
    if (x.dir != y.dir) return x.dir;
    int c = 0;
    switch (key) {
      case SortKey::Name: break;
      case SortKey::Size: c = x.size < y.size ? -1 : x.size > y.size; break;
      case SortKey::Packed: c = x.packed < y.packed ? -1 : x.packed > y.packed; break;
      case SortKey::Ratio: {
        double rx = packed_ratio(x), ry = packed_ratio(y);
        c = rx < ry ? -1 : rx > ry;
        break;
      }
      case SortKey::Type: c = file_kind(x.name, x.dir).compare(file_kind(y.name, y.dir)); break;
      case SortKey::Modified: c = x.mtime < y.mtime ? -1 : x.mtime > y.mtime; break;
    }
    if (c == 0) c = strcasecmp(x.name.c_str(), y.name.c_str());
    if (c == 0) c = x.name.compare(y.name);
    return descending ? c > 0 : c < 0;
  });
}

std::vector<int> Tree::list(int dir, SortKey key, bool descending) const {
  if (dir < 0 || dir >= int(nodes_.size())) return {};
  std::vector<int> v = nodes_[size_t(dir)].children;
  sort(v, key, descending);
  return v;
}

double packed_ratio(const Node &n) {
  if (n.packed < 0 || n.size <= 0) return -1;
  return double(n.packed) / double(n.size);
}

bool matches_query(const std::string &name, const std::string &query) {
  if (query.empty()) return true;
  return sde::lower(name).find(sde::lower(query)) != std::string::npos;
}

std::vector<int> Tree::search(int dir, const std::string &query, SortKey key, bool descending) const {
  std::vector<int> out;
  if (dir < 0 || dir >= int(nodes_.size())) return out;
  std::vector<int> stack{dir};
  while (!stack.empty()) {
    int d = stack.back();
    stack.pop_back();
    for (int c : nodes_[size_t(d)].children) {
      if (matches_query(nodes_[size_t(c)].name, query)) out.push_back(c);
      if (nodes_[size_t(c)].dir) stack.push_back(c);
    }
  }
  sort(out, key, descending);
  return out;
}

std::vector<int> Tree::ancestry(int dir) const {
  std::vector<int> out;
  if (dir < 0 || dir >= int(nodes_.size())) return out;
  for (int i = dir; i >= 0; i = nodes_[size_t(i)].parent) out.push_back(i);
  std::reverse(out.begin(), out.end());
  return out;
}

int file_category(const std::string &name) {
  static const std::map<std::string, int> kinds = {
      {"txt", 1}, {"md", 1}, {"log", 1}, {"csv", 1}, {"ini", 1}, {"conf", 1}, {"rst", 1},
      {"png", 2}, {"jpg", 2}, {"jpeg", 2}, {"gif", 2}, {"svg", 2}, {"webp", 2}, {"bmp", 2},
      {"tif", 2}, {"tiff", 2}, {"ico", 2}, {"xcf", 2}, {"heic", 2}, {"avif", 2},
      {"mp3", 3}, {"ogg", 3}, {"flac", 3}, {"wav", 3}, {"opus", 3}, {"m4a", 3}, {"aac", 3},
      {"mp4", 4}, {"mkv", 4}, {"webm", 4}, {"avi", 4}, {"mov", 4}, {"m4v", 4},
      {"zip", 5}, {"rar", 5}, {"7z", 5}, {"tar", 5}, {"gz", 5}, {"xz", 5}, {"bz2", 5},
      {"zst", 5}, {"tgz", 5}, {"deb", 5}, {"rpm", 5}, {"iso", 5}, {"jar", 5}, {"cab", 5},
      {"c", 6}, {"cc", 6}, {"cpp", 6}, {"h", 6}, {"hpp", 6}, {"py", 6}, {"rs", 6}, {"js", 6},
      {"ts", 6}, {"java", 6}, {"go", 6}, {"sh", 6}, {"html", 6}, {"css", 6}, {"json", 6},
      {"xml", 6}, {"yaml", 6}, {"yml", 6}, {"toml", 6}, {"php", 6}, {"rb", 6}, {"lua", 6},
      {"pdf", 7}, {"odt", 7}, {"ods", 7}, {"odp", 7}, {"doc", 7}, {"docx", 7}, {"xls", 7},
      {"xlsx", 7}, {"ppt", 7}, {"pptx", 7}, {"epub", 7}, {"rtf", 7},
      {"exe", 8}, {"msi", 8}, {"appimage", 8}, {"so", 8}, {"dll", 8}, {"bin", 8}, {"run", 8},
  };
  auto it = kinds.find(ext_of(name));
  return it == kinds.end() ? 0 : it->second;
}

std::string file_kind(const std::string &name, bool dir) {
  if (dir) return _("Folder");
  switch (file_category(name)) {
    case 1: return _("Text");
    case 2: return _("Image");
    case 3: return _("Audio");
    case 4: return _("Video");
    case 5: return _("Archive");
    case 6: return _("Source code");
    case 7: return _("Document");
    case 8: return _("Program");
  }
  std::string e = extension_label(name);
  return e.empty() ? std::string(_("File")) : sde::format(_("%s file"), e.c_str());
}

std::string extension_label(const std::string &name) {
  std::string e = ext_of(name);
  for (auto &c : e) c = char(toupper(static_cast<unsigned char>(c)));
  return e;
}

}  // namespace taca
