// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// What a page shows and which operation shows it. PageLayout runs the
// page's content streams through a graphics and text state interpreter
// (q/Q, cm, BT/ET, Tf, Tm, Td/TD/T*, Tc/Tw/Tz/TL/Ts/Tr, Tj/TJ/'/", colors,
// Do with forms and images, inline images, paths) and records every glyph
// with its place on the page, its style and the exact bytes of the string
// it came from; likewise every image and painted path. Glyphs are then
// grouped into lines and lines into blocks (paragraphs) — the text boxes
// of the Edit mode. Coordinates are Xanara's page space (core.h Box).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "content.h"
#include "fonts.h"
#include "pdf.h"

namespace xanara {

// A fill color as the document set it.
struct Paint {
  int n = 1;                     // components: 1 gray, 3 RGB, 4 CMYK
  double v[4] = {0, 0, 0, 0};
  double alpha = 1;              // /ca of the graphics state
  Rgb rgb() const;
  bool operator==(const Paint &o) const;
};

struct Glyph {
  unsigned code = 0;
  int bytes = 1;
  int font = -1;                 // PageLayout::fonts
  std::string text;              // Unicode, "" when unknown
  Point origin, end;             // baseline from the origin to the advance (page space)
  Point up;                      // one em upwards (page space)
  Box box;                       // the glyph's cell
  // The text state that showed it.
  double font_size = 0, hscale = 1, rise = 0, char_space = 0, word_space = 0;
  double width = 0;              // displacement for size 1 (font units / 1000)
  int render = 0;                // Tr
  Paint paint;
  bool space = false;            // a one-byte code 32 (word spacing applies)
  // Where it came from.
  int stream = 0, op = -1, part = 0;  // stream, operation, string of a TJ array
  size_t offset = 0;             // byte offset in that string
  bool horizontal = true;        // upright on the page as shown
  bool editable = true;
  int line = -1;
  double size() const;           // the em height on the page
  double advance() const;        // distance origin -> end
};

struct ImageItem {
  int stream = 0, op = -1;       // the Do or BI operation
  Matrix ctm;                    // image space (unit square) -> user space
  Box box;                       // page space
  std::string name;              // resource name of a Do
  QPDFObjectHandle xobject;      // the image XObject (Do)
  bool inline_image = false;
  bool editable = true;          // in the page's own content
  int pixels_w = 0, pixels_h = 0;
};

struct PathItem {
  int stream = 0, first_op = -1, last_op = -1;  // construction .. painting
  Box box;
};

class PageLayout {
 public:
  static std::unique_ptr<PageLayout> read(PdfFile &pdf, int page);

  struct Stream {
    QPDFObjectHandle object;     // the form XObject; null for the page
    QPDFObjectHandle resources;
    std::string data;
    std::vector<Op> ops;
    int parent = -1, parent_op = -1;  // where a form is drawn
    std::string name;            // its resource name there
  };
  struct Line {
    std::vector<int> glyphs;
    std::vector<bool> space_after;  // a gap that reads as a space
    Box box;
    double size = 0;
    int block = -1;
  };
  struct Block {
    std::vector<int> lines;
    Box box;
    int align = 0;               // 0 left, 1 center, 2 right, 3 justified
    double leading = 0;          // baseline distance
    bool editable = true;
  };

  int page = 0;
  PageGeometry geometry;
  std::vector<std::shared_ptr<PdfFont>> fonts;
  std::vector<QPDFObjectHandle> font_dicts;
  std::vector<Stream> streams;   // 0: the page's content
  std::vector<Glyph> glyphs;
  std::vector<Line> lines;
  std::vector<Block> blocks;
  std::vector<ImageItem> images;
  std::vector<PathItem> paths;
  bool complete = true;          // false: something could not be read

  int block_at(double x, double y, double slack = 2) const;
  int image_at(double x, double y) const;
  std::string block_text(int block) const;

 private:
  void build_lines();
  void build_blocks();
};

}  // namespace xanara
