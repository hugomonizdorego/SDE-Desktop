// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The installed applications for the Start menu, the panel search and the
// Start tiles, kept by the long-running panel (0.2 Alpha 1).
//
// Up to alpha52 every opening of the menu parsed every .desktop file (twice
// with the default tiles), and the search did it again for every keystroke
// through the default tiles. The catalog parses the files once, in short
// slices on idle after the panel started, and then watches the XDG
// applications directories with inotify: changes are collected for a
// moment (package managers write many files) and only new or changed files
// are parsed again. Each change publishes a new immutable snapshot. The menu
// and the search keep the snapshot they show, so pointers into it stay
// valid while a context menu or a drag runs, and move to a newer one when
// they can. Without inotify, consumers call poll(), which compares the
// cheap applications_stamp() as before.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "desktop_entry.h"
#include "menu_model.h"

namespace app_catalog {

struct Snapshot {
  unsigned generation = 0;
  // Applications (load_applications("SDE")) in the order of the menu's A-Z
  // list: digits and symbols first, then A-Z, then other scripts.
  std::vector<sde::DesktopEntry> apps;
  // Parallel to |apps|:
  std::vector<sde::menu::SearchKeys> keys;
  std::vector<std::string> letters;   // menu::index_letter() of the name
  std::vector<std::string> category;  // DesktopEntry::menu_category()
  std::vector<long long> mtime;       // of the .desktop file, seconds
  // Main categories with applications, in menu_categories() order.
  std::vector<std::string> categories;
  std::unordered_map<std::string, size_t> by_id;

  const sde::DesktopEntry *find(const std::string &id) const;
  // Index of an entry of |apps| (for the parallel vectors).
  size_t index(const sde::DesktopEntry *e) const { return size_t(e - apps.data()); }
};
using SnapshotPtr = std::shared_ptr<const Snapshot>;

// The applications now; parses them at once if they are not loaded yet.
SnapshotPtr current();
// True once the first load finished (current() returns without parsing).
bool ready();
// Starts watching the applications directories and loads them in slices
// on idle (called once after the panel is shown).
void start();
// Without inotify (or when it failed): compares applications_stamp() and
// refreshes when it changed. With inotify it only looks whether a data
// directory that did not exist (nothing to watch) was created since.
void poll();
bool watching();
// Refreshes now (tests, and after a watch overflowed).
void refresh();

// |fn(data)| runs after a new snapshot was published.
void subscribe(void (*fn)(void *), void *data);
void unsubscribe(void *data);

}  // namespace app_catalog
