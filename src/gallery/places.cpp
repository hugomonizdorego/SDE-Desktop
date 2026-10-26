// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "places.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "i18n.h"
#include "library.h"
#include "util.h"

namespace gallery {

namespace {

std::string escape_field(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '\t') out += "\\t";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out;
}

std::string unescape_field(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      out += n == 't' ? '\t' : n == 'n' ? '\n' : n;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string coordinates(double lat, double lon) {
  return sde::format("%.2f° %s, %.2f° %s", std::fabs(lat), lat >= 0 ? "N" : "S", std::fabs(lon), lon >= 0 ? "E" : "W");
}

}  // namespace

std::vector<Place> parse_gazetteer(const std::string &text) {
  std::vector<Place> out;
  for (const auto &line : sde::split(text, '\n')) {
    if (line.empty() || line[0] == '#') continue;
    auto f = sde::split(line, '\t', true);
    if (f.size() < 6) continue;
    Place p;
    p.name = f[0];
    p.region = f[1];
    p.country = f[2];
    char *end = nullptr;
    p.lat = strtod(f[3].c_str(), &end);
    if (end == f[3].c_str()) continue;
    p.lon = strtod(f[4].c_str(), &end);
    if (end == f[4].c_str()) continue;
    p.population = atol(f[5].c_str());
    if (p.name.empty() || std::fabs(p.lat) > 90 || std::fabs(p.lon) > 180) continue;
    out.push_back(std::move(p));
  }
  std::map<std::pair<std::string, std::string>, int> names;
  for (const auto &p : out) ++names[{p.name, p.country}];
  for (auto &p : out) p.ambiguous = names[{p.name, p.country}] > 1 && !p.region.empty();
  std::sort(out.begin(), out.end(), [](const Place &a, const Place &b) { return a.lat < b.lat; });
  return out;
}

std::string gazetteer_path() {
  for (const auto &d : sde::data_dirs()) {
    std::string f = d + "/sde/gallery/places.tsv";
    if (sde::file_exists(f)) return f;
  }
  return "";
}

double distance_km(double lat1, double lon1, double lat2, double lon2) {
  const double r = 6371.0, k = M_PI / 180;
  double dlat = (lat2 - lat1) * k, dlon = (lon2 - lon1) * k;
  double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(lat1 * k) * std::cos(lat2 * k) * std::sin(dlon / 2) * std::sin(dlon / 2);
  return 2 * r * std::asin(std::min(1.0, std::sqrt(a)));
}

int place_for(const std::vector<Place> &places, double lat, double lon, bool *near) {
  if (near) *near = false;
  if (places.empty()) return -1;
  // Only places within 200 km north or south: about 1.8 degrees.
  const double band = 200.0 / 111.0 + 0.01;
  auto lo = std::lower_bound(places.begin(), places.end(), lat - band,
                             [](const Place &p, double v) { return p.lat < v; });
  int best = -1, nearby = -1;
  double best_score = -1e18, nearby_score = -1e18;
  for (auto it = lo; it != places.end() && it->lat <= lat + band; ++it) {
    double d = distance_km(lat, lon, it->lat, it->lon);
    if (d > 200) continue;
    int i = int(it - places.begin());
    double size = std::log10(double(it->population) + 1000);
    // Within 50 km a town nearby wins over a city a little farther away;
    // farther out the place people know is better ("Near Bergen").
    if (d <= 50 && size - d / 12 > best_score) {
      best_score = size - d / 12;
      best = i;
    }
    if (size - d / 40 > nearby_score) {
      nearby_score = size - d / 40;
      nearby = i;
    }
  }
  if (best >= 0) return best;
  if (nearby >= 0 && near) *near = true;
  return nearby;
}

std::string place_label(const Place &p, bool near) {
  std::string label = p.name;
  if (p.ambiguous) label += ", " + p.region;
  if (!p.country.empty() && p.country != p.name) label += ", " + p.country;
  return near ? sde::format(_("Near %s"), label.c_str()) : label;
}

std::vector<PlaceGroup> group_by_place(const std::vector<LocatedPhoto> &photos, const std::vector<Place> &places) {
  std::map<std::string, size_t> by_key;
  std::vector<PlaceGroup> groups;
  // Photos in the same spot share the answer: look each spot up once.
  std::map<std::pair<long, long>, std::pair<int, bool>> memo;
  for (size_t i = 0; i < photos.size(); ++i) {
    const LocatedPhoto &ph = photos[i];
    auto spot = std::make_pair(long(std::lround(ph.lat * 200)), long(std::lround(ph.lon * 200)));
    auto m = memo.find(spot);
    int idx;
    bool near = false;
    if (m != memo.end()) {
      idx = m->second.first;
      near = m->second.second;
    } else {
      idx = place_for(places, ph.lat, ph.lon, &near);
      memo[spot] = {idx, near};
    }
    std::string key, label;
    double glat = ph.lat, glon = ph.lon;
    if (idx >= 0) {
      key = (near ? "n" : "p") + std::to_string(idx);
      label = place_label(places[size_t(idx)], near);
      glat = places[size_t(idx)].lat;
      glon = places[size_t(idx)].lon;
    } else {
      // Far from every place: cells of a quarter degree (about 25 km).
      long cy = long(std::floor(ph.lat * 4)), cx = long(std::floor(ph.lon * 4));
      key = "g" + std::to_string(cy) + ":" + std::to_string(cx);
      glat = (cy + 0.5) / 4;
      glon = (cx + 0.5) / 4;
      label = coordinates(glat, glon);
    }
    auto it = by_key.find(key);
    if (it == by_key.end()) {
      PlaceGroup g;
      g.label = label;
      g.lat = glat;
      g.lon = glon;
      by_key[key] = groups.size();
      groups.push_back(g);
      it = by_key.find(key);
    }
    PlaceGroup &g = groups[it->second];
    g.photos.push_back(i);
    g.newest = std::max(g.newest, ph.time);
  }
  for (auto &g : groups)
    std::stable_sort(g.photos.begin(), g.photos.end(),
                     [&](size_t a, size_t b) { return photos[a].time > photos[b].time; });
  std::stable_sort(groups.begin(), groups.end(), [](const PlaceGroup &a, const PlaceGroup &b) {
    return a.newest != b.newest ? a.newest > b.newest : a.label < b.label;
  });
  return groups;
}

std::map<std::string, CachedLocation> parse_location_cache(const std::string &text) {
  std::map<std::string, CachedLocation> out;
  for (const auto &line : sde::split(text, '\n')) {
    auto f = sde::split(line, '\t', true);
    if (f.size() < 4) continue;
    CachedLocation c;
    c.mtime = time_t(atoll(f[1].c_str()));
    if (f[2] != "-" && f[3] != "-") {
      c.has_gps = true;
      c.lat = atof(f[2].c_str());
      c.lon = atof(f[3].c_str());
    }
    out[unescape_field(f[0])] = c;
  }
  return out;
}

std::string format_location_cache(const std::map<std::string, CachedLocation> &cache) {
  std::string out;
  for (const auto &kv : cache) {
    out += escape_field(kv.first) + "\t" + std::to_string((long long)kv.second.mtime) + "\t";
    out += kv.second.has_gps ? sde::format("%.7f\t%.7f", kv.second.lat, kv.second.lon) : std::string("-\t-");
    out += "\n";
  }
  return out;
}

std::string location_cache_path() { return cache_home() + "/sde/gallery/locations.tsv"; }

}  // namespace gallery
