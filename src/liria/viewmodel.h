// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The items of a folder as a tab shows them: what was read, which are
// shown (hidden files, the filter typed in the search box), in which order
// (name, size, date or type, folders first; natural order: "2" before
// "10"), and the selection with its focus and anchor (click, Ctrl+click,
// Shift+click, Ctrl+A, invert, the rubber band). No widgets; the unit
// tests use it directly.
#pragma once

#include <ctime>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "file_chooser_model.h"

namespace sde {
namespace liria {

struct Item : chooser::Entry {
  // In the Trash: |folder| is where it was, |mtime| when it was deleted;
  // |original| the full original path.
  std::string original;
  // A remote item (GIO): its URI; |path| is the same URI.
  bool remote = false;
  bool executable = false;
};

class ViewModel {
 public:
  // Replaces everything (a folder was opened); the selection is cleared.
  void reset();
  void add(std::vector<Item> &&items);
  // A file changed or appeared (inotify): replaced by path, or added.
  void upsert(Item &&item);
  void remove(const std::string &path);
  // Filters, sorts and indexes again; drops selected items that are gone.
  // Returns true when the selection changed.
  bool refresh();

  int count() const { return int(shown_.size()); }
  const Item &at(int k) const { return items_[size_t(shown_[size_t(k)])]; }
  int index(const std::string &path) const;
  const std::vector<Item> &items() const { return items_; }
  size_t total() const { return items_.size(); }

  chooser::SortOrder order;
  bool show_hidden = false;
  void set_query(const std::string &query);
  const std::string &query() const { return query_; }

  // ------------------------------------------------------------- selection
  const std::set<std::string> &selected() const { return selected_; }
  bool is_selected(const std::string &path) const { return selected_.count(path) != 0; }
  std::vector<const Item *> selection() const;  // in view order
  void clear_selection();
  void select_only(const std::string &path);
  void toggle(const std::string &path);
  // From the anchor to |path| (Shift+click); |add| keeps the rest (Ctrl+Shift).
  void select_range(const std::string &path, bool add = false);
  void select_all();
  void invert();
  // The rubber band: |paths| selected, plus |base| (Ctrl kept the old ones).
  void select_set(const std::set<std::string> &paths, const std::set<std::string> &base = {});
  std::string focus, anchor;
  // Selected sizes: files and their bytes (folders are not counted inside).
  void selection_size(long &files, long &folders, long long &bytes) const;

 private:
  std::vector<Item> items_;
  std::vector<int> shown_;
  std::unordered_map<std::string, int> index_, item_of_;
  std::set<std::string> selected_;
  std::string query_, folded_;
  void reindex_items();
};

}  // namespace liria
}  // namespace sde
