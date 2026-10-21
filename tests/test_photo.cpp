// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Unit tests for Photo's editing model and saving (no display).
#include <cairo.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "edit.h"
#include "exif.h"
#include "save.h"
#include "test_photo_fixture.h"
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

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
static bool near(const RGB &a, const RGB &b, double eps = 1e-6) {
  return near(a.r, b.r, eps) && near(a.g, b.g, eps) && near(a.b, b.b, eps);
}

static void test_adjustments() {
  Adjust none;
  CHECK(none.identity());
  for (double v : {0.0, 0.1, 0.5, 0.73, 1.0}) {
    RGB c{v, 1 - v, v * 0.5};
    CHECK(near(adjust_pixel(c, none), c));
  }
  // sRGB transfer round trip.
  for (double v : {0.0, 0.02, 0.2, 0.5, 0.9, 1.0}) CHECK(near(linear_to_srgb(srgb_to_linear(v)), v, 1e-9));
  RGB mid{0.5, 0.5, 0.5};
  // Exposure: +1 EV doubles linear light; -1 EV halves it.
  Adjust ev;
  ev.exposure = 50;
  CHECK(near(srgb_to_linear(adjust_pixel(mid, ev).r), srgb_to_linear(0.5) * 2, 1e-9));
  ev.exposure = -50;
  CHECK(near(srgb_to_linear(adjust_pixel(mid, ev).r), srgb_to_linear(0.5) / 2, 1e-9));
  // Brightness raises midtones, keeps white; negative keeps black.
  Adjust br;
  br.brightness = 50;
  CHECK(adjust_pixel(mid, br).r > 0.5);
  CHECK(near(adjust_pixel({1, 1, 1}, br).r, 1));
  br.brightness = -50;
  CHECK(adjust_pixel(mid, br).r < 0.5);
  CHECK(near(adjust_pixel({0, 0, 0}, br).r, 0));
  // Brightness is monotonic.
  br.brightness = 30;
  double prev = -1;
  for (int i = 0; i <= 20; ++i) {
    double v = adjust_pixel({i / 20.0, 0, 0}, br).r;
    CHECK(v >= prev);
    prev = v;
  }
  // Contrast pivots on 0.5.
  Adjust ct;
  ct.contrast = 100;
  CHECK(near(adjust_pixel(mid, ct).r, 0.5));
  CHECK(near(adjust_pixel({0.75, 0.75, 0.75}, ct).r, 1.0));
  CHECK(near(adjust_pixel({0.3, 0.3, 0.3}, ct).r, 0.1));
  ct.contrast = -50;
  CHECK(near(adjust_pixel({0.75, 0.75, 0.75}, ct).r, 0.65));
  // Saturation -100 gives grey of the same luma; luma is kept in general.
  RGB col{0.8, 0.3, 0.1};
  Adjust sat;
  sat.saturation = -100;
  RGB grey = adjust_pixel(col, sat);
  CHECK(near(grey.r, grey.g) && near(grey.g, grey.b) && near(grey.r, luma(col)));
  sat.saturation = 50;
  RGB vivid = adjust_pixel({0.6, 0.4, 0.5}, sat);
  CHECK(near(luma(vivid), luma({0.6, 0.4, 0.5}), 1e-9));
  CHECK(vivid.r - vivid.g > 0.2);
  // Warmth: warmer = more red, less blue.
  Adjust wm;
  wm.warmth = 100;
  RGB w = adjust_pixel(mid, wm);
  CHECK(w.r > 0.5 && w.b < 0.5);
  wm.warmth = -100;
  w = adjust_pixel(mid, wm);
  CHECK(w.r < 0.5 && w.b > 0.5);
  // Fade lifts black.
  Adjust fd;
  fd.fade = 100;
  CHECK(near(adjust_pixel({0, 0, 0}, fd).r, 0.12));
  // Results stay in range.
  Adjust wild;
  wild.exposure = 100;
  wild.contrast = 100;
  wild.saturation = 100;
  wild.warmth = 100;
  RGB r = adjust_pixel({0.9, 0.2, 0.95}, wild);
  CHECK(r.r <= 1 && r.g >= 0 && r.b <= 1 && r.b >= 0);
  // Vignette: no change at the centre, darker in the corners.
  CHECK(near(vignette_factor(0, 100), 1) && near(vignette_factor(0.3, 100), 1));
  CHECK(vignette_factor(1, 100) < 0.4 && vignette_factor(1, 0) == 1);
  CHECK(vignette_factor(0.8, 50) > vignette_factor(0.9, 50));
}

static void test_filters() {
  RGB c{0.8, 0.4, 0.2};
  RGB bw = filter_pixel(c, Filter::Mono, 1);
  CHECK(near(bw.r, bw.g) && near(bw.r, luma(c)));
  RGB half = filter_pixel(c, Filter::Mono, 0.5);
  CHECK(near(half.r, (c.r + bw.r) / 2));
  RGB inv = filter_pixel(c, Filter::Invert, 1);
  CHECK(near(inv, {0.2, 0.6, 0.8}));
  RGB sep = filter_pixel({0.5, 0.5, 0.5}, Filter::Sepia, 1);
  CHECK(sep.r > sep.g && sep.g > sep.b);
  CHECK(near(filter_pixel(c, Filter::Vivid, 1), c));  // tone-only filters
  CHECK(near(filter_pixel(c, Filter::Mono, 0), c));
  CHECK(filter_preset(Filter::Original).identity() && filter_preset(Filter::Invert).identity());
  CHECK(filter_preset(Filter::Vivid).saturation > 0 && filter_preset(Filter::Cool).warmth < 0);
  Filter f;
  CHECK(parse_filter("sepia", f) && f == Filter::Sepia);
  CHECK(std::string(filter_key(Filter::Mono)) == "mono");
  CHECK(!parse_filter("nope", f));
  CHECK(filters().size() == 10);
}

static cairo_surface_t *make_surface(int w, int h) {
  // Red left half, blue right half, a green top-left pixel.
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_surface_flush(s);
  unsigned char *d = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      reinterpret_cast<uint32_t *>(d + y * stride)[x] = x < w / 2 ? 0xFFFF0000 : 0xFF0000FF;
  reinterpret_cast<uint32_t *>(d)[0] = 0xFF00FF00;
  cairo_surface_mark_dirty(s);
  return s;
}

static uint32_t pixel(cairo_surface_t *s, int x, int y) {
  cairo_surface_flush(s);
  return reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s))[x];
}

static void test_geometry() {
  cairo_surface_t *src = make_surface(40, 20);
  Edits e;
  CHECK(e.identity() && e.geometry_only());
  int ow, oh;
  result_size(e, 40, 20, ow, oh);
  CHECK(ow == 40 && oh == 20);
  // Identity rendering returns the original untouched.
  cairo_surface_t *same = render(src, e);
  CHECK(same == src);
  cairo_surface_destroy(same);
  // Rotate clockwise: the green top-left pixel moves to the top-right.
  e.orient = {1, false};
  CHECK(!e.identity() && e.geometry_only());
  result_size(e, 40, 20, ow, oh);
  CHECK(ow == 20 && oh == 40);
  cairo_surface_t *rot = render(src, e);
  CHECK(cairo_image_surface_get_width(rot) == 20 && cairo_image_surface_get_height(rot) == 40);
  CHECK(pixel(rot, 19, 0) == 0xFF00FF00);
  cairo_surface_destroy(rot);
  // Mirror: the green pixel goes to the top-right of the 40x20 image.
  e.orient = {0, true};
  cairo_surface_t *mir = render(src, e);
  CHECK(pixel(mir, 39, 0) == 0xFF00FF00);
  cairo_surface_destroy(mir);
  // Crop the right half: all blue.
  e = Edits();
  e.crop = {0.5, 0, 0.5, 1};
  CHECK(!e.geometry_only());
  result_size(e, 40, 20, ow, oh);
  CHECK(ow == 20 && oh == 20);
  cairo_surface_t *crop = render(src, e);
  CHECK(cairo_image_surface_get_width(crop) == 20);
  CHECK(pixel(crop, 0, 10) == 0xFF0000FF && pixel(crop, 19, 19) == 0xFF0000FF);
  cairo_surface_destroy(crop);
  int cx, cy, cw, ch;
  crop_pixels({0.25, 0.5, 2.0, 2.0}, 40, 20, cx, cy, cw, ch);
  CHECK(cx == 10 && cy == 10 && cw == 30 && ch == 10);
  // Resize.
  e = Edits();
  e.resize_w = 10;
  e.resize_h = 5;
  cairo_surface_t *small = render(src, e);
  CHECK(cairo_image_surface_get_width(small) == 10 && cairo_image_surface_get_height(small) == 5);
  CHECK(((pixel(small, 1, 3) >> 16) & 0xFF) > 200);
  cairo_surface_destroy(small);
  // Tone: invert makes red cyan and does not touch the source.
  e = Edits();
  e.filter = Filter::Invert;
  cairo_surface_t *inv = render(src, e);
  CHECK(inv != src);
  CHECK(pixel(inv, 5, 10) == 0xFF00FFFF);
  CHECK(pixel(src, 5, 10) == 0xFFFF0000);
  cairo_surface_destroy(inv);
  // Big image goes through the threaded path with the same result.
  cairo_surface_t *big = make_surface(1000, 600);
  e.filter = Filter::Mono;
  cairo_surface_t *mono = render(big, e);
  uint32_t p = pixel(mono, 900, 500);
  CHECK(((p >> 16) & 0xFF) == (p & 0xFF) && ((p >> 8) & 0xFF) == (p & 0xFF));
  cairo_surface_destroy(mono);
  cairo_surface_t *ds = downscale(big, 100);
  CHECK(cairo_image_surface_get_width(ds) == 100 && cairo_image_surface_get_height(ds) == 60);
  cairo_surface_destroy(ds);
  cairo_surface_destroy(big);
  cairo_surface_destroy(src);
}

static void test_history() {
  History h;
  CHECK(!h.can_undo() && !h.can_redo());
  Edits a;
  a.adjust.brightness = 10;
  h.push(a);
  h.push(a);  // duplicate ignored
  CHECK(h.size() == 2 && h.can_undo());
  Edits b = a;
  b.filter = Filter::Sepia;
  h.push(b);
  b.strength = 50;
  h.amend(b);  // a slider drag does not add steps
  CHECK(h.size() == 3 && h.current().strength == 50);
  CHECK(h.undo() == a);
  CHECK(h.can_redo());
  CHECK(h.redo().filter == Filter::Sepia);
  h.undo();
  Edits c;
  c.orient = {2, false};
  h.push(c);  // a new edit drops the redo branch
  CHECK(!h.can_redo() && h.size() == 3);
  CHECK(h.undo() == a && h.undo() == Edits() && !h.can_undo());
}

static void test_saving() {
  char tmpl[] = "/tmp/sde-photo-test-XXXXXX";
  char *root = mkdtemp(tmpl);
  CHECK(root != nullptr);
  if (!root) return;
  std::string dir = root;
  CHECK(format_of("/a/B.JPEG") == Format::Jpeg && format_of("x.png") == Format::Png);
  CHECK(output_format(Format::Gif) == Format::Png && output_format(Format::Svg) == Format::Png);
  CHECK(replace_extension("/a/b.c/pic.gif", Format::Png) == "/a/b.c/pic.png");
  CHECK(replace_extension("/a/b.c/pic", Format::Jpeg) == "/a/b.c/pic.jpg");
  cairo_surface_t *s = make_surface(64, 32);
  std::string png = dir + "/out.png";
  CHECK(write_image(s, png, Format::Png, 90));
  CHECK(sniff_format(png) == Format::Png);
  CHECK(!png_bytes(s).empty());
  std::string jpeg = fixture::encode_jpeg(64, 32);
  if (!jpeg.empty() && can_write(Format::Jpeg)) {
    // A JPEG with camera EXIF.
    fixture::Tiff t(true);
    std::string seg_tiff = t.build({t.ascii(0x010F, "Canon"), t.shrt(0x0112, 1)}, {t.shrt(0x8827, 200)}, {});
    std::string app1 = std::string("Exif\0\0", 6) + seg_tiff;
    std::string seg = "\xFF\xE1";
    seg += char((app1.size() + 2) >> 8);
    seg += char((app1.size() + 2) & 0xFF);
    seg += app1;
    CHECK(gallery::jpeg_replace_exif(jpeg, seg));
    std::string src = dir + "/cam.jpg";
    CHECK(sde::write_file_atomic(src, jpeg));
    // Rotation only: lossless, image data byte-identical apart from the tag.
    Edits e;
    e.orient = {1, false};
    std::string dst = dir + "/cam-rot.jpg";
    CHECK(lossless_possible(src, e, dst));
    CHECK(save_edits(src, e, nullptr, dst, 90) == SaveResult::Lossless);
    std::string a, b;
    sde::read_file(src, a);
    sde::read_file(dst, b);
    CHECK(a.size() == b.size());
    CHECK(gallery::jpeg_orientation(dst) == 6);
    // Rotating the result again composes with the stored orientation.
    CHECK(save_edits(dst, e, nullptr, dst, 90) == SaveResult::Lossless);
    CHECK(gallery::jpeg_orientation(dst) == 3);
    int w = 0, h = 0;
    cairo_surface_t *up = gallery::decode_image(dst, 0, &w, &h);
    CHECK(up && w == 64 && h == 32);
    if (up) cairo_surface_destroy(up);
    // A tone edit re-encodes but keeps the camera metadata.
    e = Edits();
    e.adjust.saturation = -100;
    CHECK(!lossless_possible(src, e, dst));
    cairo_surface_t *orig = gallery::decode_image(src, 0);
    cairo_surface_t *out = render(orig, e);
    std::string enc = dir + "/cam-bw.jpg";
    CHECK(save_edits(src, e, out, enc, 80) == SaveResult::Encoded);
    gallery::PhotoInfo info;
    CHECK(gallery::read_photo_info_builtin(enc, info));
    CHECK(info.make == "Canon" && info.iso == 200 && info.orientation == 1);
    CHECK(info.pixel_w == 0);  // the source had no pixel size tags
    cairo_surface_t *back = gallery::decode_image(enc, 0, &w, &h);
    CHECK(back && w == 64 && h == 32);
    if (back) {
      uint32_t p = pixel(back, 10, 16);
      int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, bl = p & 0xFF;
      CHECK(std::abs(r - g) < 12 && std::abs(g - bl) < 12);
      cairo_surface_destroy(back);
    }
    cairo_surface_destroy(out);
    cairo_surface_destroy(orig);
    // Saving as PNG from a JPEG is never lossless.
    e = Edits();
    e.orient = {1, false};
    CHECK(!lossless_possible(src, e, dir + "/x.png"));
  } else {
    fprintf(stderr, "note: JPEG encoder not available, lossless tests skipped\n");
  }
  cairo_surface_destroy(s);
  std::string cmd = "rm -rf '" + dir + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", root);
}

int main() {
  setenv("LC_ALL", "C", 1);
  test_adjustments();
  test_filters();
  test_geometry();
  test_history();
  test_saving();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("photo tests passed\n");
  return 0;
}
