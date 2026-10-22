// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "draw.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "edit.h"

namespace photo {

namespace {

double dist(Pt a, Pt b) { return std::hypot(a.x - b.x, a.y - b.y); }

double seg_dist(Pt p, Pt a, Pt b) {
  double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
  if (l2 <= 1e-12) return dist(p, a);
  double t = std::max(0.0, std::min(1.0, ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2));
  return dist(p, {a.x + t * dx, a.y + t * dy});
}

Box corners(const Mark &m) {
  Box b;
  if (m.pts.empty()) return b;
  b.x0 = b.x1 = m.pts[0].x;
  b.y0 = b.y1 = m.pts[0].y;
  for (const Pt &p : m.pts) {
    b.x0 = std::min(b.x0, p.x);
    b.y0 = std::min(b.y0, p.y);
    b.x1 = std::max(b.x1, p.x);
    b.y1 = std::max(b.y1, p.y);
  }
  return b;
}

const char *face_name(FontFace f) {
  switch (f) {
    case FontFace::Serif: return "Serif";
    case FontFace::Mono: return "Monospace";
    case FontFace::Sans: break;
  }
  return "Sans";
}

std::vector<std::string> text_lines(const std::string &text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (true) {
    size_t nl = text.find('\n', start);
    lines.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return lines;
}

void set_font(cairo_t *cr, const Mark &m) {
  cairo_select_font_face(cr, face_name(m.font), CAIRO_FONT_SLANT_NORMAL,
                         m.font == FontFace::Mono ? CAIRO_FONT_WEIGHT_NORMAL : CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, std::max(1.0, m.size));
}

// Measures with a private context: safe from any thread.
void measure_text(const Mark &m, double &w, double &h) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cairo_t *cr = cairo_create(s);
  set_font(cr, m);
  w = 0;
  auto lines = text_lines(m.text);
  for (const auto &l : lines) {
    cairo_text_extents_t te;
    cairo_text_extents(cr, l.c_str(), &te);
    w = std::max(w, te.x_advance);
  }
  h = m.size * 1.25 * double(lines.size());
  cairo_destroy(cr);
  cairo_surface_destroy(s);
}

double text_pad(const Mark &m) { return m.size * 0.25; }

void set_color(cairo_t *cr, uint32_t c, double opacity) {
  double a = ((c >> 24) & 0xFF) / 255.0 * std::max(0.0, std::min(1.0, opacity));
  cairo_set_source_rgba(cr, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, a);
}

double luma_of(uint32_t c) {
  return (0.2126 * ((c >> 16) & 0xFF) + 0.7152 * ((c >> 8) & 0xFF) + 0.0722 * (c & 0xFF)) / 255.0;
}

void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
  r = std::min(r, std::min(w, h) / 2);
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

// The pixel rectangle of a region mark on the target, clipped to it.
bool region_pixels(const Mark &m, const MarkMap &map, int sw, int sh, int &x, int &y, int &w, int &h) {
  if (m.pts.size() < 2) return false;
  Pt a = map.to_out(m.pts[0]), b = map.to_out(m.pts[1]);
  int x0 = int(std::floor(std::min(a.x, b.x))), y0 = int(std::floor(std::min(a.y, b.y)));
  int x1 = int(std::ceil(std::max(a.x, b.x))), y1 = int(std::ceil(std::max(a.y, b.y)));
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(sw, x1);
  y1 = std::min(sh, y1);
  if (x1 <= x0 || y1 <= y0) return false;
  x = x0;
  y = y0;
  w = x1 - x0;
  h = y1 - y0;
  return true;
}

}  // namespace

// ---------------------------------------------------------------- marks

bool Mark::operator==(const Mark &o) const {
  return kind == o.kind && pts == o.pts && color == o.color && width == o.width && opacity == o.opacity &&
         fill == o.fill && text == o.text && font == o.font && size == o.size && box == o.box;
}

bool Marks::has_effects() const {
  for (const auto &m : items)
    if (m.effect()) return true;
  return false;
}

bool Marks::has_vectors() const {
  for (const auto &m : items)
    if (!m.effect()) return true;
  return false;
}

const std::vector<uint32_t> &palette() {
  static const std::vector<uint32_t> p{0xFF111111, 0xFFFFFFFF, 0xFFE5484D, 0xFFF76B15, 0xFFFFC53D,
                                       0xFF30A46C, 0xFF0090FF, 0xFF8E4EC6, 0xFFE93D82};
  return p;
}

// ------------------------------------------------------------- mapping

Pt ViewMap::to_out(Pt p) const {
  Pt v{p.x * kx + tx, p.y * ky + ty};
  int q = ((quarter % 4) + 4) % 4;
  for (int i = 0; i < q; ++i) v = {cx - (v.y - cy), cy + (v.x - cx)};
  return v;
}

Pt ViewMap::to_image(Pt s) const {
  int q = ((quarter % 4) + 4) % 4;
  for (int i = 0; i < q; ++i) s = {cx + (s.y - cy), cy - (s.x - cx)};  // counter-clockwise
  return {kx != 0 ? (s.x - tx) / kx : 0, ky != 0 ? (s.y - ty) / ky : 0};
}

double ViewMap::scale() const { return std::sqrt(std::fabs(kx * ky)); }

void ViewMap::apply(cairo_t *cr) const {
  int q = ((quarter % 4) + 4) % 4;
  if (q) {
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, q * M_PI / 2);
    cairo_translate(cr, -cx, -cy);
  }
  cairo_translate(cr, tx, ty);
  cairo_scale(cr, kx, ky);
}

MarkMap mark_map(const Edits &e, int out_w, int out_h) {
  MarkMap m;
  int ow = e.marks.w, oh = e.marks.h;
  if (ow <= 0 || oh <= 0) return m;
  int cx, cy, cw, ch;
  crop_pixels(e.crop, ow, oh, cx, cy, cw, ch);
  m.kx = double(out_w) / cw;
  m.ky = double(out_h) / ch;
  m.tx = -cx * m.kx;
  m.ty = -cy * m.ky;
  return m;
}

ViewMap view_map(const Edits &e, double ix, double iy, double iw, double ih, int quarter) {
  int q = ((quarter % 4) + 4) % 4;
  double uw = q % 2 ? ih : iw, uh = q % 2 ? iw : ih;  // the image before the turn
  double ccx = ix + iw / 2, ccy = iy + ih / 2;
  ViewMap v;
  int ow = e.marks.w, oh = e.marks.h;
  if (ow <= 0 || oh <= 0) return v;
  int cx, cy, cw, ch;
  crop_pixels(e.crop, ow, oh, cx, cy, cw, ch);
  v.kx = uw / cw;
  v.ky = uh / ch;
  double ux = ccx - uw / 2, uy = ccy - uh / 2;
  v.tx = ux - cx * v.kx;
  v.ty = uy - cy * v.ky;
  v.quarter = q;
  v.cx = ccx;
  v.cy = ccy;
  return v;
}

// ----------------------------------------------------------- smoothing

std::vector<Pt> thin_points(const std::vector<Pt> &raw, double min_dist) {
  std::vector<Pt> out;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (out.empty() || dist(raw[i], out.back()) >= min_dist) out.push_back(raw[i]);
    else if (i + 1 == raw.size() && out.size() > 1) out.back() = raw[i];  // keep the end
    else if (i + 1 == raw.size() && dist(raw[i], out.back()) > 0) out.push_back(raw[i]);
  }
  return out;
}

std::vector<Pt> smooth_stroke(const std::vector<Pt> &pts, double step) {
  if (pts.size() < 3) return pts;
  step = std::max(0.1, step);
  std::vector<Pt> out{pts[0]};
  size_t n = pts.size();
  auto at = [&](long i) -> Pt {
    if (i < 0) return {2 * pts[0].x - pts[1].x, 2 * pts[0].y - pts[1].y};
    if (i >= long(n)) return {2 * pts[n - 1].x - pts[n - 2].x, 2 * pts[n - 1].y - pts[n - 2].y};
    return pts[size_t(i)];
  };
  for (size_t i = 0; i + 1 < n; ++i) {
    Pt p0 = at(long(i) - 1), p1 = pts[i], p2 = pts[i + 1], p3 = at(long(i) + 2);
    // Centripetal parameterisation (alpha 0.5): no cusps or loops.
    auto knot = [](double t, Pt a, Pt b) { return t + std::max(1e-6, std::sqrt(dist(a, b))); };
    double t0 = 0, t1 = knot(t0, p0, p1), t2 = knot(t1, p1, p2), t3 = knot(t2, p2, p3);
    int steps = std::max(1, int(std::ceil(dist(p1, p2) / step)));
    for (int s = 1; s <= steps; ++s) {
      if (s == steps) {
        out.push_back(p2);
        break;
      }
      double t = t1 + (t2 - t1) * s / steps;
      auto lerp = [](Pt a, Pt b, double ta, double tb, double t) {
        double k = (t - ta) / (tb - ta);
        return Pt{a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k};
      };
      Pt a1 = lerp(p0, p1, t0, t1, t), a2 = lerp(p1, p2, t1, t2, t), a3 = lerp(p2, p3, t2, t3, t);
      Pt b1 = lerp(a1, a2, t0, t2, t), b2 = lerp(a2, a3, t1, t3, t);
      out.push_back(lerp(b1, b2, t1, t2, t));
    }
  }
  return out;
}

// ------------------------------------------------------------- geometry

double mark_text_width(const Mark &m) {
  double w, h;
  measure_text(m, w, h);
  return w;
}

double mark_text_height(const Mark &m) {
  double w, h;
  measure_text(m, w, h);
  return h;
}

Box mark_bounds(const Mark &m) {
  if (m.kind == MarkKind::Text) {
    Box b;
    if (m.pts.empty()) return b;
    double w, h, pad = text_pad(m);
    measure_text(m, w, h);
    return {m.pts[0].x - pad, m.pts[0].y - pad, m.pts[0].x + w + pad, m.pts[0].y + h + pad};
  }
  Box b = corners(m);
  if (!m.effect() && !m.pts.empty()) {
    double r = m.width / 2;
    if (m.kind == MarkKind::Arrow) r = std::max(r, std::max(m.width * 4, 12.0) * 0.5);
    b.x0 -= r;
    b.y0 -= r;
    b.x1 += r;
    b.y1 += r;
  }
  return b;
}

bool hit_mark(const Mark &m, Pt p, double tol) {
  if (m.pts.empty()) return false;
  double r = m.width / 2 + tol;
  switch (m.kind) {
    case MarkKind::Pen:
    case MarkKind::Highlighter: {
      if (m.pts.size() == 1) return dist(p, m.pts[0]) <= r;
      auto s = smooth_stroke(m.pts);
      for (size_t i = 0; i + 1 < s.size(); ++i)
        if (seg_dist(p, s[i], s[i + 1]) <= r) return true;
      return false;
    }
    case MarkKind::Line:
    case MarkKind::Arrow:
      return m.pts.size() >= 2 && seg_dist(p, m.pts[0], m.pts[1]) <= (m.kind == MarkKind::Arrow ? std::max(r, m.width * 2 + tol) : r);
    case MarkKind::Rect: {
      Box b = corners(m);
      if (m.fill) return b.contains(p, r);
      if (!b.contains(p, r)) return false;
      return !(p.x > b.x0 + r && p.x < b.x1 - r && p.y > b.y0 + r && p.y < b.y1 - r);
    }
    case MarkKind::Ellipse: {
      Box b = corners(m);
      double rx = std::max(0.5, b.w() / 2), ry = std::max(0.5, b.h() / 2);
      double dx = (p.x - (b.x0 + rx)) / rx, dy = (p.y - (b.y0 + ry)) / ry;
      double k = std::sqrt(dx * dx + dy * dy), mr = std::min(rx, ry);
      if (m.fill) return k <= 1 + r / mr;
      return std::fabs(k - 1) * mr <= r;
    }
    case MarkKind::Text:
    case MarkKind::Blur:
    case MarkKind::Pixelate:
      return mark_bounds(m).contains(p, tol);
  }
  return false;
}

int mark_at(const Marks &marks, Pt p, double tol) {
  for (size_t i = marks.items.size(); i-- > 0;)
    if (hit_mark(marks.items[i], p, tol)) return int(i);
  return -1;
}

void translate_mark(Mark &m, double dx, double dy) {
  for (Pt &p : m.pts) {
    p.x += dx;
    p.y += dy;
  }
}

void reshape_mark(Mark &m, const Box &from, const Box &to) {
  double sx = from.w() > 1e-9 ? to.w() / from.w() : 1, sy = from.h() > 1e-9 ? to.h() / from.h() : 1;
  if (m.kind == MarkKind::Text) {
    // Text keeps its proportions: the height decides the size.
    double k = std::max(0.05, sy);
    double pad = text_pad(m);
    m.size = std::max(4.0, m.size * k);
    double npad = text_pad(m);
    if (!m.pts.empty()) {
      m.pts[0].x = to.x0 + (m.pts[0].x - pad - from.x0) * k + npad;
      m.pts[0].y = to.y0 + (m.pts[0].y - pad - from.y0) * k + npad;
    }
    return;
  }
  for (Pt &p : m.pts) {
    p.x = to.x0 + (p.x - from.x0) * sx;
    p.y = to.y0 + (p.y - from.y0) * sy;
  }
}

void rotate_marks(Marks &marks, int quarter_turns) {
  int q = ((quarter_turns % 4) + 4) % 4;
  for (int i = 0; i < q; ++i) {
    double h = marks.h;
    for (Mark &m : marks.items) {
      if (m.kind == MarkKind::Text && !m.pts.empty()) {
        double tw, th;
        measure_text(m, tw, th);
        Pt c{m.pts[0].x + tw / 2, m.pts[0].y + th / 2};
        Pt n{h - c.y, c.x};
        m.pts[0] = {n.x - tw / 2, n.y - th / 2};
        continue;
      }
      for (Pt &p : m.pts) p = {h - p.y, p.x};
    }
    std::swap(marks.w, marks.h);
  }
}

void flip_marks(Marks &marks, bool horizontal) {
  double w = marks.w, h = marks.h;
  for (Mark &m : marks.items) {
    if (m.kind == MarkKind::Text && !m.pts.empty()) {
      double tw, th;
      measure_text(m, tw, th);
      Pt c{m.pts[0].x + tw / 2, m.pts[0].y + th / 2};
      if (horizontal) c.x = w - c.x;
      else c.y = h - c.y;
      m.pts[0] = {c.x - tw / 2, c.y - th / 2};
      continue;
    }
    for (Pt &p : m.pts) {
      if (horizontal) p.x = w - p.x;
      else p.y = h - p.y;
    }
  }
}

bool erase_at(Marks &marks, Pt p, double radius) {
  bool changed = false;
  std::vector<Mark> out;
  for (const Mark &m : marks.items) {
    if (!m.freehand()) {
      if (hit_mark(m, p, radius)) changed = true;
      else out.push_back(m);
      continue;
    }
    double r = radius + m.width / 2;
    bool touched = false;
    for (size_t i = 0; i < m.pts.size() && !touched; ++i)
      if (dist(m.pts[i], p) <= r || (i + 1 < m.pts.size() && seg_dist(p, m.pts[i], m.pts[i + 1]) <= r))
        touched = true;
    if (!touched) {
      out.push_back(m);
      continue;
    }
    changed = true;
    // Cut the stroke: keep the runs of points outside the circle.
    std::vector<std::vector<Pt>> pieces(1);
    for (size_t i = 0; i < m.pts.size(); ++i) {
      bool inside = dist(m.pts[i], p) <= r;
      if (!inside) pieces.back().push_back(m.pts[i]);
      else if (!pieces.back().empty()) pieces.emplace_back();
      if (!inside && i + 1 < m.pts.size() && dist(m.pts[i + 1], p) > r && seg_dist(p, m.pts[i], m.pts[i + 1]) <= r)
        pieces.emplace_back();  // the segment between passes through the eraser
    }
    for (auto &piece : pieces) {
      if (piece.size() < 2) continue;  // no stray dots
      Mark part = m;
      part.pts = std::move(piece);
      out.push_back(std::move(part));
    }
  }
  marks.items = std::move(out);
  return changed;
}

// --------------------------------------------------------------- effects

void pixelate_region(cairo_surface_t *s, int x, int y, int w, int h, int block) {
  if (!s || cairo_image_surface_get_format(s) != CAIRO_FORMAT_ARGB32) return;
  int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
  int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(sw, x + w), y1 = std::min(sh, y + h);
  if (x1 <= x0 || y1 <= y0) return;
  block = std::max(1, block);
  cairo_surface_flush(s);
  unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int by = y0; by < y1; by += block)
    for (int bx = x0; bx < x1; bx += block) {
      int ex = std::min(x1, bx + block), ey = std::min(y1, by + block);
      unsigned long long sum[4] = {0, 0, 0, 0}, n = 0;
      for (int yy = by; yy < ey; ++yy) {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(data + size_t(yy) * stride);
        for (int xx = bx; xx < ex; ++xx) {
          uint32_t px = row[xx];
          for (int c = 0; c < 4; ++c) sum[c] += (px >> (8 * c)) & 0xFF;
          ++n;
        }
      }
      uint32_t avg = 0;
      for (int c = 0; c < 4; ++c) avg |= uint32_t((sum[c] + n / 2) / n) << (8 * c);
      for (int yy = by; yy < ey; ++yy) {
        uint32_t *row = reinterpret_cast<uint32_t *>(data + size_t(yy) * stride);
        for (int xx = bx; xx < ex; ++xx) row[xx] = avg;
      }
    }
  cairo_surface_mark_dirty(s);
}

void blur_region(cairo_surface_t *s, int x, int y, int w, int h, int radius) {
  if (!s || cairo_image_surface_get_format(s) != CAIRO_FORMAT_ARGB32 || radius <= 0) return;
  int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
  int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(sw, x + w), y1 = std::min(sh, y + h);
  if (x1 <= x0 || y1 <= y0) return;
  // Work on the region plus a margin, so that the edges blend with what
  // is around them; only the region is written back.
  int m = radius * 3;
  int ex0 = std::max(0, x0 - m), ey0 = std::max(0, y0 - m), ex1 = std::min(sw, x1 + m), ey1 = std::min(sh, y1 + m);
  int bw = ex1 - ex0, bh = ey1 - ey0;
  cairo_surface_flush(s);
  unsigned char *data = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  std::vector<float> buf(size_t(bw) * bh * 4), tmp(buf.size());
  for (int yy = 0; yy < bh; ++yy) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(data + size_t(yy + ey0) * stride);
    for (int xx = 0; xx < bw; ++xx)
      for (int c = 0; c < 4; ++c) buf[(size_t(yy) * bw + xx) * 4 + c] = float((row[xx + ex0] >> (8 * c)) & 0xFF);
  }
  // Three box blurs approximate a Gaussian.
  auto pass = [&](std::vector<float> &src, std::vector<float> &dst, bool horizontal) {
    int len = horizontal ? bw : bh, lines = horizontal ? bh : bw;
    for (int l = 0; l < lines; ++l) {
      auto idx = [&](int i) {
        i = std::max(0, std::min(len - 1, i));
        return horizontal ? (size_t(l) * bw + i) * 4 : (size_t(i) * bw + l) * 4;
      };
      float acc[4] = {0, 0, 0, 0};
      for (int i = -radius; i <= radius; ++i)
        for (int c = 0; c < 4; ++c) acc[c] += src[idx(i) + c];
      float inv = 1.0f / float(2 * radius + 1);
      for (int i = 0; i < len; ++i) {
        size_t o = idx(i);
        for (int c = 0; c < 4; ++c) dst[o + c] = acc[c] * inv;
        size_t add = idx(i + radius + 1), sub = idx(i - radius);
        for (int c = 0; c < 4; ++c) acc[c] += src[add + c] - src[sub + c];
      }
    }
  };
  for (int k = 0; k < 3; ++k) {
    pass(buf, tmp, true);
    pass(tmp, buf, false);
  }
  for (int yy = y0; yy < y1; ++yy) {
    uint32_t *row = reinterpret_cast<uint32_t *>(data + size_t(yy) * stride);
    for (int xx = x0; xx < x1; ++xx) {
      const float *v = &buf[(size_t(yy - ey0) * bw + (xx - ex0)) * 4];
      uint32_t a = uint32_t(std::lround(std::max(0.0f, std::min(255.0f, v[3]))));
      uint32_t px = a << 24;
      for (int c = 0; c < 3; ++c)  // premultiplied: never above alpha
        px |= std::min(a, uint32_t(std::lround(std::max(0.0f, v[c])))) << (8 * c);
      row[xx] = px;
    }
  }
  cairo_surface_mark_dirty(s);
}

// --------------------------------------------------------------- drawing

void draw_mark(cairo_t *cr, const Mark &m, const MarkMap &map) {
  if (m.pts.empty()) return;
  cairo_save(cr);
  map.apply(cr);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr, std::max(0.5, m.width));
  set_color(cr, m.color, m.opacity);
  switch (m.kind) {
    case MarkKind::Pen:
    case MarkKind::Highlighter: {
      if (m.kind == MarkKind::Highlighter) {
        cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
      }
      if (m.pts.size() == 1) {
        cairo_arc(cr, m.pts[0].x, m.pts[0].y, m.width / 2, 0, 2 * M_PI);
        cairo_fill(cr);
        break;
      }
      auto s = smooth_stroke(m.pts, std::max(0.5, 1.5 / std::max(1e-3, map.scale())));
      cairo_move_to(cr, s[0].x, s[0].y);
      for (size_t i = 1; i < s.size(); ++i) cairo_line_to(cr, s[i].x, s[i].y);
      cairo_stroke(cr);
      break;
    }
    case MarkKind::Line:
    case MarkKind::Arrow: {
      if (m.pts.size() < 2) break;
      Pt a = m.pts[0], b = m.pts[1];
      if (m.kind == MarkKind::Arrow) {
        double len = dist(a, b);
        double head = std::min(std::max(m.width * 4, 12.0), len * 0.6);
        double ang = std::atan2(b.y - a.y, b.x - a.x), spread = 0.5;
        Pt l{b.x - head * std::cos(ang - spread), b.y - head * std::sin(ang - spread)};
        Pt r{b.x - head * std::cos(ang + spread), b.y - head * std::sin(ang + spread)};
        Pt base{b.x - head * 0.8 * std::cos(ang), b.y - head * 0.8 * std::sin(ang)};
        // One path: the shaft and the head do not overlap (no darker joint
        // with a translucent colour).
        cairo_push_group(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_move_to(cr, a.x, a.y);
        cairo_line_to(cr, base.x, base.y);
        cairo_stroke(cr);
        cairo_move_to(cr, b.x, b.y);
        cairo_line_to(cr, l.x, l.y);
        cairo_line_to(cr, r.x, r.y);
        cairo_close_path(cr);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, std::max(0.5, m.width * 0.5));
        cairo_stroke(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
      } else {
        cairo_move_to(cr, a.x, a.y);
        cairo_line_to(cr, b.x, b.y);
        cairo_stroke(cr);
      }
      break;
    }
    case MarkKind::Rect:
    case MarkKind::Ellipse: {
      if (m.pts.size() < 2) break;
      Box b = corners(m);
      if (m.kind == MarkKind::Rect) {
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
        cairo_rectangle(cr, b.x0, b.y0, b.w(), b.h());
      } else {
        cairo_save(cr);
        cairo_translate(cr, b.x0 + b.w() / 2, b.y0 + b.h() / 2);
        cairo_scale(cr, std::max(0.01, b.w() / 2), std::max(0.01, b.h() / 2));
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
        cairo_restore(cr);
      }
      if (m.fill) {
        cairo_push_group(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_fill_preserve(cr);
        cairo_stroke(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
      } else {
        cairo_stroke(cr);
      }
      break;
    }
    case MarkKind::Text: {
      if (m.text.empty()) break;
      double tw, th, pad = text_pad(m);
      measure_text(m, tw, th);
      Pt o = m.pts[0];
      if (m.box) {
        bool dark_text = luma_of(m.color) < 0.5;
        uint32_t bg = dark_text ? 0xFFFFFFFF : 0xFF111111;
        set_color(cr, bg, 0.85 * m.opacity);
        rounded(cr, o.x - pad, o.y - pad, tw + 2 * pad, th + 2 * pad, pad);
        cairo_fill(cr);
      }
      set_font(cr, m);
      cairo_font_extents_t fe;
      cairo_font_extents(cr, &fe);
      set_color(cr, m.color, m.opacity);
      double line_h = m.size * 1.25, yy = o.y + (line_h - (fe.ascent + fe.descent)) / 2 + fe.ascent;
      for (const auto &l : text_lines(m.text)) {
        cairo_move_to(cr, o.x, yy);
        cairo_show_text(cr, l.c_str());
        yy += line_h;
      }
      break;
    }
    case MarkKind::Blur:
    case MarkKind::Pixelate:
      break;  // pixel effects: see draw_marks()
  }
  cairo_restore(cr);
}

void draw_marks(cairo_t *cr, const Marks &marks, const MarkMap &map, bool vectors, bool effects,
                cairo_surface_t *target, int skip) {
  for (size_t i = 0; i < marks.items.size(); ++i) {
    if (int(i) == skip) continue;
    const Mark &m = marks.items[i];
    if (m.effect()) {
      if (!effects || !target) continue;
      int sw = cairo_image_surface_get_width(target), sh = cairo_image_surface_get_height(target);
      int x, y, w, h;
      if (!region_pixels(m, map, sw, sh, x, y, w, h)) continue;
      cairo_surface_flush(target);
      int amount = std::max(1, int(std::lround(m.width * map.scale())));
      if (m.kind == MarkKind::Pixelate) pixelate_region(target, x, y, w, h, std::max(2, amount));
      else blur_region(target, x, y, w, h, amount);
      continue;
    }
    if (vectors) draw_mark(cr, m, map);
  }
}

void flatten_marks(cairo_surface_t *s, const Edits &e, bool vectors, bool effects) {
  if (!s || e.marks.empty()) return;
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  cairo_t *cr = cairo_create(s);
  draw_marks(cr, e.marks, mark_map(e, w, h), vectors, effects, s);
  cairo_destroy(cr);
  cairo_surface_flush(s);
}

}  // namespace photo
