// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Niny undo/redo history. Every buffer change is recorded as an Edit;
// consecutive typing or deleting merges into one step, and compound commands
// (replace all, indent, move line) are grouped explicitly.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace niny {

struct Edit {
  int pos = 0;
  std::string removed, inserted;
};

struct UndoStep {
  std::vector<Edit> edits;  // in the order they were made
  unsigned id = 0;
  double time = 0;
  int cursor_before = 0;
};

class UndoStack {
 public:
  // Records a change made at |pos|. |now| is in seconds (for merging).
  void record(int pos, const std::string &removed, const std::string &inserted, double now, int cursor = -1);
  // Changes between begin_group() and end_group() form one step.
  void begin_group(int cursor = -1);
  void end_group();
  // The next change starts a new step.
  void break_merge() { merge_ok_ = false; }

  bool can_undo() const { return !undo_.empty(); }
  bool can_redo() const { return !redo_.empty(); }
  // Moves the newest step to the redo list and returns it; apply its edits
  // in reverse, replacing |inserted| with |removed|.
  const UndoStep *undo();
  // Moves the newest undone step back and returns it; apply its edits in
  // order, replacing |removed| with |inserted|.
  const UndoStep *redo();

  // Identifies the current content: equal values mean equal text.
  unsigned state() const { return undo_.empty() ? 0 : undo_.back().id; }
  void mark_saved() {
    saved_ = state();
    merge_ok_ = false;
  }
  // The saved content is no longer reachable (e.g. history dropped).
  void mark_unsaved() { saved_ = ~0u; }
  bool at_saved() const { return saved_ == state(); }
  void clear();
  size_t memory() const { return bytes_; }
  size_t steps() const { return undo_.size(); }

  // Limits: oldest steps are dropped beyond these.
  size_t max_steps = 5000;
  size_t max_bytes = 64u << 20;

 private:
  void trim();
  std::vector<UndoStep> undo_, redo_;
  unsigned next_id_ = 1, saved_ = 0;
  int group_depth_ = 0;
  bool group_open_ = false;
  bool merge_ok_ = false;
  size_t bytes_ = 0;
};

}  // namespace niny
