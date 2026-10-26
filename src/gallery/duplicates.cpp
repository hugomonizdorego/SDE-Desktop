// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "duplicates.h"

#include <glib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace gallery {

namespace {

// Union-find over item indices.
struct Sets {
  std::vector<size_t> parent;
  explicit Sets(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), size_t(0)); }
  size_t find(size_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  }
  void join(size_t a, size_t b) {
    a = find(a);
    b = find(b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  }
  std::vector<std::vector<size_t>> groups() {
    std::map<size_t, std::vector<size_t>> by_root;
    for (size_t i = 0; i < parent.size(); ++i) by_root[find(i)].push_back(i);
    std::vector<std::vector<size_t>> out;
    for (auto &kv : by_root)
      if (kv.second.size() > 1) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const std::vector<size_t> &x, const std::vector<size_t> &y) { return x[0] < y[0]; });
    return out;
  }
};

}  // namespace

std::string file_sha256(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "";
  GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
  unsigned char buf[65536];
  size_t n;
  bool ok = true;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) g_checksum_update(sum, buf, gssize(n));
  if (ferror(f)) ok = false;
  fclose(f);
  std::string out = ok ? g_checksum_get_string(sum) : "";
  g_checksum_free(sum);
  return out;
}

std::vector<std::vector<size_t>> identical_groups(const std::vector<long long> &sizes,
                                                  const std::function<std::string(size_t)> &hash) {
  std::map<long long, std::vector<size_t>> by_size;
  for (size_t i = 0; i < sizes.size(); ++i)
    if (sizes[i] > 0) by_size[sizes[i]].push_back(i);
  Sets sets(sizes.size());
  for (auto &kv : by_size) {
    if (kv.second.size() < 2) continue;
    std::map<std::string, size_t> first;
    for (size_t i : kv.second) {
      std::string h = hash(i);
      if (h.empty()) continue;
      auto it = first.find(h);
      if (it == first.end()) first[h] = i;
      else sets.join(it->second, i);
    }
  }
  return sets.groups();
}

uint64_t difference_hash(cairo_surface_t *s) {
  if (!s || cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE) return 0;
  int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
  if (sw < 2 || sh < 2) return 0;
  cairo_surface_t *src = s;
  cairo_format_t format = cairo_image_surface_get_format(s);
  if (format != CAIRO_FORMAT_ARGB32 && format != CAIRO_FORMAT_RGB24) {
    src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    cairo_t *cr = cairo_create(src);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    format = CAIRO_FORMAT_ARGB32;
  }
  cairo_surface_flush(src);
  const unsigned char *data = cairo_image_surface_get_data(src);
  int stride = cairo_image_surface_get_stride(src);
  // A 9x8 grey copy: each cell the average of its pixels (cairo's scaling
  // samples only part of them when it shrinks a lot), transparency white.
  double grey[8][9];
  double lo = 1e9, hi = -1e9;
  for (int y = 0; y < 8; ++y) {
    int y0 = y * sh / 8, y1 = std::max(y0 + 1, (y + 1) * sh / 8);
    for (int x = 0; x < 9; ++x) {
      int x0 = x * sw / 9, x1 = std::max(x0 + 1, (x + 1) * sw / 9);
      double sum = 0;
      for (int yy = y0; yy < y1; ++yy) {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(data + size_t(yy) * size_t(stride));
        for (int xx = x0; xx < x1; ++xx) {
          uint32_t px = row[xx];
          double white = format == CAIRO_FORMAT_ARGB32 ? 255.0 - ((px >> 24) & 0xFF) : 0.0;
          sum += 0.299 * (((px >> 16) & 0xFF) + white) + 0.587 * (((px >> 8) & 0xFF) + white) +
                 0.114 * ((px & 0xFF) + white);
        }
      }
      grey[y][x] = sum / double((y1 - y0) * (x1 - x0));
      lo = std::min(lo, grey[y][x]);
      hi = std::max(hi, grey[y][x]);
    }
  }
  if (src != s) cairo_surface_destroy(src);
  if (hi - lo < 1) return 0;  // a blank image says nothing
  // Neighbours of (almost) the same brightness give 0, however the scaling
  // rounded them: flat areas say the same in every copy.
  uint64_t h = 0;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) h = (h << 1) | (grey[y][x] > grey[y][x + 1] + 1.5 ? 1 : 0);
  return h == 0 ? 1 : h;
}

int hamming(uint64_t a, uint64_t b) { return __builtin_popcountll(a ^ b); }

std::vector<std::vector<size_t>> similar_groups(const std::vector<uint64_t> &hashes, const std::vector<double> &aspects,
                                                int max_bits) {
  Sets sets(hashes.size());
  for (size_t i = 0; i < hashes.size(); ++i) {
    if (!hashes[i]) continue;
    for (size_t j = i + 1; j < hashes.size(); ++j) {
      if (!hashes[j] || hamming(hashes[i], hashes[j]) > max_bits) continue;
      double a = i < aspects.size() ? aspects[i] : 0, b = j < aspects.size() ? aspects[j] : 0;
      if (a > 0 && b > 0 && std::fabs(std::log(a / b)) > 0.05) continue;
      sets.join(i, j);
    }
  }
  return sets.groups();
}

std::vector<std::vector<size_t>> merge_groups(size_t n, const std::vector<std::vector<size_t>> &a,
                                              const std::vector<std::vector<size_t>> &b) {
  Sets sets(n);
  for (const auto *list : {&a, &b})
    for (const auto &g : *list)
      for (size_t k = 1; k < g.size(); ++k)
        if (g[0] < n && g[k] < n) sets.join(g[0], g[k]);
  return sets.groups();
}

}  // namespace gallery
