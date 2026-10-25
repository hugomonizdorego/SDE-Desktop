// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "logo_anim.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>

#include "config.h"
#include "effects.h"
#include "icons.h"
#include "legal.h"
#include "paint.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

constexpr double kFps = 30.0;
constexpr double kStripeSeconds = 0.42;  // one stripe fills in this time
constexpr double kCycle = 3.6;           // three stripes, then all lit for a moment

// The layers of the mark at one pixel size.
struct Layers {
  cairo_surface_t *under = nullptr;   // body, screen, labels
  cairo_surface_t *stripe[3] = {nullptr, nullptr, nullptr};
  double x0[3] = {0, 0, 0}, x1[3] = {0, 0, 0};  // horizontal extent of each stripe
  cairo_surface_t *led = nullptr;     // the green LED and its lens
  cairo_surface_t *ink = nullptr;     // the black outlines and portrait
  bool ok = false;
  ~Layers() {
    for (cairo_surface_t *s : {under, stripe[0], stripe[1], stripe[2], led, ink})
      if (s) cairo_surface_destroy(s);
  }
};

// <path id="NAME" .../> elements of the mark, by id, and the <svg> header.
struct MarkSource {
  bool loaded = false;
  std::string header;  // from <svg to the end of its start tag
  std::map<std::string, std::string> paths;
};

MarkSource &mark_source() {
  static MarkSource src;
  if (src.loaded) return src;
  src.loaded = true;
  std::string path = legal::branding_path("sde-mark"), text;
  if (!ends_with(path, ".svg") || !read_file(path, text)) return src;
  size_t svg = text.find("<svg");
  size_t svg_end = svg == std::string::npos ? svg : text.find('>', svg);
  if (svg_end == std::string::npos) return src;
  src.header = text.substr(svg, svg_end - svg + 1);
  size_t pos = 0;
  while ((pos = text.find("<path", pos)) != std::string::npos) {
    size_t end = text.find("/>", pos);
    if (end == std::string::npos) break;
    std::string el = text.substr(pos, end + 2 - pos);
    size_t id = el.find("id=\"");
    if (id != std::string::npos) {
      size_t q = el.find('"', id + 4);
      if (q != std::string::npos) src.paths[el.substr(id + 4, q - id - 4)] = el;
    }
    pos = end + 2;
  }
  return src;
}

cairo_surface_t *render(const MarkSource &src, const std::vector<std::string> &ids, int size) {
  std::string svg = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + src.header;
  for (auto &id : ids) {
    auto it = src.paths.find(id);
    if (it != src.paths.end()) svg += it->second;
  }
  svg += "</svg>";
  GError *err = nullptr;
  RsvgHandle *h = rsvg_handle_new_from_data(reinterpret_cast<const guint8 *>(svg.data()), svg.size(), &err);
  if (!h) {
    g_clear_error(&err);
    return nullptr;
  }
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(s);
  RsvgRectangle vp{0, 0, double(size), double(size)};
  bool ok = rsvg_handle_render_document(h, cr, &vp, &err);
  cairo_destroy(cr);
  g_object_unref(h);
  g_clear_error(&err);
  if (!ok) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  cairo_surface_flush(s);
  return s;
}

// Columns of |s| that have visible pixels.
void extent(cairo_surface_t *s, double *x0, double *x1) {
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  int stride = cairo_image_surface_get_stride(s);
  const unsigned char *data = cairo_image_surface_get_data(s);
  int lo = w, hi = -1;
  for (int y = 0; y < h; ++y) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * stride);
    for (int x = 0; x < w; ++x)
      if ((row[x] >> 24) > 16) {
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
  }
  *x0 = hi < 0 ? 0 : lo;
  *x1 = hi < 0 ? w : hi + 1;
}

const Layers *layers(int size) {
  static std::map<int, std::unique_ptr<Layers>> cache;
  auto it = cache.find(size);
  if (it != cache.end()) return it->second->ok ? it->second.get() : nullptr;
  auto l = std::make_unique<Layers>();
  const MarkSource &src = mark_source();
  if (src.paths.count("stripe-red") && src.paths.count("ink")) {
    l->under = render(src, {"base", "tone-1", "tone-2", "tone-3", "tone-4", "labels", "screen"}, size);
    const char *names[3] = {"stripe-red", "stripe-green", "stripe-blue"};
    for (int i = 0; i < 3; ++i) {
      l->stripe[i] = render(src, {names[i]}, size);
      if (l->stripe[i]) extent(l->stripe[i], &l->x0[i], &l->x1[i]);
    }
    l->led = render(src, {"led", "led-lens"}, size);
    l->ink = render(src, {"ink"}, size);
    l->ok = l->under && l->stripe[0] && l->stripe[1] && l->stripe[2] && l->ink;
  }
  // Cached at a few sizes only (the widgets have fixed sizes).
  if (cache.size() > 6) cache.clear();
  const Layers *out = l->ok ? l.get() : nullptr;
  cache[size] = std::move(l);
  return out;
}

double ease(double t) { return t <= 0 ? 0 : t >= 1 ? 1 : 1 - std::pow(1 - t, 3); }

}  // namespace

bool LogoAnimation::reduced_motion() {
  static int cached = -1;
  if (cached < 0) {
    Config cfg;
    cfg.load();
    cached = effects_from_config(cfg).on ? 0 : 1;
  }
  return cached == 1;
}

// MERKADU_LOGO_PHASE: a fixed moment of the animation, for screenshots.
static bool frozen() { return !env("MERKADU_LOGO_PHASE").empty(); }

LogoAnimation::LogoAnimation(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
  std::string phase = env("MERKADU_LOGO_PHASE");
  if (!phase.empty()) phase_ = std::atof(phase.c_str());
}

LogoAnimation::~LogoAnimation() { Fl::remove_timeout(tick, this); }

void LogoAnimation::running(bool on) {
  if (on == running_) return;
  running_ = on;
  Fl::remove_timeout(tick, this);
  if (on && !reduced_motion() && !frozen()) Fl::add_timeout(1.0 / kFps, tick, this);
  redraw();
}

void LogoAnimation::tick(void *data) {
  auto *self = static_cast<LogoAnimation *>(data);
  if (!self->running_) return;
  self->phase_ = std::fmod(self->phase_ + 1.0 / (kFps * kStripeSeconds), kCycle);
  // The LED: mostly on, off for a frame or two now and then.
  self->seed_ = self->seed_ * 1103515245u + 12345u;
  self->led_ = ((self->seed_ >> 16) % 7) == 0 ? 0.35 : 1.0;
  self->redraw();
  Fl::repeat_timeout(1.0 / kFps, tick, data);
}

void LogoAnimation::draw() {
  int size = std::min(w(), h());
  double ox = x() + (w() - size) / 2.0, oy = y() + (h() - size) / 2.0;
  Painter p(window()->w(), window()->h());
  const Layers *l = layers(size);
  if (!l) {
    // No mark installed: the logo icon, still.
    p.image(IconLoader::get({"sde-logo", "start-here"}, size), ox, oy, size);
    return;
  }
  bool still = !running_ || (reduced_motion() && !frozen());
  cairo_t *cr = p.cr();
  p.surface(l->under, ox, oy);
  for (int i = 0; i < 3; ++i) {
    // Lit: stripes already filled in this cycle, or everything when still.
    double fill;  // 0..1 of the stripe that is lit from the left
    if (still || phase_ >= 3.0)
      fill = 1;
    else if (phase_ >= i + 1)
      fill = 1;
    else if (phase_ < i)
      fill = 0;
    else
      fill = ease(phase_ - i);
    double x0 = l->x0[i], x1 = l->x1[i];
    if (fill < 1) p.surface(l->stripe[i], ox, oy, 0.22);
    if (fill > 0) {
      cairo_save(cr);
      cairo_rectangle(cr, ox + x0, oy, (x1 - x0) * fill, size);
      cairo_clip(cr);
      cairo_set_source_surface(cr, p.server_copy(l->stripe[i]), ox, oy);
      cairo_paint(cr);
      cairo_restore(cr);
    }
  }
  if (l->led) p.surface(l->led, ox, oy, still ? 1.0 : led_);
  p.surface(l->ink, ox, oy);
}

}  // namespace merkadu
}  // namespace sde
