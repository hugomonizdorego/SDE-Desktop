// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// What Liria remembers:
//
//   ~/.config/sde/liria.conf      the preferences (GKeyFile):
//     [view]    layout=icons|details|compact, icon_size (pixels at 100 %),
//               hidden, sort=name|size|modified|type, descending,
//               folders_first, confirm_trash
//     [home]    the sections that are open (quick_access, drives, network,
//               recent) and shown (network_shown: the Network locations
//               section can be turned off from its empty state;
//               recent_shown: Recent files)
//     [sidebar] shown, width, the groups that are open
//     [window]  width, height (pixels at 100 %), maximized
//   ~/.local/state/sde/liria-views  the layout and sorting of each folder the
//               user changed them in (at most 500 folders, as Windows does)
#pragma once

#include <map>
#include <string>

#include "file_chooser_model.h"

namespace sde {
namespace liria {

enum class Layout { Icons, Details, Compact };
std::string layout_id(Layout l);
Layout layout_from_id(const std::string &id);

struct FolderView {
  Layout layout = Layout::Icons;
  chooser::SortOrder sort;
  int icon_size = 0;  // 0: the default
};

struct Prefs {
  Layout layout = Layout::Icons;
  int icon_size = 72;  // 32 .. 256
  bool show_hidden = false;
  chooser::SortOrder sort;
  bool confirm_trash = false;  // Windows 11 deletes to the Recycle Bin without asking

  bool home_quick = true, home_drives = true, home_network = true, home_recent = true;
  bool network_shown = true, recent_shown = true;

  bool sidebar_shown = true;
  int sidebar_width = 230;
  bool side_pinned = true, side_drives = true, side_network = true;

  int width = 0, height = 0;
  bool maximized = false;

  static std::string file_path();
  static std::string views_path();
  void load(const std::string &path = "", const std::string &views = "");
  bool save() const;

  // The view of |folder|: its own, or the defaults.
  FolderView view_of(const std::string &folder) const;
  // Remembers the view of |folder| (and saves the folder views).
  void set_view(const std::string &folder, const FolderView &v);
  bool has_view(const std::string &folder) const { return views_.count(folder) != 0; }

  static int clamp_icon_size(int px);

 private:
  std::string path_, views_path_;
  std::map<std::string, std::pair<long, FolderView>> views_;  // folder -> (use order, view)
  long order_ = 0;
  bool save_views() const;
};

}  // namespace liria
}  // namespace sde
