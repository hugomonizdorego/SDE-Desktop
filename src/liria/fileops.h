// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Liria's file operations without a display:
//
//   * planning a copy, move or link: what goes where, the items whose
//     destination exists (each answered with Replace, Keep both, Skip or,
//     for two folders, Merge; "for all" answers the rest), a copy into its
//     own folder made as "name (2)", nothing moved onto itself or into
//     itself; the plan runs on SDE's file engine (desk::run_file_ops(),
//     src/desktop/file_ops.h), which keeps the old item until the new one
//     is complete and writes to USB sticks safely (src/common/safe_copy.h);
//   * the Trash (trash.h), deleting for good, restoring, emptying;
//   * renaming, new folders and documents (with the Templates folder);
//   * the undo history of the last operations (Ctrl+Z).
//
// Everything that takes long runs on a worker thread and reports through
// desk::FileProgress (pause, cancel, bytes and items), which the transfer
// dialog shows (src/common/transfer_dialog.h).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "file_ops.h"
#include "trash.h"

namespace sde {
namespace liria {

enum class Transfer { Copy, Move, Link };

// An item whose destination exists.
struct Clash {
  std::string src, dst;
  bool src_dir = false, dst_dir = false;
  long long src_size = 0, dst_size = 0;
  time_t src_mtime = 0, dst_mtime = 0;
};

enum class Answer { Replace, KeepBoth, Skip, Merge, Cancel };
// Decides a clash; sets |all| when the answer is for every later clash of
// the same kind (two folders, or anything else).
using Resolver = std::function<Answer(const Clash &clash, bool &all)>;

struct Plan {
  Transfer kind = Transfer::Copy;
  std::vector<desk::FileOp> ops;
  // Folders whose contents were merged into an existing folder by a move:
  // removed at the end when they are empty.
  std::vector<std::string> remove_after;
  // The new top-level items (pasted files are selected afterwards).
  std::vector<std::string> results;
  // (destination, source) of every item that will exist when done, for undo.
  std::vector<std::pair<std::string, std::string>> moved;
  std::vector<std::string> errors;  // items that cannot go
  bool cancelled = false;
  bool empty() const { return ops.empty(); }
};

// The plan for |sources| (absolute paths) into the folder |dest_dir|.
Plan plan_transfer(const std::vector<std::string> &sources, const std::string &dest_dir, Transfer kind,
                   const Resolver &resolve);
// Runs a plan (on a worker thread): its operations, then the emptied
// folders of merges. Returns the number of failures (messages in |errors|).
int run_plan(const Plan &plan, desk::FileProgress *progress, std::vector<std::string> *errors);

// The trash, on a worker thread. |trashed| receives what went where.
int trash_paths(const std::vector<std::string> &paths, desk::FileProgress *progress, std::vector<TrashItem> *trashed,
                std::vector<std::string> *errors);
// Deletes for good (no trash).
int delete_paths(const std::vector<std::string> &paths, desk::FileProgress *progress, std::vector<std::string> *errors);
// Puts trash items back where they came from; |restored| receives the paths.
int restore_items(const std::vector<TrashItem> &items, desk::FileProgress *progress, std::vector<std::string> *restored,
                  std::vector<std::string> *errors);
// Deletes everything in every trash.
int empty_all_trash(desk::FileProgress *progress, std::vector<std::string> *errors);

// Renames |path| to |name| in its folder (a change of case only is fine on
// any file system). |new_path| receives the new path.
bool rename_path(const std::string &path, const std::string &name, std::string *new_path, std::string *error);
// "New folder", "New folder (2)" ... free in |dir|.
std::string free_name(const std::string &dir, const std::string &name, bool is_dir);
// Creates a folder (|name| free, or made free) and returns its path ("" on failure).
std::string create_folder(const std::string &dir, const std::string &name, std::string *error);
// Creates a document from |template_path| (an empty one when ""), named
// |name| or the template's name, and returns its path.
std::string create_document(const std::string &dir, const std::string &name, const std::string &template_path,
                            std::string *error);
// The files of the XDG Templates folder (sorted, no hidden files).
std::vector<std::string> templates();

// ---------------------------------------------------------------- undo

struct UndoStep {
  enum Kind { Rename, Move, Copy, Trash, Create } kind = Rename;
  // Rename, Move: (where it is now, where it was); Copy, Create: (made, "").
  std::vector<std::pair<std::string, std::string>> pairs;
  std::vector<TrashItem> trashed;  // Trash
};

class UndoStack {
 public:
  void push(UndoStep step);
  bool empty() const { return steps_.empty(); }
  const UndoStep &top() const { return steps_.back(); }
  UndoStep pop();
  void clear() { steps_.clear(); }
  size_t size() const { return steps_.size(); }
  // "Undo Rename", "Undo Move", ... for the menu.
  static std::string label(const UndoStep &step);

 private:
  std::vector<UndoStep> steps_;
};

// Undoes |step| (on a worker thread for moves and copies): renames back,
// moves back, puts copies and new items into the trash, restores trashed
// items. Returns the number of failures.
int undo_step(const UndoStep &step, desk::FileProgress *progress, std::vector<std::string> *errors);

}  // namespace liria
}  // namespace sde
