// SPDX-License-Identifier: GPL-3.0-or-later
// File operations for desktop drag and drop (alpha 52): POSIX only (no GIO),
// never overwriting anything.
//  * move: rename(2) with RENAME_NOREPLACE; across file systems a copy
//    followed by removing the source once the copy is complete;
//  * copy: recursive, O_EXCL files, symlinks recreated, permissions,
//    modification times and user extended attributes kept;
//  * link: a symbolic link;
//  * trash: the freedesktop.org home trash (~/.local/share/Trash) for files
//    on the home file system.
// Everything can run on a worker thread: progress, pausing and
// cancellation go through the atomics of FileProgress (0.2.0 Alpha 2: items
// and pausing for the transfer dialog, src/common/transfer_dialog.h).
//
// 0.2.0 Alpha 3: safe copying (src/common/safe_copy.h, shared with Liria).
// Files written to a removable, external or network drive go through a
// writer that keeps only about a second of the drive's speed in flight;
// |done| counts bytes the drive has taken, and an operation ends only when
// everything is on the drive (FileProgress::phase: "writing the last
// data", then done). A move deletes its sources only after their copies
// are on the drive. Internal disks keep the page cache.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace sde {
namespace desk {

struct FileProgress {
  enum Phase { WORKING, FINISHING, DONE };
  std::atomic<long long> done{0}, total{0};      // bytes copied (copies, moves to another drive)
  std::atomic<long> files_done{0}, files_total{0};  // items: files, folders, links
  std::atomic<bool> cancel{false}, pause{false};
  // FINISHING: writing the last data to |drive|; DONE: everything there.
  std::atomic<int> phase{WORKING};
  // The destination is a drive the user may remove (a USB stick, an
  // external disk), on a FAT file system (|fat|): Windows checks such a
  // drive when it was not ejected.
  std::atomic<bool> removable{false}, fat{false};
  std::mutex mutex;
  std::string current;  // file being copied (under |mutex|)
  std::string drive;    // name of the destination drive ("SANDISK"), or "" (under |mutex|)
  void set_current(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex);
    current = path;
  }
  // Waits while paused; false once cancelled.
  bool wait_while_paused();
};

// Items in |path| itself included (recursive, symlinks not followed).
long count_items(const std::string &path);

// True when |a| and the directory |dir| are on one file system.
bool same_filesystem(const std::string &a, const std::string &dir);
// True when |path| is |dir| or inside it (no symlink resolution).
bool path_inside(const std::string &path, const std::string &dir);
// Bytes in |path| (recursive, symlinks not followed).
long long tree_size(const std::string &path);
bool path_exists(const std::string &path);  // lstat() succeeds

// Each returns false and fills |error| on failure. |dst| must not exist.
// Copies and moves return when the data is on the drive (see above).
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
// On removable, external and network drives it returns only when
// everything is on the drive (also after a Cancel).
int run_file_ops(const std::vector<FileOp> &ops, FileProgress *progress,
                 std::vector<std::string> *errors);

}  // namespace desk
}  // namespace sde
