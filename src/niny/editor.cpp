// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The editor view: Fl_Text_Editor with Niny's gutter, current line band,
// search match and bracket highlights, margin line and slim scrollbars,
// drawn as a cairo layer over FLTK's text.
#include <FL/Fl.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "niny.h"
#include "textops.h"
#include "util.h"

using namespace sde;

namespace niny {

EditorView::EditorView(App *app, int x, int y, int w, int h) : Fl_Text_Editor(x, y, w, h), app_(app) {
  box(FL_FLAT_BOX);
  cursor_style(SIMPLE_CURSOR);
  scrollbar_align(FL_ALIGN_BOTTOM | FL_ALIGN_RIGHT);
  maintain_absolute_top_line_number(1);
  memset(table_, 0, sizeof table_);
  // The view only edits through Niny's key handling and undo history.
  remove_key_binding('z', FL_CTRL);
  remove_key_binding('z', FL_CTRL | FL_SHIFT);
}

void EditorView::apply_look() {
  const Look &L = look();
  const Settings &s = app_->settings();
  static std::string regular, bold, italic;
  regular = " " + s.font;
  bold = "B" + s.font;
  italic = "I" + s.font;
  Fl::set_font(FL_FREE_FONT, regular.c_str());
  Fl::set_font(Fl_Font(FL_FREE_FONT + 1), bold.c_str());
  Fl::set_font(Fl_Font(FL_FREE_FONT + 2), italic.c_str());
  font_px_ = std::max(7, int(std::lround(s.font_size * 4.0 / 3.0 * ui_scale())) + s.zoom);
  int n = int(sizeof table_ / sizeof table_[0]);
  for (int i = 0; i < n; ++i) {
    int k = i < kStyleCount ? i : 0;
    table_[i].color = L.syntax[k].fl();
    table_[i].font = L.bold[k] ? Fl_Font(FL_FREE_FONT + 1) : L.italic[k] ? Fl_Font(FL_FREE_FONT + 2) : FL_FREE_FONT;
    table_[i].size = font_px_;
    table_[i].attr = 0;
  }
  textfont(FL_FREE_FONT);
  textsize(font_px_);
  textcolor(L.syntax[0].fl());
  color(L.editor.fl());
  selection_color(L.selection.fl());
  cursor_color(L.accent.fl());
  linenumber_font(FL_FREE_FONT);
  linenumber_size(std::max(7, font_px_ - S(1)));
  linenumber_bgcolor(L.gutter.fl());
  linenumber_fgcolor(L.gutter_text.fl());
  scrollbar_width(S(12));
  for (Fl_Scrollbar *sb : {mVScrollBar, mHScrollBar}) {
    sb->box(FL_FLAT_BOX);
    sb->slider(FL_FLAT_BOX);
    sb->color(L.editor.fl());
    sb->selection_color(L.border.fl());
    sb->labelcolor(L.dim.fl());
  }
  fl_font(FL_FREE_FONT, font_px_);
  char_w_ = std::max(1, int(std::lround(fl_width("0"))));
  if (doc_) highlight_data(doc_->style, table_, n, 'A', nullptr, nullptr);
  gutter_digits_ = 0;
  update_gutter();
  resize(x(), y(), w(), h());
  redraw();
}

void EditorView::apply_wrap(bool wrap) { wrap_mode(wrap ? WRAP_AT_BOUNDS : WRAP_NONE, 0); }

void EditorView::update_gutter() {
  if (!app_->settings().line_numbers) {
    if (linenumber_width() != S(8)) linenumber_width(S(8));
    gutter_digits_ = 0;
    return;
  }
  int lines = std::max(1, mNBufferLines + 1);
  int digits = 1;
  while (lines >= 10) {
    lines /= 10;
    ++digits;
  }
  digits = std::max(digits, 3);
  if (digits == gutter_digits_) return;
  gutter_digits_ = digits;
  fl_font(FL_FREE_FONT, std::max(7, font_px_ - S(1)));
  int dw = int(std::ceil(fl_width("0")));
  linenumber_width(digits * dw + S(30));
}

void EditorView::attach(Document *doc) {
  if (doc_ && doc_ != doc && doc_->buf == buffer()) {
    doc_->cursor = insert_position();
    doc_->top_line = mTopLineNum;
    doc_->horiz = mHorizOffset;
  }
  doc_ = doc;
  if (!doc) return;
  buffer(doc->buf);
  int n = int(sizeof table_ / sizeof table_[0]);
  highlight_data(doc->style, table_, n, 'A', nullptr, nullptr);
  gutter_digits_ = 0;
  update_gutter();
  insert_position(std::min(doc->cursor, doc->buf->length()));
  scroll(doc->top_line, doc->horiz);
  last_cursor_ = -1;
  br_rev_ = ~0u;
}

void EditorView::resize(int X, int Y, int W, int H) { Fl_Text_Editor::resize(X, Y, W, H); }

void EditorView::goto_pos(int pos, bool center, int sel_to) {
  if (!doc_) return;
  int len = buffer()->length();
  pos = std::max(0, std::min(pos, len));
  if (sel_to >= 0) {
    sel_to = std::max(0, std::min(sel_to, len));
    buffer()->select(pos, sel_to);
    insert_position(sel_to);
  } else {
    buffer()->unselect();
    insert_position(pos);
  }
  int px, py;
  bool visible = position_to_xy(pos, &px, &py) && py + mMaxsize <= text_area.y + text_area.h;
  if (center && !visible && !mContinuousWrap) {
    int line = doc_->line_of(pos);
    scroll(std::max(1, line - mNVisibleLines / 3), 0);
  }
  show_insert_position();
  notify_cursor();
  redraw();
}

void EditorView::notify_cursor() {
  if (insert_position() != last_cursor_) {
    last_cursor_ = insert_position();
  }
  app_->cursor_moved();
}

int EditorView::handle(int event) {
  if (!buffer()) return 0;
  switch (event) {
    case FL_KEYBOARD: {
      if (app_->modal()) return 0;
      if (app_->editor_key(Fl::event_key(), Fl::event_state(), Fl::event_text(), Fl::event_length())) {
        notify_cursor();
        return 1;
      }
      int r = Fl_Text_Editor::handle(event);
      notify_cursor();
      return r;
    }
    case FL_DND_ENTER:
      dnd_ = true;
      return Fl_Text_Editor::handle(event);
    case FL_DND_LEAVE:
      dnd_ = false;
      return Fl_Text_Editor::handle(event);
    case FL_PASTE: {
      const char *t = Fl::event_text();
      if (dnd_ && t) {
        dnd_ = false;
        std::vector<std::string> files;
        bool all = true;
        for (auto &line : split(t, '\n')) {
          std::string l = trim(line);
          if (l.empty()) continue;
          std::string p = uri_to_path(l);
          if (p.empty() || p[0] != '/' || !file_exists(p)) {
            all = false;
            break;
          }
          files.push_back(p);
        }
        if (all && !files.empty()) {
          app_->open_files(files, "");
          return 1;
        }
      }
      dnd_ = false;
      int r = Fl_Text_Editor::handle(event);
      notify_cursor();
      return r;
    }
    case FL_PUSH:
    case FL_DRAG:
    case FL_RELEASE:
    case FL_MOUSEWHEEL: {
      if (event == FL_MOUSEWHEEL && (Fl::event_state() & FL_CTRL)) {
        app_->zoom(Fl::event_dy() < 0 ? 1 : -1);
        return 1;
      }
      int r = Fl_Text_Editor::handle(event);
      if (event != FL_MOUSEWHEEL) notify_cursor();
      return r;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return Fl_Text_Editor::handle(event);
    default:
      return Fl_Text_Editor::handle(event);
  }
}

void EditorView::draw() {
  // Overlays are drawn over the text; always redraw everything below them.
  set_damage(FL_DAMAGE_ALL);
  Fl_Text_Editor::draw();
  if (doc_) draw_overlay();
}

void EditorView::draw_overlay() {
  const Look &L = look();
  const Settings &s = app_->settings();
  Fl_Text_Buffer *b = buffer();
  int cursor = insert_position();
  Painter p(window()->w(), window()->h());
  cairo_t *cr = p.cr();
  cairo_save(cr);
  cairo_rectangle(cr, text_area.x, text_area.y, text_area.w, text_area.h);
  cairo_clip(cr);
  int lh = mMaxsize;
  int cx = 0, cy = 0;
  bool cursor_visible = position_to_xy(cursor, &cx, &cy) != 0;
  int sa = 0, se = 0;
  bool has_sel = b->selection_position(&sa, &se) && se > sa;
  // Current line.
  if (s.highlight_line && cursor_visible && !has_sel) {
    p.fill_rect(text_area.x, cy, text_area.w, lh, L.current_line);
  }
  // Margin line.
  if (s.right_margin > 0) {
    double mx = text_area.x - mHorizOffset + s.right_margin * char_w_ + 0.5;
    if (mx > text_area.x && mx < text_area.x + text_area.w)
      p.line(mx, text_area.y, mx, text_area.y + text_area.h, L.margin, 1);
  }
  // Search matches in view.
  if (app_->find_visible()) {
    const auto &ms = app_->matches();
    auto it = std::lower_bound(ms.begin(), ms.end(), size_t(std::max(0, mFirstChar - 4096)),
                               [](const Match &m, size_t v) { return m.end < v; });
    for (; it != ms.end() && int(it->start) <= mLastChar; ++it) {
      int x1, y1, x2, y2;
      if (!position_to_xy(int(it->start), &x1, &y1)) continue;
      if (!position_to_xy(int(it->end), &x2, &y2) || y2 != y1) x2 = x1 + char_w_;
      bool current = has_sel && int(it->start) == sa && int(it->end) == se;
      double w = std::max(x2 - x1, 2);
      if (current) {
        p.stroke_rounded(x1 - 1, y1, w + 2, lh, Sd(3), L.match_current, Sd(2));
      } else {
        p.fill_rounded(x1 - 1, y1, w + 2, lh, Sd(3), L.match);
      }
    }
  }
  // Matching brackets.
  if (s.bracket_match) {
    if (br_rev_ != doc_->revision || br_cursor_ != cursor) {
      br_rev_ = doc_->revision;
      br_cursor_ = cursor;
      br_a_ = br_b_ = -1;
      int len = b->length();
      for (int cand : {cursor - 1, cursor}) {
        if (cand < 0 || cand >= len || !is_bracket(char(b->byte_at(cand)))) continue;
        const int window = 256 * 1024;
        int from = std::max(0, cand - window), to = std::min(len, cand + window + 1);
        char *t = b->text_range(from, to);
        char *st = doc_->style->text_range(from, to);
        std::string text(t, size_t(to - from)), styles(st, size_t(to - from));
        free(t);
        free(st);
        long m = match_bracket(text, styles, cand - from, window);
        if (m >= 0) {
          br_a_ = cand;
          br_b_ = from + m;
          break;
        }
      }
    }
    if (br_a_ >= 0) {
      for (long pos : {br_a_, br_b_}) {
        int bx, by, ex, ey;
        if (!position_to_xy(int(pos), &bx, &by)) continue;
        if (!position_to_xy(int(pos) + 1, &ex, &ey) || ey != by) ex = bx + char_w_;
        p.fill_rounded(bx - 1, by + 1, ex - bx + 2, lh - 2, Sd(3), L.bracket.alpha(L.dark ? 0.28 : 0.18));
        p.stroke_rounded(bx - 1, by + 1, ex - bx + 2, lh - 2, Sd(3), L.bracket.alpha(0.75), 1);
      }
    }
  }
  // Caret: a 2 px accent bar.
  if (cursor_visible && Fl::focus() == this && !app_->modal()) {
    p.fill_rounded(cx - Sd(1), cy + 1, std::max(2.0, Sd(2)), lh - 2, Sd(1), L.accent);
  }
  cairo_restore(cr);
  draw_gutter(p);
  draw_scrollbars(p);
}

void EditorView::draw_gutter(Painter &p) {
  const Look &L = look();
  int gx = x() + Fl::box_dx(box()), gw = mLineNumWidth;
  if (gw <= 0) return;
  int gy = text_area.y, gh = text_area.h;
  p.fill_rect(gx, y(), gw, h() - (mHScrollBar->visible() ? mHScrollBar->h() : 0), L.gutter);
  p.line(gx + gw - S(8) - 0.5, gy, gx + gw - S(8) - 0.5, gy + gh, L.border.alpha(L.dark ? 0.35 : 0.5), 1);
  if (!app_->settings().line_numbers) return;
  p.flush();
  int lh = mMaxsize;
  int cursor_line_start = buffer()->line_start(insert_position());
  int line = get_absolute_top_line_number();
  fl_push_clip(gx, gy, gw, gh);
  int fs = std::max(7, font_px_ - S(1));
  char num[24];
  int Y = gy;
  for (int vis = 0; vis < mNVisibleLines; ++vis, Y += lh) {
    int ls = mLineStarts[vis];
    if (ls == -1) break;
    bool starts = ls == 0 || buffer()->char_at(ls - 1) == '\n';
    if (!starts) {
      if (vis == 0) ++line;
      continue;
    }
    bool current = ls == cursor_line_start;
    snprintf(num, sizeof num, "%d", line);
    fl_font(current ? Fl_Font(FL_FREE_FONT + 1) : FL_FREE_FONT, fs);
    fl_color((current ? L.gutter_current : L.gutter_text).fl());
    fl_draw(num, gx, Y, gw - S(16), lh, FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    ++line;
  }
  fl_pop_clip();
}

void EditorView::draw_scrollbars(Painter &p) {
  const Look &L = look();
  auto thumb = [&](Fl_Scrollbar *sb, bool vertical) {
    if (!sb->visible()) return;
    double X = sb->x(), Y = sb->y(), W = sb->w(), H = sb->h();
    p.fill_rect(X, Y, W, H, L.editor);
    double track = vertical ? H : W, thick = vertical ? W : H;
    double off = 0;
    if (track >= 3 * thick) {  // FLTK reserves room for arrow buttons
      off = thick;
      track -= 2 * thick;
    }
    double range = sb->maximum() - sb->minimum();
    double val = range == 0 ? 0 : (sb->value() - sb->minimum()) / range;
    val = std::max(0.0, std::min(1.0, val));
    double len = std::max(sb->slider_size() * track, thick * 1.5);
    double pos = off + val * (track - len);
    bool hot = Fl::belowmouse() == sb || Fl::pushed() == sb;
    Color c = L.dim.alpha(hot ? 0.55 : 0.32);
    double pad = Sd(3);
    if (vertical) {
      // Match ticks along the track.
      const auto &ml = app_->match_lines();
      if (app_->find_visible() && !ml.empty()) {
        double total = std::max(1, mNBufferLines + 1);
        for (int l : ml) {
          double ty = Y + off + (l - 1) / total * track;
          p.fill_rect(X + pad, ty, W - 2 * pad, std::max(2.0, Sd(2)), L.match.alpha(0.9));
        }
      }
      p.fill_rounded(X + pad, Y + pos + pad, W - 2 * pad, len - 2 * pad, (W - 2 * pad) / 2, c);
    } else {
      p.fill_rounded(X + pos + pad, Y + pad, len - 2 * pad, H - 2 * pad, (H - 2 * pad) / 2, c);
    }
  };
  thumb(mVScrollBar, true);
  thumb(mHScrollBar, false);
  if (mVScrollBar->visible() && mHScrollBar->visible())
    p.fill_rect(mVScrollBar->x(), mHScrollBar->y(), mVScrollBar->w(), mHScrollBar->h(), L.editor);
}

}  // namespace niny
