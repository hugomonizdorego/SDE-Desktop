// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's Storage (0.2.0 Alpha 4): what the apps of Flatpak and Snap keep
// on this computer, what Clean Up can free, and the Merkadu folder, which
// shows the data of both in one place of the home folder. Display-free, for
// the unit tests.
//
// Where Flatpak and snapd keep things, and why it stays there
// (docs/MERKADU.md, "One place for Flatpak and Snap"):
//
//   ~/.var/app/ID          each Flatpak app's settings, data and cache; the
//                          path is fixed in Flatpak (flatpak_get_data_dir())
//   ~/snap/NAME            each snap's data (~/.snap/data/NAME with snapd's
//   ~/.snap/data/NAME      hidden snap folder); snapd computes it from the
//                          home folder, the AppArmor rules of every snap
//                          allow writes there only and snap-confine refuses
//                          ~/snap as a symbolic link
//   /var/lib/flatpak,      the installed Flatpak apps and runtimes (system,
//   ~/.local/share/flatpak and the user's own)
//   /var/lib/snapd, /snap  the installed snaps, their revisions, snapshots
//
// ~/Merkadu (MERKADU_FOLDER) holds two links, Flatpak -> ~/.var/app and
// Snap -> ~/snap or ~/.snap/data, and a README: nothing is moved, so
// Flatpak and snapd find everything where they look for it, and removing
// the folder removes only the links.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "model.h"

namespace sde {
namespace merkadu {

struct StoragePaths {
  std::string home;
  std::string flatpak_data;  // ~/.var/app
  std::string snap_data;     // ~/snap
  std::string snap_hidden;   // ~/.snap/data
  std::string folder;        // ~/Merkadu
};
// The paths of the user's home folder; |folder_name| is the Merkadu
// folder's ([Folders] name=).
StoragePaths storage_paths(const std::string &folder_name = "Merkadu");

// The bytes a file or folder takes on the disk (allocated blocks; hard
// links once), without following symbolic links or crossing into other
// file systems. -1 when it cannot be read.
long long disk_usage(const std::string &path);

// A folder of app data in the home folder.
struct DataDir {
  Source source = Source::Flatpak;
  std::string id;          // the Flatpak app id or the snap's name
  std::string path;
  long long size = -1;     // bytes on the disk, -1 when not measured
  bool installed = true;   // false: data its app left behind when it was removed
};
// The data folders of Flatpak apps and snaps. |flatpak_ids| and
// |snap_names| are the installed apps; a source whose list is not known
// (|*_known| false: not loaded, not installed) has no left-over data, so
// that nothing of it is ever offered for deletion by mistake.
std::vector<DataDir> scan_app_data(const StoragePaths &p, const std::set<std::string> &flatpak_ids,
                                   bool flatpak_known, const std::set<std::string> &snap_names, bool snap_known,
                                   bool measure);
// Deletes a data folder: only a folder (never a link) directly in
// ~/.var/app, ~/snap or ~/.snap/data, and nothing outside it.
bool remove_app_data(const StoragePaths &p, const std::string &path, std::string &error);

// The Merkadu folder.
struct FolderState {
  bool exists = false;   // ~/Merkadu is a folder
  bool blocked = false;  // something else is called ~/Merkadu (a file)
  bool flatpak = false;  // its Flatpak link points to ~/.var/app
  bool snap = false;     // its Snap link points to the snap data folder
  std::string snap_target;  // the snap data folder that is used ("" none yet)
  bool readme = false;
};
FolderState folder_state(const StoragePaths &p);
// Creates the folder or brings it up to date: the links (~/.var/app is
// made when it is missing, as Flatpak itself would; the Snap link only
// when snapd made a data folder) and the README. Files of the user in the
// folder, and folders of the user called Flatpak or Snap, stay as they are.
bool make_folder(const StoragePaths &p, const std::string &readme_name, const std::string &readme,
                 std::string &error);
// Undoes make_folder(): the links, the README when it is Merkadu's, the
// folder when nothing else is in it. Never the data the links point to.
bool remove_folder(const StoragePaths &p, const std::string &readme_name, const std::string &readme,
                   std::string &error);

// An old revision of a snap that snapd keeps to go back to (/v2/snaps
// ?select=all: the revisions that are not "active").
struct SnapRevision {
  std::string name, title, revision, version;
  bool active = false;
  long long size = -1;  // installed-size
};
bool parse_snap_revisions(const std::string &json, std::vector<SnapRevision> &out);

}  // namespace merkadu
}  // namespace sde
