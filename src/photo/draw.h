// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo's drawing tools: annotations (marks) kept as vectors on top of the
// photo while editing, and flattened into the pixels when the photo is
// saved.
//
// Coordinates. Marks live in the pixels of the *oriented, uncropped* full
// size image (the photo after rotation/flip, before crop and resize):
// drawing at any zoom level stores full resolution positions, cropping only
// hides marks, and rotating or flipping the photo turns the marks with it
// (rotate_marks, flip_marks). MarkMap takes those coordinates to a
// rendered surface (the result of crop and resize, at any preview scale);
// ViewMap takes them to the screen (zoom, pan and an optional view
// rotation).
//
// Nothing here needs a display: the smoothing, mapping, hit testing,
// erasing and flattening are unit tested.
#pragma once

#include <cairo.h>

#include <cstdint>
#include <string>
#include <vector>

namespace photo {

struct Edits;

struct Pt {
  double x = 0, y = 0;
  bool operator==(const Pt &o) const { return x == o.x && y == o.y; }
};

struct Box {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double w() const { return x1 - x0; }
  double h() const { return y1 - y0; }
  bool contains(Pt p, double tol = 0) const {
    return p.x >= x0 - tol && p.x <= x1 + tol && p.y >= y0 - tol && p.y <= y1 + tol;
  }
};

// RedEye: a circle (two corners) in which red pupils are made dark (the
// Red-Eye Fix plugin places it with a click).
enum class MarkKind { Pen, Highlighter, Line, Arrow, Rect, Ellipse, Text, Blur, Pixelate, RedEye };

enum class FontFace { Sans, Serif, Mono };

struct Mark {
  MarkKind kind = MarkKind::Pen;
  // Pen, Highlighter: the points drawn (smoothed when rendered).
  // Line, Arrow: start and end. Rect, Ellipse, Blur, Pixelate: two corners.
  // Text: the top-left corner.
  std::vector<Pt> pts;
  uint32_t color = 0xFFE5484D;  // 0xAARRGGBB, not premultiplied
  double width = 6;             // stroke width; block size/radius for Blur and Pixelate
  double opacity = 1;           // 0..1
  bool fill = false;            // Rect, Ellipse: filled shape
  // Text.
  std::string text;
  FontFace font = FontFace::Sans;
  double size = 48;  // font size in image pixels
  bool box = false;  // a background box behind the text
  bool effect() const { return kind == MarkKind::Blur || kind == MarkKind::Pixelate || kind == MarkKind::RedEye; }
  bool freehand() const { return kind == MarkKind::Pen || kind == MarkKind::Highlighter; }
  bool operator==(const Mark &o) const;
  bool operator!=(const Mark &o) const { return !(*this == o); }
};

// The marks of a photo, and the size of the oriented image they refer to.
struct Marks {
  int w = 0, h = 0;
  std::vector<Mark> items;
  bool empty() const { return items.empty(); }
  bool has_effects() const;
  bool has_vectors() const;
  bool operator==(const Marks &o) const { return items == o.items && (items.empty() || (w == o.w && h == o.h)); }
  bool operator!=(const Marks &o) const { return !(*this == o); }
};

// An affine map without shear: out = (x * kx + tx, y * ky + ty), with an
// optional quarter turn applied after scaling (screen views).
struct ViewMap {
  double kx = 1, ky = 1, tx = 0, ty = 0;
  int quarter = 0;        // clockwise quarter turns of the view
  double cx = 0, cy = 0;  // centre of the turn
  Pt to_out(Pt p) const;
  Pt to_image(Pt s) const;
  double scale() const;  // average length factor
  void apply(cairo_t *cr) const;  // sets cr to draw in image coordinates
};
using MarkMap = ViewMap;

// Image coordinates (oriented, uncropped, e.marks.w x e.marks.h) to the
// pixels of a rendered result |out_w| x |out_h| of Edits |e|.
MarkMap mark_map(const Edits &e, int out_w, int out_h);
// Image coordinates to the screen: the rendered result is shown in the
// screen rectangle (ix, iy, iw, ih) (the bounding box after the turn),
// turned by |quarter| clockwise quarter turns around its centre.
ViewMap view_map(const Edits &e, double ix, double iy, double iw, double ih, int quarter = 0);

// Stroke smoothing. Points closer than |min_dist| to the previous kept
// point are dropped (the last point is always kept); the rest are joined by
// a centripetal Catmull-Rom spline that passes through every kept point,
// with segments of at most |step| units.
std::vector<Pt> thin_points(const std::vector<Pt> &raw, double min_dist);
std::vector<Pt> smooth_stroke(const std::vector<Pt> &pts, double step = 2.0);

// Geometry.
Box mark_bounds(const Mark &m);  // includes the stroke width
bool hit_mark(const Mark &m, Pt p, double tolerance);
int mark_at(const Marks &marks, Pt p, double tolerance);  // topmost, -1 none
void translate_mark(Mark &m, double dx, double dy);
// Scales |m| so that box |from| becomes |to| (text scales its size).
void reshape_mark(Mark &m, const Box &from, const Box &to);
double mark_text_width(const Mark &m);  // text extents with cairo's toy API
double mark_text_height(const Mark &m);
// Quarter turns clockwise / mirror of the whole image (w x h before).
void rotate_marks(Marks &marks, int quarter_turns);
void flip_marks(Marks &marks, bool horizontal);
// Eraser: cuts freehand strokes where the circle (p, radius) touches them
// and removes other marks it touches. Returns true when something changed.
bool erase_at(Marks &marks, Pt p, double radius);

// Pixel effects on an ARGB32 surface, in its pixels (clipped to it).
void pixelate_region(cairo_surface_t *s, int x, int y, int w, int h, int block);
void blur_region(cairo_surface_t *s, int x, int y, int w, int h, int radius);
// Darkens the red pupils in the ellipse inside (x, y, w, h); pixels that
// are not red stay as they are.
void red_eye_region(cairo_surface_t *s, int x, int y, int w, int h);
// The red pupil near (x, y) of |s|: the box around the red pixels joined
// to the reddest one within |radius|, in pixels of |s|. False when nothing
// there is red.
bool find_red_eye(cairo_surface_t *s, double x, double y, double radius, Box &out);

// Draws marks with |map| onto |cr| (image coordinates to cr's space).
// |vectors| and |effects| choose which kinds are drawn; effects need the
// surface |target| that cr draws on (they read and write its pixels) and
// are skipped when it is null.
void draw_marks(cairo_t *cr, const Marks &marks, const MarkMap &map, bool vectors, bool effects,
                cairo_surface_t *target = nullptr, int skip = -1);
void draw_mark(cairo_t *cr, const Mark &m, const MarkMap &map);
// Renders all marks into |s| (the rendered result of the edits).
void flatten_marks(cairo_surface_t *s, const Edits &e, bool vectors = true, bool effects = true);

// The default palette (0xAARRGGBB).
const std::vector<uint32_t> &palette();

}  // namespace photo
