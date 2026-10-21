// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Xanara core tests: rendering, editing round trips, tile cache LRU,
// page ranges and history. No display is needed.
#include <cairo-pdf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core.h"
#include "util.h"

using namespace xanara;

static int failures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                           \
    }                                                                       \
  } while (0)

static std::string make_pdf(const std::string &dir, int pages) {
  std::string path = dir + "/sample.pdf";
  cairo_surface_t *s = cairo_pdf_surface_create(path.c_str(), 595, 842);  // A4
  cairo_t *cr = cairo_create(s);
  for (int p = 0; p < pages; ++p) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 18);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_move_to(cr, 72, 100);
    std::string line = "Xanara sharp text page " + std::to_string(p + 1);
    cairo_show_text(cr, line.c_str());
    cairo_move_to(cr, 72, 140);
    cairo_show_text(cr, "The quick brown fox jumps over the lazy dog");
    cairo_rectangle(cr, 72, 200, 200, 100);
    cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);
    cairo_fill(cr);
    cairo_show_page(cr);
  }
  cairo_destroy(cr);
  cairo_surface_destroy(s);
  return path;
}

static int dark_pixels(cairo_surface_t *s) {
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  int stride = cairo_image_surface_get_stride(s), n = 0;
  unsigned char *d = cairo_image_surface_get_data(s);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint32_t p = *reinterpret_cast<uint32_t *>(d + y * stride + x * 4);
      int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
      if (r < 80 && g < 80 && b < 80) ++n;
    }
  return n;
}

static uint32_t pixel(cairo_surface_t *s, int x, int y) {
  unsigned char *d = cairo_image_surface_get_data(s);
  return *reinterpret_cast<uint32_t *>(d + y * cairo_image_surface_get_stride(s) + x * 4) & 0xffffff;
}

static void test_geometry() {
  RenderParams rp;
  rp.scale = 2.0;
  for (int rot : {0, 90, 180, 270}) {
    rp.rotation = rot;
    double dx, dy, px, py;
    page_to_device(595, 842, rp, 100, 200, dx, dy);
    device_to_page(595, 842, rp, dx, dy, px, py);
    CHECK(std::fabs(px - 100) < 1e-9 && std::fabs(py - 200) < 1e-9);
    int w, h;
    page_pixels(595, 842, rp, w, h);
    CHECK((rot % 180 == 0) ? (w == 1190 && h == 1684) : (w == 1684 && h == 1190));
  }
}

static void test_render_and_edit(const std::string &dir) {
  std::string path = make_pdf(dir, 2);
  std::string error;
  bool encrypted = false;
  auto doc = Document::open(path, "", error, encrypted);
  CHECK(doc != nullptr);
  if (!doc) {
    fprintf(stderr, "open failed: %s\n", error.c_str());
    return;
  }
  CHECK(doc->count() == 2);
  CHECK(std::fabs(doc->width(0) - 595) < 0.5 && std::fabs(doc->height(0) - 842) < 0.5);

  // Two zoom levels: the output is exactly page size x scale, and text
  // coverage grows with the square of the zoom (real re-rendering, not a
  // scaled bitmap).
  RenderParams a, b;
  a.scale = 1.0;
  b.scale = 2.5;
  int aw, ah, bw, bh;
  page_pixels(doc->width(0), doc->height(0), a, aw, ah);
  page_pixels(doc->width(0), doc->height(0), b, bw, bh);
  cairo_surface_t *sa = doc->render(0, a, 0, 0, aw, ah);
  cairo_surface_t *sb = doc->render(0, b, 0, 0, bw, bh);
  CHECK(cairo_image_surface_get_width(sa) == 595 && cairo_image_surface_get_height(sa) == 842);
  CHECK(cairo_image_surface_get_width(sb) == 1488 && cairo_image_surface_get_height(sb) == 2105);
  int da = dark_pixels(sa), db = dark_pixels(sb);
  CHECK(da > 100);
  CHECK(db > da * 4);
  // A tile of the larger render matches the same area of the full render.
  cairo_surface_t *tile = doc->render(0, b, 512, 0, 512, 512);
  CHECK(cairo_image_surface_get_width(tile) == 512);
  CHECK(pixel(tile, 0, 300) == pixel(sb, 512, 300));
  cairo_surface_destroy(tile);
  // Red square at (72..272, 200..300) pt.
  uint32_t red = pixel(sa, 150, 250);
  CHECK(((red >> 16) & 255) > 180 && ((red >> 8) & 255) < 60);
  // Night mode keeps the hue (red stays reddish) and darkens paper.
  apply_color_mode(sa, ColorMode::Night);
  uint32_t paper = pixel(sa, 5, 5), nred = pixel(sa, 150, 250);
  CHECK(((paper >> 16) & 255) < 60);
  CHECK(((nred >> 16) & 255) > ((nred >> 8) & 255));
  cairo_surface_destroy(sa);
  cairo_surface_destroy(sb);

  // Text: find and selection.
  auto hits = doc->find(0, "quick brown", false, false);
  CHECK(hits.size() == 1);
  if (!hits.empty()) CHECK(hits[0].y1 > 110 && hits[0].y2 < 150 && hits[0].x1 > 60);
  CHECK(doc->find(1, "page 2", false, false).size() == 1);
  Box sel{70, 125, 400, 145};
  auto boxes = doc->selection_boxes(0, sel, POPPLER_SELECTION_GLYPH);
  CHECK(!boxes.empty());
  std::string text = doc->selected_text(0, sel, POPPLER_SELECTION_GLYPH);
  CHECK(text.find("quick") != std::string::npos);

  // Annotations of every kind.
  CHECK(doc->annotations(0).empty());
  CHECK(doc->add_markup(0, MarkupKind::Highlight, boxes, Rgb{1, 0.85, 0.1}) != nullptr);
  CHECK(doc->add_markup(0, MarkupKind::Underline, hits, Rgb{0, 0.4, 1}) != nullptr);
  CHECK(doc->add_note(0, 400, 90, "Remember this", Rgb{1, 0.8, 0}) != nullptr);
  CHECK(doc->add_square(0, Box{300, 400, 450, 480}, Rgb{0, 0.6, 0.2}, 2) != nullptr);
  CHECK(doc->add_line(0, Point{80, 500}, Point{300, 560}, Rgb{0.2, 0.2, 0.9}, 2) != nullptr);
  CHECK(doc->add_ink(1, {{{100, 600}, {150, 620}, {200, 610}}}, Rgb{0.9, 0, 0.3}, 2.5) != nullptr);
  CHECK(doc->add_free_text(1, Box{100, 300, 300, 360}, "Free text box", Rgb{0, 0, 0}, 12) != nullptr);
  CHECK(doc->modified());
  size_t n0 = doc->annotations(0).size();
  CHECK(n0 == 5);
  CHECK(doc->annotations(1).size() == 2);
  // Move and recolour the square.
  for (const auto &an : doc->annotations(0))
    if (an.type == POPPLER_ANNOT_SQUARE) {
      CHECK(doc->move_annot(0, an.annot, 10, 20));
      break;
    }
  for (const auto &an : doc->annotations(0))
    if (an.type == POPPLER_ANNOT_SQUARE) {
      CHECK(std::fabs(an.box.x1 - 310) < 1.5 && std::fabs(an.box.y1 - 420) < 1.5);
      CHECK(doc->set_annot_color(0, an.annot, Rgb{1, 0, 0}));
      break;
    }
  // Delete the line.
  for (const auto &an : doc->annotations(0))
    if (an.type == POPPLER_ANNOT_LINE) {
      CHECK(doc->remove_annot(0, an.annot));
      break;
    }
  CHECK(doc->annotations(0).size() == 4);
  uint64_t rev = doc->revision(0);
  CHECK(rev > 1);

  // Save to a new file and over the original; reopen and verify.
  std::string copy = dir + "/copy.pdf";
  CHECK(doc->save_to(copy, error));
  CHECK(doc->save_to(path, error));
  doc.reset();
  for (const std::string &p : {copy, path}) {
    auto again = Document::open(p, "", error, encrypted);
    CHECK(again != nullptr);
    if (!again) continue;
    const auto &an = again->annotations(0);
    CHECK(an.size() == 4);
    bool note = false, highlight = false, square = false;
    for (const auto &x : an) {
      if (x.type == POPPLER_ANNOT_TEXT && x.contents == "Remember this") note = true;
      if (x.type == POPPLER_ANNOT_HIGHLIGHT) highlight = true;
      if (x.type == POPPLER_ANNOT_SQUARE && x.has_color && x.color.r > 0.9 && x.color.g < 0.1) square = true;
    }
    CHECK(note && highlight && square);
    CHECK(again->annotations(1).size() == 2);
  }
  // No temporary files are left behind.
  int stray = 0;
  for (const auto &f : sde::list_dir(dir))
    if (f.find(".xanara-") != std::string::npos) ++stray;
  CHECK(stray == 0);
}

static cairo_surface_t *tile(int w, int h) { return cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h); }

static void test_cache() {
  size_t one = TileCache::surface_bytes(tile(64, 64));  // leaks one tiny surface; fine in a test
  CHECK(one == 64 * 64 * 4);
  TileCache cache(one * 3);
  auto key = [](int page, int col) {
    TileKey k;
    k.page = page;
    k.col = col;
    k.scale_q = 10000;
    k.revision = 1;
    return k;
  };
  cache.put(key(0, 0), tile(64, 64));
  cache.put(key(0, 1), tile(64, 64));
  cache.put(key(1, 0), tile(64, 64));
  CHECK(cache.size() == 3 && cache.bytes() == one * 3);
  CHECK(cache.get(key(0, 0)) != nullptr);  // now most recent
  cache.put(key(2, 0), tile(64, 64));     // evicts (0,1), the least recent
  CHECK(cache.size() == 3);
  CHECK(!cache.contains(key(0, 1)));
  CHECK(cache.contains(key(0, 0)) && cache.contains(key(1, 0)) && cache.contains(key(2, 0)));
  // A tile larger than the whole budget is still kept (it is on screen).
  cache.put(key(3, 0), tile(256, 256));
  CHECK(cache.contains(key(3, 0)) && cache.size() == 1);
  // Replacing a key does not leak bytes; revision is part of the key.
  cache.set_budget(one * 10);
  cache.put(key(4, 0), tile(64, 64));
  cache.put(key(4, 0), tile(64, 64));
  TileKey k2 = key(4, 0);
  k2.revision = 2;
  CHECK(!cache.contains(k2));
  int seen = 0;
  cache.each_of_page(4, [&](const TileKey &, cairo_surface_t *) { ++seen; });
  CHECK(seen == 1);
  cache.drop_page(4);
  CHECK(!cache.contains(key(4, 0)));
  cache.clear();
  CHECK(cache.size() == 0 && cache.bytes() == 0);
}

static void test_ranges_history(const std::string &dir) {
  std::vector<int> p;
  CHECK(parse_page_ranges("1-3,5,7-z", 9, p));
  CHECK((p == std::vector<int>{0, 1, 2, 4, 6, 7, 8}));
  CHECK(format_page_ranges(p) == "1-3,5,7-9");
  CHECK(parse_page_ranges("3-1", 5, p) && (p == std::vector<int>{2, 1, 0}));
  CHECK(!parse_page_ranges("0", 5, p));
  CHECK(!parse_page_ranges("4-9", 5, p));
  CHECK(!parse_page_ranges("x", 5, p));

  History h(dir + "/state/history");
  h.remember({dir + "/a.pdf", 3, "width", 10});
  h.remember({dir + "/b.pdf", 7, "1.5", 20});
  h.remember({dir + "/a.pdf", 4, "page", 30});
  CHECK(h.save());
  History h2(dir + "/state/history");
  h2.load();
  auto r = h2.recent(10, false);
  CHECK(r.size() == 2 && r[0].path == dir + "/a.pdf" && r[0].page == 4 && r[0].zoom == "page");
  CHECK(h2.find(dir + "/b.pdf") && h2.find(dir + "/b.pdf")->zoom == "1.5");
  CHECK(h2.recent(10, true).empty());  // the files do not exist
}

static void test_threaded_render(const std::string &dir) {
  std::string error;
  bool enc;
  auto doc = Document::open(dir + "/copy.pdf", "", error, enc);
  CHECK(doc != nullptr);
  if (!doc) return;
  std::atomic<int> notified{0};
  {
    Renderer r(doc.get(), [&] { ++notified; });
    std::vector<RenderJob> jobs;
    for (int i = 0; i < 4; ++i) {
      RenderJob j;
      j.key.page = i % 2;
      j.key.col = i;
      j.params.scale = 1.5;
      j.tx = i * 128;
      j.tw = j.th = 128;
      jobs.push_back(j);
    }
    r.replace_queue(jobs);
    std::vector<RenderResult> got;
    for (int i = 0; i < 400 && got.size() < 4; ++i) {
      usleep(10000);
      for (auto &x : r.take_results()) got.push_back(x);
    }
    CHECK(got.size() == 4);
    for (auto &x : got) {
      CHECK(x.surface && cairo_image_surface_get_width(x.surface) == 128);
      cairo_surface_destroy(x.surface);
    }
  }
  CHECK(notified > 0);
}

int main() {
  char tmpl[] = "/tmp/xanara-test-XXXXXX";
  const char *dir = mkdtemp(tmpl);
  if (!dir) return 1;
  test_geometry();
  test_render_and_edit(dir);
  test_cache();
  test_ranges_history(dir);
  test_threaded_render(dir);
  std::string cmd = std::string("rm -rf '") + dir + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", dir);
  printf("xanara: free text %s, ink %s\n", native_free_text() ? "native" : "stamp",
         native_ink() ? "native" : "stamp");
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("all xanara tests passed\n");
  return 0;
}
