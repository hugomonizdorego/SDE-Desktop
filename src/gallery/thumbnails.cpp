// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "thumbnails.h"

#include <FL/Fl_BMP_Image.H>
#include <FL/Fl_GIF_Image.H>
#include <FL/Fl_JPEG_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Pixmap.H>
#include <fcntl.h>
#include <librsvg/rsvg.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef SDE_GALLERY_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>

#include "exif.h"
#include "library.h"
#include "util.h"

namespace gallery {

// ---------------------------------------------------------------- decoding

namespace {

// FLTK's pixmap conversion (GIF images are Fl_Pixmap in FLTK 1.3) uses the
// shared color map, so it is serialized; JPEG/PNG/BMP decoders are reentrant.
std::mutex g_pixmap_mutex;

constexpr long long kMaxPixels = 160LL * 1000 * 1000;

cairo_surface_t *fit_surface(cairo_surface_t *src, int max_dim) {
  int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
  if (max_dim <= 0 || (w <= max_dim && h <= max_dim)) return src;
  double sc = double(max_dim) / std::max(w, h);
  int tw = std::max(1, int(std::lround(w * sc))), th = std::max(1, int(std::lround(h * sc)));
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
  cairo_t *cr = cairo_create(out);
  cairo_scale(cr, double(tw) / w, double(th) / h);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(src);
  return out;
}

cairo_surface_t *from_fltk(Fl_Image *img) {
  if (!img || img->w() <= 0 || img->h() <= 0 || img->count() < 1 || !img->data() ||
      !img->data()[0])
    return nullptr;
  int w = img->w(), h = img->h(), d = img->d();
  if (d < 1 || d > 4 || (long long)w * h > kMaxPixels) return nullptr;
  int ld = img->ld() ? img->ld() : w * d;
  const unsigned char *src = reinterpret_cast<const unsigned char *>(img->data()[0]);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  unsigned char *dst = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < h; ++y) {
    uint32_t *row = reinterpret_cast<uint32_t *>(dst + size_t(y) * stride);
    const unsigned char *p = src + size_t(y) * ld;
    for (int x = 0; x < w; ++x, p += d) {
      unsigned r, g, b, a = 255;
      if (d < 3) {
        r = g = b = p[0];
        if (d == 2) a = p[1];
      } else {
        r = p[0];
        g = p[1];
        b = p[2];
        if (d == 4) a = p[3];
      }
      row[x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
    }
  }
  cairo_surface_mark_dirty(s);
  return s;
}

std::string sniff(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "";
  unsigned char m[16] = {0};
  size_t n = fread(m, 1, sizeof m, f);
  fclose(f);
  if (n >= 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF) return "jpeg";
  if (n >= 8 && !memcmp(m, "\x89PNG\r\n\x1a\n", 8)) return "png";
  if (n >= 4 && !memcmp(m, "GIF8", 4)) return "gif";
  if (n >= 2 && m[0] == 'B' && m[1] == 'M') return "bmp";
  if (n >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WEBP", 4)) return "webp";
  if (n >= 2 && m[0] == 0x1f && m[1] == 0x8b) return "gzip";  // .svgz
  return "";
}

cairo_surface_t *decode_svg(const std::string &path, int max_dim, int *ow, int *oh) {
  GError *err = nullptr;
  RsvgHandle *handle = rsvg_handle_new_from_file(path.c_str(), &err);
  if (!handle) {
    if (err) g_error_free(err);
    return nullptr;
  }
  double w = 0, h = 0;
#if LIBRSVG_CHECK_VERSION(2, 52, 0)
  if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &w, &h)) w = h = 0;
#endif
  if (w <= 0 || h <= 0) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    RsvgDimensionData dim;
    rsvg_handle_get_dimensions(handle, &dim);
#pragma GCC diagnostic pop
    w = dim.width;
    h = dim.height;
  }
  if (w <= 0 || h <= 0) w = h = 512;
  if (ow) *ow = int(std::lround(w));
  if (oh) *oh = int(std::lround(h));
  // Vector art is rendered to fill the requested size; the viewer gets
  // 2048 px so that it stays sharp when zoomed.
  int target = max_dim > 0 ? std::min(max_dim, 2048) : 2048;
  double sc = double(target) / std::max(w, h);
  int tw = std::max(1, int(std::lround(w * sc))), th = std::max(1, int(std::lround(h * sc)));
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
  cairo_t *cr = cairo_create(s);
  RsvgRectangle viewport{0, 0, double(tw), double(th)};
  bool ok = rsvg_handle_render_document(handle, cr, &viewport, nullptr);
  cairo_destroy(cr);
  g_object_unref(handle);
  if (!ok) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  return s;
}

#ifdef SDE_GALLERY_PIXBUF
// |orient|: apply the orientation the loader reports (TIFF, WebP ...).
cairo_surface_t *decode_pixbuf(const std::string &path, int max_dim, int *ow, int *oh,
                               bool orient = true) {
  int w = 0, h = 0;
  if (!gdk_pixbuf_get_file_info(path.c_str(), &w, &h) || w <= 0 || h <= 0) return nullptr;
  if ((long long)w * h > kMaxPixels) return nullptr;
  if (ow) *ow = w;
  if (oh) *oh = h;
  GError *err = nullptr;
  GdkPixbuf *pb = max_dim > 0 && (w > max_dim || h > max_dim)
                      ? gdk_pixbuf_new_from_file_at_scale(path.c_str(), max_dim, max_dim, TRUE, &err)
                      : gdk_pixbuf_new_from_file(path.c_str(), &err);
  if (!pb) {
    if (err) g_error_free(err);
    return nullptr;
  }
  if (orient) {
    GdkPixbuf *upright = gdk_pixbuf_apply_embedded_orientation(pb);
    if (upright) {
      if (gdk_pixbuf_get_width(upright) != gdk_pixbuf_get_width(pb) && ow && oh) std::swap(*ow, *oh);
      g_object_unref(pb);
      pb = upright;
    }
  }
  cairo_surface_t *s = pixbuf_to_surface(pb);
  g_object_unref(pb);
  return s;
}
#endif

}  // namespace

#ifdef SDE_GALLERY_PIXBUF
cairo_surface_t *pixbuf_to_surface(GdkPixbuf *pb) {
  int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb);
  int channels = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb);
  bool alpha = gdk_pixbuf_get_has_alpha(pb);
  const guchar *px = gdk_pixbuf_read_pixels(pb);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
  unsigned char *dst = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < ph; ++y) {
    const guchar *p = px + size_t(y) * rs;
    uint32_t *row = reinterpret_cast<uint32_t *>(dst + size_t(y) * stride);
    for (int x = 0; x < pw; ++x, p += channels) {
      unsigned a = alpha && channels == 4 ? p[3] : 255;
      row[x] = (a << 24) | ((p[0] * a / 255) << 16) | ((p[1] * a / 255) << 8) | (p[2] * a / 255);
    }
  }
  cairo_surface_mark_dirty(s);
  return s;
}
#endif

namespace {

cairo_surface_t *decode_unoriented(const std::string &path, const std::string &kind, int max_dim,
                                   int *ow, int *oh);

}  // namespace

cairo_surface_t *orient_surface(cairo_surface_t *s, int tag) {
  if (!s || tag <= 1 || tag > 8) return s;
  Orient o = orient_from_exif(tag);
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  int nw = o.k % 2 ? h : w, nh = o.k % 2 ? w : h;
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
  if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(out);
    return s;
  }
  cairo_t *cr = cairo_create(out);
  // Map the stored image onto the upright canvas: mirror, then rotate.
  cairo_translate(cr, nw / 2.0, nh / 2.0);
  cairo_rotate(cr, o.k * M_PI / 2);
  if (o.m) cairo_scale(cr, -1, 1);
  cairo_translate(cr, -w / 2.0, -h / 2.0);
  cairo_set_source_surface(cr, s, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(s);
  return out;
}

cairo_surface_t *decode_image_data(const std::string &data) {
  if (data.size() < 8) return nullptr;
  const unsigned char *d = reinterpret_cast<const unsigned char *>(data.data());
  if (d[0] == 0xFF && d[1] == 0xD8) {
    Fl_JPEG_Image img("exif-preview", d);
    return from_fltk(&img);
  }
#ifdef SDE_GALLERY_PIXBUF
  GInputStream *in = g_memory_input_stream_new_from_data(data.data(), gssize(data.size()), nullptr);
  GdkPixbuf *pb = gdk_pixbuf_new_from_stream(in, nullptr, nullptr);
  g_object_unref(in);
  if (!pb) return nullptr;
  cairo_surface_t *s = pixbuf_to_surface(pb);
  g_object_unref(pb);
  return s;
#else
  return nullptr;
#endif
}

cairo_surface_t *decode_exif_preview(const std::string &path) {
  std::string jpeg = exif_thumbnail(path);
  if (jpeg.empty()) return nullptr;
  return orient_surface(decode_image_data(jpeg), jpeg_orientation(path));
}

cairo_surface_t *decode_image(const std::string &path, int max_dim, int *ow, int *oh) {
  if (ow) *ow = 0;
  if (oh) *oh = 0;
  std::string ext = extension_of(path), kind = sniff(path);
  if (ext == "svg" || ext == "svgz") return decode_svg(path, max_dim, ow, oh);
  if (kind != "jpeg") return decode_unoriented(path, kind, max_dim, ow, oh);
  // JPEG: decode, then turn upright following the EXIF orientation.
  int tag = jpeg_orientation(path);
  int w = 0, h = 0;
  cairo_surface_t *s = decode_unoriented(path, kind, max_dim, &w, &h);
  if (!s) return nullptr;
  if (orient_from_exif(tag).k % 2) std::swap(w, h);
  if (ow) *ow = w;
  if (oh) *oh = h;
  return orient_surface(s, tag);
}

namespace {

cairo_surface_t *decode_unoriented(const std::string &path, const std::string &kind, int max_dim,
                                   int *ow, int *oh) {
  std::unique_ptr<Fl_Image> img;
  cairo_surface_t *s = nullptr;
  if (kind == "jpeg") {
#ifdef SDE_GALLERY_PIXBUF
    // libjpeg scales by 1/2, 1/4 or 1/8 while decoding: much faster for
    // thumbnails and screen-sized previews of camera photos.
    if (max_dim > 0) {
      int w = 0, h = 0;
      if (gdk_pixbuf_get_file_info(path.c_str(), &w, &h) && (w > max_dim * 2 || h > max_dim * 2))
        if (cairo_surface_t *pb = decode_pixbuf(path, max_dim, ow, oh, false)) return pb;
    }
#endif
    img.reset(new Fl_JPEG_Image(path.c_str()));
  } else if (kind == "png") {
    img.reset(new Fl_PNG_Image(path.c_str()));
  } else if (kind == "bmp") {
    img.reset(new Fl_BMP_Image(path.c_str()));
  } else if (kind == "gif") {
#ifdef SDE_GALLERY_PIXBUF
    // gdk-pixbuf keeps GIF transparency; FLTK is the fallback below.
    if (cairo_surface_t *pb = decode_pixbuf(path, max_dim, ow, oh)) return pb;
#endif
    std::lock_guard<std::mutex> lock(g_pixmap_mutex);
    Fl_GIF_Image gif(path.c_str());
    // FLTK 1.3 decodes GIF into an Fl_Pixmap (XPM data): convert to RGBA.
    if (gif.w() > 0 && gif.h() > 0 && gif.count() > 0 && !gif.fail()) {
      Fl_RGB_Image rgb(static_cast<Fl_Pixmap *>(&gif), FL_WHITE);
      s = from_fltk(&rgb);
    }
  }
  if (img) s = from_fltk(img.get());
  img.reset();
  if (s) {
    if (ow) *ow = cairo_image_surface_get_width(s);
    if (oh) *oh = cairo_image_surface_get_height(s);
    return fit_surface(s, max_dim);
  }
#ifdef SDE_GALLERY_PIXBUF
  return decode_pixbuf(path, max_dim, ow, oh, kind != "jpeg");
#else
  return nullptr;
#endif
}

}  // namespace

std::set<std::string> extra_formats() {
  std::set<std::string> out;
#ifdef SDE_GALLERY_PIXBUF
  static const std::set<std::string> wanted{"webp", "tif", "tiff", "avif", "heic", "heif", "jxl"};
  GSList *formats = gdk_pixbuf_get_formats();
  for (GSList *l = formats; l; l = l->next) {
    auto *fmt = static_cast<GdkPixbufFormat *>(l->data);
    gchar **exts = gdk_pixbuf_format_get_extensions(fmt);
    for (gchar **e = exts; e && *e; ++e)
      if (wanted.count(sde::lower(*e))) out.insert(sde::lower(*e));
    g_strfreev(exts);
  }
  g_slist_free(formats);
#endif
  return out;
}

// ------------------------------------------------------------- PNG + cache

namespace {

uint32_t crc32(const unsigned char *data, size_t len, uint32_t crc = 0) {
  static uint32_t table[256];
  static std::once_flag once;
  std::call_once(once, [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  });
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

uint32_t be32(const std::string &s, size_t at) {
  return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) |
         (uint32_t(uint8_t(s[at + 2])) << 8) | uint32_t(uint8_t(s[at + 3]));
}

void put_be32(std::string &s, uint32_t v) {
  s += char(v >> 24);
  s += char(v >> 16);
  s += char(v >> 8);
  s += char(v);
}

cairo_status_t write_to_string(void *closure, const unsigned char *data, unsigned int len) {
  static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), len);
  return CAIRO_STATUS_SUCCESS;
}

struct ReadCursor {
  const std::string *data;
  size_t pos;
};

cairo_status_t read_from_string(void *closure, unsigned char *data, unsigned int len) {
  auto *c = static_cast<ReadCursor *>(closure);
  if (c->pos + len > c->data->size()) return CAIRO_STATUS_READ_ERROR;
  memcpy(data, c->data->data() + c->pos, len);
  c->pos += len;
  return CAIRO_STATUS_SUCCESS;
}

bool make_private_dirs(const std::string &path) {
  std::string cur;
  for (const auto &part : sde::split(path, '/')) {
    cur += "/" + part;
    if (mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
  }
  return sde::dir_exists(path);
}

}  // namespace

std::string png_text(const std::string &png, const std::string &key) {
  if (png.size() < 8 || png.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) return "";
  size_t at = 8;
  while (at + 12 <= png.size()) {
    uint32_t len = be32(png, at);
    if (len > png.size() - at - 12) break;
    std::string type = png.substr(at + 4, 4);
    if (type == "tEXt") {
      std::string body = png.substr(at + 8, len);
      size_t nul = body.find('\0');
      if (nul != std::string::npos && body.compare(0, nul, key) == 0 && nul == key.size())
        return body.substr(nul + 1);
    } else if (type == "IDAT" || type == "IEND") {
      // Thumbnailers write their text before the image data, but keep
      // looking: the spec does not require that order.
    }
    at += 12 + len;
  }
  return "";
}

std::string png_add_text(const std::string &png,
                         const std::vector<std::pair<std::string, std::string>> &text) {
  if (png.size() < 33 || png.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) return png;
  size_t ihdr_end = 8 + 12 + be32(png, 8);
  if (ihdr_end > png.size()) return png;
  std::string chunks;
  for (const auto &kv : text) {
    std::string body = "tEXt" + kv.first + std::string(1, '\0') + kv.second;
    put_be32(chunks, uint32_t(body.size() - 4));
    chunks += body;
    put_be32(chunks, crc32(reinterpret_cast<const unsigned char *>(body.data()), body.size()));
  }
  return png.substr(0, ihdr_end) + chunks + png.substr(ihdr_end);
}

cairo_surface_t *load_cached_thumbnail(const std::string &path, time_t mtime, bool large, int *w,
                                       int *h) {
  std::string data;
  if (!sde::read_file(thumbnail_path(path, large), data)) return nullptr;
  if (png_text(data, "Thumb::MTime") != std::to_string((long long)mtime)) return nullptr;
  ReadCursor cursor{&data, 0};
  cairo_surface_t *s = cairo_image_surface_create_from_png_stream(read_from_string, &cursor);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  if (w) *w = atoi(png_text(data, "Thumb::Image::Width").c_str());
  if (h) *h = atoi(png_text(data, "Thumb::Image::Height").c_str());
  return s;
}

bool save_thumbnail(cairo_surface_t *thumb, const std::string &path, time_t mtime, int w, int h,
                    bool large) {
  if (!thumb || in_thumbnail_cache(path)) return false;
  std::string target = thumbnail_path(path, large);
  std::string dir = target.substr(0, target.rfind('/'));
  if (!make_private_dirs(dir)) return false;
  std::string png;
  if (cairo_surface_write_to_png_stream(thumb, write_to_string, &png) != CAIRO_STATUS_SUCCESS)
    return false;
  struct stat st {};
  long long size = stat(path.c_str(), &st) == 0 ? (long long)st.st_size : 0;
  std::vector<std::pair<std::string, std::string>> text{
      {"Thumb::URI", file_uri(path)},
      {"Thumb::MTime", std::to_string((long long)mtime)},
      {"Thumb::Size", std::to_string(size)},
      {"Software", "SDE Gallery"}};
  if (w > 0 && h > 0) {
    text.push_back({"Thumb::Image::Width", std::to_string(w)});
    text.push_back({"Thumb::Image::Height", std::to_string(h)});
  }
  png = png_add_text(png, text);
  static std::atomic<unsigned> counter{0};
  std::string tmp = dir + "/.sde-gallery-" + std::to_string(getpid()) + "-" +
                    std::to_string(counter++) + ".png";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = write(fd, png.data(), png.size()) == ssize_t(png.size());
  ok = close(fd) == 0 && ok;
  if (!ok || rename(tmp.c_str(), target.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ service

ThumbnailService::ThumbnailService(std::function<void()> notify, int threads)
    : notify_(std::move(notify)) {
  threads = std::max(1, std::min(4, threads));
  for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { worker(); });
}

ThumbnailService::~ThumbnailService() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    queue_.clear();
    full_.clear();
  }
  cv_.notify_all();
  for (auto &t : threads_) t.join();
  for (auto &r : results_)
    if (r.surface) cairo_surface_destroy(r.surface);
}

void ThumbnailService::replace_queue(std::vector<Request> jobs) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
    for (auto &j : jobs)
      if (!inflight_.count({j.path, int(j.kind)})) queue_.push_back(std::move(j));
  }
  cv_.notify_all();
}

void ThumbnailService::load_full(const std::string &path, int max_dim, uint64_t generation) {
  load_full_steps(path, {max_dim}, generation);
}

void ThumbnailService::load_full_steps(const std::string &path, const std::vector<int> &max_dims,
                                       uint64_t generation) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    full_.clear();
    for (int m : max_dims) full_.push_back(FullJob{path, m, generation});
  }
  cv_.notify_all();
}

std::vector<Result> ThumbnailService::take_results() {
  notified_ = false;
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Result> out;
  out.swap(results_);
  return out;
}

void ThumbnailService::worker() {
  for (;;) {
    Request job;
    bool full = false;
    int full_max = 0;
    uint64_t gen = 0;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !full_.empty() || !queue_.empty(); });
      if (stop_) return;
      if (!full_.empty()) {
        full = true;
        job.path = full_.front().path;
        job.kind = JobKind::Full;
        full_max = full_.front().max_dim;
        gen = full_.front().generation;
        full_.pop_front();
      } else {
        job = std::move(queue_.front());
        queue_.pop_front();
        inflight_.insert({job.path, int(job.kind)});
      }
    }
    Result r;
    r.path = job.path;
    r.kind = job.kind;
    r.generation = gen;
    r.max_dim = full_max;
    if (full) {
      r.surface = decode_image(job.path, full_max, &r.width, &r.height);
    } else {
      bool large = job.kind == JobKind::Large;
      r.surface = load_cached_thumbnail(job.path, job.mtime, large, &r.width, &r.height);
      if (!r.surface && !stop_) {
        r.surface = decode_image(job.path, large ? 256 : 128, &r.width, &r.height);
        if (r.surface) save_thumbnail(r.surface, job.path, job.mtime, r.width, r.height, large);
      }
    }
    if (stop_) {
      if (r.surface) cairo_surface_destroy(r.surface);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!full) inflight_.erase({r.path, int(r.kind)});
      results_.push_back(std::move(r));
    }
    if (!notified_.exchange(true) && notify_) notify_();
  }
}

}  // namespace gallery
