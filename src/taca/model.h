// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The folder tree of an archive: members grouped into folders (including
// folders that only exist implicitly in member paths), with sizes summed
// up, sorting and search. No display code.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"

namespace taca {

struct Node {
  std::string name;
  std::string path;  // member path, "" for the root
  bool dir = false;
  bool symlink = false;
  bool encrypted = false;
  bool unsafe = false;
  int64_t size = 0;    // folders: sum of their files
  int64_t packed = -1; // -1 unknown
  time_t mtime = 0;
  size_t files = 0;    // folders: files inside, recursively
  int parent = -1;
  std::vector<int> children;
};

enum class SortKey { Name, Size, Packed, Ratio, Type, Modified };

// Packed size as a share of the size (0.42 = 42 %), -1 when unknown.
double packed_ratio(const Node &n);

class Tree {
 public:
  void build(const std::vector<Entry> &entries);
  // Every entry directly below the root, named after the last component of
  // its path (a folder listing or a list of find results). Node i + 1 is
  // entries[i]; folders are not summed up.
  void build_flat(const std::vector<Entry> &entries);
  void clear() {
    nodes_.clear();
    index_.clear();
  }
  const Node &node(int i) const { return nodes_[size_t(i)]; }
  size_t size() const { return nodes_.size(); }
  int root() const { return 0; }
  // Index of the node for |path| ("" = root), -1 if missing.
  int find(const std::string &path) const;
  // Children of |dir|, folders first, sorted by |key|.
  std::vector<int> list(int dir, SortKey key, bool descending) const;
  // Every node below |dir| whose name contains |query| (case-insensitive).
  std::vector<int> search(int dir, const std::string &query, SortKey key, bool descending) const;
  // Path components of a folder, for the breadcrumb (root first).
  std::vector<int> ancestry(int dir) const;

 private:
  int ensure_dir(const std::string &path);
  void sort(std::vector<int> &v, SortKey key, bool descending) const;
  std::vector<Node> nodes_;
  std::unordered_map<std::string, int> index_;
};

// "Text", "Image", "Folder" ... from a file name.
std::string file_kind(const std::string &name, bool dir);
// Short extension label for icons ("PDF", "TXT"), "" for none.
std::string extension_label(const std::string &name);
// A category used to tint file icons: 0 other, 1 text, 2 image, 3 audio,
// 4 video, 5 archive, 6 code, 7 document, 8 executable.
int file_category(const std::string &name);
bool matches_query(const std::string &name, const std::string &query);

}  // namespace taca
