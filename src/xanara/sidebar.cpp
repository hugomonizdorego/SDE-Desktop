// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The sidebar: page thumbnails, the document outline and the list of
// annotations in the document.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

namespace {

std::string annot_label(PopplerAnnotType t) {
  switch (t) {
    case POPPLER_ANNOT_TEXT: return _("Note");
    case POPPLER_ANNOT_FREE_TEXT: return _("Text box");
    case POPPLER_ANNOT_HIGHLIGHT: return _("Highlight");
    case POPPLER_ANNOT_UNDERLINE: return _("Underline");
    case POPPLER_ANNOT_STRIKE_OUT: return _("Strike-out");
    case POPPLER_ANNOT_SQUIGGLY: return _("Squiggly");
    case POPPLER_ANNOT_INK: return _("Drawing");
    case POPPLER_ANNOT_SQUARE: return _("Rectangle");
    case POPPLER_ANNOT_CIRCLE: return _("Ellipse");
    case POPPLER_ANNOT_LINE: return _("Line");
    case POPPLER_ANNOT_STAMP: return _("Stamp");
    default: return _("Annotation");
  }
}

void flatten(std::vector<OutlineItem> &items, int depth, std::vector<std::pair<OutlineItem *, int>> &out) {
  for (auto &it : items) {
    out.push_back({&it, depth});
    if (it.expanded) flatten(it.children, depth + 1, out);
  }
}

}  // namespace

Sidebar::Sidebar(XanaraWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

int Sidebar::tabs_h() const { return S(46); }

int Sidebar::thumb_w() const { return std::max(S(60), std::min(S(150), w() - S(64))); }

void Sidebar::set_tab(Tab t) {
  tab_ = t;
  scroll_ = 0;
  if (t == Notes && notes_dirty_) rebuild_notes();
  if (t == Pages) page_changed(app_->view()->current_page());
  redraw();
}

void Sidebar::document_changed() {
  outline_.clear();
  if (Document *d = app_->document()) outline_ = d->outline();
  rebuild_rows();
  notes_dirty_ = true;
  if (tab_ == Notes) rebuild_notes();
  scroll_ = 0;
  follow_page_ = -1;
  redraw();
}

void Sidebar::rebuild_rows() {
  rows_.clear();
  std::vector<std::pair<OutlineItem *, int>> flat;
  flatten(outline_, 0, flat);
  for (auto &f : flat) {
    Row r;
    r.depth = f.second;
    r.title = f.first->title.empty() ? std::string(_("(untitled)")) : f.first->title;
    r.target = f.first->target;
    r.item = f.first;
    r.page = f.first->target.kind == LinkTarget::Page ? f.first->target.page : -1;
    rows_.push_back(r);
  }
}

void Sidebar::rebuild_notes() {
  notes_.clear();
  notes_dirty_ = false;
  Document *d = app_->document();
  if (!d) return;
  for (int p = 0; p < d->count() && p < 3000; ++p) {
    for (const auto &a : d->annotations(p)) {
      Row r;
      r.page = p;
      r.title = annot_label(a.type);
      std::string c = a.contents;
      for (auto &ch : c)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
      r.detail = c.empty() ? format(_("Page %d"), p + 1) : format(_("Page %d · %s"), p + 1, c.c_str());
      r.target.kind = LinkTarget::Page;
      r.target.page = p;
      r.target.top = std::max(0.0, a.box.y1 - 24);
      notes_.push_back(r);
    }
  }
}

int Sidebar::content_h() const {
  Document *d = app_->document();
  if (!d) return 0;
  if (tab_ == Pages) {
    int tw = thumb_w(), total = S(12);
    for (int p = 0; p < d->count(); ++p) total += app_->view()->thumb_height(p, tw) + S(36);
    return total;
  }
  const auto &list = tab_ == Outline ? rows_ : notes_;
  int rh = tab_ == Outline ? S(32) : S(48);
  return S(8) + int(list.size()) * rh + S(8);
}

void Sidebar::clamp() {
  double max = std::max(0, content_h() - (h() - tabs_h()));
  scroll_ = std::clamp(scroll_, 0.0, max);
}

void Sidebar::page_changed(int page) {
  if (tab_ != Pages || !app_->document()) {
    redraw();
    return;
  }
  // Keep the current page's thumbnail in view.
  int tw = thumb_w(), y0 = S(12);
  for (int p = 0; p < page; ++p) y0 += app_->view()->thumb_height(p, tw) + S(36);
  int th = app_->view()->thumb_height(page, tw) + S(36);
  int view = h() - tabs_h();
  if (y0 < scroll_) scroll_ = y0 - S(8);
  else if (y0 + th > scroll_ + view) scroll_ = y0 + th - view + S(8);
  clamp();
  redraw();
}

void Sidebar::draw() {
  const Theme &t = app_->theme();
  Document *d = app_->document();
  if (tab_ == Notes && notes_dirty_) rebuild_notes();
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), t.surface_alt);
    p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), t.border);
    // Tabs: three icons in a segmented pill.
    int tw = (w() - S(24)) / 3, ty = y() + S(8), th = S(30);
    p.fill_rounded(x() + S(12), ty, tw * 3, th, th / 2.0, t.background);
    icon::Kind glyphs[] = {icon::Pages, icon::Outline, icon::Notes};
    for (int k = 0; k < 3; ++k) {
      int bx = x() + S(12) + k * tw;
      if (k == int(tab_)) p.fill_rounded(bx + S(2), ty + S(2), tw - S(4), th - S(4), (th - S(4)) / 2.0, t.surface);
      else if (k == hover_tab_) p.fill_rounded(bx + S(2), ty + S(2), tw - S(4), th - S(4), (th - S(4)) / 2.0, t.hover);
      icon::draw(p, glyphs[k], bx + (tw - S(18)) / 2.0, ty + S(6), S(18), k == int(tab_) ? t.accent_text : t.text_dim);
    }
    cairo_rectangle(cr, x(), y() + tabs_h(), w(), h() - tabs_h());
    cairo_clip(cr);
    int top = y() + tabs_h() - int(scroll_);
    if (d && tab_ == Pages) {
      int tw2 = thumb_w(), yy = top + S(12), cur = app_->view()->current_page();
      for (int pg = 0; pg < d->count(); ++pg) {
        int th2 = app_->view()->thumb_height(pg, tw2);
        int item_h = th2 + S(36);
        if (yy + item_h >= y() + tabs_h() && yy < y() + h()) {
          int tx = x() + (w() - tw2) / 2;
          if (pg == hover_) p.fill_rounded(tx - S(8), yy - S(6), tw2 + S(16), item_h - S(4), S(8), t.hover);
          p.shadow(tx, yy, tw2, th2, S(2), t.shadow.alpha(0.18), S(4));
          p.fill_rect(tx, yy, tw2, th2, Color::hex(0xFFFFFF));
          if (cairo_surface_t *s = app_->view()->thumbnail(pg, tw2)) {
            double f = double(tw2) / cairo_image_surface_get_width(s);
            cairo_save(cr);
            cairo_translate(cr, tx, yy);
            cairo_scale(cr, f, f);
            cairo_set_source_surface(cr, s, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), f == 1.0 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_restore(cr);
          }
          if (pg == cur) p.stroke_rounded(tx - S(3) + 0.5, yy - S(3) + 0.5, tw2 + S(6) - 1, th2 + S(6) - 1, S(3), t.accent, Sd(2));
        }
        yy += item_h;
        if (yy > y() + h() + S(400)) break;
      }
    } else if (d) {
      const auto &list = tab_ == Outline ? rows_ : notes_;
      int rh = tab_ == Outline ? S(32) : S(48);
      int cur = app_->view()->current_page();
      for (int k = 0; k < int(list.size()); ++k) {
        int ry = top + S(8) + k * rh;
        if (ry + rh < y() + tabs_h() || ry > y() + h()) continue;
        const Row &r = list[size_t(k)];
        if (k == hover_) p.fill_rounded(x() + S(8), ry, w() - S(16), rh - S(2), S(6), t.hover);
        int indent = x() + S(14) + r.depth * S(14);
        if (tab_ == Outline && r.item && !r.item->children.empty())
          icon::draw(p, r.item->expanded ? icon::ChevronDown : icon::Chevron, indent, ry + (rh - S(16)) / 2.0, S(16),
                     t.text_dim);
        if (tab_ == Notes)
          p.fill_rounded(x() + S(14), ry + S(10), S(4), rh - S(22), S(2), r.page == cur ? t.accent : t.border);
      }
    }
    cairo_restore(cr);
  }
  // Text.
  fl_push_clip(x(), y() + tabs_h(), w(), h() - tabs_h());
  int top = y() + tabs_h() - int(scroll_);
  if (!d) {
    text_font(t.font_size - S(1));
    draw_text(_("No document"), x() + S(16), y() + tabs_h() + S(12), w() - S(32), S(24), t.text_dim);
  } else if (tab_ == Pages) {
    int tw2 = thumb_w(), yy = top + S(12);
    text_font(t.font_size - S(1));
    for (int pg = 0; pg < d->count(); ++pg) {
      int th2 = app_->view()->thumb_height(pg, tw2);
      if (yy + th2 + S(36) >= y() + tabs_h() && yy < y() + h())
        draw_text(std::to_string(pg + 1), x(), yy + th2 + S(4), w(), S(22),
                  pg == app_->view()->current_page() ? t.accent_text : t.text_dim, 1);
      yy += th2 + S(36);
      if (yy > y() + h() + S(400)) break;
    }
  } else {
    const auto &list = tab_ == Outline ? rows_ : notes_;
    int rh = tab_ == Outline ? S(32) : S(48);
    if (list.empty()) {
      text_font(t.font_size - S(1));
      draw_text(tab_ == Outline ? _("This document has no outline") : _("No annotations yet"), x() + S(16),
                y() + tabs_h() + S(12), w() - S(32), S(24), t.text_dim);
    }
    for (int k = 0; k < int(list.size()); ++k) {
      int ry = top + S(8) + k * rh;
      if (ry + rh < y() + tabs_h() || ry > y() + h()) continue;
      const Row &r = list[size_t(k)];
      if (tab_ == Outline) {
        int indent = x() + S(14) + r.depth * S(14) + S(18);
        text_font(t.font_size - (r.depth ? S(1) : 0), r.depth == 0);
        int pw = r.page >= 0 ? S(36) : 0;
        draw_text(r.title, indent, ry, x() + w() - indent - S(12) - pw, rh, t.text);
        if (r.page >= 0) {
          text_font(t.font_size - S(2));
          draw_text(std::to_string(r.page + 1), x() + w() - S(12) - pw, ry, pw, rh, t.text_dim, 2);
        }
      } else {
        text_font(t.font_size - S(1), true);
        draw_text(r.title, x() + S(26), ry + S(4), w() - S(40), S(20), t.text);
        text_font(t.font_size - S(2));
        draw_text(r.detail, x() + S(26), ry + S(23), w() - S(40), S(18), t.text_dim);
      }
    }
  }
  fl_pop_clip();
}

int Sidebar::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  Document *d = app_->document();
  auto item_at = [&](int &k) -> bool {
    k = -1;
    if (!d || my < y() + tabs_h()) return false;
    int top = y() + tabs_h() - int(scroll_);
    if (tab_ == Pages) {
      int tw2 = thumb_w(), yy = top + S(12);
      for (int pg = 0; pg < d->count(); ++pg) {
        int ih = app_->view()->thumb_height(pg, tw2) + S(36);
        if (my >= yy - S(6) && my < yy + ih - S(6)) {
          k = pg;
          return true;
        }
        yy += ih;
        if (yy > my + S(10)) break;
      }
      return false;
    }
    const auto &list = tab_ == Outline ? rows_ : notes_;
    int rh = tab_ == Outline ? S(32) : S(48);
    int idx = (my - top - S(8)) / rh;
    if (my - top - S(8) < 0 || idx >= int(list.size())) return false;
    k = idx;
    return true;
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int k = -1, tab = -1;
      if (my < y() + tabs_h()) {
        int tw = (w() - S(24)) / 3;
        if (mx >= x() + S(12) && mx < x() + S(12) + 3 * tw) tab = (mx - x() - S(12)) / tw;
      } else {
        item_at(k);
      }
      if (k != hover_ || tab != hover_tab_) {
        hover_ = k;
        hover_tab_ = tab;
        redraw();
      }
      static const char *tips[] = {N_("Pages"), N_("Outline"), N_("Annotations")};
      tooltip(tab >= 0 ? _(tips[tab]) : nullptr);
      return 1;
    }
    case FL_LEAVE:
      hover_ = hover_tab_ = -1;
      redraw();
      return 1;
    case FL_MOUSEWHEEL:
      scroll_ += Fl::event_dy() * S(60);
      clamp();
      redraw();
      return 1;
    case FL_PUSH: {
      if (my < y() + tabs_h()) {
        int tw = (w() - S(24)) / 3;
        if (tw > 0 && mx >= x() + S(12) && mx < x() + S(12) + 3 * tw) {
          set_tab(Tab((mx - x() - S(12)) / tw));
          app_->save_settings();
        }
        return 1;
      }
      int k;
      if (!item_at(k)) return 1;
      if (tab_ == Pages) {
        app_->view()->go_to(k, -1, false);
      } else {
        const auto &list = tab_ == Outline ? rows_ : notes_;
        const Row &r = list[size_t(k)];
        int indent = x() + S(14) + r.depth * S(14);
        if (tab_ == Outline && r.item && !r.item->children.empty() &&
            (mx < indent + S(18) || r.target.kind == LinkTarget::Nothing)) {
          r.item->expanded = !r.item->expanded;
          rebuild_rows();
          clamp();
        } else {
          app_->view()->follow(r.target);
        }
      }
      redraw();
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}

}  // namespace xanara
