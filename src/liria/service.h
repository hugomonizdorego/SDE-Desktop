// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// One Liria per session, and the D-Bus interfaces of a file manager:
//
//   org.debion.Liria  /org/debion/Liria  org.debion.Liria
//     Open(as locations, as select)   a new window of the running Liria
//                                     (what `liria URI` does when Liria runs)
//     Quit()
//   org.freedesktop.FileManager1  /org/freedesktop/FileManager1
//     ShowFolders(as uris, s startup_id)
//     ShowItems(as uris, s startup_id)          browsers' "Show in folder"
//     ShowItemProperties(as uris, s startup_id)
//
// D-Bus starts Liria for FileManager1 when no file manager runs
// (data/dbus/org.debion.Liria.FileManager1.service: liria --service).
#pragma once

#include <string>
#include <vector>

namespace sde {
namespace liria {

extern const char *const kBusName;  // "org.debion.Liria"

// Asks a running Liria to open |locations| (and select |select|) in a new
// window. True when one did.
bool hand_off(const std::vector<std::string> &locations, const std::vector<std::string> &select);
bool ask_running_to_quit();
// Owns org.debion.Liria (and org.freedesktop.FileManager1 when no other
// file manager has it) and answers the calls. False when another Liria
// owns the name.
bool start_dbus_service();

// file:// URIs and paths relative to |cwd| as Liria's locations.
std::string location_of_argument(const std::string &arg, const std::string &cwd);

}  // namespace liria
}  // namespace sde
