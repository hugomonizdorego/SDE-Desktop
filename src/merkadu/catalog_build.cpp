// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "catalog_build.h"

#include <algorithm>

#include "util.h"

namespace sde {
namespace merkadu {

void PackageList::prepare() {
  index.clear();
  index.reserve(packages.size());
  for (size_t i = 0; i < packages.size(); ++i) index.emplace(packages[i].name, i);
  installed.clear();
  for (auto &p : packages) {
    p.lower_summary = lower(p.summary);
    if (!p.installed) continue;
    App a;
    a.id = "deb:" + p.name;
    a.name = p.name;
    Origin o;
    o.source = Source::Debian;
    o.id = p.name;
    o.installed = true;
    o.installed_version = p.installed_version;
    o.update = p.upgradable;
    o.version = p.candidate_version;
    o.installed_size = p.current_size;
    a.origins.push_back(o);
    installed.push_back(std::move(a));
  }
}

const Package *PackageList::find(const std::string &name) const {
  auto it = index.find(name);
  return it == index.end() ? nullptr : &packages[it->second];
}

CatalogResult build_catalog(const CatalogInputs &in) {
  CatalogResult r;
  Catalog &catalog = r.catalog;
  const PackageList *pl = in.packages && !in.packages->packages.empty() ? in.packages.get() : nullptr;
  if (in.catalog)
    for (const App &src : *in.catalog) {
      App a = src;
      // A metainfo file without a package: the package that owns its
      // launcher, if one does.
      if (a.needs_owner)
        for (auto &d : a.desktop_ids) {
          auto it = in.owners.find(d);
          if (it == in.owners.end()) continue;
          Origin o;
          o.source = Source::Debian;
          o.id = it->second;
          o.license = a.license;
          a.origins.push_back(o);
          if (std::find(a.pkgnames.begin(), a.pkgnames.end(), o.id) == a.pkgnames.end())
            a.pkgnames.push_back(o.id);
          break;
        }
      catalog.add(std::move(a));
    }
  for (auto &a : in.store) catalog.add(a);
  for (int s = 0; s < kSourceCount; ++s) {
    if (!in.installed[s]) continue;
    for (auto &a : *in.installed[s]) {
      if (s == int(Source::Debian) && starts_with(a.id, "deb:") && !a.origins.empty()) {
        // An installed package: it marks the app it belongs to, if any.
        App *target = catalog.find_origin(Source::Debian, a.origins.front().id);
        if (!target) continue;
        App mark;
        mark.id = target->id;
        mark.origins = a.origins;
        catalog.add(std::move(mark));
      } else {
        catalog.add(a);
      }
    }
  }
  // Details fetched for app pages (sizes, versions).
  if (!in.details.empty())
    for (auto &a : catalog.apps())
      for (auto &o : a.origins) {
        auto it = in.details.find(o.key());
        if (it != in.details.end()) merge_origin(a, it->second);
      }
  for (auto &a : catalog.apps()) {
    // A Debian origin needs a package: one the catalog names that APT
    // knows (once its lists are read); anything else is no way to install
    // the app from Debian.
    auto bad = [&](const Origin &o) {
      if (o.id.empty()) return true;
      if (o.source != Source::Debian || o.installed || !pl) return false;
      const Package *p = pl->find(o.id);
      return !p || (p->candidate_version.empty() && !p->installed);
    };
    size_t before = a.origins.size();
    a.origins.erase(std::remove_if(a.origins.begin(), a.origins.end(), bad), a.origins.end());
    for (size_t i = a.origins.size(); i < before; ++i) ++r.dropped_debian;
    if (!pl) continue;
    // The package lists know the version, sizes and section of every
    // Debian app.
    for (auto &o : a.origins) {
      if (o.source != Source::Debian) continue;
      const Package *p = pl->find(o.id);
      if (!p) continue;
      if (o.version.empty()) o.version = p->candidate_version;
      if (o.download_size < 0) o.download_size = p->download_size;
      if (o.installed_size < 0) o.installed_size = o.installed ? p->current_size : p->installed_size;
      if (a.section.empty()) a.section = p->section;
    }
  }
  catalog.remove_empty();
  catalog.sort_origins(in.order);
  for (auto &a : catalog.apps()) {
    for (int s = 0; s < kSourceCount; ++s)
      if (a.has_source(Source(s))) ++r.counts[s];
    a.prepare_search();
  }
  if (in.categories) r.departments.build(*in.categories, catalog);
  // All Software's rows of the other sources, by department.
  std::vector<std::string> department(catalog.size());
  if (r.departments.built())
    for (size_t i = 0; i < catalog.size(); ++i)
      if (const Category *c = r.departments.department(i)) department[i] = c->id;
  r.flatpak_rows = source_rows(catalog, Source::Flatpak, department);
  r.snap_rows = source_rows(catalog, Source::Snap, department);
  // New & Updated: the newest releases of apps that are not installed.
  std::vector<const App *> fresh;
  std::time_t now = in.now ? in.now : std::time(nullptr);
  for (auto &a : catalog.apps())
    if (a.is_app() && !a.installed() && a.newest_release() > 0 && a.newest_release() <= now + 86400)
      fresh.push_back(&a);
  std::stable_sort(fresh.begin(), fresh.end(),
                   [](const App *x, const App *y) { return x->newest_release() > y->newest_release(); });
  for (size_t i = 0; i < fresh.size() && i < in.fresh_count; ++i) r.fresh.push_back(fresh[i]->id);
  return r;
}

}  // namespace merkadu
}  // namespace sde
