// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The collage editor's side panel: the ten designs with live previews, the
// style (size, spacing, corners, border, background, title, shape) and the
// photos (their order, and the selected one).
#include <FL/Fl.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Int_Input.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "collage_ui.h"
#include "i18n.h"
#include "menu.h"
#include "menubar.h"
#include "util.h"

using namespace sde;
namespace icon = gallery::icon;

namespace photo {

namespace {

enum BoxId {
  B_DESIGN = 0,         // + design index
  B_SIZE = 100,         // + preset index; B_SIZE + 99 custom
  B_CUSTOM_SIZE = 199,
  B_BORDER_COLOR = 200,  // + swatch
  B_BACKGROUND = 300,    // + swatch
  B_BACKGROUND_CUSTOM = 390,
  B_BLUR = 391,
  B_SHOW_TITLE = 400,
  B_SHAPE = 410,  // + shape
  B_ADD = 500,
  B_SHUFFLE = 501,
  B_RESET = 502,
  B_REPLACE = 503,
  B_AS_BACKGROUND = 504,
  B_REMOVE = 505,
  B_TILE = 1000,  // + item
};

const uint32_t kBorderColors[] = {0xFFFFFFFF, 0xFF1E1E22, 0xFFF3EEE4, 0xFF9AA0A6, 0xFFD4AF37, 0xFFE8A2B5};
const uint32_t kBackgrounds[] = {0xFFFFFFFF, 0xFFF3EEE4, 0xFFE9E2D6, 0xFFFCEEF1, 0xFFE3F1EA,
                                 0xFFE2EDF8, 0xFF2B2F36, 0xFF101010};
constexpr int kBorderCount = int(sizeof kBorderColors / sizeof kBorderColors[0]);
constexpr int kBackgroundCount = int(sizeof kBackgrounds / sizeof kBackgrounds[0]);

Color argb(uint32_t c) { return Color::hex(c & 0xFFFFFF, ((c >> 24) & 0xFF) / 255.0); }

// The image scaled to cover a box.
void paint_cover(cairo_t *cr, cairo_surface_t *s, double x, double y, double w, double h) {
  double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
  double k = std::max(w / sw, h / sh);
  cairo_save(cr);
  cairo_rectangle(cr, x, y, w, h);
  cairo_clip(cr);
  cairo_translate(cr, x + (w - sw * k) / 2, y + (h - sh * k) / 2);
  cairo_scale(cr, k, k);
  cairo_set_source_surface(cr, s, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_restore(cr);
}

}  // namespace

CollagePanel::CollagePanel(CollageWindow *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  begin();
  auto slider = [this](double lo, double hi, double step) {
    auto *s = new Slider(0, 0, 10, S(26));
    s->bounds(lo, hi);
    s->step(step);
    s->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    s->callback([](Fl_Widget *wd, void *p) {
      auto *panel = static_cast<CollagePanel *>(p);
      int which = wd == panel->spacing_ ? 0 : wd == panel->radius_ ? 1 : wd == panel->border_ ? 2 : 3;
      panel->slider_changed(which);
    }, this);
    return s;
  };
  spacing_ = slider(0, 10, 0.5);
  radius_ = slider(0, 50, 1);
  border_ = slider(0, 4, 0.25);
  zoom_ = slider(100, 500, 5);
  title_ = new Fl_Input(0, 0, 10, S(30));
  title_->when(FL_WHEN_CHANGED);
  title_->callback([](Fl_Widget *, void *p) {
    auto *panel = static_cast<CollagePanel *>(p);
    Collage c = panel->app_->collage();
    c.title = trim(panel->title_->value());
    // One undo step for the whole title.
    panel->app_->apply(c, panel->gesture_);
    panel->gesture_ = true;
  }, this);
  open_ = new FlatButton(0, 0, 10, S(34), _("Open in Photo"));
  open_->callback([](Fl_Widget *, void *p) { static_cast<CollagePanel *>(p)->app_->open_in_photo(); }, this);
  export_ = new FlatButton(0, 0, 10, S(34), _("Export…"), true);
  export_->callback([](Fl_Widget *, void *p) { static_cast<CollagePanel *>(p)->app_->export_dialog(); }, this);
  end();
  resizable(nullptr);
  previews_.assign(size_t(kCollageDesigns), nullptr);
  layout();
}

CollagePanel::~CollagePanel() {
  Fl::remove_timeout(previews_cb, this);
  for (auto *s : previews_)
    if (s) cairo_surface_destroy(s);
}

void CollagePanel::resize(int x, int y, int w, int h) {
  bool wider = w != this->w();
  Fl_Widget::resize(x, y, w, h);  // children are placed by layout()
  if (wider) previews_changed();
  layout();
}

bool CollagePanel::editing_text() const { return Fl::focus() == title_; }

void CollagePanel::set_tab(Tab t) {
  tab_ = t;
  scroll_ = 0;
  hover_ = -1;
  if (Fl::focus() == title_) Fl::focus(app_->canvas_);
  layout();
  redraw();
}

void CollagePanel::sync() {
  const Collage &c = app_->collage();
  spacing_->value(c.spacing);
  radius_->value(c.radius);
  border_->value(c.border);
  int sel = app_->selected();
  zoom_->value(sel >= 0 && sel < int(c.items.size()) ? c.items[size_t(sel)].zoom * 100 : 100);
  if (Fl::focus() != title_) {
    gesture_ = false;
    if (title_->value() != c.title) title_->value(c.title.c_str());
  }
  bool have = !c.items.empty();
  for (auto *b : {open_, export_}) have ? b->activate() : b->deactivate();
  previews_changed();
  layout();
  redraw();
}

void CollagePanel::previews_changed() {
  previews_dirty_ = true;
  Fl::remove_timeout(previews_cb, this);
  Fl::add_timeout(0.15, previews_cb, this);
}

void CollagePanel::previews_cb(void *p) {
  auto *panel = static_cast<CollagePanel *>(p);
  if (panel->tab_ == Designs) {
    panel->render_previews();
    panel->redraw();
  }
}

void CollagePanel::render_previews() {
  if (!previews_dirty_) return;
  previews_dirty_ = false;
  const Collage &cur = app_->collage();
  int pw = w() - S(40);
  double cw = (pw - S(10)) / 2.0, ph = cw * 0.72;
  for (int i = 0; i < kCollageDesigns; ++i) {
    if (previews_[size_t(i)]) cairo_surface_destroy(previews_[size_t(i)]);
    previews_[size_t(i)] = nullptr;
    Collage d = cur;
    set_design(d, collage_designs()[size_t(i)].id);
    if (d.items.empty()) {
      // Without photos the designs show their shape with placeholders.
      for (int k = 0; k < 6; ++k) {
        CollageItem it;
        it.path = std::to_string(k);
        it.w = k % 3 == 1 ? 3 : 4;
        it.h = 3;
        d.items.push_back(it);
      }
      d.suggested_title = _("Title");
    }
    double k = std::min((cw - S(10)) / d.width, (ph - S(10)) / d.height);
    CollageImages img;
    img.photo = [&](int item) {
      return item < int(cur.items.size()) ? app_->thumb(cur.items[size_t(item)].path) : nullptr;
    };
    previews_[size_t(i)] = render_collage(d, k, img);
  }
}

void CollagePanel::layout() {
  for (int i = 0; i < children(); ++i) child(i)->hide();
  boxes_.clear();
  const Theme &t = app_->theme();
  const Collage &c = app_->collage();
  int px = x() + S(20), pw = w() - S(40);
  double y0 = top() - scroll_ + S(6), yy = y0;
  int ctop = top(), cbottom = bottom();
  auto place = [&](Fl_Widget *wd, double wy, int ww, int wh, int wx) {
    wd->resize(wx, int(wy), ww, wh);
    if (wy >= ctop - 1 && wy + wh <= cbottom + 1) wd->show();
  };
  auto label = [&](int index) {
    if (index >= 0 && index < 8) label_y_[index] = yy;
    yy += S(24);
  };
  for (auto &l : label_y_) l = -1e9;
  if (tab_ == Designs) {
    double cw = (pw - S(10)) / 2.0, ph = cw * 0.72, ch = ph + S(28);
    for (int i = 0; i < kCollageDesigns; ++i)
      boxes_.push_back({px + (i % 2) * (cw + S(10)), yy + (i / 2) * (ch + S(10)), cw, ch, B_DESIGN + i});
    yy += 5 * (ch + S(10));
  } else if (tab_ == Style) {
    label(0);  // SIZE
    double cw = (pw - S(8)) / 2.0;
    const auto &sizes = collage_sizes();
    for (size_t i = 0; i <= sizes.size(); ++i)
      boxes_.push_back({px + (i % 2) * (cw + S(8)), yy + (i / 2) * S(38), cw, double(S(32)),
                        i < sizes.size() ? B_SIZE + int(i) : B_CUSTOM_SIZE});
    yy += ((sizes.size() + 2) / 2) * S(38) + S(10);
    label(1);  // SPACING
    place(spacing_, yy, pw, S(26), px);
    yy += S(40);
    label(2);  // CORNERS
    place(radius_, yy, pw, S(26), px);
    yy += S(40);
    label(3);  // BORDER
    place(border_, yy, pw, S(26), px);
    yy += S(34);
    if (c.border > 0) {
      double sw = double(pw) / kBorderCount;
      for (int i = 0; i < kBorderCount; ++i) boxes_.push_back({px + i * sw, yy, sw, double(S(30)), B_BORDER_COLOR + i});
      yy += S(40);
    } else {
      yy += S(6);
    }
    label(4);  // BACKGROUND
    double sw = double(pw) / (kBackgroundCount + 1);
    for (int i = 0; i < kBackgroundCount; ++i) boxes_.push_back({px + i * sw, yy, sw, double(S(30)), B_BACKGROUND + i});
    boxes_.push_back({px + kBackgroundCount * sw, yy, sw, double(S(30)), B_BACKGROUND_CUSTOM});
    yy += S(38);
    boxes_.push_back({double(px), yy, double(pw), double(S(32)), B_BLUR});
    yy += S(46);
    label(5);  // TITLE
    place(title_, yy, pw, S(30), px);
    yy += S(38);
    boxes_.push_back({double(px), yy, double(pw), double(S(32)), B_SHOW_TITLE});
    yy += S(46);
    if (c.design == CollageDesign::Shapes) {
      label(6);  // SHAPE
      double hw = (pw - S(8)) / 2.0;
      boxes_.push_back({double(px), yy, hw, double(S(32)), B_SHAPE});
      boxes_.push_back({px + hw + S(8), yy, hw, double(S(32)), B_SHAPE + 1});
      yy += S(46);
    }
  } else {
    int n = int(c.items.size());
    double gap = S(8), ts = (pw - 3 * gap) / 4;
    for (int i = 0; i <= n; ++i)
      boxes_.push_back({px + (i % 4) * (ts + gap), yy + (i / 4) * (ts + gap), ts, ts, i < n ? B_TILE + i : B_ADD});
    yy += ((n + 4) / 4) * (ts + gap) + S(6);
    boxes_.push_back({double(px), yy, double(pw), double(S(32)), B_SHUFFLE});
    yy += S(46);
    int sel = app_->selected();
    if (sel >= 0 && sel < n) {
      label(7);  // SELECTED PHOTO
      yy += S(22);  // its name
      yy += S(20);
      place(zoom_, yy, pw, S(26), px);
      yy += S(40);
      double hw = (pw - S(8)) / 2.0;
      boxes_.push_back({double(px), yy, hw, double(S(32)), B_RESET});
      boxes_.push_back({px + hw + S(8), yy, hw, double(S(32)), B_REPLACE});
      yy += S(40);
      boxes_.push_back({double(px), yy, hw, double(S(32)), B_AS_BACKGROUND});
      boxes_.push_back({px + hw + S(8), yy, hw, double(S(32)), B_REMOVE});
      yy += S(46);
    }
  }
  content_h_ = yy + scroll_ - top();
  double room = cbottom - ctop;
  if (scroll_ > std::max(0.0, content_h_ - room)) {
    scroll_ = std::max(0.0, content_h_ - room);
    layout();
    return;
  }
  int by = y() + h() - S(52), half = (pw - S(8)) / 2;
  open_->resize(px, by, half, S(34));
  export_->resize(px + half + S(8), by, half, S(34));
  open_->show();
  export_->show();
  title_->box(FL_FLAT_BOX);
  title_->color(t.surface_alt.fl());
  title_->textcolor(t.text.fl());
  title_->selection_color(t.accent.fl());
  title_->cursor_color(t.accent.fl());
  title_->textsize(t.font_size);
}

int CollagePanel::box_at(int mx, int my) const {
  if (my < top() || my >= bottom()) return -1;
  for (const auto &b : boxes_)
    if (b.in(mx, my)) return b.id;
  return -1;
}

void CollagePanel::draw_content(Painter *p) {
  const Theme &t = app_->theme();
  const Collage &c = app_->collage();
  int px = x() + S(20), pw = w() - S(40);
  int sel = app_->selected();
  auto chip = [&](const Box &b, const std::string &text, bool on, bool enabled = true) {
    bool hot = b.id == hover_ && enabled;
    if (p) {
      Color bg = on ? t.accent.alpha(t.dark ? 0.32 : 0.16) : hot ? t.hover : t.surface_alt;
      p->fill_rounded(b.x, b.y, b.w, b.h, Sd(9), bg);
      if (on) p->stroke_rounded(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, Sd(9), t.accent, Sd(1.2));
    } else {
      text_font(t.font_size - S(1), on);
      draw_text(text, int(b.x + S(6)), int(b.y), int(b.w - S(12)), int(b.h),
                !enabled ? t.text_dim.alpha(0.6) : on ? t.accent_text : t.text, 1);
    }
  };
  for (const auto &b : boxes_) {
    if (b.y + b.h < top() || b.y > bottom()) continue;
    bool hot = b.id == hover_;
    if (b.id >= B_DESIGN && b.id < B_DESIGN + kCollageDesigns) {
      const auto &d = collage_designs()[size_t(b.id - B_DESIGN)];
      bool on = d.id == c.design;
      double ph = b.h - S(28);
      if (p) {
        p->fill_rounded(b.x, b.y, b.w, ph, Sd(10), hot ? t.hover : t.surface_alt);
        cairo_surface_t *s = previews_[size_t(b.id - B_DESIGN)];
        if (s) {
          double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
          double sx = b.x + (b.w - sw) / 2, sy = b.y + (ph - sh) / 2;
          p->shadow(sx, sy, sw, sh, 1, t.shadow.alpha(t.shadow.a * 0.5), S(4));
          p->surface(s, sx, sy);
        }
        if (on) p->stroke_rounded(b.x - Sd(1.5), b.y - Sd(1.5), b.w + Sd(3), ph + Sd(3), Sd(11), t.accent, Sd(2.5));
      } else {
        text_font(t.font_size - S(1), on);
        draw_text(_(d.name), int(b.x), int(b.y + ph + S(4)), int(b.w), S(22), on ? t.accent_text : t.text, 1);
      }
    } else if (b.id >= B_SIZE && b.id <= B_CUSTOM_SIZE) {
      int preset = size_preset(c.width, c.height);
      bool on = b.id == B_CUSTOM_SIZE ? preset < 0 : preset == b.id - B_SIZE;
      std::string text = b.id == B_CUSTOM_SIZE
                             ? (preset < 0 ? format(_("Custom %d × %d"), c.width, c.height) : std::string(_("Custom…")))
                             : std::string(_(collage_sizes()[size_t(b.id - B_SIZE)].name));
      chip(b, text, on);
    } else if ((b.id >= B_BORDER_COLOR && b.id < B_BORDER_COLOR + kBorderCount) ||
               (b.id >= B_BACKGROUND && b.id < B_BACKGROUND + kBackgroundCount)) {
      if (!p) continue;
      bool border = b.id < B_BACKGROUND;
      uint32_t col = border ? kBorderColors[b.id - B_BORDER_COLOR] : kBackgrounds[b.id - B_BACKGROUND];
      uint32_t cur = border ? c.border_color : c.background;
      bool on = (col & 0xFFFFFF) == (cur & 0xFFFFFF) && (border || c.blur_background < 0);
      double r = std::min(b.w, b.h) / 2 - Sd(3), cx = b.x + b.w / 2, cy = b.y + b.h / 2;
      if (on) p->circle(cx, cy, r + Sd(2.5), t.accent);
      else if (hot) p->circle(cx, cy, r + Sd(2), t.border);
      p->circle(cx, cy, r, t.surface);
      p->circle(cx, cy, r - Sd(1.5), argb(col));
      cairo_new_path(p->cr());
      cairo_arc(p->cr(), cx, cy, r - Sd(1.5), 0, 2 * M_PI);
      p->set(t.border);
      cairo_set_line_width(p->cr(), 1);
      cairo_stroke(p->cr());
    } else if (b.id == B_BACKGROUND_CUSTOM) {
      if (!p) continue;
      double r = std::min(b.w, b.h) / 2 - Sd(3), cx = b.x + b.w / 2, cy = b.y + b.h / 2;
      bool custom = c.blur_background < 0;
      for (uint32_t col : kBackgrounds) custom = custom && (col & 0xFFFFFF) != (c.background & 0xFFFFFF);
      if (custom) p->circle(cx, cy, r + Sd(2.5), t.accent);
      for (int i = 0; i < 12; ++i) {
        double a0 = i * M_PI / 6, a1 = (i + 1) * M_PI / 6, hue = i / 12.0;
        auto f = [hue](double off) {
          double k = std::fmod(hue * 6 + off, 6);
          return std::max(0.0, std::min(1.0, std::min(k, 4 - k)));
        };
        cairo_move_to(p->cr(), cx, cy);
        cairo_arc(p->cr(), cx, cy, r - Sd(1.5), a0, a1 + 0.02);
        cairo_close_path(p->cr());
        cairo_set_source_rgb(p->cr(), f(5), f(3), f(1));
        cairo_fill(p->cr());
      }
      p->circle(cx, cy, r * 0.45, custom ? argb(c.background) : t.surface);
    } else if (b.id == B_BLUR) {
      int item = c.blur_background >= 0 ? c.blur_background : sel >= 0 ? sel : 0;
      bool on = c.blur_background >= 0;
      if (p) {
        chip(b, "", on, !c.items.empty());
        if (item < int(c.items.size())) {
          if (cairo_surface_t *s = app_->backdrop(item)) {
            cairo_save(p->cr());
            p->rounded_rect(b.x + S(5), b.y + S(5), S(36), b.h - S(10), Sd(6));
            cairo_clip(p->cr());
            paint_cover(p->cr(), s, b.x + S(5), b.y + S(5), S(36), b.h - S(10));
            cairo_restore(p->cr());
          }
        }
      } else {
        text_font(t.font_size - S(1), on);
        draw_text(on ? _("Blurred photo behind the collage") : _("Blur a photo behind the collage"), int(b.x + S(50)),
                  int(b.y), int(b.w - S(56)), int(b.h), c.items.empty() ? t.text_dim : on ? t.accent_text : t.text);
      }
    } else if (b.id == B_SHOW_TITLE) {
      bool on = c.show_title;
      if (p) {
        chip(b, "", on);
        double bx = b.x + S(10), by = b.y + (b.h - S(16)) / 2;
        p->stroke_rounded(bx, by, S(16), S(16), Sd(4), on ? t.accent : t.text_dim, Sd(1.5));
        if (on) p->fill_rounded(bx + S(3), by + S(3), S(10), S(10), Sd(2), t.accent);
      } else {
        text_font(t.font_size - S(1), on);
        draw_text(_("Show the title"), int(b.x + S(34)), int(b.y), int(b.w - S(40)), int(b.h),
                  on ? t.accent_text : t.text);
      }
    } else if (b.id == B_SHAPE || b.id == B_SHAPE + 1) {
      bool heart = b.id == B_SHAPE;
      chip(b, heart ? _("Heart") : _("Circles"), (c.shape == CollageShape::Heart) == heart);
    } else if (b.id >= B_TILE && b.id < B_TILE + int(c.items.size())) {
      int item = b.id - B_TILE;
      if (!p) continue;
      cairo_t *cr = p->cr();
      double bx = b.x, by = b.y;
      bool dragged = drag_moved_ && drag_from_ == item;
      cairo_save(cr);
      p->rounded_rect(bx, by, b.w, b.h, Sd(8));
      cairo_clip(cr);
      p->fill_rect(bx, by, b.w, b.h, t.surface_alt);
      if (cairo_surface_t *s = app_->thumb(c.items[size_t(item)].path)) paint_cover(cr, s, bx, by, b.w, b.h);
      else icon::draw(*p, icon::Picture, bx + b.w * 0.3, by + b.h * 0.3, b.w * 0.4, t.text_dim.alpha(0.5));
      if (dragged) p->fill_rect(bx, by, b.w, b.h, t.surface.alpha(0.6));
      cairo_restore(cr);
      if (item == sel) p->stroke_rounded(bx - Sd(1.5), by - Sd(1.5), b.w + Sd(3), b.h + Sd(3), Sd(9), t.accent, Sd(2.5));
      if (c.blur_background == item) {
        p->circle(bx + S(13), by + b.h - S(13), S(9), Color::hex(0x000000, 0.55));
        p->circle(bx + S(13), by + b.h - S(13), S(4), Color::hex(0xFFFFFF, 0.9));
      }
      if (hot && !drag_moved_) {
        // The remove badge.
        double r = S(10), cx = bx + b.w - S(13), cy = by + S(13);
        p->circle(cx, cy, r, Color::hex(0x000000, 0.6));
        icon::draw(*p, icon::Close, cx - S(7), cy - S(7), S(14), Color::hex(0xFFFFFF));
      }
      if (drag_moved_ && drag_to_ == item) p->fill_rounded(bx - S(6), by, S(3), b.h, Sd(1.5), t.accent);
      if (drag_moved_ && drag_to_ == int(c.items.size()) && item + 1 == int(c.items.size()))
        p->fill_rounded(bx + b.w + S(3), by, S(3), b.h, Sd(1.5), t.accent);
    } else if (b.id == B_ADD) {
      if (p) {
        p->fill_rounded(b.x, b.y, b.w, b.h, Sd(8), hot ? t.hover : t.surface_alt);
        double dash[] = {Sd(4), Sd(4)};
        cairo_set_dash(p->cr(), dash, 2, 0);
        p->stroke_rounded(b.x + 1, b.y + 1, b.w - 2, b.h - 2, Sd(8), t.text_dim.alpha(0.6), Sd(1.2));
        cairo_set_dash(p->cr(), nullptr, 0, 0);
        icon::draw(*p, icon::Plus, b.x + b.w / 2 - S(12), b.y + b.h / 2 - S(12), S(24), t.accent_text);
      }
    } else if (b.id == B_SHUFFLE) {
      chip(b, _("Shuffle the Photos"), false, c.items.size() > 1);
    } else if (b.id == B_RESET) {
      chip(b, _("Reset Position"), false);
    } else if (b.id == B_REPLACE) {
      chip(b, _("Replace…"), false);
    } else if (b.id == B_AS_BACKGROUND) {
      chip(b, _("Blur Behind"), c.blur_background == sel);
    } else if (b.id == B_REMOVE) {
      chip(b, _("Remove"), false);
    }
  }
  if (p) return;
  // Section titles and values.
  const char *titles[] = {N_("SIZE"), N_("SPACING"), N_("CORNERS"), N_("BORDER"),
                          N_("BACKGROUND"), N_("TITLE"), N_("SHAPE"), N_("SELECTED PHOTO")};
  for (int i = 0; i < 8; ++i) {
    double ly = label_y_[i];
    if (ly < top() - S(20) || ly > bottom()) continue;
    text_font(t.font_size - S(1), true);
    draw_text(_(titles[i]), px, int(ly), pw, S(18), t.accent_text);
    text_font(t.font_size - S(1));
    std::string value;
    if (i == 1) value = format("%g%%", c.spacing);
    else if (i == 2) value = format("%g%%", c.radius);
    else if (i == 3) value = c.border > 0 ? format("%g%%", c.border) : std::string(_("None"));
    if (!value.empty()) draw_text(value, px, int(ly), pw, S(18), t.text_dim, 2);
    if (i == 7 && sel >= 0 && sel < int(c.items.size())) {
      text_font(t.font_size);
      draw_text(basename_of(c.items[size_t(sel)].path), px, int(ly + S(22)), pw, S(20), t.text);
      text_font(t.font_size - S(1));
      draw_text(_("Zoom"), px, int(zoom_->y() - S(20)), pw, S(18), t.text);
      draw_text(format("%d%%", int(std::lround(zoom_->value()))), px, int(zoom_->y() - S(20)), pw, S(18), t.text_dim, 2);
    }
  }
  if (title_->visible() && title_->size() == 0 && Fl::focus() != title_) {
    text_font(t.font_size);
    std::string hint = c.suggested_title.empty() ? std::string(_("A title for the collage")) : c.suggested_title;
    draw_text(hint, title_->x() + S(4), title_->y(), title_->w() - S(8), title_->h(), t.text_dim);
  }
}

void CollagePanel::draw() {
  const Theme &t = app_->theme();
  if (tab_ == Designs && previews_dirty_) render_previews();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), t.surface);
    p.line(x() + 0.5, y(), x() + 0.5, y() + h(), t.border);
    double tx = x() + S(20), tw = w() - S(40), th = S(34), ty = y() + S(14);
    p.fill_rounded(tx, ty, tw, th, th / 2, t.surface_alt);
    double seg = (tw - S(6)) / 3;
    p.fill_rounded(tx + S(3) + seg * tab_, ty + S(3), seg, th - S(6), (th - S(6)) / 2, t.accent);
    cairo_save(p.cr());
    cairo_rectangle(p.cr(), x(), top(), w(), bottom() - top());
    cairo_clip(p.cr());
    draw_content(&p);
    cairo_restore(p.cr());
    double room = bottom() - top();
    if (content_h_ > room + 1) {
      double track = room - S(8);
      double bar = std::max(double(S(24)), track * room / content_h_);
      double by = top() + S(4) + (track - bar) * scroll_ / std::max(1.0, content_h_ - room);
      p.fill_rounded(x() + w() - S(7), by, S(4), bar, S(2), t.text_dim.alpha(0.45));
      if (scroll_ < content_h_ - room - 1)
        p.fill_gradient(x() + 1, bottom() - S(18), w() - S(10), S(18), 0, t.surface.alpha(0), t.surface);
    }
    p.line(x() + S(20), y() + h() - S(64), x() + w() - S(20), y() + h() - S(64), t.border);
  }
  double tx = x() + S(20), tw = w() - S(40), th = S(34), ty = y() + S(14);
  double seg = (tw - S(6)) / 3;
  const char *names[] = {_("Designs"), _("Style"), _("Photos")};
  for (int i = 0; i < 3; ++i) {
    text_font(t.font_size - S(1), i == tab_);
    draw_text(names[i], int(tx + S(3) + seg * i), int(ty), int(seg), int(th), i == tab_ ? t.on_accent : t.text, 1);
  }
  fl_push_clip(x(), top(), w(), bottom() - top());
  draw_content(nullptr);
  for (int i = 0; i < children(); ++i) {
    Fl_Widget *c = child(i);
    if (c != open_ && c != export_ && c->visible()) draw_child(*c);
  }
  fl_pop_clip();
  draw_child(*open_);
  draw_child(*export_);
}

void CollagePanel::slider_changed(int which) {
  Collage c = app_->collage();
  if (which == 0) c.spacing = spacing_->value();
  else if (which == 1) c.radius = radius_->value();
  else if (which == 2) c.border = border_->value();
  else {
    int sel = app_->selected();
    if (sel < 0 || sel >= int(c.items.size())) return;
    double factor = zoom_->value() / 100 / std::max(1.0, c.items[size_t(sel)].zoom);
    int cell = cell_of_item(app_->layout_info(), sel);
    if (cell < 0) return;
    const CollageCell &cl = app_->layout_info().cells[size_t(cell)];
    zoom_item(c.items[size_t(sel)], cl.w, cl.h, factor, 0.5, 0.5);
  }
  bool release = Fl::event() == FL_RELEASE;
  bool drag = Fl::event() == FL_DRAG || release;
  app_->canvas_->set_fast(drag && !release);
  app_->apply(c, gesture_ && drag);
  gesture_ = drag && !release;
  if (release) app_->canvas_->invalidate();
  layout();
}

void CollagePanel::pick_custom_size() {
  const Collage &c = app_->collage();
  struct Win : BufferedWindow {
    Fl_Int_Input *width_in = nullptr, *height_in = nullptr;
    int result = 0;
    Win(int ww, int hh) : BufferedWindow(ww, hh, _("Custom Size")) {}
    void draw() override {
      const Theme &t = active_theme();
      {
        Painter p(w(), h());
        p.fill_rect(0, 0, w(), h(), t.surface);
      }
      text_font(t.font_size + S(1), true);
      draw_text(_("Custom Size"), S(24), S(20), w() - S(48), S(24), t.text);
      text_font(t.font_size);
      draw_text(_("Width and height in pixels:"), S(24), S(52), w() - S(48), S(22), t.text_dim);
      draw_text("×", width_in->x() + width_in->w(), width_in->y(), S(30), width_in->h(), t.text_dim, 1);
      draw_children();
    }
    int handle(int e) override {
      if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
        hide();
        return 1;
      }
      if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
        result = 1;
        hide();
        return 1;
      }
      return BufferedWindow::handle(e);
    }
  };
  int W = S(360), H = S(190);
  Win *win = new Win(W, H);
  const Theme &t = app_->theme();
  win->begin();
  int iw = (W - S(48) - S(30)) / 2;
  win->width_in = new Fl_Int_Input(S(24), S(84), iw, S(32));
  win->height_in = new Fl_Int_Input(S(24) + iw + S(30), S(84), iw, S(32));
  for (auto *in : {win->width_in, win->height_in}) {
    in->box(FL_FLAT_BOX);
    in->color(t.surface_alt.fl());
    in->textcolor(t.text.fl());
    in->selection_color(t.accent.fl());
    in->cursor_color(t.accent.fl());
    in->textsize(t.font_size);
  }
  win->width_in->value(std::to_string(c.width).c_str());
  win->height_in->value(std::to_string(c.height).c_str());
  auto *cancel = new FlatButton(W - S(24) - 2 * S(110) - S(8), H - S(52), S(110), S(34), _("Cancel"));
  cancel->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  auto *ok = new FlatButton(W - S(24) - S(110), H - S(52), S(110), S(34), _("Apply"), true);
  ok->callback([](Fl_Widget *b, void *) {
    static_cast<Win *>(b->window())->result = 1;
    b->window()->hide();
  });
  win->end();
  win->color(t.surface.fl());
  if (Fl_Window *parent = window())
    win->position(parent->x() + (parent->w() - W) / 2, parent->y() + (parent->h() - H) / 3);
  win->set_modal();
  win->show();
  Fl::focus(win->width_in);
  win->width_in->insert_position(0, win->width_in->size());
  while (win->shown()) Fl::wait();
  if (win->result) {
    int nw = atoi(win->width_in->value()), nh = atoi(win->height_in->value());
    if (nw > 0 && nh > 0) {
      Collage n = app_->collage();
      set_size(n, nw, nh);
      app_->apply(n);
      if (nw < kMinCollageSide || nh < kMinCollageSide || nw > kMaxCollageSide || nh > kMaxCollageSide)
        app_->toast(format(_("Sizes go from %d to %d pixels"), kMinCollageSide, kMaxCollageSide));
    }
  }
  Fl::delete_widget(win);
}

void CollagePanel::pick_color(bool border) {
  Collage c = app_->collage();
  uint32_t col = border ? c.border_color : c.background;
  uchar r = (col >> 16) & 0xFF, g = (col >> 8) & 0xFF, b = col & 0xFF;
  if (!fl_color_chooser(border ? _("Border Colour") : _("Background Colour"), r, g, b)) return;
  uint32_t v = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
  if (border) {
    c.border_color = v;
  } else {
    c.background = v;
    c.auto_background = false;
    c.blur_background = -1;
  }
  app_->apply(c);
}

int CollagePanel::click(int mx, int my) {
  int id = box_at(mx, my);
  if (id < 0) return 0;
  Collage c = app_->collage();
  int sel = app_->selected();
  if (id >= B_DESIGN && id < B_DESIGN + kCollageDesigns) {
    app_->choose_design(collage_designs()[size_t(id - B_DESIGN)].id);
  } else if (id >= B_SIZE && id < B_SIZE + int(collage_sizes().size())) {
    const auto &s = collage_sizes()[size_t(id - B_SIZE)];
    set_size(c, s.width, s.height);
    app_->apply(c);
  } else if (id == B_CUSTOM_SIZE) {
    pick_custom_size();
  } else if (id >= B_BORDER_COLOR && id < B_BORDER_COLOR + kBorderCount) {
    c.border_color = kBorderColors[id - B_BORDER_COLOR];
    app_->apply(c);
  } else if (id >= B_BACKGROUND && id < B_BACKGROUND + kBackgroundCount) {
    c.background = kBackgrounds[id - B_BACKGROUND];
    c.auto_background = false;
    c.blur_background = -1;
    app_->apply(c);
  } else if (id == B_BACKGROUND_CUSTOM) {
    pick_color(false);
  } else if (id == B_BLUR) {
    if (c.items.empty()) return 1;
    app_->use_as_background(c.blur_background >= 0 ? c.blur_background : sel >= 0 ? sel : 0);
  } else if (id == B_SHOW_TITLE) {
    c.show_title = !c.show_title;
    app_->apply(c);
  } else if (id == B_SHAPE || id == B_SHAPE + 1) {
    c.shape = id == B_SHAPE ? CollageShape::Heart : CollageShape::Circles;
    app_->apply(c);
  } else if (id == B_ADD) {
    app_->add_dialog();
  } else if (id == B_SHUFFLE) {
    app_->shuffle();
  } else if (id == B_RESET) {
    app_->reset_position(sel);
  } else if (id == B_REPLACE) {
    app_->replace_dialog(sel);
  } else if (id == B_AS_BACKGROUND) {
    app_->use_as_background(sel);
  } else if (id == B_REMOVE) {
    app_->remove_photo(sel);
  }
  layout();
  redraw();
  return 1;
}

int CollagePanel::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  const Collage &c = app_->collage();
  switch (event) {
    case FL_PUSH: {
      if (my >= y() + S(14) && my < y() + S(48) && mx >= x() + S(20) && mx < x() + w() - S(20)) {
        int seg = (mx - x() - S(23)) * 3 / std::max(1, w() - S(46));
        set_tab(Tab(std::max(0, std::min(2, seg))));
        return 1;
      }
      int id = box_at(mx, my);
      if (Fl::focus() == title_ && !(mx >= title_->x() && mx < title_->x() + title_->w() && my >= title_->y() &&
                                     my < title_->y() + title_->h()))
        Fl::focus(app_->canvas_);
      if (id >= B_TILE && id < B_TILE + int(c.items.size())) {
        int item = id - B_TILE;
        // The remove badge.
        for (const auto &b : boxes_)
          if (b.id == id && mx > b.x + b.w - S(26) && my < b.y + S(26)) {
            app_->remove_photo(item);
            return 1;
          }
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          app_->select(item);
          ContextMenu m;
          m.item(_("Replace Photo…"), [this, item] { app_->replace_dialog(item); }, {"image"});
          m.item(_("Move Earlier"), [this] { app_->move_selected(-1); }, {"", 0, item > 0});
          m.item(_("Move Later"), [this] { app_->move_selected(1); }, {"", 0, item + 1 < int(c.items.size()), true});
          m.item(_("Remove from Collage"), [this, item] { app_->remove_photo(item); }, {"delete"});
          m.popup();
          return 1;
        }
        drag_from_ = item;
        drag_to_ = -1;
        drag_moved_ = false;
        press_x_ = mx;
        press_y_ = my;
        app_->select(item);
        return 1;
      }
      if (click(mx, my)) return 1;
      break;
    }
    case FL_DRAG:
      if (drag_from_ >= 0) {
        if (!drag_moved_ && std::abs(mx - press_x_) + std::abs(my - press_y_) < S(6)) return 1;
        drag_moved_ = true;
        // The gap nearest to the pointer.
        int n = int(c.items.size()), best = n;
        double bestd = 1e18;
        for (const auto &b : boxes_) {
          if (b.id < B_TILE || b.id >= B_TILE + n) continue;
          int i = b.id - B_TILE;
          for (int side = 0; side < 2; ++side) {
            double gx = side ? b.x + b.w : b.x, gy = b.y + b.h / 2;
            double d = std::hypot(mx - gx, my - gy);
            if (d < bestd) {
              bestd = d;
              best = i + side;
            }
          }
        }
        drag_to_ = best;
        window()->cursor(FL_CURSOR_MOVE);
        redraw();
        return 1;
      }
      break;
    case FL_RELEASE:
      if (drag_from_ >= 0) {
        int from = drag_from_, gap = drag_to_;
        bool moved = drag_moved_;
        drag_from_ = drag_to_ = -1;
        drag_moved_ = false;
        window()->cursor(FL_CURSOR_DEFAULT);
        if (moved && gap >= 0) {
          int to = gap > from ? gap - 1 : gap;
          if (to != from) {
            Collage n = app_->collage();
            move_item(n, from, to);
            app_->apply(n);
            app_->select(to);
          }
        }
        redraw();
        return 1;
      }
      break;
    case FL_MOVE:
    case FL_ENTER: {
      int id = box_at(mx, my);
      if (id != hover_) {
        hover_ = id;
        redraw();
      }
      window()->cursor(id >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
      break;
    }
    case FL_LEAVE:
      hover_ = -1;
      window()->cursor(FL_CURSOR_DEFAULT);
      redraw();
      break;
    case FL_MOUSEWHEEL:
      if (my >= top() && my < bottom()) {
        double room = bottom() - top();
        scroll_ = std::max(0.0, std::min(std::max(0.0, content_h_ - room), scroll_ + Fl::event_dy() * S(48)));
        layout();
        redraw();
        return 1;
      }
      break;
  }
  int r = Fl_Group::handle(event);
  if (event == FL_PUSH || event == FL_ENTER || event == FL_MOVE) return 1;
  return r;
}

}  // namespace photo
