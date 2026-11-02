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
  // Remotes with AppStream data: the system installation's
  // (/var/lib/flatpak/appstream/NAME) by name, others as INSTALLATION/NAME.
  std::vector<std::string> flatpak_remotes;
  // The DEP-11 catalogs AppStream has not linked yet (no "appstreamcli
  // refresh" after the download): Merkadu reads them from here.
  std::vector<std::string> unlinked;
};
// |root| prefixes every path (the tests).
CatalogSources catalog_sources(const std::string &root = "");

// A Flatpak installation: "system" (/var/lib/flatpak), "user"
// (~/.local/share/flatpak) or one of /etc/flatpak/installations.d.
struct FlatpakInstallationDir {
  std::string name;
  std::string path;
};
// The installations of this system and user ($FLATPAK_SYSTEM_DIR and
// $FLATPAK_USER_DIR as Flatpak reads them; |root| prefixes the system
// paths, for the tests).
std::vector<FlatpakInstallationDir> flatpak_installations(const std::string &root = "");

// The AppStream data of one remote of one installation
// (INSTALLATION/appstream/REMOTE/ARCH/active). Flathub's file says
// origin="flatpak" for every app: the remote an app is installed from is
// the directory its catalog was found in, never that attribute (0.2
// Alpha 3 asked libflatpak for the remote "flatpak" and every installation
// from Flathub failed).
struct FlatpakCatalog {
  std::string installation;  // FlatpakInstallationDir::name
  std::string remote;        // the remote's name
  std::string dir;           // .../active (with icons/64x64 and icons/128x128)
  std::string file;          // dir/appstream.xml.gz or dir/appstream.xml
};
// The catalogs of the configured, enabled remotes of |installations| for
// |arch| (Flatpak's name of this machine's architecture when empty).
std::vector<FlatpakCatalog> flatpak_catalogs(const std::vector<FlatpakInstallationDir> &installations,
                                             const std::string &arch = "");
// Flatpak's name for this machine's architecture ("x86_64", "aarch64", "i386", "arm").
std::string flatpak_arch();

struct CatalogData {
  std::vector<App> apps;
  std::string error;  // why nothing could be loaded
  int debian = 0, flatpak = 0;
  CatalogSources sources;
  double seconds = 0;
};

// The system's catalog: the distribution's (libappstream: DEP-11, the
// metainfo of installed software) and the Flatpak remotes'. |extra_dirs|
// are additional catalog directories (with xml/ or yaml/ subdirectories,
// like /usr/share/swcatalog).
CatalogData load_appstream(const std::vector<std::string> &extra_dirs = {});
// The apps of one Flatpak remote's catalog, as installed from that remote
// of that installation.
CatalogData load_flatpak_catalog(const FlatpakCatalog &catalog);
// The components of one AppStream file (catalog XML or DEP-11 YAML, or a
// metainfo file), for the tests.
CatalogData load_appstream_file(const std::string &path);
// Where a component comes from when it has a Flatpak bundle: the remote and
// installation of its catalog and the directory of its cached icons.
struct ComponentContext {
  std::string remote;
  std::string installation = "system";
  std::string icons_dir;  // .../active/icons
};
// One component as an app; false for kinds Merkadu does not list (add-ons,
// codecs, runtimes, firmware ...). Components with a package name get a
// Debian origin, with a Flatpak bundle a Flatpak origin; a metainfo file
// with neither gets none (App::needs_owner).
bool component_to_app(AsComponent *cpt, App &out, const ComponentContext *ctx = nullptr);

}  // namespace merkadu
}  // namespace sde
