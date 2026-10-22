// SPDX-License-Identifier: GPL-3.0-or-later
// File operations for desktop drag and drop (alpha 52): POSIX only (no GIO),
// never overwriting anything.
//  * move: rename(2) with RENAME_NOREPLACE; across file systems a copy
//    followed by removing the source once the copy is complete;
//  * copy: recursive, O_EXCL files, symlinks recreated, permissions and
//    modification times kept;
//  * link: a symbolic link;
//  * trash: the freedesktop.org home trash (~/.local/share/Trash) for files
//    on the home file system.
// Everything can run on a worker thread: progress and cancellation go
// through the atomics of FileProgress.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace sde {
namespace desk {

struct FileProgress {
  std::atomic<long long> done{0}, total{0};
  std::atomic<bool> cancel{false};
  std::mutex mutex;
  std::string current;  // file being copied (under |mutex|)
  void set_current(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex);
    current = path;
  }
};

// True when |a| and the directory |dir| are on one file system.
bool same_filesystem(const std::string &a, const std::string &dir);
// True when |path| is |dir| or inside it (no symlink resolution).
bool path_inside(const std::string &path, const std::string &dir);
// Bytes in |path| (recursive, symlinks not followed).
long long tree_size(const std::string &path);
bool path_exists(const std::string &path);  // lstat() succeeds

// Each returns false and fills |error| on failure. |dst| must not exist.
bool move_path(const std::string &src, const std::string &dst, FileProgress *progress,
               std::string *error);
bool copy_path(const std::string &src, const std::string &dst, FileProgress *progress,
               std::string *error);
bool link_path(const std::string &src, const std::string &dst, std::string *error);
bool remove_tree(const std::string &path);

// The .trashinfo contents for |path| deleted at |when| (local time).
std::string trash_info(const std::string &path, long long when);
// Moves |path| into the home trash |trash_dir| (~/.local/share/Trash).
// False when it is on another file system (callers fall back to gio).
bool trash_path(const std::string &path, const std::string &trash_dir, std::string *error);
std::string home_trash_dir();

// One planned operation (after conflicts were resolved).
struct FileOp {
  enum Kind { MOVE, COPY, LINK, TRASH } kind = COPY;
  std::string src, dst;   // dst: full destination path (empty for TRASH)
  bool replace = false;   // dst exists and the user chose Replace
};

// Runs |ops|; a replaced destination is moved aside first and deleted only
// once the new copy is complete (restored when the operation fails).
// Returns the number of failed operations; their messages go to |errors|.
int run_file_ops(const std::vector<FileOp> &ops, FileProgress *progress,
                 std::vector<std::string> *errors);

}  // namespace desk
}  // namespace sde
