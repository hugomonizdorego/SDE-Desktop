// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Image decoding and the asynchronous thumbnail service. Decoding runs on
// worker threads; finished images are handed to the FLTK main thread with
// Fl::awake() and collected there with take_results(). No FLTK widget or
// drawing call is made off the main thread.
#pragma once

#include <cairo.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct _GdkPixbuf;  // GdkPixbuf

namespace gallery {

// Decodes |path| (JPEG, PNG, GIF, BMP with FLTK, SVG with librsvg, others
// with gdk-pixbuf when available) into an ARGB32 surface no larger than
// |max_dim| on its longest side (0 = natural size). *w/*h receive the
// original dimensions. Thread safe. Caller owns the surface.
cairo_surface_t *decode_image(const std::string &path, int max_dim, int *w = nullptr,
                              int *h = nullptr);

// Applies an EXIF orientation (1-8) to |s| and returns the result; |s| is
// released when a new surface is made.
cairo_surface_t *orient_surface(cairo_surface_t *s, int exif_orientation);
// Decodes JPEG or PNG data held in memory (natural size).
cairo_surface_t *decode_image_data(const std::string &data);
// The small preview JPEG embedded in a photo's EXIF data, already turned
// upright, or nullptr. Much faster than decoding the photo.
cairo_surface_t *decode_exif_preview(const std::string &path);

// Converts a GdkPixbuf (RGB or RGBA) into a new ARGB32 surface. Only
// available when built with gdk-pixbuf (SDE_GALLERY_PIXBUF).
cairo_surface_t *pixbuf_to_surface(::_GdkPixbuf *pixbuf);

// Extensions that gdk-pixbuf decodes beyond the built-in formats (e.g.
// "webp" when its loader is installed). Empty without gdk-pixbuf.
std::set<std::string> extra_formats();

// freedesktop.org thumbnail cache. load_cached_thumbnail() returns nullptr
// unless a thumbnail exists whose Thumb::MTime matches |mtime|.
cairo_surface_t *load_cached_thumbnail(const std::string &path, time_t mtime, bool large,
                                       int *w = nullptr, int *h = nullptr);
bool save_thumbnail(cairo_surface_t *thumb, const std::string &path, time_t mtime, int w, int h,
                    bool large);
// PNG tEXt chunks of a thumbnail file (for Thumb::MTime etc.).
std::string png_text(const std::string &png_data, const std::string &key);
// Adds tEXt chunks after the PNG header (IHDR).
std::string png_add_text(const std::string &png_data,
                         const std::vector<std::pair<std::string, std::string>> &text);

enum class JobKind { Normal = 0, Large = 1, Full = 2 };

struct Result {
  std::string path;
  JobKind kind = JobKind::Normal;
  cairo_surface_t *surface = nullptr;  // nullptr: the file could not be decoded
  int width = 0, height = 0;           // original image size (0 if unknown)
  uint64_t generation = 0;
  int max_dim = 0;                     // Full: the size limit that was asked for
};

class ThumbnailService {
 public:
  // |notify| is called from a worker thread when results are waiting; it
  // must only wake the main loop (Fl::awake).
  explicit ThumbnailService(std::function<void()> notify, int threads = 2);
  ~ThumbnailService();
  ThumbnailService(const ThumbnailService &) = delete;
  ThumbnailService &operator=(const ThumbnailService &) = delete;

  struct Request {
    std::string path;
    time_t mtime = 0;
    JobKind kind = JobKind::Normal;
  };
  // Replaces the queue of waiting thumbnail jobs (most important first).
  // Jobs that a worker is already decoding are not queued twice.
  void replace_queue(std::vector<Request> jobs);
  // Loads a full-size image ahead of every thumbnail; |generation|
  // identifies the request in the result.
  void load_full(const std::string &path, int max_dim, uint64_t generation);
  // Like load_full(), in several steps: e.g. {1600, 0} first delivers a
  // quick screen-sized image, then the natural size (0 = no limit).
  void load_full_steps(const std::string &path, const std::vector<int> &max_dims,
                       uint64_t generation);
  std::vector<Result> take_results();

 private:
  void worker();
  std::function<void()> notify_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Request> queue_;
  std::set<std::pair<std::string, int>> inflight_;
  struct FullJob {
    std::string path;
    int max_dim = 0;
    uint64_t generation = 0;
  };
  std::deque<FullJob> full_;
  std::vector<Result> results_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> notified_{false};
  std::vector<std::thread> threads_;
};

}  // namespace gallery
