// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The left icon rail and its side panel: search results, a grid of page
// thumbnails (drag to reorder pages), the outline, bookmarks, the list of
// annotations and saved signatures. The bottom of the rail has page up,
// the current page box, the page count and page down.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "icons.h"
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

const icon::Kind kRailGlyphs[] = {icon::Find, icon::Document, icon::Outline, icon::Bookmark, icon::Notes,
                                  icon::Signature};

std::map<std::string, cairo_surface_t *> &signature_images() {
  static std::map<std::string, cairo_surface_t *> m;
  return m;
}

}  // namespace

const char *Sidebar::panel_title(Panel p) {
  switch (p) {
    case Search: return _("Search");
    case Thumbnails: return _("Thumbnail");
    case Outline: return _("Outline");
    case Bookmarks: return _("Bookmarks");
    case Annotations: return _("Annotations");
    case Signatures: return _("Signatures");
    default: return "";
  }
}

const char *Sidebar::panel_id(Panel p) {
  static const char *ids[] = {"search", "thumbnails", "outline", "bookmarks", "annotations", "signatures"};
  return ids[std::clamp(int(p), 0, 5)];
}

Sidebar::Sidebar(XanaraWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

int Sidebar::rail_w() const { return S(54); }

int Sidebar::rail_step() const {
  int bx, by, bw, bh;
  page_box(bx, by, bw, bh);
  int space = by - (h() < S(520) ? S(10) : S(48)) - y() - S(8);
  return std::max(S(30), std::min(S(50), space / PanelCount));
}
int Sidebar::header_h() const { return S(52); }
int Sidebar::content_top() const {
  return y() + header_h() + (panel_ == Search ? S(64) : 0);
}

void Sidebar::page_box(int &bx, int &by, int &bw, int &bh) const {
  bw = S(40);
  bh = S(28);
  bx = x() + (rail_w() - bw) / 2;
  // Short windows drop the page up/down buttons to keep the rail usable.
  by = y() + h() - (h() < S(520) ? S(62) : S(98));
}

void Sidebar::search_box(int &bx, int &by, int &bw, int &bh) const {
  bx = panel_x() + S(16);
  by = y() + header_h() + S(2);
  bw = panel_w() - S(32) - S(34);
  bh = S(30);
}

void Sidebar::set_panel(Panel p) {
  panel_ = p;
  scroll_ = 0;
  hover_ = -1;
  if (p == Annotations && notes_dirty_) rebuild_notes();
  if (p == Bookmarks) rebuild_bookmarks();
  if (p == Signatures) rebuild_signatures();
  if (p == Thumbnails && app_->view()) page_changed(app_->view()->current_page());
  redraw();
}

void Sidebar::document_changed() {
  outline_.clear();
  if (Document *d = app_->document()) outline_ = d->outline();
  rebuild_rows();
  notes_dirty_ = true;
  if (panel_ == Annotations) rebuild_notes();
  rebuild_bookmarks();
  search_key_.clear();
  search_count_ = -1;
  scroll_ = 0;
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
      r.type = a.type;
      r.box = a.box;
      r.title = annot_label(a.type);
      std::string c = a.contents;
      for (auto &ch : c)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
      r.detail = c.empty() ? format(_("Page %d"), p + 1) : format(_("Page %d · %s"), p + 1, c.c_str());
      if (!a.author.empty()) r.detail += " · " + a.author;
      notes_.push_back(r);
    }
  }
}

void Sidebar::rebuild_search() {
  DocView *v = app_->view();
  Document *d = app_->document();
  search_.clear();
  if (!v || !d) return;
  search_key_ = v->find_query();
  search_count_ = v->find_count();
  for (int k = 0; k < v->find_count() && k < 2000; ++k) {
    Row r;
    r.page = v->hit_page(k);
    r.hit = k;
    Box b = v->hit_box(k);
    // The line around the match as context.
    std::string line = d->selected_text(r.page, Box{0, b.y1 + 1, d->width(r.page), b.y2 - 1}, POPPLER_SELECTION_LINE);
    for (auto &ch : line)
      if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    r.title = format(_("Page %d"), r.page + 1);
    r.detail = trim(line);
    search_.push_back(r);
  }
}

void Sidebar::rebuild_bookmarks() {
  bookmarks_.clear();
  DocTab *t = app_->tab();
  if (!t) return;
  for (int p : app_->bookmarks(t->target)) {
    if (p < 0 || p >= t->doc->count()) continue;
    Row r;
    r.page = p;
    r.title = format(_("Page %s"), t->doc->page_label(p).c_str());
    for (const auto &o : rows_)
      if (o.page >= 0 && o.page <= p) r.detail = o.title;
    if (r.detail.empty()) r.detail = display_name(t->target);
    bookmarks_.push_back(r);
  }
}

void Sidebar::rebuild_signatures() {
  signatures_.clear();
  std::string dir = app_->signatures_dir();
  auto files = list_dir(dir);
  std::sort(files.begin(), files.end());
  for (const auto &f : files) {
    if (!ends_with(lower(f), ".png")) continue;
    Row r;
    r.path = dir + "/" + f;
    r.title = f;
    signatures_.push_back(r);
  }
}

const std::vector<Sidebar::Row> &Sidebar::rows() const {
  switch (panel_) {
    case Search: return search_;
    case Outline: return rows_;
    case Bookmarks: return bookmarks_;
    case Annotations: return notes_;
    case Signatures: return signatures_;
    default: return rows_;
  }
}

int Sidebar::row_h() const {
  switch (panel_) {
    case Outline: return S(34);
    case Signatures: return S(84);
    default: return S(54);
  }
}

int Sidebar::thumb_cols() const { return panel_w() >= S(340) ? 2 : 1; }

int Sidebar::thumb_w() const {
  int cols = thumb_cols();
  int w0 = (panel_w() - S(48) - (cols - 1) * S(20)) / cols;
  return std::clamp(w0, S(60), S(180));
}

int Sidebar::thumb_cell_h(int page) const {
  DocView *v = app_->view();
  return (v ? v->thumb_height(page, thumb_w()) : thumb_w()) + S(44);
}

int Sidebar::content_h() const {
  Document *d = app_->document();
  if (!d) return 0;
  if (panel_ == Thumbnails) {
    int cols = thumb_cols(), total = S(10);
    for (int p = 0; p < d->count(); p += cols) {
      int rh = 0;
      for (int c = 0; c < cols && p + c < d->count(); ++c) rh = std::max(rh, thumb_cell_h(p + c));
      total += rh;
    }
    return total + S(10);
  }
  return S(8) + int(rows().size()) * row_h() + S(8);
}

void Sidebar::clamp() {
  double max = std::max(0, content_h() - (y() + h() - content_top()));
  scroll_ = std::clamp(scroll_, 0.0, max);
}

void Sidebar::page_changed(int page) {
  Document *d = app_->document();
  if (panel_ != Thumbnails || !d || !open_) {
    redraw();
    return;
  }
  // Keep the current page's thumbnail in view.
  int cols = thumb_cols(), y0 = S(10), row_hh = 0;
  for (int p = 0; p < d->count(); p += cols) {
    row_hh = 0;
    for (int c = 0; c < cols && p + c < d->count(); ++c) row_hh = std::max(row_hh, thumb_cell_h(p + c));
    if (page < p + cols) break;
    y0 += row_hh;
  }
  int viewh = y() + h() - content_top();
  if (y0 < scroll_) scroll_ = y0 - S(8);
  else if (y0 + row_hh > scroll_ + viewh) scroll_ = std::min(double(y0 - S(8)), double(y0 + row_hh - viewh + S(8)));
  clamp();
  redraw();
}

// Index of the thumbnail or row under the pointer, or -1.
int Sidebar::item_at(int mx, int my) const {
  Document *d = app_->document();
  if (!d || !open_ || mx < panel_x() || my < content_top()) return -1;
  int top = content_top() - int(scroll_);
  if (panel_ == Thumbnails) {
    int cols = thumb_cols(), tw = thumb_w(), yy = top + S(10);
    int gap = cols > 1 ? (panel_w() - cols * tw) / (cols + 1) : (panel_w() - tw) / 2;
    for (int p = 0; p < d->count(); p += cols) {
      int rh = 0;
      for (int c = 0; c < cols && p + c < d->count(); ++c) rh = std::max(rh, thumb_cell_h(p + c));
      if (my >= yy && my < yy + rh) {
        for (int c = 0; c < cols && p + c < d->count(); ++c) {
          int cx = panel_x() + gap + c * (tw + gap);
          if (mx >= cx - S(10) && mx < cx + tw + S(10)) return p + c;
        }
        return -1;
      }
      yy += rh;
      if (yy > my) break;
    }
    return -1;
  }
  int idx = (my - top - S(8)) / row_h();
  if (my - top - S(8) < 0 || idx >= int(rows().size())) return -1;
  return idx;
}

int Sidebar::rail_item_at(int mx, int my) const {
  if (mx >= x() + rail_w()) return -1;
  int ry = y() + S(8);
  for (int k = 0; k < PanelCount; ++k) {
    if (my >= ry && my < ry + rail_step() - S(4)) return k;
    ry += rail_step();
  }
  int bx, by, bw, bh;
  page_box(bx, by, bw, bh);
  if (h() < S(520)) return -1;
  if (my >= by - S(40) && my < by - S(4)) return 100;  // page up
  if (my >= by + bh + S(26) && my < by + bh + S(62)) return 101;  // page down
  return -1;
}

void Sidebar::draw_rail(Painter &p) {
  const Theme &t = app_->theme();
  p.fill_rect(x(), y(), rail_w(), h(), t.surface);
  p.line(x() + rail_w() - 0.5, y(), x() + rail_w() - 0.5, y() + h(), t.border);
  int ry = y() + S(8);
  int cs = std::min(S(42), rail_step() - S(6));
  for (int k = 0; k < PanelCount; ++k) {
    int cx = x() + (rail_w() - cs) / 2;
    bool sel = open_ && k == int(panel_);
    if (sel) p.fill_rounded(cx, ry + S(2), cs, cs, S(10), t.dark ? t.hover.mix(t.text, 0.06) : t.hover.mix(t.border, 0.5));
    else if (k == hover_rail_) p.fill_rounded(cx, ry + S(2), cs, cs, S(10), t.hover);
    icon::draw(p, kRailGlyphs[k], cx + (cs - S(22)) / 2.0, ry + S(2) + (cs - S(22)) / 2.0, S(22),
               sel ? t.text : t.text_dim);
    ry += rail_step();
  }
  int bx, by, bw, bh;
  page_box(bx, by, bw, bh);
  bool arrows = h() >= S(520);
  int cx = x() + (rail_w() - S(30)) / 2;
  if (arrows) {
    if (hover_rail_ == 100) p.fill_rounded(cx, by - S(38), S(30), S(30), S(8), t.hover);
    if (hover_rail_ == 101) p.fill_rounded(cx, by + bh + S(30), S(30), S(30), S(8), t.hover);
    icon::draw(p, icon::Up, cx + S(4), by - S(34), S(22), t.text);
    icon::draw(p, icon::Down, cx + S(4), by + bh + S(34), S(22), t.text);
  }
  p.fill_rounded(bx - S(3), by - S(3), bw + S(6), bh + S(6), S(7), t.background);
  p.stroke_rounded(bx - S(3) + 0.5, by - S(3) + 0.5, bw + S(5), bh + S(5), S(7),
                   Fl::focus() && Fl::focus()->parent() == window() && Fl::focus()->x() == bx ? t.accent : t.accent.alpha(0.7),
                   Sd(1.5));
}

void Sidebar::draw() {
  const Theme &t = app_->theme();
  Document *d = app_->document();
  DocView *v = app_->view();
  if (panel_ == Annotations && notes_dirty_) rebuild_notes();
  if (panel_ == Search && v && (search_key_ != v->find_query() || search_count_ != v->find_count())) rebuild_search();
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    draw_rail(p);
    if (open_) {
      p.fill_rect(panel_x(), y(), panel_w(), h(), t.surface_alt);
      p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), t.border);
      p.line(panel_x(), y() + header_h() - 0.5, x() + w(), y() + header_h() - 0.5, t.border.alpha(0.6));
      // Header actions.
      int ax = x() + w() - S(44);
      int ay = y() + (header_h() - S(32)) / 2;
      if (panel_ == Bookmarks || panel_ == Signatures) {
        if (hover_ == -10) p.fill_rounded(ax, ay, S(32), S(32), S(8), t.hover);
        icon::draw(p, icon::Plus, ax + S(6), ay + S(6), S(20), t.text);
      }
      if (panel_ == Signatures) {
        if (hover_ == -11) p.fill_rounded(ax - S(36), ay, S(32), S(32), S(8), t.hover);
        icon::draw(p, icon::Pen, ax - S(30), ay + S(6), S(20), t.text);
      }
      if (panel_ == Search) {
        int sx, sy, sw, sh;
        search_box(sx, sy, sw, sh);
        p.fill_rounded(sx - S(4), sy - S(2), sw + S(8), sh + S(4), S(8), t.background);
        p.stroke_rounded(sx - S(4) + 0.5, sy - S(2) + 0.5, sw + S(7), sh + S(3), S(8), t.border);
        int bx = sx + sw + S(8);
        if (app_->find_case()) p.fill_rounded(bx, sy, S(30), S(30), S(8), t.accent.alpha(0.18));
        else if (hover_ == -12) p.fill_rounded(bx, sy, S(30), S(30), S(8), t.hover);
      }
      cairo_rectangle(cr, panel_x(), content_top(), panel_w(), y() + h() - content_top());
      cairo_clip(cr);
      int top = content_top() - int(scroll_);
      if (d && v && panel_ == Thumbnails) {
        int cols = thumb_cols(), tw = thumb_w(), yy = top + S(10), cur = v->current_page();
        int gap = cols > 1 ? (panel_w() - cols * tw) / (cols + 1) : (panel_w() - tw) / 2;
        for (int pg = 0; pg < d->count(); pg += cols) {
          int rh = 0;
          for (int c = 0; c < cols && pg + c < d->count(); ++c) rh = std::max(rh, thumb_cell_h(pg + c));
          if (yy + rh >= content_top() && yy < y() + h()) {
            for (int c = 0; c < cols && pg + c < d->count(); ++c) {
              int page = pg + c;
              int th = v->thumb_height(page, tw);
              int tx = panel_x() + gap + c * (tw + gap), ty = yy + S(8);
              if (page == cur) {
                p.fill_rounded(tx - S(12), ty - S(10), tw + S(24), th + S(48), S(10), t.accent.alpha(0.10));
                p.stroke_rounded(tx - S(12) + 0.5, ty - S(10) + 0.5, tw + S(23), th + S(47), S(10), t.accent, Sd(2));
              } else if (page == hover_) {
                p.fill_rounded(tx - S(12), ty - S(10), tw + S(24), th + S(48), S(10), t.hover);
              }
              p.shadow(tx, ty, tw, th, S(3), t.shadow.alpha(0.2), S(4));
              p.fill_rect(tx, ty, tw, th, Color::hex(0xFFFFFF));
              if (cairo_surface_t *s = v->thumbnail(page, tw)) {
                double f = double(tw) / cairo_image_surface_get_width(s);
                cairo_save(cr);
                cairo_translate(cr, tx, ty);
                cairo_scale(cr, f, f);
                cairo_set_source_surface(cr, s, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), f == 1.0 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
                cairo_paint(cr);
                cairo_restore(cr);
              }
              p.stroke_rounded(tx + 0.5, ty + 0.5, tw - 1, th - 1, S(2), t.border.alpha(0.6));
              // Where a dragged page goes.
              if (drag_from_ >= 0 && drag_to_ == page && drag_to_ != drag_from_) {
                double lx = drag_to_ > drag_from_ ? tx + tw + gap / 2.0 : tx - gap / 2.0;
                if (cols == 1) p.fill_rounded(tx, drag_to_ > drag_from_ ? ty + th + S(36) : ty - S(12), tw, S(3), S(1.5), t.accent);
                else p.fill_rounded(lx - S(1.5), ty, S(3), th, S(1.5), t.accent);
              }
            }
          }
          yy += rh;
          if (yy > y() + h() + S(400)) break;
        }
      } else if (d) {
        const auto &list = rows();
        int rh = row_h(), cur = v ? v->current_page() : -1;
        for (int k = 0; k < int(list.size()); ++k) {
          int ry = top + S(8) + k * rh;
          if (ry + rh < content_top() || ry > y() + h()) continue;
          const Row &r = list[size_t(k)];
          bool sel = (panel_ == Search && v && r.hit == v->find_index()) ||
                     (panel_ == Annotations && v && v->selected_annot_page() == r.page && v->selected_annot() &&
                      std::fabs(v->selected_annot()->box.x1 - r.box.x1) < 1 && std::fabs(v->selected_annot()->box.y1 - r.box.y1) < 1);
          if (sel) p.fill_rounded(panel_x() + S(8), ry, panel_w() - S(16), rh - S(4), S(8), t.accent.alpha(0.14));
          else if (k == hover_) p.fill_rounded(panel_x() + S(8), ry, panel_w() - S(16), rh - S(4), S(8), t.hover);
          int indent = panel_x() + S(14) + r.depth * S(14);
          if (panel_ == Outline && r.item && !r.item->children.empty())
            icon::draw(p, r.item->expanded ? icon::ChevronDown : icon::Chevron, indent, ry + (rh - S(16)) / 2.0, S(16),
                       t.text_dim);
          if (panel_ == Annotations) {
            Color bar = t.border;
            for (const auto &a : d->annotations(r.page))
              if (a.type == r.type && std::fabs(a.box.x1 - r.box.x1) < 1 && a.has_color) bar = Color{a.color.r, a.color.g, a.color.b, 1};
            p.fill_rounded(panel_x() + S(16), ry + S(10), S(4), rh - S(24), S(2), bar);
          }
          if (panel_ == Bookmarks)
            icon::draw(p, icon::Bookmark, panel_x() + S(16), ry + (rh - S(22)) / 2.0, S(18),
                       r.page == cur ? t.accent_text : t.text_dim);
          if (panel_ == Signatures) {
            auto &cache = signature_images();
            if (!cache.count(r.path)) cache[r.path] = load_image(r.path, 0, 0);
            p.fill_rounded(panel_x() + S(16), ry + S(4), panel_w() - S(32), rh - S(14), S(8), Color::hex(0xFFFFFF));
            if (cairo_surface_t *img = cache[r.path]) {
              double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
              double bw = panel_w() - S(48), bh = rh - S(22);
              double f = std::min(bw / iw, bh / ih);
              cairo_save(cr);
              cairo_translate(cr, panel_x() + S(24) + (bw - iw * f) / 2, ry + S(8) + (bh - ih * f) / 2);
              cairo_scale(cr, f, f);
              cairo_set_source_surface(cr, img, 0, 0);
              cairo_paint(cr);
              cairo_restore(cr);
            }
          }
        }
      }
    }
    cairo_restore(cr);
  }
  // Text.
  {
    int bx, by, bw, bh;
    page_box(bx, by, bw, bh);
    text_font(t.font_size - S(1));
    draw_text(d ? std::to_string(d->count()) : std::string("–"), x(), by + bh + S(4), rail_w(), S(22), t.text_dim, 1);
  }
  if (!open_) return;
  text_font(t.font_size + S(3), true);
  draw_text(panel_title(panel_), panel_x() + S(18), y(), panel_w() - S(100), header_h(), t.text);
  fl_push_clip(panel_x(), content_top(), panel_w(), y() + h() - content_top());
  int top = content_top() - int(scroll_);
  if (!d) {
    text_font(t.font_size - S(1));
    draw_text(_("No document"), panel_x() + S(16), content_top() + S(12), panel_w() - S(32), S(24), t.text_dim);
  } else if (panel_ == Thumbnails && v) {
    int cols = thumb_cols(), tw = thumb_w(), yy = top + S(10), cur = v->current_page();
    int gap = cols > 1 ? (panel_w() - cols * tw) / (cols + 1) : (panel_w() - tw) / 2;
    for (int pg = 0; pg < d->count(); pg += cols) {
      int rh = 0;
      for (int c = 0; c < cols && pg + c < d->count(); ++c) rh = std::max(rh, thumb_cell_h(pg + c));
      if (yy + rh >= content_top() && yy < y() + h())
        for (int c = 0; c < cols && pg + c < d->count(); ++c) {
          int page = pg + c, th = v->thumb_height(page, tw);
          int tx = panel_x() + gap + c * (tw + gap);
          text_font(t.font_size, page == cur);
          draw_text(d->page_label(page), tx, yy + S(8) + th + S(6), tw, S(24), page == cur ? t.accent_text : t.text_dim, 1);
        }
      yy += rh;
      if (yy > y() + h() + S(400)) break;
    }
  } else {
    const auto &list = rows();
    int rh = row_h();
    if (list.empty()) {
      const char *empty = panel_ == Outline       ? _("This document has no outline")
                          : panel_ == Annotations ? _("No annotations yet")
                          : panel_ == Bookmarks   ? _("No bookmarks yet — press + or Ctrl+D")
                          : panel_ == Signatures  ? _("No signatures yet — draw one with the pen or add an image with +")
                          : v && v->find_running() ? _("Searching…")
                          : v && !v->find_query().empty() ? _("No matches")
                                                          : _("Type and press Enter");
      text_font(t.font_size - S(1));
      fl_color(t.text_dim.fl());
      fl_draw(empty, panel_x() + S(18), content_top() + S(10), panel_w() - S(36), S(60),
              FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
    }
    for (int k = 0; k < int(list.size()); ++k) {
      int ry = top + S(8) + k * rh;
      if (ry + rh < content_top() || ry > y() + h()) continue;
      const Row &r = list[size_t(k)];
      if (panel_ == Outline) {
        int indent = panel_x() + S(14) + r.depth * S(14) + S(18);
        text_font(t.font_size - (r.depth ? S(1) : 0), r.depth == 0);
        int pw = r.page >= 0 ? S(36) : 0;
        draw_text(r.title, indent, ry, x() + w() - indent - S(12) - pw, rh, t.text);
        if (r.page >= 0) {
          text_font(t.font_size - S(2));
          draw_text(std::to_string(r.page + 1), x() + w() - S(12) - pw, ry, pw, rh, t.text_dim, 2);
        }
      } else if (panel_ == Signatures) {
        // The picture says it all.
      } else {
        int lx = panel_x() + (panel_ == Bookmarks ? S(42) : S(28));
        text_font(t.font_size - S(1), true);
        draw_text(r.title, lx, ry + S(5), x() + w() - lx - S(16), S(20), t.text);
        text_font(t.font_size - S(2));
        draw_text(r.detail, lx, ry + S(26), x() + w() - lx - S(16), S(18), t.text_dim);
      }
    }
  }
  fl_pop_clip();
  if (panel_ == Search && v) {
    int sx, sy, sw, sh;
    search_box(sx, sy, sw, sh);
    text_font(t.font_size - S(2), true);
    draw_text("Aa", sx + sw + S(8), sy, S(30), S(30), app_->theme().text_dim, 1);
    std::string count;
    if (v->find_running()) count = format(_("%d found so far…"), v->find_count());
    else if (!v->find_query().empty()) count = format(v->find_count() == 1 ? _("%d result") : _("%d results"), v->find_count());
    text_font(t.font_size - S(2));
    draw_text(count, panel_x() + S(18), sy + sh + S(4), panel_w() - S(36), S(24), t.text_dim);
  }
}

int Sidebar::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  DocView *v = app_->view();
  Document *d = app_->document();
  int ax = x() + w() - S(44), ay = y() + (header_h() - S(32)) / 2;
  auto header_action = [&]() -> int {
    if (!open_ || my < ay || my >= ay + S(32)) return -1;
    if ((panel_ == Bookmarks || panel_ == Signatures) && mx >= ax && mx < ax + S(32)) return -10;
    if (panel_ == Signatures && mx >= ax - S(36) && mx < ax - S(4)) return -11;
    return -1;
  };
  auto case_button = [&]() -> bool {
    if (!open_ || panel_ != Search) return false;
    int sx, sy, sw, sh;
    search_box(sx, sy, sw, sh);
    return mx >= sx + sw + S(8) && mx < sx + sw + S(38) && my >= sy && my < sy + S(30);
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int k = header_action();
      if (k == -1 && case_button()) k = -12;
      if (k == -1) k = item_at(mx, my);
      int r = rail_item_at(mx, my);
      bool edge = open_ && mx >= x() + w() - S(5);
      window()->cursor(edge ? FL_CURSOR_WE : FL_CURSOR_DEFAULT);
      if (k != hover_ || r != hover_rail_) {
        hover_ = k;
        hover_rail_ = r;
        redraw();
      }
      const char *tip = nullptr;
      if (r >= 0 && r < PanelCount) tip = panel_title(Panel(r));
      else if (r == 100) tip = _("Previous page");
      else if (r == 101) tip = _("Next page");
      else if (k == -10) tip = panel_ == Bookmarks ? _("Bookmark this page (Ctrl+D)") : _("Add a signature image");
      else if (k == -11) tip = _("Draw a signature");
      else if (k == -12) tip = _("Match case");
      tooltip(tip);
      return 1;
    }
    case FL_LEAVE:
      hover_ = hover_rail_ = -1;
      window()->cursor(FL_CURSOR_DEFAULT);
      redraw();
      return 1;
    case FL_MOUSEWHEEL:
      if (mx < panel_x()) return 1;
      scroll_ += Fl::event_dy() * S(60);
      clamp();
      redraw();
      return 1;
    case FL_PUSH: {
      press_x_ = mx;
      press_y_ = my;
      drag_from_ = drag_to_ = -1;
      if (open_ && mx >= x() + w() - S(5)) {
        resizing_ = true;
        return 1;
      }
      int r = rail_item_at(mx, my);
      if (r >= 0 && r < PanelCount) {
        if (open_ && r == int(panel_)) {
          app_->toggle_sidebar();
        } else {
          set_panel(Panel(r));
          if (!open_) app_->toggle_sidebar();
          else app_->layout();
          app_->settings_changed();
        }
        if (panel_ == Search && open_) app_->show_find(true);
        return 1;
      }
      if (r == 100 && v) v->prev_page();
      if (r == 101 && v) v->next_page();
      if (r >= 0) return 1;
      int h = header_action();
      if (h == -10) {
        if (panel_ == Bookmarks) app_->toggle_bookmark();
        else app_->choose_signature();
        rebuild_bookmarks();
        rebuild_signatures();
        redraw();
        return 1;
      }
      if (h == -11) {
        app_->draw_signature();
        rebuild_signatures();
        redraw();
        return 1;
      }
      if (case_button()) {
        app_->toggle_find_case();
        redraw();
        return 1;
      }
      int k = item_at(mx, my);
      if (k < 0 || !v || !d) return 1;
      if (panel_ == Thumbnails) {
        drag_from_ = k;
        v->go_to(k, -1, false);
      } else {
        const Row &row = rows()[size_t(k)];
        switch (panel_) {
          case Outline: {
            int indent = panel_x() + S(14) + row.depth * S(14);
            if (row.item && !row.item->children.empty() &&
                (mx < indent + S(18) || row.target.kind == LinkTarget::Nothing)) {
              row.item->expanded = !row.item->expanded;
              rebuild_rows();
              clamp();
            } else {
              v->follow(row.target);
            }
            break;
          }
          case Search: v->show_hit(row.hit); break;
          case Bookmarks: v->go_to(row.page, -1, true); break;
          case Annotations:
            for (const auto &a : d->annotations(row.page))
              if (a.type == row.type && std::fabs(a.box.x1 - row.box.x1) < 1 && std::fabs(a.box.y1 - row.box.y1) < 1) {
                v->select_annot(row.page, a);
                break;
              }
            break;
          case Signatures:
            if (cairo_surface_t *img = load_image(row.path, 0, 0)) {
              v->set_signature(img);
              v->set_tool(Tool::Signature);
              app_->toast(_("Click on the page to place the signature"));
            }
            break;
          default: break;
        }
      }
      redraw();
      return 1;
    }
    case FL_DRAG:
      if (resizing_) {
        app_->set_side_w(mx - x() - rail_w());
        return 1;
      }
      if (drag_from_ >= 0 && std::abs(my - press_y_) + std::abs(mx - press_x_) > S(8)) {
        int k = item_at(mx, my);
        if (my < content_top() + S(20)) {
          scroll_ -= S(20);
          clamp();
        } else if (my > y() + h() - S(20)) {
          scroll_ += S(20);
          clamp();
        }
        if (k >= 0) drag_to_ = k;
        window()->cursor(FL_CURSOR_MOVE);
        redraw();
      }
      return 1;
    case FL_RELEASE:
      if (resizing_) {
        resizing_ = false;
        return 1;
      }
      window()->cursor(FL_CURSOR_DEFAULT);
      if (drag_from_ >= 0 && drag_to_ >= 0 && drag_to_ != drag_from_) {
        int from = drag_from_, to = drag_to_;
        drag_from_ = drag_to_ = -1;
        app_->page_operation("move", from, to);
      }
      drag_from_ = drag_to_ = -1;
      redraw();
      return 1;
  }
  return Fl_Widget::handle(event);
}

}  // namespace xanara
