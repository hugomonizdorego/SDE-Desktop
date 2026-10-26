// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Xanara core tests: rendering, editing round trips, tile cache LRU,
// page ranges and history. No display is needed.
#include <cairo-pdf.h>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core.h"
#include "edit.h"
#include "layout.h"
#include "model.h"
#include "pdf.h"
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
// The same for calls that explain a failure in |error|.
#define CHECK_OK(call)                                                                              \
  do {                                                                                              \
    error.clear();                                                                                  \
    if (!(call)) {                                                                                  \
      fprintf(stderr, "%s:%d: CHECK failed: %s (%s)\n", __FILE__, __LINE__, #call, error.c_str()); \
      ++failures;                                                                                   \
    }                                                                                               \
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
  // The add_* calls return a reference for the caller (the viewer keeps it
  // on its undo stack); the test gives it back at once.
  auto added = [](PopplerAnnot *a) {
    if (!a) return false;
    g_object_unref(a);
    return true;
  };
  CHECK(added(doc->add_markup(0, MarkupKind::Highlight, boxes, Rgb{1, 0.85, 0.1})));
  CHECK(added(doc->add_markup(0, MarkupKind::Underline, hits, Rgb{0, 0.4, 1})));
  CHECK(added(doc->add_note(0, 400, 90, "Remember this", Rgb{1, 0.8, 0})));
  CHECK(added(doc->add_square(0, Box{300, 400, 450, 480}, Rgb{0, 0.6, 0.2}, 2)));
  CHECK(added(doc->add_line(0, Point{80, 500}, Point{300, 560}, Rgb{0.2, 0.2, 0.9}, 2)));
  CHECK(added(doc->add_ink(1, {{{100, 600}, {150, 620}, {200, 610}}}, Rgb{0.9, 0, 0.3}, 2.5)));
  CHECK(added(doc->add_free_text(1, Box{100, 300, 300, 360}, "Free text box", Rgb{0, 0, 0}, 12)));
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
  cairo_surface_t *probe = tile(64, 64);
  size_t one = TileCache::surface_bytes(probe);
  cairo_surface_destroy(probe);
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
  // Edit mode starts with the tool that edits text and images.
  CHECK(tools_for(Mode::Edit).front()->act == Act::EditContent && tools_for(Mode::Edit).front()->canvas);
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

// ------------------------------------------------- the PDF engine (0.2 Alpha 3)

// The text poppler extracts from |path| (page |page|) and the rectangle of
// each character, in the order of the text.
struct PageText {
  std::string text;
  std::vector<PopplerRectangle> rects;
  bool ok = false;
};

static PageText poppler_text(const std::string &path, int page, const std::string &password = "") {
  PageText out;
  GError *err = nullptr;
  PopplerDocument *doc =
      poppler_document_new_from_file(file_uri(path).c_str(), password.empty() ? nullptr : password.c_str(), &err);
  if (!doc) {
    if (err) g_error_free(err);
    return out;
  }
  PopplerPage *pg = poppler_document_get_page(doc, page);
  if (pg) {
    gchar *t = poppler_page_get_text(pg);
    out.text = t ? t : "";
    g_free(t);
    PopplerRectangle *r = nullptr;
    guint n = 0;
    if (poppler_page_get_text_layout(pg, &r, &n)) {
      out.rects.assign(r, r + n);
      g_free(r);
    }
    out.ok = true;
    g_object_unref(pg);
  }
  g_object_unref(doc);
  return out;
}

// The rectangle of the first character of |word| in |pt| (poppler counts
// characters, not bytes).
static bool char_rect(const PageText &pt, const std::string &word, PopplerRectangle &r) {
  size_t at = pt.text.find(word);
  if (at == std::string::npos) return false;
  size_t chars = utf32(pt.text.substr(0, at)).size();
  if (chars >= pt.rects.size()) return false;
  r = pt.rects[chars];
  return true;
}

static bool same_rect(const PopplerRectangle &a, const PopplerRectangle &b, double tol = 0.05) {
  return std::fabs(a.x1 - b.x1) < tol && std::fabs(a.y1 - b.y1) < tol && std::fabs(a.x2 - b.x2) < tol &&
         std::fabs(a.y2 - b.y2) < tol;
}

// Two blocks of text: a heading and a paragraph of three lines (cairo
// embeds a subset of the system's sans font), and a red square.
static std::string make_report(const std::string &dir, const std::string &name) {
  std::string path = dir + "/" + name;
  cairo_surface_t *s = cairo_pdf_surface_create(path.c_str(), 595, 842);
  cairo_t *cr = cairo_create(s);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 20);
  cairo_set_source_rgb(cr, 0.1, 0.2, 0.4);
  cairo_move_to(cr, 72, 90);
  cairo_show_text(cr, "Quarterly Report");
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgb(cr, 0, 0, 0);
  const char *lines[] = {"The quick brown fox jumps over the lazy dog,", "while seven bright stars watch the hills",
                         "and the river sings softly at night."};
  for (int k = 0; k < 3; ++k) {
    cairo_move_to(cr, 72, 140 + k * 16);
    cairo_show_text(cr, lines[k]);
  }
  cairo_move_to(cr, 72, 400);
  cairo_show_text(cr, "Signed in Lisbon");
  cairo_rectangle(cr, 300, 500, 120, 80);
  cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);
  cairo_fill(cr);
  cairo_show_page(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(s);
  return path;
}

// A page with a standard 14 font that is neither embedded nor has widths,
// the way small generators write PDF.
static std::string make_base14(const std::string &dir) {
  std::string path = dir + "/base14.pdf", error;
  std::unique_ptr<PdfFile> f = PdfFile::create();
  QPDF &q = f->qpdf();
  QPDFObjectHandle font = QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
  QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
  fonts.replaceKey("/F1", q.makeIndirectObject(font));
  res.replaceKey("/Font", fonts);
  QPDFObjectHandle page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 400 220] >>");
  page.replaceKey("/Resources", res);
  page.replaceKey("/Contents", q.newStream("BT /F1 18 Tf 40 150 Td (SDE Suite - Xanara) Tj ET\n"
                                           "BT /F1 12 Tf 40 100 Td (Second line stays) Tj ET\n"));
  QPDFPageDocumentHelper(q).addPage(QPDFPageObjectHelper(q.makeIndirectObject(page)), false);
  CHECK(f->write(path, PdfFile::Write::Rewrite, error));
  return path;
}

static int block_with(const PageLayout &l, const std::string &text) {
  for (size_t b = 0; b < l.blocks.size(); ++b)
    if (l.block_text(int(b)).find(text) != std::string::npos) return int(b);
  return -1;
}

static void test_content_parser() {
  std::string c = "q 1 0 0 1 72 700 cm BT /F1 12 Tf [(Hel) -20 <6C6C6F>] TJ (a\\)b\\101) Tj ET Q\n"
                  "BI /W 2 /H 1 /CS /G /BPC 8 ID \x01\xFF EI 0 g";
  auto ops = parse_content(c);
  CHECK(ops.size() == 10);
  if (ops.size() == 10) {
    CHECK(ops[1].name == "cm" && ops[1].num(4) == 72 && ops[1].num(5) == 700);
    CHECK(ops[3].name == "Tf" && ops[3].args[0].str == "F1");
    CHECK(ops[4].name == "TJ" && ops[4].args[0].items.size() == 3 && ops[4].args[0].items[2].str == "llo" &&
          ops[4].args[0].items[2].hex);
    CHECK(ops[5].name == "Tj" && ops[5].args[0].str == "a)bA");
    CHECK(ops[8].name == "BI" && ops[8].data_end - ops[8].data_begin == 2);
    CHECK(ops[9].name == "g");
    CHECK(c.substr(ops[4].begin, ops[4].end - ops[4].begin) == "[(Hel) -20 <6C6C6F>] TJ");
  }
  CHECK(pdf_num(1.5) == "1.5" && pdf_num(-0.00001) == "0" && pdf_num(12) == "12");
  CHECK(apply_splices("abcdef", {{1, 3, "XY"}, {4, 5, ""}}) == "aXYdf");
  CHECK(pdf_name("A B") == "/A#20B");
  // CMaps and glyph names.
  CMap cm;
  CHECK(cm.parse("1 begincodespacerange <00> <80> endcodespacerange 1 begincodespacerange <8140> <9FFC> "
                 "endcodespacerange 1 begincidrange <8140> <817E> 633 endcidrange"));
  unsigned code = 0;
  CHECK(cm.next("\x81\x41z", 0, code) == 2 && code == 0x8141 && cm.cid(code) == 634);
  CHECK(cm.next("z", 0, code) == 1 && code == 'z');
  UnicodeMap um;
  CHECK(um.parse("2 beginbfchar <0003> <0020> <0011> <00660069> endbfchar 1 beginbfrange <0024> <0026> <0041> "
                 "endbfrange"));
  std::string t;
  CHECK(um.lookup(0x11, t) && t == "fi");
  CHECK(um.lookup(0x25, t) && t == "B");
  CHECK(glyph_unicode("Aacute") == 0xC1 && glyph_unicode("uni20AC") == 0x20AC && glyph_unicode("u1F600") == 0x1F600);
  CHECK(glyph_unicode("a.sc") == 'a' && glyph_unicode("nonsense-name") == 0);
}

static void test_layout_and_edit(const std::string &dir) {
  std::string src = make_report(dir, "report.pdf"), error;
  PageText before = poppler_text(src, 0);
  CHECK(before.ok && before.text.find("quick brown fox") != std::string::npos);
  std::unique_ptr<PdfFile> pdf = PdfFile::open(src, "", error);
  CHECK(pdf != nullptr);
  if (!pdf) return;
  CHECK(pdf->count() == 1 && pdf->can_append() && !pdf->repaired());
  std::shared_ptr<PageLayout> layout(PageLayout::read(*pdf, 0).release());
  CHECK(layout->complete);
  // Heading, paragraph and the signature line are three blocks.
  int heading = block_with(*layout, "Quarterly Report"), para = block_with(*layout, "brown fox");
  int sig = block_with(*layout, "Signed in Lisbon");
  CHECK(heading >= 0 && para >= 0 && sig >= 0 && heading != para && para != sig);
  if (para < 0) return;
  CHECK(layout->blocks[size_t(para)].lines.size() == 3);
  CHECK(layout->block_text(para).find("the hills\nand the river") != std::string::npos);
  CHECK(layout->blocks[size_t(para)].editable);
  // The page space matches poppler's: the 'T' of "The" starts at 72 pt.
  const Glyph &t0 = layout->glyphs[size_t(layout->lines[size_t(layout->blocks[size_t(para)].lines[0])].glyphs[0])];
  CHECK(std::fabs(t0.origin.x - 72) < 0.01 && std::fabs(t0.origin.y - 140) < 0.01);

  // Replace "brown" with "red" (letters the embedded subset has).
  TextEdit ed;
  CHECK(ed.start(layout, para));
  std::string text = ed.text();
  size_t at = utf32(text.substr(0, text.find("brown"))).size();
  ed.set_caret(at, false);
  ed.set_caret(at + 5, true);
  CHECK(ed.selected_text() == "brown");
  ed.insert("red");
  CHECK(ed.modified() && ed.text().find("quick red fox") != std::string::npos);
  auto lines = ed.lines();
  CHECK(lines.size() == 3);
  // The edited line is set again; the two below keep their glyphs.
  if (lines.size() == 3) CHECK(!lines[0].kept && lines[1].kept && lines[2].kept);
  CHECK(ed.undo() && ed.text().find("quick brown fox") != std::string::npos);
  CHECK(ed.redo() && ed.text().find("quick red fox") != std::string::npos);
  CHECK_OK(commit_text(*pdf, *layout, ed, error));
  std::string out = dir + "/report-edited.pdf";
  CHECK_OK(pdf->write(out, PdfFile::Write::Auto, error));
  // An incremental update: the original bytes come first, unchanged.
  std::string a, b;
  CHECK(read_file(src, a) && read_file(out, b) && b.size() > a.size() && b.compare(0, a.size(), a) == 0);
  PageText after = poppler_text(out, 0);
  CHECK(after.text.find("The quick red fox jumps over the lazy dog,") != std::string::npos);
  CHECK(after.text.find("brown") == std::string::npos);
  // Everything else is where it was.
  for (const char *w : {"Quarterly", "The quick", "while seven", "and the river", "Signed in"}) {
    PopplerRectangle r1, r2;
    CHECK(char_rect(before, w, r1) && char_rect(after, w, r2));
    CHECK(same_rect(r1, r2));
  }

  // A word with letters the subset lacks: an installed font is embedded
  // (Type0, Identity-H, ToUnicode), so the text is shown and extractable.
  pdf = PdfFile::open(out, "", error);
  CHECK(pdf != nullptr);
  if (!pdf) return;
  layout.reset(PageLayout::read(*pdf, 0).release());
  sig = block_with(*layout, "Signed in Lisbon");
  CHECK(sig >= 0);
  if (sig < 0) return;
  TextEdit ed2;
  CHECK(ed2.start(layout, sig));
  ed2.move_end(false, true);
  ed2.insert(" — Quintão & Żółw");
  CHECK_OK(commit_text(*pdf, *layout, ed2, error));
  std::string out2 = dir + "/report-edited2.pdf";
  CHECK_OK(pdf->write(out2, PdfFile::Write::Auto, error));
  PageText third = poppler_text(out2, 0);
  CHECK(third.text.find("Signed in Lisbon — Quintão & Żółw") != std::string::npos);
  CHECK(third.text.find("quick red fox") != std::string::npos);
  {
    // Every font of the page is embedded.
    GError *err = nullptr;
    PopplerDocument *doc = poppler_document_new_from_file(file_uri(out2).c_str(), nullptr, &err);
    CHECK(doc != nullptr);
    if (doc) {
      PopplerFontInfo *fi = poppler_font_info_new(doc);
      PopplerFontsIter *it = nullptr;
      int fonts = 0, embedded = 0;
      if (poppler_font_info_scan(fi, 1, &it) && it) {
        do {
          ++fonts;
          if (poppler_fonts_iter_is_embedded(it)) ++embedded;
        } while (poppler_fonts_iter_next(it));
        poppler_fonts_iter_free(it);
      }
      CHECK(fonts >= 3 && fonts == embedded);
      poppler_font_info_free(fi);
      g_object_unref(doc);
    }
  }
  // The edited text can be edited again (its own fonts are read back).
  pdf = PdfFile::open(out2, "", error);
  layout.reset(PageLayout::read(*pdf, 0).release());
  sig = block_with(*layout, "Quintão");
  CHECK(sig >= 0 && layout->block_text(sig).find("Signed in Lisbon — Quintão & Żółw") != std::string::npos);
}

static void test_new_text_and_styles(const std::string &dir) {
  std::string src = make_base14(dir), error;
  std::unique_ptr<PdfFile> pdf = PdfFile::open(src, "", error);
  CHECK(pdf != nullptr);
  if (!pdf) return;
  std::shared_ptr<PageLayout> layout(PageLayout::read(*pdf, 0).release());
  int b = block_with(*layout, "SDE Suite - Xanara");
  CHECK(b >= 0);
  if (b < 0) return;
  // Helvetica without /Widths: the standard metrics.
  const Glyph &s = layout->glyphs[size_t(layout->lines[size_t(layout->blocks[size_t(b)].lines[0])].glyphs[0])];
  CHECK(std::fabs(s.width - 0.667) < 0.001);
  TextEdit ed;
  CHECK(ed.start(layout, b));
  ed.move_end(false, true);
  ed.insert(" Editor");
  CHECK(ed.text() == "SDE Suite - Xanara Editor");
  // Bold for the new word: an installed bold font takes over.
  ed.set_caret(ed.size() - 6, false);
  ed.set_caret(ed.size(), true);
  ed.restyle([](TextStyle &st) { st.bold = true; });
  CHECK(ed.style_at_caret().bold && ed.style_at_caret().font < 0);
  CHECK_OK(commit_text(*pdf, *layout, ed, error));
  // A new text box elsewhere.
  TextEdit nb;
  TextStyle st;
  st.family = "Serif";
  st.size = 14;
  st.paint.n = 3;
  st.paint.v[0] = 0.1;
  st.paint.v[1] = 0.3;
  st.paint.v[2] = 0.8;
  nb.start_new(layout, Point{40, 150}, 0, st);
  nb.insert("Fresh line one");
  nb.new_paragraph();
  nb.insert("and two");
  CHECK(nb.lines().size() == 2);
  CHECK_OK(commit_text(*pdf, *layout, nb, error));
  std::string out = dir + "/base14-edited.pdf";
  CHECK_OK(pdf->write(out, PdfFile::Write::Auto, error));
  PageText pt = poppler_text(out, 0);
  CHECK(pt.text.find("SDE Suite - Xanara Editor") != std::string::npos);
  CHECK(pt.text.find("Fresh line one") != std::string::npos && pt.text.find("and two") != std::string::npos);
  CHECK(pt.text.find("Second line stays") != std::string::npos);
  PageText orig = poppler_text(src, 0);
  PopplerRectangle r1, r2;
  CHECK(char_rect(orig, "Second", r1) && char_rect(pt, "Second", r2) && same_rect(r1, r2));
  CHECK(char_rect(orig, "SDE", r1) && char_rect(pt, "SDE", r2) && same_rect(r1, r2, 0.1));
}

static void test_wrapping(const std::string &dir) {
  std::string src = make_report(dir, "wrap.pdf"), error;
  std::unique_ptr<PdfFile> pdf = PdfFile::open(src, "", error);
  std::shared_ptr<PageLayout> layout(PageLayout::read(*pdf, 0).release());
  int para = block_with(*layout, "brown fox");
  TextEdit ed;
  CHECK(para >= 0 && ed.start(layout, para));
  if (para < 0) return;
  // A long insertion in the first line flows into the next ones, which
  // are set again; nothing leaves the box on the right.
  Box frame = ed.frame();
  ed.set_caret(4, false);
  ed.insert("very very very very very very ");
  auto lines = ed.lines();
  CHECK(lines.size() >= 4);
  for (const auto &l : lines) CHECK(l.x2 <= frame.x2 + 0.5);
  CHECK(!lines[0].kept && !lines[1].kept);
  // Enter makes a new paragraph; Backspace joins it again.
  TextEdit e2;
  CHECK(e2.start(layout, para));
  std::string original = e2.text();
  e2.set_caret(10, false);  // after "The quick "
  e2.new_paragraph();
  CHECK(e2.lines().size() == 4 && e2.lines()[1].text.rfind("brown fox", 0) == 0);
  e2.erase(false, false);
  CHECK(e2.text() == original);
  CHECK(e2.lines().size() == 3 && e2.lines()[0].kept && e2.lines()[2].kept);
  // Word movement and selection.
  e2.set_caret(0, false);
  e2.move_right(true, true);
  CHECK(e2.selected_text() == "The ");
  e2.select_word(6);
  CHECK(e2.selected_text() == "quick");
  // Alignment: centered lines inside the box.
  e2.set_align(1);
  auto cl = e2.lines();
  CHECK(!cl.empty() && !cl[0].kept);
  if (cl.size() == 3) CHECK(std::fabs((cl[1].x1 + cl[1].x2) / 2 - (frame.x1 + frame.x2) / 2) < 1.0);
}

static void test_files(const std::string &dir) {
  std::string a = make_report(dir, "a.pdf"), b = make_pdf(dir, 3), error;
  // Merge and split.
  std::string merged = dir + "/merged.pdf";
  CHECK_OK(merge_files({a, b}, merged, error));
  std::unique_ptr<PdfFile> m = PdfFile::open(merged, "", error);
  CHECK(m && m->count() == 4);
  if (!m) return;
  std::vector<std::string> parts;
  CHECK_OK(split_pages(*m, 3, dir + "/part-%d.pdf", parts, error));
  CHECK(parts.size() == 2);
  if (parts.size() == 2) {
    CHECK(PdfFile::open(parts[0], "", error)->count() == 3);
    CHECK(PdfFile::open(parts[1], "", error)->count() == 1);
  }
  // Pages: rotate, reorder (delete page 2), insert, extract.
  CHECK(rotate_pages(*m, {0}, 90));
  CHECK_OK(reorder_pages(*m, {3, 0, 2}, error));
  CHECK_OK(insert_pages(*m, 0, {a}, error));
  std::string pages = dir + "/pages.pdf";
  CHECK_OK(m->write(pages, PdfFile::Write::Auto, error));
  std::unique_ptr<PdfFile> p = PdfFile::open(pages, "", error);
  CHECK(p && p->count() == 4 && p->geometry(2).rotate == 90);
  CHECK(poppler_text(pages, 0).text.find("page 3") != std::string::npos);
  CHECK(poppler_text(pages, 1).text.find("Quarterly") != std::string::npos);
  std::string ex = dir + "/extract.pdf";
  CHECK_OK(extract_pages(*p, {1, 3}, ex, error));
  CHECK(PdfFile::open(ex, "", error)->count() == 2);
  // Encryption round trip; an encrypted file is edited and stays encrypted.
  std::string locked = dir + "/locked.pdf";
  Permissions perms;
  perms.print = false;
  CHECK_OK(write_protected(*PdfFile::open(a, "", error), locked, "secret", "owner", perms, error));
  bool enc = false;
  CHECK(PdfFile::open(locked, "", error, &enc) == nullptr && enc);
  CHECK(!poppler_text(locked, 0).ok);
  std::unique_ptr<PdfFile> l = PdfFile::open(locked, "secret", error);
  CHECK(l && l->encrypted() && !l->can_append());
  if (l) {
    std::shared_ptr<PageLayout> layout(PageLayout::read(*l, 0).release());
    int sig = block_with(*layout, "Signed in Lisbon");
    TextEdit ed;
    CHECK(sig >= 0 && ed.start(layout, sig));
    ed.move_end(false, true);
    ed.insert(" today");
    CHECK_OK(commit_text(*l, *layout, ed, error));
    std::string locked2 = dir + "/locked2.pdf";
    CHECK_OK(l->write(locked2, PdfFile::Write::Auto, error));
    CHECK(!poppler_text(locked2, 0).ok);
    CHECK(poppler_text(locked2, 0, "secret").text.find("Signed in Lisbon today") != std::string::npos);
    std::string open = dir + "/unlocked.pdf";
    std::unique_ptr<PdfFile> l2 = PdfFile::open(locked2, "secret", error);
    CHECK(l2 && write_unprotected(*l2, open, error));
    CHECK(poppler_text(open, 0).text.find("today") != std::string::npos);
  }
  // Object streams and a cross-reference stream: the update follows suit.
  std::string packed = dir + "/packed.pdf";
  {
    std::unique_ptr<PdfFile> f = PdfFile::open(a, "", error);
    CHECK(f && f->write(packed, PdfFile::Write::Compact, error));
  }
  std::unique_ptr<PdfFile> pk = PdfFile::open(packed, "", error);
  CHECK(pk && pk->can_append());
  if (pk) {
    std::shared_ptr<PageLayout> layout(PageLayout::read(*pk, 0).release());
    int h = block_with(*layout, "Quarterly Report");
    TextEdit ed;
    CHECK(h >= 0 && ed.start(layout, h));
    ed.select_all();
    ed.insert("Annual Report");
    CHECK_OK(commit_text(*pk, *layout, ed, error));
    std::string out = dir + "/packed-edited.pdf";
    CHECK_OK(pk->write(out, PdfFile::Write::Auto, error));
    std::string bytes, before_bytes;
    CHECK(read_file(out, bytes) && read_file(packed, before_bytes));
    CHECK(bytes.compare(0, before_bytes.size(), before_bytes) == 0);
    CHECK(bytes.rfind("/Type /XRef") > before_bytes.size() && bytes.rfind("/Type /XRef") != std::string::npos);
    CHECK(poppler_text(out, 0).text.find("Annual Report") != std::string::npos);
  }
  // A damaged cross-reference table is repaired and written completely.
  std::string data;
  CHECK(read_file(a, data));
  size_t sx = data.rfind("startxref");
  if (sx != std::string::npos) {
    std::string broken = data.substr(0, sx) + "startxref\n123\n%%EOF\n";
    std::string bpath = dir + "/broken.pdf";
    CHECK(write_file_atomic(bpath, broken));
    std::unique_ptr<PdfFile> bf = PdfFile::open(bpath, "", error);
    CHECK(bf && bf->repaired() && !bf->can_append());
    if (bf) {
      CHECK(rotate_pages(*bf, {0}, 180));
      std::string fixed = dir + "/fixed.pdf";
      CHECK_OK(bf->write(fixed, PdfFile::Write::Auto, error));
      std::unique_ptr<PdfFile> ff = PdfFile::open(fixed, "", error);
      CHECK(ff && !ff->repaired() && ff->geometry(0).rotate == 180);
    }
  }
}

static void test_objects(const std::string &dir) {
  std::string src = make_pdf(dir, 2), error;
  std::unique_ptr<PdfFile> f = PdfFile::open(src, "", error);
  CHECK(f != nullptr);
  if (!f) return;
  // Links, form fields, crop box, metadata, stamps.
  CHECK(add_link(*f, 0, Box{72, 80, 300, 110}, "https://example.org", -1));
  CHECK(add_link(*f, 1, Box{72, 80, 300, 110}, "", 0));
  CHECK(!add_link(*f, 0, Box{0, 0, 10, 10}, "", 7));
  FieldSpec fs;
  fs.name = "Name";
  CHECK(add_field(*f, 0, Box{72, 400, 300, 424}, fs));
  fs.kind = FieldKind::Check;
  fs.name = "Agree";
  CHECK(add_field(*f, 0, Box{72, 440, 86, 454}, fs));
  fs.kind = FieldKind::Combo;
  fs.name = "Country";
  fs.options = {"Portugal", "Indonésia"};
  CHECK(add_field(*f, 1, Box{72, 400, 300, 424}, fs));
  FieldSpec bad;
  CHECK(!add_field(*f, 0, Box{0, 0, 50, 50}, bad));
  CHECK(crop_pages(*f, {1}, Box{50, 50, 400, 500}));
  std::string out = dir + "/objects.pdf";
  CHECK_OK(f->write(out, PdfFile::Write::Auto, error));
  bool enc;
  auto d = Document::open(out, "", error, enc);
  CHECK(d && d->count() == 2);
  if (d) {
    CHECK(d->fields(0).size() == 2 && d->fields(1).size() == 1);
    bool uri = false, page = false;
    for (const auto &l : d->links(0)) uri = uri || l.target.text == "https://example.org";
    for (const auto &l : d->links(1)) page = page || (l.target.kind == LinkTarget::Page && l.target.page == 0);
    CHECK(uri && page);
    CHECK(std::fabs(d->width(1) - 350) < 1 && std::fabs(d->height(1) - 450) < 1);
    for (const auto &fld : d->fields(0))
      if (fld.type == POPPLER_FORM_FIELD_TEXT) CHECK(d->set_field_text(0, fld.field, "Joana"));
    std::string saved = dir + "/objects-saved.pdf";
    CHECK(d->save_to(saved, error));
  }
  // Metadata goes, and the file is written completely.
  std::unique_ptr<PdfFile> g = PdfFile::open(out, "", error);
  g->qpdf().getTrailer().replaceKey("/Info", g->add(QPDFObjectHandle::parse("<< /Author (Somebody) >>")));
  std::string with_info = dir + "/info.pdf";
  CHECK_OK(g->write(with_info, PdfFile::Write::Rewrite, error));
  g = PdfFile::open(with_info, "", error);
  CHECK(strip_metadata(*g) && !g->can_append());
  std::string stripped = dir + "/stripped.pdf";
  CHECK_OK(g->write(stripped, PdfFile::Write::Auto, error));
  std::string bytes;
  CHECK(read_file(stripped, bytes) && bytes.find("Somebody") == std::string::npos);
  // Stamps over every page.
  StampSpec st;
  st.kind = StampSpec::Bates;
  st.font_size = 10;
  st.prefix = "XAN";
  std::string stamp = dir + "/bates.pdf";
  CHECK(write_stamp_pdf(stamp, {Point{595, 842}, Point{595, 842}}, st, error));
  std::unique_ptr<PdfFile> h = PdfFile::open(src, "", error);
  CHECK_OK(overlay_pages(*h, stamp, false, error));
  std::string stamped = dir + "/stamped.pdf";
  CHECK_OK(h->write(stamped, PdfFile::Write::Auto, error));
  CHECK(poppler_text(stamped, 1).text.find("XAN000002") != std::string::npos);
  CHECK(poppler_text(stamped, 1).text.find("page 2") != std::string::npos);
  // Compression keeps the pages and their text.
  std::string small = dir + "/small.pdf";
  std::unique_ptr<PdfFile> c = PdfFile::open(stamped, "", error);
  CHECK_OK(write_compressed(*c, small, 150, error));
  CHECK(poppler_text(small, 0).text.find("XAN000001") != std::string::npos);
}

static void test_images_and_redaction(const std::string &dir) {
  std::string src = make_report(dir, "img.pdf"), error;
  // A PNG with transparency.
  std::string png = dir + "/dot.png";
  {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 32);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgba(cr, 0.1, 0.6, 0.2, 1);
    cairo_rectangle(cr, 0, 0, 40, 32);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_write_to_png(s, png.c_str());
    cairo_surface_destroy(s);
  }
  std::unique_ptr<PdfFile> f = PdfFile::open(src, "", error);
  int w = 0, h = 0;
  QPDFObjectHandle x = image_xobject(*f, png, w, h, error);
  CHECK(x.isStream() && w == 64 && h == 32 && x.getDict().getKey("/SMask").isStream());
  CHECK_OK(add_image(*f, 0, x, Box{100, 600, 228, 664}, error));
  std::string out = dir + "/img1.pdf";
  CHECK_OK(f->write(out, PdfFile::Write::Auto, error));
  f = PdfFile::open(out, "", error);
  std::unique_ptr<PageLayout> l = PageLayout::read(*f, 0);
  CHECK(l->images.size() == 1);
  if (l->images.size() == 1) {
    const Box &b = l->images[0].box;
    CHECK(std::fabs(b.x1 - 100) < 0.01 && std::fabs(b.y1 - 600) < 0.01 && std::fabs(b.x2 - 228) < 0.01);
    CHECK(l->image_at(150, 630) == 0);
    CHECK_OK(move_image(*f, *l, 0, Box{300, 650, 364, 682}, error));
    std::string out2 = dir + "/img2.pdf";
    CHECK_OK(f->write(out2, PdfFile::Write::Auto, error));
    f = PdfFile::open(out2, "", error);
    l = PageLayout::read(*f, 0);
    CHECK(l->images.size() == 1 && std::fabs(l->images[0].box.x1 - 300) < 0.01 &&
          std::fabs(l->images[0].box.y2 - 682) < 0.01);
    CHECK_OK(delete_image(*f, *l, 0, error));
    std::string out3 = dir + "/img3.pdf";
    CHECK_OK(f->write(out3, PdfFile::Write::Auto, error));
    f = PdfFile::open(out3, "", error);
    CHECK(PageLayout::read(*f, 0)->images.empty());
  }
  // Redaction removes the words under the box, keeps the rest in place.
  f = PdfFile::open(src, "", error);
  l = PageLayout::read(*f, 0);
  Box target;
  bool found = false;
  for (const Glyph &g : l->glyphs)
    if (g.text == "b" && l->block_text(l->lines[size_t(g.line)].block).find("brown") != std::string::npos && !found) {
      // "brown": five glyphs from this "b".
      size_t i = size_t(&g - &l->glyphs[0]);
      target = Box{g.box.x1, g.box.y1, l->glyphs[i + 4].box.x2, g.box.y2};
      found = true;
    }
  CHECK(found);
  CHECK_OK(redact_page(*f, 0, {target}, error));
  CHECK(!f->can_append());
  std::string red = dir + "/redacted.pdf";
  CHECK_OK(f->write(red, PdfFile::Write::Auto, error));
  PageText before = poppler_text(src, 0), after = poppler_text(red, 0);
  CHECK(after.text.find("brown") == std::string::npos);
  CHECK(after.text.find("quick") != std::string::npos && after.text.find("fox jumps") != std::string::npos);
  PopplerRectangle r1, r2;
  CHECK(char_rect(before, "fox", r1) && char_rect(after, "fox", r2) && same_rect(r1, r2));
  // Not in the file either (the content is read back by the engine).
  std::unique_ptr<PdfFile> rf = PdfFile::open(red, "", error);
  std::unique_ptr<PageLayout> rl = PageLayout::read(*rf, 0);
  std::string all;
  for (const Glyph &g : rl->glyphs) all += g.text;
  CHECK(all.find("brown") == std::string::npos && all.find("quick") != std::string::npos);
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
  test_exporters(dir);
  test_content_parser();
  test_layout_and_edit(dir);
  test_new_text_and_styles(dir);
  test_wrapping(dir);
  test_files(dir);
  test_objects(dir);
  test_images_and_redaction(dir);
  std::string cmd = std::string("rm -rf '") + dir + "'";
  if (getenv("XANARA_TEST_KEEP")) printf("xanara: test files kept in %s\n", dir);
  else if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", dir);
  printf("xanara: free text %s, ink %s\n", native_free_text() ? "native" : "stamp",
         native_ink() ? "native" : "stamp");
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("all xanara tests passed\n");
  return 0;
}
