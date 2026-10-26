// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Fonts of documents and of the system. PdfFont reads a font dictionary:
// how its strings split into character codes, the width of each code
// (/Widths, /W, or the tables of the standard 14 fonts), the Unicode text
// of each code (/ToUnicode, the encoding and its glyph names, the font's
// own cmap) and, for edited text, which code shows a given character.
// SystemFont is an installed font chosen with fontconfig to look like a
// document's font (or picked by the user); it shapes text with HarfBuzz
// and embeds the glyphs a change uses as a subset (hb-subset) in a Type0
// font with Identity-H and a /ToUnicode map, so that new text is shown
// the same everywhere and stays searchable and copyable.
#pragma once

#include <qpdf/QPDFObjectHandle.hh>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "pdf.h"

struct hb_face_t;
struct hb_font_t;

namespace xanara {

namespace fontdata {
struct GlyphName {
  const char *name;
  uint16_t unicode;
};
extern const uint16_t kStandardEncoding[256], kWinAnsiEncoding[256], kMacRomanEncoding[256];
extern const GlyphName kGlyphNames[];
extern const size_t kGlyphNameCount;
extern const uint16_t kBase14Chars[];
extern const size_t kBase14CharCount;
extern const uint16_t kHelvetica[], kHelveticaBold[], kTimesRoman[], kTimesBold[], kTimesItalic[],
    kTimesBoldItalic[];
}  // namespace fontdata

// The Unicode value of an Adobe glyph name ("Aacute", "uni00C1",
// "u1F600", "fi"); 0 when unknown.
char32_t glyph_unicode(const std::string &name);
std::string utf8(char32_t cp);
std::u32string utf32(const std::string &utf8);
std::string utf8(const std::u32string &text);

// A CMap: the code space (how many bytes each code has) and the
// code -> CID mapping of a composite font's encoding.
class CMap {
 public:
  bool parse(const std::string &text, int depth = 0);
  // A predefined CMap ("Identity-H", "UniJIS-UCS2-H", from poppler-data).
  bool load(const std::string &name, int depth = 0);
  // Reads the code at |i|; returns its length in bytes.
  int next(const std::string &s, size_t i, unsigned &code) const;
  unsigned cid(unsigned code) const;
  bool identity = false;
  bool vertical = false;

 private:
  struct Space {
    int bytes;
    unsigned char lo[4], hi[4];
  };
  struct Range {
    unsigned lo, hi, cid;
  };
  std::vector<Space> spaces_;
  std::vector<Range> ranges_;
  std::map<unsigned, unsigned> chars_;
};

// A /ToUnicode map: code -> text.
class UnicodeMap {
 public:
  bool parse(const std::string &text);
  bool lookup(unsigned code, std::string &out) const;
  bool empty() const { return chars_.empty() && ranges_.empty(); }
  // Every single character mapping (code, character), for reverse lookups.
  void each(const std::function<void(unsigned, const std::string &)> &fn) const;

 private:
  struct Range {
    unsigned lo, hi;
    std::u16string start;            // incremented for each code
    std::vector<std::string> list;   // or one entry per code
  };
  std::map<unsigned, std::string> chars_;
  std::vector<Range> ranges_;
};

// The font of some text in a document.
class PdfFont {
 public:
  static std::shared_ptr<PdfFont> load(QPDFObjectHandle dict);
  ~PdfFont();

  enum Kind { Simple, Type3, Composite };
  Kind kind = Simple;
  std::string name;   // /BaseFont without the subset tag ("Arial-BoldMT")
  std::string family; // a family name for fontconfig ("Arial")
  bool embedded = false, bold = false, italic = false, serif = false, fixed = false, symbolic = false;
  bool vertical = false;      // vertical writing: shown, not edited
  bool known_widths = true;   // widths from the document or the standard tables
  Matrix matrix{0.001, 0, 0, 0.001, 0, 0};  // glyph space -> text space
  double ascent = 0.8, descent = -0.2;      // text space, for font size 1
  QPDFObjectHandle dict;

  // The code at |i| of a shown string; |bytes| receives its length.
  unsigned next(const std::string &s, size_t i, int &bytes) const;
  // The horizontal displacement of a code for font size 1 (text space).
  double width(unsigned code) const;
  // The Unicode text of a code, "" when the document does not tell.
  std::string text(unsigned code) const;
  // The string bytes that show |cp|, when this font has a glyph for it.
  bool encode(char32_t cp, std::string &bytes) const;
  // Bytes per code when encoding (1 for simple fonts).
  int code_length() const { return kind == Composite ? encode_bytes_ : 1; }

 private:
  PdfFont() = default;
  void load_simple();
  void load_composite();
  void read_descriptor(QPDFObjectHandle fd);
  void reverse_map() const;
  bool has_glyph(unsigned code, char32_t cp) const;
  void open_program() const;

  // Simple fonts.
  double widths_[256] = {0};
  bool have_width_[256] = {false};
  std::string names_[256];         // glyph names from /Differences
  const uint16_t *base_ = nullptr; // base encoding (code -> Unicode)
  const uint16_t *metrics_ = nullptr;  // standard 14 widths by Unicode
  double fixed_width_ = 0;         // Courier
  double missing_width_ = 0;
  // Composite fonts.
  CMap cmap_;
  std::map<unsigned, double> cid_widths_;
  double default_width_ = 1.0;
  std::vector<uint16_t> cid_to_gid_;  // empty: identity
  std::string collection_;            // "Adobe-Japan1"
  int encode_bytes_ = 2;
  UnicodeMap to_unicode_;
  // The embedded font program (FreeType checks), loaded lazily.
  std::string program_;
  mutable void *face_ = nullptr;  // FT_Face
  mutable bool face_tried_ = false;
  mutable std::map<unsigned, char32_t> gid_unicode_;
  mutable bool reverse_ready_ = false;
  mutable std::map<char32_t, std::string> reverse_;
};

// What a document's font looks like, to find an installed font for it.
struct FontRequest {
  std::string family;   // empty: by the flags alone
  bool bold = false, italic = false, serif = false, fixed = false;
  bool operator<(const FontRequest &o) const;
};
FontRequest request_for(const PdfFont &f);

// An installed font (fontconfig, HarfBuzz).
class SystemFont {
 public:
  ~SystemFont();
  // The installed font closest to |r|; never null while any font exists.
  static std::shared_ptr<SystemFont> find(const FontRequest &r);
  // An installed font with a glyph for |cp|, preferring one like |r|.
  static std::shared_ptr<SystemFont> fallback(char32_t cp, const FontRequest &r);
  // The families the user can choose from, sorted.
  static std::vector<std::string> families();
  // A font file (cached).
  static std::shared_ptr<SystemFont> open(const std::string &file, int index);

  std::string family, style, file, postscript;
  int index = 0;
  bool cff = false, bold = false, italic = false, fixed = false;
  double upem = 1000;
  double ascent = 0.9, descent = -0.22, cap_height = 0.7, italic_angle = 0;
  double bbox[4] = {0, -0.2, 1, 0.9};

  bool covers(char32_t cp) const;
  unsigned glyph(char32_t cp) const;
  // The width written to the PDF for a glyph (font size 1), the same that
  // layout uses so that positions match exactly.
  double width(unsigned gid) const;
  struct Shaped {
    unsigned gid = 0;
    size_t cluster = 0;    // index into the shaped text
    double advance = 0;    // font size 1
    double dx = 0, dy = 0; // offsets, font size 1
  };
  std::vector<Shaped> shape(const std::u32string &text) const;
  // The embedded Type0 font with the glyphs of |text_of| (glyph -> text
  // it shows) for |pdf|.
  QPDFObjectHandle embed(PdfFile &pdf, const std::map<unsigned, std::string> &text_of, std::string &error) const;

 private:
  SystemFont() = default;
  std::string data_;
  hb_face_t *face_ = nullptr;
  hb_font_t *font_ = nullptr;
};

}  // namespace xanara
