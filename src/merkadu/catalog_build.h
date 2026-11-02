// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The catalog Merkadu shows, merged from what every part contributed: the
// AppStream components (Debian's DEP-11 catalog, the Flatpak remotes'
// catalogs), what each source has installed, the Snap Store's answers, the
// details fetched for app pages and APT's package lists. Built on a worker
// thread from immutable inputs (0.2 Alpha 3 merged it on the window's
// thread several times while starting, which kept the window from drawing
// on slow computers), together with the departments of every app.
//
// The rules that decide what an app may be installed from live here:
//   * a Debian origin needs a real package: one APT knows (once its lists
//     are read), named by the catalog, or owning the app's launcher in
//     /usr/share/applications; a metainfo file without a package (Flatpak
//     or Snap exports, a launcher of the user's own) never becomes one;
//   * a Flatpak origin names the remote and installation whose catalog it
//     came from (see appstream_catalog.h).
#pragma once

#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "categories.h"
#include "model.h"
#include "packages.h"

namespace sde {
namespace merkadu {

// APT's package lists, prepared on the APT thread.
struct PackageList {
  std::vector<Package> packages;                   // sorted by name
  std::unordered_map<std::string, size_t> index;   // name -> packages[i]
  std::vector<App> installed;                      // "deb:NAME" apps of the installed packages
  std::string error;
  // Builds the index and the installed apps from |packages|.
  void prepare();
  const Package *find(const std::string &name) const;
};

struct CatalogInputs {
  std::shared_ptr<const std::vector<App>> catalog;  // AppStream components
  std::shared_ptr<const std::vector<App>> installed[kSourceCount];
  std::vector<App> store;                           // Snap Store answers
  std::map<std::string, Origin> details;            // Origin::key() -> details
  std::map<std::string, std::string> owners;        // desktop id -> package
  std::shared_ptr<const PackageList> packages;      // nullptr until read
  std::vector<Source> order = {Source::Debian, Source::Flatpak, Source::Snap};
  const Categories *categories = nullptr;           // departments to index (optional)
  std::time_t now = 0;
  size_t fresh_count = 48;                          // how many "New & Updated" to keep
};

struct CatalogResult {
  Catalog catalog;
  int counts[kSourceCount] = {0, 0, 0};
  CategoryIndex departments;
  std::vector<std::string> fresh;  // the newest releases of apps that are not installed, newest first
  int dropped_debian = 0;          // Debian origins without a package APT knows
  // All Software's rows of the Flatpak and Snap apps (source_rows()).
  std::vector<Package> flatpak_rows, snap_rows;
};

CatalogResult build_catalog(const CatalogInputs &in);

}  // namespace merkadu
}  // namespace sde
