// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Unit tests for Photo's drawing tools (no display): stroke smoothing,
// coordinate mapping at zoom, pan and rotation, flattening (pixel checks),
// blur and pixelate regions, erasing, and undo/redo of drawing steps.
#include <cairo.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "draw.h"
#include "edit.h"

using namespace photo;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
static bool near(Pt a, Pt b, double eps = 1e-6) { return near(a.x, b.x, eps) && near(a.y, b.y, eps); }

static cairo_surface_t *solid(int w, int h, uint32_t argb) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgba(cr, ((argb >> 16) & 0xFF) / 255.0, ((argb >> 8) & 0xFF) / 255.0, (argb & 0xFF) / 255.0,
                        ((argb >> 24) & 0xFF) / 255.0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
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

static int ch(uint32_t p, int shift) { return int((p >> shift) & 0xFF); }

// A surface with a different colour in every pixel (a gradient pattern).
static cairo_surface_t *pattern(int w, int h) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  unsigned char *d = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      reinterpret_cast<uint32_t *>(d + size_t(y) * stride)[x] =
          0xFF000000u | (uint32_t((x * 37 + y * 11) & 0xFF) << 16) | (uint32_t((x * 5 + y * 53) & 0xFF) << 8) |
          uint32_t(((x ^ y) * 29) & 0xFF);
  cairo_surface_mark_dirty(s);
  return s;
}

static void test_smoothing() {
  // Thinning drops points closer than the minimum distance, keeps the end.
  std::vector<Pt> raw{{0, 0}, {0.3, 0}, {0.6, 0}, {2, 0}, {2.2, 0}, {5, 0}, {5.1, 0}};
  auto thin = thin_points(raw, 1.0);
  CHECK(thin.size() == 3);  // 0, 2, then 5.1 replaces 5
  CHECK(near(thin.front(), {0, 0}) && near(thin.back(), {5.1, 0}));
  // Fewer than three points are drawn as they are.
  CHECK(smooth_stroke({{1, 1}, {4, 5}}).size() == 2);
  // A straight line stays straight, and the spline passes through every
  // input point in order, starting and ending at the stroke's ends.
  std::vector<Pt> line{{0, 0}, {10, 10}, {20, 20}, {30, 30}};
  auto sl = smooth_stroke(line, 1.0);
  CHECK(near(sl.front(), line.front()) && near(sl.back(), line.back()));
  for (const Pt &p : sl) CHECK(near(p.x, p.y, 1e-9));
  size_t found = 0;
  for (const Pt &p : sl)
    if (found < line.size() && near(p, line[found], 1e-9)) ++found;
  CHECK(found == line.size());
  // A zig-zag is smoothed: the turn between consecutive output segments is
  // far below the 90 degrees of the input corners, and the curve stays
  // close to the input polygon.
  std::vector<Pt> zig{{0, 0}, {20, 20}, {40, 0}, {60, 20}, {80, 0}};
  auto sz = smooth_stroke(zig, 1.0);
  CHECK(sz.size() > zig.size() * 10);
  double max_turn = 0;
  for (size_t i = 1; i + 1 < sz.size(); ++i) {
    double a1 = std::atan2(sz[i].y - sz[i - 1].y, sz[i].x - sz[i - 1].x);
    double a2 = std::atan2(sz[i + 1].y - sz[i].y, sz[i + 1].x - sz[i].x);
    double d = std::fabs(std::remainder(a2 - a1, 2 * M_PI));
    max_turn = std::max(max_turn, d);
  }
  CHECK(max_turn < M_PI / 4);
  for (const Pt &p : sz) CHECK(p.y > -6 && p.y < 26 && p.x >= -0.001 && p.x <= 80.001);
  // Segment length is bounded by the step.
  for (size_t i = 1; i < sz.size(); ++i) CHECK(std::hypot(sz[i].x - sz[i - 1].x, sz[i].y - sz[i - 1].y) <= 1.6);
  // Repeated points do not produce NaNs.
  auto dup = smooth_stroke({{5, 5}, {5, 5}, {9, 9}, {9, 9}}, 1.0);
  for (const Pt &p : dup) CHECK(std::isfinite(p.x) && std::isfinite(p.y));
}

static void test_mapping() {
  Edits e;
  e.marks.w = 400;
  e.marks.h = 200;
  // No crop, full size result: identity.
  MarkMap m = mark_map(e, 400, 200);
  CHECK(near(m.to_out({123, 45}), {123, 45}));
  // A half-size preview halves everything.
  m = mark_map(e, 200, 100);
  CHECK(near(m.to_out({100, 50}), {50, 25}));
  CHECK(near(m.scale(), 0.5));
  // Crop the right half: x 200 becomes 0 in the result.
  e.crop = {0.5, 0, 0.5, 1};
  m = mark_map(e, 200, 200);
  CHECK(near(m.to_out({200, 0}), {0, 0}));
  CHECK(near(m.to_out({400, 200}), {200, 200}));
  // And with a resize of the crop to 100 x 100.
  m = mark_map(e, 100, 100);
  CHECK(near(m.to_out({300, 100}), {50, 50}));
  // Screen: the cropped result shown at (50, 60), 2x zoom (400 x 400 px for
  // a 200 x 200 result) — zoom and pan are the rectangle on screen.
  ViewMap v = view_map(e, 50, 60, 400, 400);
  CHECK(near(v.to_out({200, 0}), {50, 60}));
  CHECK(near(v.to_out({300, 100}), {250, 260}));
  CHECK(near(v.to_image({250, 260}), {300, 100}));
  // Panning moves the rectangle: the same image point follows.
  ViewMap panned = view_map(e, 50 - 120, 60 + 30, 400, 400);
  CHECK(near(panned.to_out({300, 100}), {250 - 120, 260 + 30}));
  // Round trips at odd zoom levels.
  for (double z : {0.137, 0.5, 1.0, 3.3, 12.0}) {
    ViewMap vz = view_map(e, 17.5, -40, 200 * z, 200 * z);
    for (Pt p : {Pt{200, 0}, Pt{250.25, 199.5}, Pt{399, 3}}) CHECK(near(vz.to_image(vz.to_out(p)), p, 1e-6));
  }
  // A view turned a quarter clockwise: the bounding rectangle is the same
  // here (square), the top-left corner of the image goes top-right.
  ViewMap r = view_map(e, 0, 0, 200, 200, 1);
  CHECK(near(r.to_out({200, 0}), {200, 0}));
  CHECK(near(r.to_out({400, 0}), {200, 200}));
  CHECK(near(r.to_out({200, 200}), {0, 0}));
  for (int q = 0; q < 4; ++q) {
    Edits wide;
    wide.marks.w = 300;
    wide.marks.h = 100;
    double iw = q % 2 ? 100 : 300, ih = q % 2 ? 300 : 100;
    ViewMap vq = view_map(wide, 10, 20, iw, ih, q);
    for (Pt p : {Pt{0, 0}, Pt{300, 100}, Pt{150, 50}, Pt{299, 1}}) {
      Pt s = vq.to_out(p);
      CHECK(s.x >= 10 - 1e-6 && s.x <= 10 + iw + 1e-6 && s.y >= 20 - 1e-6 && s.y <= 20 + ih + 1e-6);
      CHECK(near(vq.to_image(s), p, 1e-6));
    }
    // cairo agrees with to_out().
    cairo_surface_t *dummy = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(dummy);
    vq.apply(cr);
    double x = 75, y = 25;
    cairo_user_to_device(cr, &x, &y);
    CHECK(near({x, y}, vq.to_out({75, 25}), 1e-6));
    cairo_destroy(cr);
    cairo_surface_destroy(dummy);
  }
}

static void test_rotation_follows_image() {
  // A red dot drawn near the top-left of a 60 x 40 photo; after rotating
  // the photo right, the dot is near the top-right of the 40 x 60 result.
  cairo_surface_t *src = solid(60, 40, 0xFFFFFFFF);
  Edits e;
  e.marks.w = 60;
  e.marks.h = 40;
  Mark dot;
  dot.kind = MarkKind::Rect;
  dot.fill = true;
  dot.color = 0xFFFF0000;
  dot.width = 1;
  dot.pts = {{5, 5}, {15, 10}};
  e.marks.items.push_back(dot);
  cairo_surface_t *out = render(src, e);
  CHECK(ch(pixel(out, 10, 7), 8) < 40 && ch(pixel(out, 10, 7), 16) > 200);
  CHECK(pixel(out, 50, 30) == 0xFFFFFFFF);
  cairo_surface_destroy(out);
  Edits r = e;
  r.orient = gallery::orient_then(r.orient, gallery::Orient{1, false});
  rotate_marks(r.marks, 1);
  CHECK(r.marks.w == 40 && r.marks.h == 60);
  out = render(src, r);
  CHECK(cairo_image_surface_get_width(out) == 40 && cairo_image_surface_get_height(out) == 60);
  // (10, 7) -> (40 - 7, 10) = (33, 10).
  CHECK(ch(pixel(out, 32, 10), 16) > 200 && ch(pixel(out, 32, 10), 8) < 40);
  CHECK(pixel(out, 7, 10) == 0xFFFFFFFF);
  cairo_surface_destroy(out);
  // Mirroring.
  Marks f = e.marks;
  flip_marks(f, true);
  CHECK(near(f.items[0].pts[0], {55, 5}));
  flip_marks(f, false);
  CHECK(near(f.items[0].pts[0], {55, 35}));
  // Four quarter turns give the marks back.
  Marks q = e.marks;
  rotate_marks(q, 4);
  CHECK(q == e.marks);
  rotate_marks(q, 1);
  rotate_marks(q, 3);
  CHECK(q.w == 60 && q.h == 40 && near(q.items[0].pts[1], {15, 10}, 1e-9));
  cairo_surface_destroy(src);
}

static void test_flatten() {
  // An opaque filled rectangle: exact colour inside, untouched outside.
  cairo_surface_t *base = solid(100, 80, 0xFF808080);
  Edits e;
  e.marks.w = 100;
  e.marks.h = 80;
  Mark rect;
  rect.kind = MarkKind::Rect;
  rect.fill = true;
  rect.color = 0xFF0000FF;
  rect.width = 2;
  rect.pts = {{20, 20}, {40, 50}};
  e.marks.items.push_back(rect);
  cairo_surface_t *out = render(base, e);
  CHECK(out != base);
  CHECK(pixel(out, 30, 35) == 0xFF0000FF);
  CHECK(pixel(out, 5, 5) == 0xFF808080);
  CHECK(pixel(out, 60, 35) == 0xFF808080);
  // The original is never changed.
  CHECK(pixel(base, 30, 35) == 0xFF808080);
  cairo_surface_destroy(out);
  // Without fill only the outline is drawn.
  e.marks.items[0].fill = false;
  out = render(base, e);
  CHECK(pixel(out, 30, 35) == 0xFF808080);
  CHECK(pixel(out, 20, 35) == 0xFF0000FF);
  cairo_surface_destroy(out);
  // Opacity: a 50% white line over grey is lighter grey.
  Edits l;
  l.marks.w = 100;
  l.marks.h = 80;
  Mark line;
  line.kind = MarkKind::Line;
  line.color = 0xFFFFFFFF;
  line.opacity = 0.5;
  line.width = 10;
  line.pts = {{10, 40}, {90, 40}};
  l.marks.items.push_back(line);
  out = render(base, l);
  int v = ch(pixel(out, 50, 40), 0);
  CHECK(v > 180 && v < 205);  // 128 + 127 * 0.5 = 191
  cairo_surface_destroy(out);
  // The highlighter multiplies: yellow over white stays yellow, over a
  // blue-grey it darkens and never lightens.
  cairo_surface_t *white = solid(60, 20, 0xFFFFFFFF);
  Edits h;
  h.marks.w = 60;
  h.marks.h = 20;
  Mark hl;
  hl.kind = MarkKind::Highlighter;
  hl.color = 0xFFFFFF00;
  hl.opacity = 1;
  hl.width = 10;
  hl.pts = {{5, 10}, {30, 10}, {55, 10}};
  h.marks.items.push_back(hl);
  out = render(white, h);
  uint32_t p = pixel(out, 30, 10);
  CHECK(ch(p, 16) == 255 && ch(p, 8) == 255 && ch(p, 0) < 5);
  cairo_surface_destroy(out);
  cairo_surface_t *grey = solid(60, 20, 0xFF6080A0);
  out = render(grey, h);
  p = pixel(out, 30, 10);
  CHECK(ch(p, 16) <= 0x60 + 1 && ch(p, 8) <= 0x80 + 1 && ch(p, 0) < 5);
  CHECK(pixel(out, 30, 1) == 0xFF6080A0);
  cairo_surface_destroy(out);
  // Full resolution: marks drawn at a preview scale land on the same place
  // of the full image (crop 50% from the left, rendered at half size).
  cairo_surface_t *big = solid(200, 100, 0xFF000000);
  Edits c;
  c.marks.w = 200;
  c.marks.h = 100;
  c.crop = {0.5, 0, 0.5, 1};
  Mark sq = rect;
  sq.fill = true;
  sq.color = 0xFF00FF00;
  sq.pts = {{150, 40}, {170, 60}};
  c.marks.items.push_back(sq);
  out = render(big, c);
  CHECK(cairo_image_surface_get_width(out) == 100);
  CHECK(pixel(out, 60, 50) == 0xFF00FF00);  // 160 - 100
  CHECK(pixel(out, 20, 50) == 0xFF000000);
  cairo_surface_destroy(out);
  cairo_surface_t *half = solid(100, 50, 0xFF000000);
  out = render(half, c, 0.5);
  CHECK(cairo_image_surface_get_width(out) == 50);
  CHECK(pixel(out, 30, 25) == 0xFF00FF00);
  CHECK(pixel(out, 10, 25) == 0xFF000000);
  cairo_surface_destroy(out);
  // render() can leave the marks out, or draw only the effects.
  out = render(big, c, 1.0, kNoMarks);
  CHECK(pixel(out, 60, 50) == 0xFF000000);
  cairo_surface_destroy(out);
  // Marks make the edit non-identity and never "geometry only" (a JPEG
  // with drawings must be encoded again).
  CHECK(!c.marks.empty() && !c.identity() && !c.geometry_only());
  Edits only_marks;
  only_marks.marks = c.marks;
  CHECK(!only_marks.identity() && only_marks.tone_identity());
  // Text: something is drawn inside its bounds, nothing outside.
  Edits t;
  t.marks.w = 200;
  t.marks.h = 100;
  Mark tm;
  tm.kind = MarkKind::Text;
  tm.text = "Hi";
  tm.size = 40;
  tm.color = 0xFFFFFFFF;
  tm.box = false;
  tm.pts = {{20, 20}};
  t.marks.items.push_back(tm);
  Box tb = mark_bounds(tm);
  CHECK(tb.w() > 20 && tb.h() > 40);
  out = render(big, t);
  int lit = 0, stray = 0;
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 200; ++x) {
      bool on = ch(pixel(out, x, y), 8) > 128;
      if (on && tb.contains({x + 0.5, y + 0.5})) ++lit;
      else if (on) ++stray;
    }
  CHECK(lit > 50 && stray == 0);
  cairo_surface_destroy(out);
  // With a box the background behind dark text is light.
  t.marks.items[0].box = true;
  t.marks.items[0].color = 0xFF000000;
  out = render(big, t);
  CHECK(ch(pixel(out, int(tb.x0) + 2, int(tb.y0) + int(tb.h() / 2)), 8) > 150);
  cairo_surface_destroy(out);
  cairo_surface_destroy(big);
  cairo_surface_destroy(half);
  cairo_surface_destroy(white);
  cairo_surface_destroy(grey);
  cairo_surface_destroy(base);
}

static void test_effects() {
  // Pixelate: uniform blocks of the average colour inside, untouched
  // outside.
  cairo_surface_t *s = pattern(64, 48);
  cairo_surface_t *orig = pattern(64, 48);
  pixelate_region(s, 8, 8, 32, 16, 8);
  for (int by = 8; by < 24; by += 8)
    for (int bx = 8; bx < 40; bx += 8) {
      uint32_t first = pixel(s, bx, by);
      for (int y = by; y < by + 8; ++y)
        for (int x = bx; x < bx + 8; ++x) CHECK(pixel(s, x, y) == first);
      // The block holds the average of the original block.
      long sum = 0;
      for (int y = by; y < by + 8; ++y)
        for (int x = bx; x < bx + 8; ++x) sum += ch(pixel(orig, x, y), 16);
      CHECK(std::abs(ch(first, 16) - int(sum / 64)) <= 1);
    }
  int changed_outside = 0;
  for (int y = 0; y < 48; ++y)
    for (int x = 0; x < 64; ++x)
      if (!(x >= 8 && x < 40 && y >= 8 && y < 24) && pixel(s, x, y) != pixel(orig, x, y)) ++changed_outside;
  CHECK(changed_outside == 0);
  // Regions are clipped to the surface.
  pixelate_region(s, 50, 40, 100, 100, 6);
  pixelate_region(s, -10, -10, 5, 5, 4);  // entirely outside: no-op, no crash
  cairo_surface_destroy(s);
  // Blur: the region's contrast drops a lot; outside stays exact.
  s = pattern(64, 48);
  blur_region(s, 16, 8, 32, 32, 4);
  auto spread = [](cairo_surface_t *img) {
    double mean = 0, var = 0;
    int n = 0;
    for (int y = 12; y < 36; ++y)
      for (int x = 20; x < 44; ++x) {
        mean += ch(pixel(img, x, y), 16);
        ++n;
      }
    mean /= n;
    for (int y = 12; y < 36; ++y)
      for (int x = 20; x < 44; ++x) var += std::pow(ch(pixel(img, x, y), 16) - mean, 2);
    return var / n;
  };
  CHECK(spread(s) < spread(orig) * 0.2);
  changed_outside = 0;
  for (int y = 0; y < 48; ++y)
    for (int x = 0; x < 64; ++x)
      if (!(x >= 16 && x < 48 && y >= 8 && y < 40) && pixel(s, x, y) != pixel(orig, x, y)) ++changed_outside;
  CHECK(changed_outside == 0);
  // Pixels stay valid premultiplied ARGB.
  for (int y = 8; y < 40; ++y)
    for (int x = 16; x < 48; ++x) {
      uint32_t p = pixel(s, x, y);
      CHECK(ch(p, 24) >= ch(p, 16) && ch(p, 24) >= ch(p, 8) && ch(p, 24) >= ch(p, 0));
    }
  cairo_surface_destroy(s);
  // Through the pipeline: a pixelate mark at a half-size preview uses
  // blocks of half the size.
  Edits e;
  e.marks.w = 64;
  e.marks.h = 48;
  Mark px;
  px.kind = MarkKind::Pixelate;
  px.width = 8;
  px.pts = {{0, 0}, {32, 32}};
  e.marks.items.push_back(px);
  cairo_surface_t *out = render(orig, e);
  CHECK(pixel(out, 0, 0) == pixel(out, 7, 7) && pixel(out, 0, 0) != pixel(out, 8, 0));
  CHECK(pixel(out, 40, 40) == pixel(orig, 40, 40));
  cairo_surface_destroy(out);
  // Vectors only: the pixelate mark is not applied.
  out = render(orig, e, 1.0, kMarkVectors);
  CHECK(pixel(out, 1, 0) == pixel(orig, 1, 0));
  cairo_surface_destroy(out);
  cairo_surface_destroy(orig);
}

static void test_hit_and_edit() {
  Marks marks;
  marks.w = marks.h = 500;
  Mark pen;
  pen.kind = MarkKind::Pen;
  pen.width = 6;
  for (int i = 0; i <= 20; ++i) pen.pts.push_back({50.0 + i * 10, 100});
  Mark box;
  box.kind = MarkKind::Rect;
  box.width = 4;
  box.pts = {{300, 300}, {400, 380}};
  marks.items = {pen, box};
  CHECK(mark_at(marks, {120, 102}, 1) == 0);
  CHECK(mark_at(marks, {120, 120}, 1) == -1);
  CHECK(mark_at(marks, {300, 340}, 1) == 1);  // on the outline
  CHECK(mark_at(marks, {350, 340}, 1) == -1);  // inside an empty rectangle
  marks.items[1].fill = true;
  CHECK(mark_at(marks, {350, 340}, 1) == 1);
  // Moving and resizing.
  Mark moved = box;
  translate_mark(moved, 10, -20);
  CHECK(near(moved.pts[0], {310, 280}));
  Mark big = box;
  reshape_mark(big, {300, 300, 400, 380}, {300, 300, 500, 460});
  CHECK(near(big.pts[1], {500, 460}));
  // The eraser cuts a stroke in two and leaves the photo alone.
  Marks er = marks;
  CHECK(erase_at(er, {150, 100}, 12));
  int pens = 0;
  for (const auto &m : er.items)
    if (m.kind == MarkKind::Pen) {
      ++pens;
      for (const Pt &p : m.pts) CHECK(std::fabs(p.x - 150) > 12);
    }
  CHECK(pens == 2);
  CHECK(!erase_at(er, {20, 20}, 5));
  // Erasing a shape removes it.
  CHECK(erase_at(er, {350, 340}, 3));
  for (const auto &m : er.items) CHECK(m.kind != MarkKind::Rect);
  // A sparse stroke is cut where a segment passes under the eraser.
  Marks sparse;
  sparse.w = sparse.h = 100;
  Mark s2;
  s2.kind = MarkKind::Pen;
  s2.width = 2;
  s2.pts = {{0, 50}, {40, 50}, {60, 50}, {100, 50}};
  sparse.items.push_back(s2);
  CHECK(erase_at(sparse, {50, 50}, 3));
  CHECK(sparse.items.size() == 2);
  // Only the part under the eraser goes, not the whole long segment.
  if (sparse.items.size() == 2) {
    double left_end = 0, right_start = 100;
    for (const Pt &p : sparse.items[0].pts) left_end = std::max(left_end, p.x);
    for (const Pt &p : sparse.items[1].pts) right_start = std::min(right_start, p.x);
    CHECK(left_end > 42 && left_end < 47);
    CHECK(right_start > 53 && right_start < 58);
    CHECK(near(sparse.items[0].pts.front(), {0, 50}) && near(sparse.items[1].pts.back(), {100, 50}));
  }
  // A two-point stroke is cut too.
  Marks two;
  two.w = two.h = 100;
  Mark t2;
  t2.kind = MarkKind::Highlighter;
  t2.width = 2;
  t2.pts = {{0, 10}, {100, 10}};
  two.items.push_back(t2);
  CHECK(erase_at(two, {30, 10}, 4) && two.items.size() == 2);
}

static void test_undo_redo() {
  // Each finished stroke is one step in Photo's history; undo removes the
  // last stroke, redo brings it back, and a new stroke drops the redo.
  History h;
  Edits e;
  e.marks.w = e.marks.h = 100;
  Mark a;
  a.pts = {{1, 1}, {5, 5}};
  Mark b = a;
  b.color = 0xFF00FF00;
  e.marks.items.push_back(a);
  h.push(e);
  e.marks.items.push_back(b);
  h.push(e);
  CHECK(h.current().marks.items.size() == 2);
  CHECK(h.undo().marks.items.size() == 1);
  CHECK(h.undo().marks.items.empty());
  CHECK(!h.can_undo());
  CHECK(h.redo().marks.items.size() == 1);
  CHECK(h.redo().marks.items.size() == 2 && h.current().marks.items[1].color == 0xFF00FF00);
  h.undo();
  Edits c = h.current();
  Mark d = a;
  d.kind = MarkKind::Arrow;
  c.marks.items.push_back(d);
  h.push(c);
  CHECK(!h.can_redo() && h.current().marks.items.back().kind == MarkKind::Arrow);
  // Moving a mark is a step too; an eraser drag amends one step.
  Edits m = h.current();
  translate_mark(m.marks.items[0], 3, 0);
  h.push(m);
  CHECK(near(h.current().marks.items[0].pts[0], {4, 1}));
  size_t steps = h.size();
  Edits er = h.current();
  er.marks.items.pop_back();
  h.push(er);
  er.marks.items.clear();
  h.amend(er);
  CHECK(h.size() == steps + 1 && h.current().marks.empty());
  CHECK(h.undo().marks.items.size() == 2);
  // Equal marks do not make a new step.
  size_t n = h.size();
  h.push(h.current());
  CHECK(h.size() == n);
  // Drawing and tone edits share the history.
  Edits tone = h.current();
  tone.adjust.contrast = 20;
  h.push(tone);
  CHECK(h.current().marks.items.size() == 2 && h.current().adjust.contrast == 20);
}

int main() {
  setenv("LC_ALL", "C", 1);
  test_smoothing();
  test_mapping();
  test_rotation_follows_image();
  test_flatten();
  test_effects();
  test_hit_and_edit();
  test_undo_redo();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("photo drawing tests passed\n");
  return 0;
}
