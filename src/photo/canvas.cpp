// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The image view of Photo.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "photo.h"

using namespace sde;
namespace icon = gallery::icon;

namespace photo {

namespace {
constexpr double kMinScale = 0.01, kMaxScale = 32;
}

Canvas::Canvas(PhotoWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

Canvas::~Canvas() {
  Fl::remove_timeout(anim_cb, this);
  stop_animation();
  drop_caches();
  if (image_) cairo_surface_destroy(image_);
}

void Canvas::drop_caches() {
  if (mip_) cairo_surface_destroy(mip_);
  if (server_) cairo_surface_destroy(server_);
  mip_ = server_ = nullptr;
}

void Canvas::resize(int x, int y, int w, int h) {
  bool grew = w > this->w() || h > this->h();
  Fl_Widget::resize(x, y, w, h);
  if (grew) drop_caches();
  clamp_pan();
}

void Canvas::set_image(cairo_surface_t *s, int full_w, int full_h, bool keep_view) {
  if (s) cairo_surface_reference(s);
  if (image_) cairo_surface_destroy(image_);
  image_ = s;
  drop_caches();
  bool same_size = full_w == full_w_ && full_h == full_h_;
  full_w_ = full_w;
  full_h_ = full_h;
  if (!keep_view || !same_size) {
    mode_ = cropping_ ? ZoomMode::Fit : mode_ == ZoomMode::Custom ? ZoomMode::Fit : mode_;
    pan_x_ = pan_y_ = 0;
    shown_scale_ = target_scale();
  }
  clamp_pan();
  redraw();
}

void Canvas::clear() {
  stop_animation();
  if (image_) cairo_surface_destroy(image_);
  image_ = nullptr;
  drop_caches();
  full_w_ = full_h_ = 0;
  redraw();
}

int Canvas::filmstrip_height() const {
  return filmstrip_ && chrome_ && app_->photos().size() > 1 ? S(78) : 0;
}

void Canvas::set_filmstrip(bool on) {
  filmstrip_ = on;
  clamp_pan();
  redraw();
}

void Canvas::image_area(double &ax, double &ay, double &aw, double &ah) const {
  ax = x();
  ay = y();
  aw = w();
  ah = h() - filmstrip_height();
}

double Canvas::fit_scale(bool fill) const {
  if (full_w_ <= 0 || full_h_ <= 0) return 1;
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  double margin = chrome_ ? S(24) : 0;
  if (cropping_) margin = S(48);
  double sx = (aw - margin) / full_w_, sy = (ah - margin) / full_h_;
  if (fill) return std::max(sx, sy);
  // Small pictures are not enlarged to fit (vector art is decoded large).
  return std::max(kMinScale, std::min({sx, sy, 1.0}));
}

double Canvas::target_scale() const {
  switch (mode_) {
    case ZoomMode::Fit: return fit_scale(false);
    case ZoomMode::Fill: return fit_scale(true);
    case ZoomMode::Actual: return 1.0;
    case ZoomMode::Custom: return custom_;
  }
  return 1.0;
}

double Canvas::scale() const {
  // Fit and Fill follow the size of the view (the filmstrip may appear later).
  if (!animating_ && (mode_ == ZoomMode::Fit || mode_ == ZoomMode::Fill || shown_scale_ <= 0)) return target_scale();
  return shown_scale_;
}

void Canvas::clamp_pan() {
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  double s = scale();
  double mx = std::max(0.0, (full_w_ * s - aw) / 2), my = std::max(0.0, (full_h_ * s - ah) / 2);
  pan_x_ = std::max(-mx, std::min(mx, pan_x_));
  pan_y_ = std::max(-my, std::min(my, pan_y_));
}

void Canvas::zoom_at(double ns, double cx, double cy, bool animate) {
  if (!animating_) shown_scale_ = scale();
  ns = std::max(kMinScale, std::min(kMaxScale, ns));
  anchor_x_ = cx;
  anchor_y_ = cy;
  double fit = fit_scale(false);
  if (std::fabs(ns - fit) / fit < 0.02) {
    mode_ = ZoomMode::Fit;
  } else if (std::fabs(ns - 1) < 0.005) {
    mode_ = ZoomMode::Actual;
  } else {
    mode_ = ZoomMode::Custom;
    custom_ = ns;
  }
  if (!animate) {
    // Keep the image point under the anchor in place.
    double ax, ay, aw, ah;
    image_area(ax, ay, aw, ah);
    double s = scale();
    double px = (cx - (ax + aw / 2 + pan_x_)) / s, py = (cy - (ay + ah / 2 + pan_y_)) / s;
    shown_scale_ = target_scale();
    pan_x_ = cx - ax - aw / 2 - px * shown_scale_;
    pan_y_ = cy - ay - ah / 2 - py * shown_scale_;
    if (mode_ == ZoomMode::Fit || mode_ == ZoomMode::Fill) pan_x_ = pan_y_ = 0;
    clamp_pan();
    redraw();
    app_->update_title();
    return;
  }
  if (!animating_) {
    animating_ = true;
    Fl::add_timeout(1.0 / 60, anim_cb, this);
  }
}

void Canvas::anim_cb(void *p) {
  auto *c = static_cast<Canvas *>(p);
  double target = c->target_scale(), cur = c->scale();
  double ax, ay, aw, ah;
  c->image_area(ax, ay, aw, ah);
  double px = (c->anchor_x_ - (ax + aw / 2 + c->pan_x_)) / cur;
  double py = (c->anchor_y_ - (ay + ah / 2 + c->pan_y_)) / cur;
  double next;
  bool done = std::fabs(std::log(target / cur)) < 0.01;
  if (done) next = target;
  else next = std::exp(std::log(cur) + (std::log(target) - std::log(cur)) * 0.35);
  c->shown_scale_ = next;
  c->pan_x_ = c->anchor_x_ - ax - aw / 2 - px * next;
  c->pan_y_ = c->anchor_y_ - ay - ah / 2 - py * next;
  if (done && (c->mode_ == ZoomMode::Fit || c->mode_ == ZoomMode::Fill)) c->pan_x_ = c->pan_y_ = 0;
  c->clamp_pan();
  c->redraw();
  if (done) {
    c->animating_ = false;
    c->app_->update_title();
  } else {
    Fl::repeat_timeout(1.0 / 60, anim_cb, p);
  }
}

void Canvas::set_zoom_mode(ZoomMode m) {
  if (!animating_) shown_scale_ = scale();
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  anchor_x_ = ax + aw / 2;
  anchor_y_ = ay + ah / 2;
  mode_ = m;
  if (m == ZoomMode::Fit || m == ZoomMode::Fill) {
    // Recentre while animating.
    pan_x_ = pan_y_ = 0;
  }
  if (!animating_) {
    animating_ = true;
    Fl::add_timeout(1.0 / 60, anim_cb, this);
  }
}

void Canvas::zoom_by(double f) {
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  zoom_at(target_scale() * f, ax + aw / 2, ay + ah / 2, true);
}

void Canvas::image_rect(double &ix, double &iy, double &iw, double &ih) const {
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  double s = scale();
  iw = full_w_ * s;
  ih = full_h_ * s;
  ix = ax + aw / 2 + pan_x_ - iw / 2;
  iy = ay + ah / 2 + pan_y_ - ih / 2;
}

// ------------------------------------------------------------- animation

void Canvas::set_animation(std::vector<std::pair<cairo_surface_t *, int>> frames) {
  stop_animation();
  frames_ = std::move(frames);
  frame_ = 0;
  if (frames_.size() > 1) Fl::add_timeout(std::max(20, frames_[0].second) / 1000.0, frame_cb, this);
}

void Canvas::stop_animation() {
  Fl::remove_timeout(frame_cb, this);
  for (auto &f : frames_) cairo_surface_destroy(f.first);
  frames_.clear();
}

void Canvas::frame_cb(void *p) {
  auto *c = static_cast<Canvas *>(p);
  if (c->frames_.empty()) return;
  c->frame_ = (c->frame_ + 1) % c->frames_.size();
  cairo_surface_t *f = c->frames_[c->frame_].first;
  cairo_surface_reference(f);
  if (c->image_) cairo_surface_destroy(c->image_);
  c->image_ = f;
  c->drop_caches();
  c->redraw();
  Fl::repeat_timeout(std::max(20, c->frames_[c->frame_].second) / 1000.0, frame_cb, p);
}

// ------------------------------------------------------------------ crop

void Canvas::begin_crop(const Rect &r, double aspect) {
  cropping_ = true;
  crop_ = r;
  aspect_ = aspect;
  mode_ = ZoomMode::Fit;
  pan_x_ = pan_y_ = 0;
  shown_scale_ = target_scale();
  redraw();
}

void Canvas::set_crop_aspect(double aspect) {
  aspect_ = aspect;
  if (aspect > 0 && full_w_ > 0 && full_h_ > 0) {
    // Largest centred rectangle of that aspect inside the current one.
    double cw = crop_.w * full_w_, ch = crop_.h * full_h_;
    double nw = cw, nh = cw / aspect;
    if (nh > ch) {
      nh = ch;
      nw = ch * aspect;
    }
    double cx = crop_.x + crop_.w / 2, cy = crop_.y + crop_.h / 2;
    crop_.w = nw / full_w_;
    crop_.h = nh / full_h_;
    crop_.x = std::max(0.0, std::min(1 - crop_.w, cx - crop_.w / 2));
    crop_.y = std::max(0.0, std::min(1 - crop_.h, cy - crop_.h / 2));
  }
  redraw();
}

void Canvas::end_crop() {
  cropping_ = false;
  crop_drag_ = -1;
  redraw();
}

int Canvas::crop_handle_at(int mx, int my) const {
  double ix, iy, iw, ih;
  image_rect(ix, iy, iw, ih);
  double x0 = ix + crop_.x * iw, y0 = iy + crop_.y * ih, x1 = x0 + crop_.w * iw, y1 = y0 + crop_.h * ih;
  double r = S(14);
  struct P {
    double x, y;
  } pts[8] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {(x0 + x1) / 2, y0}, {x1, (y0 + y1) / 2},
              {(x0 + x1) / 2, y1}, {x0, (y0 + y1) / 2}};
  for (int i = 0; i < 8; ++i)
    if (std::fabs(mx - pts[i].x) <= r && std::fabs(my - pts[i].y) <= r) return i;
  if (mx > x0 && mx < x1 && my > y0 && my < y1) return 8;
  return -1;
}

// ------------------------------------------------------------------ draw

void Canvas::draw_filmstrip(Painter &p) {
  const Theme &t = app_->theme();
  const auto &photos = app_->photos();
  int fh = filmstrip_height();
  if (!fh) return;
  double fy = y() + h() - fh;
  p.fill_rect(x(), fy, w(), fh, t.surface);
  p.line(x(), fy + 0.5, x() + w(), fy + 0.5, t.border);
  double ts = S(56), gap = S(6), step = ts + gap;
  int cur = app_->index();
  // Keep the current photo centred.
  double centre = x() + w() / 2.0;
  double first_x = centre - ts / 2 - cur * step;
  int lo = std::max(0, int((x() - first_x) / step) - 1);
  int hi = std::min(int(photos.size()) - 1, int((x() + w() - first_x) / step) + 1);
  std::vector<const gallery::Photo *> want;
  cairo_t *cr = p.cr();
  for (int i = lo; i <= hi; ++i) {
    double tx = first_x + i * step, ty = fy + (fh - ts) / 2;
    const auto &ph = photos[size_t(i)];
    cairo_surface_t *s = app_->thumbnail(ph);
    if (!s) want.push_back(&ph);
    cairo_save(cr);
    p.rounded_rect(tx, ty, ts, ts, Sd(6));
    cairo_clip(cr);
    p.fill_rect(tx, ty, ts, ts, t.surface_alt);
    if (s) {
      double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
      double sc = std::max(ts / sw, ts / sh);
      cairo_translate(cr, tx + (ts - sw * sc) / 2, ty + (ts - sh * sc) / 2);
      cairo_scale(cr, sc, sc);
      cairo_set_source_surface(cr, s, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
      cairo_paint(cr);
    } else {
      icon::draw(p, icon::Picture, tx + ts * 0.3, ty + ts * 0.3, ts * 0.4, t.text_dim.alpha(0.5));
    }
    cairo_restore(cr);
    if (i != cur) {
      if (i != film_hover_) p.fill_rounded(tx, ty, ts, ts, Sd(6), Color::hex(0x000000, t.dark ? 0.35 : 0.18));
    } else {
      p.stroke_rounded(tx - Sd(2), ty - Sd(2), ts + Sd(4), ts + Sd(4), Sd(8), t.accent, Sd(2.5));
    }
  }
  app_->request_thumbnails(want);
}

int Canvas::filmstrip_at(int mx, int my) const {
  int fh = filmstrip_height();
  if (!fh || my < y() + h() - fh) return -2;  // not on the strip
  double ts = S(56), gap = S(6), step = ts + gap;
  double first_x = x() + w() / 2.0 - ts / 2 - app_->index() * step;
  int i = int(std::floor((mx - first_x) / step));
  if (i < 0 || i >= int(app_->photos().size())) return -1;
  if (mx - (first_x + i * step) > ts) return -1;
  return i;
}

void Canvas::draw() {
  const Theme &t = app_->theme();
  bool dark_stage = !chrome_;
  Color bg = dark_stage ? Color::hex(0x000000)
             : t.dark   ? t.background.mix(Color::hex(0x000000), 0.35)
                        : t.surface_alt.mix(t.background, 0.4);
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  std::vector<std::pair<std::string, int>> messages;
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), bg);
    cairo_save(cr);
    cairo_rectangle(cr, ax, ay, aw, ah);
    cairo_clip(cr);
    if (image_ && full_w_ > 0) {
      double ix, iy, iw, ih;
      image_rect(ix, iy, iw, ih);
      if (chrome_) p.shadow(ix, iy, iw, ih, 1, t.shadow.alpha(t.shadow.a * 0.7), S(10));
      // A neutral card under transparent images.
      p.fill_rect(ix, iy, iw, ih, t.dark ? Color::hex(0x2A313B) : Color::hex(0xFFFFFF));
      int sw = cairo_image_surface_get_width(image_), sh = cairo_image_surface_get_height(image_);
      // Pick the screen-sized copy when it has enough pixels.
      int limit = int(std::max(aw, ah) * 1.5);
      if (!mip_ && (sw > limit || sh > limit)) mip_ = downscale(image_, limit);
      cairo_surface_t *src = image_;
      if (mip_ && iw <= cairo_image_surface_get_width(mip_) * 1.02) {
        if (!server_) {
          server_ = cairo_surface_create_similar(cairo_get_target(cr), CAIRO_CONTENT_COLOR_ALPHA,
                                                 cairo_image_surface_get_width(mip_),
                                                 cairo_image_surface_get_height(mip_));
          cairo_t *c2 = cairo_create(server_);
          cairo_set_source_surface(c2, mip_, 0, 0);
          cairo_set_operator(c2, CAIRO_OPERATOR_SOURCE);
          cairo_paint(c2);
          cairo_destroy(c2);
        }
        src = server_;
        sw = cairo_image_surface_get_width(mip_);
        sh = cairo_image_surface_get_height(mip_);
      }
      cairo_save(cr);
      cairo_rectangle(cr, ix, iy, iw, ih);
      cairo_clip(cr);
      cairo_translate(cr, ix, iy);
      double k = iw / sw;
      cairo_scale(cr, k, ih / sh);
      cairo_set_source_surface(cr, src, 0, 0);
      cairo_filter_t filter = k >= 3 ? CAIRO_FILTER_NEAREST : animating_ ? CAIRO_FILTER_FAST : CAIRO_FILTER_GOOD;
      cairo_pattern_set_filter(cairo_get_source(cr), filter);
      cairo_paint(cr);
      cairo_restore(cr);
      // Drawings: a layer above the photo until it is saved.
      draw_overlay(p);
      if (cropping_) {
        double x0 = ix + crop_.x * iw, y0 = iy + crop_.y * ih, cw = crop_.w * iw, ch = crop_.h * ih;
        // Dim outside the selection.
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(cr, ix, iy, iw, ih);
        cairo_rectangle(cr, x0, y0, cw, ch);
        p.set(Color::hex(0x000000, 0.55));
        cairo_fill(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
        // Rule of thirds.
        for (int i = 1; i < 3; ++i) {
          p.line(x0 + cw * i / 3, y0, x0 + cw * i / 3, y0 + ch, Color::hex(0xFFFFFF, 0.45));
          p.line(x0, y0 + ch * i / 3, x0 + cw, y0 + ch * i / 3, Color::hex(0xFFFFFF, 0.45));
        }
        p.stroke_rounded(x0 + 0.5, y0 + 0.5, cw - 1, ch - 1, 0, Color::hex(0xFFFFFF), Sd(1.5));
        double hs = S(10);
        double pts[8][2] = {{x0, y0}, {x0 + cw, y0}, {x0 + cw, y0 + ch}, {x0, y0 + ch},
                            {x0 + cw / 2, y0}, {x0 + cw, y0 + ch / 2}, {x0 + cw / 2, y0 + ch}, {x0, y0 + ch / 2}};
        for (int i = 0; i < 8; ++i) {
          p.circle(pts[i][0], pts[i][1], hs / 2 + Sd(1.5), Color::hex(0x000000, 0.35));
          p.circle(pts[i][0], pts[i][1], hs / 2, i < 4 ? Color::hex(0xFFFFFF) : t.accent);
        }
        int pw = 0, ph = 0, cxp, cyp;
        crop_pixels(crop_, full_w_, full_h_, cxp, cyp, pw, ph);
        messages.push_back({format("%d × %d", pw, ph), int(y0 + ch + S(8))});
      }
    } else {
      double gs = S(64);
      icon::draw(p, app_->load_failed() ? icon::Broken : icon::Picture, ax + (aw - gs) / 2, ay + (ah - gs) / 2 - S(16),
                 gs, (dark_stage ? Color::hex(0xFFFFFF) : t.text_dim).alpha(0.6));
    }
    cairo_restore(cr);
    draw_filmstrip(p);
    cairo_restore(cr);
  }
  if (!image_ && app_->current()) {
    text_font(t.font_size);
    const char *msg = app_->load_failed() ? _("This image cannot be displayed") : _("Loading…");
    draw_text(msg, int(ax), int(ay + ah / 2 + S(34)), int(aw), S(24), dark_stage ? Color::hex(0xFFFFFF) : t.text_dim, 1);
  } else if (!app_->current()) {
    text_font(t.font_size + S(2), true);
    draw_text(_("No photo open"), int(ax), int(ay + ah / 2 + S(34)), int(aw), S(26), t.text, 1);
    text_font(t.font_size);
    draw_text(_("Press Ctrl+O to open a photo, or open one from Gallery."), int(ax), int(ay + ah / 2 + S(62)),
              int(aw), S(22), t.text_dim, 1);
  }
  for (const auto &m : messages) {
    text_font(t.font_size - S(1), true);
    int tw = text_width(m.first) + S(20);
    int mx = int(ax + aw / 2 - tw / 2), my = std::min(m.second, int(ay + ah) - S(30));
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(mx, my, tw, S(24), S(12), Color::hex(0x000000, 0.65));
    }
    draw_text(m.first, mx, my, tw, S(24), Color::hex(0xFFFFFF), 1);
  }
}

// ---------------------------------------------------------------- events

int Canvas::handle(int event) {
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  int mx = Fl::event_x(), my = Fl::event_y();
  if (app_->drawing() && chrome_ && (event == FL_PUSH || event == FL_DRAG || event == FL_RELEASE ||
                                      event == FL_MOVE || event == FL_ENTER)) {
    bool strip = filmstrip_at(mx, my) != -2 && event != FL_DRAG && event != FL_RELEASE;
    if (event == FL_PUSH && Fl::focus() != this) Fl::focus(this);
    if (!strip && draw_event(event, mx, my)) return 1;
  }
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int fi = filmstrip_at(mx, my);
      int hv = fi >= 0 ? fi : -1;
      if (hv != film_hover_) {
        film_hover_ = hv;
        redraw();
      }
      if (!chrome_) {
        window()->cursor(FL_CURSOR_NONE);
      } else if (cropping_) {
        int hnd = crop_handle_at(mx, my);
        static const Fl_Cursor cursors[] = {FL_CURSOR_NW, FL_CURSOR_NE, FL_CURSOR_SE, FL_CURSOR_SW,
                                            FL_CURSOR_N,  FL_CURSOR_E,  FL_CURSOR_S,  FL_CURSOR_W, FL_CURSOR_MOVE};
        window()->cursor(hnd >= 0 ? cursors[hnd] : FL_CURSOR_DEFAULT);
      } else {
        double ix, iy, iw, ih;
        image_rect(ix, iy, iw, ih);
        bool pannable = iw > aw + 1 || ih > ah + 1;
        window()->cursor(fi == -2 && pannable ? FL_CURSOR_MOVE : FL_CURSOR_DEFAULT);
      }
      return 1;
    }
    case FL_LEAVE:
      film_hover_ = -1;
      window()->cursor(FL_CURSOR_DEFAULT);
      redraw();
      return 1;
    case FL_MOUSEWHEEL: {
      if (filmstrip_at(mx, my) != -2) {
        int d = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
        if (d) app_->step(d > 0 ? 1 : -1);
        return 1;
      }
      if (Fl::event_dx() && !Fl::event_dy()) {
        pan_x_ -= Fl::event_dx() * S(60);
        clamp_pan();
        redraw();
        return 1;
      }
      int dy = Fl::event_dy();
      if (!dy || !image_) return 1;
      // Wheel and Ctrl+wheel zoom around the pointer; Ctrl gives finer steps.
      double f = (Fl::event_state() & FL_CTRL) ? 1.1 : 1.25;
      zoom_at(target_scale() * (dy < 0 ? f : 1 / f), mx, my, true);
      return 1;
    }
    case FL_PUSH: {
      if (Fl::focus() != this) Fl::focus(this);
      int fi = filmstrip_at(mx, my);
      if (fi != -2) {
        if (fi >= 0) app_->go(fi);
        return 1;
      }
      if (cropping_) {
        crop_drag_ = crop_handle_at(mx, my);
        press_x_ = mx;
        press_y_ = my;
        press_crop_ = crop_;
        return 1;
      }
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        app_->menu_->popup();
        return 1;
      }
      if (Fl::event_clicks()) {
        Fl::event_clicks(0);
        if (mode_ == ZoomMode::Fit) zoom_at(std::max(1.0, fit_scale(false) * 2), mx, my, true);
        else set_zoom_mode(ZoomMode::Fit);
        return 1;
      }
      dragging_ = true;
      press_x_ = mx;
      press_y_ = my;
      press_pan_x_ = pan_x_;
      press_pan_y_ = pan_y_;
      return 1;
    }
    case FL_DRAG:
      if (cropping_ && crop_drag_ >= 0) {
        double ix, iy, iw, ih;
        image_rect(ix, iy, iw, ih);
        double dx = (mx - press_x_) / iw, dy = (my - press_y_) / ih;
        Rect r = press_crop_;
        double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
        const double minw = 0.02;
        int hnd = crop_drag_;
        if (hnd == 8) {
          double nx = std::max(0.0, std::min(1 - r.w, r.x + dx)), ny = std::max(0.0, std::min(1 - r.h, r.y + dy));
          crop_.x = nx;
          crop_.y = ny;
        } else {
          bool left = hnd == 0 || hnd == 3 || hnd == 7, right = hnd == 1 || hnd == 2 || hnd == 5;
          bool top = hnd == 0 || hnd == 1 || hnd == 4, bottom = hnd == 2 || hnd == 3 || hnd == 6;
          if (left) x0 = std::max(0.0, std::min(x1 - minw, x0 + dx));
          if (right) x1 = std::min(1.0, std::max(x0 + minw, x1 + dx));
          if (top) y0 = std::max(0.0, std::min(y1 - minw, y0 + dy));
          if (bottom) y1 = std::min(1.0, std::max(y0 + minw, y1 + dy));
          if (aspect_ > 0 && full_w_ > 0) {
            // Keep the aspect: width drives height unless only an edge
            // along the other axis is dragged.
            double ratio = aspect_ * full_h_ / double(full_w_);  // normalized w/h
            bool vertical_only = (top || bottom) && !left && !right;
            if (vertical_only) {
              double nw = (y1 - y0) * ratio, cx = (x0 + x1) / 2;
              x0 = cx - nw / 2;
              x1 = cx + nw / 2;
            } else {
              double nh = (x1 - x0) / ratio;
              if (top && !bottom) y0 = y1 - nh;
              else if (bottom && !top) y1 = y0 + nh;
              else {
                double cy = (y0 + y1) / 2;
                y0 = cy - nh / 2;
                y1 = cy + nh / 2;
              }
            }
            if (x0 < 0 || y0 < 0 || x1 > 1 || y1 > 1) break;  // keep the last valid rectangle
          }
          crop_ = {x0, y0, x1 - x0, y1 - y0};
        }
        redraw();
        return 1;
      }
      if (dragging_) {
        pan_x_ = press_pan_x_ + (mx - press_x_);
        pan_y_ = press_pan_y_ + (my - press_y_);
        if (mode_ == ZoomMode::Fit || mode_ == ZoomMode::Fill) {
          double ix, iy, iw, ih;
          image_rect(ix, iy, iw, ih);
          if (iw > aw || ih > ah) {
            custom_ = scale();
            mode_ = ZoomMode::Custom;
          }
        }
        clamp_pan();
        redraw();
      }
      return 1;
    case FL_RELEASE:
      if (dragging_ && !cropping_ && !app_->drawing() && std::abs(mx - press_x_) < 4 && std::abs(my - press_y_) < 4 &&
          mode_ == ZoomMode::Fit) {
        // A click on the left or right fifth of the picture steps.
        if (mx < ax + aw * 0.2) app_->step(-1);
        else if (mx > ax + aw * 0.8) app_->step(1);
      }
      dragging_ = false;
      crop_drag_ = -1;
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      return 1;
  }
  return Fl_Widget::handle(event);
}

}  // namespace photo
