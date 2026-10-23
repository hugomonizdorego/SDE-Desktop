// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Albums shared by Gallery and Photo. Gallery starts Photo with
// "sde-view --album=<id> FILE"; both programs resolve the id with the same
// functions below, so Photo steps through exactly the photos, and in the
// same order, that Gallery shows. Ids:
//
//   all, favorites, recent, screenshots   library views
//   folder:/path   (or a bare /path)      a library folder, recursive
//   device:/media/u/CARD                  removable media, recursive
//   dir:/path                             one directory, not recursive
//   list:/path/to/file                    a saved list (search results)
//
// The sort order is the one Gallery stores in [gallery] sort, unless the
// command line names one with --sort.
//
// Photo tells a running Gallery about changes through the root window
// property _SDE_GALLERY_EVENTS (appended lines "<verb>\t<path>"), which
// Gallery reads and deletes. Verbs: changed, removed, added, select.
#pragma once

#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "library.h"

namespace sde {
class Config;
}

namespace gallery {

enum class AlbumKind { All, Favorites, Recent, Screenshots, Folder, Device, Directory, List };

struct Album {
  AlbumKind kind = AlbumKind::All;
  std::string path;  // folder, device mount point, directory or list file
  bool operator==(const Album &o) const { return kind == o.kind && path == o.path; }
  bool operator!=(const Album &o) const { return !(*this == o); }
};

std::string album_id(const Album &a);
bool parse_album(const std::string &id, Album &out);
std::string sort_name(Sort s);  // newest, oldest, name
bool parse_sort(const std::string &name, Sort &out);

// What Gallery knows about the library, as stored in sde.conf [gallery].
struct LibrarySettings {
  std::string pictures;                   // XDG Pictures
  std::vector<std::string> user_folders;  // added folders
  std::set<std::string> favorites;
  Sort sort = Sort::Newest;
  int recent_days = 30;
  static LibrarySettings load(const sde::Config &cfg);
  std::string screenshots() const { return pictures + "/Screenshots"; }
  std::vector<std::string> roots() const;  // pictures + user folders
};

// True when |path| is strictly below directory |dir|.
bool path_under(const std::string &path, const std::string &dir);
// Folders to scan for an album, and whether the scan recurses.
std::vector<std::string> album_roots(const Album &a, const LibrarySettings &s, bool *recursive);
bool album_contains(const Album &a, const Photo &p, const LibrarySettings &s, time_t now);
// Filters |photos| (a scan of album_roots) to the album and sorts them.
std::vector<Photo> album_photos(const std::vector<Photo> &photos, const Album &a,
                                const LibrarySettings &s, Sort sort, time_t now,
                                const std::string &query = "");
// Images directly in |dir| (no recursion).
std::vector<Photo> scan_directory(const std::string &dir, const std::set<std::string> &extra_formats);
// Reads and writes a list album (one absolute path per line).
std::vector<std::string> read_album_list(const std::string &file);
bool write_album_list(const std::string &file, const std::vector<std::string> &paths);
// Photo records for paths (stat), skipping missing files.
std::vector<Photo> photos_for_paths(const std::vector<std::string> &paths);
int index_of(const std::vector<Photo> &photos, const std::string &path);

// Events from Photo to Gallery.
struct GalleryEvent {
  std::string verb;  // changed, removed, added, select
  std::string path;
};
std::string encode_events(const std::vector<GalleryEvent> &events);
std::vector<GalleryEvent> decode_events(const std::string &text);

// Moves a file to the freedesktop.org Trash (via GIO). |error| receives a
// message on failure (e.g. a file system without a trash folder).
bool move_to_trash(const std::string &path, std::string *error = nullptr);

}  // namespace gallery
