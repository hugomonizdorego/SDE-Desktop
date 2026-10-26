// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Document fonts (decoding, widths, Unicode) and installed fonts
// (fontconfig matching, HarfBuzz shaping, hb-subset embedding); see
// fonts.h. FreeType opens embedded font programs only to check whether a
// glyph is really there before Xanara reuses a document's font for new
// text.
#include "fonts.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ot.h>
#include <hb-subset.h>
#include <hb.h>
#include <qpdf/Buffer.hh>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <tuple>

#include "content.h"
#include "i18n.h"
#include "util.h"

using namespace sde;

namespace xanara {

// --------------------------------------------------------------- unicode

std::string utf8(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  return out;
}

std::string utf8(const std::u32string &text) {
  std::string out;
  for (char32_t c : text) out += utf8(c);
  return out;
}

std::u32string utf32(const std::string &s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    char32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n = 4;
    } else {
      out += 0xFFFD;
      ++i;
      continue;
    }
    if (i + size_t(n) > s.size()) {
      out += 0xFFFD;
      break;
    }
    for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + size_t(k)] & 0x3F);
    out += cp;
    i += size_t(n);
  }
  return out;
}

static std::string utf16be_to_utf8(const std::string &b) {
  if (b.size() == 1) return utf8(char32_t((unsigned char)b[0]));
  std::string out;
  for (size_t i = 0; i + 1 < b.size(); i += 2) {
    char32_t u = char32_t(((unsigned char)b[i] << 8) | (unsigned char)b[i + 1]);
    if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
      char32_t lo = char32_t(((unsigned char)b[i + 2] << 8) | (unsigned char)b[i + 3]);
      if (lo >= 0xDC00 && lo < 0xE000) {
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    out += utf8(u);
  }
  return out;
}

static std::string utf16be(const std::string &text) {
  std::string out;
  for (char32_t c : utf32(text)) {
    if (c >= 0x10000) {
      char32_t v = c - 0x10000;
      unsigned hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
      out += char(hi >> 8);
      out += char(hi & 0xFF);
      out += char(lo >> 8);
      out += char(lo & 0xFF);
    } else {
      out += char(c >> 8);
      out += char(c & 0xFF);
    }
  }
  return out;
}

static bool hex_digits(const std::string &s, size_t from, size_t n, unsigned long &v) {
  if (from + n > s.size()) return false;
  v = 0;
  for (size_t k = from; k < from + n; ++k) {
    char c = s[k];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= unsigned(c - '0');
    else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
    else return false;  // the AGL wants upper case
  }
  return true;
}

static std::string glyph_text(const std::string &name0) {
  std::string name = name0.substr(0, name0.find('.'));
  if (name.empty()) return "";
  if (name.find('_') != std::string::npos) {  // ligatures: f_f_i
    std::string out;
    for (const auto &part : split(name, '_')) {
      std::string t = glyph_text(part);
      if (t.empty()) return "";
      out += t;
    }
    return out;
  }
  unsigned long v;
  if (name.size() >= 7 && name.compare(0, 3, "uni") == 0 && (name.size() - 3) % 4 == 0) {
    std::string out;
    for (size_t k = 3; k < name.size(); k += 4) {
      if (!hex_digits(name, k, 4, v) || (v >= 0xD800 && v < 0xE000)) return "";
      out += utf8(char32_t(v));
    }
    return out;
  }
  if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u' && hex_digits(name, 1, name.size() - 1, v) &&
      v < 0x110000 && !(v >= 0xD800 && v < 0xE000))
    return utf8(char32_t(v));
  const fontdata::GlyphName *b = fontdata::kGlyphNames, *e = b + fontdata::kGlyphNameCount;
  auto it = std::lower_bound(b, e, name, [](const fontdata::GlyphName &g, const std::string &n) { return n.compare(g.name) > 0; });
  if (it != e && name == it->name) return utf8(char32_t(it->unicode));
  return "";
}

char32_t glyph_unicode(const std::string &name) {
  std::u32string t = utf32(glyph_text(name));
  return t.size() == 1 ? t[0] : 0;
}

// ------------------------------------------------------------------ CMap

static unsigned code_value(const std::string &bytes) {
  unsigned v = 0;
  for (size_t k = 0; k < bytes.size() && k < 4; ++k) v = (v << 8) | (unsigned char)bytes[k];
  return v;
}

static std::string predefined_cmap(const std::string &name) {
  if (name.find('/') != std::string::npos || name.empty()) return "";
  static const char *roots[] = {"/usr/share/poppler/cMap", "/usr/local/share/poppler/cMap"};
  for (const char *root : roots)
    for (const auto &dir : list_dir(root)) {
      std::string path = std::string(root) + "/" + dir + "/" + name, text;
      if (file_exists(path) && read_file(path, text)) return text;
    }
  return "";
}

bool CMap::load(const std::string &name, int depth) {
  if (name == "Identity-H" || name == "Identity-V") {
    identity = true;
    vertical = name == "Identity-V";
    return true;
  }
  if (depth > 4) return false;
  std::string text = predefined_cmap(name);
  if (text.empty()) return false;
  if (name.size() > 2 && name.compare(name.size() - 2, 2, "-V") == 0) vertical = true;
  return parse(text, depth);
}

bool CMap::parse(const std::string &text, int depth) {
  for (const Op &op : parse_content(text)) {
    const auto &a = op.args;
    if (op.name == "usecmap" && !a.empty() && a.back().kind == Operand::Name) {
      load(a.back().str, depth + 1);
    } else if (op.name == "endcodespacerange") {
      for (size_t k = 0; k + 1 < a.size(); k += 2) {
        const std::string &lo = a[k].str, &hi = a[k + 1].str;
        if (lo.empty() || lo.size() != hi.size() || lo.size() > 4) continue;
        Space s{int(lo.size()), {0}, {0}};
        for (size_t b = 0; b < lo.size(); ++b) {
          s.lo[b] = (unsigned char)lo[b];
          s.hi[b] = (unsigned char)hi[b];
        }
        spaces_.push_back(s);
      }
    } else if (op.name == "endcidrange") {
      for (size_t k = 0; k + 2 < a.size(); k += 3)
        if (a[k + 2].is_number())
          ranges_.push_back(Range{code_value(a[k].str), code_value(a[k + 1].str), unsigned(a[k + 2].num)});
    } else if (op.name == "endcidchar") {
      for (size_t k = 0; k + 1 < a.size(); k += 2)
        if (a[k + 1].is_number()) chars_[code_value(a[k].str)] = unsigned(a[k + 1].num);
    } else if (op.name == "def" && a.size() == 2 && a[0].is_name("WMode") && a[1].is_number()) {
      vertical = a[1].num == 1;
    }
  }
  return !spaces_.empty() || identity;
}

int CMap::next(const std::string &s, size_t i, unsigned &code) const {
  if (identity || spaces_.empty()) {
    if (i + 1 < s.size()) {
      code = ((unsigned char)s[i] << 8) | (unsigned char)s[i + 1];
      return 2;
    }
    code = (unsigned char)s[i];
    return 1;
  }
  for (int n = 1; n <= 4; ++n) {
    if (i + size_t(n) > s.size()) break;
    for (const Space &sp : spaces_) {
      if (sp.bytes != n) continue;
      bool in = true;
      for (int b = 0; b < n && in; ++b) {
        unsigned char c = (unsigned char)s[i + size_t(b)];
        in = c >= sp.lo[b] && c <= sp.hi[b];
      }
      if (in) {
        code = code_value(s.substr(i, size_t(n)));
        return n;
      }
    }
  }
  // Not in any code space: as long as the shortest range.
  int n = 4;
  for (const Space &sp : spaces_) n = std::min(n, sp.bytes);
  n = std::max(1, std::min<int>(n, int(s.size() - i)));
  code = code_value(s.substr(i, size_t(n)));
  return n;
}

unsigned CMap::cid(unsigned code) const {
  if (identity) return code;
  auto it = chars_.find(code);
  if (it != chars_.end()) return it->second;
  for (const Range &r : ranges_)
    if (code >= r.lo && code <= r.hi) return r.cid + (code - r.lo);
  return 0;
}

// ------------------------------------------------------------ ToUnicode

bool UnicodeMap::parse(const std::string &text) {
  for (const Op &op : parse_content(text)) {
    const auto &a = op.args;
    if (op.name == "endbfchar") {
      for (size_t k = 0; k + 1 < a.size(); k += 2) {
        if (a[k].kind != Operand::String) continue;
        std::string t = a[k + 1].kind == Operand::String ? utf16be_to_utf8(a[k + 1].str)
                        : a[k + 1].kind == Operand::Name ? glyph_text(a[k + 1].str)
                                                         : "";
        if (!t.empty()) chars_[code_value(a[k].str)] = t;
      }
    } else if (op.name == "endbfrange") {
      for (size_t k = 0; k + 2 < a.size(); k += 3) {
        if (a[k].kind != Operand::String || a[k + 1].kind != Operand::String) continue;
        Range r;
        r.lo = code_value(a[k].str);
        r.hi = code_value(a[k + 1].str);
        if (r.hi < r.lo || r.hi - r.lo > 0x10000) continue;
        if (a[k + 2].kind == Operand::String) {
          const std::string &d = a[k + 2].str;
          for (size_t b = 0; b + 1 < d.size(); b += 2) r.start += char16_t(((unsigned char)d[b] << 8) | (unsigned char)d[b + 1]);
          if (d.size() == 1) r.start += char16_t((unsigned char)d[0]);
          if (r.start.empty()) continue;
        } else if (a[k + 2].kind == Operand::Array) {
          for (const auto &it : a[k + 2].items) r.list.push_back(it.kind == Operand::String ? utf16be_to_utf8(it.str) : "");
        } else {
          continue;
        }
        ranges_.push_back(std::move(r));
      }
    }
  }
  return !empty();
}

static std::string u16_to_utf8(const std::u16string &u) {
  std::string b;
  for (char16_t c : u) {
    b += char(c >> 8);
    b += char(c & 0xFF);
  }
  return utf16be_to_utf8(b);
}

bool UnicodeMap::lookup(unsigned code, std::string &out) const {
  auto it = chars_.find(code);
  if (it != chars_.end()) {
    out = it->second;
    return true;
  }
  for (const Range &r : ranges_) {
    if (code < r.lo || code > r.hi) continue;
    if (!r.list.empty()) {
      size_t k = code - r.lo;
      if (k >= r.list.size() || r.list[k].empty()) return false;
      out = r.list[k];
      return true;
    }
    std::u16string u = r.start;
    u.back() = char16_t(u.back() + (code - r.lo));
    out = u16_to_utf8(u);
    return !out.empty();
  }
  return false;
}

void UnicodeMap::each(const std::function<void(unsigned, const std::string &)> &fn) const {
  for (const auto &kv : chars_) fn(kv.first, kv.second);
  for (const Range &r : ranges_)
    for (unsigned c = r.lo; c <= r.hi; ++c) {
      std::string t;
      if (chars_.count(c) == 0 && lookup(c, t)) fn(c, t);
      if (c == 0xFFFFFFFFu) break;
    }
}

// -------------------------------------------------------------- FreeType

namespace {

std::mutex &ft_mutex() {
  static std::mutex m;
  return m;
}

FT_Library ft_library() {
  static FT_Library lib = nullptr;
  if (!lib && FT_Init_FreeType(&lib) != 0) lib = nullptr;
  return lib;
}

std::string stream_bytes(QPDFObjectHandle s) {
  if (!s.isStream()) return "";
  try {
    std::shared_ptr<Buffer> b = s.getStreamData(qpdf_dl_generalized);
    return std::string(reinterpret_cast<const char *>(b->getBuffer()), b->getSize());
  } catch (std::exception &) {
    return "";
  }
}

std::string name_of(QPDFObjectHandle o) { return o.isName() ? o.getName().substr(1) : ""; }

double number_of(QPDFObjectHandle o, double fallback) { return o.isNumber() ? o.getNumericValue() : fallback; }

// "ABCDEF+Arial-BoldMT" -> "Arial-BoldMT"
std::string strip_subset(const std::string &n) {
  if (n.size() > 7 && n[6] == '+' &&
      std::all_of(n.begin(), n.begin() + 6, [](char c) { return c >= 'A' && c <= 'Z'; }))
    return n.substr(7);
  return n;
}

bool contains_ci(const std::string &hay, const char *needle) {
  return lower(hay).find(lower(needle)) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------- PdfFont

PdfFont::~PdfFont() {
  if (face_) {
    std::lock_guard<std::mutex> lock(ft_mutex());
    FT_Done_Face(static_cast<FT_Face>(face_));
  }
}

std::shared_ptr<PdfFont> PdfFont::load(QPDFObjectHandle dict) {
  std::shared_ptr<PdfFont> f(new PdfFont());
  if (!dict.isDictionary()) {
    f->known_widths = false;
    return f;
  }
  f->dict = dict;
  try {
    std::string subtype = name_of(dict.getKey("/Subtype"));
    f->name = strip_subset(name_of(dict.getKey("/BaseFont")));
    // The family: "Arial-BoldItalicMT" -> "Arial", "TimesNewRomanPSMT" ->
    // "TimesNewRoman" (fontconfig ignores the missing blanks).
    std::string fam = f->name.substr(0, f->name.find_first_of("-,"));
    for (const char *suffix : {"PSMT", "MT", "PS"})
      if (fam.size() > strlen(suffix) + 2 && ends_with(fam, suffix)) {
        fam.resize(fam.size() - strlen(suffix));
        break;
      }
    f->family = fam;
    std::string style = f->name.size() > fam.size() ? f->name.substr(fam.size()) : "";
    for (const char *b : {"Bold", "Black", "Heavy", "Semibold", "SemiBold", "Demi"})
      if (style.find(b) != std::string::npos) f->bold = true;
    for (const char *it : {"Italic", "Oblique", "-It", ",It"})
      if (style.find(it) != std::string::npos) f->italic = true;
    if (subtype == "Type0") {
      f->kind = Composite;
      f->load_composite();
    } else {
      f->kind = subtype == "Type3" ? Type3 : Simple;
      f->load_simple();
    }
    QPDFObjectHandle tu = dict.getKey("/ToUnicode");
    if (tu.isStream()) f->to_unicode_.parse(stream_bytes(tu));
    if (f->family.empty()) f->family = f->serif ? "Serif" : "Sans";
    for (const char *s : {"Times", "Roman", "Serif", "Georgia", "Cambria", "Garamond", "Palatino", "Book Antiqua",
                          "Minion", "Caladea", "Century", "Bookman"})
      if (contains_ci(f->name, s) && !contains_ci(f->name, "Sans")) f->serif = true;
    for (const char *m : {"Courier", "Mono", "Consolas", "Menlo", "Code"})
      if (contains_ci(f->name, m)) f->fixed = true;
  } catch (std::exception &) {
    f->known_widths = false;
  }
  return f;
}

void PdfFont::read_descriptor(QPDFObjectHandle fd) {
  if (!fd.isDictionary()) return;
  int flags = fd.getKey("/Flags").isInteger() ? fd.getKey("/Flags").getIntValueAsInt() : 0;
  fixed = flags & 1;
  serif = flags & 2;
  symbolic = (flags & 4) && !(flags & 32);
  if (flags & 64) italic = true;
  if (flags & (1 << 18)) bold = true;
  if (number_of(fd.getKey("/FontWeight"), 400) >= 600) bold = true;
  if (std::fabs(number_of(fd.getKey("/ItalicAngle"), 0)) > 2) italic = true;
  double a = number_of(fd.getKey("/Ascent"), 0), d = number_of(fd.getKey("/Descent"), 0);
  if (a > 0) ascent = std::min(a, 1500.0) / 1000.0;
  if (d < 0) descent = std::max(d, -1000.0) / 1000.0;
  missing_width_ = number_of(fd.getKey("/MissingWidth"), 0);
  QPDFObjectHandle fam = fd.getKey("/FontFamily");
  if (fam.isString() && !fam.getUTF8Value().empty()) family = fam.getUTF8Value();
  for (const char *key : {"/FontFile2", "/FontFile3", "/FontFile"}) {
    QPDFObjectHandle ff = fd.getKey(key);
    if (ff.isStream()) {
      embedded = true;
      program_ = stream_bytes(ff);
      break;
    }
  }
}

static const uint16_t *base14_metrics(const std::string &n0, double &fixed_width) {
  std::string n = n0;
  n.erase(std::remove(n.begin(), n.end(), ' '), n.end());
  bool bold = n.find("Bold") != std::string::npos, italic = n.find("Italic") != std::string::npos ||
                                                             n.find("Oblique") != std::string::npos;
  fixed_width = 0;
  if (starts_with(n, "Courier")) {
    fixed_width = 600;
    return nullptr;
  }
  if (starts_with(n, "Helvetica") || starts_with(n, "Arial"))
    return bold ? fontdata::kHelveticaBold : fontdata::kHelvetica;
  if (starts_with(n, "Times"))
    return bold ? (italic ? fontdata::kTimesBoldItalic : fontdata::kTimesBold)
                : (italic ? fontdata::kTimesItalic : fontdata::kTimesRoman);
  return nullptr;
}

static double base14_width(const uint16_t *table, char32_t cp) {
  const uint16_t *b = fontdata::kBase14Chars, *e = b + fontdata::kBase14CharCount;
  const uint16_t *it = std::lower_bound(b, e, uint16_t(cp));
  if (cp > 0xFFFF || it == e || *it != cp) return -1;
  return table[it - b];
}

void PdfFont::load_simple() {
  read_descriptor(dict.getKey("/FontDescriptor"));
  if (kind == Type3) {
    QPDFObjectHandle m = dict.getKey("/FontMatrix");
    if (m.isArray() && m.getArrayNItems() == 6) {
      double v[6];
      for (int k = 0; k < 6; ++k) v[k] = number_of(m.getArrayItem(k), 0);
      matrix = Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
    }
    QPDFObjectHandle bb = dict.getKey("/FontBBox");
    if (bb.isArray() && bb.getArrayNItems() == 4) {
      double y1 = number_of(bb.getArrayItem(1), 0) * matrix.d, y2 = number_of(bb.getArrayItem(3), 0) * matrix.d;
      if (y2 > y1 && y2 - y1 < 4) {
        ascent = std::max(y1, y2);
        descent = std::min(y1, y2);
      }
    }
    embedded = true;
  }
  QPDFObjectHandle widths = dict.getKey("/Widths");
  int first = dict.getKey("/FirstChar").isInteger() ? dict.getKey("/FirstChar").getIntValueAsInt() : 0;
  if (widths.isArray()) {
    int n = widths.getArrayNItems();
    for (int k = 0; k < n; ++k) {
      int code = first + k;
      if (code < 0 || code > 255) continue;
      widths_[code] = number_of(widths.getArrayItem(k), 0);
      have_width_[code] = true;
    }
  } else if (kind == Simple) {
    metrics_ = base14_metrics(name, fixed_width_);
    if (!metrics_ && fixed_width_ == 0) known_widths = false;  // the font program's widths: unknown here
  } else {
    known_widths = false;
  }
  // The encoding: a base encoding and /Differences.
  QPDFObjectHandle enc = dict.getKey("/Encoding");
  auto table = [](const std::string &n) -> const uint16_t * {
    if (n == "WinAnsiEncoding") return fontdata::kWinAnsiEncoding;
    if (n == "MacRomanEncoding") return fontdata::kMacRomanEncoding;
    if (n == "StandardEncoding") return fontdata::kStandardEncoding;
    return nullptr;
  };
  if (enc.isName()) base_ = table(name_of(enc));
  else if (enc.isDictionary()) {
    base_ = table(name_of(enc.getKey("/BaseEncoding")));
    QPDFObjectHandle diff = enc.getKey("/Differences");
    if (diff.isArray()) {
      int code = 0;
      for (auto it : diff.getArrayAsVector()) {
        if (it.isInteger()) code = it.getIntValueAsInt();
        else if (it.isName() && code >= 0 && code < 256) names_[code++] = name_of(it);
      }
    }
  }
  bool symbol_font = starts_with(name, "Symbol") || starts_with(name, "ZapfDingbats") || starts_with(name, "Wingdings");
  if (!base_ && !symbol_font && (!symbolic || !embedded) && kind == Simple) base_ = fontdata::kStandardEncoding;
  if (symbol_font) symbolic = true;
}

void PdfFont::load_composite() {
  QPDFObjectHandle enc = dict.getKey("/Encoding");
  if (enc.isName()) {
    if (!cmap_.load(name_of(enc))) known_widths = false;
  } else if (enc.isStream()) {
    QPDFObjectHandle use = enc.getDict().getKey("/UseCMap");
    if (use.isName()) cmap_.load(name_of(use));
    if (!cmap_.parse(stream_bytes(enc))) known_widths = false;
  } else {
    known_widths = false;
  }
  vertical = cmap_.vertical;
  encode_bytes_ = 2;
  QPDFObjectHandle kids = dict.getKey("/DescendantFonts");
  QPDFObjectHandle cid = kids.isArray() && kids.getArrayNItems() > 0 ? kids.getArrayItem(0) : QPDFObjectHandle();
  if (!cid.isDictionary()) {
    known_widths = false;
    return;
  }
  read_descriptor(cid.getKey("/FontDescriptor"));
  if (name.empty()) name = strip_subset(name_of(cid.getKey("/BaseFont")));
  QPDFObjectHandle info = cid.getKey("/CIDSystemInfo");
  if (info.isDictionary()) {
    QPDFObjectHandle reg = info.getKey("/Registry"), ord = info.getKey("/Ordering");
    if (reg.isString() && ord.isString()) collection_ = reg.getUTF8Value() + "-" + ord.getUTF8Value();
  }
  default_width_ = number_of(cid.getKey("/DW"), 1000) / 1000.0;
  QPDFObjectHandle w = cid.getKey("/W");
  if (w.isArray()) {
    std::vector<QPDFObjectHandle> v = w.getArrayAsVector();
    for (size_t k = 0; k < v.size();) {
      if (!v[k].isNumber()) break;
      unsigned c1 = unsigned(v[k].getNumericValue());
      if (k + 1 < v.size() && v[k + 1].isArray()) {
        unsigned c = c1;
        for (auto x : v[k + 1].getArrayAsVector()) cid_widths_[c++] = number_of(x, 0) / 1000.0;
        k += 2;
      } else if (k + 2 < v.size()) {
        unsigned c2 = unsigned(number_of(v[k + 1], c1));
        double wv = number_of(v[k + 2], 0) / 1000.0;
        for (unsigned c = c1; c <= c2 && c - c1 < 0x10000; ++c) cid_widths_[c] = wv;
        k += 3;
      } else {
        break;
      }
    }
  }
  QPDFObjectHandle map = cid.getKey("/CIDToGIDMap");
  if (map.isStream()) {
    std::string b = stream_bytes(map);
    for (size_t k = 0; k + 1 < b.size(); k += 2)
      cid_to_gid_.push_back(uint16_t(((unsigned char)b[k] << 8) | (unsigned char)b[k + 1]));
  }
}

unsigned PdfFont::next(const std::string &s, size_t i, int &bytes) const {
  if (kind == Composite) {
    unsigned code = 0;
    bytes = cmap_.next(s, i, code);
    return code;
  }
  bytes = 1;
  return (unsigned char)s[i];
}

double PdfFont::width(unsigned code) const {
  if (kind == Composite) {
    auto it = cid_widths_.find(cmap_.cid(code));
    return it != cid_widths_.end() ? it->second : default_width_;
  }
  double scale = kind == Type3 ? matrix.a : 0.001;
  if (code < 256 && have_width_[code]) return widths_[code] * scale;
  if (fixed_width_ > 0) return fixed_width_ * scale;
  if (metrics_ && code < 256) {
    char32_t cp = 0;
    if (!names_[code].empty()) cp = glyph_unicode(names_[code]);
    else if (base_) cp = base_[code];
    double w = cp ? base14_width(metrics_, cp) : -1;
    if (w >= 0) return w * scale;
  }
  return missing_width_ * scale;
}

std::string PdfFont::text(unsigned code) const {
  std::string s;
  if (to_unicode_.lookup(code, s)) return s;
  if (kind == Composite) {
    unsigned cid = cmap_.cid(code);
    if (!collection_.empty() && collection_ != "Adobe-Identity") {
      // poppler-data: one Unicode value per CID.
      static std::map<std::string, std::vector<char32_t>> tables;
      auto it = tables.find(collection_);
      if (it == tables.end()) {
        std::vector<char32_t> t;
        std::string text;
        for (const char *root : {"/usr/share/poppler/cidToUnicode/", "/usr/local/share/poppler/cidToUnicode/"})
          if (read_file(std::string(root) + collection_, text)) break;
        for (const auto &line : split(text, '\n', true)) t.push_back(char32_t(strtoul(line.c_str(), nullptr, 16)));
        it = tables.emplace(collection_, std::move(t)).first;
      }
      if (cid < it->second.size() && it->second[cid]) return utf8(it->second[cid]);
      return "";
    }
    // Identity: the embedded font's own cmap, glyph by glyph.
    open_program();
    if (gid_unicode_.empty()) return "";
    unsigned gid = cid_to_gid_.empty() ? cid : (cid < cid_to_gid_.size() ? cid_to_gid_[cid] : 0);
    auto g = gid_unicode_.find(gid);
    return g != gid_unicode_.end() ? utf8(g->second) : "";
  }
  if (code < 256) {
    if (!names_[code].empty()) return glyph_text(names_[code]);
    if (base_ && base_[code]) return utf8(char32_t(base_[code]));
  }
  return "";
}

void PdfFont::open_program() const {
  if (face_tried_) return;
  face_tried_ = true;
  if (program_.empty()) return;
  std::lock_guard<std::mutex> lock(ft_mutex());
  FT_Library lib = ft_library();
  FT_Face face = nullptr;
  if (!lib || FT_New_Memory_Face(lib, reinterpret_cast<const FT_Byte *>(program_.data()), FT_Long(program_.size()), 0,
                                 &face) != 0)
    return;
  face_ = face;
  if (kind == Composite && FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
    FT_UInt gid;
    FT_ULong cp = FT_Get_First_Char(face, &gid);
    while (gid != 0) {
      gid_unicode_.emplace(gid, char32_t(cp));
      cp = FT_Get_Next_Char(face, cp, &gid);
    }
  }
}

bool PdfFont::has_glyph(unsigned code, char32_t cp) const {
  if (kind == Type3) return code < 256 && have_width_[code] && widths_[code] > 0;
  if (kind == Simple && code < 256 && have_width_[code] && widths_[code] <= 0 && cp != ' ') return false;
  if (!embedded) return true;
  open_program();
  if (!face_) return true;
  std::lock_guard<std::mutex> lock(ft_mutex());
  FT_Face face = static_cast<FT_Face>(face_);
  if (kind == Composite) {
    if (!(face->face_flags & FT_FACE_FLAG_SFNT) || (face->face_flags & FT_FACE_FLAG_CID_KEYED)) return true;
    unsigned cid = cmap_.cid(code);
    unsigned gid = cid_to_gid_.empty() ? cid : (cid < cid_to_gid_.size() ? cid_to_gid_[cid] : 0);
    if (gid == 0 || long(gid) >= face->num_glyphs) return false;
    if (cp == ' ' || cp == 0xA0) return true;
    if (FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) return false;
    return face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || face->glyph->outline.n_contours > 0;
  }
  if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0) {
    FT_UInt gid = FT_Get_Char_Index(face, cp);
    if (gid) return true;
  }
  if (!names_[code].empty() && FT_HAS_GLYPH_NAMES(face))
    return FT_Get_Name_Index(face, names_[code].c_str()) != 0;
  // Symbolic TrueType fonts: (3,0) cmaps at F000 + code, or (1,0) by code.
  for (int k = 0; k < face->num_charmaps; ++k) {
    FT_CharMap cm = face->charmaps[k];
    if (cm->platform_id == 3 && cm->encoding_id == 0) {
      FT_Set_Charmap(face, cm);
      if (FT_Get_Char_Index(face, 0xF000 + code) || FT_Get_Char_Index(face, code)) return true;
    } else if (cm->platform_id == 1 && cm->encoding_id == 0) {
      FT_Set_Charmap(face, cm);
      if (FT_Get_Char_Index(face, code)) return true;
    }
  }
  return false;
}

void PdfFont::reverse_map() const {
  if (reverse_ready_) return;
  reverse_ready_ = true;
  auto take = [&](unsigned code, const std::string &t, int nbytes) {
    std::u32string u = utf32(t);
    if (u.size() != 1 || reverse_.count(u[0])) return;
    std::string bytes;
    for (int k = nbytes - 1; k >= 0; --k) bytes += char((code >> (8 * k)) & 0xFF);
    if (!has_glyph(code, u[0])) return;
    reverse_[u[0]] = bytes;
  };
  if (kind == Composite) {
    if (!known_widths || vertical) return;
    if (!to_unicode_.empty()) {
      to_unicode_.each([&](unsigned code, const std::string &t) {
        if (code <= 0xFFFF) take(code, t, 2);
      });
    } else if (cmap_.identity) {
      open_program();
      for (const auto &kv : gid_unicode_) {
        unsigned cid = kv.first;
        if (!cid_to_gid_.empty()) {
          auto it = std::find(cid_to_gid_.begin(), cid_to_gid_.end(), uint16_t(kv.first));
          if (it == cid_to_gid_.end()) continue;
          cid = unsigned(it - cid_to_gid_.begin());
        }
        if (cid <= 0xFFFF) take(cid, utf8(kv.second), 2);
      }
    }
    return;
  }
  for (unsigned code = 1; code < 256; ++code) {
    std::string t = text(code);
    if (!t.empty()) take(code, t, 1);
  }
}

bool PdfFont::encode(char32_t cp, std::string &bytes) const {
  if (!known_widths || vertical) return false;
  // Composite fonts only with a code space that encode() writes (2 bytes).
  if (kind == Composite && !cmap_.identity && !to_unicode_.empty()) {
    std::string probe(2, '\0');
    unsigned c;
    if (cmap_.next(probe, 0, c) != 2) return false;
  }
  reverse_map();
  auto it = reverse_.find(cp);
  if (it == reverse_.end()) return false;
  bytes = it->second;
  return true;
}

// ----------------------------------------------------------- SystemFont

bool FontRequest::operator<(const FontRequest &o) const {
  return std::tie(family, bold, italic, serif, fixed) < std::tie(o.family, o.bold, o.italic, o.serif, o.fixed);
}

FontRequest request_for(const PdfFont &f) {
  FontRequest r;
  r.family = f.family;
  r.bold = f.bold;
  r.italic = f.italic;
  r.serif = f.serif;
  r.fixed = f.fixed;
  if (starts_with(f.name, "Helvetica")) r.family = "Helvetica";
  if (starts_with(f.name, "Times")) r.family = "Times";
  if (starts_with(f.name, "Courier")) r.family = "Courier";
  return r;
}

SystemFont::~SystemFont() {
  if (font_) hb_font_destroy(font_);
  if (face_) hb_face_destroy(face_);
}

std::shared_ptr<SystemFont> SystemFont::open(const std::string &file, int index) {
  static std::map<std::pair<std::string, int>, std::shared_ptr<SystemFont>> cache;
  auto key = std::make_pair(file, index);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  std::shared_ptr<SystemFont> f(new SystemFont());
  if (!read_file(file, f->data_) || f->data_.size() < 12) {
    cache[key] = nullptr;
    return nullptr;
  }
  hb_blob_t *blob = hb_blob_create(f->data_.data(), unsigned(f->data_.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  f->face_ = hb_face_create(blob, unsigned(index));
  hb_blob_destroy(blob);
  if (!f->face_ || hb_face_get_glyph_count(f->face_) == 0) {
    cache[key] = nullptr;
    return nullptr;
  }
  f->font_ = hb_font_create(f->face_);
  f->file = file;
  f->index = index;
  f->upem = std::max(16u, hb_face_get_upem(f->face_));
  hb_position_t v;
  if (hb_ot_metrics_get_position(f->font_, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, &v)) f->ascent = v / f->upem;
  if (hb_ot_metrics_get_position(f->font_, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, &v)) f->descent = v / f->upem;
  if (hb_ot_metrics_get_position(f->font_, HB_OT_METRICS_TAG_CAP_HEIGHT, &v) && v > 0) f->cap_height = v / f->upem;
  else f->cap_height = f->ascent * 0.72;
  auto table = [&](const char *tag, std::string &out) {
    hb_blob_t *b = hb_face_reference_table(f->face_, hb_tag_from_string(tag, 4));
    unsigned len = 0;
    const char *d = hb_blob_get_data(b, &len);
    out.assign(d ? d : "", len);
    hb_blob_destroy(b);
    return len > 0;
  };
  auto u16 = [](const std::string &s, size_t at) { return int(((unsigned char)s[at] << 8) | (unsigned char)s[at + 1]); };
  auto s16 = [&](const std::string &s, size_t at) { return int(int16_t(uint16_t(u16(s, at)))); };
  std::string t;
  if (table("head", t) && t.size() >= 44)
    for (int k = 0; k < 4; ++k) f->bbox[k] = s16(t, 36 + size_t(2 * k)) / f->upem;
  if (table("post", t) && t.size() >= 16) {
    f->italic_angle = s16(t, 4) + u16(t, 6) / 65536.0;
    f->fixed = t[12] || t[13] || t[14] || t[15];
  }
  f->cff = table("CFF ", t) || table("CFF2", t);
  char buf[256];
  unsigned len = sizeof buf;
  if (hb_ot_name_get_utf8(f->face_, HB_OT_NAME_ID_POSTSCRIPT_NAME, HB_LANGUAGE_INVALID, &len, buf) > 0) f->postscript = buf;
  if (f->postscript.empty()) f->postscript = "Font";
  for (auto &c : f->postscript)
    if (c <= ' ' || c > '~' || strchr("()<>[]{}/%#", c)) c = '-';
  cache[key] = f;
  return f;
}

namespace {

bool usable_format(FcPattern *p) {
  FcChar8 *fmt = nullptr;
  if (FcPatternGetString(p, FC_FONTFORMAT, 0, &fmt) != FcResultMatch || !fmt) return true;
  std::string f = reinterpret_cast<const char *>(fmt);
  return f == "TrueType" || f == "CFF";
}

std::shared_ptr<SystemFont> from_pattern(FcPattern *p) {
  FcChar8 *file = nullptr;
  int index = 0;
  if (FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch || !file) return nullptr;
  FcPatternGetInteger(p, FC_INDEX, 0, &index);
  return SystemFont::open(reinterpret_cast<const char *>(file), index & 0xFFFF);
}

}  // namespace

static FcPattern *request_pattern(const FontRequest &r) {
  FcPattern *p = FcPatternCreate();
  if (!r.family.empty()) FcPatternAddString(p, FC_FAMILY, reinterpret_cast<const FcChar8 *>(r.family.c_str()));
  const char *generic = r.fixed ? "monospace" : r.serif ? "serif" : "sans-serif";
  FcPatternAddString(p, FC_FAMILY, reinterpret_cast<const FcChar8 *>(generic));
  FcPatternAddInteger(p, FC_WEIGHT, r.bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
  FcPatternAddInteger(p, FC_SLANT, r.italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
  FcPatternAddBool(p, FC_SCALABLE, FcTrue);
  FcConfigSubstitute(nullptr, p, FcMatchPattern);
  FcDefaultSubstitute(p);
  return p;
}

static void describe(SystemFont &f, FcPattern *p) {
  FcChar8 *s = nullptr;
  if (FcPatternGetString(p, FC_FAMILY, 0, &s) == FcResultMatch && s) f.family = reinterpret_cast<const char *>(s);
  if (FcPatternGetString(p, FC_STYLE, 0, &s) == FcResultMatch && s) f.style = reinterpret_cast<const char *>(s);
  int w = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN;
  FcPatternGetInteger(p, FC_WEIGHT, 0, &w);
  FcPatternGetInteger(p, FC_SLANT, 0, &slant);
  f.bold = w >= FC_WEIGHT_DEMIBOLD;
  f.italic = slant != FC_SLANT_ROMAN;
}

std::shared_ptr<SystemFont> SystemFont::find(const FontRequest &r) {
  static std::map<FontRequest, std::shared_ptr<SystemFont>> cache;
  auto it = cache.find(r);
  if (it != cache.end()) return it->second;
  std::shared_ptr<SystemFont> found;
  FcPattern *p = request_pattern(r);
  FcResult res;
  FcFontSet *set = FcFontSort(nullptr, p, FcFalse, nullptr, &res);
  for (int k = 0; set && k < set->nfont && !found; ++k) {
    FcPattern *m = FcFontRenderPrepare(nullptr, p, set->fonts[k]);
    if (m && usable_format(m)) {
      found = from_pattern(m);
      if (found && found->family.empty()) describe(*found, m);
    }
    if (m) FcPatternDestroy(m);
  }
  if (set) FcFontSetDestroy(set);
  FcPatternDestroy(p);
  cache[r] = found;
  return found;
}

std::shared_ptr<SystemFont> SystemFont::fallback(char32_t cp, const FontRequest &r) {
  static std::map<std::pair<char32_t, FontRequest>, std::shared_ptr<SystemFont>> cache;
  auto key = std::make_pair(cp, r);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  std::shared_ptr<SystemFont> found;
  FcPattern *p = request_pattern(r);
  FcCharSet *cs = FcCharSetCreate();
  FcCharSetAddChar(cs, FcChar32(cp));
  FcPatternAddCharSet(p, FC_CHARSET, cs);
  FcResult res;
  FcFontSet *set = FcFontSort(nullptr, p, FcFalse, nullptr, &res);
  for (int k = 0; set && k < set->nfont && !found; ++k) {
    FcCharSet *have = nullptr;
    if (FcPatternGetCharSet(set->fonts[k], FC_CHARSET, 0, &have) == FcResultMatch && have &&
        !FcCharSetHasChar(have, FcChar32(cp)))
      continue;
    FcPattern *m = FcFontRenderPrepare(nullptr, p, set->fonts[k]);
    if (m && usable_format(m)) {
      std::shared_ptr<SystemFont> f = from_pattern(m);
      if (f && f->covers(cp)) {
        if (f->family.empty()) describe(*f, m);
        found = f;
      }
    }
    if (m) FcPatternDestroy(m);
  }
  if (set) FcFontSetDestroy(set);
  FcCharSetDestroy(cs);
  FcPatternDestroy(p);
  cache[key] = found;
  return found;
}

std::vector<std::string> SystemFont::families() {
  std::vector<std::string> out;
  FcPattern *p = FcPatternCreate();
  FcPatternAddBool(p, FC_SCALABLE, FcTrue);
  FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, FC_FONTFORMAT, nullptr);
  FcFontSet *set = FcFontList(nullptr, p, os);
  for (int k = 0; set && k < set->nfont; ++k) {
    if (!usable_format(set->fonts[k])) continue;
    FcChar8 *fam = nullptr;
    if (FcPatternGetString(set->fonts[k], FC_FAMILY, 0, &fam) == FcResultMatch && fam) {
      std::string f = reinterpret_cast<const char *>(fam);
      if (!f.empty() && f[0] != '.') out.push_back(f);
    }
  }
  if (set) FcFontSetDestroy(set);
  FcObjectSetDestroy(os);
  FcPatternDestroy(p);
  std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool SystemFont::covers(char32_t cp) const { return glyph(cp) != 0; }

unsigned SystemFont::glyph(char32_t cp) const {
  hb_codepoint_t g = 0;
  if (!hb_font_get_nominal_glyph(font_, cp, &g)) return 0;
  return g;
}

double SystemFont::width(unsigned gid) const {
  double units = hb_font_get_glyph_h_advance(font_, gid) * 1000.0 / upem;
  return std::round(units * 10) / 10000.0;
}

std::vector<SystemFont::Shaped> SystemFont::shape(const std::u32string &text) const {
  std::vector<Shaped> out;
  if (text.empty()) return out;
  hb_buffer_t *b = hb_buffer_create();
  hb_buffer_add_utf32(b, reinterpret_cast<const uint32_t *>(text.data()), int(text.size()), 0, int(text.size()));
  hb_buffer_guess_segment_properties(b);
  hb_buffer_set_direction(b, HB_DIRECTION_LTR);
  hb_buffer_set_cluster_level(b, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
  hb_shape(font_, b, nullptr, 0);
  unsigned n = 0;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos(b, &n);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(b, &n);
  for (unsigned k = 0; k < n; ++k) {
    Shaped s;
    s.gid = info[k].codepoint;
    s.cluster = info[k].cluster;
    s.advance = pos[k].x_advance / upem;
    s.dx = pos[k].x_offset / upem;
    s.dy = pos[k].y_offset / upem;
    out.push_back(s);
  }
  hb_buffer_destroy(b);
  return out;
}

QPDFObjectHandle SystemFont::embed(PdfFile &pdf, const std::map<unsigned, std::string> &text_of,
                                   std::string &error) const {
  hb_subset_input_t *in = hb_subset_input_create_or_fail();
  if (!in) {
    error = "hb-subset";
    return QPDFObjectHandle();
  }
  hb_set_t *glyphs = hb_subset_input_glyph_set(in);
  hb_set_add(glyphs, 0);
  for (const auto &kv : text_of) hb_set_add(glyphs, kv.first);
  hb_set_t *unicodes = hb_subset_input_unicode_set(in);
  for (const auto &kv : text_of)
    for (char32_t c : utf32(kv.second)) hb_set_add(unicodes, c);
  hb_subset_input_set_flags(in, HB_SUBSET_FLAGS_RETAIN_GIDS);
  hb_face_t *sub = hb_subset_or_fail(face_, in);
  hb_subset_input_destroy(in);
  if (!sub) {
    error = "hb-subset";
    return QPDFObjectHandle();
  }
  hb_blob_t *blob = hb_face_reference_blob(sub);
  unsigned len = 0;
  const char *d = hb_blob_get_data(blob, &len);
  std::string program(d ? d : "", len);
  hb_blob_destroy(blob);
  hb_face_destroy(sub);
  if (program.empty()) {
    error = "hb-subset";
    return QPDFObjectHandle();
  }
  // The subset tag: six letters from the glyphs it holds.
  unsigned long hash = 5381;
  for (const auto &kv : text_of) hash = hash * 33 + kv.first;
  for (char c : postscript) hash = hash * 33 + (unsigned char)c;
  std::string tag;
  for (int k = 0; k < 6; ++k) {
    tag += char('A' + hash % 26);
    hash /= 26;
  }
  std::string base = tag + "+" + postscript;
  auto num = [](double v) { return QPDFObjectHandle::newReal(std::round(v * 1000) / 1000.0, 3, true); };
  auto bbox = QPDFObjectHandle::newArray();
  for (double v : this->bbox) bbox.appendItem(num(v * 1000));
  QPDFObjectHandle fd = QPDFObjectHandle::newDictionary();
  fd.replaceKey("/Type", QPDFObjectHandle::newName("/FontDescriptor"));
  fd.replaceKey("/FontName", QPDFObjectHandle::newName("/" + base));
  int flags = 32 | (fixed ? 1 : 0) | (italic ? 64 : 0);
  fd.replaceKey("/Flags", QPDFObjectHandle::newInteger(flags));
  fd.replaceKey("/FontBBox", bbox);
  fd.replaceKey("/ItalicAngle", num(italic_angle));
  fd.replaceKey("/Ascent", num(ascent * 1000));
  fd.replaceKey("/Descent", num(descent * 1000));
  fd.replaceKey("/CapHeight", num(cap_height * 1000));
  fd.replaceKey("/StemV", QPDFObjectHandle::newInteger(bold ? 140 : 80));
  QPDFObjectHandle fdict = QPDFObjectHandle::newDictionary();
  if (cff) fdict.replaceKey("/Subtype", QPDFObjectHandle::newName("/OpenType"));
  else fdict.replaceKey("/Length1", QPDFObjectHandle::newInteger((long long)program.size()));
  fd.replaceKey(cff ? "/FontFile3" : "/FontFile2", pdf.add_stream(program, fdict));
  // Widths: runs of consecutive glyphs.
  QPDFObjectHandle w = QPDFObjectHandle::newArray();
  for (auto it = text_of.begin(); it != text_of.end();) {
    unsigned first = it->first, expect = first;
    QPDFObjectHandle run = QPDFObjectHandle::newArray();
    while (it != text_of.end() && it->first == expect) {
      run.appendItem(num(width(it->first) * 1000));
      ++it;
      ++expect;
    }
    w.appendItem(QPDFObjectHandle::newInteger(first));
    w.appendItem(run);
  }
  QPDFObjectHandle cid = QPDFObjectHandle::newDictionary();
  cid.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
  cid.replaceKey("/Subtype", QPDFObjectHandle::newName(cff ? "/CIDFontType0" : "/CIDFontType2"));
  cid.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + base));
  QPDFObjectHandle info = QPDFObjectHandle::newDictionary();
  info.replaceKey("/Registry", QPDFObjectHandle::newString("Adobe"));
  info.replaceKey("/Ordering", QPDFObjectHandle::newString("Identity"));
  info.replaceKey("/Supplement", QPDFObjectHandle::newInteger(0));
  cid.replaceKey("/CIDSystemInfo", info);
  cid.replaceKey("/FontDescriptor", pdf.add(fd));
  cid.replaceKey("/DW", QPDFObjectHandle::newInteger(1000));
  cid.replaceKey("/W", w);
  if (!cff) cid.replaceKey("/CIDToGIDMap", QPDFObjectHandle::newName("/Identity"));
  // /ToUnicode: every glyph with the text it shows.
  std::string cmap =
      "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
      "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
      "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
      "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
  std::vector<std::pair<unsigned, std::string>> entries;
  for (const auto &kv : text_of)
    if (!kv.second.empty()) entries.push_back(kv);
  for (size_t k = 0; k < entries.size(); k += 100) {
    size_t n = std::min<size_t>(100, entries.size() - k);
    cmap += format("%zu beginbfchar\n", n);
    for (size_t j = k; j < k + n; ++j) {
      std::string code(2, '\0');
      code[0] = char(entries[j].first >> 8);
      code[1] = char(entries[j].first & 0xFF);
      cmap += pdf_hex(code) + " " + pdf_hex(utf16be(entries[j].second)) + "\n";
    }
    cmap += "endbfchar\n";
  }
  cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
  QPDFObjectHandle font = QPDFObjectHandle::newDictionary();
  font.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
  font.replaceKey("/Subtype", QPDFObjectHandle::newName("/Type0"));
  font.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + base));
  font.replaceKey("/Encoding", QPDFObjectHandle::newName("/Identity-H"));
  QPDFObjectHandle kids = QPDFObjectHandle::newArray();
  kids.appendItem(pdf.add(cid));
  font.replaceKey("/DescendantFonts", kids);
  font.replaceKey("/ToUnicode", pdf.add_stream(cmap, QPDFObjectHandle::newNull()));
  return pdf.add(font);
}

}  // namespace xanara
