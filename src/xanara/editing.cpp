// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Direct editing on the page canvas (the Edit mode). The text blocks and
// pictures of a page come from the PDF engine's PageLayout (layout.h);
// clicking into text starts a TextEdit (edit.h) that the keyboard and the
// mouse change like a word processor does. While the box is edited, the
// canvas shows it as it will be written: the page without the box's
// original glyphs (a copy of the document) with the box alone drawn over
// it (a one page PDF), both rendered by poppler, so that what the user sees
// is what the file will hold. Leaving the box writes the change into a new
// working copy of the document: one step of undo.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <glib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

namespace {

constexpr double kBlink = 0.53;

// Opens PDF bytes with poppler.
PopplerDocument *open_bytes(const std::string &bytes, const std::string &password) {
  GBytes *gb = g_bytes_new(bytes.data(), bytes.size());
  GError *err = nullptr;
  PopplerDocument *d = poppler_document_new_from_bytes(gb, password.empty() ? nullptr : password.c_str(), &err);
  g_bytes_unref(gb);
  if (err) g_error_free(err);
  return d;
}

void stroke_box(cairo_t *cr, double x, double y, double w, double h, const Color &c, double width,
                bool dashed = false) {
  if (dashed) {
    static const double dash[] = {4, 3};
    cairo_set_dash(cr, dash, 2, 0);
  }
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
  cairo_set_line_width(cr, width);
  cairo_rectangle(cr, std::round(x) + 0.5, std::round(y) + 0.5, std::round(w), std::round(h));
  cairo_stroke(cr);
  if (dashed) cairo_set_dash(cr, nullptr, 0, 0);
}

void handle_square(Painter &p, double cx, double cy, double size, const Theme &t) {
  cairo_t *cr = p.cr();
  p.fill_rect(cx - size / 2, cy - size / 2, size, size, t.surface);
  cairo_set_source_rgb(cr, t.accent.r, t.accent.g, t.accent.b);
  cairo_set_line_width(cr, 1);
  cairo_rectangle(cr, std::round(cx - size / 2) + 0.5, std::round(cy - size / 2) + 0.5, std::round(size) - 1,
                  std::round(size) - 1);
  cairo_stroke(cr);
}

}  // namespace

// ------------------------------------------------------------- layouts

std::shared_ptr<PageLayout> DocView::layout_of(int page, bool now) {
  if (!doc_ || page < 0 || page >= doc_->count()) return nullptr;
  auto it = layouts_.find(page);
  if (it != layouts_.end()) return it->second;
  if (!now) {
    if (layouts_wanted_.insert(page).second && layouts_wanted_.size() == 1) Fl::add_timeout(0.0, layouts_cb, this);
    return nullptr;
  }
  if (!edit_pdf_ && !edit_pdf_failed_) {
    std::string error;
    edit_pdf_ = PdfFile::open(doc_->path(), doc_->password(), error);
    edit_pdf_failed_ = !edit_pdf_;
  }
  if (!edit_pdf_) return nullptr;
  std::shared_ptr<PageLayout> l(PageLayout::read(*edit_pdf_, page).release());
  layouts_[page] = l;
  layouts_wanted_.erase(page);
  return l;
}

void DocView::layouts_cb(void *p) {
  auto *self = static_cast<DocView *>(p);
  if (self->layouts_wanted_.empty()) return;
  int page = *self->layouts_wanted_.begin();
  self->layouts_wanted_.erase(self->layouts_wanted_.begin());
  self->layout_of(page, true);
  self->redraw();
  if (!self->layouts_wanted_.empty()) Fl::repeat_timeout(0.0, layouts_cb, p);
}

void DocView::forget_layouts() {
  layouts_.clear();
  layouts_wanted_.clear();
  Fl::remove_timeout(layouts_cb, this);
  edit_pdf_.reset();
  edit_pdf_failed_ = false;
  hover_block_ = hover_image_ = hover_page_ = -1;
}

// ------------------------------------------------------------ the style

TextStyle DocView::text_style() const { return text_ ? text_->style_at_caret() : new_style_; }

std::string DocView::text_family() const {
  TextStyle st = text_style();
  if (!st.family.empty()) return st.family;
  if (text_ && st.font >= 0 && text_->page_layout() && st.font < int(text_->page_layout()->fonts.size())) {
    const PdfFont &f = *text_->page_layout()->fonts[size_t(st.font)];
    return f.family.empty() ? f.name : f.family;
  }
  FontRequest r;
  r.bold = st.bold;
  r.italic = st.italic;
  std::shared_ptr<SystemFont> f = SystemFont::find(r);
  return f ? f->family : std::string("Sans");
}

void DocView::restyle_text(const std::function<void(TextStyle &)> &change) {
  if (text_) {
    text_->restyle(change);
    edit_changed();
    return;
  }
  change(new_style_);
  app_->tool_changed();
}

double DocView::font_size() const { return text_ || tool_ == Tool::NewText ? text_style().size : font_size_; }

void DocView::set_font_size(double s) {
  if (text_ || tool_ == Tool::NewText) {
    s = std::clamp(s, 1.0, 999.0);
    restyle_text([s](TextStyle &st) { st.size = s; });
    return;
  }
  font_size_ = s;
}

int DocView::align() const { return text_ ? text_->align() : align_; }

void DocView::set_align(int a) {
  if (text_) {
    text_->set_align(a);
    edit_changed();
    return;
  }
  align_ = a;
}

// ------------------------------------------------------------- sessions

void DocView::start_text(int page, int block, double px, double py) {
  std::shared_ptr<PageLayout> L = layout_of(page);
  if (!L) return;
  std::unique_ptr<TextEdit> t(new TextEdit());
  if (!t->start(L, block)) {
    app_->toast(_("This text cannot be edited: it is turned, invisible, or its font gives no widths"));
    return;
  }
  text_ = std::move(t);
  text_->set_caret(text_->hit(px, py), false);
  if (!make_background()) {
    text_.reset();
    drop_overlay();
    app_->toast(_("This text cannot be edited: the page draws it in a way Xanara cannot change"));
    return;
  }
  sel_image_ = sel_image_page_ = -1;
  hover_block_ = -1;
  edit_changed(true);
}

void DocView::start_new_text(int page, const Box &box) {
  std::shared_ptr<PageLayout> L = layout_of(page);
  if (!L) {
    app_->toast(_("This page cannot be edited"));
    return;
  }
  finish_text_edit();
  TextStyle st = new_style_;
  st.font = -1;
  set_tool(Tool::Edit);
  text_.reset(new TextEdit());
  text_->start_new(L, Point{box.x1, box.y1}, box.w() > 20 ? box.w() : 0, st);
  make_background();
  edit_changed(true);
}

bool DocView::make_background() {
  if (bg_doc_) {
    g_object_unref(bg_doc_);
    bg_doc_ = nullptr;
  }
  if (bg_) {
    cairo_surface_destroy(bg_);
    bg_ = nullptr;
  }
  if (!text_ || !doc_) return false;
  if (text_->block() < 0) return true;  // a new box: the page as it is
  std::string error, bytes;
  std::unique_ptr<PdfFile> copy = PdfFile::open(doc_->path(), doc_->password(), error);
  if (!copy) return false;
  std::unique_ptr<PageLayout> l = PageLayout::read(*copy, text_->page());
  if (!l || !same_content(*l, *text_->page_layout())) return false;
  if (!remove_glyphs(*copy, *l, text_->all_glyphs(), error)) return false;
  l.reset();
  if (!copy->write_memory(bytes, PdfFile::Write::Auto, error)) return false;
  bg_doc_ = open_bytes(bytes, doc_->password());
  return bg_doc_ != nullptr;
}

int DocView::overlay_key() const {
  RenderParams rp = params();
  return ((quantize_scale(rp.scale) * 4 + rotation_ / 90) * 2 + (show_annots_ ? 1 : 0)) * 3 + int(mode_);
}

void DocView::edit_region(Box &r) const {
  r = text_->frame();
  if (text_->block() >= 0) r = r.united(text_->page_layout()->blocks[size_t(text_->block())].box);
  double m = 6 + 0.45 * text_->style_at_caret().size;
  r.x1 -= m;
  r.y1 -= m;
  r.x2 += m;
  r.y2 += m;
  int page = text_->page();
  r.x1 = std::max(0.0, r.x1);
  r.y1 = std::max(0.0, r.y1);
  r.x2 = std::min(doc_->width(page), r.x2);
  r.y2 = std::min(doc_->height(page), r.y2);
}

void DocView::render_overlay() {
  preview_pending_ = false;
  Fl::remove_timeout(preview_cb, this);
  if (!text_ || !doc_ || !edit_pdf_) return;
  int page = text_->page();
  if (page < 0 || page >= doc_->count()) return;
  RenderParams rp = params(), rn = rp;
  rn.mode = ColorMode::Normal;
  double pw = doc_->width(page), ph = doc_->height(page);
  int pxw, pxh;
  page_pixels(pw, ph, rp, pxw, pxh);
  Box region;
  edit_region(region);
  double ax, ay, bx, by;
  page_to_device(pw, ph, rp, region.x1, region.y1, ax, ay);
  page_to_device(pw, ph, rp, region.x2, region.y2, bx, by);
  int rx = std::max(0, int(std::floor(std::min(ax, bx)))), ry = std::max(0, int(std::floor(std::min(ay, by))));
  int rx2 = std::min(pxw, int(std::ceil(std::max(ax, bx)))), ry2 = std::min(pxh, int(std::ceil(std::max(ay, by))));
  if (rx2 <= rx || ry2 <= ry) return;
  int rw = rx2 - rx, rh = ry2 - ry, key = overlay_key();
  // The page under the box, for a larger area so that typing seldom needs
  // a new one.
  bool have = bg_ && bg_key_ == key && rx >= bg_x_ && ry >= bg_y_ && rx2 <= bg_x_ + cairo_image_surface_get_width(bg_) &&
              ry2 <= bg_y_ + cairo_image_surface_get_height(bg_);
  if (!have) {
    if (bg_) cairo_surface_destroy(bg_);
    int pad = std::max(S(64), std::max(rw, rh) / 2);
    int gx = std::max(0, rx - pad), gy = std::max(0, ry - pad);
    int gx2 = std::min(pxw, rx2 + pad), gy2 = std::min(pxh, ry2 + pad);
    if (bg_doc_) {
      PopplerPage *pg = poppler_document_get_page(bg_doc_, page);
      bg_ = render_area(pg, pw, ph, rn, gx, gy, gx2 - gx, gy2 - gy);
      if (pg) g_object_unref(pg);
    } else {
      bg_ = doc_->render(page, rn, gx, gy, gx2 - gx, gy2 - gy);
    }
    bg_x_ = gx;
    bg_y_ = gy;
    bg_key_ = key;
  }
  // The box alone, as the engine will write it.
  std::string bytes, error;
  PopplerDocument *pv = nullptr;
  if (!text_->empty() && preview_pdf(*edit_pdf_, *text_->page_layout(), *text_, bytes, error))
    pv = open_bytes(bytes, "");
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, rw, rh);
  cairo_t *cr = cairo_create(s);
  if (bg_) {
    cairo_set_source_surface(cr, bg_, bg_x_ - rx, bg_y_ - ry);
    cairo_paint(cr);
  }
  if (pv) {
    PopplerPage *pg = poppler_document_get_page(pv, 0);
    render_into(cr, pg, pw, ph, rn, rx, ry);
    if (pg) g_object_unref(pg);
    g_object_unref(pv);
  }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  apply_color_mode(s, rp.mode);
  if (overlay_) cairo_surface_destroy(overlay_);
  overlay_ = s;
  ov_x_ = rx;
  ov_y_ = ry;
  ov_key_ = key;
  ov_page_ = page;
  ov_box_ = region;
}

void DocView::drop_overlay() {
  preview_pending_ = false;
  Fl::remove_timeout(preview_cb, this);
  if (overlay_) cairo_surface_destroy(overlay_);
  overlay_ = nullptr;
  ov_page_ = -1;
  if (bg_) cairo_surface_destroy(bg_);
  bg_ = nullptr;
  if (bg_doc_) g_object_unref(bg_doc_);
  bg_doc_ = nullptr;
}

void DocView::preview_cb(void *p) {
  auto *self = static_cast<DocView *>(p);
  self->render_overlay();
  self->redraw();
}

void DocView::blink_cb(void *p) {
  auto *self = static_cast<DocView *>(p);
  if (!self->text_) return;
  self->caret_on_ = !self->caret_on_;
  self->redraw();
  Fl::repeat_timeout(kBlink, blink_cb, p);
}

void DocView::edit_changed(bool now) {
  caret_on_ = true;
  Fl::remove_timeout(blink_cb, this);
  if (text_) Fl::add_timeout(kBlink, blink_cb, this);
  if (now) {
    render_overlay();
  } else if (!preview_pending_) {
    preview_pending_ = true;
    Fl::add_timeout(0.0, preview_cb, this);
  }
  app_->tool_changed();
  redraw();
}

bool DocView::finish_text_edit() {
  bool ok = true;
  if (text_) {
    if (preview_pending_) render_overlay();
    Fl::remove_timeout(blink_cb, this);
    edit_drag_ = EditDrag::Off;
    bool write = text_->modified() && !(text_->block() < 0 && trim(text_->text()).empty());
    int page = text_->page();
    if (!write) {
      text_.reset();
      drop_overlay();
    } else {
      // The box as it will be written stays on screen until the page's new
      // tiles arrive.
      if (ghost_) cairo_surface_destroy(ghost_);
      ghost_ = overlay_;
      overlay_ = nullptr;
      ghost_x_ = ov_x_;
      ghost_y_ = ov_y_;
      ghost_key_ = ov_key_;
      ghost_page_ = ghost_ ? page : -1;
      drop_overlay();
      std::unique_ptr<TextEdit> t = std::move(text_);
      std::shared_ptr<PageLayout> base = layout_of(page);
      ok = app_->commit_page_edit(
          this, page,
          [&](PdfFile &pdf, PageLayout &l, std::string &e) {
            // The same page read from the copy that is written.
            t->rebase(std::shared_ptr<PageLayout>(&l, [](PageLayout *) {}));
            return commit_text(pdf, l, *t, e);
          },
          t->page_layout(), _("Text changed — save to keep it"));
      if (!ok) {
        if (ghost_) cairo_surface_destroy(ghost_);
        ghost_ = nullptr;
        ghost_page_ = -1;
        // Keep editing, so that nothing typed is lost.
        if (base && doc_) {
          t->rebase(base);
          text_ = std::move(t);
          if (make_background()) {
            edit_changed(true);
          } else {
            text_.reset();
            drop_overlay();
          }
        }
      }
    }
  }
  if (sel_image_ >= 0) {
    sel_image_ = sel_image_page_ = -1;
    if (drag_image_) cairo_surface_destroy(drag_image_);
    drag_image_ = nullptr;
  }
  app_->tool_changed();
  redraw();
  return ok;
}

// --------------------------------------------------------------- pictures

bool DocView::commit_image(int page, const std::function<bool(PdfFile &, PageLayout &, std::string &)> &op,
                           const std::string &message) {
  std::shared_ptr<PageLayout> L = layout_of(page);
  if (!L) return false;
  return app_->commit_page_edit(this, page, op, L.get(), message);
}

Box DocView::selected_image_box() const {
  auto it = layouts_.find(sel_image_page_);
  if (sel_image_ < 0 || it == layouts_.end() || sel_image_ >= int(it->second->images.size())) return Box{};
  return it->second->images[size_t(sel_image_)].box;
}

void DocView::selected_image_pixels(int &w, int &h) const {
  w = h = 0;
  auto it = layouts_.find(sel_image_page_);
  if (sel_image_ < 0 || it == layouts_.end() || sel_image_ >= int(it->second->images.size())) return;
  w = it->second->images[size_t(sel_image_)].pixels_w;
  h = it->second->images[size_t(sel_image_)].pixels_h;
}

void DocView::replace_selected_image() {
  if (sel_image_ < 0) return;
  int idx = sel_image_, page = sel_image_page_;
  std::string file = app_->choose_image_file();
  if (file.empty()) return;
  if (commit_image(page,
                   [&](PdfFile &pdf, PageLayout &l, std::string &e) {
                     int w = 0, h = 0;
                     QPDFObjectHandle xo = image_xobject(pdf, file, w, h, e);
                     if (!xo.isInitialized() || !xo.isStream()) return false;
                     return replace_image(pdf, l, idx, xo, e);
                   },
                   _("Picture replaced — save to keep it"))) {
    sel_image_ = idx;
    sel_image_page_ = page;
    app_->tool_changed();
  }
}

void DocView::delete_selected_image() {
  if (sel_image_ < 0) return;
  int idx = sel_image_, page = sel_image_page_;
  sel_image_ = sel_image_page_ = -1;
  commit_image(page, [&](PdfFile &pdf, PageLayout &l, std::string &e) { return delete_image(pdf, l, idx, e); },
               _("Picture deleted — save to keep the change"));
  app_->tool_changed();
}

void DocView::add_picture(int page, const Box &box) {
  if (image_file_.empty()) {
    app_->toast(_("Choose an image first"));
    return;
  }
  std::string file = image_file_;
  bool ok = app_->commit_page_edit(
      this, page,
      [&](PdfFile &pdf, PageLayout &, std::string &e) {
        int w = 0, h = 0;
        QPDFObjectHandle xo = image_xobject(pdf, file, w, h, e);
        if (!xo.isInitialized() || !xo.isStream()) return false;
        return add_image(pdf, page, xo, box, e);
      },
      nullptr, _("Picture added — save to keep it"));
  if (!ok) return;
  set_tool(Tool::Edit);
  // The new picture is drawn last: select it.
  if (std::shared_ptr<PageLayout> L = layout_of(page)) {
    for (int k = int(L->images.size()) - 1; k >= 0; --k)
      if (L->images[size_t(k)].editable) {
        sel_image_ = k;
        sel_image_page_ = page;
        break;
      }
  }
  app_->tool_changed();
  redraw();
}

// ----------------------------------------------------------------- mouse

int DocView::edit_handle_at(int mx, int my) const {
  double bx, by, bw, bh;
  if (text_) {
    int page = text_->page();
    Box f = text_->frame();
    f = Box{f.x1 - 2, f.y1 - 2, f.x2 + 2, f.y2 + 2};
    double hx, hy, hw, hh;
    page_box_device(page, Box{f.x2, (f.y1 + f.y2) / 2, f.x2, (f.y1 + f.y2) / 2}, hx, hy, hw, hh);
    if (std::fabs(mx - hx) <= S(7) && std::fabs(my - hy) <= S(7)) return 2;
    page_box_device(page, f, bx, by, bw, bh);
    int band = S(6);
    bool outer = mx >= bx - band && mx <= bx + bw + band && my >= by - band && my <= by + bh + band;
    bool inner = mx > bx + 1 && mx < bx + bw - 1 && my > by + 1 && my < by + bh - 1;
    return outer && !inner ? 1 : 0;
  }
  if (sel_image_ >= 0) {
    Box b = selected_image_box();
    if (b.empty()) return 0;
    page_box_device(sel_image_page_, b, bx, by, bw, bh);
    const double cx[] = {bx, bx + bw, bx + bw, bx}, cy[] = {by, by, by + bh, by + bh};
    for (int k = 0; k < 4; ++k)
      if (std::fabs(mx - cx[k]) <= S(7) && std::fabs(my - cy[k]) <= S(7)) return 10 + k;
  }
  return 0;
}

void DocView::edit_hover(int mx, int my, Fl_Cursor &c) {
  c = FL_CURSOR_DEFAULT;
  int h = edit_handle_at(mx, my);
  if (h == 1) c = FL_CURSOR_MOVE;
  else if (h == 2) c = FL_CURSOR_WE;
  else if (h >= 10) c = (h - 10) % 2 == 0 ? FL_CURSOR_NWSE : FL_CURSOR_NESW;
  int hb = -1, hi = -1, hp = -1;
  int page;
  double px, py;
  if (h == 0 && screen_to_page(mx, my, page, px, py, false)) {
    if (tool_ == Tool::NewText) {
      c = FL_CURSOR_INSERT;
    } else if (text_ && page == text_->page() && text_->frame().contains(px, py, 3)) {
      c = FL_CURSOR_INSERT;
    } else if (std::shared_ptr<PageLayout> L = layout_of(page)) {
      int b = L->block_at(px, py);
      int k = L->image_at(px, py);
      if (b >= 0 && L->blocks[size_t(b)].editable) {
        hb = b;
        hp = page;
        c = FL_CURSOR_INSERT;
      } else if (k >= 0 && L->images[size_t(k)].editable) {
        hi = k;
        hp = page;
        c = FL_CURSOR_MOVE;
      }
    }
  }
  if (hb != hover_block_ || hi != hover_image_ || hp != hover_page_) {
    hover_block_ = hb;
    hover_image_ = hi;
    hover_page_ = hp;
    redraw();
  }
}

bool DocView::edit_press(int mx, int my) {
  int page;
  double px, py;
  bool on_page = screen_to_page(mx, my, page, px, py, false);
  int clicks = Fl::event_clicks();
  bool shift = Fl::event_state() & FL_SHIFT;
  if (text_) {
    int h = edit_handle_at(mx, my);
    int tp = text_->page();
    if (h == 1 || h == 2) {
      edit_drag_ = h == 1 ? EditDrag::Move : EditDrag::Width;
      double qx, qy;
      device_to_page(doc_->width(tp), doc_->height(tp), params(), mx - (x() + slots_[size_t(tp)].x - std::round(sx_)),
                     my - (y() + slots_[size_t(tp)].y - std::round(sy_)), qx, qy);
      edit_p0_ = Point{qx, qy};
      drag_dx_ = drag_dy_ = 0;
      return true;
    }
    if (on_page && page == tp && text_->frame().contains(px, py, 3)) {
      size_t at = text_->hit(px, py);
      if (clicks >= 2) text_->select_paragraph(at);
      else if (clicks == 1) text_->select_word(at);
      else text_->set_caret(at, shift);
      edit_drag_ = clicks ? EditDrag::Off : EditDrag::Select;
      caret_on_ = true;
      Fl::remove_timeout(blink_cb, this);
      Fl::add_timeout(kBlink, blink_cb, this);
      app_->tool_changed();
      redraw();
      return true;
    }
    // A click outside the box: it is done.
    finish_text_edit();
    if (text_) return true;  // it could not be written: still editing
    on_page = screen_to_page(mx, my, page, px, py, false);
  }
  if (!on_page) {
    if (sel_image_ >= 0) {
      sel_image_ = sel_image_page_ = -1;
      app_->tool_changed();
      redraw();
    }
    return false;
  }
  if (tool_ == Tool::NewText) return false;  // a box is dragged (place_at)
  std::shared_ptr<PageLayout> L = layout_of(page);
  if (!L) {
    app_->toast(_("This page cannot be edited"));
    return true;
  }
  if (sel_image_ >= 0 && sel_image_page_ == page) {
    int h = edit_handle_at(mx, my);
    if (h >= 10) {
      edit_drag_ = EditDrag::ImageCorner;
      drag_corner_ = h - 10;
      drag_box_ = selected_image_box();
      edit_p0_ = Point{px, py};
      return true;
    }
  }
  int b = L->block_at(px, py), k = L->image_at(px, py);
  // Text over a picture wins: a click into it edits the text.
  if (b >= 0 && L->blocks[size_t(b)].editable) {
    start_text(page, b, px, py);
    if (text_) {
      size_t at = text_->hit(px, py);
      if (clicks >= 2) text_->select_paragraph(at);
      else if (clicks == 1) text_->select_word(at);
      edit_drag_ = clicks ? EditDrag::Off : EditDrag::Select;
    }
    redraw();
    return true;
  }
  if (k >= 0 && L->images[size_t(k)].editable) {
    sel_image_ = k;
    sel_image_page_ = page;
    edit_drag_ = EditDrag::Image;
    drag_box_ = L->images[size_t(k)].box;
    edit_p0_ = Point{px, py};
    app_->tool_changed();
    redraw();
    return true;
  }
  if (sel_image_ >= 0) {
    sel_image_ = sel_image_page_ = -1;
    app_->tool_changed();
  }
  if (b >= 0) app_->toast(_("This text cannot be edited: it is turned, invisible, or drawn inside a form"));
  else if (k >= 0) app_->toast(_("This picture is part of a drawing that cannot be moved"));
  redraw();
  return true;
}

void DocView::edit_drag(int mx, int my) {
  int ref = text_ ? text_->page() : sel_image_page_;
  if (ref < 0 || ref >= int(slots_.size())) return;
  if (my > y() + h() - S(16)) scroll_by(0, S(14), false);
  else if (my < y() + S(16)) scroll_by(0, -S(14), false);
  const Slot &s = slots_[size_t(ref)];
  double cx = mx - x() + std::round(sx_), cy = my - y() + std::round(sy_);
  double dx = std::clamp(cx - s.x, 0.0, double(s.w)), dy = std::clamp(cy - s.y, 0.0, double(s.h));
  double px, py;
  device_to_page(doc_->width(ref), doc_->height(ref), params(), dx, dy, px, py);
  switch (edit_drag_) {
    case EditDrag::Select:
      if (text_) text_->set_caret(text_->hit(px, py), true);
      break;
    case EditDrag::Move:
      drag_dx_ = px - edit_p0_.x;
      drag_dy_ = py - edit_p0_.y;
      break;
    case EditDrag::Width:
      drag_dx_ = px - edit_p0_.x;
      break;
    case EditDrag::Image: {
      Box b = selected_image_box();
      double ddx = px - edit_p0_.x, ddy = py - edit_p0_.y;
      drag_box_ = Box{b.x1 + ddx, b.y1 + ddy, b.x2 + ddx, b.y2 + ddy};
      break;
    }
    case EditDrag::ImageCorner: {
      Box b = selected_image_box();
      // The opposite corner stays; the picture keeps its proportions
      // unless Shift is held.
      double ax = drag_corner_ == 0 || drag_corner_ == 3 ? b.x2 : b.x1;
      double ay = drag_corner_ <= 1 ? b.y2 : b.y1;
      double w = std::max(4.0, std::fabs(px - ax)), hgt = std::max(4.0, std::fabs(py - ay));
      if (!(Fl::event_state() & FL_SHIFT) && b.w() > 0 && b.h() > 0) {
        double k = std::max(w / b.w(), hgt / b.h());
        w = b.w() * k;
        hgt = b.h() * k;
      }
      double x1 = drag_corner_ == 0 || drag_corner_ == 3 ? ax - w : ax;
      double y1 = drag_corner_ <= 1 ? ay - hgt : ay;
      drag_box_ = Box{x1, y1, x1 + w, y1 + hgt};
      break;
    }
    case EditDrag::Off: break;
  }
  redraw();
}

void DocView::edit_release(int mx, int my) {
  EditDrag d = edit_drag_;
  edit_drag_ = EditDrag::Off;
  bool moved = std::abs(mx - press_x_) + std::abs(my - press_y_) > 3;
  switch (d) {
    case EditDrag::Select:
      if (text_ && text_->has_selection()) {
        // The X11 primary selection, like a text field.
        std::string s = text_->selected_text();
        if (!s.empty()) Fl::copy(s.c_str(), int(s.size()), 0);
      }
      break;
    case EditDrag::Move:
      if (text_ && moved && (std::fabs(drag_dx_) > 0.1 || std::fabs(drag_dy_) > 0.1)) {
        text_->move_by(drag_dx_, drag_dy_);
        edit_changed(true);
      }
      break;
    case EditDrag::Width:
      if (text_ && moved) {
        Box f = text_->frame();
        text_->set_width(std::max(12.0, f.w() + drag_dx_));
        edit_changed(true);
      }
      break;
    case EditDrag::Image:
    case EditDrag::ImageCorner:
      if (sel_image_ >= 0 && moved && !drag_box_.empty()) {
        int idx = sel_image_, page = sel_image_page_;
        Box b = drag_box_;
        if (commit_image(page, [&](PdfFile &pdf, PageLayout &l, std::string &e) { return move_image(pdf, l, idx, b, e); },
                         d == EditDrag::Image ? _("Picture moved — save to keep it")
                                              : _("Picture resized — save to keep it"))) {
          sel_image_ = idx;
          sel_image_page_ = page;
        }
      }
      break;
    case EditDrag::Off: break;
  }
  drag_dx_ = drag_dy_ = 0;
  app_->tool_changed();
  redraw();
}

// --------------------------------------------------------------- keyboard

void DocView::paste_text() {
  paste_pending_ = true;
  Fl::paste(*this, 1, Fl::clipboard_plain_text);
}

bool DocView::edit_key(int key, int state) {
  if (!text_) {
    if (sel_image_ >= 0) {
      if (key == FL_Delete || key == FL_BackSpace) {
        delete_selected_image();
        return true;
      }
      if (key == FL_Escape) {
        sel_image_ = sel_image_page_ = -1;
        app_->tool_changed();
        redraw();
        return true;
      }
    }
    return false;
  }
  bool ctrl = state & FL_CTRL, shift = state & FL_SHIFT, alt = state & FL_ALT;
  auto moved = [&]() {
    caret_on_ = true;
    Fl::remove_timeout(blink_cb, this);
    Fl::add_timeout(kBlink, blink_cb, this);
    app_->tool_changed();
    redraw();
  };
  auto copy = [&]() {
    std::string s = text_->selected_text();
    if (s.empty()) return false;
    Fl::copy(s.c_str(), int(s.size()), 1, Fl::clipboard_plain_text);
    Fl::copy(s.c_str(), int(s.size()), 0);
    return true;
  };
  if (ctrl && !alt) {
    switch (key) {
      case 'a': text_->select_all(); moved(); return true;
      case 'c': copy(); return true;
      case 'x':
        if (copy()) {
          text_->erase(false, false);
          edit_changed();
        }
        return true;
      case 'v': paste_text(); return true;
      case 'z':
        if (shift ? text_->redo() : text_->undo()) edit_changed();
        return true;
      case 'y':
        if (text_->redo()) edit_changed();
        return true;
      case 'b': {
        bool on = !text_->style_at_caret().bold;
        restyle_text([on](TextStyle &s) { s.bold = on; });
        return true;
      }
      case 'i': {
        bool on = !text_->style_at_caret().italic;
        restyle_text([on](TextStyle &s) { s.italic = on; });
        return true;
      }
      case 'l': set_align(0); return true;
      case 'e': set_align(1); return true;
      case 'r': set_align(2); return true;
      case 'j': set_align(3); return true;
      default: break;
    }
  }
  switch (key) {
    case FL_Escape: finish_text_edit(); return true;
    case FL_Left: text_->move_left(shift, ctrl); moved(); return true;
    case FL_Right: text_->move_right(shift, ctrl); moved(); return true;
    case FL_Up: text_->move_up(shift); moved(); return true;
    case FL_Down: text_->move_down(shift); moved(); return true;
    case FL_Home: text_->move_home(shift, ctrl); moved(); return true;
    case FL_End: text_->move_end(shift, ctrl); moved(); return true;
    case FL_BackSpace: text_->erase(false, ctrl); edit_changed(); return true;
    case FL_Delete: text_->erase(true, ctrl); edit_changed(); return true;
    case FL_Enter:
    case FL_KP_Enter:
      if (ctrl) return false;
      text_->new_paragraph();
      edit_changed();
      return true;
    case FL_Tab:
      if (ctrl) return false;
      text_->insert("\t");
      edit_changed();
      return true;
    case FL_Page_Up:
    case FL_Page_Down: return false;
    default: break;
  }
  if (ctrl || alt || (state & FL_META)) return false;
  int del = 0;
  if (Fl::compose(del)) {
    for (; del > 0; --del) text_->erase(false, false);
    if (Fl::event_length() > 0) text_->insert(std::string(Fl::event_text(), size_t(Fl::event_length())));
    edit_changed();
    return true;
  }
  return false;
}

bool DocView::edit_context_menu(int mx, int my) {
  if (!text_ && sel_image_ < 0) return false;
  enum { Cut = 1, Copy, Paste, All, Bold, Italic, Done, Replace, Delete };
  std::vector<Fl_Menu_Item> items;
  auto add = [&](const char *text, long id, int flags = 0) {
    Fl_Menu_Item it;
    memset(&it, 0, sizeof it);
    it.text = text;
    it.user_data_ = reinterpret_cast<void *>(id);
    it.flags = flags;
    it.labelsize(app_->theme().font_size);
    it.labelcolor((flags & FL_MENU_INACTIVE) ? app_->theme().text_dim.fl() : app_->theme().text.fl());
    items.push_back(it);
  };
  if (text_) {
    int sel = text_->has_selection() ? 0 : FL_MENU_INACTIVE;
    add(_("Cut"), Cut, sel);
    add(_("Copy"), Copy, sel);
    add(_("Paste"), Paste);
    add(_("Select All"), All, FL_MENU_DIVIDER);
    add(_("Bold"), Bold, FL_MENU_TOGGLE | (text_->style_at_caret().bold ? FL_MENU_VALUE : 0));
    add(_("Italic"), Italic, FL_MENU_TOGGLE | (text_->style_at_caret().italic ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
    add(_("Done"), Done);
  } else {
    add(_("Replace Picture…"), Replace);
    add(_("Delete Picture"), Delete);
  }
  Fl_Menu_Item end;
  memset(&end, 0, sizeof end);
  items.push_back(end);
  const Fl_Menu_Item *picked = items.data()->popup(mx, my);
  if (!picked) return true;
  switch (reinterpret_cast<long>(picked->user_data())) {
    case Cut: edit_key('x', FL_CTRL); break;
    case Copy: edit_key('c', FL_CTRL); break;
    case Paste: paste_text(); break;
    case All: edit_key('a', FL_CTRL); break;
    case Bold: edit_key('b', FL_CTRL); break;
    case Italic: edit_key('i', FL_CTRL); break;
    case Done: finish_text_edit(); break;
    case Replace: replace_selected_image(); break;
    case Delete: delete_selected_image(); break;
  }
  return true;
}

// ---------------------------------------------------------------- drawing

void DocView::draw_edit_overlay(Painter &p, int page) {
  if (!text_ || text_->page() != page || !overlay_ || ov_page_ != page) return;
  cairo_t *cr = p.cr();
  const Slot &s = slots_[size_t(page)];
  int ox = x() + s.x - int(std::round(sx_)), oy = y() + s.y - int(std::round(sy_));
  if (ov_key_ != overlay_key() && settled_) render_overlay();
  if (!overlay_) return;
  if (ov_key_ == overlay_key()) {
    cairo_set_source_surface(cr, overlay_, ox + ov_x_, oy + ov_y_);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_rectangle(cr, ox + ov_x_, oy + ov_y_, cairo_image_surface_get_width(overlay_),
                    cairo_image_surface_get_height(overlay_));
    cairo_fill(cr);
    return;
  }
  // The zoom is changing: the last picture, scaled, until it settles.
  double bx, by, bw, bh;
  page_box_device(page, ov_box_, bx, by, bw, bh);
  int iw = cairo_image_surface_get_width(overlay_), ih = cairo_image_surface_get_height(overlay_);
  if (iw <= 0 || ih <= 0 || ((ov_key_ / 6) % 4) != rotation_ / 90) return;
  cairo_save(cr);
  cairo_rectangle(cr, bx, by, bw, bh);
  cairo_clip(cr);
  cairo_translate(cr, bx, by);
  cairo_scale(cr, bw / iw, bh / ih);
  cairo_set_source_surface(cr, overlay_, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_restore(cr);
}

bool DocView::draw_ghost(Painter &p, int page) {
  if (!ghost_ || ghost_page_ != page || ghost_key_ != overlay_key()) return false;
  cairo_t *cr = p.cr();
  const Slot &s = slots_[size_t(page)];
  int ox = x() + s.x - int(std::round(sx_)), oy = y() + s.y - int(std::round(sy_));
  cairo_set_source_surface(cr, ghost_, ox + ghost_x_, oy + ghost_y_);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_rectangle(cr, ox + ghost_x_, oy + ghost_y_, cairo_image_surface_get_width(ghost_),
                  cairo_image_surface_get_height(ghost_));
  cairo_fill(cr);
  return true;
}

void DocView::draw_edit(Painter &p, int page) {
  if (!edit_tool() || presentation_) return;
  const Theme &t = app_->theme();
  cairo_t *cr = p.cr();
  double bx, by, bw, bh;
  std::shared_ptr<PageLayout> L = layout_of(page, false);
  bool session = text_ && text_->page() == page;
  if (L && tool_ == Tool::Edit) {
    // Every text block and picture that can be edited, faintly; the one
    // under the pointer clearly.
    for (int b = 0; b < int(L->blocks.size()); ++b) {
      const PageLayout::Block &B = L->blocks[size_t(b)];
      if (!B.editable || (session && text_->block() == b)) continue;
      page_box_device(page, B.box, bx, by, bw, bh);
      bool hov = page == hover_page_ && b == hover_block_;
      stroke_box(cr, bx - S(3), by - S(2), bw + S(6), bh + S(4), hov ? t.accent : t.accent.alpha(0.28),
                 hov ? Sd(1.5) : 1.0);
    }
    for (int k = 0; k < int(L->images.size()); ++k) {
      const ImageItem &im = L->images[size_t(k)];
      if (!im.editable) continue;
      bool sel = page == sel_image_page_ && k == sel_image_;
      bool hov = page == hover_page_ && k == hover_image_;
      page_box_device(page, im.box, bx, by, bw, bh);
      bool dragging = sel && (edit_drag_ == EditDrag::Image || edit_drag_ == EditDrag::ImageCorner);
      if (dragging) {
        // The picture leaves its place: dim it there, show it where it goes.
        p.fill_rect(bx, by, bw, bh, Color::hex(mode_ == ColorMode::Night ? 0x1D2026 : 0xFFFFFF, 0.55));
        double nx, ny, nw, nh;
        page_box_device(page, drag_box_, nx, ny, nw, nh);
        p.fill_rect(nx, ny, nw, nh, t.accent.alpha(0.14));
        stroke_box(cr, nx, ny, nw, nh, t.accent, Sd(1.5));
        continue;
      }
      if (sel) {
        stroke_box(cr, bx, by, bw, bh, t.accent, Sd(1.5));
        double hs = S(8);
        for (double hx : {bx, bx + bw})
          for (double hy : {by, by + bh}) handle_square(p, hx, hy, hs, t);
      } else {
        stroke_box(cr, bx, by, bw, bh, hov ? t.accent : t.accent.alpha(0.28), hov ? Sd(1.5) : 1.0, !hov);
      }
    }
  }
  if (!session) return;
  // The box being edited: its frame, the selection and the caret.
  Box f = text_->frame();
  f = Box{f.x1 - 2, f.y1 - 2, f.x2 + 2, f.y2 + 2};
  if (edit_drag_ == EditDrag::Move) f = Box{f.x1 + drag_dx_, f.y1 + drag_dy_, f.x2 + drag_dx_, f.y2 + drag_dy_};
  if (edit_drag_ == EditDrag::Width) f.x2 = std::max(f.x1 + 12, f.x2 + drag_dx_);
  page_box_device(page, f, bx, by, bw, bh);
  stroke_box(cr, bx, by, bw, bh, t.accent, Sd(1.2), edit_drag_ == EditDrag::Move || edit_drag_ == EditDrag::Width);
  double hx, hy, hw, hh;
  page_box_device(page, Box{f.x2, (f.y1 + f.y2) / 2, f.x2, (f.y1 + f.y2) / 2}, hx, hy, hw, hh);
  handle_square(p, hx, hy, S(8), t);
  if (edit_drag_ == EditDrag::Move || edit_drag_ == EditDrag::Width) return;
  for (const Box &b : text_->selection_boxes()) {
    page_box_device(page, b, bx, by, bw, bh);
    p.fill_rect(bx, by, bw, bh, t.accent.alpha(0.32));
  }
  if (caret_on_ && !text_->has_selection() && Fl::focus() == this) {
    TextEdit::Caret c = text_->caret_rect();
    double ax, ay, aw, ah, cx2, cy2, cw, ch;
    page_box_device(page, Box{c.x, c.top, c.x, c.top}, ax, ay, aw, ah);
    page_box_device(page, Box{c.x, c.bottom, c.x, c.bottom}, cx2, cy2, cw, ch);
    Color ink = mode_ == ColorMode::Night ? Color::hex(0xF2F4F7) : Color::hex(0x101418);
    cairo_set_source_rgba(cr, ink.r, ink.g, ink.b, 1);
    cairo_set_line_width(cr, std::max(1.0, Sd(1.6)));
    cairo_move_to(cr, std::round(ax) + 0.5, ay);
    cairo_line_to(cr, std::round(cx2) + 0.5, cy2);
    cairo_stroke(cr);
  }
}

}  // namespace xanara
