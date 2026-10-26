// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Gallery's organizing model: albums the user creates, and the planning of
// batch actions (copy, move, export with resize). Albums are list files
// ("one absolute path per line", see read_album_list) in
// $XDG_DATA_HOME/sde/gallery/albums/<name>.album, so Photo steps through an
// album with "sde-view --album=list:<file>". Nothing here needs a display.
#pragma once

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace gallery {

struct UserAlbum {
  std::string name;  // shown name (the file name without .album)
  std::string file;  // the list file
  size_t count = 0;  // photos listed
};

// The folder that holds the albums (created on demand by create_album).
std::string albums_dir();
// Albums in |dir|, sorted by name (case-insensitive).
std::vector<UserAlbum> list_user_albums(const std::string &dir);
// A name is valid when it is not empty, has no '/' and no control
// characters, and is not "." or "..". Leading/trailing spaces are dropped.
bool valid_album_name(const std::string &name, std::string *clean = nullptr);
// Creates an empty album; fails when the name is invalid or taken.
bool create_album(const std::string &dir, const std::string &name, std::string *file = nullptr,
                  std::string *error = nullptr);
bool rename_album(const std::string &file, const std::string &new_name, std::string *new_file = nullptr,
                  std::string *error = nullptr);
bool delete_album(const std::string &file);
// Adds paths (keeping the album's order, skipping those already in it);
// returns how many were added, -1 on error.
int add_to_album(const std::string &file, const std::vector<std::string> &paths);
// Removes paths; returns how many were removed, -1 on error.
int remove_from_album(const std::string &file, const std::vector<std::string> &paths);
// Follows renames and deletions of files (from Photo or batch moves).
void album_path_changed(const std::string &dir, const std::string &from, const std::string &to);

// Batch planning.
struct CopyItem {
  std::string source, dest;
};
// "IMG_1.jpg" in |dir|, or "IMG_1 (2).jpg", "IMG_1 (3).jpg"... when the
// name is taken (|exists|) or already planned in this batch. |ext|
// replaces the extension when not empty ("png").
std::string unique_destination(const std::string &dir, const std::string &name,
                               const std::function<bool(const std::string &)> &exists,
                               const std::vector<std::string> &planned = {}, const std::string &ext = "");
std::vector<CopyItem> plan_copy(const std::vector<std::string> &sources, const std::string &dir,
                                const std::function<bool(const std::string &)> &exists,
                                const std::string &ext = "");
// The size of a w x h image made to fit in |max_edge| (never enlarged;
// 0 keeps the size).
void fit_size(int w, int h, int max_edge, int &ow, int &oh);

// Batch rename (a plugin). Tokens of a pattern: {name} the old name
// without its extension, {n} a number counting from the start (zero-padded
// to the widest), {date} and {time} when the photo was taken or else
// modified (2026-10-26, 14.05.33), {camera} the camera model, {folder} the
// name of the photo's folder. The extension stays as it is.
struct RenameSource {
  std::string path;
  time_t taken = 0;
  std::string camera;
};
struct RenameItem {
  std::string from, to;
};
std::string rename_one(const std::string &pattern, const RenameSource &src, int number, int width);
// The renames for |files|: names that would be taken (|exists|, or by two
// photos) get " (2)", " (3)"... Fails with |error| when a name would be
// empty or contain "/". Photos whose name does not change are left out.
bool plan_rename(const std::vector<RenameSource> &files, const std::string &pattern, int start,
                 const std::function<bool(const std::string &)> &exists, std::vector<RenameItem> &out,
                 std::string *error = nullptr);
// Renames in two steps (through temporary names), so that names may be
// swapped or shifted. Returns how many were renamed; on failure the rest
// keep their old names.
size_t apply_rename(const std::vector<RenameItem> &items, std::string *error = nullptr);

}  // namespace gallery
