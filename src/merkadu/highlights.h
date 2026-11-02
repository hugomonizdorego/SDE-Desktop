// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// What Home highlights (0.2 Alpha 3.1: Alpha 3 showed one fixed app, the
// first of the distribution's list). The highlights rotate through apps of
// four kinds, taken in turn so that none fills the row alone:
//
//   * the distribution's choice ([Highlights] apps= of merkadu.conf),
//   * new releases (the newest AppStream releases of the catalogs),
//   * apps that are popular on Flathub (its "popular" and "trending"
//     collections, downloaded when Flathub is set up and kept for offline),
//   * the Snap Store's featured snaps (when snapd runs).
//
// Every list starts at another place each day; apps that are installed or
// have no icon of their own come last.
#pragma once

#include <string>
#include <vector>

#include "model.h"

namespace sde {
namespace merkadu {

struct HighlightSources {
  std::vector<std::string> distribution;    // references of merkadu.conf (resolve_reference())
  std::vector<std::string> fresh;           // app ids, newest first
  std::vector<std::string> popular;         // Flathub's app ids, most popular first
  std::vector<std::string> featured_snaps;  // app ids
};

struct Highlight {
  enum Why { Distribution, New, Popular, Featured };
  const App *app = nullptr;
  Why why = Distribution;
};

// Up to |count| highlights; |day| (days since the epoch) rotates the lists.
std::vector<Highlight> choose_highlights(const Catalog &catalog, const HighlightSources &sources, size_t count,
                                         long day);

// The app ids of a Flathub collection (flathub.org/api/v2/collection/...:
// {"hits": [{"app_id": ...}]}, or a list of such objects or of ids).
std::vector<std::string> parse_flathub_collection(const std::string &json);

}  // namespace merkadu
}  // namespace sde
