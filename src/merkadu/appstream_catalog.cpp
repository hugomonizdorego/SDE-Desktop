// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "appstream_catalog.h"

#include <appstream.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

#include "util.h"

namespace sde {
namespace merkadu {

namespace {

std::string str(const char *s) { return s ? s : ""; }

// The kinds Merkadu lists.
bool listed_kind(AsComponentKind k) {
  return k == AS_COMPONENT_KIND_DESKTOP_APP || k == AS_COMPONENT_KIND_CONSOLE_APP ||
         k == AS_COMPONENT_KIND_WEB_APP || k == AS_COMPONENT_KIND_FONT;
}

bool is_dep11_catalog(const std::string &name) {
  return name.find("_dep11_Components-") != std::string::npos &&
         (ends_with(name, ".yml.gz") || ends_with(name, ".yml") || ends_with(name, ".yml.xz"));
}

// A cached icon AppStream could not resolve: the usual catalog icon
// directories, |size| first, then the largest (Debian's DEP-11 icons,
// Flatpak remotes).
std::string find_cached_icon(const std::string &origin, const std::string &name, guint size, guint scale) {
  if (origin.empty() || name.empty() || name.find('/') != std::string::npos) return "";
  static const char *roots[] = {"/var/lib/swcatalog/icons", "/var/cache/swcatalog/icons",
                                "/usr/share/swcatalog/icons", "/var/lib/app-info/icons",
                                "/var/cache/app-info/icons"};
  std::vector<std::string> sizes;
  if (size > 0)
    sizes.push_back(std::to_string(size) + "x" + std::to_string(size) + (scale > 1 ? "@" + std::to_string(scale) : ""));
  for (const char *s : {"128x128", "64x64@2", "64x64", "48x48"}) sizes.push_back(s);
  for (const char *root : roots)
    for (auto &s : sizes) {
      std::string p = std::string(root) + "/" + origin + "/" + s + "/" + name;
      if (file_exists(p)) return p;
    }
  return "";
}

// Every icon of the component: theme names, files of the catalog's icon
// directories and the catalog's media URLs, with their sizes.
void add_icons(AsComponent *cpt, App &app) {
  GPtrArray *icons = as_component_get_icons(cpt);
  for (guint i = 0; icons && i < icons->len; ++i) {
    AsIcon *icon = AS_ICON(g_ptr_array_index(icons, i));
    guint scale = std::max(1u, as_icon_get_scale(icon));
    int size = int(as_icon_get_width(icon) * scale);
    switch (as_icon_get_kind(icon)) {
      case AS_ICON_KIND_STOCK:
        add_icon(app, IconRef{str(as_icon_get_name(icon)), 0});
        break;
      case AS_ICON_KIND_CACHED:
      case AS_ICON_KIND_LOCAL: {
        std::string f = str(as_icon_get_filename(icon));
        if (!f.empty() && f[0] != '/')
          f = find_cached_icon(str(as_component_get_origin(cpt)), f, as_icon_get_width(icon), scale);
        if (!f.empty() && file_exists(f)) add_icon(app, IconRef{f, size});
        break;
      }
      case AS_ICON_KIND_REMOTE: {
        std::string url = str(as_icon_get_url(icon));
        if (starts_with(url, "https://")) add_icon(app, IconRef{url, size});
        break;
      }
      default:
        break;
    }
  }
  // app.icon: the best one for list rows.
  std::vector<std::string> best = icon_candidates(app, 64);
  if (!best.empty()) app.icon = best.front();
}

void add_screenshots(AsComponent *cpt, App &app) {
  GPtrArray *shots = as_component_get_screenshots_all(cpt);
  for (guint i = 0; shots && i < shots->len && app.screenshots.size() < 8; ++i) {
    AsScreenshot *s = AS_SCREENSHOT(g_ptr_array_index(shots, i));
    if (as_screenshot_get_media_kind(s) != AS_SCREENSHOT_MEDIA_KIND_IMAGE) continue;
    GPtrArray *images = as_screenshot_get_images_all(s);
    Screenshot shot;
    guint best_thumb = 0;
    for (guint k = 0; images && k < images->len; ++k) {
      AsImage *img = AS_IMAGE(g_ptr_array_index(images, k));
      std::string url = str(as_image_get_url(img));
      if (!starts_with(url, "https://") && !starts_with(url, "http://") && !starts_with(url, "/")) continue;
      if (as_image_get_locale(img) && std::strcmp(as_image_get_locale(img), "C") != 0) continue;
      if (as_image_get_kind(img) == AS_IMAGE_KIND_SOURCE) {
        shot.image = url;
      } else {
        // The thumbnail closest to 624 px wide (what the details page shows).
        guint w = as_image_get_width(img);
        auto dist = [](guint x) { return x > 624 ? x - 624 : 624 - x; };
        if (shot.thumbnail.empty() || dist(w) < dist(best_thumb)) {
          shot.thumbnail = url;
          best_thumb = w;
        }
      }
    }
    if (shot.image.empty()) shot.image = shot.thumbnail;
    if (shot.thumbnail.empty()) shot.thumbnail = shot.image;
    if (shot.image.empty()) continue;
    shot.caption = str(as_screenshot_get_caption(s));
    // The default screenshot first.
    if (as_screenshot_get_kind(s) == AS_SCREENSHOT_KIND_DEFAULT)
      app.screenshots.insert(app.screenshots.begin(), shot);
    else
      app.screenshots.push_back(shot);
  }
}

void add_releases(AsComponent *cpt, App &app) {
  AsReleaseList *list = as_component_get_releases_plain(cpt);
  if (!list) return;
  guint n = as_release_list_get_size(list);
  for (guint i = 0; i < n && app.releases.size() < 5; ++i) {
    AsRelease *r = as_release_list_index_safe(list, i);
    if (!r || as_release_get_kind(r) == AS_RELEASE_KIND_DEVELOPMENT) continue;
    Release rel;
    rel.version = str(as_release_get_version(r));
    rel.timestamp = std::time_t(as_release_get_timestamp(r));
    rel.description = description_text(str(as_release_get_description(r)));
    app.releases.push_back(rel);
  }
  std::stable_sort(app.releases.begin(), app.releases.end(),
                   [](const Release &a, const Release &b) { return a.timestamp > b.timestamp; });
}

std::vector<std::string> strings_of(GPtrArray *a) {
  std::vector<std::string> out;
  for (guint i = 0; a && i < a->len; ++i) out.push_back(static_cast<const char *>(g_ptr_array_index(a, i)));
  return out;
}

void add_components(AsComponentBox *box, CatalogData &data) {
  guint n = box ? as_component_box_get_size(box) : 0;
  data.apps.reserve(data.apps.size() + n);
  for (guint i = 0; i < n; ++i) {
    App app;
    if (!component_to_app(as_component_box_index_safe(box, i), app)) continue;
    for (auto &o : app.origins) (o.source == Source::Flatpak ? data.flatpak : data.debian)++;
    data.apps.push_back(std::move(app));
  }
}

std::string cache_home() {
  std::string base = env("XDG_CACHE_HOME");
  return base.empty() ? join_path(home_dir(), ".cache") : base;
}

// A catalog directory of Merkadu's own with links to the DEP-11 files that
// AppStream does not read yet ("appstreamcli refresh" did not run after
// they were downloaded); "" when there is nothing to read.
std::string link_unlinked(const std::vector<std::string> &files) {
  std::string dir = join_path(cache_home(), "merkadu/dep11");
  std::string yaml = dir + "/yaml";
  if (!make_dirs(yaml)) return "";
  std::set<std::string> want;
  for (auto &f : files) want.insert(basename_of(f));
  for (auto &name : list_dir(yaml))
    if (!want.count(name)) unlink((yaml + "/" + name).c_str());
  int linked = 0;
  for (auto &f : files) {
    std::string link = yaml + "/" + basename_of(f);
    char target[4096];
    ssize_t n = readlink(link.c_str(), target, sizeof target - 1);
    if (n > 0 && std::string(target, size_t(n)) == f) {
      ++linked;
      continue;
    }
    unlink(link.c_str());
    if (symlink(f.c_str(), link.c_str()) == 0) ++linked;
  }
  return linked ? dir : "";
}

}  // namespace

CatalogSources catalog_sources(const std::string &root) {
  CatalogSources s;
  std::string lists = root + "/var/lib/apt/lists";
  s.appstream_apt = file_exists(root + "/etc/apt/apt.conf.d/50appstream");
  std::set<std::string> linked;
  std::string linkdir = root + "/var/lib/swcatalog/yaml";
  for (auto &name : list_dir(linkdir))
    if (file_exists(linkdir + "/" + name)) linked.insert(name);  // not a dangling link
  for (auto &name : list_dir(lists)) {
    if (is_dep11_catalog(name)) {
      ++s.dep11;
      if (linked.count(name))
        ++s.dep11_linked;
      else
        s.unlinked.push_back(lists + "/" + name);
    } else if (name.find("_dep11_icons-") != std::string::npos) {
      ++s.dep11_icons;
    } else if (ends_with(name, "_Release") || ends_with(name, "_InRelease")) {
      struct stat st;
      if (stat((lists + "/" + name).c_str(), &st) == 0) s.lists_time = std::max(s.lists_time, std::time_t(st.st_mtime));
    }
  }
  std::sort(s.unlinked.begin(), s.unlinked.end());
  std::string icons = root + "/var/lib/swcatalog/icons";
  for (auto &origin : list_dir(icons))
    if (!list_dir(icons + "/" + origin).empty()) ++s.icon_origins;
  for (const char *d : {"/usr/share/swcatalog/xml", "/usr/share/swcatalog/yaml", "/var/cache/swcatalog/xml",
                        "/var/cache/swcatalog/yaml"})
    s.other_catalogs += int(list_dir(root + d).size());
  std::string fp = root + "/var/lib/flatpak/appstream";
  for (auto &remote : list_dir(fp)) {
    bool data = false;
    for (auto &arch : list_dir(fp + "/" + remote)) {
      std::string active = fp + "/" + remote + "/" + arch + "/active";
      data = data || file_exists(active + "/appstream.xml.gz") || file_exists(active + "/appstream.xml");
    }
    if (data) s.flatpak_remotes.push_back(remote);
  }
  std::sort(s.flatpak_remotes.begin(), s.flatpak_remotes.end());
  return s;
}

bool component_to_app(AsComponent *cpt, App &out) {
  if (!cpt || !listed_kind(as_component_get_kind(cpt))) return false;
  App app;
  app.appstream_id = str(as_component_get_id(cpt));
  app.id = normalize_id(app.appstream_id);
  if (app.id.empty()) return false;
  app.kind = str(as_component_kind_to_string(as_component_get_kind(cpt)));
  app.name = clean_text(str(as_component_get_name(cpt)), 200);
  app.summary = clean_text(str(as_component_get_summary(cpt)), 400);
  app.description = clean_text(description_text(str(as_component_get_description(cpt))), 16384);
  if (app.name.empty()) return false;
  app.categories = strings_of(as_component_get_categories(cpt));
  app.keywords = strings_of(as_component_get_keywords(cpt));
  if (AsDeveloper *dev = as_component_get_developer(cpt)) app.developer = str(as_developer_get_name(dev));
  app.license = str(as_component_get_project_license(cpt));
  app.homepage = str(as_component_get_url(cpt, AS_URL_KIND_HOMEPAGE));
  app.bugtracker = str(as_component_get_url(cpt, AS_URL_KIND_BUGTRACKER));
  app.help = str(as_component_get_url(cpt, AS_URL_KIND_HELP));
  app.donation = str(as_component_get_url(cpt, AS_URL_KIND_DONATION));
  if (AsLaunchable *l = as_component_get_launchable(cpt, AS_LAUNCHABLE_KIND_DESKTOP_ID))
    app.desktop_ids = strings_of(as_launchable_get_entries(l));
  add_icons(cpt, app);
  add_screenshots(cpt, app);
  add_releases(cpt, app);

  std::string origin = str(as_component_get_origin(cpt));
  if (AsBundle *b = as_component_get_bundle(cpt, AS_BUNDLE_KIND_FLATPAK)) {
    Origin o;
    o.source = Source::Flatpak;
    o.id = str(as_bundle_get_id(b));
    o.remote = origin;
    o.installation = "system";
    o.license = app.license;
    o.publisher = app.developer;
    if (!valid_flatpak_ref(o.id)) return false;
    if (!app.releases.empty()) o.version = app.releases.front().version;
    app.origins.push_back(o);
  } else {
    gchar **pkgs = as_component_get_pkgnames(cpt);
    for (int i = 0; pkgs && pkgs[i]; ++i)
      if (valid_package_name(pkgs[i])) app.pkgnames.push_back(pkgs[i]);
    // Metainfo files of installed software name no package; the catalog
    // usually has the same component with one. Packages are found for the
    // rest by their desktop file (see the PackageKit backend).
    Origin o;
    o.source = Source::Debian;
    o.id = app.pkgnames.empty() ? std::string() : app.pkgnames.front();
    o.remote = origin;
    o.license = app.license;
    app.origins.push_back(o);
  }
  out = std::move(app);
  return true;
}

CatalogData load_appstream(const std::vector<std::string> &extra_dirs) {
  auto t0 = std::chrono::steady_clock::now();
  CatalogData data;
  data.sources = catalog_sources();
  std::vector<std::string> extra = extra_dirs;
  if (!data.sources.unlinked.empty()) {
    std::string own = link_unlinked(data.sources.unlinked);
    if (!own.empty()) extra.push_back(own);
  }
  AsPool *pool = as_pool_new();
  as_pool_set_flags(pool, AsPoolFlags(AS_POOL_FLAG_LOAD_OS_CATALOG | AS_POOL_FLAG_LOAD_OS_METAINFO |
                                      AS_POOL_FLAG_LOAD_FLATPAK));
  for (auto &d : extra) as_pool_add_extra_data_location(pool, d.c_str(), AS_FORMAT_STYLE_CATALOG);
  GError *err = nullptr;
  if (!as_pool_load(pool, nullptr, &err)) {
    // Partial data is still usable (a broken file of one remote).
    data.error = err ? err->message : "AppStream data could not be loaded";
    g_clear_error(&err);
  }
  AsComponentBox *box = as_pool_get_components(pool);
  add_components(box, data);
  if (box) g_object_unref(box);
  g_object_unref(pool);
  if (!data.apps.empty()) data.error.clear();
  data.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return data;
}

CatalogData load_appstream_file(const std::string &path) {
  CatalogData data;
  AsMetadata *md = as_metadata_new();
  GFile *file = g_file_new_for_path(path.c_str());
  GError *err = nullptr;
  bool yaml = ends_with(path, ".yml") || ends_with(path, ".yaml") || ends_with(path, ".yml.gz");
  as_metadata_set_format_style(md, as_metadata_file_guess_style(path.c_str()));
  if (!as_metadata_parse_file(md, file, yaml ? AS_FORMAT_KIND_YAML : AS_FORMAT_KIND_XML, &err)) {
    data.error = err ? err->message : "cannot parse " + path;
    g_clear_error(&err);
  } else {
    add_components(as_metadata_get_components(md), data);
  }
  g_object_unref(file);
  g_object_unref(md);
  return data;
}

}  // namespace merkadu
}  // namespace sde
