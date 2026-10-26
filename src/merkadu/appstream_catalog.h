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
//
// How the Debian catalog gets there: the appstream package makes APT
// download it (/etc/apt/apt.conf.d/50appstream) at the next "apt update" or
// PackageKit refresh, and its hook then runs "appstreamcli refresh" as root,
// which links the files into /var/lib/swcatalog/yaml and unpacks the icons.
// A system where appstream was installed after the last refresh has no
// catalog at all; catalog_sources() tells the window what is missing.
#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "model.h"

typedef struct _AsComponent AsComponent;

namespace sde {
namespace merkadu {

// Where the catalog comes from on this system.
struct CatalogSources {
  bool appstream_apt = false;  // /etc/apt/apt.conf.d/50appstream: APT downloads the catalog
  int dep11 = 0;               // catalogs APT downloaded (*_dep11_Components-*.yml.gz)
  int dep11_linked = 0;        // of them where AppStream reads them (/var/lib/swcatalog/yaml)
  int dep11_icons = 0;         // icon archives APT downloaded (*_dep11_icons-*.tar.gz)
  int icon_origins = 0;        // catalogs with unpacked icons (/var/lib/swcatalog/icons/ORIGIN)
  int other_catalogs = 0;      // other catalog files (/usr/share/swcatalog, /var/cache/swcatalog)
  std::time_t lists_time = 0;  // the last refresh of APT's lists (newest Release file)
  std::vector<std::string> flatpak_remotes;  // remotes with AppStream data (/var/lib/flatpak/appstream/NAME)
  // The DEP-11 catalogs AppStream has not linked yet (no "appstreamcli
  // refresh" after the download): Merkadu reads them from here.
  std::vector<std::string> unlinked;
};
// |root| prefixes every path (the tests).
CatalogSources catalog_sources(const std::string &root = "");

struct CatalogData {
  std::vector<App> apps;
  std::string error;  // why nothing could be loaded
  int debian = 0, flatpak = 0;
  CatalogSources sources;
  double seconds = 0;
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
