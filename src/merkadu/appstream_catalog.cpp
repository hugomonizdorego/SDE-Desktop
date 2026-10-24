// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "appstream_catalog.h"

#include <appstream.h>
#include <gio/gio.h>

#include <algorithm>
#include <cstring>

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

// A cached icon AppStream could not resolve: the usual catalog icon
// directories, largest first (Debian's DEP-11 icons, Flatpak remotes).
std::string find_cached_icon(const std::string &origin, const std::string &name) {
  if (origin.empty() || name.empty() || name.find('/') != std::string::npos) return "";
  static const char *roots[] = {"/var/lib/swcatalog/icons", "/var/cache/swcatalog/icons",
                                "/usr/share/swcatalog/icons", "/var/lib/app-info/icons",
                                "/var/cache/app-info/icons"};
  static const char *sizes[] = {"128x128", "64x64@2", "64x64", "48x48"};
  for (const char *root : roots)
    for (const char *size : sizes) {
      std::string p = std::string(root) + "/" + origin + "/" + size + "/" + name;
      if (file_exists(p)) return p;
    }
  return "";
}

// The best icon: a file of at least 64 px (the largest up to 128 px), a
// stock name, then a remote URL.
std::string best_icon(AsComponent *cpt) {
  GPtrArray *icons = as_component_get_icons(cpt);
  std::string file, stock, remote;
  guint file_size = 0;
  for (guint i = 0; icons && i < icons->len; ++i) {
    AsIcon *icon = AS_ICON(g_ptr_array_index(icons, i));
    switch (as_icon_get_kind(icon)) {
      case AS_ICON_KIND_STOCK:
        if (stock.empty()) stock = str(as_icon_get_name(icon));
        break;
      case AS_ICON_KIND_CACHED:
      case AS_ICON_KIND_LOCAL: {
        std::string f = str(as_icon_get_filename(icon));
        if (!f.empty() && f[0] != '/') f = find_cached_icon(str(as_component_get_origin(cpt)), f);
        if (f.empty() || !file_exists(f)) break;
        guint size = as_icon_get_width(icon) * std::max(1u, as_icon_get_scale(icon));
        if (size == 0) size = 64;
        // Prefer sizes from 64 to 128 px; among them the larger one.
        auto score = [](guint s) { return s > 128 ? 128 - (s - 128) / 8 : s; };
        if (file.empty() || score(size) > score(file_size)) {
          file = f;
          file_size = size;
        }
        break;
      }
      case AS_ICON_KIND_REMOTE:
        if (remote.empty() && starts_with(str(as_icon_get_url(icon)), "https://")) remote = as_icon_get_url(icon);
        break;
      default:
        break;
    }
  }
  if (!file.empty()) return file;
  if (!stock.empty()) return stock;
  return remote;
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

}  // namespace

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
  app.icon = best_icon(cpt);
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
  CatalogData data;
  AsPool *pool = as_pool_new();
  as_pool_set_flags(pool, AsPoolFlags(AS_POOL_FLAG_LOAD_OS_CATALOG | AS_POOL_FLAG_LOAD_OS_METAINFO |
                                      AS_POOL_FLAG_LOAD_FLATPAK));
  for (auto &d : extra_dirs) as_pool_add_extra_data_location(pool, d.c_str(), AS_FORMAT_STYLE_CATALOG);
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
