// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Tile cache (LRU, bounded in bytes), the background renderer and the
// reading history.
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "core.h"
#include "util.h"

namespace xanara {

// ------------------------------------------------------------ tile cache

size_t TileKeyHash::operator()(const TileKey &k) const {
  size_t h = std::hash<long long>()((long long)k.page * 1000003LL + k.col * 7919 + k.row);
  h ^= std::hash<int>()(k.scale_q) + 0x9e3779b9 + (h << 6) + (h >> 2);
  h ^= std::hash<int>()(k.rotation * 4 + k.mode) + 0x9e3779b9 + (h << 6) + (h >> 2);
  h ^= std::hash<unsigned long long>()(k.revision) + 0x9e3779b9 + (h << 6) + (h >> 2);
  return h;
}

int quantize_scale(double scale) { return int(std::lround(scale * 10000)); }

TileCache::TileCache(size_t budget_bytes) : budget_(budget_bytes) {}

TileCache::~TileCache() { clear(); }

size_t TileCache::surface_bytes(cairo_surface_t *s) {
  if (!s) return 0;
  if (cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE)
    return size_t(cairo_image_surface_get_stride(s)) * size_t(cairo_image_surface_get_height(s));
  return 0;
}

cairo_surface_t *TileCache::get(const TileKey &key) {
  auto it = map_.find(key);
  if (it == map_.end()) return nullptr;
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->surface;
}

void TileCache::put(const TileKey &key, cairo_surface_t *surface) {
  if (!surface) return;
  auto it = map_.find(key);
  if (it != map_.end()) {
    bytes_ -= it->second->bytes;
    cairo_surface_destroy(it->second->surface);
    lru_.erase(it->second);
    map_.erase(it);
  }
  size_t b = surface_bytes(surface);
  lru_.push_front(Entry{key, surface, b});
  map_[key] = lru_.begin();
  bytes_ += b;
  evict(&key);
}

void TileCache::evict(const TileKey *keep) {
  while (bytes_ > budget_ && !lru_.empty()) {
    auto last = std::prev(lru_.end());
    if (keep && last->key == *keep) break;
    bytes_ -= last->bytes;
    cairo_surface_destroy(last->surface);
    map_.erase(last->key);
    lru_.erase(last);
  }
}

void TileCache::drop_page(int page) {
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (it->key.page == page) {
      bytes_ -= it->bytes;
      cairo_surface_destroy(it->surface);
      map_.erase(it->key);
      it = lru_.erase(it);
    } else {
      ++it;
    }
  }
}

void TileCache::clear() {
  for (auto &e : lru_) cairo_surface_destroy(e.surface);
  lru_.clear();
  map_.clear();
  bytes_ = 0;
}

void TileCache::set_budget(size_t bytes) {
  budget_ = bytes;
  evict(nullptr);
}

void TileCache::each_of_page(int page, const std::function<void(const TileKey &, cairo_surface_t *)> &fn) const {
  for (const auto &e : lru_)
    if (e.key.page == page) fn(e.key, e.surface);
}

// --------------------------------------------------------------- renderer

Renderer::Renderer(Document *doc, std::function<void()> notify) : doc_(doc), notify_(std::move(notify)) {
  thread_ = std::thread([this] { run(); });
}

Renderer::~Renderer() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  for (auto &r : results_)
    if (r.surface) cairo_surface_destroy(r.surface);
}

void Renderer::replace_queue(std::vector<RenderJob> jobs) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
    for (auto &j : jobs) {
      if (has_current_ && j.key == current_) continue;
      bool done = false;
      for (const auto &r : results_)
        if (r.key == j.key) done = true;
      if (!done) queue_.push_back(j);
    }
  }
  cv_.notify_one();
}

std::vector<RenderResult> Renderer::take_results() {
  std::lock_guard<std::mutex> lock(mu_);
  notified_ = false;
  std::vector<RenderResult> out;
  out.swap(results_);
  return out;
}

bool Renderer::busy() const {
  std::lock_guard<std::mutex> lock(mu_);
  return has_current_ || !queue_.empty();
}

void Renderer::run() {
  for (;;) {
    RenderJob job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      job = queue_.front();
      queue_.pop_front();
      has_current_ = true;
      current_ = job.key;
    }
    cairo_surface_t *s = doc_->render(job.key.page, job.params, job.tx, job.ty, job.tw, job.th);
    {
      std::lock_guard<std::mutex> lock(mu_);
      has_current_ = false;
      if (stop_) {
        if (s) cairo_surface_destroy(s);
        return;
      }
      results_.push_back(RenderResult{job.key, s});
    }
    if (!notified_.exchange(true) && notify_) notify_();
  }
}

// ---------------------------------------------------------------- history

History::History(std::string file) : file_(file.empty() ? default_file() : std::move(file)) {}

std::string History::default_file() { return sde::state_home() + "/sde/xanara-history"; }

void History::load() {
  entries_.clear();
  std::string data;
  if (!sde::read_file(file_, data)) return;
  for (const auto &line : sde::split(data, '\n')) {
    auto f = sde::split(line, '\t', true);
    if (f.size() < 4 || f[0].empty() || f[0][0] != '/') continue;
    HistoryEntry e;
    e.path = f[0];
    e.page = std::max(0, atoi(f[1].c_str()));
    e.zoom = f[2].empty() ? "width" : f[2];
    e.time = atoll(f[3].c_str());
    entries_.push_back(e);
  }
}

bool History::save() const {
  std::string out;
  for (const auto &e : entries_)
    out += e.path + "\t" + std::to_string(e.page) + "\t" + e.zoom + "\t" + std::to_string(e.time) + "\n";
  std::string dir = file_.substr(0, file_.rfind('/'));
  sde::make_dirs(dir);
  return sde::write_file_atomic(file_, out);
}

const HistoryEntry *History::find(const std::string &path) const {
  for (const auto &e : entries_)
    if (e.path == path) return &e;
  return nullptr;
}

void History::remember(const HistoryEntry &e0) {
  if (e0.path.empty() || e0.path.find_first_of("\t\n") != std::string::npos) return;
  HistoryEntry e = e0;
  if (!e.time) e.time = (long long)time(nullptr);
  forget(e.path);
  entries_.insert(entries_.begin(), e);
  if (entries_.size() > 200) entries_.resize(200);
}

void History::forget(const std::string &path) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const HistoryEntry &x) { return x.path == path; }),
                 entries_.end());
}

std::vector<HistoryEntry> History::recent(size_t n, bool existing_only) const {
  std::vector<HistoryEntry> out;
  for (const auto &e : entries_) {
    if (out.size() >= n) break;
    if (existing_only && !sde::file_exists(e.path)) continue;
    out.push_back(e);
  }
  return out;
}

}  // namespace xanara
