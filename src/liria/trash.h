// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Trash of the freedesktop.org Trash specification 1.0, which GTK,
// Qt and the SDE desktop share:
//
//   * files of the home file system go to the home trash,
//     $XDG_DATA_HOME/Trash (files/ and info/<name>.trashinfo);
//   * files on other file systems (USB sticks, other disks) go to the trash
//     of that file system: $topdir/.Trash/$uid when the administrator made
//     a shared $topdir/.Trash (a folder with the sticky bit, not a link),
//     else $topdir/.Trash-$uid, made when needed (mode 0700). The file
//     never crosses drives, so deleting a large file is instant and a stick
//     keeps its own trash;
//   * Path= is the original path (URI-escaped), relative to $topdir in a
//     topdir trash; DeletionDate= the local time of the deletion.
//
// The Trash view lists every trash of the mounted file systems; items are
// restored to where they came from (the folders above are made again) or
// deleted for good, and Empty Trash empties them all. The home trash's
// directorysizes cache is left to the programs that keep it (it is only a
// cache, the specification allows that).
#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace sde {
namespace liria {

struct TrashItem {
  std::string trash;     // the trash folder ("~/.local/share/Trash", "/media/a/X/.Trash-1000")
  std::string name;      // the name in files/ (the info file is info/<name>.trashinfo)
  std::string original;  // where it was (absolute)
  time_t deleted = 0;
  std::string files_path() const { return trash + "/files/" + name; }
  std::string info_path() const { return trash + "/info/" + name + ".trashinfo"; }
};

// The home trash (made when |create|).
std::string home_trash();
// The trash a file at |path| goes to ("" when it has none: a read-only
// drive, a network share without a trash); makes it when |create|.
std::string trash_for(const std::string &path, bool create, std::string *error = nullptr);
// Whether |path| can go to a trash (false on read-only drives, network
// shares and in other people's folders): the delete question then offers
// "Delete permanently" instead.
bool can_trash(const std::string &path);
// The mount point of the file system |path| is on ("/" or "/media/a/X").
std::string mount_point_of(const std::string &path);
// Every trash folder that exists: the home trash and those of the mounted
// file systems (/proc/self/mountinfo, |mountinfo| for tests).
std::vector<std::string> trash_folders(const std::string &mountinfo = "");
// The mount table read instead of /proc/self/mountinfo from now on: the
// unit tests use it, so that they never list or empty the trash of a real
// drive.
void set_mount_table(const std::string &mountinfo);

// The .trashinfo text for |original| deleted at |when|; |topdir| non-empty:
// the path relative to it (a topdir trash).
std::string trash_info_text(const std::string &original, time_t when, const std::string &topdir = "");
// Reads a .trashinfo; false when it is not one. |topdir|: for relative paths.
bool parse_trash_info(const std::string &text, const std::string &topdir, std::string &original, time_t &deleted);

// Moves |path| into its trash; |item| (may be null) receives where it went.
bool move_to_trash(const std::string &path, TrashItem *item, std::string *error);
// Everything in the trash folders, newest first.
std::vector<TrashItem> list_trash(const std::vector<std::string> &folders);
std::vector<TrashItem> list_trash();
// The item of |files_path| (a path inside some trash's files/).
bool trash_item_of(const std::string &files_path, TrashItem &item);
// Puts |item| back at |to| (its original place when empty), making the
// folders above. Fails (EEXIST in |err|) when something is there.
bool restore_from_trash(const TrashItem &item, const std::string &to, std::string *error, int *err = nullptr);
// Deletes |item| for good (its info file too).
bool erase_from_trash(const TrashItem &item, std::string *error);
// The number of items in every trash (capped at |limit| for speed).
size_t trash_count(size_t limit = 1000000);
bool trash_is_empty();

}  // namespace liria
}  // namespace sde
