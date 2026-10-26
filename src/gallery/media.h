// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Storage media for Gallery: USB drives, SD cards and other removable media
// mounted below /media or /run/media (udisks) are read from
// /proc/self/mountinfo. Import planning decides where copied photos go.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "library.h"

namespace gallery {

struct MountEntry {
  std::string source;       // /dev/sdb1
  std::string mount_point;  // /media/alex/NIKON D750
  std::string fs_type;      // vfat, exfat, ...
  std::string options;      // rw,nosuid,...
  // Cameras and phones mounted by GIO (gvfs): their name and location.
  std::string name;         // "Pixel 7"
  std::string uri;          // mtp://Google_Pixel_7_2A1.../
  bool read_only() const;
  bool camera() const { return !uri.empty(); }
  // The name shown in the sidebar: the volume label (last path component).
  std::string label() const;
};

// Decodes the octal escapes the kernel uses in mountinfo ("\040" = space).
std::string unescape_mount_field(const std::string &s);
// Parses /proc/self/mountinfo text (format of proc(5)).
std::vector<MountEntry> parse_mountinfo(const std::string &text);
// Mounts that look like removable media: below /media/ or /run/media/ (as
// udisks and older automounters do), not pseudo or network file systems.
bool is_removable_mount(const MountEntry &m);
std::vector<MountEntry> removable_mounts(const std::string &mountinfo_text);
// Reads /proc/self/mountinfo and returns its removable mounts.
std::vector<MountEntry> current_removable_mounts();

// Cameras (PTP, gphoto2://) and phones (MTP, mtp://) that GIO has mounted
// are folders in <runtime dir>/gvfs (gvfsd-fuse): "mtp:host=Google_Pixel_7_
// 2A111FDH200D4A", "gphoto2:host=Canon_Inc._Canon_Digital_Camera" (older
// gvfs: "gphoto2:host=%5Busb%3A001%2C005%5D").
std::vector<MountEntry> gvfs_cameras(const std::string &runtime_dir);
// "Google Pixel 7" from such a folder name (the serial number left out).
std::string gvfs_device_name(const std::string &folder);
// Cameras and phones in the output of "gio mount -li" that are not
// mounted yet: their name and activation root (mtp://..., gphoto2://...).
struct GioVolume {
  std::string name, root;
  bool mounted = false;
};
std::vector<GioVolume> parse_gio_volumes(const std::string &text);
// Cameras in the output of "gphoto2 --auto-detect": (model, port).
std::vector<std::pair<std::string, std::string>> parse_gphoto2_cameras(const std::string &text);

// Import: copies go to <dest_root>/<YYYY-MM-DD>/<name>, the date being the
// photo's modification day (cameras keep it). A file that already exists
// there with the same size is skipped; otherwise a free name "IMG (2).jpg"
// is chosen. |exists| returns the size of a destination file, or -1.
struct ImportItem {
  std::string source, dest;
  bool skip = false;  // already imported
};
std::vector<ImportItem> plan_import(const std::vector<Photo> &photos, const std::string &dest_root,
                                    const std::function<long long(const std::string &)> &exists);
// The default import folder: <Pictures>/Imported.
std::string import_root(const std::string &pictures);
// Copies a file (to a temporary name first), keeping its modification time.
bool copy_file(const std::string &from, const std::string &to, std::string *error = nullptr);

}  // namespace gallery
