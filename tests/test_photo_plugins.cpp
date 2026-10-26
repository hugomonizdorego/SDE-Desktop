// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for Photo's plugins (no display): Auto Enhance, the finishing
// touches (frame, watermark, background blur), red eyes, and Metadata &
// Privacy (removing the location or all details without re-encoding).
#include <cairo.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "draw.h"
#include "edit.h"
#include "exif.h"
#include "metadata.h"
#include "save.h"
#include "thumbnails.h"
#include "util.h"

using namespace photo;

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

static std::string g_tmp;

static cairo_surface_t *solid(int w, int h, double r, double g, double b) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, r, g, b);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}

static uint32_t pixel(cairo_surface_t *s, int x, int y) {
  cairo_surface_flush(s);
  const unsigned char *d = cairo_image_surface_get_data(s);
  return reinterpret_cast<const uint32_t *>(d + size_t(y) * cairo_image_surface_get_stride(s))[x];
}

static int chan(uint32_t px, int shift) { return int((px >> shift) & 0xFF); }

// ------------------------------------------------------------ auto enhance

static void test_auto_adjust() {
  // A dark picture gets brighter.
  cairo_surface_t *dark = solid(200, 150, 0.12, 0.12, 0.12);
  Adjust a = auto_adjust(dark);
  CHECK(a.exposure > 20);
  cairo_surface_destroy(dark);
  // A flat grey one (little range) gets more contrast.
  cairo_surface_t *flat = solid(200, 150, 0.5, 0.5, 0.5);
  cairo_t *cr = cairo_create(flat);
  cairo_set_source_rgb(cr, 0.56, 0.56, 0.56);
  cairo_rectangle(cr, 0, 0, 100, 150);
  cairo_fill(cr);
  cairo_destroy(cr);
  a = auto_adjust(flat);
  CHECK(a.contrast > 20);
  CHECK(std::fabs(a.exposure) < 1);
  cairo_surface_destroy(flat);
  // A blue cast is warmed.
  cairo_surface_t *blue = solid(200, 150, 0.35, 0.45, 0.7);
  a = auto_adjust(blue);
  CHECK(a.warmth > 5);
  cairo_surface_destroy(blue);
  // A good picture (full range, middle exposure, colourful) is left alone.
  cairo_surface_t *good = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 256, 64);
  cr = cairo_create(good);
  for (int x = 0; x < 256; ++x) {
    // Dark to light (the whole range), through every hue.
    double lum = 0.04 + 0.92 * x / 255.0, h = x * 7 * 2 * M_PI / 255.0, c = 0.15;
    double r = lum + c * std::cos(h), g = lum + c * std::cos(h - 2.094), b = lum + c * std::cos(h + 2.094);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_rectangle(cr, x, 0, 1, 64);
    cairo_fill(cr);
  }
  cairo_destroy(cr);
  a = auto_adjust(good);
  if (!(a.identity() || (std::fabs(a.exposure) < 8 && a.contrast < 8 && std::fabs(a.warmth) < 8)))
    fprintf(stderr, "auto: exposure %g contrast %g saturation %g warmth %g\n", a.exposure, a.contrast, a.saturation,
            a.warmth);
  CHECK(a.identity() || (std::fabs(a.exposure) < 8 && a.contrast < 8 && std::fabs(a.warmth) < 8));
  cairo_surface_destroy(good);
}

// ---------------------------------------------------------- frame and size

static void test_frame() {
  Edits e;
  e.finish.frame = FrameStyle::Border;
  e.finish.frame_width = 5;
  e.finish.frame_color = 0xFF204060;
  int ow, oh, cw, ch;
  content_size(e, 400, 300, cw, ch);
  result_size(e, 400, 300, ow, oh);
  CHECK(cw == 400 && ch == 300);
  CHECK(ow == 430 && oh == 330);  // 5 % of the shorter side on each side
  e.finish.frame = FrameStyle::Print;
  result_size(e, 400, 300, ow, oh);
  CHECK(ow == 430 && oh == 300 + 15 + 54);
  e.finish.frame = FrameStyle::Border;

  cairo_surface_t *src = solid(400, 300, 1, 0, 0);
  cairo_surface_t *out = render(src, e);
  CHECK(cairo_image_surface_get_width(out) == 430 && cairo_image_surface_get_height(out) == 330);
  uint32_t edge = pixel(out, 3, 3), mid = pixel(out, 215, 165);
  CHECK(chan(edge, 16) == 0x20 && chan(edge, 8) == 0x40 && chan(edge, 0) == 0x60);
  CHECK(chan(mid, 16) == 255 && chan(mid, 8) == 0);
  cairo_surface_destroy(out);
  // Without the finishing touches (the crop view): the photo alone.
  out = render(src, e, 1.0, kMarkEffects);
  CHECK(cairo_image_surface_get_width(out) == 400);
  cairo_surface_destroy(out);
  // Every style makes a picture of the size result_size() promised.
  for (FrameStyle f : {FrameStyle::Border, FrameStyle::Mat, FrameStyle::Rounded, FrameStyle::Print, FrameStyle::Shadow}) {
    e.finish.frame = f;
    result_size(e, 400, 300, ow, oh);
    out = render(src, e);
    CHECK(cairo_image_surface_get_width(out) == ow && cairo_image_surface_get_height(out) == oh);
    cairo_surface_destroy(out);
  }
  // Rounded corners: the corner of the photo shows the frame colour.
  e.finish.frame = FrameStyle::Rounded;
  out = render(src, e);
  uint32_t corner = pixel(out, 16, 16);
  CHECK(chan(corner, 16) == 0x20);
  cairo_surface_destroy(out);
  cairo_surface_destroy(src);

  // Drawings stay on the photo inside the frame.
  e.finish.frame = FrameStyle::Border;
  e.marks.w = 400;
  e.marks.h = 300;
  ViewMap v = view_map(e, 0, 0, 430, 330);
  Pt p = v.to_out({0, 0});
  CHECK(std::fabs(p.x - 15) < 0.01 && std::fabs(p.y - 15) < 0.01);
  p = v.to_out({400, 300});
  CHECK(std::fabs(p.x - 415) < 0.01 && std::fabs(p.y - 315) < 0.01);
  // Twice the size on the screen: still inside the frame.
  v = view_map(e, 100, 50, 860, 660);
  p = v.to_out({200, 150});
  CHECK(std::fabs(p.x - 530) < 0.01 && std::fabs(p.y - 380) < 0.01);
  // Mark maps of the photo itself (flattening) are unchanged.
  MarkMap m = mark_map(e, 400, 300);
  CHECK(std::fabs(m.to_out({10, 10}).x - 10) < 1e-9);
  Edits plain;
  CHECK(plain.finish.identity() && plain.identity());
  Edits framed = plain;
  framed.finish.frame = FrameStyle::Shadow;
  CHECK(!framed.identity() && !framed.geometry_only() && framed != plain);
  Edits signed_ = plain;
  signed_.finish.watermark = "   ";
  CHECK(signed_.finish.identity());  // blank text signs nothing
}

static double variance(cairo_surface_t *s, int x0, int y0, int w, int h) {
  double sum = 0, sum2 = 0;
  int n = 0;
  for (int y = y0; y < y0 + h; ++y)
    for (int x = x0; x < x0 + w; ++x) {
      double v = chan(pixel(s, x, y), 8);
      sum += v;
      sum2 += v * v;
      ++n;
    }
  double mean = sum / n;
  return sum2 / n - mean * mean;
}

static void test_focus_blur_and_watermark() {
  // A fine checkerboard: blurring makes it grey.
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 600, 400);
  cairo_t *cr = cairo_create(s);
  for (int y = 0; y < 400; y += 8)
    for (int x = 0; x < 600; x += 8) {
      double v = ((x + y) / 8) % 2 ? 1 : 0;
      cairo_set_source_rgb(cr, v, v, v);
      cairo_rectangle(cr, x, y, 8, 8);
      cairo_fill(cr);
    }
  cairo_destroy(cr);
  double before_centre = variance(s, 280, 180, 40, 40), before_corner = variance(s, 10, 10, 40, 40);
  Finish f;
  f.focus = FocusShape::Circle;
  f.focus_x = 0.5;
  f.focus_y = 0.5;
  f.focus_size = 30;
  f.blur = 80;
  CHECK(!f.identity());
  apply_focus_blur(s, f);
  CHECK(variance(s, 280, 180, 40, 40) > before_centre * 0.9);  // sharp
  CHECK(variance(s, 10, 10, 40, 40) < before_corner * 0.25);  // blurred
  cairo_surface_destroy(s);

  // Tilt-shift: a sharp band across the middle.
  s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 600, 400);
  cr = cairo_create(s);
  for (int y = 0; y < 400; y += 8)
    for (int x = 0; x < 600; x += 8) {
      cairo_set_source_rgb(cr, ((x + y) / 8) % 2, 0.5, 0.5);
      cairo_rectangle(cr, x, y, 8, 8);
      cairo_fill(cr);
    }
  cairo_destroy(cr);
  f.focus = FocusShape::Band;
  apply_focus_blur(s, f);
  auto red_var = [&](int y) {
    double sum = 0, sum2 = 0;
    for (int x = 100; x < 500; ++x) {
      double v = chan(pixel(s, x, y), 16);
      sum += v;
      sum2 += v * v;
    }
    double m = sum / 400;
    return sum2 / 400 - m * m;
  };
  CHECK(red_var(200) > red_var(10) * 4);
  cairo_surface_destroy(s);

  // The watermark goes to the chosen corner.
  s = solid(800, 500, 0.1, 0.1, 0.4);
  Finish w;
  w.watermark = "© Test 2026";
  w.watermark_corner = 3;
  w.watermark_size = 6;
  w.watermark_opacity = 100;
  draw_watermark(s, w);
  int bright_br = 0, bright_tl = 0;
  for (int y = 0; y < 500; ++y)
    for (int x = 0; x < 800; ++x) {
      if (chan(pixel(s, x, y), 0) < 200) continue;
      if (x > 400 && y > 300) ++bright_br;
      if (x < 400 && y < 200) ++bright_tl;
    }
  CHECK(bright_br > 200 && bright_tl == 0);
  cairo_surface_destroy(s);
}

static void test_red_eye() {
  cairo_surface_t *s = solid(100, 100, 0.85, 0.7, 0.6);  // skin
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 0.85, 0.08, 0.1);  // a red pupil
  cairo_arc(cr, 50, 50, 10, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.1, 0.2, 0.8);  // something blue next to it
  cairo_rectangle(cr, 42, 30, 6, 4);
  cairo_fill(cr);
  cairo_destroy(cr);
  uint32_t skin = pixel(s, 36, 50), blue = pixel(s, 44, 31);
  // A click near the pupil finds all of it; a click on skin finds nothing.
  Box found;
  CHECK(find_red_eye(s, 53, 47, 15, found));
  CHECK(std::fabs(found.x0 - 40) <= 2 && std::fabs(found.x1 - 60) <= 2);
  CHECK(std::fabs(found.y0 - 40) <= 2 && std::fabs(found.y1 - 60) <= 2);
  CHECK(!find_red_eye(s, 10, 85, 8, found));
  red_eye_region(s, 30, 30, 40, 40);
  uint32_t pupil = pixel(s, 50, 50);
  CHECK(chan(pupil, 16) < 60);  // no longer red
  CHECK(pixel(s, 36, 50) == skin);  // not red enough: untouched
  CHECK(pixel(s, 44, 31) == blue);
  // As a mark: rendered with the effects, moved by the crop.
  cairo_surface_t *src = solid(200, 100, 0.85, 0.7, 0.6);
  cr = cairo_create(src);
  cairo_set_source_rgb(cr, 0.9, 0.05, 0.05);
  cairo_arc(cr, 150, 50, 8, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_destroy(cr);
  Edits e;
  e.marks.w = 200;
  e.marks.h = 100;
  Mark m;
  m.kind = MarkKind::RedEye;
  m.pts = {{138, 38}, {162, 62}};
  e.marks.items.push_back(m);
  CHECK(e.marks.has_effects() && !e.marks.has_vectors());
  e.crop = {0.5, 0, 0.5, 1};
  cairo_surface_t *out = render(src, e, 1.0, kMarkEffects);
  CHECK(cairo_image_surface_get_width(out) == 100);
  CHECK(chan(pixel(out, 50, 50), 16) < 60);
  cairo_surface_destroy(out);
  cairo_surface_destroy(src);
  cairo_surface_destroy(s);
}

// --------------------------------------------------------------- metadata

static void le16(std::string &s, unsigned v) {
  s += char(v & 0xFF);
  s += char(v >> 8);
}
static void le32(std::string &s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xFF);
}

// EXIF (TIFF, little endian): Make "Canon", orientation 6 and a GPS block
// at 60° 23' 51" N, 5° 19' 28.2" E.
static std::string make_exif_tiff() {
  std::string t = "II";
  le16(t, 42);
  le32(t, 8);
  // IFD0 at 8: 3 entries, then the next IFD (none): 8 + 2 + 36 + 4 = 50.
  const uint32_t make_at = 50, gps_at = 56;
  le16(t, 3);
  le16(t, 0x010F); le16(t, 2); le32(t, 6); le32(t, make_at);
  le16(t, 0x0112); le16(t, 3); le32(t, 1); le16(t, 6); le16(t, 0);
  le16(t, 0x8825); le16(t, 4); le32(t, 1); le32(t, gps_at);
  le32(t, 0);
  t += std::string("Canon\0", 6);
  // GPS IFD at 56: 4 entries: 56 + 2 + 48 + 4 = 110; values after it.
  const uint32_t lat_at = 110, lon_at = 134;
  le16(t, 4);
  le16(t, 1); le16(t, 2); le32(t, 2); t += std::string("N\0\0\0", 4);
  le16(t, 2); le16(t, 5); le32(t, 3); le32(t, lat_at);
  le16(t, 3); le16(t, 2); le32(t, 2); t += std::string("E\0\0\0", 4);
  le16(t, 4); le16(t, 5); le32(t, 3); le32(t, lon_at);
  le32(t, 0);
  for (uint32_t v : {60u, 1u, 23u, 1u, 5100u, 100u}) le32(t, v);
  for (uint32_t v : {5u, 1u, 19u, 1u, 2820u, 100u}) le32(t, v);
  return t;
}

static std::string segment(unsigned char marker, const std::string &payload) {
  std::string s;
  s += char(0xFF);
  s += char(marker);
  size_t len = payload.size() + 2;
  s += char(len >> 8);
  s += char(len & 0xFF);
  return s + payload;
}

static bool exif_info(const std::string &tiff, gallery::PhotoInfo &info) {
  return gallery::parse_exif_tiff(reinterpret_cast<const unsigned char *>(tiff.data()), tiff.size(), info);
}

static void test_tiff_gps() {
  std::string tiff = make_exif_tiff();
  gallery::PhotoInfo info;
  CHECK(exif_info(tiff, info) && info.has_gps && std::fabs(info.latitude - 60.3975) < 0.001);
  CHECK(info.make == "Canon" && info.orientation == 6);
  std::string before = tiff;
  CHECK(gallery::tiff_remove_gps(tiff));
  CHECK(tiff.size() == before.size());
  gallery::PhotoInfo after;
  CHECK(exif_info(tiff, after) && !after.has_gps);
  CHECK(after.make == "Canon" && after.orientation == 6);
  // The position itself is gone from the bytes, not only unlinked.
  std::string lat_bytes;
  for (uint32_t v : {60u, 1u, 23u, 1u, 5100u, 100u}) le32(lat_bytes, v);
  CHECK(before.find(lat_bytes) != std::string::npos && tiff.find(lat_bytes) == std::string::npos);
  CHECK(!gallery::tiff_remove_gps(tiff));  // nothing left to remove
}

static void test_jpeg_metadata() {
  // A real JPEG, with EXIF (and GPS), XMP, IPTC and a comment added.
  cairo_surface_t *s = solid(64, 48, 0.2, 0.6, 0.3);
  std::string path = g_tmp + "/photo.jpg";
  CHECK(write_image(s, path, Format::Jpeg, 90));
  cairo_surface_destroy(s);
  std::string jpeg;
  CHECK(sde::read_file(path, jpeg));
  std::string exif = segment(0xE1, std::string("Exif\0\0", 6) + make_exif_tiff());
  CHECK(gallery::jpeg_replace_exif(jpeg, exif));
  std::string xmp_gps = segment(0xE1, std::string("http://ns.adobe.com/xap/1.0/\0", 29) +
                                          "<x:xmpmeta><exif:GPSLatitude>60,23.85N</exif:GPSLatitude></x:xmpmeta>");
  std::string iptc = segment(0xED, "Photoshop 3.0\0 8BIM some IPTC data");
  std::string comment = segment(0xFE, "Taken at home");
  jpeg.insert(2, xmp_gps + iptc + comment);
  CHECK(gallery::has_removable_metadata(jpeg));
  auto fields = gallery::list_metadata(jpeg);
  bool maker = false, latitude = false, xmp = false, orient = false;
  for (const auto &f : fields) {
    if (f.name == "Camera maker" && f.value == "Canon") maker = true;
    if (f.name == "Latitude" && f.value == "60° 23′ 51″" && f.group == "Location") latitude = true;
    if (f.name == "XMP data") xmp = true;
    if (f.name == "Orientation" && f.value == "Turned right") orient = true;
  }
  CHECK(maker && latitude && xmp && orient);

  // Remove the location: the camera details stay.
  std::string loc = jpeg;
  bool changed = false;
  CHECK(gallery::jpeg_remove_location(loc, &changed) && changed);
  CHECK(loc.find("GPSLatitude") == std::string::npos);
  CHECK(loc.find("Taken at home") != std::string::npos);
  CHECK(sde::write_file_atomic(g_tmp + "/noloc.jpg", loc));
  gallery::PhotoInfo info;
  CHECK(gallery::read_photo_info(g_tmp + "/noloc.jpg", info) && !info.has_gps && info.make == "Canon");
  CHECK(gallery::jpeg_remove_location(loc, &changed) && !changed);
  cairo_surface_t *img = gallery::decode_image(g_tmp + "/noloc.jpg", 0);
  CHECK(img && cairo_image_surface_get_width(img) == 48);  // turned right (orientation 6)
  if (img) cairo_surface_destroy(img);

  // Remove everything: the picture and its orientation stay.
  std::string bare = jpeg;
  CHECK(gallery::jpeg_strip_metadata(bare));
  CHECK(bare.size() < jpeg.size());
  CHECK(bare.find("Canon") == std::string::npos && bare.find("Taken at home") == std::string::npos);
  CHECK(bare.find("8BIM") == std::string::npos && bare.find("GPSLatitude") == std::string::npos);
  CHECK(!gallery::has_removable_metadata(bare));
  CHECK(sde::write_file_atomic(g_tmp + "/bare.jpg", bare));
  gallery::PhotoInfo binfo;
  gallery::read_photo_info(g_tmp + "/bare.jpg", binfo);
  CHECK(binfo.orientation == 6 && binfo.make.empty() && !binfo.has_gps);
  img = gallery::decode_image(g_tmp + "/bare.jpg", 0);
  CHECK(img != nullptr);
  if (img) cairo_surface_destroy(img);
  // The scan data is untouched: the end of the file is the same.
  CHECK(bare.substr(bare.size() - 64) == jpeg.substr(jpeg.size() - 64));
  std::string other = "not a jpeg";
  CHECK(!gallery::jpeg_strip_metadata(other));
}

static std::string png_chunk(const std::string &type, const std::string &data) {
  std::string c;
  uint32_t n = uint32_t(data.size());
  for (int i = 3; i >= 0; --i) c += char((n >> (8 * i)) & 0xFF);
  c += type + data;
  // CRC-32 of type and data.
  uint32_t crc = 0xFFFFFFFFu;
  std::string td = type + data;
  for (unsigned char ch : td) {
    crc ^= ch;
    for (int k = 0; k < 8; ++k) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
  }
  crc ^= 0xFFFFFFFFu;
  for (int i = 3; i >= 0; --i) c += char((crc >> (8 * i)) & 0xFF);
  return c;
}

static void test_png_metadata() {
  cairo_surface_t *s = solid(32, 24, 0.9, 0.4, 0.1);
  std::string path = g_tmp + "/photo.png";
  CHECK(write_image(s, path, Format::Png, 0));
  cairo_surface_destroy(s);
  std::string png;
  CHECK(sde::read_file(path, png));
  // eXIf and a text chunk after IHDR (8 + 25 bytes).
  png.insert(33, png_chunk("eXIf", make_exif_tiff()) + png_chunk("tEXt", std::string("Author\0Jane Doe", 15)));
  CHECK(sde::write_file_atomic(g_tmp + "/meta.png", png));
  gallery::PhotoInfo info;
  CHECK(gallery::read_photo_info(g_tmp + "/meta.png", info) && info.has_gps);
  CHECK(gallery::has_removable_metadata(png));

  std::string loc = png;
  bool changed = false;
  CHECK(gallery::png_remove_location(loc, &changed) && changed);
  CHECK(sde::write_file_atomic(g_tmp + "/noloc.png", loc));
  gallery::PhotoInfo after;
  CHECK(gallery::read_photo_info(g_tmp + "/noloc.png", after) && !after.has_gps && after.make == "Canon");
  cairo_surface_t *img = gallery::decode_image(g_tmp + "/noloc.png", 0);  // CRCs are right
  CHECK(img && cairo_image_surface_get_width(img) == 32);
  if (img) cairo_surface_destroy(img);

  std::string bare = png;
  CHECK(gallery::png_strip_metadata(bare));
  CHECK(bare.find("eXIf") == std::string::npos && bare.find("Jane Doe") == std::string::npos);
  CHECK(!gallery::has_removable_metadata(bare));
  CHECK(sde::write_file_atomic(g_tmp + "/bare.png", bare));
  img = gallery::decode_image(g_tmp + "/bare.png", 0);
  CHECK(img && cairo_image_surface_get_height(img) == 24);
  if (img) cairo_surface_destroy(img);
}

// Remove Location rewrites the photo itself: a symbolic link stays a link,
// and the photo keeps its permissions and its dates.
static void test_rewrite_in_place() {
  std::string photo = g_tmp + "/private.jpg", link = g_tmp + "/link.jpg";
  CHECK(sde::write_file_atomic(photo, "old contents"));
  CHECK(chmod(photo.c_str(), 0600) == 0);
  struct timespec times[2] = {{1700000000, 0}, {1700000000, 0}};
  CHECK(utimensat(AT_FDCWD, photo.c_str(), times, 0) == 0);
  CHECK(symlink("private.jpg", link.c_str()) == 0);
  CHECK(gallery::rewrite_photo_in_place(link, "new contents"));
  struct stat st {};
  CHECK(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
  CHECK(stat(photo.c_str(), &st) == 0 && (st.st_mode & 07777) == 0600 && st.st_mtime == 1700000000);
  std::string now;
  CHECK(sde::read_file(photo, now) && now == "new contents");
  CHECK(gallery::rewrite_photo_in_place(photo, "again"));
  CHECK(stat(photo.c_str(), &st) == 0 && (st.st_mode & 07777) == 0600 && st.st_mtime == 1700000000);
}

int main() {
  char tmpl[] = "/tmp/sde-photo-plugins-XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  g_tmp = tmpl;
  setenv("LC_ALL", "C", 1);
  test_auto_adjust();
  test_frame();
  test_focus_blur_and_watermark();
  test_red_eye();
  test_tiff_gps();
  test_jpeg_metadata();
  test_png_metadata();
  test_rewrite_in_place();
  std::string rm = "rm -rf '" + g_tmp + "'";
  if (system(rm.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("photo plugin tests passed\n");
  return 0;
}
