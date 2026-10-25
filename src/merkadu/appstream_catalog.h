// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The software catalog from AppStream metadata, read with libappstream:
// the distribution's catalog (Debian's DEP-11 data, downloaded by APT into
// /var/lib/apt/lists and /var/lib/swcatalog, with its icons), the metainfo
// files of installed software and the AppStream data of the system's
// Flatpak remotes (/var/lib/flatpak/appstream). Components with a package
// name become Debian origins, components with a Flatpak bundle Flatpak
// origins. Blocking: call it from a worker thread.
#pragma once

#include <string>
#include <vector>

#include "model.h"

typedef struct _AsComponent AsComponent;

namespace sde {
namespace merkadu {

struct CatalogData {
  std::vector<App> apps;
  std::string error;  // why nothing could be loaded
  int debian = 0, flatpak = 0;
};

// The system's catalog. |extra_dirs| are additional catalog directories
// (with xml/ or yaml/ subdirectories, like /usr/share/swcatalog).
CatalogData load_appstream(const std::vector<std::string> &extra_dirs = {});
// The components of one AppStream file (catalog XML or DEP-11 YAML, or a
// metainfo file), for the tests.
CatalogData load_appstream_file(const std::string &path);
// One component as an app; false for kinds Merkadu does not list (add-ons,
// codecs, runtimes, firmware ...) and components without a source.
bool component_to_app(AsComponent *cpt, App &out);

}  // namespace merkadu
}  // namespace sde
