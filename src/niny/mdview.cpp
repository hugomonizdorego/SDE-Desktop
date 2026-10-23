// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// A light rendered view of Markdown documents: headings, paragraphs with
// bold/italic/code/links, lists, task lists, quotes, code blocks and rules,
// wrapped to a comfortable reading width. It is read-only; links open in the
// web browser.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "niny.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

namespace niny {

MarkdownView::MarkdownView(App *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

void MarkdownView::show_doc(Document *doc) {
  if (doc != doc_) {
    doc_ = doc;
    scroll_ = 0;
    rev_ = ~0u;
  }
  redraw();
}

namespace {

Fl_Font font_for(bool bold, bool italic, bool code) {
  if (code) return FL_FREE_FONT;
  if (bold && italic) return FL_HELVETICA_BOLD_ITALIC;
  if (bold) return FL_HELVETICA_BOLD;
  if (italic) return FL_HELVETICA_ITALIC;
  return FL_HELVETICA;
}

}  // namespace

void MarkdownView::relayout() {
  runs_.clear();
  decos_.clear();
  laid_w_ = w();
  if (!doc_) return;
  rev_ = doc_->revision;
  const Look &L = look();
  const Settings &st = app_->settings();
  int body = std::max(L.theme.font_size + S(3), int(std::lround(st.font_size * 4.0 / 3.0 * ui_scale())) + st.zoom + S(2));
  double pad = S(36);
  double col_w = std::min<double>(w() - 2 * pad, S(860));
  if (col_w < S(120)) col_w = w() - S(16);
  double x0 = x() + (w() - col_w) / 2;
  double y = S(28);
  auto lines = md::parse(doc_->text());

  // Lays out inline spans between [left, right) starting at y; returns the
  // y below the last line.
  auto wrap = [&](const std::vector<md::Span> &spans, double left, double right, int size, bool bold_all,
                  bool italic_all, Color color, double line_h) {
    double cx = left;
    bool line_used = false;
    for (auto &sp : spans) {
      bool bold = sp.bold || bold_all, italic = sp.italic || italic_all;
      int fsize = sp.code ? std::max(7, int(size * 0.9)) : size;
      Fl_Font f = font_for(bold, italic, sp.code);
      fl_font(f, fsize);
      Color c = sp.link ? L.accent_text : sp.code ? L.syntax[kString - 'A'] : color;
      // Split into words, keeping the following spaces with each word.
      size_t i = 0;
      const std::string &t = sp.text;
      while (i < t.size()) {
        size_t e = i;
        while (e < t.size() && t[e] != ' ') ++e;
        size_t sp_end = e;
        while (sp_end < t.size() && t[sp_end] == ' ') ++sp_end;
        std::string word = t.substr(i, e - i), tail = t.substr(e, sp_end - e);
        double ww = fl_width(word.c_str()), tw = tail.empty() ? 0 : fl_width(tail.c_str());
        if (cx + ww > right && line_used) {
          y += line_h;
          cx = left;
          line_used = false;
        }
        if (!(word.empty() && !line_used)) {
          Run r;
          r.text = word + tail;
          r.x = cx;
          r.y = y;
          r.w = ww;
          r.font = f;
          r.size = fsize;
          r.color = c;
          r.code = sp.code;
          r.strike = sp.strike;
          r.underline = sp.link;
          r.url = sp.url;
          runs_.push_back(r);
          cx += ww + tw;
          line_used = true;
        }
        i = sp_end;
      }
    }
    return y + line_h;
  };

  double line_h = std::round(body * 1.55);
  size_t i = 0;
  while (i < lines.size()) {
    const md::Line &ln = lines[i];
    switch (ln.kind) {
      case md::Block::Blank:
        y += std::round(body * 0.5);
        break;
      case md::Block::Heading: {
        static const double scale[] = {1.0, 1.9, 1.55, 1.3, 1.15, 1.0, 0.92};
        int hs = int(std::lround(body * scale[std::min(6, std::max(1, ln.level))]));
        if (y > S(28)) y += std::round(body * 0.6);
        y = wrap(md::parse_inline(ln.text), x0, x0 + col_w, hs, true, false, L.text, std::round(hs * 1.35));
        if (ln.level <= 2) {
          decos_.push_back({7, x0, y + S(2), col_w, 1});
          y += S(8);
        }
        y += std::round(body * 0.35);
        break;
      }
      case md::Block::Paragraph:
        y = wrap(md::parse_inline(ln.text), x0, x0 + col_w, body, false, false, L.text, line_h);
        y += std::round(body * 0.45);
        break;
      case md::Block::Bullet:
      case md::Block::Numbered:
      case md::Block::Task: {
        double indent = S(26) * (ln.level + 1);
        double tx = x0 + indent;
        if (ln.kind == md::Block::Bullet) {
          decos_.push_back({4, tx - S(15), y + line_h / 2 - Sd(2.5), Sd(5), Sd(5)});
        } else if (ln.kind == md::Block::Task) {
          decos_.push_back({ln.checked ? 6 : 5, tx - S(20), y + (line_h - S(14)) / 2, double(S(14)), double(S(14))});
          tx += S(4);
        } else {
          fl_font(FL_HELVETICA, body);
          Run r;
          r.text = ln.marker;
          r.w = fl_width(r.text.c_str());
          r.x = tx - S(6) - r.w;
          r.y = y;
          r.font = FL_HELVETICA;
          r.size = body;
          r.color = L.dim;
          runs_.push_back(r);
        }
        y = wrap(md::parse_inline(ln.text), tx, x0 + col_w, body, false, false,
                 ln.kind == md::Block::Task && ln.checked ? L.dim : L.text, line_h);
        y += S(2);
        if (i + 1 < lines.size() && lines[i + 1].kind == md::Block::Paragraph) y += std::round(body * 0.3);
        break;
      }
      case md::Block::Quote: {
        double top = y;
        y = wrap(md::parse_inline(ln.text), x0 + S(18), x0 + col_w, body, false, true, L.dim, line_h);
        decos_.push_back({2, x0 + S(2), top + S(2), Sd(3), y - top - S(4)});
        y += std::round(body * 0.45);
        break;
      }
      case md::Block::Code: {
        double top = y;
        int cs = std::max(7, int(body * 0.9));
        double ch = std::round(cs * 1.5);
        y += S(10);
        while (i < lines.size() && lines[i].kind == md::Block::Code) {
          Run r;
          r.text = lines[i].text;
          r.x = x0 + S(14);
          r.y = y;
          r.font = FL_FREE_FONT;
          r.size = cs;
          r.color = L.text;
          runs_.push_back(r);
          y += ch;
          ++i;
        }
        y += S(10);
        decos_.push_back({1, x0, top, col_w, y - top});
        y += std::round(body * 0.5);
        continue;  // i already advanced
      }
      case md::Block::Rule:
        y += S(8);
        decos_.push_back({3, x0, y, col_w, 1});
        y += S(14);
        break;
    }
    ++i;
  }
  if (lines.empty()) {
    Run r;
    r.text = _("This document is empty.");
    r.x = x0;
    r.y = y;
    r.font = FL_HELVETICA_ITALIC;
    r.size = body;
    r.color = L.dim;
    runs_.push_back(r);
    y += line_h;
  }
  height_ = y + S(40);
  scroll_ = std::max(0, std::min(scroll_, int(height_) - h()));
}

void MarkdownView::draw() {
  if (!doc_) return;
  if (rev_ != doc_->revision || laid_w_ != w()) relayout();
  const Look &L = look();
  int oy = y() - scroll_;
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), L.editor);
    Color soft = L.dark ? Color::hex(0xFFFFFF, 0.05) : Color::hex(0x000000, 0.04);
    for (auto &d : decos_) {
      double dy = d.y + oy;
      if (dy > y() + h() || dy + d.h < y()) continue;
      switch (d.kind) {
        case 1:
          p.fill_rounded(d.x, dy, d.w, d.h, Sd(6), soft);
          p.stroke_rounded(d.x + 0.5, dy + 0.5, d.w - 1, d.h - 1, Sd(6), L.border.alpha(0.6), 1);
          break;
        case 2: p.fill_rounded(d.x, dy, d.w, d.h, d.w / 2, L.accent.alpha(0.6)); break;
        case 3:
        case 7: p.line(d.x, dy + 0.5, d.x + d.w, dy + 0.5, L.border, 1); break;
        case 4: p.circle(d.x + d.w / 2, dy + d.h / 2, d.w / 2, L.text.mix(L.dim, 0.3)); break;
        case 5: p.stroke_rounded(d.x + 0.5, dy + 0.5, d.w - 1, d.h - 1, Sd(3), L.dim, Sd(1.3)); break;
        case 6:
          p.fill_rounded(d.x, dy, d.w, d.h, Sd(3), L.accent);
          icon::draw(p, icon::Check, d.x + 1, dy + 1, d.w - 2, L.on_accent);
          break;
      }
    }
    for (size_t k = 0; k < runs_.size(); ++k) {
      const Run &r = runs_[k];
      if (!r.code || r.text.empty()) continue;
      double ry = r.y + oy;
      if (ry > y() + h() || ry + r.size * 2 < y()) continue;
      // Inline code chips (code block lines have no width set).
      if (r.w > 0) p.fill_rounded(r.x - S(3), ry + S(3), r.w + S(6), r.size * 1.45, Sd(4), soft.alpha(soft.a * 1.6));
    }
    // Read-only hint.
    cairo_restore(cr);
  }
  fl_push_clip(x(), y(), w(), h());
  for (size_t k = 0; k < runs_.size(); ++k) {
    const Run &r = runs_[k];
    int ry = int(r.y) + oy;
    if (ry > y() + h() || ry + r.size * 2 < y()) continue;
    fl_font(r.font, r.size);
    Color c = int(k) == hover_link_ ? r.color.mix(L.text, 0.25) : r.color;
    fl_color(c.fl());
    int base = ry + int(std::round(r.size * 1.5 * 0.5 + (fl_height() / 2.0 - fl_descent())));
    fl_draw(r.text.c_str(), int(r.x), base);
    if (r.underline || r.strike) {
      int ly = r.strike ? base - int(r.size * 0.32) : base + S(2);
      fl_line(int(r.x), ly, int(r.x + (r.w > 0 ? r.w : fl_width(r.text.c_str()))), ly);
    }
  }
  fl_pop_clip();
  // Scroll thumb and the hint pill.
  {
    Painter p(window()->w(), window()->h());
    if (height_ > h()) {
      double track = h() - S(8);
      double len = std::max<double>(S(30), track * h() / height_);
      double pos = (track - len) * scroll_ / std::max(1.0, height_ - h());
      p.fill_rounded(x() + w() - S(9), y() + S(4) + pos, S(5), len, Sd(2.5), L.dim.alpha(0.35));
    }
    text_font(L.theme.font_size - S(2));
    std::string hint = _("Formatted view · Ctrl+Shift+M to edit");
    double hw = text_width(hint) + S(20);
    p.fill_rounded(x() + w() - hw - S(18), y() + S(8), hw, S(24), S(12), L.bar.alpha(0.95));
    p.stroke_rounded(x() + w() - hw - S(18) + 0.5, y() + S(8) + 0.5, hw - 1, S(24) - 1, S(12), L.border, 1);
    p.flush();
    draw_text(hint, int(x() + w() - hw - S(18)), y() + S(8), int(hw), S(24), L.dim, 1);
  }
}

int MarkdownView::handle(int event) {
  switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS:
      return 1;
    case FL_MOUSEWHEEL: {
      if (Fl::event_state() & FL_CTRL) {
        app_->zoom(Fl::event_dy() < 0 ? 1 : -1);
        rev_ = ~0u;
        return 1;
      }
      int max = std::max(0, int(height_) - h());
      scroll_ = std::max(0, std::min(max, scroll_ + Fl::event_dy() * S(48)));
      redraw();
      return 1;
    }
    case FL_KEYBOARD: {
      int k = Fl::event_key(), max = std::max(0, int(height_) - h()), old = scroll_;
      if (k == FL_Down) scroll_ += S(40);
      else if (k == FL_Up) scroll_ -= S(40);
      else if (k == FL_Page_Down || k == ' ') scroll_ += h() - S(60);
      else if (k == FL_Page_Up) scroll_ -= h() - S(60);
      else if (k == FL_Home) scroll_ = 0;
      else if (k == FL_End) scroll_ = max;
      else return 0;
      scroll_ = std::max(0, std::min(max, scroll_));
      if (scroll_ != old) redraw();
      return 1;
    }
    case FL_ENTER:
    case FL_MOVE: {
      int mx = Fl::event_x(), my = Fl::event_y() + scroll_ - y();
      int found = -1;
      for (size_t k = 0; k < runs_.size(); ++k) {
        const Run &r = runs_[k];
        if (r.url.empty()) continue;
        if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.size * 1.6) found = int(k);
      }
      if (found != hover_link_) {
        hover_link_ = found;
        window()->cursor(found >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      if (hover_link_ >= 0) {
        hover_link_ = -1;
        window()->cursor(FL_CURSOR_DEFAULT);
        redraw();
      }
      return 1;
    case FL_PUSH:
      take_focus();
      if (Fl::event_button() == FL_LEFT_MOUSE && hover_link_ >= 0) {
        const std::string &url = runs_[size_t(hover_link_)].url;
        if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0 || url.compare(0, 7, "mailto:") == 0)
          open_uri(url);
        else
          app_->toast(format(_("Link: %s"), url.c_str()));
      } else if (Fl::event_clicks() > 0) {
        app_->toggle_preview();  // double-click: back to editing
      }
      return 1;
  }
  return Fl_Widget::handle(event);
}

}  // namespace niny
