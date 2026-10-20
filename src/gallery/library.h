// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Gallery library model: the photos found in the picture folders, how they
// are sorted, filtered and grouped by month, and where their freedesktop.org
// thumbnails live. Nothing here touches FLTK, so it can be unit tested and
// used from the scanning thread.
#pragma once

#include <sys/types.h>

#include <ctime>
#include <set>
#include <string>
#include <vector>

namespace gallery {

struct Photo {
  std::string path;  // absolute path
  std::string name;  // file name
  std::string dir;   // containing directory
  time_t mtime = 0;
  long long size = 0;
};

enum class Sort { Newest, Oldest, Name };

// A run of consecutive photos sharing a header (a month, or one group when
// sorted by name).
struct Group {
  std::string key;    // "2026-10"
  std::string label;  // "October 2026"
  size_t first = 0;
  size_t count = 0;
};

// "YYYY-MM" in local time.
std::string month_key(time_t t);
// Localized "October 2026"; "This Month" style labels are left to the UI.
std::string month_label(time_t t);

void sort_photos(std::vector<Photo> &photos, Sort sort);
// Groups an already sorted list. Name sorting yields a single group.
std::vector<Group> group_photos(const std::vector<Photo> &photos, Sort sort);

// Case-insensitive substring match on the file name; empty query matches.
bool matches_search(const std::string &name, const std::string &query);

// Extensions Gallery shows. |extra| adds formats provided by gdk-pixbuf.
bool is_image_name(const std::string &name, const std::set<std::string> &extra = {});
std::string extension_of(const std::string &name);  // lower case, without dot

// freedesktop.org Thumbnail Managing Standard.
std::string cache_home();                                  // $XDG_CACHE_HOME or ~/.cache
std::string file_uri(const std::string &path);             // file:///a%20b.png
std::string md5_hex(const std::string &data);
// <cache>/thumbnails/{normal,large}/<md5(uri)>.png
std::string thumbnail_path(const std::string &path, bool large,
                           const std::string &cache = cache_home());
bool in_thumbnail_cache(const std::string &path, const std::string &cache = cache_home());

// Lists stored in one sde.conf value: entries separated by ';' with '%', ';'
// and '#' escaped, so arbitrary paths survive the INI round trip.
std::string encode_list(const std::vector<std::string> &items);
std::vector<std::string> decode_list(const std::string &value);

// The user's XDG Pictures directory (~/Pictures when unset).
std::string pictures_dir();

// Recursively collects images below |roots| (hidden folders and the
// thumbnail cache are skipped). Duplicates and overlapping roots are merged.
std::vector<Photo> scan_folders(const std::vector<std::string> &roots,
                                const std::set<std::string> &extra_formats, size_t limit = 100000);

// "2.4 MB"
std::string human_size(long long bytes);

}  // namespace gallery
