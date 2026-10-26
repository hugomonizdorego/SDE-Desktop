// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Direct editing of page content; see edit.h.
//
// Line breaking keeps what the user did not touch: a line of the
// original paragraph that still holds exactly its characters, at its
// place, stays in the content stream as it was (not even re-encoded).
// Only the lines from the first change on are set again, greedily inside
// the box, until the text meets an original line break once more.
#include "edit.h"

#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <map>

#include "icons.h"
#include "util.h"

using namespace sde;

namespace xanara {

bool TextStyle::operator==(const TextStyle &o) const {
  return font == o.font && family == o.family && std::fabs(size - o.size) < 1e-6 && bold == o.bold &&
         italic == o.italic && paint == o.paint && std::fabs(hscale - o.hscale) < 1e-6 &&
         std::fabs(rise - o.rise) < 1e-6 && std::fabs(char_space - o.char_space) < 1e-6;
}

namespace {

enum Kind { kInsert = 1, kErase, kStyle, kAlign, kMove };

bool is_break(char32_t c) { return c == '\n' || c == 0x200B; }
bool is_space(char32_t c) { return c == ' ' || c == 0xA0 || c == '\t'; }
bool is_word(char32_t c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c > 0x7F;
}

std::string code_bytes(unsigned code, int n) {
  std::string b;
  for (int k = n - 1; k >= 0; --k) b += char((code >> (8 * k)) & 0xFF);
  return b;
}

}  // namespace

// ------------------------------------------------------------- TextEdit

TextEdit::TextEdit() = default;
TextEdit::~TextEdit() = default;

int TextEdit::intern(const TextStyle &s) {
  for (size_t k = 0; k < styles_.size(); ++k)
    if (styles_[k] == s) return int(k);
  styles_.push_back(s);
  return int(styles_.size()) - 1;
}

bool TextEdit::start(std::shared_ptr<PageLayout> layout, int block) {
  if (!layout || block < 0 || block >= int(layout->blocks.size())) return false;
  const PageLayout::Block &B = layout->blocks[size_t(block)];
  if (!B.editable) return false;
  layout_ = layout;
  block_ = block;
  chars_.clear();
  styles_.clear();
  undo_.clear();
  redo_.clear();
  rows_.clear();
  orig_baselines_.clear();
  orig_lengths_.clear();
  modified_ = moved_ = false;
  last_kind_ = -1;
  typing_set_ = false;
  align_ = B.align;
  const auto &G = layout->glyphs;
  double left = 1e18, right = -1e18, size_sum = 0;
  int size_n = 0;
  for (int li : B.lines) {
    const auto &l = layout->lines[size_t(li)];
    left = std::min(left, G[size_t(l.glyphs.front())].origin.x);
    right = std::max(right, G[size_t(l.glyphs.back())].end.x);
    for (int gi : l.glyphs) {
      size_sum += G[size_t(gi)].size();
      ++size_n;
    }
  }
  double base = size_n ? size_sum / size_n : 12;
  x0_ = left;
  width_ = std::max(right - left, base);
  y0_ = G[size_t(layout->lines[size_t(B.lines[0])].glyphs.front())].origin.y;
  leading_ = B.lines.size() > 1 ? B.leading / base : 1.2;
  indent_ = G[size_t(layout->lines[size_t(B.lines[0])].glyphs.front())].origin.x - left;
  if (indent_ < 0.5) indent_ = 0;
  auto_width_ = B.lines.size() == 1;
  if (auto_width_) {
    // A single line grows to the right, as far as the page goes.
    double page_w = layout->geometry.width();
    width_ = std::max(width_, page_w - left - std::min(36.0, page_w * 0.05));
  }
  for (size_t L = 0; L < B.lines.size(); ++L) {
    const auto &l = layout->lines[size_t(B.lines[L])];
    orig_baselines_.push_back(G[size_t(l.glyphs.front())].origin.y);
    int pos = 0;
    int last_style = 0;
    for (size_t j = 0; j < l.glyphs.size(); ++j) {
      const Glyph &g = G[size_t(l.glyphs[j])];
      TextStyle s;
      s.font = g.font;
      s.size = g.size();
      double k = g.font_size != 0 ? s.size / std::fabs(g.font_size) : 1;
      s.paint = g.paint;
      s.hscale = g.hscale;
      s.rise = g.rise * k;
      s.char_space = g.char_space * k;
      if (g.font >= 0) {
        s.bold = layout->fonts[size_t(g.font)]->bold;
        s.italic = layout->fonts[size_t(g.font)]->italic;
      }
      Char c;
      std::u32string t = utf32(g.text);
      c.cp = t.empty() ? 0xFFFD : t[0];
      c.style = last_style = intern(s);
      c.glyph = l.glyphs[j];
      c.line = int(L);
      c.pos = pos++;
      chars_.push_back(c);
      if (l.space_after[j]) {
        Char sp;
        sp.cp = ' ';
        sp.style = c.style;
        sp.line = int(L);
        sp.pos = pos++;
        chars_.push_back(sp);
      }
    }
    if (L + 1 < B.lines.size()) {
      // The break to the next line: a space, nothing after a space or a
      // hyphen, or the end of a paragraph after a clearly short line.
      Char br;
      br.soft = true;
      br.style = last_style;
      br.line = int(L);
      br.pos = pos++;
      char32_t prev = chars_.back().cp;
      bool short_line = (B.align == 0 || B.align == 3) && G[size_t(l.glyphs.back())].end.x < right - std::max(3 * base, 0.15 * (right - left));
      if (B.align == 1 || B.align == 2 || short_line) br.cp = '\n';
      else br.cp = is_space(prev) || prev == '-' || prev == 0x2010 || prev == 0xAD ? char32_t(0x200B) : char32_t(' ');
      chars_.push_back(br);
    }
    orig_lengths_.push_back(pos);
  }
  orig_align_ = align_;
  orig_width_ = width_;
  for (auto &c : chars_) c.orig_style = c.style;
  caret_ = anchor_ = 0;
  relayout();
  return true;
}

void TextEdit::start_new(std::shared_ptr<PageLayout> layout, Point at, double width, const TextStyle &style) {
  layout_ = layout;
  block_ = -1;
  chars_.clear();
  styles_.clear();
  undo_.clear();
  redo_.clear();
  orig_baselines_.clear();
  orig_lengths_.clear();
  modified_ = moved_ = false;
  last_kind_ = -1;
  align_ = orig_align_ = 0;
  typing_ = style;
  typing_set_ = true;
  intern(style);
  leading_ = 1.2;
  indent_ = 0;
  double asc = 0.8;
  FontRequest r;
  r.family = style.family;
  r.bold = style.bold;
  r.italic = style.italic;
  if (auto f = SystemFont::find(r)) asc = std::clamp(f->ascent, 0.6, 1.1);
  x0_ = at.x;
  y0_ = at.y + style.size * asc;
  double page_w = layout ? layout->geometry.width() : 612;
  auto_width_ = width <= 0;
  width_ = width > 0 ? width : std::max(style.size * 2, page_w - at.x - std::min(36.0, page_w * 0.05));
  orig_width_ = width_;
  caret_ = anchor_ = 0;
  relayout();
}

std::string TextEdit::text() const {
  std::string out;
  for (const auto &c : chars_) {
    if (c.cp == 0x200B) continue;
    if (c.glyph >= 0 && layout_ && !layout_->glyphs[size_t(c.glyph)].text.empty())
      out += layout_->glyphs[size_t(c.glyph)].text;
    else
      out += utf8(c.cp);
  }
  return out;
}

// ------------------------------------------------------------- layout

void TextEdit::advances(std::vector<double> &adv, std::vector<Laid> &laid) const {
  size_t n = chars_.size();
  adv.assign(n, 0);
  laid.assign(n, Laid());
  for (size_t i = 0; i < n; ++i) {
    laid[i].ch = i;
    laid[i].nch = 0;  // no glyph until resolved
  }
  // Which characters need an installed font: those the document's font
  // has no glyph for, and then their whole word, so that no word is set
  // in two fonts. Spaces go with the word before them.
  std::vector<char> sys(n, 0);
  for (size_t k = 0; k < n; ++k) {
    const Char &c = chars_[k];
    const TextStyle &st = styles_[size_t(c.style)];
    if (is_break(c.cp)) continue;
    bool doc = false;
    if (st.font >= 0 && st.family.empty() && layout_) {
      std::string b;
      doc = (c.glyph >= 0 && layout_->glyphs[size_t(c.glyph)].font == st.font) ||
            (c.cp != 0xFFFD && layout_->fonts[size_t(st.font)]->encode(c.cp, b));
    }
    sys[k] = !doc;
  }
  for (size_t a = 0; a < n;) {
    if (is_break(chars_[a].cp) || is_space(chars_[a].cp)) {
      ++a;
      continue;
    }
    size_t b = a;
    bool any = false;
    for (; b < n && !is_break(chars_[b].cp) && !is_space(chars_[b].cp) && chars_[b].style == chars_[a].style; ++b)
      if (sys[b]) any = true;
    if (any)
      for (size_t k = a; k < b; ++k)
        if (chars_[k].cp != 0xFFFD) sys[k] = 1;  // a glyph without known text stays as it is
    a = b;
  }
  for (size_t k = 1; k < n; ++k)
    if (is_space(chars_[k].cp) && sys[k - 1] && chars_[k].style == chars_[k - 1].style) sys[k] = 1;
  size_t i = 0;
  while (i < n) {
    const Char &c = chars_[i];
    const TextStyle &st = styles_[size_t(c.style)];
    if (is_break(c.cp)) {
      ++i;
      continue;
    }
    // An original glyph in its document font: its codes as they were.
    if (!sys[i] && c.glyph >= 0 && layout_ && st.font >= 0 && layout_->glyphs[size_t(c.glyph)].font == st.font) {
      const Glyph &g = layout_->glyphs[size_t(c.glyph)];
      Laid &L = laid[i];
      L.nch = 1;
      L.doc_font = g.font;
      L.bytes = code_bytes(g.code, g.bytes);
      L.width = g.width;
      L.size = st.size;
      L.style = c.style;
      L.orig = c.glyph;
      L.text = g.text;
      adv[i] = (g.width * st.size + st.char_space) * st.hscale;
      ++i;
      continue;
    }
    // A new character in the document's font.
    if (!sys[i] && st.font >= 0 && st.family.empty() && layout_) {
      const PdfFont &f = *layout_->fonts[size_t(st.font)];
      std::string b;
      if (c.cp != 0xFFFD && f.encode(c.cp, b)) {
        int nb = 0;
        unsigned code = f.next(b, 0, nb);
        Laid &L = laid[i];
        L.nch = 1;
        L.doc_font = st.font;
        L.bytes = b;
        L.width = f.width(code);
        L.size = st.size;
        L.style = c.style;
        L.text = utf8(c.cp);
        adv[i] = (L.width * st.size + st.char_space) * st.hscale;
      }
      ++i;
      continue;
    }
    // An installed font: shape the run of characters that use it.
    FontRequest req;
    if (st.font >= 0 && st.family.empty() && layout_) {
      req = request_for(*layout_->fonts[size_t(st.font)]);
    } else {
      req.family = st.family;
      req.bold = st.bold;
      req.italic = st.italic;
    }
    std::shared_ptr<SystemFont> f = SystemFont::find(req);
    char32_t cp = c.cp == 0xFFFD ? char32_t('?') : c.cp;
    if (!f || !f->covers(cp)) {
      std::shared_ptr<SystemFont> fb = SystemFont::fallback(cp, req);
      if (fb) f = fb;
    }
    if (!f) {
      ++i;
      continue;
    }
    size_t j = i + 1;
    std::u32string run(1, cp);
    while (j < n) {
      const Char &d = chars_[j];
      if (!sys[j] || d.style != c.style || is_break(d.cp) || run.size() > 4000) break;
      if (!f->covers(d.cp) && !is_space(d.cp)) break;
      run += d.cp;
      ++j;
    }
    std::vector<SystemFont::Shaped> shaped = f->shape(run);
    // The glyphs of each cluster, which belong to its first character; the
    // characters of a cluster (a ligature, a base with marks) share its
    // advance, so that the caret can stop between them.
    std::map<size_t, std::vector<const SystemFont::Shaped *>> clusters;
    for (const auto &s : shaped) clusters[std::min(s.cluster, run.size() - 1)].push_back(&s);
    for (auto it = clusters.begin(); it != clusters.end(); ++it) {
      auto nx = std::next(it);
      size_t from = it->first, to = nx == clusters.end() ? run.size() : nx->first;
      double total = 0;
      for (const auto *s : it->second) total += s->advance;
      for (size_t m = from; m < to; ++m)
        adv[i + m] = (total / double(to - from) * st.size + st.char_space) * st.hscale;
      // Laid.x / .y hold the glyph's offset from the cluster's start (em).
      double pen = 0;
      bool first = true;
      Laid &head = laid[i + from];
      for (const auto *s : it->second) {
        Laid L;
        L.ch = i + from;
        L.nch = first ? to - from : 0;
        L.sys = f;
        L.gid = s->gid;
        L.width = f->width(s->gid);
        L.size = st.size;
        L.style = c.style;
        L.x = pen + s->dx;
        L.y = s->dy;
        L.text = first ? utf8(run.substr(from, to - from)) : "";
        if (first) head = L;
        else head.extra.push_back(L);
        pen += s->advance;
        first = false;
      }
    }
    i = j;
  }
}

void TextEdit::relayout() {
  rows_.clear();
  std::vector<double> adv;
  std::vector<Laid> laid;
  advances(adv, laid);
  size_t n = chars_.size();
  double prev_base = y0_;
  bool par_start = true;
  size_t i = 0;
  int row = 0;
  auto row_size = [&](size_t a, size_t b) {
    double s = 0;
    for (size_t k = a; k < b; ++k) s = std::max(s, styles_[size_t(chars_[k].style)].size);
    if (s == 0) s = typing_set_ ? typing_.size : (chars_.empty() ? 12 : styles_[size_t(chars_[std::min(a, n - 1)].style)].size);
    return s;
  };
  for (;;) {
    Row r;
    r.begin = i;
    // An original line, untouched and in its place?
    bool kept = false;
    if (!moved_ && align_ == orig_align_ && i < n && chars_[i].line == row && chars_[i].pos == 0 &&
        size_t(row) < orig_lengths_.size()) {
      size_t len = size_t(orig_lengths_[size_t(row)]);
      kept = i + len <= n;
      for (size_t k = 0; kept && k < len; ++k) {
        const Char &c = chars_[i + k];
        kept = c.line == row && c.pos == int(k) && c.style == c.orig_style;
      }
      if (kept && width_ + 0.5 < orig_width_) {
        // The box got narrower: only lines that still fit stay.
        const Glyph &last = layout_->glyphs[size_t(layout_->lines[size_t(layout_->blocks[size_t(block_)].lines[size_t(row)])].glyphs.back())];
        kept = last.end.x <= x0_ + width_ + 0.5;
      }
      // Text typed after the last original line continues that line.
      if (kept && size_t(row) + 1 == orig_lengths_.size() && i + len < n && chars_[i + len].cp != '\n') kept = false;
      if (kept) r.end = i + len;
    }
    if (!kept) {
      double avail = width_ - (par_start ? indent_ : 0), x = 0;
      size_t k = i, last_break = std::string::npos;
      while (k < n) {
        char32_t cp = chars_[k].cp;
        if (cp == '\n') {
          ++k;
          break;
        }
        if (x + adv[k] > avail + 0.01 && k > i && !is_space(cp) && cp != 0x200B) {
          if (last_break != std::string::npos) k = last_break + 1;
          break;
        }
        if (is_space(cp) || cp == 0x200B) last_break = k;
        x += adv[k];
        ++k;
      }
      r.end = k;
    }
    // The baseline: after the previous line, by the leading of the box;
    // where an original line was, exactly there.
    double size = row_size(r.begin, r.end);
    double base = row == 0 ? y0_ : prev_base + leading_ * size;
    if (row > 0 && size_t(row) < orig_baselines_.size() && !moved_) {
      double orig = orig_baselines_[size_t(row)];
      if (std::fabs(orig - base) < 0.25 * size) base = orig;
    }
    if (kept && std::fabs(base - orig_baselines_[size_t(row)]) > 0.01) kept = false;
    r.kept = kept;
    r.orig_line = kept ? row : -1;
    r.baseline = base;
    // Positions.
    r.xs.assign(r.end - r.begin + 1, 0);
    if (kept) {
      const auto &G = layout_->glyphs;
      double pen = 0;
      for (size_t k = r.begin; k < r.end; ++k) {
        const Char &c = chars_[k];
        if (c.glyph >= 0) {
          const Glyph &g = G[size_t(c.glyph)];
          r.xs[k - r.begin] = g.origin.x;
          pen = g.end.x;
          Laid L = laid[k];
          L.x = g.origin.x;
          L.y = g.origin.y;
          r.glyphs.push_back(L);
        } else {
          r.xs[k - r.begin] = pen;
          if (is_space(c.cp) && k + 1 < r.end && chars_[k + 1].glyph >= 0) pen = G[size_t(chars_[k + 1].glyph)].origin.x;
        }
      }
      r.xs[r.end - r.begin] = pen;
      r.x1 = r.xs[0];
      r.x2 = pen;
    } else {
      size_t vis_end = r.end;  // without the trailing spaces and break
      while (vis_end > r.begin && (is_space(chars_[vis_end - 1].cp) || is_break(chars_[vis_end - 1].cp))) --vis_end;
      double lw = 0;
      int spaces = 0;
      for (size_t k = r.begin; k < vis_end; ++k) {
        lw += adv[k];
        if (is_space(chars_[k].cp) || chars_[k].cp == 0x200B) ++spaces;
      }
      double x = x0_ + (par_start ? indent_ : 0), extra = 0;
      bool last_of_par = r.end >= n || chars_[r.end - 1].cp == '\n';
      double room = width_ - (par_start ? indent_ : 0) - lw;
      if (align_ == 1) x += auto_width_ ? 0 : room / 2;
      else if (align_ == 2) x += auto_width_ ? 0 : room;
      else if (align_ == 3 && !last_of_par && spaces > 0 && room > 0 && room < 0.5 * width_) extra = room / spaces;
      for (size_t k = r.begin; k < r.end; ++k) {
        r.xs[k - r.begin] = x;
        const Laid &L = laid[k];
        if (L.nch > 0 && k < vis_end) {
          const TextStyle &st = styles_[size_t(chars_[k].style)];
          auto place = [&](const Laid &src) {
            Laid out = src;
            out.extra.clear();
            if (out.sys) {  // offsets in em, see advances()
              out.x = x + src.x * st.size * st.hscale;
              out.y = base - st.rise - src.y * st.size;
            } else {
              out.x = x;
              out.y = base - st.rise;
            }
            r.glyphs.push_back(out);
          };
          place(L);
          for (const Laid &e : L.extra) place(e);
        }
        x += adv[k];
        if (k < vis_end && (is_space(chars_[k].cp) || chars_[k].cp == 0x200B)) x += extra;
      }
      r.xs[r.end - r.begin] = x;
      r.x1 = r.xs[0];
      r.x2 = r.xs[vis_end - r.begin];
    }
    double asc = 0.8, desc = 0.22;
    r.top = base - size * asc;
    r.bottom = base + size * desc;
    rows_.push_back(std::move(r));
    prev_base = base;
    par_start = rows_.back().end > rows_.back().begin && chars_[rows_.back().end - 1].cp == '\n';
    ++row;
    if (rows_.back().end >= n) {
      // After a final paragraph break, an empty line for the caret.
      if (n > 0 && chars_[n - 1].cp == '\n') {
        Row e;
        e.begin = e.end = n;
        e.baseline = base + leading_ * size;
        e.xs.assign(1, x0_);
        e.x1 = e.x2 = x0_;
        e.top = e.baseline - size * asc;
        e.bottom = e.baseline + size * desc;
        rows_.push_back(e);
      }
      break;
    }
    if (rows_.back().end == rows_.back().begin) break;  // safety
    i = rows_.back().end;
  }
}

// --------------------------------------------------------------- caret

size_t TextEdit::row_of(size_t i) const {
  for (size_t r = 0; r < rows_.size(); ++r) {
    const Row &R = rows_[r];
    bool last = r + 1 == rows_.size();
    if (i >= R.begin && (i < R.end || (last && i <= R.end))) return r;
    if (i == R.end && r + 1 < rows_.size() && rows_[r + 1].begin > i) return r;
  }
  return rows_.empty() ? 0 : rows_.size() - 1;
}

TextEdit::Caret TextEdit::caret_rect() const {
  Caret c;
  if (rows_.empty()) return c;
  const Row &r = rows_[row_of(caret_)];
  size_t k = std::min(caret_, r.end) - r.begin;
  c.x = k < r.xs.size() ? r.xs[k] : r.x2;
  c.top = r.top;
  c.bottom = r.bottom;
  return c;
}

size_t TextEdit::hit(double x, double y) const {
  if (rows_.empty()) return 0;
  size_t best = 0;
  double bd = 1e18;
  for (size_t r = 0; r < rows_.size(); ++r) {
    const Row &R = rows_[r];
    double d = y < R.top ? R.top - y : y > R.bottom ? y - R.bottom : 0;
    if (d < bd) {
      bd = d;
      best = r;
    }
  }
  const Row &R = rows_[best];
  bool last = best + 1 == rows_.size();
  size_t hi = R.end;
  if (!last && hi > R.begin) --hi;  // the break belongs before the next line
  size_t pick = R.begin;
  double pd = 1e18;
  for (size_t k = R.begin; k <= hi; ++k) {
    double d = std::fabs(R.xs[k - R.begin] - x);
    if (d < pd) {
      pd = d;
      pick = k;
    }
  }
  return pick;
}

void TextEdit::set_caret(size_t i, bool extend) {
  caret_ = std::min(i, chars_.size());
  if (!extend) anchor_ = caret_;
  last_kind_ = -1;
}

bool TextEdit::word_char(size_t i) const { return i < chars_.size() && is_word(chars_[i].cp); }

void TextEdit::move_left(bool extend, bool word) {
  if (!extend && has_selection()) {
    set_caret(std::min(caret_, anchor_), false);
    return;
  }
  size_t c = caret_;
  if (c > 0) --c;
  if (word) {
    while (c > 0 && !word_char(c)) --c;
    while (c > 0 && word_char(c - 1)) --c;
  }
  while (c > 0 && chars_[c].cp == 0x200B) --c;
  set_caret(c, extend);
}

void TextEdit::move_right(bool extend, bool word) {
  if (!extend && has_selection()) {
    set_caret(std::max(caret_, anchor_), false);
    return;
  }
  size_t c = std::min(caret_ + 1, chars_.size());
  if (word) {
    while (c < chars_.size() && word_char(c)) ++c;
    while (c < chars_.size() && !word_char(c) && chars_[c].cp != '\n') ++c;
  }
  set_caret(c, extend);
}

void TextEdit::move_up(bool extend) {
  if (rows_.empty()) return;
  size_t r = row_of(caret_);
  if (r == 0) {
    set_caret(0, extend);
    return;
  }
  Caret c = caret_rect();
  const Row &up = rows_[r - 1];
  set_caret(hit(c.x, (up.top + up.bottom) / 2), extend);
}

void TextEdit::move_down(bool extend) {
  if (rows_.empty()) return;
  size_t r = row_of(caret_);
  if (r + 1 >= rows_.size()) {
    set_caret(chars_.size(), extend);
    return;
  }
  Caret c = caret_rect();
  const Row &dn = rows_[r + 1];
  set_caret(hit(c.x, (dn.top + dn.bottom) / 2), extend);
}

void TextEdit::move_home(bool extend, bool box) {
  if (box || rows_.empty()) {
    set_caret(0, extend);
    return;
  }
  set_caret(rows_[row_of(caret_)].begin, extend);
}

void TextEdit::move_end(bool extend, bool box) {
  if (box || rows_.empty()) {
    set_caret(chars_.size(), extend);
    return;
  }
  size_t r = row_of(caret_);
  const Row &R = rows_[r];
  size_t e = R.end;
  if (r + 1 < rows_.size() && e > R.begin) --e;
  set_caret(e, extend);
}

void TextEdit::select_word(size_t at) {
  at = std::min(at, chars_.size());
  size_t a = at, b = at;
  if (!word_char(a) && a > 0 && word_char(a - 1)) --a;
  if (word_char(a)) {
    while (a > 0 && word_char(a - 1)) --a;
    b = a;
    while (b < chars_.size() && word_char(b)) ++b;
  } else if (a < chars_.size()) {
    b = a + 1;
  }
  anchor_ = a;
  caret_ = b;
  last_kind_ = -1;
}

void TextEdit::select_paragraph(size_t at) {
  at = std::min(at, chars_.size());
  size_t a = at, b = at;
  while (a > 0 && chars_[a - 1].cp != '\n') --a;
  while (b < chars_.size() && chars_[b].cp != '\n') ++b;
  anchor_ = a;
  caret_ = b;
  last_kind_ = -1;
}

void TextEdit::select_all() {
  anchor_ = 0;
  caret_ = chars_.size();
  last_kind_ = -1;
}

std::string TextEdit::selected_text() const {
  size_t a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
  std::string out;
  for (size_t k = a; k < b && k < chars_.size(); ++k) {
    const Char &c = chars_[k];
    if (c.cp == 0x200B) continue;
    if (c.glyph >= 0 && !layout_->glyphs[size_t(c.glyph)].text.empty()) out += layout_->glyphs[size_t(c.glyph)].text;
    else out += utf8(c.cp == 0xFFFD ? char32_t('?') : c.cp);
  }
  return out;
}

std::vector<Box> TextEdit::selection_boxes() const {
  std::vector<Box> out;
  size_t a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
  if (a == b) return out;
  for (const Row &R : rows_) {
    size_t s = std::max(a, R.begin), e = std::min(b, R.end);
    if (s >= e && !(R.begin == R.end && a <= R.begin && b > R.begin)) continue;
    double x1 = R.xs[s - R.begin], x2 = R.xs[std::min(e, R.end) - R.begin];
    if (x2 - x1 < 1 && e > s) x2 = x1 + 3;  // a selected line break
    out.push_back(Box{x1, R.top, x2, R.bottom});
  }
  return out;
}

// -------------------------------------------------------------- changes

TextEdit::Snapshot TextEdit::snapshot() const {
  Snapshot s;
  s.chars = chars_;
  s.styles = styles_;
  s.caret = caret_;
  s.anchor = anchor_;
  s.align = align_;
  s.x0 = x0_;
  s.y0 = y0_;
  s.width = width_;
  s.moved = moved_;
  s.modified = modified_;
  s.auto_width = auto_width_;
  return s;
}

void TextEdit::restore(const Snapshot &s) {
  chars_ = s.chars;
  styles_ = s.styles;
  caret_ = std::min(s.caret, chars_.size());
  anchor_ = std::min(s.anchor, chars_.size());
  align_ = s.align;
  x0_ = s.x0;
  y0_ = s.y0;
  width_ = s.width;
  moved_ = s.moved;
  modified_ = s.modified;
  auto_width_ = s.auto_width;
  last_kind_ = -1;
  relayout();
}

void TextEdit::push_undo(int kind) {
  if (kind == kInsert && last_kind_ == kInsert) return;  // typing goes into one step
  if (kind == kErase && last_kind_ == kErase) return;
  undo_.push_back(snapshot());
  if (undo_.size() > 300) undo_.erase(undo_.begin());
  redo_.clear();
  last_kind_ = kind;
}

bool TextEdit::undo() {
  if (undo_.empty()) return false;
  redo_.push_back(snapshot());
  Snapshot s = undo_.back();
  undo_.pop_back();
  restore(s);
  return true;
}

bool TextEdit::redo() {
  if (redo_.empty()) return false;
  undo_.push_back(snapshot());
  Snapshot s = redo_.back();
  redo_.pop_back();
  restore(s);
  return true;
}

TextStyle TextEdit::style_at_caret() const {
  if (typing_set_) return typing_;
  if (chars_.empty()) return styles_.empty() ? TextStyle() : styles_[0];
  size_t k = caret_ > 0 ? caret_ - 1 : 0;
  while (k > 0 && is_break(chars_[k].cp)) --k;
  return styles_[size_t(chars_[std::min(k, chars_.size() - 1)].style)];
}

void TextEdit::insert(const std::string &utf8_text) {
  std::u32string t = utf32(utf8_text);
  std::u32string clean;
  for (size_t k = 0; k < t.size(); ++k) {
    char32_t c = t[k];
    if (c == '\r') {
      if (k + 1 < t.size() && t[k + 1] == '\n') continue;
      c = '\n';
    }
    if (c == '\t') c = ' ';
    if (c < 0x20 && c != '\n') continue;
    clean += c;
  }
  if (clean.empty()) return;
  bool word_end = clean.size() == 1 && !is_word(clean[0]);
  push_undo(word_end ? kStyle : kInsert);  // a space or punctuation starts a new undo step
  if (word_end) last_kind_ = kInsert;
  TextStyle st = style_at_caret();
  if (has_selection()) {
    size_t a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
    if (!typing_set_ && a < chars_.size()) st = styles_[size_t(chars_[a].style)];
    chars_.erase(chars_.begin() + long(a), chars_.begin() + long(b));
    caret_ = anchor_ = a;
  }
  int s = intern(st);
  std::vector<Char> add;
  for (char32_t c : clean) {
    Char ch;
    ch.cp = c;
    ch.style = s;
    ch.orig_style = -1;
    add.push_back(ch);
  }
  chars_.insert(chars_.begin() + long(caret_), add.begin(), add.end());
  caret_ += add.size();
  anchor_ = caret_;
  modified_ = true;
  relayout();
}

void TextEdit::erase(bool forward, bool word) {
  size_t a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
  if (a == b) {
    if (forward) {
      if (b >= chars_.size()) return;
      b = b + 1;
      if (word)
        while (b < chars_.size() && word_char(b)) ++b;
    } else {
      if (a == 0) return;
      a = a - 1;
      if (word) {
        while (a > 0 && !word_char(a)) --a;
        while (a > 0 && word_char(a - 1)) --a;
      }
      // A soft break before the caret goes together with what it joins.
      while (a > 0 && chars_[a].cp == 0x200B) --a;
    }
  }
  push_undo(kErase);
  chars_.erase(chars_.begin() + long(a), chars_.begin() + long(b));
  caret_ = anchor_ = a;
  modified_ = true;
  relayout();
}

void TextEdit::restyle(const std::function<void(TextStyle &)> &change) {
  push_undo(kStyle);
  last_kind_ = kStyle;
  auto apply = [&](TextStyle s) {
    TextStyle old = s;
    change(s);
    if (s.font >= 0 && (s.bold != old.bold || s.italic != old.italic || s.family != old.family)) {
      // The document's font has one weight and slant: an installed font
      // of the same family takes over.
      if (s.family.empty() && layout_) s.family = request_for(*layout_->fonts[size_t(s.font)]).family;
      s.font = -1;
    }
    return s;
  };
  size_t a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
  if (a == b) {
    a = 0;
    b = chars_.size();
  }
  std::map<int, int> remap;
  for (size_t k = a; k < b; ++k) {
    int old = chars_[k].style;
    auto it = remap.find(old);
    if (it == remap.end()) it = remap.emplace(old, intern(apply(styles_[size_t(old)]))).first;
    chars_[k].style = it->second;
  }
  typing_ = apply(style_at_caret());
  typing_set_ = true;
  modified_ = true;
  relayout();
}

void TextEdit::set_align(int a) {
  if (a == align_) return;
  push_undo(kAlign);
  last_kind_ = kAlign;
  align_ = std::clamp(a, 0, 3);
  if (auto_width_ && a != 0) {
    // Centering needs a box: the width the text has now.
    double w = 0;
    for (const Row &r : rows_) w = std::max(w, r.x2 - x0_);
    width_ = std::max(w, 12.0);
    auto_width_ = false;
  }
  modified_ = true;
  relayout();
}

void TextEdit::move_by(double dx, double dy) {
  if (dx == 0 && dy == 0) return;
  push_undo(kMove);
  last_kind_ = kMove;
  x0_ += dx;
  y0_ += dy;
  moved_ = true;
  modified_ = true;
  relayout();
}

void TextEdit::set_width(double w) {
  push_undo(kMove);
  last_kind_ = kMove;
  width_ = std::max(w, 6.0);
  auto_width_ = false;
  modified_ = true;
  relayout();
}

Box TextEdit::frame() const {
  if (rows_.empty()) return Box{x0_, y0_ - 10, x0_ + width_, y0_ + 3};
  double x2 = x0_ + width_;
  if (auto_width_) {
    x2 = x0_ + 12;
    for (const Row &r : rows_) x2 = std::max(x2, r.x2 + 2);
  }
  double x1 = x0_;
  for (const Row &r : rows_) x1 = std::min(x1, r.x1);
  return Box{x1, rows_.front().top, x2, rows_.back().bottom};
}

std::vector<TextEdit::LineInfo> TextEdit::lines() const {
  std::vector<LineInfo> out;
  for (const Row &r : rows_) {
    LineInfo li;
    for (size_t k = r.begin; k < r.end; ++k) {
      const Char &c = chars_[k];
      if (is_break(c.cp)) continue;
      if (c.glyph >= 0 && !layout_->glyphs[size_t(c.glyph)].text.empty()) li.text += layout_->glyphs[size_t(c.glyph)].text;
      else li.text += utf8(c.cp);
    }
    while (!li.text.empty() && li.text.back() == ' ') li.text.pop_back();
    li.x1 = r.x1;
    li.x2 = r.x2;
    li.baseline = r.baseline;
    li.kept = r.kept;
    out.push_back(li);
  }
  return out;
}

std::set<int> TextEdit::all_glyphs() const {
  std::set<int> out;
  if (!layout_ || block_ < 0) return out;
  for (int li : layout_->blocks[size_t(block_)].lines)
    for (int gi : layout_->lines[size_t(li)].glyphs) out.insert(gi);
  return out;
}

std::set<int> TextEdit::removed_glyphs() const {
  std::set<int> out = all_glyphs();
  if (!layout_ || block_ < 0) return out;
  for (const Row &r : rows_) {
    if (!r.kept) continue;
    for (int gi : layout_->lines[size_t(layout_->blocks[size_t(block_)].lines[size_t(r.orig_line)])].glyphs)
      out.erase(gi);
  }
  return out;
}

// --------------------------------------------------------------- output

namespace {

std::string paint_ops(const Paint &p) {
  switch (p.n) {
    case 1: return pdf_num(p.v[0], 4) + " g\n";
    case 4:
      return pdf_num(p.v[0], 4) + " " + pdf_num(p.v[1], 4) + " " + pdf_num(p.v[2], 4) + " " + pdf_num(p.v[3], 4) +
             " k\n";
    case 3:
      return pdf_num(p.v[0], 4) + " " + pdf_num(p.v[1], 4) + " " + pdf_num(p.v[2], 4) + " rg\n";
    default: return "0 g\n";
  }
}

std::string matrix_text(const Matrix &m) {
  return pdf_num(m.a, 5) + " " + pdf_num(m.b, 5) + " " + pdf_num(m.c, 5) + " " + pdf_num(m.d, 5) + " " +
         pdf_num(m.e, 4) + " " + pdf_num(m.f, 4);
}

}  // namespace

std::string TextEdit::content(PdfFile &pdf, QPDFObjectHandle owner, QPDFObjectHandle resources, bool changed_only,
                              std::string &error) const {
  if (!layout_) return "";
  // The glyphs of every installed font, to embed each once.
  std::map<const SystemFont *, std::map<unsigned, std::string>> used;
  std::map<const SystemFont *, std::shared_ptr<SystemFont>> keep;
  for (const Row &r : rows_) {
    if (changed_only && r.kept) continue;
    for (const Laid &g : r.glyphs)
      if (g.sys) {
        auto &m = used[g.sys.get()];
        auto it = m.find(g.gid);
        if (it == m.end() || it->second.empty()) m[g.gid] = g.text;
        keep[g.sys.get()] = g.sys;
      }
  }
  std::map<const SystemFont *, std::string> sys_names;
  for (auto &kv : used) {
    QPDFObjectHandle f = kv.first->embed(pdf, kv.second, error);
    if (!f.isInitialized() || f.isNull()) return "";
    sys_names[kv.first] = add_resource(pdf, owner, resources, "/Font", f, "Xf");
  }
  std::map<int, std::string> doc_names;
  auto doc_name = [&](int font) {
    auto it = doc_names.find(font);
    if (it != doc_names.end()) return it->second;
    QPDFObjectHandle dict = layout_->font_dicts[size_t(font)];
    if (dict.getOwningQPDF() != &pdf.qpdf()) {
      dict = pdf.qpdf().copyForeignObject(dict);
      pdf.adopt(dict);
    }
    std::string name = add_resource(pdf, owner, resources, "/Font", dict, "Xf");
    doc_names[font] = name;
    return name;
  };
  std::map<long, std::string> alpha_names;
  auto alpha_name = [&](double a) {
    long key = lround(a * 1000);
    auto it = alpha_names.find(key);
    if (it != alpha_names.end()) return it->second;
    QPDFObjectHandle gs = QPDFObjectHandle::newDictionary();
    gs.replaceKey("/Type", QPDFObjectHandle::newName("/ExtGState"));
    gs.replaceKey("/ca", QPDFObjectHandle::newReal(a, 3, true));
    std::string name = add_resource(pdf, owner, resources, "/ExtGState", pdf.add(gs), "Xg");
    alpha_names[key] = name;
    return name;
  };
  Matrix to_user = layout_->geometry.to_user();
  Point dir = to_user.vector(1, 0), up = to_user.vector(0, -1);
  std::string out;
  for (const Row &r : rows_) {
    if (changed_only && r.kept) continue;
    size_t k = 0;
    while (k < r.glyphs.size()) {
      const Laid &g0 = r.glyphs[k];
      const TextStyle &st = styles_[size_t(g0.style)];
      std::string font = g0.sys ? sys_names[g0.sys.get()] : doc_name(g0.doc_font);
      // A run: the same font, size, paint and baseline.
      size_t e = k + 1;
      while (e < r.glyphs.size()) {
        const Laid &g = r.glyphs[e];
        const TextStyle &s2 = styles_[size_t(g.style)];
        bool same = (g.sys ? g.sys == g0.sys : (!g0.sys && g.doc_font == g0.doc_font)) &&
                    std::fabs(g.size - g0.size) < 1e-6 && s2.paint == st.paint && std::fabs(s2.hscale - st.hscale) < 1e-6 &&
                    std::fabs(g.y - g0.y) < 1e-4;
        if (!same) break;
        ++e;
      }
      std::string ops = "BT\n0 Tc 0 Tw 0 Ts 0 Tr\n";
      if (st.paint.alpha < 0.999) ops += alpha_name(st.paint.alpha) + " gs\n";
      ops += paint_ops(st.paint);
      ops += font + " " + pdf_num(g0.size, 4) + " Tf\n";
      ops += pdf_num(st.hscale * 100, 3) + " Tz\n";
      Point o = to_user.apply(g0.x, g0.y);
      ops += matrix_text(Matrix{dir.x, dir.y, up.x, up.y, o.x, o.y}) + " Tm\n[";
      double pen = g0.x;
      std::string run;
      bool sys = bool(g0.sys);
      auto flush = [&]() {
        if (!run.empty()) ops += pdf_hex(run);
        run.clear();
      };
      for (size_t j = k; j < e; ++j) {
        const Laid &g = r.glyphs[j];
        double delta = g.x - pen;
        if (std::fabs(delta) > 0.002) {
          flush();
          ops += " " + pdf_num(-delta * 1000 / (g.size * st.hscale), 3) + " ";
          pen = g.x;
        }
        if (sys) {
          run += char((g.gid >> 8) & 0xFF);
          run += char(g.gid & 0xFF);
        } else {
          run += g.bytes;
        }
        pen += g.width * g.size * st.hscale;
      }
      flush();
      ops += "] TJ\nET\n";
      out += ops;
      k = e;
    }
  }
  if (out.empty()) return out;
  return "q\n" + out + "Q\n";
}

// ------------------------------------------------------------ the page

namespace {

// The operators of the original content that leave the graphics state
// changed for whatever is drawn after them.
bool leaves_state(const std::vector<Op> &ops) {
  int depth = 0;
  for (const Op &op : ops) {
    if (op.name == "q") ++depth;
    else if (op.name == "Q") depth = std::max(0, depth - 1);
    else if (depth == 0 && (op.name == "cm" || op.name == "W" || op.name == "W*" || op.name == "gs")) return true;
  }
  return depth != 0;
}

// The text operator |op| without the codes of the glyphs in |gone|.
std::string rewrite_show(const Op &op, const std::vector<const Glyph *> &glyphs, const std::set<const Glyph *> &gone) {
  struct Item {
    bool number;
    double num;
    std::string bytes;
  };
  std::vector<Item> items;
  auto add_num = [&](double v) {
    if (!items.empty() && items.back().number) items.back().num += v;
    else items.push_back(Item{true, v, ""});
  };
  auto add_bytes = [&](const std::string &b) {
    if (b.empty()) return;
    if (!items.empty() && !items.back().number) items.back().bytes += b;
    else items.push_back(Item{false, 0, b});
  };
  auto rewrite_string = [&](int part, const std::string &s) {
    std::vector<const Glyph *> mine;
    for (const Glyph *g : glyphs)
      if (g->part == part) mine.push_back(g);
    std::sort(mine.begin(), mine.end(), [](const Glyph *a, const Glyph *b) { return a->offset < b->offset; });
    size_t at = 0;
    for (const Glyph *g : mine) {
      if (g->offset > at) add_bytes(s.substr(at, g->offset - at));
      if (gone.count(g)) {
        if (g->font_size != 0)
          add_num(-1000.0 * (g->width + (g->char_space + (g->space ? g->word_space : 0)) / g->font_size));
      } else {
        add_bytes(s.substr(g->offset, size_t(g->bytes)));
      }
      at = g->offset + size_t(g->bytes);
    }
    if (at < s.size()) add_bytes(s.substr(at));
  };
  std::string prefix;
  if (op.name == "TJ" && !op.args.empty() && op.args[0].kind == Operand::Array) {
    int part = 0;
    for (const Operand &e : op.args[0].items) {
      if (e.kind == Operand::String) rewrite_string(part, e.str);
      else if (e.kind == Operand::Number) add_num(e.num);
      ++part;
    }
  } else if (!op.args.empty() && op.args.back().kind == Operand::String) {
    rewrite_string(0, op.args.back().str);
    if (op.name == "'") prefix = "T* ";
    if (op.name == "\"" && op.args.size() >= 3)
      prefix = unparse(op.args[0]) + " Tw " + unparse(op.args[1]) + " Tc T* ";
  }
  std::string out = " " + prefix + "[";
  for (size_t k = 0; k < items.size(); ++k) {
    if (k) out += " ";
    if (items[k].number) {
      if (std::fabs(items[k].num) < 1e-4) {
        out += "0";
        continue;
      }
      out += pdf_num(items[k].num, 3);
    } else {
      out += pdf_hex(items[k].bytes);
    }
  }
  return out + "] TJ ";
}

}  // namespace

static bool rewrite_page(PdfFile &pdf, PageLayout &layout, const std::set<int> &glyphs,
                         const std::vector<Splice> &extra, const std::string &append, std::string &error) {
  QPDFObjectHandle page = pdf.page(layout.page);
  if (!page.isDictionary()) {
    error = "no such page";
    return false;
  }
  // Group the glyphs by stream and operation.
  std::map<int, std::map<int, std::vector<const Glyph *>>> by_op;
  std::set<const Glyph *> gone;
  for (int gi : glyphs) {
    if (gi < 0 || gi >= int(layout.glyphs.size())) continue;
    const Glyph &g = layout.glyphs[size_t(gi)];
    if (g.stream != 0 && layout.streams[size_t(g.stream)].parent != 0) {
      error = "text inside nested forms";
      return false;
    }
    gone.insert(&g);
    by_op[g.stream][g.op];
  }
  for (const Glyph &g : layout.glyphs)
    if (by_op.count(g.stream) && by_op[g.stream].count(g.op)) by_op[g.stream][g.op].push_back(&g);
  QPDFObjectHandle res;
  std::vector<Splice> page_splices = extra;
  for (auto &st : by_op) {
    PageLayout::Stream &S = layout.streams[size_t(st.first)];
    std::vector<Splice> splices;
    for (auto &op : st.second) {
      const Op &o = S.ops[size_t(op.first)];
      splices.push_back(Splice{o.begin, o.end, rewrite_show(o, op.second, gone)});
    }
    if (st.first == 0) {
      page_splices.insert(page_splices.end(), splices.begin(), splices.end());
      continue;
    }
    // A form: a private copy with the new content, drawn instead of the
    // original by this page.
    std::string data = apply_splices(S.data, splices);
    QPDFObjectHandle dict = S.object.getDict().shallowCopy();
    QPDFObjectHandle copy = pdf.add_stream(data, dict);
    if (!res.isInitialized()) res = page_resources(pdf, page);
    QPDFObjectHandle xo = res.getKey("/XObject");
    if (xo.isIndirect()) {
      res.replaceKey("/XObject", xo.shallowCopy());
      xo = res.getKey("/XObject");
    }
    if (!xo.isDictionary()) {
      error = "form resources";
      return false;
    }
    xo.replaceKey("/" + S.name, copy);
    pdf.changed(page);
  }
  if (page_splices.empty() && append.empty()) return true;
  std::string data = apply_splices(layout.streams[0].data, page_splices);
  if (!append.empty()) {
    if (leaves_state(parse_content(data))) data = "q\n" + data + "\nQ\n";
    data += "\n" + append;
  }
  set_page_content(pdf, page, data);
  return true;
}

bool remove_glyphs(PdfFile &pdf, PageLayout &layout, const std::set<int> &glyphs, std::string &error) {
  return rewrite_page(pdf, layout, glyphs, {}, "", error);
}

void append_content(PdfFile &pdf, int page_index, const std::string &ops) {
  QPDFObjectHandle page = pdf.page(page_index);
  if (!page.isDictionary() || ops.empty()) return;
  // New streams around the old ones, which stay as they are.
  QPDFPageObjectHelper ph(page);
  bool wrap = leaves_state(parse_content(page_content(page)));
  if (wrap) ph.addPageContents(pdf.add_stream("q\n", QPDFObjectHandle::newNull(), false), true);
  ph.addPageContents(pdf.add_stream((wrap ? "\nQ\n" : "\n") + ops, QPDFObjectHandle::newNull()), false);
  pdf.changed(page);
}

bool commit_text(PdfFile &pdf, PageLayout &layout, const TextEdit &edit, std::string &error) {
  QPDFObjectHandle page = pdf.page(layout.page);
  if (!page.isDictionary()) {
    error = "no such page";
    return false;
  }
  QPDFObjectHandle res = page_resources(pdf, page);
  std::string ops = edit.content(pdf, page, res, true, error);
  if (!error.empty()) return false;
  std::set<int> gone = edit.removed_glyphs();
  if (gone.empty()) {
    append_content(pdf, layout.page, ops);
    return true;
  }
  return rewrite_page(pdf, layout, gone, {}, ops, error);
}

bool preview_pdf(PdfFile &source, const PageLayout &layout, const TextEdit &edit, std::string &out,
                 std::string &error) {
  (void)source;
  std::unique_ptr<PdfFile> tiny = PdfFile::create();
  QPDF &q = tiny->qpdf();
  QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
  page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
  QPDFObjectHandle box = QPDFObjectHandle::newArray();
  for (double v : {layout.geometry.box.x1, layout.geometry.box.y1, layout.geometry.box.x2, layout.geometry.box.y2})
    box.appendItem(QPDFObjectHandle::newReal(v, 4, true));
  page.replaceKey("/MediaBox", box);
  if (layout.geometry.rotate) page.replaceKey("/Rotate", QPDFObjectHandle::newInteger(layout.geometry.rotate));
  page.replaceKey("/Resources", QPDFObjectHandle::newDictionary());
  try {
    QPDFPageDocumentHelper(q).addPage(QPDFPageObjectHelper(q.makeIndirectObject(page)), false);
    QPDFObjectHandle pg = tiny->page(0);
    QPDFObjectHandle res = pg.getKey("/Resources");
    std::string ops = edit.content(*tiny, pg, res, false, error);
    if (!error.empty()) return false;
    set_page_content(*tiny, pg, ops);
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  return tiny->write_memory(out, PdfFile::Write::Rewrite, error);
}

// --------------------------------------------------------------- images

namespace {

struct JpegError {
  jpeg_error_mgr mgr;
  jmp_buf jump;
};

void jpeg_fail(j_common_ptr c) { longjmp(reinterpret_cast<JpegError *>(c->err)->jump, 1); }

// Width, height and color layout of a JPEG file, without decoding it.
bool jpeg_info(const std::string &data, int &w, int &h, int &comps, bool &adobe) {
  jpeg_decompress_struct c;
  JpegError err;
  c.err = jpeg_std_error(&err.mgr);
  err.mgr.error_exit = jpeg_fail;
  if (setjmp(err.jump)) {
    jpeg_destroy_decompress(&c);
    return false;
  }
  jpeg_create_decompress(&c);
  jpeg_mem_src(&c, reinterpret_cast<const unsigned char *>(data.data()), (unsigned long)data.size());
  if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&c);
    return false;
  }
  w = int(c.image_width);
  h = int(c.image_height);
  comps = c.num_components;
  adobe = c.saw_Adobe_marker;
  jpeg_destroy_decompress(&c);
  return w > 0 && h > 0 && (comps == 1 || comps == 3 || comps == 4);
}

QPDFObjectHandle image_dict(int w, int h, const char *cs) {
  QPDFObjectHandle d = QPDFObjectHandle::newDictionary();
  d.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  d.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
  d.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
  d.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
  d.replaceKey("/ColorSpace", QPDFObjectHandle::newName(cs));
  d.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  return d;
}

// The matrix that maps the unit square of an image onto |box| (page
// space), upright.
Matrix placement(const PageGeometry &geo, const Box &box) {
  Matrix to_user = geo.to_user();
  Point o = to_user.apply(box.x1, box.y2);
  Point ux = to_user.vector(box.w(), 0), uy = to_user.vector(0, -box.h());
  return Matrix{ux.x, ux.y, uy.x, uy.y, o.x, o.y};
}

}  // namespace

QPDFObjectHandle image_xobject(PdfFile &pdf, cairo_surface_t *img, std::string &error) {
  if (!img || cairo_surface_get_type(img) != CAIRO_SURFACE_TYPE_IMAGE) {
    error = "not an image";
    return QPDFObjectHandle();
  }
  cairo_surface_flush(img);
  int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
  int stride = cairo_image_surface_get_stride(img);
  bool alpha = cairo_image_surface_get_format(img) == CAIRO_FORMAT_ARGB32;
  const unsigned char *data = cairo_image_surface_get_data(img);
  if (w <= 0 || h <= 0 || !data) {
    error = "empty image";
    return QPDFObjectHandle();
  }
  std::string rgb, mask;
  rgb.reserve(size_t(w) * size_t(h) * 3);
  bool any_alpha = false;
  for (int y = 0; y < h; ++y) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(data + size_t(y) * size_t(stride));
    for (int x = 0; x < w; ++x) {
      uint32_t p = row[x];
      unsigned a = alpha ? (p >> 24) & 255 : 255, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
      if (a > 0 && a < 255) {  // cairo stores premultiplied colors
        r = std::min(255u, r * 255 / a);
        g = std::min(255u, g * 255 / a);
        b = std::min(255u, b * 255 / a);
      }
      rgb += char(r);
      rgb += char(g);
      rgb += char(b);
      mask += char(a);
      any_alpha = any_alpha || a < 255;
    }
  }
  QPDFObjectHandle dict = image_dict(w, h, "/DeviceRGB");
  if (any_alpha) dict.replaceKey("/SMask", pdf.add_stream(mask, image_dict(w, h, "/DeviceGray")));
  return pdf.add_stream(rgb, dict);
}

QPDFObjectHandle image_xobject(PdfFile &pdf, const std::string &path, int &w, int &h, std::string &error) {
  std::string data;
  if (!read_file(path, data) || data.empty()) {
    error = "the image could not be read";
    return QPDFObjectHandle();
  }
  int comps = 0;
  bool adobe = false;
  if (data.size() > 3 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8 &&
      jpeg_info(data, w, h, comps, adobe)) {
    // JPEG data goes into the PDF unchanged (DCTDecode).
    QPDFObjectHandle dict = image_dict(w, h, comps == 1 ? "/DeviceGray" : comps == 3 ? "/DeviceRGB" : "/DeviceCMYK");
    if (comps == 4 && adobe) {
      QPDFObjectHandle dec = QPDFObjectHandle::newArray();
      for (int k = 0; k < 4; ++k) {
        dec.appendItem(QPDFObjectHandle::newInteger(1));
        dec.appendItem(QPDFObjectHandle::newInteger(0));
      }
      dict.replaceKey("/Decode", dec);
    }
    QPDFObjectHandle s = pdf.qpdf().newStream();
    for (const auto &key : dict.getKeys()) s.getDict().replaceKey(key, dict.getKey(key));
    s.replaceStreamData(data, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
    pdf.changed(s);
    return s;
  }
  cairo_surface_t *img = load_image(path, 0, 0);
  if (!img) {
    error = "the image could not be read";
    return QPDFObjectHandle();
  }
  w = cairo_image_surface_get_width(img);
  h = cairo_image_surface_get_height(img);
  QPDFObjectHandle x = image_xobject(pdf, img, error);
  cairo_surface_destroy(img);
  return x;
}

bool add_image(PdfFile &pdf, int page, QPDFObjectHandle xobject, const Box &box, std::string &error) {
  QPDFObjectHandle pg = pdf.page(page);
  if (!pg.isDictionary() || !xobject.isStream()) {
    error = "no such page";
    return false;
  }
  QPDFObjectHandle res = page_resources(pdf, pg);
  std::string name = add_resource(pdf, pg, res, "/XObject", xobject, "Im");
  Matrix m = placement(pdf.geometry(page), box.normalized());
  append_content(pdf, page, "q " + matrix_text(m) + " cm " + name + " Do Q\n");
  return true;
}

static bool image_splice(PdfFile &pdf, PageLayout &layout, int index, Splice s, std::string &error) {
  if (index < 0 || index >= int(layout.images.size()) || !layout.images[size_t(index)].editable) {
    error = "this image is part of a form";
    return false;
  }
  return rewrite_page(pdf, layout, {}, {s}, "", error);
}

bool move_image(PdfFile &pdf, PageLayout &layout, int index, const Box &box0, std::string &error) {
  if (index < 0 || index >= int(layout.images.size())) return false;
  const ImageItem &im = layout.images[size_t(index)];
  Box ob = im.box, box = box0.normalized();
  if (ob.w() < 1e-6 || ob.h() < 1e-6) {
    error = "empty image";
    return false;
  }
  Matrix pd = Matrix::translate(-ob.x1, -ob.y1) * Matrix::scale(box.w() / ob.w(), box.h() / ob.h()) *
              Matrix::translate(box.x1, box.y1);
  Matrix m = layout.geometry.to_display() * pd * layout.geometry.to_user();
  Matrix inv;
  if (!im.ctm.invert(inv)) {
    error = "the image has no area";
    return false;
  }
  Matrix x = im.ctm * m * inv;
  const Op &op = layout.streams[0].ops[size_t(im.op)];
  std::string text = layout.streams[0].data.substr(op.begin, op.end - op.begin);
  return image_splice(pdf, layout, index, Splice{op.begin, op.end, " q " + matrix_text(x) + " cm " + text + " Q "}, error);
}

bool delete_image(PdfFile &pdf, PageLayout &layout, int index, std::string &error) {
  if (index < 0 || index >= int(layout.images.size())) return false;
  const Op &op = layout.streams[0].ops[size_t(layout.images[size_t(index)].op)];
  return image_splice(pdf, layout, index, Splice{op.begin, op.end, " "}, error);
}

bool replace_image(PdfFile &pdf, PageLayout &layout, int index, QPDFObjectHandle xobject, std::string &error) {
  if (index < 0 || index >= int(layout.images.size()) || !xobject.isStream()) return false;
  const ImageItem &im = layout.images[size_t(index)];
  QPDFObjectHandle d = xobject.getDict();
  double iw = d.getKey("/Width").isNumber() ? d.getKey("/Width").getNumericValue() : 1;
  double ih = d.getKey("/Height").isNumber() ? d.getKey("/Height").getNumericValue() : 1;
  // The new picture inside the old box, keeping its proportions.
  Box ob = im.box;
  double k = std::min(ob.w() / std::max(1.0, iw), ob.h() / std::max(1.0, ih));
  double w = iw * k, h = ih * k;
  Box nb{ob.x1 + (ob.w() - w) / 2, ob.y1 + (ob.h() - h) / 2, ob.x1 + (ob.w() + w) / 2, ob.y1 + (ob.h() + h) / 2};
  Matrix inv;
  if (!im.ctm.invert(inv)) {
    error = "the image has no area";
    return false;
  }
  Matrix x = placement(layout.geometry, nb) * inv;
  QPDFObjectHandle page = pdf.page(layout.page);
  QPDFObjectHandle res = page_resources(pdf, page);
  std::string name = add_resource(pdf, page, res, "/XObject", xobject, "Im");
  const Op &op = layout.streams[0].ops[size_t(im.op)];
  return image_splice(pdf, layout, index, Splice{op.begin, op.end, " q " + matrix_text(x) + " cm " + name + " Do Q "},
                      error);
}

// ------------------------------------------------------------- redaction

namespace {

bool overlaps(const Box &a, const Box &b) { return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2; }
bool inside(const Box &a, const Box &outer) {
  return a.x1 >= outer.x1 - 0.5 && a.x2 <= outer.x2 + 0.5 && a.y1 >= outer.y1 - 0.5 && a.y2 <= outer.y2 + 0.5;
}

// A copy of an image XObject with the pixels under |boxes| painted black.
QPDFObjectHandle redacted_image(PdfFile &pdf, const PageLayout &layout, const ImageItem &im,
                                const std::vector<Box> &boxes, std::string &error) {
  QPDFObjectHandle x = im.xobject;
  QPDFObjectHandle d = x.getDict();
  int w = d.getKey("/Width").isInteger() ? d.getKey("/Width").getIntValueAsInt() : 0;
  int h = d.getKey("/Height").isInteger() ? d.getKey("/Height").getIntValueAsInt() : 0;
  int bpc = d.getKey("/BitsPerComponent").isInteger() ? d.getKey("/BitsPerComponent").getIntValueAsInt() : 0;
  QPDFObjectHandle cs = d.getKey("/ColorSpace");
  int comps = 0;
  if (cs.isName()) comps = cs.getName() == "/DeviceGray" ? 1 : cs.getName() == "/DeviceRGB" ? 3 : cs.getName() == "/DeviceCMYK" ? 4 : 0;
  else if (cs.isArray() && cs.getArrayNItems() == 2 && cs.getArrayItem(0).isName() &&
           cs.getArrayItem(0).getName() == "/ICCBased" && cs.getArrayItem(1).isStream()) {
    QPDFObjectHandle nn = cs.getArrayItem(1).getDict().getKey("/N");
    comps = nn.isInteger() ? nn.getIntValueAsInt() : 0;
  }
  if (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue()) comps = 0;
  if (w <= 0 || h <= 0 || bpc != 8 || comps <= 0 || d.hasKey("/Decode")) {
    error = "an image Xanara cannot redact in place";
    return QPDFObjectHandle();
  }
  std::string px;
  try {
    std::shared_ptr<Buffer> b = x.getStreamData(qpdf_dl_all);
    px.assign(reinterpret_cast<const char *>(b->getBuffer()), b->getSize());
  } catch (std::exception &e) {
    error = e.what();
    return QPDFObjectHandle();
  }
  if (px.size() < size_t(w) * size_t(h) * size_t(comps)) {
    error = "image data too short";
    return QPDFObjectHandle();
  }
  // Page space -> image pixels: the inverse of unit square -> page.
  Matrix unit_to_page = im.ctm * layout.geometry.to_display(), page_to_unit;
  if (!unit_to_page.invert(page_to_unit)) {
    error = "the image has no area";
    return QPDFObjectHandle();
  }
  for (const Box &b : boxes) {
    if (!overlaps(b, im.box)) continue;
    Point c[4] = {page_to_unit.apply(b.x1, b.y1), page_to_unit.apply(b.x2, b.y1), page_to_unit.apply(b.x1, b.y2),
                  page_to_unit.apply(b.x2, b.y2)};
    double u1 = 1e9, v1 = 1e9, u2 = -1e9, v2 = -1e9;
    for (const Point &p : c) {
      u1 = std::min(u1, p.x);
      u2 = std::max(u2, p.x);
      v1 = std::min(v1, p.y);
      v2 = std::max(v2, p.y);
    }
    // Image rows run from the top (v = 1) down.
    int x1 = std::max(0, int(std::floor(u1 * w)) - 1), x2 = std::min(w, int(std::ceil(u2 * w)) + 1);
    int y1 = std::max(0, int(std::floor((1 - v2) * h)) - 1), y2 = std::min(h, int(std::ceil((1 - v1) * h)) + 1);
    for (int y = y1; y < y2; ++y)
      for (int x = x1; x < x2; ++x)
        for (int k = 0; k < comps; ++k)
          px[(size_t(y) * size_t(w) + size_t(x)) * size_t(comps) + size_t(k)] = char(comps == 4 ? (k == 3 ? 255 : 0) : 0);
  }
  QPDFObjectHandle dict = QPDFObjectHandle::newDictionary();
  for (const char *key : {"/Type", "/Subtype", "/Width", "/Height", "/BitsPerComponent", "/ColorSpace", "/SMask",
                          "/Intent", "/Interpolate"})
    if (d.hasKey(key)) dict.replaceKey(key, d.getKey(key));
  return pdf.add_stream(px, dict);
}

}  // namespace

bool redact_page(PdfFile &pdf, int page, const std::vector<Box> &boxes0, std::string &error) {
  std::vector<Box> boxes;
  for (const Box &b : boxes0) boxes.push_back(b.normalized());
  if (boxes.empty()) return true;
  std::unique_ptr<PageLayout> layout = PageLayout::read(pdf, page);
  if (!layout->complete) {
    error = "the page could not be read completely";
    return false;
  }
  QPDFObjectHandle pg = pdf.page(page);
  auto hit = [&](const Box &b) {
    for (const Box &r : boxes)
      if (overlaps(b, r)) return true;
    return false;
  };
  auto covered = [&](const Box &b) {
    for (const Box &r : boxes)
      if (inside(b, r)) return true;
    return false;
  };
  // Text.
  std::set<int> gone;
  for (size_t k = 0; k < layout->glyphs.size(); ++k) {
    const Glyph &g = layout->glyphs[k];
    Box cell = g.box;
    if (cell.empty()) cell = Box{g.origin.x - 0.5, g.origin.y - 0.5, g.origin.x + 0.5, g.origin.y + 0.5};
    if (hit(cell)) gone.insert(int(k));
  }
  std::vector<Splice> page_splices;
  QPDFObjectHandle res = page_resources(pdf, pg);
  // Images: removed when the boxes cover them, else redacted copies.
  for (const ImageItem &im : layout->images) {
    if (!hit(im.box)) continue;
    if (im.stream != 0) {
      error = "an image inside a form";
      return false;
    }
    const Op &op = layout->streams[0].ops[size_t(im.op)];
    if (covered(im.box) || im.inline_image) {
      if (!covered(im.box)) {
        error = "an inline image";
        return false;
      }
      page_splices.push_back(Splice{op.begin, op.end, " "});
      continue;
    }
    QPDFObjectHandle copy = redacted_image(pdf, *layout, im, boxes, error);
    if (!copy.isInitialized() || copy.isNull()) return false;
    std::string name = add_resource(pdf, pg, res, "/XObject", copy, "Rx");
    page_splices.push_back(Splice{op.begin, op.end, " " + name + " Do "});
  }
  // Paths entirely under a box are removed; the rest is clipped below.
  for (const PathItem &p : layout->paths) {
    if (p.stream != 0 || !covered(p.box) || p.first_op < 0) continue;
    const auto &ops = layout->streams[0].ops;
    for (int k = p.first_op; k <= p.last_op && k < int(ops.size()); ++k) {
      const std::string &n = ops[size_t(k)].name;
      static const char *path_ops[] = {"m", "l", "c", "v", "y", "re", "h", "S", "s", "f", "F", "f*",
                                       "B", "B*", "b", "b*", "n", "W", "W*", "sh"};
      if (std::find_if(std::begin(path_ops), std::end(path_ops), [&](const char *x) { return n == x; }) !=
          std::end(path_ops))
        page_splices.push_back(Splice{ops[size_t(k)].begin, ops[size_t(k)].end, " "});
    }
  }
  for (int gi : gone) {
    const Glyph &g = layout->glyphs[size_t(gi)];
    if (g.stream != 0 && layout->streams[size_t(g.stream)].parent != 0) {
      error = "text inside nested forms";
      return false;
    }
  }
  // The rest of the page, clipped around the boxes, then black boxes.
  Matrix to_user = layout->geometry.to_user();
  auto rect_path = [&](const Box &b, bool reverse) {
    Point p[4] = {to_user.apply(b.x1, b.y1), to_user.apply(b.x2, b.y1), to_user.apply(b.x2, b.y2),
                  to_user.apply(b.x1, b.y2)};
    std::string s = pdf_num(p[0].x, 3) + " " + pdf_num(p[0].y, 3) + " m ";
    for (int k = 1; k < 4; ++k) {
      const Point &q = p[reverse ? 4 - k : k];
      s += pdf_num(q.x, 3) + " " + pdf_num(q.y, 3) + " l ";
    }
    return s + "h ";
  };
  Box whole{-10000, -10000, 20000, 20000};
  std::string clip = "q " + rect_path(whole, false);
  for (const Box &b : boxes) clip += rect_path(b, true);
  clip += "W n\n";
  std::string black = "q 0 g ";
  for (const Box &b : boxes) black += rect_path(b, false) + "f ";
  black += "Q\n";
  // Annotations that touch a box go too.
  QPDFObjectHandle annots = pg.getKey("/Annots");
  if (annots.isArray()) {
    Matrix to_display = layout->geometry.to_display();
    QPDFObjectHandle keep = QPDFObjectHandle::newArray();
    bool removed = false;
    for (auto a : annots.getArrayAsVector()) {
      QPDFObjectHandle rect = a.isDictionary() ? a.getKey("/Rect") : QPDFObjectHandle();
      bool touch = false;
      if (rect.isArray() && rect.getArrayNItems() == 4) {
        double v[4];
        for (int k = 0; k < 4; ++k) v[k] = rect.getArrayItem(k).isNumber() ? rect.getArrayItem(k).getNumericValue() : 0;
        Point p1 = to_display.apply(v[0], v[1]), p2 = to_display.apply(v[2], v[3]);
        touch = hit(Box{std::min(p1.x, p2.x), std::min(p1.y, p2.y), std::max(p1.x, p2.x), std::max(p1.y, p2.y)});
      }
      if (touch) removed = true;
      else keep.appendItem(a);
    }
    if (removed) {
      pg.replaceKey("/Annots", keep);
      pdf.changed(pg);
    }
  }
  // Apply: glyph removal and splices, then wrap with the clip.
  if (!rewrite_page(pdf, *layout, gone, page_splices, "", error)) return false;
  std::string data = page_content(pdf.page(page));
  set_page_content(pdf, pdf.page(page), clip + data + "\nQ\n" + black);
  pdf.structural();
  return true;
}

}  // namespace xanara
