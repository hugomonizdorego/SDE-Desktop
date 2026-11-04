// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Liria's locations (docs/LIRIA.md). A tab shows one of
//
//   liria:home     the Home page: Quick access, drives, network locations
//                  and recent files
//   trash:///      the Trash (every trash folder of the freedesktop.org
//                  specification, trash.h)
//   network:///    the network: mounted shares, recent servers and
//                  Connect to Server
//   /path          a local folder; network shares that GVfs mounted are
//                  local folders too (its FUSE folder below
//                  $XDG_RUNTIME_DIR/gvfs) when gvfs-fuse runs
//   smb://...      a folder on another computer that GIO lists (sftp://,
//                  ftp://, dav://, nfs://, ...: when there is no FUSE folder)
//
// No widgets and no I/O beyond stat(); shared with the unit tests.
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace liria {

extern const char *const kHome;     // "liria:home"
extern const char *const kTrash;    // "trash:///"
extern const char *const kNetwork;  // "network:///"

bool is_local(const std::string &loc);   // an absolute path
bool is_remote(const std::string &loc);  // a URI GIO lists (smb://...)
bool is_special(const std::string &loc); // Home, Trash, Network

// "/a/b/" -> "/a/b", "trash:" -> "trash:///", "file:///a%20b" -> "/a b",
// "home:" -> liria:home; "" when it is none of the above.
std::string normalize_location(const std::string &loc);
// The folder above: "/a/b" -> "/a", "smb://nas/share/x" -> "smb://nas/share/",
// "smb://nas/share/" -> "smb://nas/"; "" for "/", the Home page, the Trash and
// the network.
std::string parent_location(const std::string &loc);
// The name of a location as tabs and titles show it: "Home", "Trash",
// "Network", "File System", a drive's name, "Documents", "share on nas".
std::string location_title(const std::string &loc);
// Freedesktop icon names for the location, best first.
std::vector<std::string> location_icons(const std::string &loc);

// Absolute path -> file:// URI (escaped), and back ("" when not a file URI).
std::string path_to_uri(const std::string &path);
std::string uri_to_path(const std::string &uri);
// The last part of a URI's path, unescaped ("smb://nas/share/a%20b" -> "a b").
std::string uri_basename(const std::string &uri);

// A GVfs FUSE folder ($XDG_RUNTIME_DIR/gvfs/smb-share:server=nas,share=docs)
// as a URI (smb://nas/docs/) and its name for people ("docs on nas"); ""
// when |path| is not inside one. |base| is the gvfs folder (tests).
std::string gvfs_uri_of(const std::string &path, const std::string &base = "");
std::string gvfs_label(const std::string &mount_dir_name);

// The folders a location is made of, for the breadcrumbs, anchored at the
// home folder, a drive (|anchors|: mount points, longest wins) or "/":
// "/home/a/Documents" -> {"/home/a", "/home/a/Documents"}.
std::vector<std::string> crumb_chain(const std::string &loc, const std::vector<std::string> &anchors = {});

// A file name a person can type: not empty, no "/" or NUL, not "." or "..",
// at most 255 bytes. Returns the reason when it cannot be one ("" when fine).
std::string name_problem(const std::string &name);

// "~/Documents" for paths below the home folder, for people.
std::string home_relative(const std::string &path);

}  // namespace liria
}  // namespace sde
