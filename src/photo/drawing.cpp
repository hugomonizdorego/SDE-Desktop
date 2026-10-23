// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Drawing on the canvas: the Draw tab's tools (pen, highlighter, eraser,
// shapes, text, blur and pixelate regions, colour picker, select). Marks
// are kept in image coordinates (see draw.h), so they stay in place at
// every zoom level and pan position; the vector marks are drawn live on
// top of the photo and flattened into it only when saving.
#include <FL/Fl.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "photo.h"

using namespace sde;

namespace photo {

namespace {

MarkKind kind_of(Tool t) {
  switch (t) {
    case Tool::Highlighter: return MarkKind::Highlighter;
    case Tool::Line: return MarkKind::Line;
    case Tool::Arrow: return MarkKind::Arrow;
    case Tool::Rect: return MarkKind::Rect;
    case Tool::Ellipse: return MarkKind::Ellipse;
    case Tool::Text: return MarkKind::Text;
    case Tool::Blur: return MarkKind::Blur;
    case Tool::Pixelate: return MarkKind::Pixelate;
    default: break;
  }
  return MarkKind::Pen;
}

void dashed_box(cairo_t *cr, double x0, double y0, double x1, double y1, const Color &accent) {
  double dash[] = {Sd(5), Sd(4)};
  cairo_save(cr);
  cairo_set_line_width(cr, Sd(1.5));
  cairo_rectangle(cr, std::min(x0, x1) + 0.5, std::min(y0, y1) + 0.5, std::fabs(x1 - x0), std::fabs(y1 - y0));
  cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
  cairo_stroke_preserve(cr);
  cairo_set_dash(cr, dash, 2, 0);
  cairo_set_source_rgba(cr, accent.r, accent.g, accent.b, 1);
  cairo_stroke(cr);
  cairo_restore(cr);
}

}  // namespace

ViewMap Canvas::screen_map() const {
  double ix, iy, iw, ih;
  image_rect(ix, iy, iw, ih);
  Edits e = app_->edits_for_marks();
  if (cropping_) {
    // The whole picture is shown while cropping.
    e.crop = Rect();
    e.resize_w = e.resize_h = 0;
  }
  return view_map(e, ix, iy, iw, ih);
}

void Canvas::select_mark(int i) {
  if (i == selected_) return;
  selected_ = i;
  const auto &items = app_->edits().marks.items;
  if (i >= 0 && i < int(items.size())) {
    // Show the selected mark's style in the panel (in screen pixels).
    const Mark &m = items[size_t(i)];
    DrawSettings &d = app_->draw_settings();
    double k = std::max(1e-6, screen_map().scale());
    int tool = m.kind == MarkKind::Pen ? int(Tool::Pen) : m.kind == MarkKind::Highlighter ? int(Tool::Highlighter)
             : m.kind == MarkKind::Line ? int(Tool::Line) : m.kind == MarkKind::Arrow ? int(Tool::Arrow)
             : m.kind == MarkKind::Rect ? int(Tool::Rect) : m.kind == MarkKind::Ellipse ? int(Tool::Ellipse)
             : m.kind == MarkKind::Blur ? int(Tool::Blur) : m.kind == MarkKind::Pixelate ? int(Tool::Pixelate)
                                        : int(Tool::Text);
    if (m.kind == MarkKind::Text) {
      d.text_size = std::max(8.0, std::min(160.0, std::round(m.size * k)));
      d.font = m.font;
      d.text_box = m.box;
    } else {
      d.width[tool] = std::max(1.0, std::round(m.width * k));
    }
    if (!m.effect()) {
      d.color = m.color;
      d.opacity[tool] = std::round(m.opacity * 100);
    }
    if (m.kind == MarkKind::Rect || m.kind == MarkKind::Ellipse) d.fill = m.fill;
  }
  // The panel shows the options of the selected mark.
  if (app_->panel_ && app_->panel_->tab() == SidePanel::Draw) app_->panel_->layout();
  redraw();
  app_->redraw();
}

void Canvas::cancel_drawing() {
  live_on_ = false;
  mark_drag_ = -1;
  erasing_ = false;
  middle_pan_ = false;
  select_mark(-1);
  redraw();
}

Mark Canvas::style_mark(MarkKind kind) const {
  const DrawSettings &d = app_->draw_settings();
  double k = std::max(1e-6, screen_map().scale());  // screen pixels per image pixel
  Mark m;
  m.kind = kind;
  m.color = d.color;
  m.width = d.tool_width() / k;
  m.opacity = d.tool_opacity();
  m.fill = d.fill && (kind == MarkKind::Rect || kind == MarkKind::Ellipse);
  m.font = d.font;
  m.size = d.text_size / k;
  m.box = d.text_box;
  if (m.effect()) m.opacity = 1;
  return m;
}

int Canvas::handle_at(int mx, int my) const {
  const Marks &marks = mark_drag_ >= 0 ? drag_marks_ : app_->edits().marks;
  if (selected_ < 0 || selected_ >= int(marks.items.size())) return -1;
  ViewMap v = screen_map();
  Box b = mark_bounds(marks.items[size_t(selected_)]);
  Pt a = v.to_out({b.x0, b.y0}), c = v.to_out({b.x1, b.y1});
  Pt pts[4] = {{a.x, a.y}, {c.x, a.y}, {c.x, c.y}, {a.x, c.y}};
  for (int i = 0; i < 4; ++i)
    if (std::fabs(mx - pts[i].x) <= S(9) && std::fabs(my - pts[i].y) <= S(9)) return i;
  return -1;
}

void Canvas::commit_live() {
  live_on_ = false;
  if (live_.pts.empty()) return;
  ViewMap v = screen_map();
  double k = std::max(1e-6, v.scale());
  if (live_.freehand()) {
    live_.pts = thin_points(live_.pts, 0.75 / k);
  } else if (live_.pts.size() >= 2) {
    Pt a = v.to_out(live_.pts[0]), b = v.to_out(live_.pts[1]);
    bool line = live_.kind == MarkKind::Line || live_.kind == MarkKind::Arrow;
    double dx = std::fabs(a.x - b.x), dy = std::fabs(a.y - b.y);
    if (line ? std::hypot(dx, dy) < S(4) : (dx < S(4) || dy < S(4))) {
      redraw();
      return;  // a click, not a shape
    }
  }
  Edits e = app_->edits_for_marks();
  e.marks.items.push_back(live_);
  app_->apply(e);
  redraw();
}

int Canvas::draw_event(int event, int mx, int my) {
  ViewMap v = screen_map();
  Pt p = v.to_image({double(mx), double(my)});
  double k = std::max(1e-6, v.scale());
  DrawSettings &d = app_->draw_settings();
  bool shift = Fl::event_state() & FL_SHIFT;
  switch (event) {
    case FL_PUSH: {
      if (Fl::event_button() == FL_MIDDLE_MOUSE) {
        middle_pan_ = dragging_ = true;
        press_x_ = mx;
        press_y_ = my;
        press_pan_x_ = pan_x_;
        press_pan_y_ = pan_y_;
        return 1;
      }
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      const Marks &marks = app_->edits().marks;
      // The corners of the selection resize it, with every tool.
      int hnd = handle_at(mx, my);
      if (hnd >= 0) {
        mark_drag_ = hnd;
        drag_marks_ = app_->edits_for_marks().marks;
        press_box_ = mark_bounds(drag_marks_.items[size_t(selected_)]);
        press_img_ = p;
        return 1;
      }
      switch (d.tool) {
        case Tool::Select: {
          int i = mark_at(marks, p, S(6) / k);
          if (i >= 0) {
            select_mark(i);
            if (Fl::event_clicks() && marks.items[size_t(i)].kind == MarkKind::Text) {
              Fl::event_clicks(0);
              app_->edit_text_mark(i);
              return 1;
            }
            mark_drag_ = 4;
            drag_marks_ = app_->edits_for_marks().marks;
            press_box_ = mark_bounds(drag_marks_.items[size_t(i)]);
            press_img_ = p;
          } else {
            select_mark(-1);
            dragging_ = true;  // empty space: move the photo
            press_x_ = mx;
            press_y_ = my;
            press_pan_x_ = pan_x_;
            press_pan_y_ = pan_y_;
          }
          return 1;
        }
        case Tool::Picker: {
          double ix, iy, iw, ih;
          image_rect(ix, iy, iw, ih);
          if (!image_ || mx < ix || my < iy || mx >= ix + iw || my >= iy + ih) return 1;
          int sw = cairo_image_surface_get_width(image_), sh = cairo_image_surface_get_height(image_);
          int sx = std::min(sw - 1, int((mx - ix) / iw * sw)), sy = std::min(sh - 1, int((my - iy) / ih * sh));
          cairo_surface_flush(image_);
          uint32_t px = reinterpret_cast<const uint32_t *>(cairo_image_surface_get_data(image_) +
                                                           size_t(sy) * cairo_image_surface_get_stride(image_))[sx];
          unsigned a = px >> 24;
          auto un = [a](unsigned c) { return a ? std::min(255u, (c * 255 + a / 2) / a) : 0u; };
          d.color = 0xFF000000u | (un((px >> 16) & 0xFF) << 16) | (un((px >> 8) & 0xFF) << 8) | un(px & 0xFF);
          app_->set_tool(Tool::Pen);
          app_->style_changed(false);
          char hex[16];
          snprintf(hex, sizeof hex, "#%06X", unsigned(d.color & 0xFFFFFF));
          app_->toast(format(_("Colour %s picked"), hex));
          return 1;
        }
        case Tool::Eraser:
          erasing_ = true;
          erased_ = false;
          select_mark(-1);
          break;  // erases below, like a drag
        case Tool::Text: {
          int i = mark_at(marks, p, S(4) / k);
          if (i >= 0 && marks.items[size_t(i)].kind == MarkKind::Text) {
            select_mark(i);
            app_->edit_text_mark(i);
            return 1;
          }
          const char *r = fl_input("%s", "", _("Text to place on the photo:"));
          if (!r || !*r) return 1;
          Mark m = style_mark(MarkKind::Text);
          m.text = r;
          m.pts = {p};
          Edits e = app_->edits_for_marks();
          e.marks.items.push_back(m);
          app_->apply(e);
          select_mark(int(e.marks.items.size()) - 1);
          return 1;
        }
        default:
          select_mark(-1);
          live_ = style_mark(kind_of(d.tool));
          if (live_.kind == MarkKind::Highlighter) live_.color |= 0xFF000000u;
          live_.pts = {p};
          if (!live_.freehand()) live_.pts.push_back(p);
          live_on_ = true;
          redraw();
          return 1;
      }
    }
      // The eraser erases where it is pressed, as when dragged.
      [[fallthrough]];
    case FL_DRAG: {
      if (middle_pan_) return 0;  // the canvas pans
      if (erasing_) {
        Edits e = app_->edits_for_marks();
        double r = d.width[int(Tool::Eraser)] / 2 / k;
        // Erase along the way from the last position: fast drags leave no
        // gaps.
        Pt from = event == FL_PUSH ? p : press_img_;
        int steps = std::max(1, int(std::ceil(std::hypot(p.x - from.x, p.y - from.y) / std::max(1e-6, r * 0.5))));
        bool any = false;
        for (int s = 1; s <= steps; ++s)
          any |= erase_at(e.marks, {from.x + (p.x - from.x) * s / steps, from.y + (p.y - from.y) * s / steps}, r);
        press_img_ = p;
        if (any) {
          app_->apply(e, erased_);  // one undo step per drag
          erased_ = true;
        }
        hover_x_ = mx;
        hover_y_ = my;
        redraw();
        return 1;
      }
      if (mark_drag_ >= 0) {
        drag_marks_ = app_->edits_for_marks().marks;
        if (selected_ < 0 || selected_ >= int(drag_marks_.items.size())) return 1;
        Mark &m = drag_marks_.items[size_t(selected_)];
        if (mark_drag_ == 4) {
          translate_mark(m, p.x - press_img_.x, p.y - press_img_.y);
        } else {
          Box b = press_box_;
          double min = S(8) / k;
          bool left = mark_drag_ == 0 || mark_drag_ == 3, top = mark_drag_ == 0 || mark_drag_ == 1;
          double nx = left ? std::min(p.x, b.x1 - min) : std::max(p.x, b.x0 + min);
          double ny = top ? std::min(p.y, b.y1 - min) : std::max(p.y, b.y0 + min);
          Box nb = b;
          (left ? nb.x0 : nb.x1) = nx;
          (top ? nb.y0 : nb.y1) = ny;
          if (shift || m.kind == MarkKind::Text) {
            // Keep the proportions.
            double s = std::max(nb.w() / std::max(1e-6, b.w()), nb.h() / std::max(1e-6, b.h()));
            double w = b.w() * s, h = b.h() * s;
            if (left) nb.x0 = nb.x1 - w;
            else nb.x1 = nb.x0 + w;
            if (top) nb.y0 = nb.y1 - h;
            else nb.y1 = nb.y0 + h;
          }
          reshape_mark(m, b, nb);
        }
        redraw();
        return 1;
      }
      if (live_on_) {
        if (live_.freehand()) {
          Pt last = v.to_out(live_.pts.back());
          if (std::hypot(last.x - mx, last.y - my) >= 1.5) live_.pts.push_back(p);
        } else {
          Pt a = live_.pts[0], b = p;
          if (shift) {
            double dx = b.x - a.x, dy = b.y - a.y;
            if (live_.kind == MarkKind::Line || live_.kind == MarkKind::Arrow) {
              // Snap to 45 degrees.
              double ang = std::round(std::atan2(dy, dx) / (M_PI / 4)) * (M_PI / 4), len = std::hypot(dx, dy);
              b = {a.x + len * std::cos(ang), a.y + len * std::sin(ang)};
            } else {
              double s = std::max(std::fabs(dx), std::fabs(dy));
              b = {a.x + (dx < 0 ? -s : s), a.y + (dy < 0 ? -s : s)};
            }
          }
          live_.pts[1] = b;
        }
        redraw();
        return 1;
      }
      return dragging_ ? 0 : 1;
    }
    case FL_RELEASE:
      if (middle_pan_) {
        middle_pan_ = dragging_ = false;
        return 1;
      }
      if (erasing_) {
        erasing_ = false;
        return 1;
      }
      if (mark_drag_ >= 0) {
        mark_drag_ = -1;
        Edits e = app_->edits_for_marks();
        if (drag_marks_ != e.marks && drag_marks_.items.size() == e.marks.items.size()) {
          e.marks = drag_marks_;
          app_->apply(e);
        }
        redraw();
        return 1;
      }
      if (live_on_) {
        commit_live();
        return 1;
      }
      dragging_ = false;
      return 1;
    case FL_MOVE:
    case FL_ENTER: {
      hover_x_ = mx;
      hover_y_ = my;
      double ix, iy, iw, ih;
      image_rect(ix, iy, iw, ih);
      hover_in_ = mx >= ix && my >= iy && mx < ix + iw && my < iy + ih;
      Fl_Cursor c = FL_CURSOR_CROSS;
      int hnd = handle_at(mx, my);
      if (hnd >= 0) {
        c = hnd == 0 || hnd == 2 ? FL_CURSOR_NWSE : FL_CURSOR_NESW;
      } else if (d.tool == Tool::Select) {
        int i = mark_at(app_->edits().marks, p, S(6) / k);
        c = i >= 0 ? (i == selected_ ? FL_CURSOR_MOVE : FL_CURSOR_HAND) : FL_CURSOR_DEFAULT;
      } else if (d.tool == Tool::Text) {
        c = FL_CURSOR_INSERT;
      } else if (d.tool == Tool::Eraser) {
        c = hover_in_ ? FL_CURSOR_NONE : FL_CURSOR_DEFAULT;
      }
      if (!hover_in_ && d.tool != Tool::Select && hnd < 0) c = FL_CURSOR_DEFAULT;
      window()->cursor(c);
      if (d.tool == Tool::Eraser) redraw();
      return 1;
    }
  }
  return 0;
}

void Canvas::draw_overlay(Painter &p) {
  const Marks &marks = mark_drag_ >= 0 ? drag_marks_ : app_->edits().marks;
  bool drawing = app_->drawing();
  if (marks.empty() && !live_on_ && !(drawing && app_->draw_settings().tool == Tool::Eraser)) return;
  cairo_t *cr = p.cr();
  ViewMap v = screen_map();
  double ix, iy, iw, ih;
  image_rect(ix, iy, iw, ih);
  const Theme &t = app_->theme();
  cairo_save(cr);
  cairo_rectangle(cr, ix, iy, iw, ih);
  cairo_clip(cr);
  // The vector layer (effects are part of the rendered photo).
  if (!marks.empty()) draw_marks(cr, marks, v, true, false);
  if (drawing || mark_drag_ >= 0)
    for (const Mark &m : marks.items)
      if (m.effect() && m.pts.size() >= 2) {
        Pt a = v.to_out(m.pts[0]), b = v.to_out(m.pts[1]);
        dashed_box(cr, a.x, a.y, b.x, b.y, Color::hex(0xFFFFFF, 0.8));
      }
  if (live_on_) {
    if (live_.effect()) {
      Pt a = v.to_out(live_.pts[0]), b = v.to_out(live_.pts[1]);
      p.fill_rect(std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y),
                  Color::hex(0xFFFFFF, 0.25));
      dashed_box(cr, a.x, a.y, b.x, b.y, t.accent);
    } else {
      draw_mark(cr, live_, v);
    }
  }
  if (drawing && selected_ >= 0 && selected_ < int(marks.items.size())) {
    Box b = mark_bounds(marks.items[size_t(selected_)]);
    Pt a = v.to_out({b.x0, b.y0}), c = v.to_out({b.x1, b.y1});
    dashed_box(cr, a.x - S(3), a.y - S(3), c.x + S(3), c.y + S(3), t.accent);
    Pt pts[4] = {{a.x - S(3), a.y - S(3)}, {c.x + S(3), a.y - S(3)}, {c.x + S(3), c.y + S(3)}, {a.x - S(3), c.y + S(3)}};
    for (const Pt &h : pts) {
      p.circle(h.x, h.y, Sd(6), Color::hex(0x000000, 0.35));
      p.circle(h.x, h.y, Sd(5), Color::hex(0xFFFFFF));
      p.circle(h.x, h.y, Sd(3), t.accent);
    }
  }
  if (drawing && app_->draw_settings().tool == Tool::Eraser && hover_in_) {
    double r = app_->draw_settings().width[int(Tool::Eraser)] / 2;
    cairo_new_path(cr);
    cairo_arc(cr, hover_x_, hover_y_, r, 0, 2 * M_PI);
    cairo_set_line_width(cr, Sd(2.5));
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, Sd(1.2));
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

}  // namespace photo
