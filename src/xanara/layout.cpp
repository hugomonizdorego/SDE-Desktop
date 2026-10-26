// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The content stream interpreter and the grouping of glyphs into lines
// and blocks; see layout.h. It follows PDF 32000-1 sections 8 and 9 for
// the graphics and text state; what it does not need for editing (clip
// paths, shading geometry, transparency groups) it only skips.
#include "layout.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <algorithm>
#include <cmath>
#include <map>

namespace xanara {

Rgb Paint::rgb() const {
  switch (n) {
    case 1: return Rgb{v[0], v[0], v[0]};
    case 3: return Rgb{v[0], v[1], v[2]};
    case 4: {
      double k = v[3];
      return Rgb{1 - std::min(1.0, v[0] + k), 1 - std::min(1.0, v[1] + k), 1 - std::min(1.0, v[2] + k)};
    }
    default: return Rgb{0, 0, 0};
  }
}

bool Paint::operator==(const Paint &o) const {
  if (n != o.n || std::fabs(alpha - o.alpha) > 1e-4) return false;
  for (int k = 0; k < n && k < 4; ++k)
    if (std::fabs(v[k] - o.v[k]) > 1e-4) return false;
  return true;
}

double Glyph::size() const { return std::hypot(up.x, up.y); }
double Glyph::advance() const { return std::hypot(end.x - origin.x, end.y - origin.y); }

namespace {

struct GState {
  Matrix ctm;
  Paint fill;
  double tc = 0, tw = 0, th = 1, tl = 0, rise = 0, tfs = 0;
  int font = -1;
  int render = 0;
};

Matrix matrix_of(const Op &op) {
  return Matrix{op.num(0, 1), op.num(1, 0), op.num(2, 0), op.num(3, 1), op.num(4, 0), op.num(5, 0)};
}

Matrix matrix_of(QPDFObjectHandle a) {
  Matrix m;
  if (a.isArray() && a.getArrayNItems() == 6) {
    double v[6];
    for (int k = 0; k < 6; ++k) v[k] = a.getArrayItem(k).isNumber() ? a.getArrayItem(k).getNumericValue() : 0;
    m = Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
  }
  return m;
}

Box bounds(std::initializer_list<Point> pts) {
  Box b{1e18, 1e18, -1e18, -1e18};
  for (const Point &p : pts) {
    b.x1 = std::min(b.x1, p.x);
    b.y1 = std::min(b.y1, p.y);
    b.x2 = std::max(b.x2, p.x);
    b.y2 = std::max(b.y2, p.y);
  }
  return b;
}

std::string stream_data(QPDFObjectHandle s) {
  try {
    std::shared_ptr<Buffer> b = s.getStreamData(qpdf_dl_generalized);
    return std::string(reinterpret_cast<const char *>(b->getBuffer()), b->getSize());
  } catch (std::exception &) {
    return "";
  }
}

struct Interp {
  PdfFile &pdf;
  PageLayout &L;
  Matrix to_display;
  std::map<QPDFObjGen, int> font_index;

  Interp(PdfFile &p, PageLayout &l) : pdf(p), L(l), to_display(l.geometry.to_display()) {}

  int font_for(QPDFObjectHandle resources, const std::string &name) {
    QPDFObjectHandle fonts = resources.isDictionary() ? resources.getKey("/Font") : QPDFObjectHandle();
    QPDFObjectHandle dict = fonts.isDictionary() ? fonts.getKey("/" + name) : QPDFObjectHandle();
    if (!dict.isDictionary()) return -1;
    if (dict.isIndirect()) {
      auto it = font_index.find(dict.getObjGen());
      if (it != font_index.end()) return it->second;
    }
    L.fonts.push_back(PdfFont::load(dict));
    L.font_dicts.push_back(dict);
    int k = int(L.fonts.size()) - 1;
    if (dict.isIndirect()) font_index[dict.getObjGen()] = k;
    return k;
  }

  int components(QPDFObjectHandle resources, const Operand &cs) {
    if (cs.kind != Operand::Name) return 0;
    const std::string &n = cs.str;
    if (n == "DeviceGray" || n == "CalGray" || n == "G") return 1;
    if (n == "DeviceRGB" || n == "CalRGB" || n == "RGB") return 3;
    if (n == "DeviceCMYK" || n == "CMYK") return 4;
    QPDFObjectHandle spaces = resources.isDictionary() ? resources.getKey("/ColorSpace") : QPDFObjectHandle();
    QPDFObjectHandle s = spaces.isDictionary() ? spaces.getKey("/" + n) : QPDFObjectHandle();
    if (s.isArray() && s.getArrayNItems() >= 2 && s.getArrayItem(0).isName()) {
      std::string kind = s.getArrayItem(0).getName();
      if (kind == "/ICCBased" && s.getArrayItem(1).isStream()) {
        QPDFObjectHandle nn = s.getArrayItem(1).getDict().getKey("/N");
        return nn.isInteger() ? nn.getIntValueAsInt() : 0;
      }
      if (kind == "/CalRGB") return 3;
      if (kind == "/CalGray") return 1;
    }
    return 0;  // Separation, DeviceN, Indexed, patterns: not reproduced
  }

  void show(int si, int op_index, int part, const std::string &s, GState &gs, Matrix &tm) {
    PdfFont *font = gs.font >= 0 ? L.fonts[size_t(gs.font)].get() : nullptr;
    if (!font) {
      L.complete = false;
      return;
    }
    for (size_t i = 0; i < s.size();) {
      int nb = 1;
      unsigned code = font->next(s, i, nb);
      Glyph g;
      g.code = code;
      g.bytes = nb;
      g.font = gs.font;
      g.text = font->text(code);
      g.width = font->width(code);
      g.space = nb == 1 && code == 32;
      g.font_size = gs.tfs;
      g.hscale = gs.th;
      g.rise = gs.rise;
      g.char_space = gs.tc;
      g.word_space = gs.tw;
      g.render = gs.render;
      g.paint = gs.fill;
      g.stream = si;
      g.op = op_index;
      g.part = part;
      g.offset = i;
      Matrix text_to_page = tm * gs.ctm * to_display;
      Matrix trm = Matrix{gs.tfs * gs.th, 0, 0, gs.tfs, 0, gs.rise} * text_to_page;
      double tx = (g.width * gs.tfs + gs.tc + (g.space ? gs.tw : 0)) * gs.th;
      g.origin = trm.apply(0, 0);
      Point adv = text_to_page.vector(tx, 0);
      g.end = Point{g.origin.x + adv.x, g.origin.y + adv.y};
      Point up = Matrix{gs.th, 0, 0, 1, 0, 0}.vector(0, 1);
      up = (Matrix{gs.tfs, 0, 0, gs.tfs, 0, 0} * text_to_page).vector(up.x, up.y);
      g.up = up;
      double a = font->ascent, d = font->descent;
      Point o = g.origin, e = g.end;
      g.box = bounds({Point{o.x + up.x * d, o.y + up.y * d}, Point{o.x + up.x * a, o.y + up.y * a},
                      Point{e.x + up.x * d, e.y + up.y * d}, Point{e.x + up.x * a, e.y + up.y * a}});
      double len = std::hypot(adv.x, adv.y), ulen = std::hypot(up.x, up.y);
      bool upright = ulen > 1e-6 && up.y < 0 && std::fabs(up.x) < 0.05 * ulen;
      g.horizontal = upright && (len < 1e-9 || (adv.x > 0 && std::fabs(adv.y) < 0.05 * len));
      g.editable = g.horizontal && font->known_widths && !font->vertical && gs.render != 3 && gs.render < 4 &&
                   (si == 0 || L.streams[size_t(si)].parent == 0) && gs.tfs != 0;
      L.glyphs.push_back(std::move(g));
      tm = Matrix::translate(tx, 0) * tm;
      i += size_t(nb);
    }
  }

  void run(int si, Matrix ctm, int depth) {
    if (depth > 12) return;
    std::vector<GState> stack;
    GState gs;
    gs.ctm = ctm;
    Matrix tm, tlm;
    Box path{1e18, 1e18, -1e18, -1e18};
    int path_first = -1;
    Point cur;
    auto extend = [&](double x, double y) {
      Point p = (gs.ctm * to_display).apply(x, y);
      path.x1 = std::min(path.x1, p.x);
      path.y1 = std::min(path.y1, p.y);
      path.x2 = std::max(path.x2, p.x);
      path.y2 = std::max(path.y2, p.y);
    };
    size_t n = L.streams[size_t(si)].ops.size();
    for (size_t k = 0; k < n; ++k) {
      const Op &op = L.streams[size_t(si)].ops[k];
      const std::string &o = op.name;
      QPDFObjectHandle res = L.streams[size_t(si)].resources;
      if (o == "q") {
        if (stack.size() < 256) stack.push_back(gs);
      } else if (o == "Q") {
        if (!stack.empty()) {
          gs = stack.back();
          stack.pop_back();
        }
      } else if (o == "cm") {
        gs.ctm = matrix_of(op) * gs.ctm;
      } else if (o == "BT") {
        tm = tlm = Matrix();
      } else if (o == "Tf") {
        if (!op.args.empty() && op.args[0].kind == Operand::Name) gs.font = font_for(res, op.args[0].str);
        gs.tfs = op.num(1, gs.tfs);
      } else if (o == "Tc") {
        gs.tc = op.num(0);
      } else if (o == "Tw") {
        gs.tw = op.num(0);
      } else if (o == "Tz") {
        gs.th = op.num(0, 100) / 100.0;
      } else if (o == "TL") {
        gs.tl = op.num(0);
      } else if (o == "Ts") {
        gs.rise = op.num(0);
      } else if (o == "Tr") {
        gs.render = int(op.num(0));
      } else if (o == "Td" || o == "TD") {
        if (o == "TD") gs.tl = -op.num(1);
        tlm = Matrix::translate(op.num(0), op.num(1)) * tlm;
        tm = tlm;
      } else if (o == "Tm") {
        tlm = tm = matrix_of(op);
      } else if (o == "T*") {
        tlm = Matrix::translate(0, -gs.tl) * tlm;
        tm = tlm;
      } else if (o == "Tj" || o == "'" || o == "\"") {
        if (o != "Tj") {
          if (o == "\"") {
            gs.tw = op.num(0);
            gs.tc = op.num(1);
          }
          tlm = Matrix::translate(0, -gs.tl) * tlm;
          tm = tlm;
        }
        if (!op.args.empty() && op.args.back().kind == Operand::String)
          show(si, int(k), 0, op.args.back().str, gs, tm);
      } else if (o == "TJ") {
        if (op.args.empty() || op.args[0].kind != Operand::Array) continue;
        int part = 0;
        for (const Operand &e : op.args[0].items) {
          if (e.kind == Operand::String) {
            show(si, int(k), part, e.str, gs, tm);
          } else if (e.kind == Operand::Number) {
            tm = Matrix::translate(-e.num / 1000.0 * gs.tfs * gs.th, 0) * tm;
          }
          ++part;
        }
      } else if (o == "g" || o == "rg" || o == "k") {
        int c = o == "g" ? 1 : o == "rg" ? 3 : 4;
        gs.fill.n = c;
        for (int j = 0; j < c; ++j) gs.fill.v[j] = std::clamp(op.num(size_t(j)), 0.0, 1.0);
      } else if (o == "cs") {
        int c = op.args.empty() ? 0 : components(res, op.args[0]);
        gs.fill.n = c;
        for (int j = 0; j < 4; ++j) gs.fill.v[j] = c == 4 && j == 3 ? 1 : 0;
      } else if (o == "sc" || o == "scn") {
        if (gs.fill.n > 0 && int(op.args.size()) >= gs.fill.n)
          for (int j = 0; j < gs.fill.n; ++j) gs.fill.v[j] = std::clamp(op.num(size_t(j)), 0.0, 1.0);
      } else if (o == "gs") {
        if (!op.args.empty() && op.args[0].kind == Operand::Name && res.isDictionary()) {
          QPDFObjectHandle ext = res.getKey("/ExtGState");
          QPDFObjectHandle g = ext.isDictionary() ? ext.getKey("/" + op.args[0].str) : QPDFObjectHandle();
          if (g.isDictionary() && g.getKey("/ca").isNumber()) gs.fill.alpha = g.getKey("/ca").getNumericValue();
        }
      } else if (o == "Do") {
        if (op.args.empty() || op.args[0].kind != Operand::Name || !res.isDictionary()) continue;
        std::string name = op.args[0].str;
        QPDFObjectHandle xo = res.getKey("/XObject");
        QPDFObjectHandle x = xo.isDictionary() ? xo.getKey("/" + name) : QPDFObjectHandle();
        if (!x.isStream()) continue;
        QPDFObjectHandle dict = x.getDict();
        QPDFObjectHandle sub = dict.getKey("/Subtype");
        if (sub.isName() && sub.getName() == "/Image") {
          ImageItem im;
          im.stream = si;
          im.op = int(k);
          im.ctm = gs.ctm;
          im.name = name;
          im.xobject = x;
          im.editable = si == 0;
          im.pixels_w = dict.getKey("/Width").isInteger() ? dict.getKey("/Width").getIntValueAsInt() : 0;
          im.pixels_h = dict.getKey("/Height").isInteger() ? dict.getKey("/Height").getIntValueAsInt() : 0;
          Matrix m = gs.ctm * to_display;
          im.box = bounds({m.apply(0, 0), m.apply(1, 0), m.apply(0, 1), m.apply(1, 1)});
          L.images.push_back(im);
        } else if (sub.isName() && sub.getName() == "/Form") {
          PageLayout::Stream fs;
          fs.object = x;
          QPDFObjectHandle r = dict.getKey("/Resources");
          fs.resources = r.isDictionary() ? r : res;
          fs.data = stream_data(x);
          fs.ops = parse_content(fs.data);
          fs.parent = si;
          fs.parent_op = int(k);
          fs.name = name;
          Matrix fm = matrix_of(dict.getKey("/Matrix")) * gs.ctm;
          L.streams.push_back(std::move(fs));
          int child = int(L.streams.size()) - 1;
          Paint keep = gs.fill;
          run(child, fm, depth + 1);
          gs.fill = keep;
        }
      } else if (o == "BI") {
        ImageItem im;
        im.stream = si;
        im.op = int(k);
        im.ctm = gs.ctm;
        im.inline_image = true;
        im.editable = si == 0;
        Matrix m = gs.ctm * to_display;
        im.box = bounds({m.apply(0, 0), m.apply(1, 0), m.apply(0, 1), m.apply(1, 1)});
        L.images.push_back(im);
      } else if (o == "m" || o == "l") {
        if (path_first < 0) path_first = int(k);
        cur = Point{op.num(0), op.num(1)};
        extend(cur.x, cur.y);
      } else if (o == "c" || o == "v" || o == "y") {
        if (path_first < 0) path_first = int(k);
        for (size_t j = 0; j + 1 < op.args.size(); j += 2) extend(op.num(j), op.num(j + 1));
      } else if (o == "re") {
        if (path_first < 0) path_first = int(k);
        double x = op.num(0), y = op.num(1), w = op.num(2), h = op.num(3);
        extend(x, y);
        extend(x + w, y + h);
        extend(x + w, y);
        extend(x, y + h);
      } else if (o == "S" || o == "s" || o == "f" || o == "F" || o == "f*" || o == "B" || o == "B*" || o == "b" ||
                 o == "b*" || o == "n") {
        if (o != "n" && path_first >= 0 && path.x2 >= path.x1)
          L.paths.push_back(PathItem{si, path_first, int(k), path});
        path = Box{1e18, 1e18, -1e18, -1e18};
        path_first = -1;
      } else if (o == "sh") {
        Box page{0, 0, L.geometry.width(), L.geometry.height()};
        L.paths.push_back(PathItem{si, int(k), int(k), page});
      }
    }
  }
};

}  // namespace

std::unique_ptr<PageLayout> PageLayout::read(PdfFile &pdf, int page) {
  std::unique_ptr<PageLayout> L(new PageLayout());
  L->page = page;
  QPDFObjectHandle pg = pdf.page(page);
  if (!pg.isDictionary()) {
    L->complete = false;
    return L;
  }
  try {
    L->geometry = pdf.geometry(page);
    Stream s;
    s.resources = QPDFPageObjectHelper(pg).getAttribute("/Resources", false);
    s.data = page_content(pg);
    s.ops = parse_content(s.data);
    L->streams.push_back(std::move(s));
    Interp in(pdf, *L);
    in.run(0, Matrix(), 0);
  } catch (std::exception &) {
    L->complete = false;
  }
  L->build_lines();
  L->build_blocks();
  return L;
}

void PageLayout::build_lines() {
  lines.clear();
  int cur = -1;
  for (int i = 0; i < int(glyphs.size()); ++i) {
    Glyph &g = glyphs[size_t(i)];
    if (!g.horizontal || g.render == 3 || g.render == 7 || g.size() < 0.5) continue;
    double sz = g.size();
    if (cur >= 0) {
      Line &l = lines[size_t(cur)];
      const Glyph &p = glyphs[size_t(l.glyphs.back())];
      double gap = g.origin.x - p.end.x;
      double base = std::fabs(g.origin.y - p.origin.y);
      double ref = std::max(sz, p.size());
      bool same = base < 0.3 * ref && gap > -0.5 * ref && gap < 2.5 * ref && sz / p.size() < 1.7 && p.size() / sz < 1.7;
      if (same) {
        bool spaced = gap > 0.15 * ref && !p.space && !g.space && g.text != " " && p.text != " ";
        l.space_after.back() = spaced;
        l.glyphs.push_back(i);
        l.space_after.push_back(false);
        g.line = cur;
        continue;
      }
    }
    Line l;
    l.glyphs.push_back(i);
    l.space_after.push_back(false);
    lines.push_back(l);
    cur = int(lines.size()) - 1;
    g.line = cur;
  }
  // Fragments of one line drawn apart (word by word, or out of order).
  for (size_t a = 0; a < lines.size(); ++a) {
    for (size_t b = 0; b < lines.size(); ++b) {
      if (a == b || lines[a].glyphs.empty() || lines[b].glyphs.empty()) continue;
      const Glyph &ea = glyphs[size_t(lines[a].glyphs.back())];
      const Glyph &sb = glyphs[size_t(lines[b].glyphs.front())];
      double ref = std::max(ea.size(), sb.size());
      double gap = sb.origin.x - ea.end.x;
      if (std::fabs(ea.origin.y - sb.origin.y) < 0.2 * ref && gap > -0.3 * ref && gap < 1.2 * ref &&
          ea.size() / sb.size() < 1.4 && sb.size() / ea.size() < 1.4) {
        lines[a].space_after.back() = gap > 0.15 * ref && !ea.space && !sb.space;
        for (size_t k = 0; k < lines[b].glyphs.size(); ++k) {
          lines[a].glyphs.push_back(lines[b].glyphs[k]);
          lines[a].space_after.push_back(lines[b].space_after[k]);
        }
        lines[b].glyphs.clear();
        b = size_t(-1);  // look again from the start for this line
        if (lines[a].glyphs.size() > 100000) break;
      }
    }
  }
  std::vector<Line> kept;
  for (auto &l : lines)
    if (!l.glyphs.empty()) kept.push_back(std::move(l));
  lines = std::move(kept);
  for (size_t k = 0; k < lines.size(); ++k) {
    Line &l = lines[k];
    l.box = Box{1e18, 1e18, -1e18, -1e18};
    double size = 0;
    for (int gi : l.glyphs) {
      Glyph &g = glyphs[size_t(gi)];
      g.line = int(k);
      l.box.x1 = std::min(l.box.x1, std::min(g.box.x1, g.origin.x));
      l.box.y1 = std::min(l.box.y1, g.box.y1);
      l.box.x2 = std::max(l.box.x2, std::max(g.box.x2, g.end.x));
      l.box.y2 = std::max(l.box.y2, g.box.y2);
      size = std::max(size, g.size());
    }
    l.size = size;
  }
}

void PageLayout::build_blocks() {
  blocks.clear();
  std::vector<int> order(lines.size());
  for (size_t k = 0; k < order.size(); ++k) order[k] = int(k);
  auto baseline = [&](int li) { return glyphs[size_t(lines[size_t(li)].glyphs.front())].origin.y; };
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    double ya = baseline(a), yb = baseline(b);
    if (std::fabs(ya - yb) > 1) return ya < yb;
    return lines[size_t(a)].box.x1 < lines[size_t(b)].box.x1;
  });
  for (int li : order) {
    Line &L = lines[size_t(li)];
    double y = baseline(li);
    int best = -1;
    double best_d = 1e18;
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
      Block &B = blocks[bi];
      int last = B.lines.back();
      const Line &P = lines[size_t(last)];
      double d = y - baseline(last);
      double s = std::max(L.size, P.size);
      if (d < 0.6 * s || d > 2.0 * s) continue;
      if (B.lines.size() >= 2 && std::fabs(d - B.leading) > 0.2 * s) continue;
      if (L.size / P.size > 1.34 || P.size / L.size > 1.34) continue;
      double overlap = std::min(L.box.x2, B.box.x2) - std::max(L.box.x1, B.box.x1);
      double narrow = std::min(L.box.w(), B.box.w());
      if (overlap < 0.3 * narrow) continue;
      if (d < best_d) {
        best_d = d;
        best = int(bi);
      }
    }
    if (best < 0) {
      Block B;
      B.lines.push_back(li);
      B.box = L.box;
      B.leading = L.size * 1.2;
      blocks.push_back(B);
      L.block = int(blocks.size()) - 1;
    } else {
      Block &B = blocks[size_t(best)];
      if (B.lines.size() == 1) B.leading = best_d;
      B.lines.push_back(li);
      B.box = B.box.united(L.box);
      L.block = best;
    }
  }
  for (Block &B : blocks) {
    B.editable = true;
    for (int li : B.lines)
      for (int gi : lines[size_t(li)].glyphs)
        if (!glyphs[size_t(gi)].editable) B.editable = false;
    if (B.lines.size() < 2) continue;
    double maxr = -1e18, minl = 1e18;
    for (int li : B.lines) {
      maxr = std::max(maxr, lines[size_t(li)].box.x2);
      minl = std::min(minl, lines[size_t(li)].box.x1);
    }
    bool left = true, right = true, center = true, full = true;
    double c0 = (lines[size_t(B.lines[0])].box.x1 + lines[size_t(B.lines[0])].box.x2) / 2;
    for (size_t k = 0; k < B.lines.size(); ++k) {
      const Line &l = lines[size_t(B.lines[k])];
      if (std::fabs(l.box.x1 - minl) > 1.5 && !(k == 0 && l.box.x1 > minl)) left = false;
      if (std::fabs(l.box.x2 - maxr) > 1.5) {
        right = false;
        if (k + 1 < B.lines.size()) full = false;
      }
      if (std::fabs((l.box.x1 + l.box.x2) / 2 - c0) > 1.5) center = false;
    }
    B.align = left && full && !right && B.lines.size() > 2 ? 3 : left ? 0 : right ? 2 : center ? 1 : 0;
    if (left && full && B.lines.size() == 2 && !right) B.align = 0;
  }
}

int PageLayout::block_at(double x, double y, double slack) const {
  int best = -1;
  double area = 1e18;
  for (size_t k = 0; k < blocks.size(); ++k) {
    const Box &b = blocks[k].box;
    if (!b.contains(x, y, slack)) continue;
    double a = b.w() * b.h();
    if (a < area) {
      area = a;
      best = int(k);
    }
  }
  return best;
}

int PageLayout::image_at(double x, double y) const {
  for (int k = int(images.size()) - 1; k >= 0; --k)
    if (images[size_t(k)].box.contains(x, y)) return k;
  return -1;
}

bool same_content(const PageLayout &a, const PageLayout &b) {
  if (a.page != b.page || a.streams.size() != b.streams.size() || a.glyphs.size() != b.glyphs.size() ||
      a.images.size() != b.images.size() || a.fonts.size() != b.fonts.size() || a.blocks.size() != b.blocks.size())
    return false;
  for (size_t k = 0; k < a.streams.size(); ++k)
    if (a.streams[k].data != b.streams[k].data || a.streams[k].name != b.streams[k].name) return false;
  return true;
}

std::string PageLayout::block_text(int block) const {
  std::string out;
  if (block < 0 || block >= int(blocks.size())) return out;
  for (size_t k = 0; k < blocks[size_t(block)].lines.size(); ++k) {
    if (k) out += "\n";
    const Line &l = lines[size_t(blocks[size_t(block)].lines[k])];
    for (size_t j = 0; j < l.glyphs.size(); ++j) {
      const Glyph &g = glyphs[size_t(l.glyphs[j])];
      out += g.text.empty() ? "\xEF\xBF\xBD" : g.text;
      if (l.space_after[j]) out += " ";
    }
  }
  return out;
}

}  // namespace xanara
