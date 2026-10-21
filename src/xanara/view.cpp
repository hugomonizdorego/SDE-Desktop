// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The page canvas. Pages are drawn from 512 px tiles rendered at the exact
// device scale (zoom x 96/72 x UI scale) and placed on whole pixels, so text
// is as sharp as the screen allows. While the zoom changes, the canvas shows
// the nearest cached tiles scaled, and renders the exact scale once the zoom
// has settled.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <set>

#include "i18n.h"
#include "spawn.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

namespace {

constexpr double kMinZoom = 0.1, kMaxZoom = 32.0;
constexpr double kSettleDelay = 0.16;
constexpr size_t kDefaultCacheBytes = 224u * 1024u * 1024u;

Color paper_color(ColorMode m) {
  if (m == ColorMode::Night) return Color::hex(0x1D2026);
  if (m == ColorMode::Sepia) return Color::hex(0xF4ECD8);
  return Color::hex(0xFFFFFF);
}

bool is_markup(PopplerAnnotType t) {
  return t == POPPLER_ANNOT_HIGHLIGHT || t == POPPLER_ANNOT_UNDERLINE || t == POPPLER_ANNOT_STRIKE_OUT ||
         t == POPPLER_ANNOT_SQUIGGLY;
}

int tool_index(Tool t) { return int(t); }

// Views that are alive, so a late renderer notification never reaches a
// view that was closed meanwhile.
std::set<DocView *> &live_views() {
  static std::set<DocView *> v;
  return v;
}

void results_cb(void *p) {
  auto *v = static_cast<DocView *>(p);
  if (live_views().count(v)) v->results_ready();
}

std::string today() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

}  // namespace

Tool tool_for(Act a, FieldKind *kind) {
  switch (a) {
    case Act::Highlight: return Tool::Highlight;
    case Act::Underline: return Tool::Underline;
    case Act::Squiggly: return Tool::Squiggly;
    case Act::Strike: return Tool::Strike;
    case Act::TextBox: return Tool::Text;
    case Act::Note: return Tool::Note;
    case Act::Pen: return Tool::Pen;
    case Act::Eraser: return Tool::Eraser;
    case Act::Rect: return Tool::Rect;
    case Act::Ellipse: return Tool::Ellipse;
    case Act::Arrow: return Tool::Arrow;
    case Act::Line: return Tool::Line;
    case Act::Measure: return Tool::Measure;
    case Act::Stamp: return Tool::Stamp;
    case Act::Signature: case Act::FillSign: return Tool::Signature;
    case Act::AddText: case Act::FillText: return Tool::FillText;
    case Act::AddImage: return Tool::Image;
    case Act::AddLink: return Tool::Link;
    case Act::Crop: return Tool::Crop;
    case Act::FillCheck: return Tool::FillCheck;
    case Act::FillCross: return Tool::FillCross;
    case Act::FillDot: return Tool::FillDot;
    case Act::FillDate: return Tool::FillDate;
    case Act::FillInitials: return Tool::FillInitials;
    case Act::Redact: return Tool::Redact;
    case Act::FieldText: case Act::FieldCheck: case Act::FieldRadio: case Act::FieldCombo: case Act::FieldList:
    case Act::FieldButton: case Act::FieldSignature:
      if (kind) {
        static const FieldKind kinds[] = {FieldKind::Text, FieldKind::Check, FieldKind::Radio, FieldKind::Combo,
                                          FieldKind::List, FieldKind::Button, FieldKind::Signature};
        *kind = kinds[int(a) - int(Act::FieldText)];
      }
      return Tool::Field;
    default: return Tool::Select;
  }
}

const char *tool_name(Tool t) {
  switch (t) {
    case Tool::Select: return _("Select");
    case Tool::Hand: return _("Hand");
    case Tool::Highlight: return _("Highlight");
    case Tool::Underline: return _("Underline");
    case Tool::Squiggly: return _("Squiggly");
    case Tool::Strike: return _("Strike Out");
    case Tool::Note: return _("Sticky Note");
    case Tool::Text: return _("Text Box");
    case Tool::Pen: return _("Pen");
    case Tool::Eraser: return _("Eraser");
    case Tool::Rect: return _("Rectangle");
    case Tool::Ellipse: return _("Ellipse");
    case Tool::Arrow: return _("Arrow");
    case Tool::Line: return _("Line");
    case Tool::Measure: return _("Measure");
    case Tool::Stamp: return _("Stamp");
    case Tool::Signature: return _("Signature");
    case Tool::Image: return _("Image");
    case Tool::Link: return _("Link");
    case Tool::Crop: return _("Crop");
    case Tool::Field: return _("Form Field");
    case Tool::FillText: return _("Text");
    case Tool::FillCheck: return _("Check Mark");
    case Tool::FillCross: return _("Cross");
    case Tool::FillDot: return _("Dot");
    case Tool::FillDate: return _("Date");
    case Tool::FillInitials: return _("Initials");
    case Tool::Redact: return _("Redaction");
    case Tool::Count: break;
  }
  return "";
}

const char *tool_hint(Tool t) {
  switch (t) {
    case Tool::Highlight: case Tool::Underline: case Tool::Squiggly: case Tool::Strike: return _("Drag across text");
    case Tool::Note: return _("Click where the note belongs");
    case Tool::Text: return _("Drag a box, then type");
    case Tool::Pen: return _("Draw on the page");
    case Tool::Eraser: return _("Click or drag over annotations to remove them");
    case Tool::Rect: case Tool::Ellipse: return _("Drag to draw the shape");
    case Tool::Arrow: case Tool::Line: return _("Drag from start to end");
    case Tool::Measure: return _("Drag between two points to measure");
    case Tool::Stamp: return _("Click to place the stamp");
    case Tool::Signature: return _("Click to place your signature");
    case Tool::Image: return _("Click or drag a box to place the image");
    case Tool::Link: return _("Drag over the area that should be a link");
    case Tool::Crop: return _("Drag the area to keep");
    case Tool::Field: return _("Drag the field's box (click for check boxes and radio buttons)");
    case Tool::FillText: return _("Click and type");
    case Tool::FillCheck: case Tool::FillCross: case Tool::FillDot: case Tool::FillDate: case Tool::FillInitials:
      return _("Click where it belongs");
    case Tool::Redact: return _("Drag over what must disappear, then Apply Redactions");
    default: return "";
  }
}

bool tool_has_width(Tool t) {
  return t == Tool::Pen || t == Tool::Rect || t == Tool::Ellipse || t == Tool::Arrow || t == Tool::Line ||
         t == Tool::Measure;
}

bool tool_has_font(Tool t) { return t == Tool::Text || t == Tool::FillText || t == Tool::FillDate || t == Tool::FillInitials; }

bool tool_has_color(Tool t) {
  return t != Tool::Select && t != Tool::Hand && t != Tool::Eraser && t != Tool::Image && t != Tool::Link &&
         t != Tool::Crop && t != Tool::Field && t != Tool::Signature && t != Tool::Redact;
}

DocView::DocView(XanaraWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {
  cache_.reset(new TileCache(kDefaultCacheBytes));
  live_views().insert(this);
  for (int k = 0; k < kToolCount; ++k) {
    tool_ink_[k] = Rgb{0.85, 0.18, 0.2};
    opacity_[k] = 1.0;
  }
  tool_ink_[tool_index(Tool::Squiggly)] = Rgb{0.95, 0.55, 0.1};
  tool_ink_[tool_index(Tool::Ellipse)] = Rgb{0.11, 0.49, 0.84};
  tool_ink_[tool_index(Tool::Arrow)] = Rgb{0.85, 0.18, 0.2};
  tool_ink_[tool_index(Tool::Measure)] = Rgb{0.11, 0.49, 0.84};
  tool_ink_[tool_index(Tool::Stamp)] = Rgb{0.18, 0.62, 0.27};
  for (Tool t : {Tool::FillText, Tool::FillCheck, Tool::FillCross, Tool::FillDot, Tool::FillDate, Tool::FillInitials})
    tool_ink_[tool_index(t)] = Rgb{0.08, 0.12, 0.2};
  tool_ink_[tool_index(Tool::Highlight)] = Rgb{1.0, 0.84, 0.15};
  tool_ink_[tool_index(Tool::Underline)] = Rgb{0.13, 0.66, 0.36};
  tool_ink_[tool_index(Tool::Strike)] = Rgb{0.85, 0.18, 0.2};
  tool_ink_[tool_index(Tool::Note)] = Rgb{1.0, 0.84, 0.15};
  tool_ink_[tool_index(Tool::Text)] = Rgb{0.08, 0.12, 0.2};
  tool_ink_[tool_index(Tool::Pen)] = Rgb{0.13, 0.45, 0.85};
  tool_ink_[tool_index(Tool::Rect)] = Rgb{0.85, 0.18, 0.2};
  tool_ink_[tool_index(Tool::Line)] = Rgb{0.85, 0.18, 0.2};
  tool_ink_[tool_index(Tool::Select)] = Rgb{1.0, 0.84, 0.15};
  tool_ink_[tool_index(Tool::Signature)] = Rgb{0.08, 0.12, 0.2};
}

DocView::~DocView() {
  live_views().erase(this);
  Fl::remove_timeout(animate_cb, this);
  Fl::remove_timeout(settle_cb, this);
  Fl::remove_timeout(find_cb, this);
  Fl::remove_timeout(schedule_cb, this);
  renderer_.reset();
  clear_steps(undo_);
  clear_steps(redo_);
  if (signature_) cairo_surface_destroy(signature_);
  if (image_) cairo_surface_destroy(image_);
}

void DocView::set_cache_budget(size_t bytes) { cache_->set_budget(std::max<size_t>(bytes, 32u << 20)); }

// ------------------------------------------------------------- document

void DocView::set_document(Document *doc) {
  renderer_.reset();  // joins the thread before the old document goes away
  cache_->clear();
  thumb_jobs_.clear();
  view_jobs_.clear();
  clear_steps(undo_);
  clear_steps(redo_);
  redactions_.clear();
  clear_selection();
  find_clear();
  annot_sel_page_ = -1;
  drag_ = Drag::Idle;
  doc_ = doc;
  current_ = 0;
  sx_ = sy_ = tx_ = ty_ = 0;
  if (doc_) {
    DocView *self = this;
    renderer_.reset(new Renderer(doc_, [self] { Fl::awake(results_cb, self); }));
  }
  relayout(false);
  redraw();
}

RenderParams DocView::params() const {
  RenderParams rp;
  rp.scale = zoom_ * base_scale();
  rp.rotation = rotation_;
  rp.mode = mode_;
  rp.annots = show_annots_;
  return rp;
}

double DocView::base_scale() const { return 96.0 / 72.0 * ui_scale(); }

RenderParams DocView::thumb_params(int page, int width) const {
  RenderParams rp;
  bool turned = rotation_ == 90 || rotation_ == 270;
  double pw = turned ? doc_->height(page) : doc_->width(page);
  rp.scale = width / std::max(1.0, pw);
  rp.rotation = rotation_;
  rp.mode = mode_;
  rp.annots = show_annots_;
  return rp;
}

int DocView::thumb_height(int page, int width) const {
  if (!doc_) return width;
  bool turned = rotation_ == 90 || rotation_ == 270;
  double pw = turned ? doc_->height(page) : doc_->width(page), ph = turned ? doc_->width(page) : doc_->height(page);
  return std::max(1, int(std::lround(width * ph / std::max(1.0, pw))));
}

// ---------------------------------------------------------------- layout

double DocView::fit_zoom(Fit f) const {
  if (!doc_) return 1.0;
  int m = presentation_ ? 0 : S(22), gap = presentation_ ? 0 : S(14);
  bool turned = rotation_ == 90 || rotation_ == 270;
  auto pw = [&](int i) { return turned ? doc_->height(i) : doc_->width(i); };
  auto ph = [&](int i) { return turned ? doc_->width(i) : doc_->height(i); };
  double avail_w = std::max(50, view_w() - 2 * m), avail_h = std::max(50, view_h() - 2 * m);
  double widest = 0;
  int n = doc_->count();
  if (layout_ == Layout::Book) {
    widest = pw(0);
    for (int i = 1; i < n; i += 2) widest = std::max(widest, pw(i) + (i + 1 < n ? pw(i + 1) : 0));
    widest = std::max(widest, 2 * pw(0));
    avail_w -= gap;
  } else if (layout_ == Layout::Single || f == Fit::Page) {
    widest = pw(current_);
  } else {
    for (int i = 0; i < n && i < 4000; ++i) widest = std::max(widest, pw(i));
  }
  double z = avail_w / (std::max(1.0, widest) * base_scale());
  if (f == Fit::Page) z = std::min(z, avail_h / (std::max(1.0, ph(current_)) * base_scale()));
  return std::clamp(z, kMinZoom, kMaxZoom);
}

void DocView::relayout(bool keep_position) {
  if (!doc_) {
    slots_.clear();
    canvas_w_ = view_w();
    canvas_h_ = view_h();
    return;
  }
  int n = doc_->count();
  // Where are we now? Keep the same spot of the same page in view.
  int anchor = std::clamp(current_, 0, n - 1);
  double frac = 0, hfrac = 0.5;
  if (keep_position && size_t(anchor) < slots_.size() && slots_[size_t(anchor)].h > 0) {
    const Slot &s = slots_[size_t(anchor)];
    frac = (sy_ - s.y) / s.h;
    if (canvas_w_ > view_w()) hfrac = sx_ / std::max(1, canvas_w_ - view_w());
  }
  if (fit_ != Fit::Free) zoom_ = fit_zoom(fit_);
  RenderParams rp = params();
  int m = presentation_ ? 0 : S(22), gap = presentation_ ? 0 : S(14);
  slots_.assign(size_t(n), Slot{});
  for (int i = 0; i < n; ++i) page_pixels(doc_->width(i), doc_->height(i), rp, slots_[size_t(i)].w, slots_[size_t(i)].h);
  int widest = 0, y = m;
  if (layout_ == Layout::Single) {
    Slot &s = slots_[size_t(anchor)];
    s.shown = true;
    s.y = y;
    y += s.h + gap;
    widest = s.w;
  } else if (layout_ == Layout::Book) {
    // A book: the first page alone on the right, then facing pairs.
    for (int i = 0; i < n;) {
      int left = i == 0 ? -1 : i, right = i == 0 ? 0 : (i + 1 < n ? i + 1 : -1);
      int rh = 0, rw = 0;
      for (int p : {left, right})
        if (p >= 0) {
          slots_[size_t(p)].shown = true;
          slots_[size_t(p)].y = y;
          rh = std::max(rh, slots_[size_t(p)].h);
        }
      int lw = left >= 0 ? slots_[size_t(left)].w : (right >= 0 ? slots_[size_t(right)].w : 0);
      int rwid = right >= 0 ? slots_[size_t(right)].w : lw;
      rw = 2 * std::max(lw, rwid) + gap;
      widest = std::max(widest, rw);
      y += rh + gap;
      i = i == 0 ? 1 : i + 2;
    }
  } else {
    for (int i = 0; i < n; ++i) {
      Slot &s = slots_[size_t(i)];
      s.shown = true;
      s.y = y;
      y += s.h + gap;
      widest = std::max(widest, s.w);
    }
  }
  canvas_h_ = y - gap + m;
  canvas_w_ = std::max(view_w(), widest + 2 * m);
  int cx = canvas_w_ / 2;
  for (int i = 0; i < n; ++i) {
    Slot &s = slots_[size_t(i)];
    if (!s.shown) continue;
    if (layout_ == Layout::Book) {
      bool right = i == 0 || i % 2 == 0;
      s.x = right ? cx + gap / 2 : cx - (gap - gap / 2) - s.w;
    } else {
      s.x = (canvas_w_ - s.w) / 2;
    }
  }
  if (canvas_h_ < view_h()) {
    int off = (view_h() - canvas_h_) / 2;
    for (auto &s : slots_) s.y += off;
    canvas_h_ = view_h();
  }
  if (keep_position) {
    const Slot &s = slots_[size_t(anchor)];
    sy_ = s.y + frac * s.h;
    sx_ = hfrac * std::max(0, canvas_w_ - view_w());
  }
  clamp_scroll();
  tx_ = sx_;
  ty_ = sy_;
}

void DocView::clamp_scroll() {
  double mx = std::max(0, canvas_w_ - view_w()), my = std::max(0, canvas_h_ - view_h());
  sx_ = std::clamp(sx_, 0.0, mx);
  sy_ = std::clamp(sy_, 0.0, my);
  tx_ = std::clamp(tx_, 0.0, mx);
  ty_ = std::clamp(ty_, 0.0, my);
}

void DocView::resize(int x, int y, int w, int h) {
  bool changed = w != this->w() || h != this->h();
  Fl_Widget::resize(x, y, w, h);
  if (changed) relayout(true);
}

void DocView::set_fit(Fit f) {
  fit_ = f;
  if (f != Fit::Free) {
    settled_ = false;
    Fl::remove_timeout(settle_cb, this);
    Fl::add_timeout(kSettleDelay, settle_cb, this);
  }
  relayout(true);
  app_->update_chrome();
  redraw();
}

void DocView::set_zoom(double z, int ax, int ay) {
  if (!doc_) return;
  z = std::clamp(z, kMinZoom, kMaxZoom);
  if (ax < 0) {
    ax = x() + w() / 2;
    ay = y() + h() / 2;
  }
  // Keep the document point under the anchor where it is.
  double cx = ax - x() + sx_, cy = ay - y() + sy_;
  int page = -1;
  double px = 0, py = 0;
  bool have = screen_to_page(ax, ay, page, px, py, true);
  fit_ = Fit::Free;
  zoom_ = z;
  relayout(true);
  if (have) {
    const Slot &s = slots_[size_t(page)];
    double dx, dy;
    page_to_device(doc_->width(page), doc_->height(page), params(), px, py, dx, dy);
    sx_ = s.x + dx - (ax - x());
    sy_ = s.y + dy - (ay - y());
  } else {
    sx_ = cx;
    sy_ = cy;
  }
  clamp_scroll();
  tx_ = sx_;
  ty_ = sy_;
  settled_ = false;
  Fl::remove_timeout(settle_cb, this);
  Fl::add_timeout(kSettleDelay, settle_cb, this);
  update_current();
  app_->update_chrome();
  redraw();
}

void DocView::zoom_step(int dir, int ax, int ay) {
  static const double steps[] = {0.25, 0.33, 0.5, 0.67, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0,
                                 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0};
  double z = zoom_;
  if (dir > 0) {
    for (double s : steps)
      if (s > z * 1.01) {
        set_zoom(s, ax, ay);
        return;
      }
  } else {
    for (int i = int(sizeof steps / sizeof steps[0]) - 1; i >= 0; --i)
      if (steps[i] < z * 0.99) {
        set_zoom(steps[i], ax, ay);
        return;
      }
  }
}

void DocView::settle_cb(void *p) {
  auto *self = static_cast<DocView *>(p);
  self->settled_ = true;
  self->redraw();
}

void DocView::set_layout(Layout l) {
  layout_ = l;
  relayout(true);
  if (l == Layout::Single) go_to(current_);
  app_->update_chrome();
  redraw();
}

void DocView::rotate(int delta) {
  rotation_ = ((rotation_ + delta) % 360 + 360) % 360;
  cache_->clear();
  relayout(true);
  go_to(current_);
  app_->update_chrome();
  if (app_->sidebar()) app_->sidebar()->redraw();
  redraw();
}

void DocView::set_color_mode(ColorMode m) {
  mode_ = m;
  app_->update_chrome();
  if (app_->sidebar()) app_->sidebar()->redraw();
  redraw();
}

void DocView::set_presentation(bool on) {
  if (on == presentation_) return;
  if (on) {
    saved_layout_ = layout_;
    saved_fit_ = fit_;
    saved_zoom_ = zoom_;
    presentation_ = true;
    layout_ = Layout::Single;
    fit_ = Fit::Page;
  } else {
    presentation_ = false;
    layout_ = saved_layout_;
    fit_ = saved_fit_;
    zoom_ = saved_zoom_;
  }
  int page = current_;
  relayout(false);
  go_to(page);
  redraw();
}

// ------------------------------------------------------------ navigation

void DocView::update_current() {
  if (!doc_ || slots_.empty()) return;
  int old = current_;
  if (layout_ != Layout::Single) {
    double probe = sy_ + view_h() * 0.3;
    int best = current_;
    for (int i = 0; i < int(slots_.size()); ++i) {
      const Slot &s = slots_[size_t(i)];
      if (!s.shown) continue;
      if (s.y <= probe) best = i;
      else break;
    }
    // In a book row, the left page names the spread.
    current_ = best;
    if (layout_ == Layout::Book && best > 0 && best % 2 == 0 && slots_[size_t(best - 1)].y == slots_[size_t(best)].y)
      current_ = best - 1;
  }
  if (current_ != old) app_->page_changed();
}

void DocView::go_to(int page, double top, bool smooth) {
  if (!doc_) return;
  page = std::clamp(page, 0, doc_->count() - 1);
  if (layout_ == Layout::Single && page != current_) {
    current_ = page;
    relayout(false);
    if (fit_ == Fit::Page) relayout(false);
    sy_ = ty_ = 0;
    smooth = false;
  }
  const Slot &s = slots_[size_t(page)];
  double target = s.y - (presentation_ ? 0 : S(10));
  if (top >= 0 && (rotation_ == 0 || rotation_ == 180)) {
    double dx, dy;
    page_to_device(doc_->width(page), doc_->height(page), params(), 0, top, dx, dy);
    target = s.y + std::min(dy, double(s.h)) - S(6);
  }
  ty_ = target;
  if (canvas_w_ > view_w() && (s.x < sx_ || s.x + s.w > sx_ + view_w())) tx_ = s.x - S(10);
  clamp_scroll();
  if (!smooth) {
    sx_ = tx_;
    sy_ = ty_;
    animating_ = false;
    Fl::remove_timeout(animate_cb, this);
  } else if (!animating_) {
    animating_ = true;
    Fl::add_timeout(1.0 / 60, animate_cb, this);
  }
  int old = current_;
  current_ = page;
  if (!smooth) update_current();
  if (current_ != old || !smooth) app_->page_changed();
  redraw();
}

void DocView::next_page() {
  if (!doc_) return;
  int step = layout_ == Layout::Book ? (current_ == 0 ? 1 : 2) : 1;
  if (current_ + step < doc_->count()) go_to(current_ + step, -1, layout_ != Layout::Single);
}

void DocView::prev_page() {
  if (!doc_) return;
  int step = layout_ == Layout::Book ? (current_ <= 1 ? 1 : 2) : 1;
  if (current_ > 0) go_to(std::max(0, current_ - step), -1, layout_ != Layout::Single);
}

void DocView::scroll_by(double dx, double dy, bool smooth) {
  if (!doc_) return;
  double my = std::max(0, canvas_h_ - view_h());
  if (layout_ == Layout::Single && !presentation_) {
    if (dy > 0 && ty_ >= my - 0.5 && current_ + 1 < doc_->count()) {
      go_to(current_ + 1);
      return;
    }
    if (dy < 0 && ty_ <= 0.5 && current_ > 0) {
      go_to(current_ - 1);
      sy_ = ty_ = std::max(0, canvas_h_ - view_h());
      redraw();
      return;
    }
  }
  tx_ += dx;
  ty_ += dy;
  clamp_scroll();
  if (!smooth) {
    sx_ = tx_;
    sy_ = ty_;
    update_current();
    redraw();
    return;
  }
  if (!animating_) {
    animating_ = true;
    Fl::add_timeout(1.0 / 60, animate_cb, this);
  }
}

void DocView::scroll_pages(int dir) {
  if (presentation_) {
    if (dir > 0) next_page();
    else prev_page();
    return;
  }
  scroll_by(0, dir * (view_h() - S(48)));
}

void DocView::animate_cb(void *p) { static_cast<DocView *>(p)->animate(); }

void DocView::animate() {
  double ddx = tx_ - sx_, ddy = ty_ - sy_;
  if (std::fabs(ddx) < 0.6 && std::fabs(ddy) < 0.6) {
    sx_ = tx_;
    sy_ = ty_;
    animating_ = false;
  } else {
    sx_ += ddx * 0.32;
    sy_ += ddy * 0.32;
  }
  update_current();
  redraw();
  if (animating_) Fl::repeat_timeout(1.0 / 60, animate_cb, this);
}

void DocView::follow(const LinkTarget &t) {
  switch (t.kind) {
    case LinkTarget::Page: go_to(t.page, t.top, true); break;
    case LinkTarget::Uri:
      if (!open_uri(t.text)) app_->toast(_("No application can open this link"));
      break;
    case LinkTarget::File: {
      std::string f = t.text;
      if (!f.empty() && f[0] != '/' && doc_) f = doc_->path().substr(0, doc_->path().rfind('/') + 1) + f;
      if (!file_exists(f)) app_->toast(format(_("“%s” was not found"), t.text.c_str()));
      else if (ends_with(lower(f), ".pdf")) spawn_detached({"xanara", f});
      else open_uri(file_uri(f));
      break;
    }
    case LinkTarget::Named:
      if (t.text == "NextPage") next_page();
      else if (t.text == "PrevPage") prev_page();
      else if (t.text == "FirstPage") go_to(0);
      else if (t.text == "LastPage" && doc_) go_to(doc_->count() - 1);
      else if (t.text == "Find") app_->show_find(true);
      else if (t.text == "Print") app_->print();
      break;
    case LinkTarget::Nothing: break;
  }
}

// ----------------------------------------------------------- coordinates

bool DocView::screen_to_page(int mx, int my, int &page, double &px, double &py, bool nearest) const {
  if (!doc_) return false;
  double cx = mx - x() + std::round(sx_), cy = my - y() + std::round(sy_);
  int best = -1;
  double best_d = 1e18;
  for (int i = 0; i < int(slots_.size()); ++i) {
    const Slot &s = slots_[size_t(i)];
    if (!s.shown) continue;
    double ddx = cx < s.x ? s.x - cx : (cx > s.x + s.w ? cx - s.x - s.w : 0);
    double ddy = cy < s.y ? s.y - cy : (cy > s.y + s.h ? cy - s.y - s.h : 0);
    double d = ddx * ddx + ddy * ddy;
    if (d < best_d) {
      best_d = d;
      best = i;
    }
    if (d == 0) break;
  }
  if (best < 0 || (!nearest && best_d > 0)) return false;
  const Slot &s = slots_[size_t(best)];
  double dx = std::clamp(cx - s.x, 0.0, double(s.w)), dy = std::clamp(cy - s.y, 0.0, double(s.h));
  page = best;
  device_to_page(doc_->width(best), doc_->height(best), params(), dx, dy, px, py);
  return true;
}

void DocView::page_box_device(int page, const Box &b, double &bx, double &by, double &bw, double &bh) const {
  RenderParams rp = params();
  double ax, ay, cx, cy;
  page_to_device(doc_->width(page), doc_->height(page), rp, b.x1, b.y1, ax, ay);
  page_to_device(doc_->width(page), doc_->height(page), rp, b.x2, b.y2, cx, cy);
  const Slot &s = slots_[size_t(page)];
  bx = x() + s.x - std::round(sx_) + std::min(ax, cx);
  by = y() + s.y - std::round(sy_) + std::min(ay, cy);
  bw = std::fabs(cx - ax);
  bh = std::fabs(cy - ay);
}

// --------------------------------------------------------------- drawing

void DocView::draw() {
  Painter p(window()->w(), window()->h());
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_rectangle(cr, x(), y(), w(), h());
  cairo_clip(cr);
  const Theme &t = app_->theme();
  Color desk = presentation_ ? Color::hex(0x000000)
               : mode_ == ColorMode::Night ? Color::hex(0x0F1115)
               : t.dark ? t.background.mix(Color::hex(0x000000), 0.28)
                        : t.background.mix(Color::hex(0xB9C2CE), 0.42);
  p.fill_rect(x(), y(), w(), h(), desk);
  if (!doc_) {
    cairo_restore(cr);
    return;
  }
  view_jobs_.clear();
  std::vector<RenderJob> low;
  int top = int(std::round(sy_)), left = int(std::round(sx_));
  for (int i = 0; i < int(slots_.size()); ++i) {
    const Slot &s = slots_[size_t(i)];
    if (!s.shown) continue;
    bool visible = s.y < top + h() && s.y + s.h > top && s.x < left + w() && s.x + s.w > left;
    if (visible) {
      draw_page(p, i, view_jobs_);
      draw_overlays(p, i);
    } else if (settled_ && s.y < top + h() * 2 && s.y + s.h > top - h()) {
      // Prefetch the tiles of the pages just outside the view.
      RenderParams rp = params();
      int rev = int(doc_->revision(i));
      int y0 = std::max(0, top - h() - s.y), y1 = std::min(s.h, top + 2 * h() - s.y);
      int x0 = std::max(0, left - s.x), x1 = std::min(s.w, left + w() - s.x);
      for (int r = y0 / kTileSize; r * kTileSize < y1; ++r)
        for (int c = x0 / kTileSize; c * kTileSize < x1; ++c) {
          RenderJob j;
          j.key = TileKey{i, c, r, quantize_scale(rp.scale), rotation_, mode_key(), uint64_t(rev)};
          j.params = rp;
          j.tx = c * kTileSize;
          j.ty = r * kTileSize;
          j.tw = std::min(kTileSize, s.w - j.tx);
          j.th = std::min(kTileSize, s.h - j.ty);
          if (!cache_->contains(j.key)) low.push_back(j);
        }
    }
  }
  for (auto &j : low) view_jobs_.push_back(j);
  draw_preview(p);
  draw_scrollbars(p);
  cairo_restore(cr);
  p.flush();
  if (!hover_tip_.empty()) {
    text_font(t.font_size - S(1));
    int tw = std::min(text_width(hover_tip_) + S(20), S(420)), th = S(28);
    int hx = std::min(hover_x_ + S(14), x() + w() - tw - S(6)), hy = hover_y_ + S(20);
    if (hy + th > y() + h()) hy = hover_y_ - th - S(8);
    {
      Painter tp(window()->w(), window()->h());
      tp.shadow(hx, hy, tw, th, S(6), t.shadow, S(5));
      tp.fill_rounded(hx, hy, tw, th, S(6), t.surface);
      tp.stroke_rounded(hx + 0.5, hy + 0.5, tw - 1, th - 1, S(6), t.border);
    }
    draw_text(hover_tip_, hx + S(10), hy, tw - S(20), th, t.text);
  }
  if (!schedule_pending_) {
    schedule_pending_ = true;
    Fl::add_timeout(0, schedule_cb, this);
  }
}

void DocView::draw_page(Painter &p, int i, std::vector<RenderJob> &jobs) {
  cairo_t *cr = p.cr();
  const Slot &s = slots_[size_t(i)];
  int ox = x() + s.x - int(std::round(sx_)), oy = y() + s.y - int(std::round(sy_));
  RenderParams rp = params();
  if (!presentation_) {
    p.shadow(ox, oy, s.w, s.h, S(2), app_->theme().shadow.alpha(app_->theme().dark ? 0.5 : 0.18), S(6));
  }
  p.fill_rect(ox, oy, s.w, s.h, paper_color(mode_));
  int sq = quantize_scale(rp.scale);
  uint64_t rev = doc_->revision(i);
  int vx0 = std::max(0, x() - ox), vx1 = std::min(s.w, x() + w() - ox);
  int vy0 = std::max(0, y() - oy), vy1 = std::min(s.h, y() + h() - oy);
  if (vx1 <= vx0 || vy1 <= vy0) return;
  struct Have {
    cairo_surface_t *surface;
    int tx, ty;
  };
  std::vector<Have> have;
  bool missing = false;
  for (int r = vy0 / kTileSize; r * kTileSize < vy1; ++r)
    for (int c = vx0 / kTileSize; c * kTileSize < vx1; ++c) {
      TileKey key{i, c, r, sq, rotation_, mode_key(), rev};
      if (cairo_surface_t *t = cache_->get(key)) {
        have.push_back({t, c * kTileSize, r * kTileSize});
        continue;
      }
      missing = true;
      if (settled_) {
        RenderJob j;
        j.key = key;
        j.params = rp;
        j.tx = c * kTileSize;
        j.ty = r * kTileSize;
        j.tw = std::min(kTileSize, s.w - j.tx);
        j.th = std::min(kTileSize, s.h - j.ty);
        jobs.push_back(j);
      }
    }
  if (missing) {
    cairo_save(cr);
    cairo_rectangle(cr, ox, oy, s.w, s.h);
    cairo_clip(cr);
    // 1. The page thumbnail, stretched.
    int tw = S(128);
    RenderParams tp = thumb_params(i, tw);
    TileKey tkey{i, -1, 0, quantize_scale(tp.scale), rotation_, mode_key(), rev};
    cairo_surface_t *thumb = cache_->get(tkey);
    if (!thumb) {
      cache_->each_of_page(i, [&](const TileKey &k, cairo_surface_t *sf) {
        if (!thumb && k.col < 0 && k.rotation == rotation_ && k.mode == mode_key()) thumb = sf;
      });
      int th = thumb_height(i, tw);
      RenderJob j;
      j.key = tkey;
      j.params = tp;
      j.tw = tw;
      j.th = th;
      thumb_jobs_.push_back(j);
    }
    if (thumb) {
      double f = double(s.w) / cairo_image_surface_get_width(thumb);
      cairo_save(cr);
      cairo_translate(cr, ox, oy);
      cairo_scale(cr, f, f);
      cairo_set_source_surface(cr, thumb, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
      cairo_paint(cr);
      cairo_restore(cr);
    }
    // 2. Tiles of the nearest other scale (e.g. the zoom before this one).
    int best_q = 0;
    double best_score = 1e9;
    cache_->each_of_page(i, [&](const TileKey &k, cairo_surface_t *) {
      if (k.col < 0 || k.rotation != rotation_ || k.mode != mode_key() || k.scale_q == sq) return;
      double ratio = std::log(double(k.scale_q) / sq);
      double score = std::fabs(ratio) + (ratio < 0 ? 0.3 : 0) + (k.revision != rev ? 2 : 0);
      if (score < best_score) {
        best_score = score;
        best_q = k.scale_q;
      }
    });
    if (best_q) {
      double f = double(sq) / best_q;
      cache_->each_of_page(i, [&](const TileKey &k, cairo_surface_t *sf) {
        if (k.col < 0 || k.scale_q != best_q || k.rotation != rotation_ || k.mode != mode_key()) return;
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, f, f);
        cairo_set_source_surface(cr, sf, k.col * kTileSize, k.row * kTileSize);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_rectangle(cr, k.col * kTileSize, k.row * kTileSize, cairo_image_surface_get_width(sf),
                        cairo_image_surface_get_height(sf));
        cairo_fill(cr);
        cairo_restore(cr);
      });
    }
    cairo_restore(cr);
  }
  // 3. The exact tiles, one device pixel per tile pixel, on whole pixels.
  for (const auto &hv : have) {
    cairo_set_source_surface(cr, hv.surface, ox + hv.tx, oy + hv.ty);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_rectangle(cr, ox + hv.tx, oy + hv.ty, cairo_image_surface_get_width(hv.surface),
                    cairo_image_surface_get_height(hv.surface));
    cairo_fill(cr);
  }
}

void DocView::draw_overlays(Painter &p, int i) {
  cairo_t *cr = p.cr();
  const Theme &t = app_->theme();
  double bx, by, bw, bh;
  // Form fields.
  if (highlight_fields_ && !presentation_) {
    for (const auto &f : doc_->fields(i)) {
      if (f.read_only || f.type == POPPLER_FORM_FIELD_SIGNATURE || f.type == POPPLER_FORM_FIELD_UNKNOWN) continue;
      page_box_device(i, f.box, bx, by, bw, bh);
      p.fill_rect(bx, by, bw, bh, t.accent.alpha(0.10));
      cairo_set_source_rgba(cr, t.accent.r, t.accent.g, t.accent.b, 0.55);
      cairo_set_line_width(cr, 1);
      cairo_rectangle(cr, std::round(bx) + 0.5, std::round(by) + 0.5, std::round(bw) - 1, std::round(bh) - 1);
      cairo_stroke(cr);
    }
  }
  // Find results.
  for (int k = 0; k < int(hits_.size()); ++k) {
    if (hits_[size_t(k)].page != i) continue;
    page_box_device(i, hits_[size_t(k)].box, bx, by, bw, bh);
    bool cur = k == hit_;
    p.fill_rounded(bx - 1, by - 1, bw + 2, bh + 2, S(2), cur ? Color::hex(0xFF8A00, 0.42) : Color::hex(0xFFD400, 0.38));
    if (cur) p.stroke_rounded(bx - 1.5, by - 1.5, bw + 3, bh + 3, S(2), Color::hex(0xE06A00), Sd(1.5));
  }
  // Redaction marks waiting to be applied.
  for (const auto &rb : redactions_) {
    if (rb.first != i) continue;
    page_box_device(i, rb.second, bx, by, bw, bh);
    p.fill_rect(bx, by, bw, bh, Color::hex(0x000000, 0.42));
    cairo_set_source_rgba(cr, 0.88, 0.19, 0.19, 1);
    cairo_set_line_width(cr, Sd(1.5));
    cairo_rectangle(cr, std::round(bx) + 0.5, std::round(by) + 0.5, std::round(bw) - 1, std::round(bh) - 1);
    cairo_stroke(cr);
    cairo_save(cr);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_clip(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0.88, 0.19, 0.19, 0.6);
    for (double d = -bh; d < bw; d += Sd(8)) {
      cairo_move_to(cr, bx + d, by + bh);
      cairo_line_to(cr, bx + d + bh, by);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
  }
  // Text selection.
  for (const auto &sb : sel_boxes_) {
    if (sb.first != i) continue;
    page_box_device(i, sb.second, bx, by, bw, bh);
    p.fill_rect(bx, by, bw, bh, t.accent.alpha(0.32));
  }
  // The selected annotation.
  if (annot_sel_page_ == i) {
    page_box_device(i, annot_sel_box_, bx, by, bw, bh);
    if (drag_ == Drag::MoveAnnot) {
      bx += move_dx_;
      by += move_dy_;
    }
    static const double dash[] = {4, 3};
    cairo_set_dash(cr, dash, 2, 0);
    cairo_set_source_rgba(cr, t.accent.r, t.accent.g, t.accent.b, 1);
    cairo_set_line_width(cr, Sd(1.5));
    cairo_rectangle(cr, std::round(bx) - 2.5, std::round(by) - 2.5, std::round(bw) + 5, std::round(bh) + 5);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
    double hs = S(6);
    for (double hx : {bx - 2.5, bx + bw + 2.5})
      for (double hy : {by - 2.5, by + bh + 2.5}) {
        p.fill_rect(hx - hs / 2, hy - hs / 2, hs, hs, t.surface);
        cairo_set_source_rgb(cr, t.accent.r, t.accent.g, t.accent.b);
        cairo_rectangle(cr, hx - hs / 2 + 0.5, hy - hs / 2 + 0.5, hs - 1, hs - 1);
        cairo_stroke(cr);
      }
  }
}

void DocView::draw_scrollbars(Painter &p) {
  const Theme &t = app_->theme();
  if (presentation_) return;
  Color thumb = (t.dark || mode_ == ColorMode::Night) ? Color::hex(0xFFFFFF, 0.32) : Color::hex(0x000000, 0.30);
  if (canvas_h_ > view_h()) {
    double track = h() - S(8), len = std::max<double>(S(36), track * view_h() / canvas_h_);
    double pos = (track - len) * sy_ / std::max(1, canvas_h_ - view_h());
    double bw = drag_ == Drag::ScrollV ? S(8) : S(6);
    p.fill_rounded(x() + w() - bw - S(3), y() + S(4) + pos, bw, len, bw / 2, thumb);
  }
  if (canvas_w_ > view_w()) {
    double track = w() - S(16), len = std::max<double>(S(36), track * view_w() / canvas_w_);
    double pos = (track - len) * sx_ / std::max(1, canvas_w_ - view_w());
    double bh = drag_ == Drag::ScrollH ? S(8) : S(6);
    p.fill_rounded(x() + S(4) + pos, y() + h() - bh - S(3), len, bh, bh / 2, thumb);
  }
}

// Live previews of the tool being dragged.
void DocView::draw_preview(Painter &p) {
  if ((drag_ != Drag::Shape && drag_ != Drag::Ink) || drag_page_ < 0) return;
  cairo_t *cr = p.cr();
  const Theme &t = app_->theme();
  Rgb c = ink();
  double op = std::max(0.35, opacity());
  cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.9 * op);
  cairo_set_line_width(cr, std::max(1.0, stroke_ * params().scale));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  auto to_screen = [&](Point pt, double &sxp, double &syp) {
    double bx, by, bw, bh;
    page_box_device(drag_page_, Box{pt.x, pt.y, pt.x, pt.y}, bx, by, bw, bh);
    sxp = bx;
    syp = by;
  };
  double ax, ay, bx2, by2;
  if (drag_ == Drag::Ink) {
    for (size_t i = 0; i < stroke_pts_.size(); ++i) {
      to_screen(stroke_pts_[i], ax, ay);
      if (i == 0) cairo_move_to(cr, ax, ay);
      else cairo_line_to(cr, ax, ay);
    }
    cairo_stroke(cr);
    return;
  }
  to_screen(drag_p0_, ax, ay);
  to_screen(drag_p1_, bx2, by2);
  double x0 = std::min(ax, bx2), y0 = std::min(ay, by2), w0 = std::fabs(bx2 - ax), h0 = std::fabs(by2 - ay);
  switch (tool_) {
    case Tool::Line:
    case Tool::Arrow:
    case Tool::Measure: {
      cairo_move_to(cr, ax, ay);
      cairo_line_to(cr, bx2, by2);
      cairo_stroke(cr);
      if (tool_ == Tool::Arrow) {
        double ang = std::atan2(by2 - ay, bx2 - ax), head = Sd(12) * std::max(0.6, zoom_);
        cairo_move_to(cr, bx2 - head * std::cos(ang + 0.45), by2 - head * std::sin(ang + 0.45));
        cairo_line_to(cr, bx2, by2);
        cairo_line_to(cr, bx2 - head * std::cos(ang - 0.45), by2 - head * std::sin(ang - 0.45));
        cairo_stroke(cr);
      }
      if (tool_ == Tool::Measure) {
        double d = std::hypot(drag_p1_.x - drag_p0_.x, drag_p1_.y - drag_p0_.y);
        std::string label = measure_label(d, "mm");
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, Sd(12));
        cairo_text_extents_t te;
        cairo_text_extents(cr, label.c_str(), &te);
        double lx = (ax + bx2) / 2 - te.width / 2, ly = (ay + by2) / 2 - Sd(8);
        p.fill_rounded(lx - Sd(5), ly - te.height - Sd(4), te.width + Sd(10), te.height + Sd(8), Sd(4), t.surface);
        cairo_set_source_rgb(cr, c.r, c.g, c.b);
        cairo_move_to(cr, lx, ly);
        cairo_show_text(cr, label.c_str());
      }
      break;
    }
    case Tool::Ellipse:
      if (w0 > 1 && h0 > 1) {
        cairo_save(cr);
        cairo_translate(cr, x0 + w0 / 2, y0 + h0 / 2);
        cairo_scale(cr, w0 / 2, h0 / 2);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
        cairo_restore(cr);
        cairo_stroke(cr);
      }
      break;
    case Tool::Rect:
      cairo_rectangle(cr, x0, y0, w0, h0);
      cairo_stroke(cr);
      break;
    case Tool::Redact:
      p.fill_rect(x0, y0, w0, h0, Color::hex(0x000000, 0.45));
      p.stroke_rounded(x0 + 0.5, y0 + 0.5, w0, h0, 0, Color::hex(0xE03131), Sd(1.5));
      break;
    default: {
      // Boxes: text boxes, links, crop, form fields, images.
      static const double dash[] = {4, 3};
      cairo_set_dash(cr, dash, 2, 0);
      cairo_set_line_width(cr, Sd(1.2));
      cairo_set_source_rgba(cr, t.accent.r, t.accent.g, t.accent.b, 1);
      cairo_rectangle(cr, std::round(x0) + 0.5, std::round(y0) + 0.5, std::round(w0), std::round(h0));
      cairo_stroke(cr);
      cairo_set_dash(cr, nullptr, 0, 0);
      if (tool_ == Tool::Crop) {
        // Dim what will be cut away.
        const Slot &s = slots_[size_t(drag_page_)];
        double ox = x() + s.x - std::round(sx_), oy = y() + s.y - std::round(sy_);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
        cairo_rectangle(cr, ox, oy, s.w, s.h);
        cairo_rectangle(cr, x0, y0, w0, h0);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_fill(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
      } else {
        p.fill_rect(x0, y0, w0, h0, t.accent.alpha(0.08));
      }
    }
  }
}

// ------------------------------------------------------------- rendering

void DocView::schedule_cb(void *p) {
  auto *self = static_cast<DocView *>(p);
  self->schedule_pending_ = false;
  self->schedule();
}

void DocView::schedule() {
  if (!renderer_ || !doc_) return;
  std::vector<RenderJob> jobs;
  std::vector<RenderJob> thumbs;
  for (auto &j : thumb_jobs_)
    if (!cache_->contains(j.key)) thumbs.push_back(j);
  thumb_jobs_.clear();
  size_t visible = 0;
  for (auto &j : view_jobs_)
    if (!cache_->contains(j.key)) {
      jobs.push_back(j);
      ++visible;
    }
  // Thumbnails are small and cheap; render them right after the tiles on
  // screen, before the prefetch.
  std::vector<RenderJob> all;
  for (auto &j : jobs)
    if (j.key.page == current_ || all.size() < 24) all.push_back(j);
  for (auto &j : thumbs) all.push_back(j);
  for (size_t k = 0; k < jobs.size(); ++k)
    if (!(jobs[k].key.page == current_ || k < 24)) all.push_back(jobs[k]);
  // Dedupe.
  std::vector<RenderJob> out;
  for (auto &j : all) {
    bool dup = false;
    for (auto &o : out)
      if (o.key == j.key) {
        dup = true;
        break;
      }
    if (!dup) out.push_back(j);
  }
  (void)visible;
  renderer_->replace_queue(std::move(out));
}

void DocView::results_ready() {
  if (!renderer_) return;
  bool thumbs = false;
  for (auto &r : renderer_->take_results()) {
    if (!r.surface) continue;
    if (r.key.col < 0) thumbs = true;
    cache_->put(r.key, r.surface);
  }
  redraw();
  if (thumbs && app_->sidebar()) app_->sidebar()->redraw();
}

cairo_surface_t *DocView::thumbnail(int page, int width) {
  if (!doc_ || page < 0 || page >= doc_->count()) return nullptr;
  RenderParams tp = thumb_params(page, width);
  TileKey key{page, -1, 0, quantize_scale(tp.scale), rotation_, mode_key(), doc_->revision(page)};
  if (cairo_surface_t *s = cache_->get(key)) return s;
  RenderJob j;
  j.key = key;
  j.params = tp;
  j.tw = width;
  j.th = thumb_height(page, width);
  thumb_jobs_.push_back(j);
  if (!schedule_pending_) {
    schedule_pending_ = true;
    Fl::add_timeout(0, schedule_cb, this);
  }
  // A thumbnail of older content or another width meanwhile.
  cairo_surface_t *stale = nullptr;
  cache_->each_of_page(page, [&](const TileKey &k, cairo_surface_t *s) {
    if (!stale && k.col < 0 && k.rotation == rotation_ && k.mode == mode_key()) stale = s;
  });
  return stale;
}

void DocView::edited(int page) {
  // Tiles carry the page revision, so the new content is rendered while the
  // old tiles stay visible as a placeholder until it arrives.
  if (annot_sel_page_ == page) {
    bool found = false;
    for (const auto &a : doc_->annotations(page))
      if (a.type == annot_sel_type_ && std::fabs(a.box.x1 - annot_sel_box_.x1) < 1.5 &&
          std::fabs(a.box.y1 - annot_sel_box_.y1) < 1.5)
        found = true;
    if (!found) annot_sel_page_ = -1;
  }
  app_->document_edited();
  redraw();
}

// ------------------------------------------------------------- selection

void DocView::clear_selection() {
  sel_active_ = selecting_ = false;
  sel_boxes_.clear();
}

void DocView::update_selection() {
  sel_boxes_.clear();
  if (!doc_ || !sel_active_) return;
  int a = sel_page0_, b = sel_page1_;
  Point pa = sel_p0_, pb = sel_p1_;
  if (b < a) {
    std::swap(a, b);
    std::swap(pa, pb);
  }
  for (int pg = a; pg <= b && pg - a < 60; ++pg) {
    Box sel;
    double pw = doc_->width(pg), ph = doc_->height(pg);
    if (a == b) sel = Box{pa.x, pa.y, pb.x, pb.y};
    else if (pg == a) sel = Box{pa.x, pa.y, pw, ph};
    else if (pg == b) sel = Box{0, 0, pb.x, pb.y};
    else sel = Box{0, 0, pw, ph};
    for (const auto &bx : doc_->selection_boxes(pg, sel, sel_style_)) sel_boxes_.push_back({pg, bx});
  }
}

std::string DocView::selection_text() {
  if (!has_selection()) return "";
  int a = sel_page0_, b = sel_page1_;
  Point pa = sel_p0_, pb = sel_p1_;
  if (b < a) {
    std::swap(a, b);
    std::swap(pa, pb);
  }
  std::string out;
  for (int pg = a; pg <= b && pg - a < 60; ++pg) {
    Box sel;
    double pw = doc_->width(pg), ph = doc_->height(pg);
    if (a == b) sel = Box{pa.x, pa.y, pb.x, pb.y};
    else if (pg == a) sel = Box{pa.x, pa.y, pw, ph};
    else if (pg == b) sel = Box{0, 0, pb.x, pb.y};
    else sel = Box{0, 0, pw, ph};
    std::string s = doc_->selected_text(pg, sel, sel_style_);
    if (!out.empty() && !s.empty()) out += "\n";
    out += s;
  }
  return out;
}

void DocView::copy_selection() {
  std::string s = selection_text();
  if (s.empty()) return;
  Fl::copy(s.c_str(), int(s.size()), 1);
  Fl::copy(s.c_str(), int(s.size()), 0);
  app_->toast(_("Copied"));
}

void DocView::select_all() {
  if (!doc_) return;
  sel_active_ = true;
  sel_style_ = POPPLER_SELECTION_GLYPH;
  sel_page0_ = sel_page1_ = current_;
  sel_p0_ = Point{0, 0};
  sel_p1_ = Point{doc_->width(current_), doc_->height(current_)};
  update_selection();
  redraw();
}

void DocView::apply_markup(MarkupKind kind) {
  if (!has_selection()) {
    app_->toast(_("Select some text first"));
    return;
  }
  Tool owner = kind == MarkupKind::Highlight   ? Tool::Highlight
               : kind == MarkupKind::Underline ? Tool::Underline
               : kind == MarkupKind::Squiggly  ? Tool::Squiggly
                                               : Tool::Strike;
  Rgb c = tool_ink_[tool_index(owner)];
  double op = opacity_[tool_index(owner)];
  std::vector<int> pages;
  for (const auto &sb : sel_boxes_)
    if (std::find(pages.begin(), pages.end(), sb.first) == pages.end()) pages.push_back(sb.first);
  for (int pg : pages) {
    std::vector<Box> boxes;
    for (const auto &sb : sel_boxes_)
      if (sb.first == pg) boxes.push_back(sb.second);
    if (PopplerAnnot *a = doc_->add_markup(pg, kind, boxes, c)) {
      if (op < 0.99) doc_->set_annot_opacity(pg, a, op);
      push_undo({UndoStep::Added, pg, a});
    }
  }
  clear_selection();
  for (int pg : pages) edited(pg);
}

// ------------------------------------------------------------ annotations

bool DocView::is_text_tool(Tool t) const {
  return t == Tool::Highlight || t == Tool::Underline || t == Tool::Squiggly || t == Tool::Strike;
}

bool DocView::is_box_tool(Tool t) const {
  return t == Tool::Text || t == Tool::Rect || t == Tool::Ellipse || t == Tool::Link || t == Tool::Crop ||
         t == Tool::Field || t == Tool::Redact || t == Tool::Image || t == Tool::Line || t == Tool::Arrow ||
         t == Tool::Measure;
}

void DocView::set_tool(Tool t) {
  tool_ = t;
  if (t != Tool::Select) annot_sel_page_ = -1;
  // Picking a markup tool with text selected applies it at once.
  if (has_selection() && is_text_tool(t)) {
    apply_markup(t == Tool::Highlight   ? MarkupKind::Highlight
                 : t == Tool::Underline ? MarkupKind::Underline
                 : t == Tool::Squiggly  ? MarkupKind::Squiggly
                                        : MarkupKind::StrikeOut);
  }
  app_->tool_changed();
  redraw();
}

Rgb DocView::ink() const {
  if (tool_ == Tool::Select && annot_sel_page_ >= 0) {
    if (const Annotation *a = const_cast<DocView *>(this)->selected_annot())
      if (a->has_color) return a->color;
  }
  return tool_ink_[tool_index(tool_)];
}

void DocView::set_ink(Rgb c) {
  tool_ink_[tool_index(tool_)] = c;
  if (tool_ == Tool::Select && annot_sel_page_ >= 0) {
    if (const Annotation *a = selected_annot()) {
      UndoStep u{UndoStep::Recolored, annot_sel_page_, POPPLER_ANNOT(g_object_ref(a->annot))};
      u.color = a->color;
      u.color2 = c;
      int pg = annot_sel_page_;
      if (doc_->set_annot_color(pg, a->annot, c)) {
        push_undo(u);
        edited(pg);
      } else {
        g_object_unref(u.annot);
      }
    }
  }
  app_->tool_changed();
}

void DocView::set_opacity(double o) {
  o = std::clamp(o, 0.05, 1.0);
  if (tool_ == Tool::Select && annot_sel_page_ >= 0) {
    set_selected_opacity(o);
    return;
  }
  opacity_[tool_index(tool_)] = o;
  app_->tool_changed();
}

void DocView::set_selected_opacity(double o) {
  const Annotation *a = selected_annot();
  if (!a) return;
  UndoStep u{UndoStep::Faded, annot_sel_page_, POPPLER_ANNOT(g_object_ref(a->annot))};
  u.opacity = a->opacity;
  u.opacity2 = o;
  int pg = annot_sel_page_;
  if (doc_->set_annot_opacity(pg, a->annot, o)) {
    push_undo(u);
    edited(pg);
  } else {
    g_object_unref(u.annot);
    app_->toast(_("This annotation has no opacity setting"));
  }
  app_->tool_changed();
}

void DocView::set_show_annots(bool on) {
  show_annots_ = on;
  redraw();
  if (app_->sidebar()) app_->sidebar()->redraw();
}

const Annotation *DocView::selected_annot() {
  if (!doc_ || annot_sel_page_ < 0) return nullptr;
  for (const auto &a : doc_->annotations(annot_sel_page_))
    if (a.type == annot_sel_type_ && std::fabs(a.box.x1 - annot_sel_box_.x1) < 1.5 &&
        std::fabs(a.box.y1 - annot_sel_box_.y1) < 1.5 && std::fabs(a.box.x2 - annot_sel_box_.x2) < 1.5)
      return &a;
  return nullptr;
}

void DocView::select_annot(int page, const Annotation &a) {
  if (tool_ != Tool::Select) tool_ = Tool::Select;
  reselect_annot(page, a.box, a.type);
  go_to(page, std::max(0.0, a.box.y1 - 40), true);
}

void DocView::reselect_annot(int page, const Box &box, PopplerAnnotType type) {
  annot_sel_page_ = page;
  annot_sel_box_ = box;
  annot_sel_type_ = type;
  app_->tool_changed();
}

int DocView::annot_at(int page, double px, double py) const {
  const auto &list = doc_->annotations(page);
  for (int k = int(list.size()) - 1; k >= 0; --k)
    if (list[size_t(k)].box.contains(px, py, 2)) return k;
  return -1;
}

void DocView::delete_selected_annot() {
  const Annotation *a = selected_annot();
  if (!a) return;
  int pg = annot_sel_page_;
  PopplerAnnot *ref = POPPLER_ANNOT(g_object_ref(a->annot));
  if (doc_->remove_annot(pg, ref)) push_undo({UndoStep::Removed, pg, ref});
  else g_object_unref(ref);
  annot_sel_page_ = -1;
  edited(pg);
  app_->tool_changed();
}

void DocView::erase_at(int page, double px, double py) {
  int k = annot_at(page, px, py);
  if (k < 0) return;
  const Annotation &a = doc_->annotations(page)[size_t(k)];
  PopplerAnnot *ref = POPPLER_ANNOT(g_object_ref(a.annot));
  if (doc_->remove_annot(page, ref)) push_undo({UndoStep::Removed, page, ref});
  else g_object_unref(ref);
  edited(page);
}

void DocView::edit_selected_note() {
  const Annotation *a = selected_annot();
  if (!a || (a->type != POPPLER_ANNOT_TEXT && a->type != POPPLER_ANNOT_FREE_TEXT)) return;
  std::string text = a->contents;
  PopplerAnnot *ref = POPPLER_ANNOT(g_object_ref(a->annot));
  int pg = annot_sel_page_;
  if (app_->ask_text(_("Edit Note"), text, true)) {
    doc_->set_annot_contents(pg, ref, text);
    edited(pg);
  }
  g_object_unref(ref);
}

void DocView::push_undo(UndoStep s) {
  undo_.push_back(s);
  clear_steps(redo_);
  if (undo_.size() > 200) {
    if (undo_.front().annot) g_object_unref(undo_.front().annot);
    undo_.erase(undo_.begin());
  }
}

void DocView::clear_steps(std::vector<UndoStep> &steps) {
  for (auto &u : steps)
    if (u.annot) g_object_unref(u.annot);
  steps.clear();
}

void DocView::forget_history() {
  clear_steps(undo_);
  clear_steps(redo_);
}

// Applies a step backwards (undo) or forwards again (redo).
void DocView::apply_step(const UndoStep &u, bool forward) {
  switch (u.kind) {
    case UndoStep::Added:
      if (forward) doc_->restore_annot(u.page, u.annot);
      else doc_->remove_annot(u.page, u.annot);
      break;
    case UndoStep::Removed:
      if (forward) doc_->remove_annot(u.page, u.annot);
      else doc_->restore_annot(u.page, u.annot);
      break;
    case UndoStep::Moved:
      doc_->move_annot(u.page, u.annot, forward ? u.dx : -u.dx, forward ? u.dy : -u.dy);
      break;
    case UndoStep::Recolored: doc_->set_annot_color(u.page, u.annot, forward ? u.color2 : u.color); break;
    case UndoStep::Faded: doc_->set_annot_opacity(u.page, u.annot, forward ? u.opacity2 : u.opacity); break;
  }
}

void DocView::undo() {
  if (undo_.empty() || !doc_) return;
  UndoStep u = undo_.back();
  undo_.pop_back();
  apply_step(u, false);
  redo_.push_back(u);  // keeps the reference
  annot_sel_page_ = -1;
  edited(u.page);
  app_->tool_changed();
}

void DocView::redo() {
  if (redo_.empty() || !doc_) return;
  UndoStep u = redo_.back();
  redo_.pop_back();
  apply_step(u, true);
  undo_.push_back(u);
  annot_sel_page_ = -1;
  edited(u.page);
  app_->tool_changed();
}

void DocView::set_signature(cairo_surface_t *image) {
  if (signature_) cairo_surface_destroy(signature_);
  signature_ = image;
}

void DocView::set_image(cairo_surface_t *image) {
  if (image_) cairo_surface_destroy(image_);
  image_ = image;
}

void DocView::set_scroll_pos(double sx, double sy) {
  sx_ = tx_ = sx;
  sy_ = ty_ = sy;
  clamp_scroll();
  update_current();
  redraw();
}

const FormField *DocView::field_at(int page, double px, double py) const {
  for (const auto &f : doc_->fields(page))
    if (f.box.contains(px, py)) return &f;
  return nullptr;
}

const Link *DocView::link_at(int page, double px, double py) const {
  for (const auto &l : doc_->links(page))
    if (l.box.contains(px, py)) return &l;
  return nullptr;
}

void DocView::activate_field(int page, const FormField &f) {
  if (f.read_only) return;
  double bx, by, bw, bh;
  page_box_device(page, f.box, bx, by, bw, bh);
  switch (f.type) {
    case POPPLER_FORM_FIELD_BUTTON:
      if (doc_->toggle_field(page, f.field)) edited(page);
      break;
    case POPPLER_FORM_FIELD_TEXT:
      app_->edit_field(page, f, int(bx), int(by), int(std::max(bw, double(S(60)))), int(std::max(bh, double(S(24)))));
      break;
    case POPPLER_FORM_FIELD_CHOICE: {
      PopplerFormField *field = POPPLER_FORM_FIELD(g_object_ref(f.field));
      std::vector<std::string> labels;
      int n = 0;
      {
        std::lock_guard<std::mutex> lock(doc_->mutex());
        n = poppler_form_field_choice_get_n_items(field);
        for (int k = 0; k < n && k < 200; ++k) {
          gchar *s = poppler_form_field_choice_get_item(field, k);
          labels.push_back(s ? s : "");
          g_free(s);
        }
      }
      std::vector<Fl_Menu_Item> items(labels.size() + 2);
      memset(items.data(), 0, sizeof(Fl_Menu_Item) * items.size());
      for (size_t k = 0; k < labels.size(); ++k) {
        items[k].text = labels[k].c_str();
        items[k].labelsize(app_->theme().font_size);
        items[k].labelcolor(app_->theme().text.fl());
      }
      items[labels.size()].text = _("(None)");
      items[labels.size()].labelsize(app_->theme().font_size);
      items[labels.size()].labelcolor(app_->theme().text_dim.fl());
      const Fl_Menu_Item *picked = items.data()->popup(int(bx), int(by + bh));
      if (picked) {
        int idx = int(picked - items.data());
        if (doc_->set_field_choice(page, field, idx < int(labels.size()) ? idx : -1)) edited(page);
      }
      g_object_unref(field);
      break;
    }
    default: break;
  }
}

// ----------------------------------------------------------------- find

void DocView::find_clear() {
  Fl::remove_timeout(find_cb, this);
  hits_.clear();
  hit_ = -1;
  find_page_ = -1;
  query_.clear();
  redraw();
}

void DocView::find(const std::string &query, bool match_case) {
  if (!doc_) return;
  if (query == query_ && match_case == match_case_ && (find_page_ >= 0 || !hits_.empty())) {
    find_next(1);
    return;
  }
  find_clear();
  if (query.empty()) {
    app_->update_chrome();
    return;
  }
  query_ = query;
  match_case_ = match_case;
  find_page_ = current_;
  find_done_ = 0;
  Fl::add_timeout(0, find_cb, this);
  app_->update_chrome();
}

void DocView::find_cb(void *p) { static_cast<DocView *>(p)->find_step(); }

void DocView::find_step() {
  if (!doc_ || find_page_ < 0) return;
  double start = Fl::version() ? 0 : 0;
  (void)start;
  int n = doc_->count();
  for (int k = 0; k < 6 && find_done_ < n; ++k, ++find_done_) {
    int pg = find_page_;
    auto boxes = doc_->find(pg, query_, match_case_, false);
    if (!boxes.empty()) {
      auto pos = std::upper_bound(hits_.begin(), hits_.end(), pg, [](int v, const Hit &h) { return v < h.page; });
      size_t idx = size_t(pos - hits_.begin());
      std::vector<Hit> add;
      for (auto &b : boxes) add.push_back({pg, b});
      hits_.insert(pos, add.begin(), add.end());
      if (hit_ >= 0 && int(idx) <= hit_) hit_ += int(add.size());
      if (hit_ < 0) {
        hit_ = int(idx);
        find_next(0);
      }
    }
    find_page_ = (find_page_ + 1) % n;
  }
  if (find_done_ >= n) {
    find_page_ = -1;
    if (hits_.empty()) app_->toast(format(_("“%s” was not found"), query_.c_str()));
  } else {
    Fl::repeat_timeout(0, find_cb, this);
  }
  app_->update_chrome();
  redraw();
}

void DocView::show_hit(int k) {
  if (k < 0 || k >= int(hits_.size())) return;
  hit_ = k;
  find_next(0);
}

void DocView::find_next(int dir) {
  if (hits_.empty()) return;
  int n = int(hits_.size());
  hit_ = ((hit_ < 0 ? 0 : hit_ + dir) % n + n) % n;
  const Hit &hit = hits_[size_t(hit_)];
  if (layout_ == Layout::Single && hit.page != current_) go_to(hit.page);
  double bx, by, bw, bh;
  page_box_device(hit.page, hit.box, bx, by, bw, bh);
  double cy = by - y() + sy_, cx = bx - x() + sx_;
  if (cy < sy_ + S(30) || cy + bh > sy_ + h() - S(30)) ty_ = cy - h() / 3.0;
  if (cx < sx_ || cx + bw > sx_ + w()) tx_ = cx - w() / 3.0;
  clamp_scroll();
  if (!animating_ && (tx_ != sx_ || ty_ != sy_)) {
    animating_ = true;
    Fl::add_timeout(1.0 / 60, animate_cb, this);
  }
  app_->update_chrome();
  redraw();
}

// ------------------------------------------------------------- events

void DocView::update_cursor(int mx, int my) {
  Fl_Cursor c = FL_CURSOR_DEFAULT;
  std::string tip;
  int page;
  double px, py;
  if (!doc_ || presentation_) {
    window()->cursor(FL_CURSOR_DEFAULT);
    return;
  }
  if (screen_to_page(mx, my, page, px, py, false)) {
    if (tool_ == Tool::Select) {
      if (const FormField *f = field_at(page, px, py)) {
        if (!f->read_only) c = f->type == POPPLER_FORM_FIELD_TEXT ? FL_CURSOR_INSERT : FL_CURSOR_HAND;
      } else if (const Link *l = link_at(page, px, py)) {
        c = FL_CURSOR_HAND;
        if (l->target.kind == LinkTarget::Uri) tip = l->target.text;
        else if (l->target.kind == LinkTarget::Page) tip = format(_("Go to page %d"), l->target.page + 1);
        else if (l->target.kind == LinkTarget::File) tip = l->target.text;
      }
      int k = show_annots_ ? annot_at(page, px, py) : -1;
      if (k >= 0) {
        const Annotation &a = doc_->annotations(page)[size_t(k)];
        if (!a.contents.empty() && (a.type == POPPLER_ANNOT_TEXT || is_markup(a.type) || a.type == POPPLER_ANNOT_FREE_TEXT))
          tip = a.contents;
        if (c == FL_CURSOR_DEFAULT) c = FL_CURSOR_MOVE;
      }
      if (c == FL_CURSOR_DEFAULT) c = FL_CURSOR_INSERT;
    } else if (tool_ == Tool::Hand) {
      c = FL_CURSOR_MOVE;
    } else if (is_text_tool(tool_)) {
      c = FL_CURSOR_INSERT;
    } else if (tool_ == Tool::Eraser) {
      c = annot_at(page, px, py) >= 0 ? FL_CURSOR_HAND : FL_CURSOR_CROSS;
    } else {
      c = FL_CURSOR_CROSS;
    }
  }
  if (tip.size() > 160) tip = tip.substr(0, 157) + "…";
  for (auto &ch : tip)
    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
  if (tip != hover_tip_ || (!tip.empty() && (std::abs(mx - hover_x_) > 2 || std::abs(my - hover_y_) > 2))) {
    hover_tip_ = tip;
    hover_x_ = mx;
    hover_y_ = my;
    redraw();
  }
  window()->cursor(c);
}

int DocView::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (event) {
    case FL_ENTER:
    case FL_MOVE:
      update_cursor(mx, my);
      return 1;
    case FL_LEAVE:
      window()->cursor(FL_CURSOR_DEFAULT);
      if (!hover_tip_.empty()) {
        hover_tip_.clear();
        redraw();
      }
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      return 1;
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
    case FL_DND_LEAVE:
      return 1;
    case FL_PASTE: {
      // Files dropped from a file manager arrive as a list of URIs; each
      // PDF opens in a tab of its own.
      std::string text(Fl::event_text(), size_t(Fl::event_length()));
      for (auto &line : split(text, '\n')) {
        std::string f = trim(line);
        if (!f.empty()) app_->open_path(path_from_argument(f));
      }
      return 1;
    }
    case FL_MOUSEWHEEL: {
      if (!doc_) return 1;
      int dy = Fl::event_dy(), dx = Fl::event_dx();
      if (Fl::event_state() & FL_CTRL) {
        if (dy) set_zoom(zoom_ * std::pow(1.12, -dy), mx, my);
        return 1;
      }
      if (presentation_) {
        if (dy > 0) next_page();
        else if (dy < 0) prev_page();
        return 1;
      }
      if (Fl::event_state() & FL_SHIFT) std::swap(dx, dy);
      scroll_by(dx * S(64), dy * S(64));
      return 1;
    }
    case FL_PUSH: {
      Fl::focus(this);
      hover_tip_.clear();
      app_->commit_field();
      press_x_ = mx;
      press_y_ = my;
      press_sx_ = sx_;
      press_sy_ = sy_;
      if (!doc_) return 1;
      if (presentation_) {
        if (Fl::event_button() == FL_RIGHT_MOUSE) prev_page();
        else next_page();
        return 1;
      }
      // Scrollbars.
      if (canvas_h_ > view_h() && mx >= x() + w() - S(14)) {
        drag_ = Drag::ScrollV;
        return 1;
      }
      if (canvas_w_ > view_w() && my >= y() + h() - S(14)) {
        drag_ = Drag::ScrollH;
        return 1;
      }
      if (Fl::event_button() == FL_MIDDLE_MOUSE || (tool_ == Tool::Hand && Fl::event_button() == FL_LEFT_MOUSE)) {
        drag_ = Drag::Pan;
        window()->cursor(FL_CURSOR_MOVE);
        return 1;
      }
      int page;
      double px, py;
      bool on_page = screen_to_page(mx, my, page, px, py, false);
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (on_page && tool_ == Tool::Select && show_annots_) {
          int k = annot_at(page, px, py);
          if (k >= 0) {
            const Annotation &a = doc_->annotations(page)[size_t(k)];
            reselect_annot(page, a.box, a.type);
            redraw();
          }
        }
        open_context_menu(mx, my);
        return 1;
      }
      if (!on_page) {
        clear_selection();
        if (annot_sel_page_ >= 0) {
          annot_sel_page_ = -1;
          app_->tool_changed();
        }
        drag_ = Drag::Pan;
        redraw();
        return 1;
      }
      drag_page_ = page;
      drag_p0_ = drag_p1_ = Point{px, py};
      if (tool_ == Tool::Select) {
        if (const FormField *f = field_at(page, px, py)) {
          if (!f->read_only) {
            FormField copy = *f;
            copy.field = POPPLER_FORM_FIELD(g_object_ref(f->field));
            activate_field(page, copy);
            g_object_unref(copy.field);
            drag_ = Drag::Idle;
            return 1;
          }
        }
        int k = show_annots_ ? annot_at(page, px, py) : -1;
        if (k >= 0) {
          const Annotation &a = doc_->annotations(page)[size_t(k)];
          reselect_annot(page, a.box, a.type);
          clear_selection();
          move_dx_ = move_dy_ = 0;
          drag_ = Drag::MoveAnnot;
          if (Fl::event_clicks() > 0 && (a.type == POPPLER_ANNOT_TEXT || a.type == POPPLER_ANNOT_FREE_TEXT)) {
            drag_ = Drag::Idle;
            edit_selected_note();
          }
          redraw();
          return 1;
        }
        if (annot_sel_page_ >= 0) {
          annot_sel_page_ = -1;
          app_->tool_changed();
        }
        if (const Link *l = link_at(page, px, py)) {
          press_link_ = l->target;
          drag_ = Drag::Link;
          return 1;
        }
      }
      if (tool_ == Tool::Select || is_text_tool(tool_)) {
        drag_ = Drag::Select;
        int clicks = Fl::event_clicks();
        sel_style_ = clicks >= 2 ? POPPLER_SELECTION_LINE : clicks == 1 ? POPPLER_SELECTION_WORD : POPPLER_SELECTION_GLYPH;
        if (Fl::event_state() & FL_SHIFT && sel_active_) {
          sel_page1_ = page;
          sel_p1_ = Point{px, py};
        } else {
          sel_page0_ = sel_page1_ = page;
          sel_p0_ = sel_p1_ = Point{px, py};
          sel_active_ = sel_style_ != POPPLER_SELECTION_GLYPH;
        }
        selecting_ = true;
        if (sel_active_) update_selection();
        redraw();
        return 1;
      }
      if (tool_ == Tool::Pen) {
        drag_ = Drag::Ink;
        stroke_pts_.assign(1, Point{px, py});
        return 1;
      }
      if (tool_ == Tool::Eraser) {
        drag_ = Drag::Erase;
        erase_at(page, px, py);
        return 1;
      }
      drag_ = Drag::Shape;
      return 1;
    }
    case FL_DRAG: {
      if (drag_ == Drag::Pan) {
        sx_ = tx_ = press_sx_ - (mx - press_x_);
        sy_ = ty_ = press_sy_ - (my - press_y_);
        clamp_scroll();
        update_current();
        redraw();
        return 1;
      }
      if (drag_ == Drag::ScrollV) {
        double track = h() - S(8), len = std::max<double>(S(36), track * view_h() / canvas_h_);
        sy_ = ty_ = press_sy_ + (my - press_y_) * (canvas_h_ - view_h()) / std::max(1.0, track - len);
        clamp_scroll();
        update_current();
        redraw();
        return 1;
      }
      if (drag_ == Drag::ScrollH) {
        double track = w() - S(16), len = std::max<double>(S(36), track * view_w() / canvas_w_);
        sx_ = tx_ = press_sx_ + (mx - press_x_) * (canvas_w_ - view_w()) / std::max(1.0, track - len);
        clamp_scroll();
        redraw();
        return 1;
      }
      // Scroll while dragging near the edges.
      if (drag_ == Drag::Select || drag_ == Drag::Ink || drag_ == Drag::Shape || drag_ == Drag::MoveAnnot) {
        if (my > y() + h() - S(16)) scroll_by(0, S(14), false);
        else if (my < y() + S(16)) scroll_by(0, -S(14), false);
      }
      int page;
      double px, py;
      if (drag_ == Drag::Erase) {
        if (screen_to_page(mx, my, page, px, py, false)) erase_at(page, px, py);
        return 1;
      }
      if (drag_ == Drag::Select) {
        if (screen_to_page(mx, my, page, px, py, true)) {
          sel_page1_ = page;
          sel_p1_ = Point{px, py};
          if (std::abs(mx - press_x_) + std::abs(my - press_y_) > 3) sel_active_ = true;
          update_selection();
          redraw();
        }
        return 1;
      }
      if (drag_ == Drag::Link) {
        if (std::abs(mx - press_x_) + std::abs(my - press_y_) > 4) {
          drag_ = Drag::Select;
          sel_style_ = POPPLER_SELECTION_GLYPH;
          sel_page0_ = sel_page1_ = drag_page_;
          sel_p0_ = drag_p0_;
          selecting_ = true;
        }
        return 1;
      }
      if (drag_ == Drag::MoveAnnot) {
        move_dx_ = mx - press_x_;
        move_dy_ = my - press_y_;
        redraw();
        return 1;
      }
      if ((drag_ == Drag::Ink || drag_ == Drag::Shape) && drag_page_ >= 0) {
        // Stay on the page the drag started on.
        double cx = mx - x() + std::round(sx_), cy = my - y() + std::round(sy_);
        const Slot &s = slots_[size_t(drag_page_)];
        double dx = std::clamp(cx - s.x, 0.0, double(s.w)), dy = std::clamp(cy - s.y, 0.0, double(s.h));
        device_to_page(doc_->width(drag_page_), doc_->height(drag_page_), params(), dx, dy, px, py);
        drag_p1_ = Point{px, py};
        if (drag_ == Drag::Ink) {
          const Point &last = stroke_pts_.back();
          double minstep = 0.6 / std::max(0.2, zoom_);
          if (std::hypot(px - last.x, py - last.y) >= minstep) stroke_pts_.push_back(drag_p1_);
        }
        redraw();
      }
      return 1;
    }
    case FL_RELEASE:
      finish_drag(mx, my);
      update_cursor(mx, my);
      return 1;
    case FL_KEYBOARD:
      return keyboard(Fl::event_key(), Fl::event_state()) ? 1 : 0;
  }
  return Fl_Widget::handle(event);
}

PopplerAnnot *DocView::finish_added(int page, PopplerAnnot *a) {
  if (!a) return nullptr;
  double op = opacity_[tool_index(tool_)];
  if (op < 0.99) doc_->set_annot_opacity(page, a, op);
  push_undo({UndoStep::Added, page, a});
  edited(page);
  return a;
}

void DocView::finish_drag(int mx, int my) {
  Drag d = drag_;
  drag_ = Drag::Idle;
  if (d == Drag::Pan || d == Drag::ScrollV || d == Drag::ScrollH || d == Drag::Erase) {
    redraw();
    return;
  }
  if (!doc_) return;
  bool moved = std::abs(mx - press_x_) + std::abs(my - press_y_) > 3;
  int pg = drag_page_;
  if (d == Drag::Link) {
    follow(press_link_);
    return;
  }
  if (d == Drag::Select) {
    selecting_ = false;
    if (!sel_active_) {
      clear_selection();
      redraw();
      return;
    }
    if (tool_ == Tool::Highlight) apply_markup(MarkupKind::Highlight);
    else if (tool_ == Tool::Underline) apply_markup(MarkupKind::Underline);
    else if (tool_ == Tool::Squiggly) apply_markup(MarkupKind::Squiggly);
    else if (tool_ == Tool::Strike) apply_markup(MarkupKind::StrikeOut);
    else {
      // X11 primary selection, like a terminal.
      std::string s = selection_text();
      if (!s.empty()) Fl::copy(s.c_str(), int(s.size()), 0);
    }
    redraw();
    return;
  }
  if (d == Drag::MoveAnnot) {
    if (moved && annot_sel_page_ >= 0) {
      if (const Annotation *a = selected_annot()) {
        double ax, ay, bx, by;
        const Slot &s = slots_[size_t(annot_sel_page_)];
        double ox = x() + s.x - std::round(sx_), oy = y() + s.y - std::round(sy_);
        RenderParams rp = params();
        device_to_page(doc_->width(pg), doc_->height(pg), rp, press_x_ - ox, press_y_ - oy, ax, ay);
        device_to_page(doc_->width(pg), doc_->height(pg), rp, mx - ox, my - oy, bx, by);
        double ddx = bx - ax, ddy = by - ay;
        PopplerAnnot *ref = POPPLER_ANNOT(g_object_ref(a->annot));
        Box nb{annot_sel_box_.x1 + ddx, annot_sel_box_.y1 + ddy, annot_sel_box_.x2 + ddx, annot_sel_box_.y2 + ddy};
        PopplerAnnotType type = annot_sel_type_;
        if (doc_->move_annot(pg, ref, ddx, ddy)) {
          UndoStep u{UndoStep::Moved, pg, ref};
          u.dx = ddx;
          u.dy = ddy;
          push_undo(u);
          reselect_annot(pg, nb, type);
          edited(pg);
        } else {
          g_object_unref(ref);
        }
      }
    }
    move_dx_ = move_dy_ = 0;
    redraw();
    return;
  }
  if (pg < 0) return;
  if (d == Drag::Ink) {
    if (!stroke_pts_.empty()) finish_added(pg, doc_->add_ink(pg, {stroke_pts_}, ink(), stroke_));
    stroke_pts_.clear();
  } else if (d == Drag::Shape) {
    Box b = Box{drag_p0_.x, drag_p0_.y, drag_p1_.x, drag_p1_.y}.normalized();
    place_at(tool_, pg, drag_p0_, b, moved);
  }
  drag_page_ = -1;
  redraw();
}

// Places what the tool makes: |at| is where the press happened and
// |b| the dragged box (page space).
void DocView::place_at(Tool t, int pg, Point at, const Box &b0, bool moved) {
  Box b = b0;
  Rgb c = ink();
  switch (t) {
    case Tool::Rect:
      if (moved && b.w() > 2 && b.h() > 2) finish_added(pg, doc_->add_square(pg, b, c, stroke_));
      break;
    case Tool::Ellipse:
      if (moved && b.w() > 2 && b.h() > 2) finish_added(pg, doc_->add_circle(pg, b, c, stroke_));
      break;
    case Tool::Line:
      if (moved) finish_added(pg, doc_->add_line(pg, drag_p0_, drag_p1_, c, stroke_));
      break;
    case Tool::Arrow:
      if (moved) finish_added(pg, doc_->add_ink(pg, arrow_strokes(drag_p0_, drag_p1_, 6 + stroke_ * 2), c, stroke_));
      break;
    case Tool::Measure:
      if (moved) {
        double dist = std::hypot(drag_p1_.x - drag_p0_.x, drag_p1_.y - drag_p0_.y);
        if (PopplerAnnot *a = doc_->add_line(pg, drag_p0_, drag_p1_, c, stroke_)) {
          std::string label = measure_label(dist, "mm");
          doc_->set_annot_contents(pg, a, label);
          finish_added(pg, a);
          double lw, lh;
          cairo_surface_t *img = label_image(label, c, 9, lw, lh);
          Point mid{(drag_p0_.x + drag_p1_.x) / 2, (drag_p0_.y + drag_p1_.y) / 2};
          Box lb{mid.x - lw / 2, mid.y - lh - 3, mid.x + lw / 2, mid.y - 3};
          finish_added(pg, doc_->add_image(pg, lb, img, label));
          cairo_surface_destroy(img);
          app_->toast(format(_("Distance: %s"), label.c_str()));
        }
      }
      break;
    case Tool::Text: {
      if (!moved || b.w() < 20 || b.h() < 12) b = Box{at.x, at.y, at.x + 200, at.y + 40};
      std::string text;
      if (app_->ask_text(_("Text Box"), text, true) && !trim(text).empty())
        finish_added(pg, doc_->add_free_text(pg, b, text, c, font_size_));
      set_tool(Tool::Select);
      break;
    }
    case Tool::FillText:
    case Tool::FillDate:
    case Tool::FillInitials: {
      std::string text;
      if (t == Tool::FillDate) {
        text = today();
      } else if (t == Tool::FillInitials) {
        text = app_->initials();
        if (text.empty()) break;
      } else if (!app_->ask_text(_("Text"), text, true) || trim(text).empty()) {
        break;
      }
      double lw, lh;
      cairo_surface_t *img = label_image(text, c, font_size_, lw, lh);
      Box lb{at.x, at.y - lh / 2, at.x + lw, at.y + lh / 2};
      finish_added(pg, doc_->add_image(pg, lb, img, text));
      cairo_surface_destroy(img);
      break;
    }
    case Tool::FillCheck:
    case Tool::FillCross:
    case Tool::FillDot: {
      double sz = std::max(8.0, font_size_ * 1.2);
      Strokes st = t == Tool::FillCheck ? check_strokes(at, sz) : t == Tool::FillCross ? cross_strokes(at, sz) : dot_strokes(at, sz);
      finish_added(pg, doc_->add_ink(pg, st, c, t == Tool::FillDot ? sz * 0.3 : std::max(1.5, sz / 8)));
      break;
    }
    case Tool::Note: {
      std::string text;
      if (app_->ask_text(_("Sticky Note"), text, true)) finish_added(pg, doc_->add_note(pg, at.x - 10, at.y - 10, text, c));
      set_tool(Tool::Select);
      break;
    }
    case Tool::Stamp: {
      double sw = 150, sh = 44;
      if (moved && b.w() > 20 && b.h() > 10) {
        sw = b.w();
        sh = b.h();
      }
      Box sb = moved && b.w() > 20 ? Box{b.x1, b.y1, b.x1 + sw, b.y1 + sh} : Box{at.x - sw / 2, at.y - sh / 2, at.x + sw / 2, at.y + sh / 2};
      cairo_surface_t *img = stamp_image(stamp_text_, c, sb.w(), sb.h());
      PopplerAnnot *a = doc_->add_image(pg, sb, img, stamp_text_);
      cairo_surface_destroy(img);
      if (!a) app_->toast(_("This poppler version cannot store images in PDFs"));
      finish_added(pg, a);
      break;
    }
    case Tool::Signature:
    case Tool::Image: {
      cairo_surface_t *src = t == Tool::Signature ? signature_ : image_;
      if (!src) {
        if (t == Tool::Signature) app_->choose_signature();
        else app_->run(Act::AddImage);
        src = t == Tool::Signature ? signature_ : image_;
      }
      if (!src) break;
      double iw = cairo_image_surface_get_width(src), ih = cairo_image_surface_get_height(src);
      double bw = t == Tool::Signature ? 160 : std::min(300.0, iw * 0.75), bh = bw * ih / std::max(1.0, iw);
      Box sb = moved && b.w() > 10 && b.h() > 10 ? b
                                                  : Box{at.x - bw / 2, at.y - bh / 2, at.x + bw / 2, at.y + bh / 2};
      PopplerAnnot *a = doc_->add_image(pg, sb, src, t == Tool::Signature ? _("Signature") : _("Image"));
      if (!a) app_->toast(_("This poppler version cannot store images in PDFs"));
      finish_added(pg, a);
      if (t == Tool::Signature) set_tool(Tool::Select);
      break;
    }
    case Tool::Field:
      if (!moved || b.w() < 6 || b.h() < 6) {
        double fw = field_kind_ == FieldKind::Check || field_kind_ == FieldKind::Radio ? 14 : 160;
        double fh = field_kind_ == FieldKind::Check || field_kind_ == FieldKind::Radio ? 14
                    : field_kind_ == FieldKind::List                                    ? 60
                    : field_kind_ == FieldKind::Signature                               ? 40
                                                                                        : 22;
        b = Box{at.x, at.y, at.x + fw, at.y + fh};
      }
      app_->canvas_box(t, pg, b);
      break;
    case Tool::Link:
    case Tool::Crop:
      if (moved && b.w() > 4 && b.h() > 4) app_->canvas_box(t, pg, b);
      break;
    case Tool::Redact:
      if (moved && b.w() > 2 && b.h() > 2) {
        redactions_.push_back({pg, b});
        app_->tool_changed();
      } else {
        // A click marks the word under the pointer.
        auto boxes = doc_->selection_boxes(pg, Box{at.x, at.y, at.x, at.y}, POPPLER_SELECTION_WORD);
        for (auto &bx : boxes) redactions_.push_back({pg, Box{bx.x1 - 1, bx.y1 - 1, bx.x2 + 1, bx.y2 + 1}});
        if (!boxes.empty()) app_->tool_changed();
      }
      break;
    default: break;
  }
}

void DocView::open_context_menu(int mx, int my) {
  bool sel = has_selection();
  const Annotation *a = selected_annot();
  bool note = a && (a->type == POPPLER_ANNOT_TEXT || a->type == POPPLER_ANNOT_FREE_TEXT);
  int page;
  double px, py;
  bool on_page = screen_to_page(mx, my, page, px, py, false);
  enum { Copy = 1, High, Under, Squig, Strike, Redact, AddNote, EditNote, Delete, Undo, Redo, Bookmark, Color0 = 100 };
  std::vector<Fl_Menu_Item> items;
  auto add = [&](const char *text, long id, int flags = 0) {
    Fl_Menu_Item it;
    memset(&it, 0, sizeof it);
    it.text = text;
    it.user_data_ = reinterpret_cast<void *>(id);
    it.flags = flags;
    it.labelsize(app_->theme().font_size);
    it.labelcolor(app_->theme().text.fl());
    items.push_back(it);
  };
  if (sel) {
    add(_("Copy"), Copy);
    add(_("Highlight"), High);
    add(_("Underline"), Under);
    add(_("Squiggly"), Squig);
    add(_("Strike Out"), Strike);
    add(_("Mark for Redaction"), Redact, FL_MENU_DIVIDER);
  }
  if (a) {
    if (note) add(_("Edit Note…"), EditNote);
    add(_("Delete Annotation"), Delete, FL_MENU_DIVIDER);
    for (int k = 0; k < kSwatchCount; ++k) add(_(kSwatches[k].name), Color0 + k, k == kSwatchCount - 1 ? FL_MENU_DIVIDER : 0);
  }
  if (on_page) add(_("Add Sticky Note Here…"), AddNote);
  add(_("Bookmark This Page"), Bookmark, FL_MENU_DIVIDER);
  add(_("Undo"), Undo, can_undo() ? 0 : FL_MENU_INACTIVE);
  add(_("Redo"), Redo, can_redo() ? 0 : FL_MENU_INACTIVE);
  Fl_Menu_Item end;
  memset(&end, 0, sizeof end);
  items.push_back(end);
  const Fl_Menu_Item *picked = items.data()->popup(mx, my);
  if (!picked) return;
  long id = reinterpret_cast<long>(picked->user_data());
  switch (id) {
    case Copy: copy_selection(); break;
    case High: apply_markup(MarkupKind::Highlight); break;
    case Under: apply_markup(MarkupKind::Underline); break;
    case Squig: apply_markup(MarkupKind::Squiggly); break;
    case Strike: apply_markup(MarkupKind::StrikeOut); break;
    case Redact:
      for (const auto &sb : sel_boxes_) redactions_.push_back(sb);
      clear_selection();
      app_->toast(_("Marked — use Protect › Apply Redactions to black it out for good"));
      app_->tool_changed();
      break;
    case EditNote: edit_selected_note(); break;
    case Delete: delete_selected_annot(); break;
    case Undo: undo(); break;
    case Redo: redo(); break;
    case Bookmark: app_->toggle_bookmark(); break;
    case AddNote: {
      std::string text;
      if (app_->ask_text(_("Sticky Note"), text, true)) {
        if (PopplerAnnot *n = doc_->add_note(page, px - 10, py - 10, text, tool_ink_[tool_index(Tool::Note)])) {
          push_undo({UndoStep::Added, page, n});
          edited(page);
        }
      }
      break;
    }
    default:
      if (id >= Color0 && id < Color0 + kSwatchCount) {
        Tool keep = tool_;
        tool_ = Tool::Select;
        set_ink(kSwatches[id - Color0].color);
        tool_ = keep;
      }
  }
  redraw();
}

bool DocView::keyboard(int key, int state) {
  if (!doc_) return false;
  bool ctrl = state & FL_CTRL, shift = state & FL_SHIFT;
  if (presentation_) {
    switch (key) {
      case FL_Right: case FL_Down: case FL_Page_Down: case ' ': case FL_Enter: case 'n':
        next_page();
        return true;
      case FL_Left: case FL_Up: case FL_Page_Up: case FL_BackSpace: case 'p':
        prev_page();
        return true;
      case FL_Home: go_to(0); return true;
      case FL_End: go_to(doc_->count() - 1); return true;
    }
    return false;
  }
  if (ctrl && key == 'c') {
    copy_selection();
    return true;
  }
  if (ctrl && key == 'a') {
    select_all();
    return true;
  }
  if (ctrl) return false;
  switch (key) {
    case FL_Up: scroll_by(0, -S(72)); return true;
    case FL_Down: scroll_by(0, S(72)); return true;
    case FL_Left:
      if (canvas_w_ > view_w()) scroll_by(-S(72), 0);
      else prev_page();
      return true;
    case FL_Right:
      if (canvas_w_ > view_w()) scroll_by(S(72), 0);
      else next_page();
      return true;
    case FL_Page_Down: scroll_pages(1); return true;
    case FL_Page_Up: scroll_pages(-1); return true;
    case ' ': scroll_pages(shift ? -1 : 1); return true;
    case FL_BackSpace: scroll_pages(-1); return true;
    case FL_Home: go_to(0); return true;
    case FL_End: go_to(doc_->count() - 1); return true;
    case FL_Delete:
      if (annot_sel_page_ >= 0) {
        delete_selected_annot();
        return true;
      }
      return false;
    case FL_Escape:
      if (drag_ != Drag::Idle) {
        drag_ = Drag::Idle;
        stroke_pts_.clear();
      } else if (has_selection() || annot_sel_page_ >= 0) {
        clear_selection();
        annot_sel_page_ = -1;
        app_->tool_changed();
      } else if (tool_ != Tool::Select) {
        set_tool(Tool::Select);
      } else {
        return false;
      }
      redraw();
      return true;
    case '+': case '=': case FL_KP + '+': zoom_step(1); return true;
    case '-': case FL_KP + '-': zoom_step(-1); return true;
  }
  return false;
}

}  // namespace xanara
