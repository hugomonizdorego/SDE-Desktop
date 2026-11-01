// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "highlights.h"

#include <algorithm>
#include <set>

#include "json.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

// An app worth highlighting: an application with an icon of its own.
bool presentable(const App &a) { return a.is_app() && (!a.icon.empty() || !a.icons.empty()); }

}  // namespace

std::vector<Highlight> choose_highlights(const Catalog &catalog, const HighlightSources &sources, size_t count,
                                         long day) {
  std::vector<std::vector<Highlight>> lists(4);
  for (auto &ref : sources.distribution)
    if (const App *a = resolve_reference(catalog, ref)) lists[0].push_back({a, Highlight::Distribution});
  for (auto &id : sources.fresh)
    if (const App *a = catalog.find(id)) lists[1].push_back({a, Highlight::New});
  for (auto &id : sources.popular) {
    // Flathub's id, or the app its Flatpak was merged into.
    const App *a = catalog.find(id);
    if (!a || !a->has_source(Source::Flatpak)) a = const_cast<Catalog &>(catalog).find_origin(Source::Flatpak, id);
    if (a && a->has_source(Source::Flatpak)) lists[2].push_back({a, Highlight::Popular});
  }
  for (auto &id : sources.featured_snaps)
    if (const App *a = catalog.find(id)) lists[3].push_back({a, Highlight::Featured});
  // Each list starts at another place every day.
  for (auto &l : lists)
    if (l.size() > 1) std::rotate(l.begin(), l.begin() + long(size_t(std::max(0L, day)) % l.size()), l.end());
  std::vector<Highlight> out;
  std::set<const App *> taken;
  // First apps that are not installed and have an icon, in turn from each
  // list; then the others.
  for (int pass = 0; pass < 2 && out.size() < count; ++pass) {
    std::vector<size_t> next(lists.size(), 0);
    bool more = true;
    while (more && out.size() < count) {
      more = false;
      for (size_t k = 0; k < lists.size() && out.size() < count; ++k) {
        auto &l = lists[k];
        while (next[k] < l.size()) {
          const Highlight &h = l[next[k]++];
          bool good = presentable(*h.app) && !h.app->installed();
          if (taken.count(h.app) || (pass == 0 && !good) || !h.app->is_app()) continue;
          taken.insert(h.app);
          out.push_back(h);
          more = true;
          break;
        }
        if (next[k] < l.size()) more = true;
      }
    }
  }
  return out;
}

std::vector<std::string> parse_flathub_collection(const std::string &json) {
  std::vector<std::string> out;
  Json root;
  if (!parse_json(json, root)) return out;
  const Json *items = root.is_array() ? &root : root.get("hits");
  if (!items) items = root.get("apps");
  if (!items || !items->is_array()) return out;
  for (auto &it : items->items) {
    std::string id = it.is_object() ? it.text("app_id", it.text("id")) : it.str;
    // App ids as Flathub writes them ("org.gimp.GIMP"), nothing else.
    if (id.empty() || id.size() > 255 || id.find('.') == std::string::npos) continue;
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

}  // namespace merkadu
}  // namespace sde
