// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Xanara's UI independent model: the editing modes and the registry of
// their tools, the tab session, command lines for the optional external
// tools (qpdf, ghostscript, tesseract, pdftoppm, lp), structural PDF edits
// through qpdf JSON (links, form fields, crop boxes, metadata), exporters
// and the PDFs Xanara generates itself with cairo (stamps, images, redacted
// pages). Nothing here touches FLTK, so the unit tests use it directly.
#pragma once

#include <cairo.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core.h"

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
  Shield, Compress, Ocr, Compare, Bookmark, Attachment, Hand, More, Grid, Info, Count
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
  AddText, AddImage, AddLink, Watermark, Background, HeaderFooter, Bates, Crop,
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
  // External programs: |needs| are all required, |any| at least one of.
  std::vector<const char *> needs, any;
  const char *package;   // Debian package(s) to install when missing
};

const std::vector<ToolSpec> &tool_registry();
std::vector<const ToolSpec *> tools_for(Mode m);
const ToolSpec *tool_spec(Act a);
const ToolSpec *tool_by_id(const std::string &id);

// Can the tool run here? |have| answers whether a program is installed.
struct Availability {
  bool ok = true;
  std::string missing;  // the first missing program
  std::string reason;   // "Needs qpdf — install it with: sudo apt install qpdf"
};
Availability tool_availability(const ToolSpec &spec, const std::function<bool(const std::string &)> &have);

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

// ------------------------------------------------------- command planning

struct Permissions {
  bool print = true, modify = true, copy = true, annotate = true, forms = true, assemble = true;
};

using Argv = std::vector<std::string>;
Argv qpdf_base(const std::string &password);
Argv qpdf_encrypt_args(const std::string &in, const std::string &out, const std::string &user_pw,
                       const std::string &owner_pw, const Permissions &perms, const std::string &password);
Argv qpdf_decrypt_args(const std::string &in, const std::string &out, const std::string &password);
Argv qpdf_merge_args(const std::vector<std::string> &files, const std::string &out);
Argv qpdf_overlay_args(const std::string &in, const std::string &stamp, const std::string &out, bool underlay,
                       const std::string &password);
Argv qpdf_compress_args(const std::string &in, const std::string &out, const std::string &password);
Argv qpdf_split_args(const std::string &in, const std::string &out_pattern, int pages_per_file,
                     const std::string &password);
Argv qpdf_update_args(const std::string &in, const std::string &json, const std::string &out,
                      const std::string &password);
Argv qpdf_json_args(const std::string &in, const std::string &password);
Argv gs_compress_args(const std::string &in, const std::string &out, const std::string &quality);
Argv tesseract_args(const std::string &image, const std::string &out_base, const std::string &lang);
Argv pdftoppm_jpeg_args(const std::string &in, int page, int dpi, const std::string &out_base,
                        const std::string &password);
Argv lp_args(const std::string &file, const std::string &title);

// Redaction: pages with redaction boxes are replaced by an image of the
// page with the boxes painted black, so no text, vector art or image data
// under a box survives. |raster| maps a page (0 based) to its one page PDF.
Argv redaction_splice_args(const std::string &in, int count, const std::map<int, std::string> &raster,
                           const std::string &out, const std::string &password);
// OCR: page images turned into searchable one page PDFs, joined in order.
Argv ocr_join_args(const std::vector<std::string> &page_pdfs, const std::string &out);

// ------------------------------------------------------------------ JSON

// A small JSON value, enough for qpdf's JSON v2 (ordered objects).
class Json {
 public:
  enum Type { Null, Boolean, Number, String, Array, Object };
  Json() = default;
  static Json number(double v);
  static Json string(const std::string &s);
  static Json boolean(bool b);
  static Json array(std::vector<Json> items = {});
  static Json object();
  static bool parse(const std::string &text, Json &out, std::string &error);

  Type type() const { return type_; }
  bool is(Type t) const { return type_ == t; }
  double num() const { return num_; }
  const std::string &str() const { return str_; }
  bool boolean_value() const { return bool_; }
  std::vector<Json> &items() { return items_; }
  const std::vector<Json> &items() const { return items_; }
  const std::vector<std::pair<std::string, Json>> &members() const { return members_; }

  const Json *get(const std::string &key) const;
  Json *get(const std::string &key);
  Json &set(const std::string &key, Json value);  // replaces or appends
  bool erase(const std::string &key);
  void push(Json v) { items_.push_back(std::move(v)); }
  std::string dump() const;

 private:
  Type type_ = Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Json> items_;
  std::vector<std::pair<std::string, Json>> members_;
};

// --------------------------------------------------- structural PDF edits

struct PdfRect {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // PDF user space, y up
};

// Maps a box in Xanara's page space (top-left origin of the page as shown,
// i.e. after /Rotate) to PDF user space of a page with |media| and |rotate|.
PdfRect display_to_user(const Box &b, const PdfRect &media, int rotate);

// The objects of a PDF as `qpdf --json=2 --json-key=pages --json-key=qpdf` prints them.
class PdfObjects {
 public:
  bool load(const std::string &json_text, std::string &error);
  int max_id() const { return max_id_; }
  int page_count() const { return int(pages_.size()); }
  const std::string &page_ref(int i) const { return pages_[size_t(i)]; }
  const Json *object(const std::string &ref) const;  // the "value" of "obj:<ref>"
  const std::string &root_ref() const { return root_; }
  const Json &trailer() const { return trailer_; }
  PdfRect media_box(int page) const;  // follows /Parent for inherited boxes
  int rotation(int page) const;

 private:
  int max_id_ = 0;
  std::vector<std::string> pages_;
  std::map<std::string, Json> objects_;
  std::string root_;
  Json trailer_;
};

// Collects changed and new objects and writes the --update-from-json file.
class PdfUpdate {
 public:
  explicit PdfUpdate(const PdfObjects &src) : src_(src), next_(src.max_id() + 1) {}
  std::string add(Json value);                                  // returns "N 0 R"
  std::string add_stream(Json dict, const std::string &data);  // raw stream data
  Json value(const std::string &ref) const;  // the current value (changed or original)
  void set(const std::string &ref, Json value);
  void set_trailer(Json value);
  std::string dump() const;
  bool empty() const { return changed_.empty() && !trailer_set_; }

 private:
  const PdfObjects &src_;
  int next_;
  std::vector<std::pair<std::string, Json>> changed_;
  bool trailer_set_ = false;
  Json trailer_;
};

enum class FieldKind { Text, Check, Radio, Combo, List, Button, Signature };
struct FieldSpec {
  FieldKind kind = FieldKind::Text;
  std::string name;                  // fully qualified field name (/T)
  std::vector<std::string> options;  // choice items
  std::string label;                 // button caption
  bool multiline = false;
};

// Appends an annotation object to a page's /Annots (inline or indirect).
void append_annot(PdfUpdate &up, const PdfObjects &src, int page, const std::string &annot_ref);
// A link annotation to a URI, or to a page (0 based) when |uri| is empty.
bool plan_link(PdfUpdate &up, const PdfObjects &src, int page, const Box &box, const std::string &uri,
               int dest_page);
// A new AcroForm field with its widget on |page|.
bool plan_field(PdfUpdate &up, const PdfObjects &src, int page, const Box &box, const FieldSpec &spec);
// Sets /CropBox of |pages| to |box| (page space of each page).
bool plan_crop(PdfUpdate &up, const PdfObjects &src, const std::vector<int> &pages, const Box &box);
// Drops the document information dictionary and the XMP metadata stream.
bool plan_strip_metadata(PdfUpdate &up, const PdfObjects &src);
// qpdf JSON string forms: "u:text" for strings, "/Name" for names.
std::string pdf_text(const std::string &s);
std::string pdf_name(const std::string &s);

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
