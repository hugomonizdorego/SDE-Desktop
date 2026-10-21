// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "edit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>

#include "thumbnails.h"

namespace photo {

namespace {

double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

}  // namespace

bool Adjust::identity() const {
  return exposure == 0 && brightness == 0 && contrast == 0 && saturation == 0 && warmth == 0 &&
         fade == 0 && vignette == 0;
}

bool Adjust::operator==(const Adjust &o) const {
  return exposure == o.exposure && brightness == o.brightness && contrast == o.contrast &&
         saturation == o.saturation && warmth == o.warmth && fade == o.fade && vignette == o.vignette;
}

const std::vector<FilterInfo> &filters() {
  static const std::vector<FilterInfo> list{
      {Filter::Original, "original", "Original"}, {Filter::Vivid, "vivid", "Vivid"},
      {Filter::Warm, "warm", "Warm"},         {Filter::Cool, "cool", "Cool"},
      {Filter::Mono, "mono", "B&W"},          {Filter::Noir, "noir", "Noir"},
      {Filter::Sepia, "sepia", "Sepia"},      {Filter::Vintage, "vintage", "Vintage"},
      {Filter::Matte, "matte", "Matte"},      {Filter::Invert, "invert", "Invert"}};
  return list;
}

const char *filter_key(Filter f) {
  for (const auto &i : filters())
    if (i.id == f) return i.key;
  return "original";
}

bool parse_filter(const std::string &key, Filter &out) {
  for (const auto &i : filters())
    if (key == i.key) {
      out = i.id;
      return true;
    }
  return false;
}

Adjust filter_preset(Filter f) {
  Adjust a;
  switch (f) {
    case Filter::Vivid: a.brightness = 4; a.contrast = 18; a.saturation = 45; a.warmth = 6; a.vignette = 10; break;
    case Filter::Warm: a.brightness = 4; a.contrast = 6; a.saturation = 10; a.warmth = 45; a.fade = 6; break;
    case Filter::Cool: a.contrast = 8; a.saturation = -5; a.warmth = -45; break;
    case Filter::Mono: a.contrast = 12; break;
    case Filter::Noir: a.brightness = -6; a.contrast = 45; a.vignette = 35; break;
    case Filter::Sepia: a.brightness = 4; a.contrast = 4; a.fade = 10; a.vignette = 15; break;
    case Filter::Vintage: a.brightness = 6; a.contrast = -12; a.saturation = -20; a.warmth = 30; a.fade = 35; a.vignette = 40; break;
    case Filter::Matte: a.brightness = 6; a.contrast = -18; a.saturation = -10; a.warmth = 8; a.fade = 45; break;
    case Filter::Original:
    case Filter::Invert: break;
  }
  return a;
}

bool Edits::identity() const { return geometry_only() && orient == gallery::Orient(); }

bool Edits::geometry_only() const {
  return crop.full() && resize_w == 0 && resize_h == 0 && tone_identity();
}

bool Edits::operator==(const Edits &o) const {
  return orient == o.orient && crop == o.crop && resize_w == o.resize_w && resize_h == o.resize_h &&
         adjust == o.adjust && filter == o.filter && strength == o.strength;
}

double srgb_to_linear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double c) {
  if (c <= 0) return 0;
  return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

double luma(const RGB &c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

RGB adjust_pixel(RGB c, const Adjust &a) {
  if (a.exposure != 0) {
    double k = std::pow(2.0, a.exposure / 50.0);
    c.r = linear_to_srgb(srgb_to_linear(c.r) * k);
    c.g = linear_to_srgb(srgb_to_linear(c.g) * k);
    c.b = linear_to_srgb(srgb_to_linear(c.b) * k);
  }
  if (a.brightness != 0) {
    // Lifts (or lowers) the midtones and keeps white (or black) in place.
    double b = a.brightness / 100.0 * 0.4;
    auto f = [b](double v) { v = clamp01(v); return b > 0 ? v + b * (1 - v) : v + b * v; };
    c = {f(c.r), f(c.g), f(c.b)};
  }
  if (a.contrast != 0) {
    double k = a.contrast > 0 ? 1 + a.contrast / 100.0 : 1 + a.contrast / 100.0 * 0.8;
    auto f = [k](double v) { return (clamp01(v) - 0.5) * k + 0.5; };
    c = {f(c.r), f(c.g), f(c.b)};
  }
  if (a.saturation != 0) {
    double y = luma(c), k = 1 + a.saturation / 100.0;
    c = {y + (c.r - y) * k, y + (c.g - y) * k, y + (c.b - y) * k};
  }
  if (a.warmth != 0) {
    double w = a.warmth / 100.0;
    c.r *= 1 + 0.12 * w;
    c.g *= 1 + 0.02 * w;
    c.b *= 1 - 0.14 * w;
  }
  if (a.fade > 0) {
    double f = a.fade / 100.0;
    auto g = [f](double v) { return clamp01(v) * (1 - 0.2 * f) + 0.12 * f; };
    c = {g(c.r), g(c.g), g(c.b)};
  }
  return {clamp01(c.r), clamp01(c.g), clamp01(c.b)};
}

RGB filter_pixel(RGB c, Filter f, double s) {
  RGB m = c;
  switch (f) {
    case Filter::Mono:
    case Filter::Noir: {
      double y = luma(c);
      m = {y, y, y};
      break;
    }
    case Filter::Sepia:
    case Filter::Vintage: {
      m.r = 0.393 * c.r + 0.769 * c.g + 0.189 * c.b;
      m.g = 0.349 * c.r + 0.686 * c.g + 0.168 * c.b;
      m.b = 0.272 * c.r + 0.534 * c.g + 0.131 * c.b;
      if (f == Filter::Vintage) s *= 0.5;
      break;
    }
    case Filter::Invert: m = {1 - c.r, 1 - c.g, 1 - c.b}; break;
    default: return c;
  }
  s = clamp01(s);
  return {clamp01(c.r + (m.r - c.r) * s), clamp01(c.g + (m.g - c.g) * s), clamp01(c.b + (m.b - c.b) * s)};
}

double vignette_factor(double r, double amount) {
  if (amount <= 0) return 1;
  double t = clamp01((r - 0.35) / 0.65);
  t = t * t * (3 - 2 * t);
  return 1 - std::min(1.0, amount / 100.0) * 0.65 * t;
}

void crop_pixels(const Rect &r, int w, int h, int &cx, int &cy, int &cw, int &ch) {
  double x0 = std::max(0.0, std::min(1.0, r.x)), y0 = std::max(0.0, std::min(1.0, r.y));
  double x1 = std::max(x0, std::min(1.0, r.x + r.w)), y1 = std::max(y0, std::min(1.0, r.y + r.h));
  cx = int(std::lround(x0 * w));
  cy = int(std::lround(y0 * h));
  cw = std::max(1, int(std::lround(x1 * w)) - cx);
  ch = std::max(1, int(std::lround(y1 * h)) - cy);
  if (cx + cw > w) cx = std::max(0, w - cw);
  if (cy + ch > h) cy = std::max(0, h - ch);
}

void result_size(const Edits &e, int w, int h, int &ow, int &oh) {
  if (e.orient.k % 2) std::swap(w, h);
  int cx, cy;
  crop_pixels(e.crop, w, h, cx, cy, ow, oh);
  if (e.resize_w > 0 && e.resize_h > 0) {
    ow = e.resize_w;
    oh = e.resize_h;
  }
}

cairo_surface_t *apply_geometry(cairo_surface_t *src, const Edits &e, double scale) {
  cairo_surface_t *o = gallery::orient_surface(cairo_surface_reference(src), gallery::orient_to_exif(e.orient));
  int w = cairo_image_surface_get_width(o), h = cairo_image_surface_get_height(o);
  int cx, cy, cw, ch;
  crop_pixels(e.crop, w, h, cx, cy, cw, ch);
  int tw = cw, th = ch;
  if (e.resize_w > 0 && e.resize_h > 0) {
    // The crop rectangle is relative to the full image: when previewing a
    // downscaled original, scale the requested size likewise.
    tw = std::max(1, int(std::lround(e.resize_w * scale)));
    th = std::max(1, int(std::lround(e.resize_h * scale)));
  }
  if (cx == 0 && cy == 0 && cw == w && ch == h && tw == w && th == h) return o;
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
  cairo_t *cr = cairo_create(out);
  cairo_scale(cr, double(tw) / cw, double(th) / ch);
  cairo_set_source_surface(cr, o, -cx, -cy);
  cairo_pattern_set_filter(cairo_get_source(cr), tw < cw ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BEST);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_rectangle(cr, 0, 0, cw, ch);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(o);
  return out;
}

void apply_tone(cairo_surface_t *s, const Edits &e) {
  if (e.tone_identity()) return;
  Adjust a = e.adjust;
  double st = e.filter == Filter::Original ? 0 : e.strength / 100.0;
  Adjust p = filter_preset(e.filter);
  a.exposure += p.exposure * st;
  a.brightness += p.brightness * st;
  a.contrast += p.contrast * st;
  a.saturation += p.saturation * st;
  a.warmth += p.warmth * st;
  a.fade += p.fade * st;
  a.vignette += p.vignette * st;
  double vignette = a.vignette;
  a.vignette = 0;
  cairo_surface_flush(s);
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  int stride = cairo_image_surface_get_stride(s);
  unsigned char *data = cairo_image_surface_get_data(s);
  // Per-channel steps (exposure, brightness, contrast) through a table when
  // nothing else is needed would be faster; the full pipeline per pixel is
  // simple and fast enough with a few threads.
  auto work = [&](int y0, int y1) {
    double cxm = w / 2.0, cym = h / 2.0, norm = std::sqrt(cxm * cxm + cym * cym);
    for (int y = y0; y < y1; ++y) {
      uint32_t *row = reinterpret_cast<uint32_t *>(data + size_t(y) * stride);
      for (int x = 0; x < w; ++x) {
        uint32_t px = row[x];
        unsigned al = px >> 24;
        if (!al) continue;
        double inv = 255.0 * al;
        RGB c{((px >> 16) & 0xFF) / inv * 255, ((px >> 8) & 0xFF) / inv * 255, (px & 0xFF) / inv * 255};
        c = adjust_pixel(c, a);
        c = filter_pixel(c, e.filter, st);
        if (vignette > 0) {
          double dx = x - cxm, dy = y - cym;
          double k = vignette_factor(std::sqrt(dx * dx + dy * dy) / norm, vignette);
          c = {c.r * k, c.g * k, c.b * k};
        }
        auto q = [al](double v) { return unsigned(std::lround(clamp01(v) * al)); };
        row[x] = (al << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
      }
    }
  };
  long long pixels = (long long)w * h;
  int threads = pixels > 400000 ? int(std::max(1u, std::min(8u, std::thread::hardware_concurrency()))) : 1;
  if (threads <= 1) {
    work(0, h);
  } else {
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) pool.emplace_back(work, h * i / threads, h * (i + 1) / threads);
    for (auto &t : pool) t.join();
  }
  cairo_surface_mark_dirty(s);
}

cairo_surface_t *render(cairo_surface_t *src, const Edits &e, double scale) {
  cairo_surface_t *g = apply_geometry(src, e, scale);
  if (!e.tone_identity()) {
    if (g == src) {
      // Never modify the original: work on a copy.
      cairo_surface_destroy(g);
      int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
      g = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
      cairo_t *cr = cairo_create(g);
      cairo_set_source_surface(cr, src, 0, 0);
      cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
      cairo_paint(cr);
      cairo_destroy(cr);
    }
    apply_tone(g, e);
  }
  return g;
}

cairo_surface_t *downscale(cairo_surface_t *src, int max_dim) {
  int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
  if (max_dim <= 0 || (w <= max_dim && h <= max_dim)) return cairo_surface_reference(src);
  double sc = double(max_dim) / std::max(w, h);
  int tw = std::max(1, int(std::lround(w * sc))), th = std::max(1, int(std::lround(h * sc)));
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
  cairo_t *cr = cairo_create(out);
  cairo_scale(cr, double(tw) / w, double(th) / h);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  return out;
}

// ------------------------------------------------------------------ history

void History::reset(const Edits &e) {
  states_.assign(1, e);
  pos_ = 0;
}

void History::push(const Edits &e) {
  if (e == states_[pos_]) return;
  states_.resize(pos_ + 1);
  states_.push_back(e);
  pos_ = states_.size() - 1;
  if (states_.size() > 200) {
    states_.erase(states_.begin());
    --pos_;
  }
}

void History::amend(const Edits &e) { states_[pos_] = e; }

const Edits &History::undo() {
  if (pos_ > 0) --pos_;
  return states_[pos_];
}

const Edits &History::redo() {
  if (pos_ + 1 < states_.size()) ++pos_;
  return states_[pos_];
}

}  // namespace photo
