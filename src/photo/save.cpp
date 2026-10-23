// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "save.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef SDE_GALLERY_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include "exif.h"
#include "library.h"
#include "util.h"

namespace photo {

Format format_of(const std::string &path) {
  std::string e = gallery::extension_of(path);
  if (e == "jpg" || e == "jpeg" || e == "jpe" || e == "jfif") return Format::Jpeg;
  if (e == "png") return Format::Png;
  if (e == "tif" || e == "tiff") return Format::Tiff;
  if (e == "webp") return Format::Webp;
  if (e == "bmp") return Format::Bmp;
  if (e == "gif") return Format::Gif;
  if (e == "svg" || e == "svgz") return Format::Svg;
  return Format::Unknown;
}

Format sniff_format(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return Format::Unknown;
  unsigned char m[12] = {0};
  size_t n = fread(m, 1, sizeof m, f);
  fclose(f);
  if (n >= 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF) return Format::Jpeg;
  if (n >= 8 && !memcmp(m, "\x89PNG", 4)) return Format::Png;
  if (n >= 4 && !memcmp(m, "GIF8", 4)) return Format::Gif;
  if (n >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WEBP", 4)) return Format::Webp;
  if (n >= 4 && (!memcmp(m, "II*\0", 4) || !memcmp(m, "MM\0*", 4))) return Format::Tiff;
  if (n >= 2 && m[0] == 'B' && m[1] == 'M') return Format::Bmp;
  return format_of(path);
}

namespace {

#ifdef SDE_GALLERY_PIXBUF
bool pixbuf_writable(const char *type) {
  bool ok = false;
  GSList *formats = gdk_pixbuf_get_formats();
  for (GSList *l = formats; l; l = l->next) {
    auto *fmt = static_cast<GdkPixbufFormat *>(l->data);
    gchar *name = gdk_pixbuf_format_get_name(fmt);
    if (name && !strcmp(name, type) && gdk_pixbuf_format_is_writable(fmt)) ok = true;
    g_free(name);
  }
  g_slist_free(formats);
  return ok;
}

GdkPixbuf *to_pixbuf(cairo_surface_t *s, bool alpha) {
  cairo_surface_flush(s);
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  int stride = cairo_image_surface_get_stride(s);
  const unsigned char *src = cairo_image_surface_get_data(s);
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, alpha, 8, w, h);
  if (!pb) return nullptr;
  guchar *dst = gdk_pixbuf_get_pixels(pb);
  int rs = gdk_pixbuf_get_rowstride(pb), ch = alpha ? 4 : 3;
  for (int y = 0; y < h; ++y) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(src + size_t(y) * stride);
    guchar *d = dst + size_t(y) * rs;
    for (int x = 0; x < w; ++x, d += ch) {
      uint32_t px = row[x];
      unsigned a = px >> 24;
      unsigned r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
      if (a && a < 255) {
        r = r * 255 / a;
        g = g * 255 / a;
        b = b * 255 / a;
      }
      if (!alpha && a < 255) {
        // Flatten transparency onto white for formats without alpha.
        r = (r * a + 255 * (255 - a)) / 255;
        g = (g * a + 255 * (255 - a)) / 255;
        b = (b * a + 255 * (255 - a)) / 255;
      }
      d[0] = guchar(r > 255 ? 255 : r);
      d[1] = guchar(g > 255 ? 255 : g);
      d[2] = guchar(b > 255 ? 255 : b);
      if (alpha) d[3] = guchar(a);
    }
  }
  return pb;
}
#endif

cairo_status_t to_string(void *closure, const unsigned char *data, unsigned int len) {
  static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), len);
  return CAIRO_STATUS_SUCCESS;
}

bool has_alpha(cairo_surface_t *s) {
  cairo_surface_flush(s);
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  int stride = cairo_image_surface_get_stride(s);
  const unsigned char *src = cairo_image_surface_get_data(s);
  for (int y = 0; y < h; ++y) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(src + size_t(y) * stride);
    for (int x = 0; x < w; ++x)
      if ((row[x] >> 24) != 255) return true;
  }
  return false;
}

}  // namespace

bool can_write(Format f) {
  switch (f) {
    case Format::Png: return true;
#ifdef SDE_GALLERY_PIXBUF
    case Format::Jpeg: return pixbuf_writable("jpeg");
    case Format::Tiff: return pixbuf_writable("tiff");
    case Format::Webp: return pixbuf_writable("webp");
    case Format::Bmp: return pixbuf_writable("bmp");
#endif
    default: return false;
  }
}

const char *format_name(Format f) {
  switch (f) {
    case Format::Jpeg: return "JPEG";
    case Format::Png: return "PNG";
    case Format::Tiff: return "TIFF";
    case Format::Webp: return "WebP";
    case Format::Bmp: return "BMP";
    case Format::Gif: return "GIF";
    case Format::Svg: return "SVG";
    case Format::Unknown: break;
  }
  return "";
}

Format output_format(Format source) { return can_write(source) ? source : Format::Png; }

std::string replace_extension(const std::string &path, Format f) {
  static const char *ext[] = {"", "jpg", "png", "tiff", "webp", "bmp", "gif", "svg"};
  std::string base = path;
  size_t slash = base.rfind('/'), dot = base.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) base.resize(dot);
  return base + "." + ext[int(f)];
}

std::string png_bytes(cairo_surface_t *s) {
  std::string out;
  if (cairo_surface_write_to_png_stream(s, to_string, &out) != CAIRO_STATUS_SUCCESS) out.clear();
  return out;
}

bool write_image(cairo_surface_t *s, const std::string &path, Format f, int quality,
                 const std::string &exif_from, std::string *error) {
  auto fail = [&](const std::string &m) {
    if (error) *error = m;
    return false;
  };
  if (!s) return fail("nothing to save");
  quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
  std::string data;
  if (f == Format::Png) {
    data = png_bytes(s);
    if (data.empty()) return fail("PNG encoding failed");
  } else {
#ifdef SDE_GALLERY_PIXBUF
    const char *type = f == Format::Jpeg ? "jpeg" : f == Format::Tiff ? "tiff" : f == Format::Webp ? "webp"
                       : f == Format::Bmp ? "bmp" : nullptr;
    if (!type || !pixbuf_writable(type)) return fail(std::string(format_name(f)) + " cannot be written");
    bool alpha = (f == Format::Tiff || f == Format::Webp) && has_alpha(s);
    GdkPixbuf *pb = to_pixbuf(s, alpha);
    if (!pb) return fail("out of memory");
    gchar *buf = nullptr;
    gsize size = 0;
    GError *err = nullptr;
    std::string q = std::to_string(quality);
    gboolean ok = f == Format::Jpeg || f == Format::Webp
                      ? gdk_pixbuf_save_to_buffer(pb, &buf, &size, type, &err, "quality", q.c_str(), nullptr)
                      : gdk_pixbuf_save_to_buffer(pb, &buf, &size, type, &err, nullptr);
    g_object_unref(pb);
    if (!ok) {
      std::string m = err ? err->message : "encoding failed";
      if (err) g_error_free(err);
      return fail(m);
    }
    data.assign(buf, size);
    g_free(buf);
#else
    return fail(std::string(format_name(f)) + " cannot be written without gdk-pixbuf");
#endif
  }
  if (f == Format::Jpeg && !exif_from.empty()) {
    std::string src;
    if (sde::read_file(exif_from, src)) {
      std::string seg = gallery::exif_segment_for_reencode(src, cairo_image_surface_get_width(s),
                                                           cairo_image_surface_get_height(s));
      if (!seg.empty()) gallery::jpeg_replace_exif(data, seg);
    }
  }
  if (!sde::write_file_atomic(path, data)) return fail(std::string("cannot write ") + path + ": " + strerror(errno));
  return true;
}

bool lossless_possible(const std::string &source, const Edits &e, const std::string &dest) {
  if (!e.geometry_only() || sniff_format(source) != Format::Jpeg || format_of(dest) != Format::Jpeg)
    return false;
  std::string data;
  if (!sde::read_file(source, data)) return false;
  gallery::Orient cur = gallery::orient_from_exif(gallery::jpeg_orientation(source));
  return gallery::jpeg_set_orientation(data, gallery::orient_to_exif(gallery::orient_then(cur, e.orient)));
}

SaveResult save_edits(const std::string &source, const Edits &e, cairo_surface_t *rendered,
                      const std::string &dest, int quality, std::string *error) {
  if (e.geometry_only() && sniff_format(source) == Format::Jpeg && format_of(dest) == Format::Jpeg) {
    std::string data;
    if (sde::read_file(source, data)) {
      gallery::Orient cur = gallery::orient_from_exif(gallery::jpeg_orientation(source));
      int tag = gallery::orient_to_exif(gallery::orient_then(cur, e.orient));
      if (gallery::jpeg_set_orientation(data, tag)) {
        if (sde::write_file_atomic(dest, data)) return SaveResult::Lossless;
        if (error) *error = std::string("cannot write ") + dest + ": " + strerror(errno);
        return SaveResult::Failed;
      }
    }
  }
  Format f = format_of(dest);
  if (!can_write(f)) f = Format::Png;
  bool keep_exif = sniff_format(source) == Format::Jpeg;
  if (!write_image(rendered, dest, f, quality, keep_exif ? source : "", error)) return SaveResult::Failed;
  return SaveResult::Encoded;
}

}  // namespace photo
