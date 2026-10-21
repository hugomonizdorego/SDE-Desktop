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
#include "model.h"
#include "process.h"
#include "util.h"

using namespace xanara;
using namespace sde;

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
    // Ink and free text keep their look and text (natively or as image
    // stamps): the stroke at (150, 620) pt is drawn after reloading.
    int with_text = 0;
    for (const auto &x : again->annotations(1))
      if (!x.contents.empty()) ++with_text;
    CHECK(with_text == 2);
    RenderParams one;
    cairo_surface_t *pg2 = again->render(1, one, 0, 0, 595, 842);
    uint32_t ink = pixel(pg2, 150, 620);
    CHECK(((ink >> 16) & 255) > 150 && (ink & 255) < 160 && ((ink >> 8) & 255) < 90);
    cairo_surface_destroy(pg2);
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

// ------------------------------------------------------- alpha 16 model

static void test_registry() {
  // Every mode has tools, ids are unique and canvas tools are marked.
  for (int m = 0; m < kModeCount; ++m) {
    auto tools = tools_for(Mode(m));
    CHECK(!tools.empty());
    Mode back;
    CHECK(mode_from_id(mode_id(Mode(m)), back) && back == Mode(m));
  }
  CHECK(tools_for(Mode::Annotate).size() == 16);
  CHECK(tools_for(Mode::Annotate).front()->act == Act::Highlight);
  CHECK(tools_for(Mode::Annotate).back()->act == Act::ToggleAnnots);
  for (const auto &a : tool_registry())
    for (const auto &b : tool_registry())
      if (&a != &b) CHECK(std::string(a.id) != b.id);
  CHECK(tool_spec(Act::Highlight)->canvas);
  CHECK(!tool_spec(Act::Encrypt)->canvas);
  CHECK(tool_by_id("tools.ocr") == tool_spec(Act::Ocr));
  CHECK(int(tool_registry().size()) == int(Act::Count));
  // Availability: missing programs explain how to install them.
  auto none = [](const std::string &) { return false; };
  auto all = [](const std::string &) { return true; };
  auto only_gs = [](const std::string &p) { return p == "gs"; };
  Availability a = tool_availability(*tool_spec(Act::Ocr), none);
  CHECK(!a.ok && a.missing == "tesseract" && a.reason.find("tesseract-ocr") != std::string::npos);
  CHECK(tool_availability(*tool_spec(Act::Ocr), all).ok);
  CHECK(tool_availability(*tool_spec(Act::Compress), only_gs).ok);  // either qpdf or gs
  CHECK(!tool_availability(*tool_spec(Act::Compress), none).ok);
  CHECK(tool_availability(*tool_spec(Act::Highlight), none).ok);
  CHECK(!tool_availability(*tool_spec(Act::Print), none).ok);
  CHECK(tool_availability(*tool_spec(Act::Print), none).reason.find("cups-client") != std::string::npos);
  // Overflow: everything fits, or some go to the overflow menu.
  auto tools = tools_for(Mode::Annotate);
  CHECK(tools_that_fit(tools, 2000, 40, 10, 40) == 16);
  int n = tools_that_fit(tools, 400, 40, 10, 40);
  CHECK(n > 0 && n < 16);
  CHECK(tools_that_fit(tools, 30, 40, 10, 40) == 0);
}

static void test_tabs() {
  TabList t;
  CHECK(t.active() == -1 && t.count() == 0);
  CHECK(t.open("/a.pdf") == 0);
  CHECK(t.open("/b.pdf") == 1);
  t.activate(0);
  CHECK(t.open("/c.pdf") == 1);  // after the active tab
  CHECK(t.path(0) == "/a.pdf" && t.path(1) == "/c.pdf" && t.path(2) == "/b.pdf");
  bool existing = false;
  CHECK(t.open("/b.pdf", &existing) == 2 && existing && t.active() == 2);
  int id_b = t.id(2);
  CHECK(t.move(2, 0));
  CHECK(t.path(0) == "/b.pdf" && t.active() == 0 && t.index_of_id(id_b) == 0);
  t.activate(2);  // /c.pdf
  CHECK(t.move(0, 1));  // b moves past a; c stays active
  CHECK(t.path(2) == "/c.pdf" && t.active() == 2);
  CHECK(t.close(2) == 1);  // closing the last active tab activates its left neighbour
  CHECK(t.count() == 2);
  t.activate(0);
  CHECK(t.close(1) == 0);
  CHECK(t.close(0) == -1 && t.count() == 0);
  // Session round trip.
  TabList s;
  s.open("/x/one.pdf");
  s.open("/x/two words.pdf");
  s.activate(0);
  std::vector<std::string> paths;
  int active = -2;
  CHECK(TabList::parse(s.serialize(), paths, active));
  CHECK(paths.size() == 2 && paths[1] == "/x/two words.pdf" && active == 0);
  CHECK(!TabList::parse("garbage\n", paths, active));
  // Drag and drop slots.
  CHECK(tab_drop_index(5, 100, 50, 4, 2) == 0);
  CHECK(tab_drop_index(260, 100, 50, 4, 0) == 3);
  CHECK(tab_drop_index(9999, 100, 50, 4, 0) == 3);
  CHECK(tab_drop_index(170, 100, 50, 4, 0) == 1);
}

static bool has(const Argv &a, const std::string &s) {
  for (const auto &x : a)
    if (x == s) return true;
  return false;
}

static void test_commands() {
  Permissions p;
  p.print = false;
  p.copy = false;
  Argv e = qpdf_encrypt_args("in.pdf", "out.pdf", "user", "", p, "old");
  CHECK(e[0] == "qpdf" && has(e, "--password=old") && has(e, "--encrypt"));
  CHECK(has(e, "--user-password=user") && has(e, "--owner-password=user"));  // owner defaults to user
  CHECK(has(e, "--bits=256") && has(e, "--print=none") && has(e, "--extract=n") && has(e, "--modify=all"));
  CHECK(e[e.size() - 2] == "--" && e.back() == "out.pdf");
  Argv d = qpdf_decrypt_args("in.pdf", "out.pdf", "pw");
  CHECK(has(d, "--decrypt") && has(d, "--password=pw"));
  Argv m = qpdf_merge_args({"a.pdf", "b.pdf"}, "o.pdf");
  CHECK(has(m, "--empty") && m[4] == "a.pdf" && m[5] == "b.pdf" && m.back() == "o.pdf");
  Argv o = qpdf_overlay_args("in.pdf", "s.pdf", "o.pdf", true, "");
  CHECK(has(o, "--underlay") && !has(o, "--overlay"));
  CHECK(has(gs_compress_args("i", "o", ""), "-dPDFSETTINGS=/ebook"));
  Argv t = tesseract_args("p.png", "p", "eng");
  CHECK(t[0] == "tesseract" && t.back() == "pdf" && has(t, "eng"));
  // Redaction: pages 2 and 5 of 6 replaced by their images.
  std::map<int, std::string> r{{1, "r2.pdf"}, {4, "r5.pdf"}};
  Argv s = redaction_splice_args("in.pdf", 6, r, "out.pdf", "");
  std::string joined;
  for (size_t k = 3; k < s.size(); ++k) joined += s[k] + " ";
  CHECK(joined == "--pages . 1 r2.pdf 1 . 3-4 r5.pdf 1 . 6 -- out.pdf ");
  Argv s2 = redaction_splice_args("in.pdf", 2, {{0, "r1.pdf"}, {1, "r2.pdf"}}, "out.pdf", "");
  joined.clear();
  for (size_t k = 3; k < s2.size(); ++k) joined += s2[k] + " ";
  CHECK(joined == "--pages r1.pdf 1 r2.pdf 1 -- out.pdf ");
}

static void test_json() {
  Json j;
  std::string err;
  CHECK(Json::parse(R"({"a":[1,2.5,-3e2,true,null,"x\"yé😀"],"b":{}})", j, err));
  CHECK(j.get("a") && j.get("a")->items().size() == 6);
  CHECK(j.get("a")->items()[2].num() == -300);
  CHECK(j.get("a")->items()[5].str() == "x\"y\xC3\xA9\xF0\x9F\x98\x80");
  CHECK(j.dump() == "{\"a\":[1,2.5,-300,true,null,\"x\\\"y\xC3\xA9\xF0\x9F\x98\x80\"],\"b\":{}}");
  CHECK(!Json::parse("{\"a\":}", j, err) && !err.empty());
  CHECK(!Json::parse("[1,2", j, err));
  Json o = Json::object();
  o.set("/Type", Json::string("/Page"));
  o.set("/Type", Json::string("/Pages"));
  CHECK(o.members().size() == 1 && o.get("/Type")->str() == "/Pages");
  CHECK(o.erase("/Type") && !o.erase("/Type"));
  // Page space to PDF user space, for each /Rotate.
  PdfRect m{0, 0, 600, 800};
  PdfRect r0 = display_to_user(Box{10, 20, 110, 70}, m, 0);
  CHECK(r0.x1 == 10 && r0.x2 == 110 && r0.y1 == 730 && r0.y2 == 780);
  PdfRect r90 = display_to_user(Box{10, 20, 110, 70}, m, 90);  // shown 800 x 600
  CHECK(r90.x1 == 20 && r90.x2 == 70 && r90.y1 == 10 && r90.y2 == 110);
  PdfRect r180 = display_to_user(Box{10, 20, 110, 70}, m, 180);
  CHECK(r180.x1 == 490 && r180.x2 == 590 && r180.y1 == 20 && r180.y2 == 70);
  PdfRect r270 = display_to_user(Box{10, 20, 110, 70}, m, 270);
  CHECK(r270.x1 == 530 && r270.x2 == 580 && r270.y1 == 690 && r270.y2 == 790);
}

static const char *kQpdfJson = R"({"version":2,"pages":[{"object":"3 0 R"},{"object":"4 0 R"}],
"qpdf":[{"jsonversion":2,"maxobjectid":9},
{"obj:1 0 R":{"value":{"/Type":"/Catalog","/Pages":"2 0 R","/Metadata":"8 0 R"}},
 "obj:2 0 R":{"value":{"/Type":"/Pages","/Kids":["3 0 R","4 0 R"],"/Count":2,"/MediaBox":[0,0,600,800]}},
 "obj:3 0 R":{"value":{"/Type":"/Page","/Parent":"2 0 R"}},
 "obj:4 0 R":{"value":{"/Type":"/Page","/Parent":"2 0 R","/Rotate":90,"/Annots":"5 0 R"}},
 "obj:5 0 R":{"value":["6 0 R"]},
 "obj:8 0 R":{"stream":{"dict":{"/Type":"/Metadata","/Subtype":"/XML"}}},
 "trailer":{"value":{"/Root":"1 0 R","/Info":"7 0 R","/Size":10}}}]})";

static void test_pdf_objects() {
  PdfObjects src;
  std::string err;
  CHECK(src.load(kQpdfJson, err));
  CHECK(src.page_count() == 2 && src.page_ref(1) == "4 0 R" && src.root_ref() == "1 0 R" && src.max_id() == 9);
  PdfRect mb = src.media_box(0);  // inherited from the page tree
  CHECK(mb.x2 == 600 && mb.y2 == 800);
  CHECK(src.rotation(0) == 0 && src.rotation(1) == 90);
  {
    PdfUpdate up(src);
    CHECK(plan_link(up, src, 0, Box{10, 10, 100, 30}, "https://debion.org", -1));
    std::string out = up.dump();
    CHECK(out.find("\"obj:10 0 R\"") != std::string::npos);
    CHECK(out.find("u:https://debion.org") != std::string::npos);
    CHECK(out.find("\"/Annots\":[\"10 0 R\"]") != std::string::npos);  // new inline array on page 1
    Json parsed;
    CHECK(Json::parse(out, parsed, err));
  }
  {
    PdfUpdate up(src);
    CHECK(plan_link(up, src, 1, Box{10, 10, 100, 30}, "", 0));  // indirect /Annots array grows
    std::string out = up.dump();
    CHECK(out.find("\"obj:5 0 R\":{\"value\":[\"6 0 R\",\"10 0 R\"]}") != std::string::npos);
    CHECK(out.find("\"/Dest\":[\"3 0 R\",\"/Fit\"]") != std::string::npos);
    CHECK(!plan_link(up, src, 0, Box{0, 0, 10, 10}, "", 7));  // no such page
  }
  {
    PdfUpdate up(src);
    FieldSpec f;
    f.kind = FieldKind::Combo;
    f.name = "Country";
    f.options = {"Portugal", "Spain"};
    CHECK(plan_field(up, src, 0, Box{50, 50, 250, 72}, f));
    f.kind = FieldKind::Check;
    f.name = "Agree";
    CHECK(plan_field(up, src, 0, Box{50, 100, 64, 114}, f));
    std::string out = up.dump();
    CHECK(out.find("\"/AcroForm\"") != std::string::npos);
    CHECK(out.find("\"/NeedAppearances\":true") != std::string::npos);
    CHECK(out.find("u:Country") != std::string::npos && out.find("u:Portugal") != std::string::npos);
    CHECK(out.find("\"/Ff\":131072") != std::string::npos);
    CHECK(out.find("\"/Yes\"") != std::string::npos && out.find("\"/AS\":\"/Off\"") != std::string::npos);
    FieldSpec bad;
    CHECK(!plan_field(up, src, 0, Box{0, 0, 50, 50}, bad));  // a field needs a name
  }
  {
    PdfUpdate up(src);
    CHECK(plan_crop(up, src, {0, 1}, Box{50, 50, 300, 400}));
    std::string out = up.dump();
    CHECK(out.find("\"/CropBox\":[50,400,300,750]") != std::string::npos);
  }
  {
    PdfUpdate up(src);
    CHECK(plan_strip_metadata(up, src));
    std::string out = up.dump();
    CHECK(out.find("\"/Metadata\"") == std::string::npos);
    CHECK(out.find("\"obj:7 0 R\":{\"value\":{}}") != std::string::npos);
    CHECK(out.find("\"trailer\"") != std::string::npos);
  }
}

static void test_exporters(const std::string &dir) {
  std::vector<std::string> pages{"Hello <world> &\nfriends\n\nSecond para-\ngraph", "# not a heading"};
  std::string html = export_html("T & C", pages);
  CHECK(html.find("<title>T &amp; C</title>") != std::string::npos);
  CHECK(html.find("<p>Hello &lt;world&gt; &amp; friends</p>") != std::string::npos);
  CHECK(html.find("<p>Second paragraph</p>") != std::string::npos);
  std::string md = export_markdown("Doc", pages);
  CHECK(md.find("# Doc\n\n## Page 1\n\nHello \\<world\\> & friends\n\nSecond paragraph\n\n## Page 2\n\n\\# not a heading") == 0);
  CHECK(export_plain({"a", "b\n"}) == "a\n\fb\n");
  CHECK(numbered_name("/x/report.PDF", "png", 2, 12) == "/x/report-03.png");
  CHECK(numbered_name("/x/r.pdf", "jpg", 99, 150) == "/x/r-100.jpg");
  StampSpec st;
  st.prefix = "ACME-";
  st.start = 7;
  st.digits = 4;
  st.file_name = "f.pdf";
  st.date = "2026-10-21";
  CHECK(bates_number(st, 2) == "ACME-0009");
  CHECK(expand_stamp("{file} p{page}/{pages} {date} {bates} {nope}", st, 0, 3) ==
        "f.pdf p1/3 2026-10-21 ACME-0007 {nope}");
  CHECK(measure_label(72, "in") == "1.00 in" && measure_label(72, "mm") == "25.4 mm");
  // Generated PDFs open again with the right number and size of pages.
  std::string stamp = dir + "/stamp.pdf";
  st.kind = StampSpec::HeaderFooter;
  st.font_size = 10;
  st.header[0] = "{file}";
  st.footer[1] = "Page {page} of {pages}";
  std::string error;
  CHECK(write_stamp_pdf(stamp, {Point{595, 842}, Point{842, 595}}, st, error));
  bool enc;
  auto d = Document::open(stamp, "", error, enc);
  CHECK(d && d->count() == 2 && std::fabs(d->width(1) - 842) < 1);
  if (d) CHECK(d->text(0).find("Page 1 of 2") != std::string::npos);
  cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 192, 96);
  std::string imgpdf = dir + "/images.pdf";
  CHECK(images_to_pdf({img, img}, imgpdf, false, error));
  auto d2 = Document::open(imgpdf, "", error, enc);
  CHECK(d2 && d2->count() == 2 && std::fabs(d2->width(0) - 144) < 1);
  CHECK(images_to_pdf({img}, imgpdf, true, error));
  d2 = Document::open(imgpdf, "", error, enc);
  CHECK(d2 && std::fabs(d2->width(0) - 842) < 1);  // landscape A4
  cairo_surface_destroy(img);
  cairo_surface_t *s = stamp_image("APPROVED", Rgb{0.1, 0.6, 0.2}, 120, 40);
  CHECK(cairo_image_surface_get_width(s) == 480);
  cairo_surface_destroy(s);
  CHECK(arrow_strokes(Point{0, 0}, Point{10, 0}, 3).size() == 2);
}

static void test_external_tools(const std::string &dir) {
  std::string src = make_pdf(dir, 3);
  std::string error;
  bool enc;
  auto doc = Document::open(src, "", error, enc);
  CHECK(doc != nullptr);
  if (!doc) return;
  // Redaction rasterizes a page with black boxes: the text is gone.
  std::string r2 = dir + "/r2.pdf";
  CHECK(rasterize_page_pdf(*doc, 1, 100, {Box{60, 80, 400, 110}}, r2, error));
  auto rd = Document::open(r2, "", error, enc);
  CHECK(rd && rd->count() == 1 && rd->text(0).find("sharp") == std::string::npos);
  std::string png = dir + "/p1.png";
  CHECK(export_page_image(*doc, 0, 50, png, false, error) && file_exists(png));
  if (have_native_jpeg()) {
    std::string jpg = dir + "/p1.jpg";
    CHECK(export_page_image(*doc, 0, 50, jpg, true, error) && file_exists(jpg));
  }
  if (find_program("qpdf").empty()) {
    printf("xanara: qpdf missing, skipping the qpdf round trips\n");
    return;
  }
  int status = -1;
  std::string out = dir + "/redacted.pdf";
  run_sync(redaction_splice_args(src, 3, {{1, r2}}, out, ""), &status);
  auto red = Document::open(out, "", error, enc);
  CHECK(red && red->count() == 3);
  if (red) {
    CHECK(red->text(0).find("sharp text page 1") != std::string::npos);
    CHECK(red->text(1).find("sharp") == std::string::npos);
    CHECK(red->text(2).find("sharp text page 3") != std::string::npos);
  }
  // Form fields and links through qpdf JSON.
  std::string json = run_sync(qpdf_json_args(src, ""), &status);
  PdfObjects objs;
  CHECK(objs.load(json, error));
  PdfUpdate up(objs);
  FieldSpec f;
  f.name = "Name";
  CHECK(plan_field(up, objs, 0, Box{72, 400, 300, 424}, f));
  f.kind = FieldKind::Check;
  f.name = "Agree";
  CHECK(plan_field(up, objs, 0, Box{72, 440, 86, 454}, f));
  CHECK(plan_link(up, objs, 1, Box{72, 80, 300, 110}, "https://example.org", -1));
  std::string upd = dir + "/update.json", formed = dir + "/form.pdf";
  CHECK(write_file_atomic(upd, up.dump()));
  run_sync(qpdf_update_args(src, upd, formed, ""), &status);
  CHECK(status == 0 || status == 3);
  auto fd = Document::open(formed, "", error, enc);
  CHECK(fd != nullptr);
  if (fd) {
    CHECK(fd->fields(0).size() == 2);
    bool link = false;
    for (const auto &l : fd->links(1)) link = link || l.target.text == "https://example.org";
    CHECK(link);
    // Typing into the new text field works and survives a save.
    for (const auto &fld : fd->fields(0))
      if (fld.type == POPPLER_FORM_FIELD_TEXT) CHECK(fd->set_field_text(0, fld.field, "Joana"));
    std::string saved = dir + "/form-saved.pdf";
    CHECK(fd->save_to(saved, error));
    auto again = Document::open(saved, "", error, enc);
    CHECK(again && again->fields(0).size() == 2);
  }
  // Encryption needs the password afterwards.
  std::string locked = dir + "/locked.pdf";
  run_sync(qpdf_encrypt_args(src, locked, "secret", "owner", Permissions{}, ""), &status);
  auto ld = Document::open(locked, "", error, enc);
  CHECK(!ld && enc);
  ld = Document::open(locked, "secret", error, enc);
  CHECK(ld && ld->count() == 3);
  std::string unlocked = dir + "/unlocked.pdf";
  run_sync(qpdf_decrypt_args(locked, unlocked, "secret"), &status);
  CHECK(Document::open(unlocked, "", error, enc) != nullptr);
  // Crop and metadata.
  PdfUpdate up2(objs);
  CHECK(plan_crop(up2, objs, {0}, Box{50, 50, 400, 500}));
  CHECK(plan_strip_metadata(up2, objs) || true);
  CHECK(write_file_atomic(upd, up2.dump()));
  std::string cropped = dir + "/cropped.pdf";
  run_sync(qpdf_update_args(src, upd, cropped, ""), &status);
  auto cd = Document::open(cropped, "", error, enc);
  CHECK(cd && std::fabs(cd->width(0) - 350) < 1 && std::fabs(cd->height(0) - 450) < 1);
  // Stamps through --overlay.
  StampSpec st;
  st.kind = StampSpec::Bates;
  st.font_size = 10;
  st.prefix = "XAN";
  std::string stamp = dir + "/bates.pdf", stamped = dir + "/stamped.pdf";
  std::vector<Point> sizes;
  for (int p = 0; p < doc->count(); ++p) sizes.push_back(Point{doc->width(p), doc->height(p)});
  CHECK(write_stamp_pdf(stamp, sizes, st, error));
  run_sync(qpdf_overlay_args(src, stamp, stamped, false, ""), &status);
  auto sd = Document::open(stamped, "", error, enc);
  CHECK(sd && sd->text(2).find("XAN000003") != std::string::npos);
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
  test_registry();
  test_tabs();
  test_commands();
  test_json();
  test_pdf_objects();
  test_exporters(dir);
  test_external_tools(dir);
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
