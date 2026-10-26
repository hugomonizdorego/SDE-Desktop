// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Places (a Gallery plugin): photos grouped by where they were taken,
// without a network. The position is the photo's EXIF GPS data; the name is
// the nearest important place in a list of about 7,300 cities and towns of
// the world (Natural Earth's populated places, public domain), installed as
// $XDG_DATA_DIRS/sde/gallery/places.tsv:
//
//   name <TAB> region <TAB> country <TAB> latitude <TAB> longitude <TAB> population
//
// Positions already read are kept in ~/.cache/sde/gallery/locations.tsv
// (path, modification time, latitude, longitude; "-" for none). Nothing
// here needs a display.
#pragma once

#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gallery {

struct Place {
  std::string name, region, country;
  double lat = 0, lon = 0;
  long population = 0;
};

// Parses the places file; the result is sorted by latitude.
std::vector<Place> parse_gazetteer(const std::string &text);
// The installed places file ("" when there is none).
std::string gazetteer_path();
double distance_km(double lat1, double lon1, double lat2, double lon2);

// The place a position belongs to: the most important place within 50 km
// (a town nearby wins over a city farther away), else the nearest within
// 200 km (|near| is then set), else -1. |places| sorted by latitude.
int place_for(const std::vector<Place> &places, double lat, double lon, bool *near = nullptr);
// "Bergen, Norway"; "Near Bergen, Norway" when |near|.
std::string place_label(const Place &p, bool near);

struct LocatedPhoto {
  size_t index = 0;  // into the caller's list
  double lat = 0, lon = 0;
  time_t time = 0;
};
struct PlaceGroup {
  std::string label;
  double lat = 0, lon = 0;   // of the place (or the first photo)
  std::vector<size_t> photos;  // indices, newest first
  time_t newest = 0;
};
// Groups photos by place, the place with the newest photo first. Photos
// far from every known place are grouped by a grid of about 25 km and
// labeled with their coordinates.
std::vector<PlaceGroup> group_by_place(const std::vector<LocatedPhoto> &photos, const std::vector<Place> &places);

// The location cache: path -> (modification time, position or none).
struct CachedLocation {
  time_t mtime = 0;
  bool has_gps = false;
  double lat = 0, lon = 0;
};
std::map<std::string, CachedLocation> parse_location_cache(const std::string &text);
std::string format_location_cache(const std::map<std::string, CachedLocation> &cache);
std::string location_cache_path();

}  // namespace gallery
