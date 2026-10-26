// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Find Duplicates (a Gallery plugin): identical copies (the same bytes) and
// very similar photos (resized or saved again: their difference hashes
// differ in a few bits). Nothing here needs a display.
#pragma once

#include <cairo.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gallery {

// The SHA-256 of a file's content, "" when it cannot be read.
std::string file_sha256(const std::string &path);

// Groups of identical files: |sizes| and |hash| (e.g. file_sha256 of the
// path at an index) are only asked for files whose size another file has.
std::vector<std::vector<size_t>> identical_groups(const std::vector<long long> &sizes,
                                                  const std::function<std::string(size_t)> &hash);

// A 64-bit difference hash of an image: a 9x8 grey copy, one bit per pair
// of neighbours (left brighter than right). 0 for a blank image.
uint64_t difference_hash(cairo_surface_t *s);
int hamming(uint64_t a, uint64_t b);

// Groups of similar images: hashes within |max_bits| bits of each other and
// shapes (width/height) within 5 %; hash 0 is never matched.
std::vector<std::vector<size_t>> similar_groups(const std::vector<uint64_t> &hashes, const std::vector<double> &aspects,
                                                int max_bits = 5);

// Joins two lists of groups over n items (a copy can also be similar to a
// third photo): items connected through any group end in one group. Groups
// are returned with their items in ascending order, by their first item.
std::vector<std::vector<size_t>> merge_groups(size_t n, const std::vector<std::vector<size_t>> &a,
                                              const std::vector<std::vector<size_t>> &b);

}  // namespace gallery
