// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for Photo's collages (no display): the ten designs adapt to
// any number of photos, cells stay on the picture and do not overlap where
// a design tiles, panning and zooming inside a cell, hit tests, rendering
// and undo.
#include <cairo.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "collage.h"

using namespace photo;

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

static Collage make(CollageDesign d, int n, int w = 1600, int h = 1000) {
  Collage c;
  for (int i = 0; i < n; ++i) {
    CollageItem it;
    it.path = "/photos/" + std::to_string(i) + ".jpg";
    // A mix of landscape, portrait and square photos.
    it.w = i % 3 == 0 ? 4000 : i % 3 == 1 ? 3000 : 2000;
    it.h = i % 3 == 0 ? 3000 : i % 3 == 1 ? 4000 : 2000;
    c.items.push_back(it);
  }
  set_design(c, d);
  set_size(c, w, h);
  c.suggested_title = "Holidays 2026";
  return c;
}

static double overlap(const CollageCell &a, const CollageCell &b) {
  double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
  double x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
  return x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0;
}

static void test_catalog() {
  CHECK(collage_designs().size() == size_t(kCollageDesigns));
  std::set<std::string> keys;
  for (const auto &d : collage_designs()) {
    keys.insert(d.key);
    CollageDesign back;
    CHECK(parse_design(d.key, back) && back == d.id);
    CHECK(d.width >= kMinCollageSide && d.height >= kMinCollageSide);
    CHECK(design_info(d.id).id == d.id);
  }
  CHECK(keys.size() == size_t(kCollageDesigns));
  CollageDesign none;
  CHECK(!parse_design("nope", none));
  CHECK(size_preset(1080, 1080) == 0);
  CHECK(size_preset(1080, 1920) >= 0);
  CHECK(size_preset(3840, 2160) >= 0);
  CHECK(size_preset(2480, 3508) >= 0 && size_preset(3508, 2480) >= 0);
  CHECK(size_preset(1234, 777) == -1);
  // The size and the background follow the design until chosen.
  Collage c;
  set_design(c, CollageDesign::Story);
  CHECK(c.width == 1080 && c.height == 1920);
  set_design(c, CollageDesign::Panorama);
  CHECK(c.width == 3240 && c.height == 1080);
  CHECK(c.background == design_info(CollageDesign::Panorama).background);
  set_size(c, 50, 99999);
  CHECK(c.width == kMinCollageSide && c.height == kMaxCollageSide && !c.auto_size);
  set_design(c, CollageDesign::Grid);
  CHECK(c.width == kMinCollageSide);
  c.auto_background = false;
  c.background = 0xFF123456;
  set_design(c, CollageDesign::Magazine);
  CHECK(c.background == 0xFF123456);
}

static void test_every_design_adapts() {
  for (const auto &d : collage_designs()) {
    for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 10, 13, 20, 33, 60}) {
      for (auto size : {std::pair<int, int>{1600, 1000}, {1000, 1600}, {1080, 1080}}) {
        Collage c = make(d.id, n, size.first, size.second);
        if (d.id == CollageDesign::Shapes && n % 2) c.shape = CollageShape::Circles;
        CollageLayout l = layout_collage(c);
        // Every photo is shown exactly once.
        std::set<int> seen;
        for (const auto &cell : l.cells) {
          CHECK(cell.item >= 0 && cell.item < n);
          seen.insert(cell.item);
          CHECK(cell.w > 0.5 && cell.h > 0.5);
          // On the picture (rotated prints may reach out a little).
          double slack = cell.angle != 0 ? std::max(cell.w, cell.h) * 0.2 : 0.5;
          CHECK(cell.x >= -slack && cell.y >= -slack);
          CHECK(cell.x + cell.w <= c.width + slack && cell.y + cell.h <= c.height + slack);
        }
        if (int(seen.size()) != n || l.cells.size() != size_t(n))
          fprintf(stderr, "design %s, %d photos: %zu cells\n", d.key, n, l.cells.size());
        CHECK(int(seen.size()) == n);
        CHECK(l.cells.size() == size_t(n));
        // Tiling designs do not overlap their cells.
        bool tiles = d.id == CollageDesign::Grid || d.id == CollageDesign::Mosaic || d.id == CollageDesign::Featured ||
                     d.id == CollageDesign::Panorama || d.id == CollageDesign::Framed ||
                     d.id == CollageDesign::Filmstrip || d.id == CollageDesign::Shapes;
        if (tiles) {
          for (size_t i = 0; i < l.cells.size(); ++i)
            for (size_t j = i + 1; j < l.cells.size(); ++j) {
              const CollageCell &a = l.cells[i], &b = l.cells[j];
              // Circles in a honeycomb overlap only as squares.
              double o = a.clip == CellClip::Circle
                             ? std::max(0.0, (a.w + b.w) / 2 - std::hypot(a.cx() - b.cx(), a.cy() - b.cy()))
                             : overlap(a, b);
              if (o > 1) fprintf(stderr, "design %s, %d photos: cells %zu and %zu overlap\n", d.key, n, i, j);
              CHECK(o <= 1);
            }
        }
      }
    }
  }
}

static void test_coverage() {
  // Grid and mosaic fill most of the picture.
  for (CollageDesign d : {CollageDesign::Grid, CollageDesign::Mosaic, CollageDesign::Panorama}) {
    for (int n : {2, 5, 9, 24}) {
      Collage c = make(d, n);
      CollageLayout l = layout_collage(c);
      double area = 0;
      for (const auto &cell : l.cells) area += cell.w * cell.h;
      CHECK(area > 0.75 * c.width * c.height);
    }
  }
  // Featured: the first photo is the largest.
  Collage f = make(CollageDesign::Featured, 7);
  CollageLayout fl = layout_collage(f);
  for (size_t i = 1; i < fl.cells.size(); ++i) CHECK(fl.cells[0].w * fl.cells[0].h > fl.cells[i].w * fl.cells[i].h);
  // Magazine: the first photo is the cover, the title is on it.
  Collage m = make(CollageDesign::Magazine, 6);
  CollageLayout ml = layout_collage(m);
  CHECK(ml.cover);
  CHECK(ml.cells[0].w == m.width && ml.cells[0].h == m.height);
  CHECK(!ml.texts.empty() && ml.texts[0].text == "Holidays 2026");
  // Mosaic rows keep the photos' shapes roughly.
  Collage mo = make(CollageDesign::Mosaic, 12);
  for (const auto &cell : layout_collage(mo).cells) {
    double a = mo.items[size_t(cell.item)].aspect();
    CHECK(std::fabs(std::log(cell.w / cell.h / a)) < 0.6);
  }
  // Heart cells lie mostly inside the heart, and they cover it.
  for (int n : {5, 8, 24}) {
    Collage h = make(CollageDesign::Shapes, n, 1200, 1200);
    CollageLayout hl = layout_collage(h);
    CHECK(!hl.cells.empty());
    if (hl.cells.empty()) continue;
    auto poly = heart_polygon(hl.cells[0].hx, hl.cells[0].hy, hl.cells[0].hw, hl.cells[0].hh);
    for (const auto &cell : hl.cells) {
      CHECK(cell.clip == CellClip::Heart);
      int in = 0;
      for (int sy = 0; sy < 5; ++sy)
        for (int sx = 0; sx < 5; ++sx)
          in += inside_polygon(poly, cell.x + (sx + 0.5) * cell.w / 5, cell.y + (sy + 0.5) * cell.h / 5);
      CHECK(in >= 5);
    }
    // The tip and the lobes are part of some cell.
    const CollageCell &c0 = hl.cells[0];
    for (auto pt : {std::pair<double, double>{c0.hx + c0.hw / 2, c0.hy + c0.hh * 0.97},
                    {c0.hx + c0.hw * 0.25, c0.hy + c0.hh * 0.12}, {c0.hx + c0.hw * 0.75, c0.hy + c0.hh * 0.12}}) {
      // Up to the spacing between the cells.
      double tol = std::min(h.width, h.height) * h.spacing / 100 + 1;
      bool covered = false;
      for (const auto &cell : hl.cells)
        covered |= pt.first >= cell.x - tol && pt.first <= cell.x + cell.w + tol && pt.second >= cell.y - tol &&
                   pt.second <= cell.y + cell.h + tol;
      if (!covered) {
        fprintf(stderr, "heart, %d photos: (%.0f, %.0f) not covered; cells:", n, pt.first, pt.second);
        for (const auto &cell : hl.cells) fprintf(stderr, " [%.0f %.0f %.0f %.0f]", cell.x, cell.y, cell.w, cell.h);
        fprintf(stderr, "\n");
      }
      CHECK(covered);
    }
  }
  // A title of the user's adds a caption to a grid; none without one.
  Collage g = make(CollageDesign::Grid, 4);
  CHECK(layout_collage(g).texts.empty());
  g.title = "Summer";
  CHECK(layout_collage(g).texts.size() == 1);
  g.show_title = false;
  CHECK(layout_collage(g).texts.empty());
  // Framed and story show the suggested title.
  CHECK(!layout_collage(make(CollageDesign::Framed, 3)).texts.empty());
  CHECK(!layout_collage(make(CollageDesign::Story, 3)).texts.empty());
  // Empty collages have no cells.
  Collage e;
  CHECK(layout_collage(e).cells.empty());
}

static void test_pan_zoom() {
  CollageItem it;
  it.w = 4000;
  it.h = 2000;  // 2:1 in a square cell: only horizontal travel
  double sx, sy, sw, sh;
  cell_source(it, 400, 200, 100, 100, sx, sy, sw, sh);
  CHECK(std::fabs(sw - 200) < 1e-9 && std::fabs(sh - 200) < 1e-9);
  CHECK(std::fabs(sx - 100) < 1e-9 && sy == 0);
  it.pan_x = -1;
  cell_source(it, 400, 200, 100, 100, sx, sy, sw, sh);
  CHECK(sx == 0);
  it.pan_x = 1;
  cell_source(it, 400, 200, 100, 100, sx, sy, sw, sh);
  CHECK(std::fabs(sx - 200) < 1e-9);
  // Dragging right shows more of the left part.
  it.pan_x = 0;
  pan_item(it, 100, 100, 50, 0);
  CHECK(it.pan_x < 0);
  pan_item(it, 100, 100, 1e6, 1e6);
  CHECK(it.pan_x == -1 && it.pan_y == 0);  // no vertical travel at zoom 1
  // Zooming keeps the point under the pointer.
  CollageItem z;
  z.w = 3000;
  z.h = 2000;
  const int iw = 3000, ih = 2000;
  double cw = 300, ch = 200;
  cell_source(z, iw, ih, cw, ch, sx, sy, sw, sh);
  double ux = sx + 0.25 * sw, uy = sy + 0.75 * sh;
  zoom_item(z, cw, ch, 2.0, 0.25, 0.75);
  CHECK(std::fabs(z.zoom - 2) < 1e-9);
  cell_source(z, iw, ih, cw, ch, sx, sy, sw, sh);
  CHECK(std::fabs(sx + 0.25 * sw - ux) < 2 && std::fabs(sy + 0.75 * sh - uy) < 2);
  zoom_item(z, cw, ch, 100, 0.5, 0.5);
  CHECK(z.zoom == 5);
  zoom_item(z, cw, ch, 0.001, 0.5, 0.5);
  CHECK(z.zoom == 1);
}

static void test_hit_and_order() {
  Collage c = make(CollageDesign::Grid, 6);
  CollageLayout l = layout_collage(c);
  for (size_t i = 0; i < l.cells.size(); ++i) CHECK(cell_at(l, l.cells[i].cx(), l.cells[i].cy()) == int(i));
  CHECK(cell_at(l, -10, -10) == -1);
  CHECK(cell_of_item(l, 3) >= 0 && l.cells[size_t(cell_of_item(l, 3))].item == 3);
  // Rotated prints are hit inside their rotated shape.
  Collage p = make(CollageDesign::Polaroid, 3);
  CollageLayout pl = layout_collage(p);
  for (size_t i = 0; i < pl.cells.size(); ++i) {
    int hit = cell_at(pl, pl.cells[i].cx(), pl.cells[i].cy());
    CHECK(hit >= int(i));  // a later print may lie on top
  }
  // Reordering keeps the blurred background on the same photo.
  c.blur_background = 1;
  std::string path1 = c.items[1].path;
  move_item(c, 1, 4);
  CHECK(c.items[4].path == path1 && c.blur_background == 4);
  move_item(c, 0, 5);
  CHECK(c.blur_background == 3 && c.items[3].path == path1);
  swap_items(c, 3, 0);
  CHECK(c.blur_background == 0 && c.items[0].path == path1);
  remove_item(c, 2);
  CHECK(c.blur_background == 0 && c.items.size() == 5);
  remove_item(c, 0);
  CHECK(c.blur_background == -1);
  // Sharpness: more pixels for larger previews and zoomed photos.
  Collage s = make(CollageDesign::Featured, 5);
  CollageLayout sl = layout_collage(s);
  int a = needed_pixels(sl, s, 0, 0.5), b = needed_pixels(sl, s, 0, 1.0);
  CHECK(a > 0 && b > a);
  s.items[0].zoom = 2;
  CHECK(needed_pixels(layout_collage(s), s, 0, 1.0) > b);
  CHECK(needed_pixels(sl, s, 1, 1.0) < b);
}

static uint32_t pixel(cairo_surface_t *s, int x, int y) {
  cairo_surface_flush(s);
  return reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s))[x];
}

static void test_render() {
  // A red photo in every cell of a grid on a green background.
  cairo_surface_t *red = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 48);
  cairo_t *cr = cairo_create(red);
  cairo_set_source_rgb(cr, 1, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);
  Collage c = make(CollageDesign::Grid, 4, 400, 300);
  c.background = 0xFF00FF00;
  c.auto_background = false;
  c.spacing = 5;
  CollageImages img;
  img.photo = [&](int) { return red; };
  cairo_surface_t *out = render_collage(c, 0.5, img);
  CHECK(cairo_image_surface_get_width(out) == 200 && cairo_image_surface_get_height(out) == 150);
  CHECK(pixel(out, 1, 1) == 0xFF00FF00);
  CollageLayout l = layout_collage(c);
  CHECK(pixel(out, int(l.cells[0].cx() * 0.5), int(l.cells[0].cy() * 0.5)) == 0xFFFF0000);
  cairo_surface_destroy(out);
  // Without photos: placeholders, and nothing crashes for any design.
  CollageImages none;
  for (const auto &d : collage_designs()) {
    Collage e = make(d.id, 5, 300, 200);
    e.blur_background = 0;
    cairo_surface_t *s = render_collage(e, 1.0, none);
    CHECK(s != nullptr);
    if (s) cairo_surface_destroy(s);
    cairo_surface_t *t = render_collage(e, 1.0, img);
    CHECK(t != nullptr);
    if (t) cairo_surface_destroy(t);
  }
  cairo_surface_t *b = blurred_backdrop(red, 32);
  CHECK(b && cairo_image_surface_get_width(b) == 32);
  CHECK((pixel(b, 16, 12) & 0x00FF0000) > 0x00E00000);
  cairo_surface_destroy(b);
  cairo_surface_destroy(red);
}

static void test_speed() {
  // Laying out and drawing 40 photos at a large preview size stays quick.
  Collage c = make(CollageDesign::Mosaic, 40, 1920, 1080);
  cairo_surface_t *ph = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 240);
  CollageImages img;
  img.photo = [&](int) { return ph; };
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 3; ++i) {
    cairo_surface_t *s = render_collage(c, 0.6, img);
    cairo_surface_destroy(s);
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 3;
  printf("collage: 40 photos at 1152x648 in %.1f ms\n", ms);
  CHECK(ms < 1500);
  cairo_surface_destroy(ph);
}

static void test_history() {
  CollageHistory h;
  Collage a = make(CollageDesign::Grid, 2), b = a;
  h.reset(a);
  CHECK(!h.can_undo() && !h.can_redo());
  b.spacing = 4;
  h.push(b);
  h.push(b);  // equal: no new step
  CHECK(h.can_undo());
  CHECK(h.undo() == a);
  CHECK(h.can_redo());
  CHECK(h.redo() == b);
  Collage c2 = b;
  c2.items[0].pan_x = 0.5;
  h.amend(c2);
  CHECK(h.current() == c2 && !h.can_redo());
}

int main() {
  test_catalog();
  test_every_design_adapts();
  test_coverage();
  test_pan_zoom();
  test_hit_and_order();
  test_render();
  test_speed();
  test_history();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("collage tests passed\n");
  return 0;
}
