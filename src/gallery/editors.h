// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// "Edit with" in Gallery: the installed programs that edit a kind of image,
// and a watch on folders so that what another program saves shows up in
// Gallery. Nothing here needs a display.
//
// An image editor is an application whose desktop entry
//   * takes files (%f, %F, %u or %U in Exec=),
//   * lists the photo's MIME type (or image/*) in MimeType=, and
//   * is in the Photography or RasterGraphics category, or VectorGraphics
//     for SVG images (Inkscape), and not a Viewer (image viewers list the
//     same types).
// Photo (sde-view) and Gallery themselves are not counted: Gallery puts
// Photo first on its own.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "desktop_entry.h"

namespace gallery {

bool is_image_editor(const sde::DesktopEntry &e, const std::string &mime);
// The editors among |apps| for |mime|, sorted by name.
std::vector<sde::DesktopEntry> image_editors(const std::vector<sde::DesktopEntry> &apps, const std::string &mime);
// Every application among |apps| that opens |mime| (for "Other
// Application..."), editors first, then by name.
std::vector<sde::DesktopEntry> applications_for(const std::vector<sde::DesktopEntry> &apps, const std::string &mime);
// True when the entry's Exec= takes files.
bool takes_files(const sde::DesktopEntry &e);

// Watches folders (not their subfolders) for files that are written,
// created, moved or deleted, with Linux inotify.
class FolderWatch {
 public:
  FolderWatch();
  ~FolderWatch();
  FolderWatch(const FolderWatch &) = delete;
  FolderWatch &operator=(const FolderWatch &) = delete;
  // The descriptor to wait on (Fl::add_fd), -1 when inotify is missing.
  int fd() const { return fd_; }
  bool watch(const std::string &dir);
  bool watching(const std::string &dir) const;
  // Only file names |accept| returns true for are reported (e.g. photos;
  // the temporary files programs save through are not).
  void set_filter(std::function<bool(const std::string &name)> accept) { accept_ = std::move(accept); }
  // Reads what happened since the last call: files that exist and were
  // written, created or moved there (false), and files that are gone
  // (true), each path once, in order.
  std::vector<std::pair<std::string, bool>> take();

 private:
  int fd_ = -1;
  std::vector<std::pair<int, std::string>> dirs_;  // watch descriptor, folder
  std::function<bool(const std::string &)> accept_;
};

}  // namespace gallery
