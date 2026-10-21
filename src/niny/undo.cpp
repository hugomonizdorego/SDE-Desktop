// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "undo.h"

#include <algorithm>
#include <cctype>

namespace niny {

namespace {

size_t cost(const Edit &e) { return e.removed.size() + e.inserted.size() + sizeof(Edit); }

// One UTF-8 character without a newline.
bool single_char(const std::string &s) {
  if (s.empty() || s.size() > 4 || s.find('\n') != std::string::npos) return false;
  unsigned char c = (unsigned char)s[0];
  size_t want = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
  return s.size() == want;
}

bool is_space(const std::string &s) { return s == " " || s == "\t"; }

}  // namespace

void UndoStack::begin_group(int cursor) {
  if (group_depth_++ == 0) {
    UndoStep step;
    step.id = next_id_++;
    step.cursor_before = cursor;
    undo_.push_back(std::move(step));
    group_open_ = true;
    redo_.clear();
  }
}

void UndoStack::end_group() {
  if (group_depth_ > 0 && --group_depth_ == 0) {
    group_open_ = false;
    merge_ok_ = false;
    if (!undo_.empty() && undo_.back().edits.empty()) undo_.pop_back();
    trim();
  }
}

void UndoStack::record(int pos, const std::string &removed, const std::string &inserted, double now, int cursor) {
  if (removed.empty() && inserted.empty()) return;
  redo_.clear();
  Edit e{pos, removed, inserted};
  bytes_ += cost(e);
  if (group_open_) {
    undo_.back().edits.push_back(std::move(e));
    return;
  }
  if (merge_ok_ && !undo_.empty() && undo_.back().id != saved_ && now - undo_.back().time < 2.0) {
    UndoStep &top = undo_.back();
    Edit &last = top.edits.back();
    bool merged = false;
    if (removed.empty() && last.removed.empty() && single_char(inserted) &&
        pos == last.pos + int(last.inserted.size()) && !last.inserted.empty() &&
        !(is_space(inserted) && !is_space(std::string(1, last.inserted.back())))) {
      last.inserted += inserted;  // typing
      merged = true;
    } else if (inserted.empty() && last.inserted.empty() && single_char(removed)) {
      if (pos + int(removed.size()) == last.pos) {  // backspace
        last.removed = removed + last.removed;
        last.pos = pos;
        merged = true;
      } else if (pos == last.pos) {  // delete
        last.removed += removed;
        merged = true;
      }
    }
    if (merged) {
      top.time = now;
      return;
    }
  }
  UndoStep step;
  step.id = next_id_++;
  step.time = now;
  step.cursor_before = cursor;
  bool typing = (removed.empty() && single_char(inserted)) || (inserted.empty() && single_char(removed));
  step.edits.push_back(std::move(e));
  undo_.push_back(std::move(step));
  merge_ok_ = typing;
  trim();
}

const UndoStep *UndoStack::undo() {
  if (undo_.empty() || group_open_) return nullptr;
  redo_.push_back(std::move(undo_.back()));
  undo_.pop_back();
  merge_ok_ = false;
  return &redo_.back();
}

const UndoStep *UndoStack::redo() {
  if (redo_.empty() || group_open_) return nullptr;
  undo_.push_back(std::move(redo_.back()));
  redo_.pop_back();
  merge_ok_ = false;
  return &undo_.back();
}

void UndoStack::clear() {
  undo_.clear();
  redo_.clear();
  bytes_ = 0;
  saved_ = 0;
  group_depth_ = 0;
  group_open_ = false;
  merge_ok_ = false;
}

void UndoStack::trim() {
  if (group_open_) return;
  size_t drop = 0;
  while (undo_.size() - drop > 1 && (undo_.size() - drop > max_steps || bytes_ > max_bytes)) {
    for (auto &e : undo_[drop].edits) bytes_ -= std::min(bytes_, cost(e));
    // An empty history now stands for the content after this step.
    if (saved_ == 0) saved_ = ~0u;
    else if (saved_ == undo_[drop].id) saved_ = 0;
    ++drop;
  }
  if (drop) undo_.erase(undo_.begin(), undo_.begin() + long(drop));
}

}  // namespace niny
