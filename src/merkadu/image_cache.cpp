// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "image_cache.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <list>
#include <map>

#include "backend.h"
#include "icons.h"
#include "process.h"
#include "snapd.h"
#include "util.h"

namespace sde {
namespace merkadu {
namespace images {

namespace {

enum class State { Pending, Done, Failed };
struct Entry {
  State state = State::Pending;
  std::string path;
};

std::map<std::string, Entry> g_entries;
std::deque<std::string> g_queue;
int g_active = 0;
std::vector<std::function<void()>> g_ready;
constexpr int kParallel = 4;

std::string cache_dir() {
  std::string base = env("XDG_CACHE_HOME");
  if (base.empty()) base = join_path(home_dir(), ".cache");
  return join_path(base, "merkadu/images");
}

std::string hash_of(const std::string &s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

// The file extension for the first bytes of an image; "" for formats the
// SDE image loader cannot read.
std::string sniff(const std::string &head) {
  if (head.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return ".png";
  if (head.size() > 3 && static_cast<unsigned char>(head[0]) == 0xFF && static_cast<unsigned char>(head[1]) == 0xD8)
    return ".jpg";
  if (head.compare(0, 4, "GIF8") == 0) return ".gif";
  std::string start = head.substr(0, 256);
  if (start.find("<svg") != std::string::npos || start.find("<?xml") != std::string::npos) return ".svg";
  return "";
}

std::string existing(const std::string &stem) {
  for (const char *ext : {".png", ".jpg", ".svg", ".gif"})
    if (file_exists(stem + ext)) return stem + ext;
  return "";
}

void notify() {
  auto copy = g_ready;
  for (auto &fn : copy) fn();
}

void finished(const std::string &ref, const std::string &file) {
  Entry &e = g_entries[ref];
  e.state = file.empty() ? State::Failed : State::Done;
  e.path = file;
  --g_active;
  notify();
}

void start_next();

// Saves |data| (an image of unknown type) as stem + its extension.
std::string store(const std::string &stem, const std::string &data) {
  std::string ext = sniff(data);
  if (ext.empty()) return "";
  return write_file_atomic(stem + ext, data) ? stem + ext : "";
}

void fetch(const std::string &ref) {
  ++g_active;
  std::string dir = cache_dir();
  make_dirs(dir);
  std::string stem = join_path(dir, hash_of(ref));
  if (starts_with(ref, "snap-icon:")) {
    std::string name = ref.substr(10);
    background<std::string>(
        [name, stem]() -> std::string {
          if (!valid_snap_name(name)) return "";
          HttpResponse r = http_unix(snapd_socket(), "GET", "/v2/icons/" + name + "/icon", "", false, 15000);
          return r.status == 200 ? store(stem, r.body) : "";
        },
        [ref](std::string file) {
          finished(ref, file);
          start_next();
        });
    return;
  }
  std::string tmp = stem + ".part";
  bool started = run_async({"curl", "-fsSL", "--max-time", "40", "--connect-timeout", "8", "--max-filesize",
                            "16777216", "--proto", "=https", "--proto-redir", "=https", "-o", tmp, ref},
                           [ref, tmp, stem](const std::string &, int status) {
                             std::string data, file;
                             if (status == 0 && read_file(tmp, data)) file = store(stem, data);
                             unlink(tmp.c_str());
                             finished(ref, file);
                             start_next();
                           });
  if (!started) {
    finished(ref, "");
    // Without curl nothing can be fetched: fail the rest at once.
    while (!g_queue.empty()) {
      g_entries[g_queue.front()].state = State::Failed;
      g_queue.pop_front();
    }
  }
}

void start_next() {
  while (g_active < kParallel && !g_queue.empty()) {
    std::string ref = g_queue.front();
    g_queue.pop_front();
    fetch(ref);
  }
}

struct Picture {
  cairo_surface_t *surface = nullptr;
  ~Picture() {
    if (surface) cairo_surface_destroy(surface);
  }
};
std::list<std::pair<std::string, std::unique_ptr<Picture>>> g_pictures;  // most recent first

}  // namespace

void on_ready(std::function<void()> fn) { g_ready.push_back(std::move(fn)); }

std::string path(const std::string &ref) {
  if (ref.empty()) return "";
  if (ref[0] == '/') return file_exists(ref) ? ref : "";
  bool remote = starts_with(ref, "https://") || starts_with(ref, "snap-icon:");
  if (!remote) return "";
  auto it = g_entries.find(ref);
  if (it != g_entries.end()) return it->second.state == State::Done ? it->second.path : "";
  // Fetched by an earlier run (installed snaps' icons change with them:
  // those are fetched again by every run).
  if (starts_with(ref, "https://")) {
    std::string have = existing(join_path(cache_dir(), hash_of(ref)));
    if (!have.empty()) {
      g_entries[ref] = Entry{State::Done, have};
      return have;
    }
  }
  if (fake_mode() && starts_with(ref, "https://")) {
    g_entries[ref].state = State::Failed;  // no network in tests
    return "";
  }
  g_entries[ref].state = State::Pending;
  g_queue.push_back(ref);
  start_next();
  auto now = g_entries.find(ref);
  return now != g_entries.end() && now->second.state == State::Done ? now->second.path : "";
}

cairo_surface_t *picture(const std::string &ref, int w, int h) {
  std::string key = ref + "@" + std::to_string(w) + "x" + std::to_string(h);
  for (auto it = g_pictures.begin(); it != g_pictures.end(); ++it)
    if (it->first == key) {
      g_pictures.splice(g_pictures.begin(), g_pictures, it);
      return it->second->surface;
    }
  std::string file = path(ref);
  if (file.empty()) return nullptr;
  cairo_surface_t *img = load_image(file, w, h);
  if (!img) return nullptr;
  double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
  double sc = std::min(w / iw, h / ih);
  int tw = std::max(1, int(iw * sc + 0.5)), th = std::max(1, int(ih * sc + 0.5));
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
  cairo_t *cr = cairo_create(out);
  cairo_scale(cr, sc, sc);
  cairo_set_source_surface(cr, img, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(img);
  cairo_surface_flush(out);
  auto pic = std::make_unique<Picture>();
  pic->surface = out;
  g_pictures.emplace_front(key, std::move(pic));
  // Screenshots are large: keep the last few.
  while (g_pictures.size() > 24) g_pictures.pop_back();
  return out;
}

cairo_surface_t *icon(const std::string &ref, int size, const std::string &fallback) {
  if (ref.empty()) return IconLoader::get({fallback}, size);
  if (ref[0] == '/' || starts_with(ref, "https://") || starts_with(ref, "snap-icon:")) {
    std::string file = path(ref);
    return IconLoader::get({file.empty() ? fallback : file, fallback}, size);
  }
  if (fake_mode()) {
    // The fake sources' launchers name the icons of DIR/icons.
    std::string file = fake_dir() + "/icons/" + ref + ".svg";
    if (file_exists(file)) return IconLoader::get({file, fallback}, size);
  }
  return IconLoader::get({ref, fallback}, size);
}

cairo_surface_t *icon_file(const std::string &ref, int size) {
  static std::map<std::string, cairo_surface_t *> cache;  // path@size -> icon (nullptr: unreadable)
  static std::deque<std::string> order;
  std::string file = path(ref);
  if (file.empty() || size <= 0) return nullptr;
  std::string key = file + "@" + std::to_string(size);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  cairo_surface_t *out = nullptr;
  if (cairo_surface_t *raw = load_image(file, size, size)) {
    int w = cairo_image_surface_get_width(raw), h = cairo_image_surface_get_height(raw);
    out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(out);
    double sc = double(size) / std::max(1, std::max(w, h));
    cairo_translate(cr, (size - w * sc) / 2, (size - h * sc) / 2);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, raw, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(raw);
    cairo_surface_flush(out);
  }
  cache[key] = out;
  order.push_back(key);
  // A few hundred icons of the pages seen last.
  while (order.size() > 800) {
    auto old = cache.find(order.front());
    if (old != cache.end()) {
      if (old->second) cairo_surface_destroy(old->second);
      cache.erase(old);
    }
    order.pop_front();
  }
  return out;
}

void clear() { g_pictures.clear(); }

}  // namespace images
}  // namespace merkadu
}  // namespace sde
