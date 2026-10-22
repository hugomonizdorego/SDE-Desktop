// SPDX-License-Identifier: GPL-3.0-or-later
#include "art.h"

#include <FL/Fl.H>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>

#include "icons.h"
#include "process.h"
#include "util.h"

using namespace sde;

namespace art {

namespace {

struct Entry {
  cairo_surface_t *surface = nullptr;
  bool loading = false, failed = false;
  int size = 0;
  std::vector<std::function<void()>> waiting;
};
std::map<std::string, Entry> g_cache;
std::deque<std::string> g_order;  // least recently added first

const size_t kKeep = 6;

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

cairo_surface_t *scaled(cairo_surface_t *src, int size) {
  int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
  if (w <= 0 || h <= 0) return nullptr;
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(out);
  // Cover the square, centered.
  double sc = double(size) / std::min(w, h);
  cairo_translate(cr, (size - w * sc) / 2, (size - h * sc) / 2);
  cairo_scale(cr, sc, sc);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  return out;
}

void forget_old() {
  while (g_order.size() > kKeep) {
    auto it = g_cache.find(g_order.front());
    g_order.pop_front();
    if (it == g_cache.end() || it->second.loading) continue;
    if (it->second.surface) cairo_surface_destroy(it->second.surface);
    g_cache.erase(it);
  }
}

void done(const std::string &url, const std::string &path) {
  auto it = g_cache.find(url);
  if (it == g_cache.end()) return;
  Entry &e = it->second;
  e.loading = false;
  cairo_surface_t *img = path.empty() ? nullptr : load_image(path);
  if (img) {
    e.surface = scaled(img, e.size);
    cairo_surface_destroy(img);
  }
  e.failed = !e.surface;
  auto waiting = std::move(e.waiting);
  e.waiting.clear();
  for (auto &fn : waiting)
    if (fn) fn();
}

// Runs |fn| from the event loop.
void later(std::function<void()> fn) {
  auto *heap = new std::function<void()>(std::move(fn));
  Fl::add_timeout(0, [](void *d) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(d));
    (*f)();
  }, heap);
}

}  // namespace

std::string local_path(const std::string &url) {
  std::string rest;
  if (starts_with(url, "file://"))
    rest = url.substr(7);
  else if (!url.empty() && url[0] == '/')
    return url;
  else
    return "";
  // file://host/path: only the local host.
  if (!rest.empty() && rest[0] != '/') {
    size_t slash = rest.find('/');
    if (slash == std::string::npos) return "";
    std::string host = rest.substr(0, slash);
    if (host != "localhost") return "";
    rest = rest.substr(slash);
  }
  std::string out;
  for (size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && hexval(rest[i + 1]) >= 0 &&
        hexval(rest[i + 2]) >= 0) {
      out += char(hexval(rest[i + 1]) * 16 + hexval(rest[i + 2]));
      i += 2;
    } else {
      out += rest[i];
    }
  }
  return out;
}

cairo_surface_t *get(const std::string &url, int size, std::function<void()> ready) {
  if (url.empty() || size <= 0) return nullptr;
  auto it = g_cache.find(url);
  if (it != g_cache.end() && it->second.size == size) {
    Entry &e = it->second;
    if (e.loading && ready) e.waiting.push_back(std::move(ready));
    return e.surface;
  }
  if (it != g_cache.end() && it->second.loading) return nullptr;
  if (it != g_cache.end()) {
    if (it->second.surface) cairo_surface_destroy(it->second.surface);
    g_cache.erase(it);
  }
  Entry &e = g_cache[url];
  e.size = size;
  e.loading = true;
  if (ready) e.waiting.push_back(std::move(ready));
  g_order.push_back(url);
  forget_old();
  std::string path = local_path(url);
  if (!path.empty()) {
    later([url, path] { done(url, file_exists(path) ? path : ""); });
  } else if (starts_with(url, "http://") || starts_with(url, "https://")) {
    std::string dir = join_path(join_path(env("XDG_CACHE_HOME", join_path(home_dir(), ".cache")),
                                          "sde"),
                                "art");
    make_dirs(dir);
    char name[32];
    snprintf(name, sizeof name, "%016zx", std::hash<std::string>()(url));
    std::string file = join_path(dir, name);
    if (file_exists(file)) {
      later([url, file] { done(url, file); });
    } else {
      bool started = run_async({"curl", "-fsSL", "--max-time", "10", "--max-filesize", "4000000",
                                "-o", file + ".part", url},
                               [url, file](const std::string &, int status) {
                                 if (status == 0) rename((file + ".part").c_str(), file.c_str());
                                 else remove((file + ".part").c_str());
                                 done(url, status == 0 ? file : "");
                               });
      if (!started) later([url] { done(url, ""); });
    }
  } else {
    later([url] { done(url, ""); });
  }
  return nullptr;
}

}  // namespace art
