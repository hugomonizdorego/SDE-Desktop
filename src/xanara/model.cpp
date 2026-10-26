// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Xanara's UI independent model; see model.h.
#include "model.h"

#include <cairo-pdf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef XANARA_HAVE_JPEG
#include <jpeglib.h>
#endif

#include "i18n.h"
#include "util.h"

using namespace sde;

namespace xanara {

// ------------------------------------------------------------ modes, tools

const char *mode_label(Mode m) {
  switch (m) {
    case Mode::Annotate: return _("Annotate");
    case Mode::Edit: return _("Edit");
    case Mode::Page: return _("Page");
    case Mode::Form: return _("Form");
    case Mode::Fill: return _("Fill");
    case Mode::Convert: return _("Convert");
    case Mode::Protect: return _("Protect");
    case Mode::Tools: return _("Tools");
  }
  return "";
}

const char *mode_id(Mode m) {
  static const char *ids[] = {"annotate", "edit", "page", "form", "fill", "convert", "protect", "tools"};
  return ids[int(m)];
}

bool mode_from_id(const std::string &id, Mode &m) {
  for (int k = 0; k < kModeCount; ++k)
    if (id == mode_id(Mode(k))) {
      m = Mode(k);
      return true;
    }
  return false;
}

bool have_native_jpeg() {
#ifdef XANARA_HAVE_JPEG
  return true;
#else
  return false;
#endif
}

const std::vector<ToolSpec> &tool_registry() {
  static const std::vector<ToolSpec> reg = [] {
    using I = icon::Kind;
    std::vector<ToolSpec> r;
    auto add = [&](Act a, Mode m, const char *id, const char *label, const char *tip, I g, bool canvas, int group) {
      r.push_back(ToolSpec{a, m, id, label, tip, g, canvas, group});
    };
    // Annotate
    Mode M = Mode::Annotate;
    add(Act::Highlight, M, "annotate.highlight", N_("Highlight"), N_("Highlight text (H)"), I::Highlight, true, 0);
    add(Act::Underline, M, "annotate.underline", N_("Underline"), N_("Underline text (U)"), I::Underline, true, 0);
    add(Act::Squiggly, M, "annotate.squiggly", N_("Squiggly"), N_("Squiggly underline (Q)"), I::Squiggly, true, 0);
    add(Act::Strike, M, "annotate.strike", N_("Strike Out"), N_("Strike out text (K)"), I::Strike, true, 0);
    add(Act::TextBox, M, "annotate.textbox", N_("Text Box"), N_("Text box: drag a box, then type (T)"), I::TextBox, true, 1);
    add(Act::Note, M, "annotate.note", N_("Note"), N_("Sticky note (N)"), I::Note, true, 1);
    add(Act::Pen, M, "annotate.pen", N_("Pen"), N_("Freehand pen (P)"), I::Pen, true, 2);
    add(Act::Eraser, M, "annotate.eraser", N_("Eraser"), N_("Eraser: click or drag over annotations to remove them (E)"),
        I::Eraser, true, 2);
    add(Act::Rect, M, "annotate.rect", N_("Rectangle"), N_("Rectangle (R)"), I::Rect, true, 3);
    add(Act::Ellipse, M, "annotate.ellipse", N_("Ellipse"), N_("Ellipse (O)"), I::Ellipse, true, 3);
    add(Act::Arrow, M, "annotate.arrow", N_("Arrow"), N_("Arrow (A)"), I::Arrow, true, 3);
    add(Act::Line, M, "annotate.line", N_("Line"), N_("Line (L)"), I::Line, true, 3);
    add(Act::Measure, M, "annotate.measure", N_("Measure"), N_("Measure a distance on the page (M)"), I::Measure, true, 3);
    add(Act::Stamp, M, "annotate.stamp", N_("Stamp"), N_("Stamp: Approved, Draft, Confidential…"), I::Stamp, true, 4);
    add(Act::Signature, M, "annotate.signature", N_("Signature"), N_("Place your signature (G)"), I::Signature, true, 4);
    add(Act::ToggleAnnots, M, "annotate.visibility", N_("Show/Hide"), N_("Show or hide annotations"), I::Eye, false, 5);
    // Edit
    M = Mode::Edit;
    add(Act::EditContent, M, "edit.content", N_("Edit"),
        N_("Edit text and images: click into text and type, drag pictures"), I::EditText, true, 0);
    add(Act::AddText, M, "edit.text", N_("Text"), N_("Add text: click on the page and type"), I::AddText, true, 0);
    add(Act::AddImage, M, "edit.image", N_("Image"), N_("Add an image: choose a file, then click or drag on the page"),
        I::Image, true, 0);
    add(Act::AddLink, M, "edit.link", N_("Link"), N_("Add a link: drag over the area that should be clickable"), I::Link,
        true, 1);
    add(Act::Watermark, M, "edit.watermark", N_("Watermark"), N_("Add a text watermark to every page"), I::Watermark,
        false, 2);
    add(Act::Background, M, "edit.background", N_("Background"), N_("Add a background color behind every page"),
        I::Background, false, 2);
    add(Act::HeaderFooter, M, "edit.headerfooter", N_("Header & Footer"), N_("Add headers and footers"),
        I::HeaderFooter, false, 2);
    add(Act::Bates, M, "edit.bates", N_("Bates"), N_("Add Bates numbers"), I::Bates, false, 2);
    add(Act::Crop, M, "edit.crop", N_("Crop"), N_("Crop pages: drag the area to keep"), I::Crop, true, 3);
    // Page
    M = Mode::Page;
    add(Act::PageInsert, M, "page.insert", N_("Insert"), N_("Insert pages from another PDF after this page"), I::Insert,
        false, 0);
    add(Act::PageExtract, M, "page.extract", N_("Extract"), N_("Save some pages as a new PDF"), I::Extract, false, 0);
    add(Act::PageSplit, M, "page.split", N_("Split"), N_("Split the document into several PDFs"), I::Split, false, 0);
    add(Act::PageMerge, M, "page.merge", N_("Merge"), N_("Append other PDFs to this document"), I::Merge, false, 0);
    add(Act::PageRotateLeft, M, "page.rotateleft", N_("Rotate Left"), N_("Rotate this page to the left"),
        I::RotateLeft, false, 1);
    add(Act::PageRotateRight, M, "page.rotateright", N_("Rotate Right"), N_("Rotate this page to the right"),
        I::RotateRight, false, 1);
    add(Act::PageDelete, M, "page.delete", N_("Delete"), N_("Delete pages"), I::Trash, false, 1);
    add(Act::PageMoveUp, M, "page.moveup", N_("Move Up"), N_("Move this page up (or drag thumbnails)"), I::Up, false, 2);
    add(Act::PageMoveDown, M, "page.movedown", N_("Move Down"), N_("Move this page down (or drag thumbnails)"), I::Down,
        false, 2);
    add(Act::PageNumbers, M, "page.numbers", N_("Page Numbers"), N_("Print page numbers on every page"),
        I::PageNumbers, false, 3);
    // Form
    M = Mode::Form;
    add(Act::FieldText, M, "form.text", N_("Text Field"), N_("Text field: drag its box on the page"), I::FieldText, true,
        0);
    add(Act::FieldCheck, M, "form.check", N_("Check Box"), N_("Check box: click on the page"), I::FieldCheck, true, 0);
    add(Act::FieldRadio, M, "form.radio", N_("Radio"), N_("Radio button: click on the page"), I::FieldRadio, true, 0);
    add(Act::FieldCombo, M, "form.combo", N_("Combo Box"), N_("Drop-down list: drag its box"), I::FieldCombo, true, 0);
    add(Act::FieldList, M, "form.list", N_("List Box"), N_("List box: drag its box"), I::FieldList, true, 0);
    add(Act::FieldButton, M, "form.button", N_("Button"), N_("Push button: drag its box"), I::FieldButton, true, 0);
    add(Act::FieldSignature, M, "form.signature", N_("Signature Field"), N_("Signature field: drag its box"),
        I::FieldSign, true, 0);
    add(Act::ShowFields, M, "form.highlight", N_("Highlight Fields"), N_("Highlight the form fields"), I::Eye, false, 1);
    // Fill
    M = Mode::Fill;
    add(Act::FillText, M, "fill.text", N_("Text"), N_("Type text anywhere on the page"), I::AddText, true, 0);
    add(Act::FillCheck, M, "fill.check", N_("Check"), N_("Place a check mark"), I::Check, true, 0);
    add(Act::FillCross, M, "fill.cross", N_("Cross"), N_("Place a cross"), I::Cross, true, 0);
    add(Act::FillDot, M, "fill.dot", N_("Dot"), N_("Place a dot"), I::Dot, true, 0);
    add(Act::FillDate, M, "fill.date", N_("Date"), N_("Place today's date"), I::Calendar, true, 1);
    add(Act::FillInitials, M, "fill.initials", N_("Initials"), N_("Place your initials"), I::Initials, true, 1);
    add(Act::FillSign, M, "fill.sign", N_("Sign"), N_("Place your signature"), I::Signature, true, 1);
    // Convert
    M = Mode::Convert;
    add(Act::ToPng, M, "convert.png", N_("To PNG"), N_("Export pages as PNG images"), I::ExportImage, false, 0);
    add(Act::ToJpeg, M, "convert.jpeg", N_("To JPEG"), N_("Export pages as JPEG images"), I::ExportImage, false, 0);
    add(Act::ToText, M, "convert.text", N_("To Text"), N_("Export the text as a plain text file"), I::ExportText, false, 1);
    add(Act::ToHtml, M, "convert.html", N_("To HTML"), N_("Export the text as an HTML page"), I::Html, false, 1);
    add(Act::ToMarkdown, M, "convert.markdown", N_("To Markdown"), N_("Export the text as Markdown"), I::Markdown,
        false, 1);
    add(Act::ImagesToPdf, M, "convert.images", N_("Images to PDF"), N_("Make a PDF from images"), I::ImagesToPdf, false,
        2);
    add(Act::MergeFiles, M, "convert.merge", N_("Merge PDFs"), N_("Combine several PDFs into a new one"), I::Merge,
        false, 2);
    // Protect
    M = Mode::Protect;
    add(Act::Encrypt, M, "protect.encrypt", N_("Password"), N_("Protect with a password and set permissions"), I::Lock,
        false, 0);
    add(Act::Decrypt, M, "protect.decrypt", N_("Remove Password"), N_("Remove the password and the restrictions"),
        I::Unlock, false, 0);
    add(Act::Redact, M, "protect.redact", N_("Mark Redaction"), N_("Mark areas to black out: drag over text or images"),
        I::Redact, true, 1);
    add(Act::ApplyRedactions, M, "protect.apply", N_("Apply Redactions"),
        N_("Black out the marked areas for good: the text, pictures and drawings under them are removed"),
        I::Check, false, 1);
    add(Act::RemoveMetadata, M, "protect.metadata", N_("Remove Metadata"),
        N_("Remove the title, author and other metadata"), I::Shield, false, 2);
    // Tools
    M = Mode::Tools;
    add(Act::Compress, M, "tools.compress", N_("Compress"), N_("Make the file smaller"), I::Compress, false, 0);
    add(Act::Ocr, M, "tools.ocr", N_("OCR"), N_("Recognize text in scanned pages"), I::Ocr, false, 0);
    add(Act::Compare, M, "tools.compare", N_("Compare"), N_("Compare with another PDF side by side"), I::Compare, false,
        0);
    add(Act::Print, M, "tools.print", N_("Print"), N_("Print the document (Ctrl+P)"), I::Print, false, 1);
    return r;
  }();
  return reg;
}

std::vector<const ToolSpec *> tools_for(Mode m) {
  std::vector<const ToolSpec *> out;
  for (const auto &t : tool_registry())
    if (t.mode == m) out.push_back(&t);
  return out;
}

const ToolSpec *tool_spec(Act a) {
  for (const auto &t : tool_registry())
    if (t.act == a) return &t;
  return nullptr;
}

const ToolSpec *tool_by_id(const std::string &id) {
  for (const auto &t : tool_registry())
    if (id == t.id) return &t;
  return nullptr;
}

int tools_that_fit(const std::vector<const ToolSpec *> &tools, int width, int button, int sep, int overflow) {
  auto need = [&](size_t n) {
    int w = 0;
    for (size_t k = 0; k < n; ++k) {
      if (k > 0 && tools[k]->group != tools[k - 1]->group) w += sep;
      w += button;
    }
    return w;
  };
  if (need(tools.size()) <= width) return int(tools.size());
  int n = 0;
  while (size_t(n) < tools.size() && need(size_t(n) + 1) <= width - overflow) ++n;
  return n;
}

// ---------------------------------------------------------------- tabs

int TabList::index_of_id(int id) const {
  for (size_t k = 0; k < ids_.size(); ++k)
    if (ids_[k] == id) return int(k);
  return -1;
}

int TabList::find(const std::string &path) const {
  for (size_t k = 0; k < paths_.size(); ++k)
    if (paths_[k] == path) return int(k);
  return -1;
}

int TabList::open(const std::string &path, bool *existing) {
  int k = find(path);
  if (existing) *existing = k >= 0;
  if (k >= 0) {
    active_ = k;
    return k;
  }
  int at = active_ < 0 ? count() : active_ + 1;
  paths_.insert(paths_.begin() + at, path);
  ids_.insert(ids_.begin() + at, next_id_++);
  active_ = at;
  return at;
}

int TabList::close(int i) {
  if (i < 0 || i >= count()) return active_;
  paths_.erase(paths_.begin() + i);
  ids_.erase(ids_.begin() + i);
  if (active_ == i) active_ = paths_.empty() ? -1 : std::min(i, count() - 1);
  else if (active_ > i) --active_;
  return active_;
}

bool TabList::move(int from, int to) {
  if (from < 0 || from >= count() || to < 0 || to >= count() || from == to) return false;
  std::string p = paths_[size_t(from)];
  int id = ids_[size_t(from)];
  paths_.erase(paths_.begin() + from);
  ids_.erase(ids_.begin() + from);
  paths_.insert(paths_.begin() + to, p);
  ids_.insert(ids_.begin() + to, id);
  if (active_ == from) active_ = to;
  else if (from < active_ && to >= active_) --active_;
  else if (from > active_ && to <= active_) ++active_;
  return true;
}

void TabList::activate(int i) { active_ = (i >= -1 && i < count()) ? i : active_; }

void TabList::rename(int i, const std::string &path) {
  if (i >= 0 && i < count()) paths_[size_t(i)] = path;
}

std::string TabList::serialize() const {
  std::string out;
  for (int k = 0; k < count(); ++k) out += (k == active_ ? "*" : " ") + paths_[size_t(k)] + "\n";
  return out;
}

bool TabList::parse(const std::string &text, std::vector<std::string> &paths, int &active) {
  paths.clear();
  active = -1;
  for (const auto &line : split(text, '\n')) {
    if (line.size() < 2 || line[1] != '/') continue;
    if (line[0] == '*') active = int(paths.size());
    else if (line[0] != ' ') continue;
    paths.push_back(line.substr(1));
  }
  return !paths.empty();
}

int tab_drop_index(int x, int x0, int tab_w, int count, int from) {
  if (count <= 0 || tab_w <= 0) return from;
  int idx = (x - x0) / tab_w;
  if (x < x0) idx = 0;
  return std::clamp(idx, 0, count - 1);
}

// ------------------------------------------------------------- exporters

std::string html_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

// Paragraphs of a page: blank lines separate them, single breaks join.
static std::vector<std::string> paragraphs(const std::string &text) {
  std::vector<std::string> out;
  std::string cur;
  for (const auto &line0 : split(text, '\n', true)) {
    std::string line = trim(line0);
    if (line.empty()) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    if (!cur.empty()) {
      if (cur.back() == '-' && cur.size() > 1 && isalpha((unsigned char)cur[cur.size() - 2])) cur.pop_back();
      else cur += ' ';
    }
    cur += line;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string export_html(const std::string &title, const std::vector<std::string> &pages) {
  std::string out = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>" + html_escape(title) +
                    "</title>\n<style>body{font-family:sans-serif;max-width:46em;margin:2em auto;line-height:1.5;"
                    "padding:0 1em}section{border-bottom:1px solid #ccc;padding-bottom:1em}"
                    "h2{font-size:.9em;color:#777}</style>\n</head>\n<body>\n<h1>" +
                    html_escape(title) + "</h1>\n";
  for (size_t p = 0; p < pages.size(); ++p) {
    out += format("<section id=\"page-%zu\">\n<h2>Page %zu</h2>\n", p + 1, p + 1);
    for (const auto &para : paragraphs(pages[p])) out += "<p>" + html_escape(para) + "</p>\n";
    out += "</section>\n";
  }
  return out + "</body>\n</html>\n";
}

static std::string md_escape(const std::string &s) {
  std::string out;
  for (size_t k = 0; k < s.size(); ++k) {
    char c = s[k];
    if (strchr("\\`*_[]<>|", c)) out += '\\';
    // A line that starts like a list or a heading must not become one.
    if (k == 0 && (c == '#' || c == '-' || c == '+' || c == '>')) out += '\\';
    out += c;
  }
  return out;
}

std::string export_markdown(const std::string &title, const std::vector<std::string> &pages) {
  std::string out = "# " + md_escape(title) + "\n";
  for (size_t p = 0; p < pages.size(); ++p) {
    if (out.size() < 2 || out.compare(out.size() - 2, 2, "\n\n") != 0) out += "\n";
    out += format("## Page %zu\n\n", p + 1);
    for (const auto &para : paragraphs(pages[p])) out += md_escape(para) + "\n\n";
  }
  while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
  return out;
}

std::string export_plain(const std::vector<std::string> &pages) {
  std::string out;
  for (size_t p = 0; p < pages.size(); ++p) {
    if (p) out += "\f";
    out += pages[p];
    if (!pages[p].empty() && pages[p].back() != '\n') out += "\n";
  }
  return out;
}

std::string numbered_name(const std::string &pdf_path, const std::string &ext, int page, int count) {
  std::string base = pdf_path;
  if (ends_with(lower(base), ".pdf")) base = base.substr(0, base.size() - 4);
  int digits = std::max(2, int(std::to_string(std::max(1, count)).size()));
  return format("%s-%0*d.%s", base.c_str(), digits, page + 1, ext.c_str());
}

// --------------------------------------------------------- generated PDFs

std::string bates_number(const StampSpec &spec, int page) {
  return spec.prefix + format("%0*d", std::clamp(spec.digits, 1, 12), spec.start + page);
}

std::string expand_stamp(const std::string &tpl, const StampSpec &spec, int page, int count) {
  std::string out;
  for (size_t k = 0; k < tpl.size();) {
    if (tpl[k] == '{') {
      size_t e = tpl.find('}', k);
      if (e != std::string::npos) {
        std::string key = tpl.substr(k + 1, e - k - 1), v;
        bool known = true;
        if (key == "page") v = std::to_string(page + 1);
        else if (key == "pages") v = std::to_string(count);
        else if (key == "date") v = spec.date;
        else if (key == "file") v = spec.file_name;
        else if (key == "bates") v = bates_number(spec, page);
        else known = false;
        if (known) {
          out += v;
          k = e + 1;
          continue;
        }
      }
    }
    out += tpl[k++];
  }
  return out;
}

static void show_at(cairo_t *cr, const std::string &s, double x, double y, int align) {
  cairo_text_extents_t te;
  cairo_text_extents(cr, s.c_str(), &te);
  double tx = align == 0 ? x : align == 1 ? x - te.x_advance / 2 : x - te.x_advance;
  cairo_move_to(cr, tx, y);
  cairo_show_text(cr, s.c_str());
}

bool write_stamp_pdf(const std::string &path, const std::vector<Point> &sizes, const StampSpec &spec,
                     std::string &error) {
  if (sizes.empty()) {
    error = "no pages";
    return false;
  }
  cairo_surface_t *s = cairo_pdf_surface_create(path.c_str(), sizes[0].x, sizes[0].y);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    error = cairo_status_to_string(cairo_surface_status(s));
    cairo_surface_destroy(s);
    return false;
  }
  cairo_t *cr = cairo_create(s);
  int n = int(sizes.size());
  for (int p = 0; p < n; ++p) {
    double w = sizes[size_t(p)].x, h = sizes[size_t(p)].y;
    cairo_pdf_surface_set_size(s, w, h);
    cairo_save(cr);
    cairo_set_source_rgba(cr, spec.color.r, spec.color.g, spec.color.b, std::clamp(spec.opacity, 0.0, 1.0));
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    switch (spec.kind) {
      case StampSpec::Watermark: {
        std::string t = expand_stamp(spec.text, spec, p, n);
        cairo_set_font_size(cr, spec.font_size);
        cairo_text_extents_t te;
        cairo_text_extents(cr, t.c_str(), &te);
        cairo_translate(cr, w / 2, h / 2);
        cairo_rotate(cr, -spec.angle * M_PI / 180);
        cairo_move_to(cr, -te.x_advance / 2, te.height / 2);
        cairo_show_text(cr, t.c_str());
        break;
      }
      case StampSpec::Background:
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);
        break;
      case StampSpec::HeaderFooter:
      case StampSpec::Bates:
      case StampSpec::PageNumbers: {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, spec.font_size);
        double m = 30, top = m + spec.font_size * 0.4, bottom = h - m + spec.font_size * 0.3;
        if (spec.kind == StampSpec::HeaderFooter) {
          for (int k = 0; k < 3; ++k) {
            double x = k == 0 ? m * 1.4 : k == 1 ? w / 2 : w - m * 1.4;
            show_at(cr, expand_stamp(spec.header[k], spec, p, n), x, top, k);
            show_at(cr, expand_stamp(spec.footer[k], spec, p, n), x, bottom, k);
          }
        } else if (spec.kind == StampSpec::Bates) {
          show_at(cr, bates_number(spec, p), w - m * 1.4, bottom, 2);
        } else {
          std::string tpl = spec.footer[1].empty() ? std::string("{page}") : spec.footer[1];
          show_at(cr, expand_stamp(tpl, spec, p, n), w / 2, bottom, 1);
        }
        break;
      }
    }
    cairo_restore(cr);
    cairo_show_page(cr);
  }
  cairo_destroy(cr);
  cairo_surface_finish(s);
  bool ok = cairo_surface_status(s) == CAIRO_STATUS_SUCCESS;
  if (!ok) error = cairo_status_to_string(cairo_surface_status(s));
  cairo_surface_destroy(s);
  return ok;
}

bool images_to_pdf(const std::vector<cairo_surface_t *> &images, const std::string &path, bool a4,
                   std::string &error) {
  if (images.empty()) {
    error = "no images";
    return false;
  }
  cairo_surface_t *s = cairo_pdf_surface_create(path.c_str(), 595, 842);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    error = cairo_status_to_string(cairo_surface_status(s));
    cairo_surface_destroy(s);
    return false;
  }
  cairo_t *cr = cairo_create(s);
  for (cairo_surface_t *img : images) {
    if (!img) continue;
    double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    if (iw < 1 || ih < 1) continue;
    double pw, ph, k, ox = 0, oy = 0;
    if (a4) {
      bool land = iw > ih;
      pw = land ? 842 : 595;
      ph = land ? 595 : 842;
      double m = 24;
      k = std::min((pw - 2 * m) / iw, (ph - 2 * m) / ih);
      ox = (pw - iw * k) / 2;
      oy = (ph - ih * k) / 2;
    } else {
      k = 72.0 / 96.0;
      pw = iw * k;
      ph = ih * k;
    }
    cairo_pdf_surface_set_size(s, pw, ph);
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_show_page(cr);
  }
  cairo_destroy(cr);
  cairo_surface_finish(s);
  bool ok = cairo_surface_status(s) == CAIRO_STATUS_SUCCESS;
  if (!ok) error = cairo_status_to_string(cairo_surface_status(s));
  cairo_surface_destroy(s);
  return ok;
}

static cairo_surface_t *render_page(Document &doc, int page, double dpi) {
  RenderParams rp;
  rp.scale = dpi / 72.0;
  int w, h;
  page_pixels(doc.width(page), doc.height(page), rp, w, h);
  return doc.render(page, rp, 0, 0, w, h);
}

bool rasterize_page_pdf(Document &doc, int page, double dpi, const std::vector<Box> &black, const std::string &path,
                        std::string &error) {
  if (page < 0 || page >= doc.count()) {
    error = "no such page";
    return false;
  }
  cairo_surface_t *img = render_page(doc, page, dpi);
  if (!img) {
    error = "the page could not be rendered";
    return false;
  }
  double k = dpi / 72.0;
  {
    cairo_t *cr = cairo_create(img);
    cairo_set_source_rgb(cr, 0, 0, 0);
    for (const auto &b0 : black) {
      Box b = b0.normalized();
      cairo_rectangle(cr, std::floor(b.x1 * k), std::floor(b.y1 * k), std::ceil(b.w() * k) + 1,
                      std::ceil(b.h() * k) + 1);
    }
    cairo_fill(cr);
    cairo_destroy(cr);
  }
  cairo_surface_t *s = cairo_pdf_surface_create(path.c_str(), doc.width(page), doc.height(page));
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, 1 / k, 1 / k);
  cairo_set_source_surface(cr, img, 0, 0);
  cairo_paint(cr);
  cairo_show_page(cr);
  cairo_destroy(cr);
  cairo_surface_finish(s);
  bool ok = cairo_surface_status(s) == CAIRO_STATUS_SUCCESS;
  if (!ok) error = cairo_status_to_string(cairo_surface_status(s));
  cairo_surface_destroy(s);
  cairo_surface_destroy(img);
  return ok;
}

Strokes arrow_strokes(Point a, Point b, double head) {
  double ang = std::atan2(b.y - a.y, b.x - a.x);
  auto wing = [&](double d) { return Point{b.x - head * std::cos(ang + d), b.y - head * std::sin(ang + d)}; };
  return {{a, b}, {wing(0.45), b, wing(-0.45)}};
}

Strokes check_strokes(Point c, double s) {
  return {{Point{c.x - s * 0.45, c.y}, Point{c.x - s * 0.12, c.y + s * 0.35}, Point{c.x + s * 0.5, c.y - s * 0.4}}};
}

Strokes cross_strokes(Point c, double s) {
  double r = s * 0.4;
  return {{Point{c.x - r, c.y - r}, Point{c.x + r, c.y + r}}, {Point{c.x - r, c.y + r}, Point{c.x + r, c.y - r}}};
}

Strokes dot_strokes(Point c, double s) {
  Strokes out(1);
  double r = s * 0.15;
  for (int k = 0; k <= 12; ++k) out[0].push_back(Point{c.x + r * std::cos(k * M_PI / 6), c.y + r * std::sin(k * M_PI / 6)});
  return out;
}

cairo_surface_t *stamp_image(const std::string &text, Rgb color, double w, double h) {
  const double k = 4.0;
  int iw = std::max(1, int(std::ceil(w * k))), ih = std::max(1, int(std::ceil(h * k)));
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, k, k);
  double lw = std::max(1.5, h * 0.07), r = h * 0.18;
  double x = lw, y = lw, bw = w - 2 * lw, bh = h - 2 * lw;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + bw - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + bw - r, y + bh - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + bh - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, color.r, color.g, color.b, 0.08);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, color.r, color.g, color.b, 0.9);
  cairo_set_line_width(cr, lw);
  cairo_stroke(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  double fs = h * 0.5;
  cairo_set_font_size(cr, fs);
  cairo_text_extents_t te;
  cairo_text_extents(cr, text.c_str(), &te);
  if (te.width > bw * 0.88) {
    fs *= bw * 0.88 / te.width;
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, text.c_str(), &te);
  }
  cairo_move_to(cr, (w - te.width) / 2 - te.x_bearing, (h - te.height) / 2 - te.y_bearing);
  cairo_show_text(cr, text.c_str());
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}

cairo_surface_t *label_image(const std::string &text, Rgb color, double font_size, double &w, double &h) {
  cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *pc = cairo_create(probe);
  cairo_select_font_face(pc, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(pc, font_size);
  cairo_font_extents_t fe;
  cairo_font_extents(pc, &fe);
  auto lines = split(text, '\n', true);
  double maxw = 1;
  for (const auto &l : lines) {
    cairo_text_extents_t te;
    cairo_text_extents(pc, l.c_str(), &te);
    maxw = std::max(maxw, te.x_advance);
  }
  cairo_destroy(pc);
  cairo_surface_destroy(probe);
  w = maxw + 4;
  h = fe.height * double(std::max<size_t>(1, lines.size())) + 4;
  const double k = 4.0;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(std::ceil(w * k)), int(std::ceil(h * k)));
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, k, k);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, font_size);
  cairo_set_source_rgb(cr, color.r, color.g, color.b);
  double y = 2 + fe.ascent;
  for (const auto &l : lines) {
    cairo_move_to(cr, 2, y);
    cairo_show_text(cr, l.c_str());
    y += fe.height;
  }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}

std::string measure_label(double points, const std::string &unit) {
  if (unit == "pt") return format("%.0f pt", points);
  if (unit == "in") return format("%.2f in", points / 72.0);
  if (unit == "cm") return format("%.2f cm", points / 72.0 * 2.54);
  return format("%.1f mm", points / 72.0 * 25.4);
}

#ifdef XANARA_HAVE_JPEG
static bool write_jpeg(cairo_surface_t *img, const std::string &path, int quality, std::string &error) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) {
    error = strerror(errno);
    return false;
  }
  jpeg_compress_struct c;
  jpeg_error_mgr jerr;
  c.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&c);
  jpeg_stdio_dest(&c, f);
  int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
  c.image_width = JDIMENSION(w);
  c.image_height = JDIMENSION(h);
  c.input_components = 3;
  c.in_color_space = JCS_RGB;
  jpeg_set_defaults(&c);
  jpeg_set_quality(&c, quality, TRUE);
  jpeg_start_compress(&c, TRUE);
  cairo_surface_flush(img);
  const unsigned char *data = cairo_image_surface_get_data(img);
  int stride = cairo_image_surface_get_stride(img);
  std::vector<unsigned char> row(size_t(w) * 3);
  while (c.next_scanline < c.image_height) {
    const uint32_t *px = reinterpret_cast<const uint32_t *>(data + size_t(c.next_scanline) * size_t(stride));
    for (int x = 0; x < w; ++x) {
      row[size_t(x) * 3] = (px[x] >> 16) & 255;
      row[size_t(x) * 3 + 1] = (px[x] >> 8) & 255;
      row[size_t(x) * 3 + 2] = px[x] & 255;
    }
    JSAMPROW r = row.data();
    jpeg_write_scanlines(&c, &r, 1);
  }
  jpeg_finish_compress(&c);
  jpeg_destroy_compress(&c);
  return fclose(f) == 0;
}
#endif

bool export_page_image(Document &doc, int page, double dpi, const std::string &path, bool jpeg, std::string &error) {
  cairo_surface_t *img = render_page(doc, page, dpi);
  if (!img) {
    error = "the page could not be rendered";
    return false;
  }
  bool ok;
  if (jpeg) {
#ifdef XANARA_HAVE_JPEG
    ok = write_jpeg(img, path, 90, error);
#else
    error = "JPEG support is not built in";
    ok = false;
#endif
  } else {
    cairo_status_t st = cairo_surface_write_to_png(img, path.c_str());
    ok = st == CAIRO_STATUS_SUCCESS;
    if (!ok) error = cairo_status_to_string(st);
  }
  cairo_surface_destroy(img);
  return ok;
}

}  // namespace xanara
