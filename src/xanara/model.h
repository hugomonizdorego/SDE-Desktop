// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Xanara's UI independent model: the editing modes and the registry of
// their tools, the tab session, exporters and the PDFs Xanara generates
// itself with cairo (stamps, images, redacted pages). The PDF engine that
// changes documents is in pdf.h, layout.h and edit.h. Nothing here
// touches FLTK, so the unit tests use it directly.
#pragma once

#include <cairo.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core.h"
#include "pdf.h"

namespace xanara {

// ------------------------------------------------------------------ icons

namespace icon {
enum Kind {
  Sidebar, Open, Save, Print, Find, Menu, ZoomIn, ZoomOut, FitWidth, FitPage, Prev, Next, Up, Down,
  Select, Highlight, Underline, Strike, Note, TextBox, Pen, Rect, Line, Signature, Close, RotateLeft,
  RotateRight, Moon, Sun, Sepia, Trash, Undo, Present, Pages, Outline, Notes, Book, Single, Continuous,
  Check, Document, Chevron, ChevronDown, Lock,
  // Added with the alpha 16 redesign.
  Home, Plus, Redo, Share, Properties, Squiggly, Eraser, Ellipse, Arrow, Measure, Stamp, Eye, EyeOff,
  AddText, Image, Link, Watermark, Background, HeaderFooter, Bates, Crop, Insert, Extract, Split, Merge,
  Reorder, PageNumbers, FieldText, FieldCheck, FieldRadio, FieldCombo, FieldList, FieldButton, FieldSign,
  Cross, Dot, Calendar, Initials, ExportImage, ExportText, Html, Markdown, ImagesToPdf, Key, Unlock, Redact,
  Shield, Compress, Ocr, Compare, Bookmark, Attachment, Hand, More, Grid, Info,
  // Added with the direct editing of 0.2 Alpha 3.
  EditText, Bold, Italic, AlignLeft, AlignCenter, AlignRight, AlignJustify, Replace, Count
};
}  // namespace icon

// ------------------------------------------------------------ modes, tools

enum class Mode { Annotate = 0, Edit, Page, Form, Fill, Convert, Protect, Tools };
constexpr int kModeCount = 8;
const char *mode_label(Mode m);  // translated
const char *mode_id(Mode m);     // "annotate", ... for the settings file
bool mode_from_id(const std::string &id, Mode &m);

// Every command of the context toolbar.
enum class Act {
  // Annotate
  Highlight, Underline, Squiggly, Strike, TextBox, Note, Pen, Eraser, Rect, Ellipse, Arrow, Line, Measure,
  Stamp, Signature, ToggleAnnots,
  // Edit
  EditContent, AddText, AddImage, AddLink, Watermark, Background, HeaderFooter, Bates, Crop,
  // Page
  PageInsert, PageExtract, PageSplit, PageMerge, PageRotateLeft, PageRotateRight, PageDelete, PageMoveUp,
  PageMoveDown, PageNumbers,
  // Form
  FieldText, FieldCheck, FieldRadio, FieldCombo, FieldList, FieldButton, FieldSignature, ShowFields,
  // Fill
  FillText, FillCheck, FillCross, FillDot, FillDate, FillInitials, FillSign,
  // Convert
  ToPng, ToJpeg, ToText, ToHtml, ToMarkdown, ImagesToPdf, MergeFiles,
  // Protect
  Encrypt, Decrypt, Redact, ApplyRedactions, RemoveMetadata,
  // Tools
  Compress, Ocr, Compare, Print,
  Count
};

struct ToolSpec {
  Act act;
  Mode mode;
  const char *id;        // stable name ("annotate.highlight")
  const char *label;     // short label (untranslated, N_())
  const char *tip;       // tooltip (untranslated, N_())
  icon::Kind glyph;
  bool canvas;           // stays armed and acts on the page (vs. a one shot command)
  int group;             // a separator is drawn between groups
};

const std::vector<ToolSpec> &tool_registry();
std::vector<const ToolSpec *> tools_for(Mode m);
const ToolSpec *tool_spec(Act a);
const ToolSpec *tool_by_id(const std::string &id);

// How many context tools fit in |width| pixels with |button| wide buttons
// and |sep| wide separators, leaving room for an overflow button. Returns
// the count of tools shown in the bar; the rest go to the overflow menu.
int tools_that_fit(const std::vector<const ToolSpec *> &tools, int width, int button, int sep, int overflow);

// ---------------------------------------------------------------- tabs

// The documents open in one window. Index -1 is the Home tab.
class TabList {
 public:
  int count() const { return int(paths_.size()); }
  int active() const { return active_; }
  const std::string &path(int i) const { return paths_[size_t(i)]; }
  int id(int i) const { return ids_[size_t(i)]; }
  int index_of_id(int id) const;
  int find(const std::string &path) const;
  // Opens |path| as a new tab after the active one (or activates the tab
  // that already shows it). Returns its index.
  int open(const std::string &path, bool *existing = nullptr);
  // Closes tab |i|; the neighbour to the right (or left) becomes active,
  // Home when none is left. Returns the new active index.
  int close(int i);
  bool move(int from, int to);
  void activate(int i);  // -1: Home
  void rename(int i, const std::string &path);  // after Save As

  // The session file: one path per line, "*" marks the active tab.
  std::string serialize() const;
  static bool parse(const std::string &text, std::vector<std::string> &paths, int &active);

 private:
  std::vector<std::string> paths_;
  std::vector<int> ids_;
  int active_ = -1;
  int next_id_ = 1;
};

// The drop slot for a tab dragged to |x| over tabs starting at |x0|, each
// |tab_w| wide: the index the tab moves to.
int tab_drop_index(int x, int x0, int tab_w, int count, int from);

// ------------------------------------------------------------- exporters

std::string html_escape(const std::string &s);
std::string export_html(const std::string &title, const std::vector<std::string> &pages);
std::string export_markdown(const std::string &title, const std::vector<std::string> &pages);
std::string export_plain(const std::vector<std::string> &pages);
// "report.pdf" + "png" + page 3 of 12 -> "report-03.png"
std::string numbered_name(const std::string &pdf_path, const std::string &ext, int page, int count);

// --------------------------------------------------------- generated PDFs

struct StampSpec {
  enum Kind { Watermark, Background, HeaderFooter, Bates, PageNumbers } kind = Watermark;
  std::string text;                        // watermark text
  std::string header[3], footer[3];        // left, center, right; tokens below
  std::string prefix;                      // Bates prefix
  int start = 1, digits = 6;               // Bates numbering
  Rgb color{0.85, 0.1, 0.1};
  double opacity = 0.25, font_size = 54, angle = 45;  // angle in degrees, counter clockwise
  std::string file_name;                   // for {file}
  std::string date;                        // for {date}
};
// Expands {page} {pages} {date} {file} and {bates}.
std::string expand_stamp(const std::string &tpl, const StampSpec &spec, int page, int count);
std::string bates_number(const StampSpec &spec, int page);
// One overlay page per document page, same sizes, for `qpdf --overlay/--underlay`.
bool write_stamp_pdf(const std::string &path, const std::vector<Point> &sizes, const StampSpec &spec,
                     std::string &error);
// A PDF with one page per image (each page sized to the image at 96 dpi,
// or A4 portrait/landscape with the image fitted when |a4|).
bool images_to_pdf(const std::vector<cairo_surface_t *> &images, const std::string &path, bool a4,
                   std::string &error);
// One page PDF holding the rendered page with |black| boxes painted over.
bool rasterize_page_pdf(Document &doc, int page, double dpi, const std::vector<Box> &black,
                        const std::string &path, std::string &error);
// Renders a page to a PNG (or, with libjpeg, a JPEG) file.
bool export_page_image(Document &doc, int page, double dpi, const std::string &path, bool jpeg,
                       std::string &error);
bool have_native_jpeg();

// Shapes Xanara builds from strokes (page space) and stamps.
using Strokes = std::vector<std::vector<Point>>;
Strokes arrow_strokes(Point a, Point b, double head);
Strokes check_strokes(Point c, double size);
Strokes cross_strokes(Point c, double size);
Strokes dot_strokes(Point c, double size);
// A rubber stamp ("APPROVED") drawn into an image of |w| x |h| points.
cairo_surface_t *stamp_image(const std::string &text, Rgb color, double w, double h);
// Text drawn into an image (for text placed by Fill & Sign and labels).
cairo_surface_t *label_image(const std::string &text, Rgb color, double font_size, double &w, double &h);
// "12.7 mm" for a distance of |points| PDF points in "mm", "cm", "in" or "pt".
std::string measure_label(double points, const std::string &unit);

}  // namespace xanara
