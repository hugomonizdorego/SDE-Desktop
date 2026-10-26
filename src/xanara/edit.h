// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Direct editing of page content. TextEdit is the model of one text box
// while the user types in it, as in a word processor: characters with
// their styles, caret and selection, line breaking inside the box, undo.
// Characters that were in the document keep their font and their exact
// codes; new ones use the document's font when it has the glyph, else the
// installed font that looks most like it. Committing writes the change
// into the page: the glyphs of the lines that changed are removed from
// their text operators (each replaced by the same advance, so everything
// after them keeps its place) and the new lines are drawn with the
// document's fonts and embedded subsets of installed fonts. Images can be
// moved, resized, replaced, deleted and added; redaction removes what
// lies under a box. The page geometry is Xanara's page space (core.h).
#pragma once

#include <cairo.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "layout.h"

namespace xanara {

struct TextStyle {
  int font = -1;            // the document font of original text, -1: none
  std::string family;       // installed family; "" = like the document font
  double size = 12;         // font size in points on the page
  bool bold = false, italic = false;
  Paint paint;              // fill color
  double hscale = 1, rise = 0, char_space = 0;  // as on the page (points)
  bool operator==(const TextStyle &o) const;
};

class TextEdit {
 public:
  TextEdit();
  ~TextEdit();

  // Edits block |block| of |layout|. False when the block cannot be edited
  // (text drawn upside down, fonts without widths, invisible text).
  bool start(std::shared_ptr<PageLayout> layout, int block);
  // A new, empty text box whose first line's top-left corner is |at|;
  // |width| <= 0 makes it grow to the right as the user types.
  void start_new(std::shared_ptr<PageLayout> layout, Point at, double width, const TextStyle &style);
  bool active() const { return layout_ != nullptr; }
  int page() const { return layout_ ? layout_->page : -1; }
  int block() const { return block_; }
  const PageLayout *page_layout() const { return layout_.get(); }
  bool modified() const { return modified_; }
  bool empty() const { return chars_.empty(); }
  std::string text() const;  // the whole box, paragraphs separated by '\n'

  // Caret and selection, as character indices (0 .. size).
  size_t size() const { return chars_.size(); }
  size_t caret() const { return caret_; }
  size_t anchor() const { return anchor_; }
  bool has_selection() const { return caret_ != anchor_; }
  void set_caret(size_t i, bool extend);
  size_t hit(double x, double y) const;  // the boundary nearest to a point
  void move_left(bool extend, bool word);
  void move_right(bool extend, bool word);
  void move_up(bool extend);
  void move_down(bool extend);
  void move_home(bool extend, bool box);
  void move_end(bool extend, bool box);
  void select_word(size_t at);
  void select_paragraph(size_t at);
  void select_all();
  std::string selected_text() const;

  // Changes.
  void insert(const std::string &utf8);
  void erase(bool forward, bool word);
  void new_paragraph() { insert("\n"); }
  TextStyle style_at_caret() const;
  // Applies |change| to the style of the selection, or of the whole box
  // when nothing is selected (what is typed next uses it too).
  void restyle(const std::function<void(TextStyle &)> &change);
  int align() const { return align_; }
  void set_align(int a);
  void move_by(double dx, double dy);  // moves the box
  void set_width(double w);            // resizes it (the text flows again)
  bool undo();
  bool redo();
  bool can_undo() const { return !undo_.empty(); }
  bool can_redo() const { return !redo_.empty(); }

  // Geometry of the laid out text (page space).
  Box frame() const;           // the box: what the text needs, at least its width
  bool auto_width() const { return auto_width_; }
  struct Caret {
    double x = 0, top = 0, bottom = 0;
  };
  Caret caret_rect() const;
  std::vector<Box> selection_boxes() const;

  // Output. The content operators that draw the box (all lines, or only
  // the lines that changed) in PDF user space, with the resources they
  // need added to |pdf| / |resources| (owned by |owner|).
  std::string content(PdfFile &pdf, QPDFObjectHandle owner, QPDFObjectHandle resources, bool changed_only,
                      std::string &error) const;
  // The original glyphs that the change removes.
  std::set<int> removed_glyphs() const;
  // Every original glyph of the box (removed for the preview background).
  std::set<int> all_glyphs() const;

  // The laid out text, for tests: one entry per line.
  struct LineInfo {
    std::string text;
    double x1 = 0, x2 = 0, baseline = 0;
    bool kept = false;
  };
  std::vector<LineInfo> lines() const;

 private:
  struct Char {
    char32_t cp = 0;     // '\n': paragraph break
    int style = 0;
    int glyph = -1;      // the original glyph, while unchanged
    bool soft = false;   // the break between two original lines
    int line = -1, pos = -1;  // original line (block line index) and position in it
    int orig_style = -1; // the style it had in the document
  };
  struct Laid {
    size_t ch = 0, nch = 1;
    int doc_font = -1;
    std::shared_ptr<SystemFont> sys;
    unsigned gid = 0;
    std::string bytes;
    double x = 0, y = 0, size = 0, width = 0;  // origin (y: baseline incl. rise), width for size 1
    int style = 0;
    int orig = -1;
    std::string text;
    std::vector<Laid> extra;   // further glyphs of the same cluster
  };
  struct Row {
    size_t begin = 0, end = 0;  // chars, including the break at the end
    double baseline = 0, x1 = 0, x2 = 0, top = 0, bottom = 0;
    bool kept = false;
    int orig_line = -1;
    std::vector<double> xs;     // boundary positions begin .. end
    std::vector<Laid> glyphs;
  };
  struct Snapshot {
    std::vector<Char> chars;
    std::vector<TextStyle> styles;
    size_t caret = 0, anchor = 0;
    int align = 0;
    double x0 = 0, y0 = 0, width = 0;
    bool moved = false, modified = false, auto_width = false;
  };

  int intern(const TextStyle &s);
  void relayout();
  void push_undo(int kind);
  Snapshot snapshot() const;
  void restore(const Snapshot &s);
  void advances(std::vector<double> &adv, std::vector<Laid> &glyphs) const;
  bool word_char(size_t i) const;
  size_t row_of(size_t i) const;

  std::shared_ptr<PageLayout> layout_;
  int block_ = -1;
  std::vector<Char> chars_;
  std::vector<TextStyle> styles_;
  std::vector<Row> rows_;
  size_t caret_ = 0, anchor_ = 0;
  int align_ = 0, orig_align_ = 0;
  double x0_ = 0, y0_ = 0;          // left edge, first baseline
  double orig_width_ = 0;
  double width_ = 0, leading_ = 1.2;  // leading: baseline distance per point of size
  double indent_ = 0;               // first line indent of the original paragraph
  bool auto_width_ = false;
  bool moved_ = false, modified_ = false;
  int last_kind_ = -1;
  std::vector<double> orig_baselines_;
  std::vector<int> orig_lengths_;
  std::vector<Snapshot> undo_, redo_;
  TextStyle typing_;               // the style of what is typed next
  bool typing_set_ = false;
};

// Removes original glyphs from their text operators: each removed glyph
// becomes a positioning number of the same advance in a TJ array.
// Content in a form XObject is changed in a private copy of the form.
bool remove_glyphs(PdfFile &pdf, PageLayout &layout, const std::set<int> &glyphs, std::string &error);
// Draws |ops| on top of page |page| (the old content is wrapped in q/Q
// when it leaves the graphics state changed).
void append_content(PdfFile &pdf, int page, const std::string &ops);
// The whole text edit: removal and drawing.
bool commit_text(PdfFile &pdf, PageLayout &layout, const TextEdit &edit, std::string &error);
// A one page PDF (same page size) that shows only the box: the live
// preview while typing.
bool preview_pdf(PdfFile &source, const PageLayout &layout, const TextEdit &edit, std::string &out,
                 std::string &error);

// ---------------------------------------------------------------- images

// An image file as a new image XObject (JPEG kept as it is; PNG and the
// rest decoded, with an /SMask for transparency).
QPDFObjectHandle image_xobject(PdfFile &pdf, const std::string &path, int &w, int &h, std::string &error);
QPDFObjectHandle image_xobject(PdfFile &pdf, cairo_surface_t *image, std::string &error);
// Places an image XObject into |box| (page space) on top of the page.
bool add_image(PdfFile &pdf, int page, QPDFObjectHandle xobject, const Box &box, std::string &error);
// Moves/resizes image |index| of |layout| so that it covers |box|.
bool move_image(PdfFile &pdf, PageLayout &layout, int index, const Box &box, std::string &error);
bool delete_image(PdfFile &pdf, PageLayout &layout, int index, std::string &error);
// Shows |xobject| where image |index| was, fitted into its box.
bool replace_image(PdfFile &pdf, PageLayout &layout, int index, QPDFObjectHandle xobject, std::string &error);

// ------------------------------------------------------------- redaction

// Removes everything under |boxes| (page space) from page |page|: glyphs
// (from the text operators), image pixels (in a redacted copy of each
// image), paths that lie inside the boxes, and annotations touching
// them; paths that reach outside are clipped. Then paints the boxes
// black. False when something under a box cannot be removed this way
// (|error| says what); the caller then rasterizes the page instead.
bool redact_page(PdfFile &pdf, int page, const std::vector<Box> &boxes, std::string &error);

}  // namespace xanara
