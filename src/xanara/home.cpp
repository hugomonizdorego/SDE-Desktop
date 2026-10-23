// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Home tab: quick actions (open, merge, create a PDF from images) and
// the recently opened documents as a grid of first page thumbnails, which
// are rendered in the background one per idle moment.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

namespace {

struct Quick {
  int id;
  icon::Kind glyph;
  const char *title, *detail;
};

const Quick kQuick[] = {
    {-1, icon::Open, N_("Open File"), N_("PDF documents · Ctrl+O")},
    {-2, icon::Merge, N_("Merge PDFs"), N_("Combine several files into one")},
    {-3, icon::ImagesToPdf, N_("Images to PDF"), N_("Scans and photos as pages")},
};

std::string pretty_dir(const std::string &path) {
  std::string dir = path.substr(0, path.rfind('/'));
  std::string home = home_dir();
  if (starts_with(dir, home)) dir = "~" + dir.substr(home.size());
  return dir;
}

}  // namespace

HomeView::HomeView(XanaraWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

HomeView::~HomeView() {
  Fl::remove_timeout(thumb_cb, this);
  for (auto &t : thumbs_)
    if (t.second) cairo_surface_destroy(t.second);
}

void HomeView::refresh() {
  build();
  redraw();
}

void HomeView::build() {
  cards_.clear();
  int pad = std::min(S(40), std::max(S(16), w() / 30));
  int x0 = x() + pad, y0 = y() + pad + S(54) - int(scroll_);
  int avail = w() - 2 * pad;
  // Quick actions.
  int qw = std::min(S(260), (avail - 2 * S(14)) / 3), qh = S(76);
  if (qw < S(170)) qw = avail;
  int cx = x0, cy = y0;
  for (const auto &q : kQuick) {
    if (cx + qw > x0 + avail + 1) {
      cx = x0;
      cy += qh + S(12);
    }
    cards_.push_back(Card{cx, cy, qw, qh, q.id});
    cx += qw + S(14);
  }
  cy += qh + S(64);
  // Recent documents.
  auto rec = app_->recent();
  int cw = S(168), ch = S(246), gap = S(18);
  int cols = std::max(1, (avail + gap) / (cw + gap));
  for (size_t k = 0; k < rec.size(); ++k) {
    int col = int(k) % cols, row = int(k) / cols;
    cards_.push_back(Card{x0 + col * (cw + gap), cy + row * (ch + gap), cw, ch, int(k)});
    if (!thumbs_.count(rec[k].path) &&
        std::find(pending_.begin(), pending_.end(), rec[k].path) == pending_.end())
      pending_.push_back(rec[k].path);
  }
  int rows = rec.empty() ? 1 : (int(rec.size()) + cols - 1) / cols;
  content_h_ = (cy - y0) + S(54) + pad + rows * (ch + gap) + pad;
  if (!pending_.empty() && !Fl::has_timeout(thumb_cb, this)) Fl::add_timeout(0.02, thumb_cb, this);
}

void HomeView::thumb_cb(void *p) {
  auto *self = static_cast<HomeView *>(p);
  if (self->pending_.empty()) return;
  std::string path = self->pending_.front();
  self->pending_.erase(self->pending_.begin());
  cairo_surface_t *s = nullptr;
  std::string error;
  bool enc = false;
  if (auto d = Document::open(path, "", error, enc)) {
    RenderParams rp;
    int tw = S(144);
    rp.scale = tw / std::max(1.0, d->width(0));
    int w, h;
    page_pixels(d->width(0), d->height(0), rp, w, h);
    s = d->render(0, rp, 0, 0, w, std::min(h, S(190)));
  }
  self->thumbs_[path] = s;
  self->redraw();
  if (!self->pending_.empty()) Fl::repeat_timeout(0.01, thumb_cb, p);
}

void HomeView::draw() {
  const Theme &t = app_->theme();
  build();
  auto rec = app_->recent();
  int pad = std::min(S(40), std::max(S(16), w() / 30));
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), t.background);
    for (int k = 0; k < int(cards_.size()); ++k) {
      const Card &c = cards_[size_t(k)];
      bool hov = c.action == hover_;
      if (c.action < 0) {
        p.fill_rounded(c.x, c.y, c.w, c.h, S(12), hov ? t.hover.mix(t.surface, 0.3) : t.surface);
        p.stroke_rounded(c.x + 0.5, c.y + 0.5, c.w - 1, c.h - 1, S(12), hov ? t.accent : t.border);
        p.fill_rounded(c.x + S(16), c.y + (c.h - S(44)) / 2.0, S(44), S(44), S(12), t.accent.alpha(0.14));
        for (const auto &q : kQuick)
          if (q.id == c.action)
            icon::draw(p, q.glyph, c.x + S(26), c.y + (c.h - S(24)) / 2.0, S(24), t.accent_text);
        continue;
      }
      // A recent document.
      if (hov) p.fill_rounded(c.x - S(6), c.y - S(6), c.w + S(12), c.h + S(12), S(12), t.hover);
      int tw = S(144), th = S(190);
      int tx = c.x + (c.w - tw) / 2, ty = c.y + S(4);
      p.shadow(tx, ty, tw, th, S(4), t.shadow.alpha(0.25), S(6));
      p.fill_rect(tx, ty, tw, th, Color::hex(0xFFFFFF));
      const std::string &path = rec[size_t(c.action)].path;
      auto it = thumbs_.find(path);
      if (it != thumbs_.end() && it->second) {
        cairo_save(cr);
        cairo_rectangle(cr, tx, ty, tw, th);
        cairo_clip(cr);
        cairo_set_source_surface(cr, it->second, tx, ty);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        icon::draw(p, it == thumbs_.end() ? icon::Document : icon::Lock, tx + (tw - S(40)) / 2.0,
                   ty + (th - S(40)) / 2.0, S(40), Color::hex(0x9AA3AF));
      }
      p.stroke_rounded(tx + 0.5, ty + 0.5, tw - 1, th - 1, S(2), hov ? t.accent : t.border.alpha(0.7), hov ? Sd(2) : 1);
    }
    cairo_restore(cr);
  }
  fl_push_clip(x(), y(), w(), h());
  int y0 = y() + pad - int(scroll_);
  text_font(t.font_size + S(8), true);
  draw_text(_("Welcome to Xanara"), x() + pad, y0, w() - 2 * pad, S(36), t.text);
  for (const auto &c : cards_) {
    if (c.action < 0) {
      for (const auto &q : kQuick)
        if (q.id == c.action) {
          text_font(t.font_size + S(1), true);
          draw_text(_(q.title), c.x + S(74), c.y + S(14), c.w - S(84), S(26), t.text);
          text_font(t.font_size - S(1));
          draw_text(_(q.detail), c.x + S(74), c.y + S(40), c.w - S(84), S(22), t.text_dim);
        }
      continue;
    }
    const auto &e = rec[size_t(c.action)];
    text_font(t.font_size - S(1), true);
    draw_text(basename_of(e.path), c.x, c.y + S(202), c.w, S(20), t.text, 1);
    text_font(t.font_size - S(2));
    draw_text(format(_("Page %d · %s"), e.page + 1, pretty_dir(e.path).c_str()), c.x, c.y + S(222), c.w, S(18),
              t.text_dim, 1);
  }
  int recent_y = 0;
  for (const auto &c : cards_)
    if (c.action >= 0) {
      recent_y = c.y;
      break;
    }
  int qbottom = 0;
  for (const auto &c : cards_)
    if (c.action < 0) qbottom = std::max(qbottom, c.y + c.h);
  text_font(t.font_size + S(2), true);
  draw_text(_("Recent"), x() + pad, qbottom + S(20), S(300), S(30), t.text);
  if (rec.empty()) {
    text_font(t.font_size);
    draw_text(_("Documents you open appear here. Drop a PDF anywhere in this window to open it."), x() + pad,
              qbottom + S(56), w() - 2 * pad, S(26), t.text_dim);
  }
  (void)recent_y;
  fl_pop_clip();
}

int HomeView::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  auto card_at = [&]() -> int {
    for (const auto &c : cards_)
      if (mx >= c.x && mx < c.x + c.w && my >= c.y && my < c.y + c.h) return c.action;
    return -1000;
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int k = card_at();
      if (k != hover_) {
        hover_ = k;
        auto rec = app_->recent();
        tooltip(k >= 0 && size_t(k) < rec.size() ? nullptr : nullptr);
        redraw();
      }
      window()->cursor(k != -1000 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1000;
      window()->cursor(FL_CURSOR_DEFAULT);
      redraw();
      return 1;
    case FL_MOUSEWHEEL:
      scroll_ = std::clamp(scroll_ + Fl::event_dy() * S(60), 0.0, double(std::max(0, content_h_ - h())));
      redraw();
      return 1;
    case FL_PUSH: {
      int k = card_at();
      if (k == -1000) return 1;
      auto rec = app_->recent();
      if (Fl::event_button() == FL_RIGHT_MOUSE && k >= 0 && size_t(k) < rec.size()) {
        Fl_Menu_Item items[3];
        memset(items, 0, sizeof items);
        items[0].text = _("Open");
        items[1].text = _("Remove from Recent");
        for (int i = 0; i < 2; ++i) {
          items[i].labelsize(app_->theme().font_size);
          items[i].labelcolor(app_->theme().text.fl());
        }
        const Fl_Menu_Item *m = items->popup(mx, my);
        if (m == items) app_->open_path(rec[size_t(k)].path);
        else if (m == items + 1) app_->forget_recent(rec[size_t(k)].path);
        return 1;
      }
      window()->cursor(FL_CURSOR_DEFAULT);
      if (k == -1) app_->open_dialog();
      else if (k == -2) app_->merge_files();
      else if (k == -3) app_->images_to_pdf_dialog();
      else if (size_t(k) < rec.size()) app_->open_path(rec[size_t(k)].path, rec[size_t(k)].page);
      return 1;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
    case FL_DND_LEAVE:
      return 1;
    case FL_PASTE: {
      std::string text(Fl::event_text(), size_t(Fl::event_length()));
      for (auto &line : split(text, '\n')) {
        std::string f = trim(line);
        if (!f.empty()) app_->open_path(path_from_argument(f));
      }
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}

}  // namespace xanara
