// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "viewmodel.h"

#include <algorithm>

namespace sde {
namespace liria {

void ViewModel::reset() {
  items_.clear();
  shown_.clear();
  index_.clear();
  item_of_.clear();
  selected_.clear();
  focus.clear();
  anchor.clear();
}

void ViewModel::reindex_items() {
  item_of_.clear();
  item_of_.reserve(items_.size() * 2);
  for (size_t i = 0; i < items_.size(); ++i) item_of_[items_[i].path] = int(i);
}

void ViewModel::add(std::vector<Item> &&items) {
  items_.reserve(items_.size() + items.size());
  for (auto &it : items) {
    auto found = item_of_.find(it.path);
    if (found != item_of_.end()) {
      items_[size_t(found->second)] = std::move(it);
    } else {
      item_of_[it.path] = int(items_.size());
      items_.push_back(std::move(it));
    }
  }
}

void ViewModel::upsert(Item &&item) {
  auto found = item_of_.find(item.path);
  if (found != item_of_.end()) {
    items_[size_t(found->second)] = std::move(item);
  } else {
    item_of_[item.path] = int(items_.size());
    items_.push_back(std::move(item));
  }
}

void ViewModel::remove(const std::string &path) {
  auto found = item_of_.find(path);
  if (found == item_of_.end()) return;
  items_.erase(items_.begin() + found->second);
  reindex_items();
}

void ViewModel::set_query(const std::string &query) {
  query_ = query;
  folded_ = chooser::fold_text(query);
}

bool ViewModel::refresh() {
  shown_.clear();
  shown_.reserve(items_.size());
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item &e = items_[i];
    if (e.hidden && !show_hidden) continue;
    if (!folded_.empty() && e.fold.find(folded_) == std::string::npos) continue;
    shown_.push_back(int(i));
  }
  const chooser::SortOrder by = order;
  std::stable_sort(shown_.begin(), shown_.end(),
                   [&](int a, int b) { return chooser::entry_less(items_[size_t(a)], items_[size_t(b)], by); });
  index_.clear();
  index_.reserve(shown_.size() * 2);
  for (size_t k = 0; k < shown_.size(); ++k) index_[items_[size_t(shown_[k])].path] = int(k);
  bool changed = false;
  for (auto it = selected_.begin(); it != selected_.end();) {
    if (!index_.count(*it)) {
      it = selected_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (!focus.empty() && !index_.count(focus)) focus.clear();
  if (!anchor.empty() && !index_.count(anchor)) anchor.clear();
  return changed;
}

int ViewModel::index(const std::string &path) const {
  auto it = index_.find(path);
  return it == index_.end() ? -1 : it->second;
}

std::vector<const Item *> ViewModel::selection() const {
  std::vector<std::pair<int, const Item *>> order_of;
  for (const auto &p : selected_) {
    int k = index(p);
    if (k >= 0) order_of.push_back({k, &at(k)});
  }
  std::sort(order_of.begin(), order_of.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  std::vector<const Item *> out;
  out.reserve(order_of.size());
  for (const auto &o : order_of) out.push_back(o.second);
  return out;
}

void ViewModel::clear_selection() { selected_.clear(); }

void ViewModel::select_only(const std::string &path) {
  selected_.clear();
  if (index(path) >= 0) selected_.insert(path);
  focus = anchor = path;
}

void ViewModel::toggle(const std::string &path) {
  if (index(path) < 0) return;
  if (!selected_.erase(path)) selected_.insert(path);
  focus = anchor = path;
}

void ViewModel::select_range(const std::string &path, bool add) {
  int k = index(path);
  if (k < 0) return;
  int a = index(anchor);
  if (a < 0) a = k;
  if (!add) selected_.clear();
  for (int i = std::min(a, k); i <= std::max(a, k); ++i) selected_.insert(at(i).path);
  if (anchor.empty() || index(anchor) < 0) anchor = path;
  focus = path;
}

void ViewModel::select_all() {
  selected_.clear();
  for (int k = 0; k < count(); ++k) selected_.insert(at(k).path);
}

void ViewModel::invert() {
  std::set<std::string> next;
  for (int k = 0; k < count(); ++k)
    if (!selected_.count(at(k).path)) next.insert(at(k).path);
  selected_.swap(next);
}

void ViewModel::select_set(const std::set<std::string> &paths, const std::set<std::string> &base) {
  selected_ = base;
  for (const auto &p : paths)
    if (index(p) >= 0) selected_.insert(p);
}

void ViewModel::selection_size(long &files, long &folders, long long &bytes) const {
  files = folders = 0;
  bytes = 0;
  for (const auto &p : selected_) {
    int k = index(p);
    if (k < 0) continue;
    const Item &e = at(k);
    if (e.dir) {
      ++folders;
    } else {
      ++files;
      bytes += e.size;
    }
  }
}

}  // namespace liria
}  // namespace sde
