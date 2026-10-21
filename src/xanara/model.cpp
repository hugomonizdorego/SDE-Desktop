// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
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
    auto add = [&](Act a, Mode m, const char *id, const char *label, const char *tip, I g, bool canvas, int group,
                   std::vector<const char *> needs = {}, std::vector<const char *> any = {},
                   const char *package = nullptr) {
      r.push_back(ToolSpec{a, m, id, label, tip, g, canvas, group, std::move(needs), std::move(any), package});
    };
    const char *QPDF = "qpdf";
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
    add(Act::AddText, M, "edit.text", N_("Text"), N_("Add text: click on the page and type"), I::AddText, true, 0);
    add(Act::AddImage, M, "edit.image", N_("Image"), N_("Add an image: choose a file, then click or drag on the page"),
        I::Image, true, 0);
    add(Act::AddLink, M, "edit.link", N_("Link"), N_("Add a link: drag over the area that should be clickable"), I::Link,
        true, 0, {QPDF}, {}, "qpdf");
    add(Act::Watermark, M, "edit.watermark", N_("Watermark"), N_("Add a text watermark to every page"), I::Watermark,
        false, 1, {QPDF}, {}, "qpdf");
    add(Act::Background, M, "edit.background", N_("Background"), N_("Add a background color behind every page"),
        I::Background, false, 1, {QPDF}, {}, "qpdf");
    add(Act::HeaderFooter, M, "edit.headerfooter", N_("Header & Footer"), N_("Add headers and footers"),
        I::HeaderFooter, false, 1, {QPDF}, {}, "qpdf");
    add(Act::Bates, M, "edit.bates", N_("Bates"), N_("Add Bates numbers"), I::Bates, false, 1, {QPDF}, {}, "qpdf");
    add(Act::Crop, M, "edit.crop", N_("Crop"), N_("Crop pages: drag the area to keep"), I::Crop, true, 2, {QPDF}, {},
        "qpdf");
    // Page
    M = Mode::Page;
    add(Act::PageInsert, M, "page.insert", N_("Insert"), N_("Insert pages from another PDF after this page"), I::Insert,
        false, 0, {QPDF}, {}, "qpdf");
    add(Act::PageExtract, M, "page.extract", N_("Extract"), N_("Save some pages as a new PDF"), I::Extract, false, 0,
        {QPDF}, {}, "qpdf");
    add(Act::PageSplit, M, "page.split", N_("Split"), N_("Split the document into several PDFs"), I::Split, false, 0,
        {QPDF}, {}, "qpdf");
    add(Act::PageMerge, M, "page.merge", N_("Merge"), N_("Append other PDFs to this document"), I::Merge, false, 0,
        {QPDF}, {}, "qpdf");
    add(Act::PageRotateLeft, M, "page.rotateleft", N_("Rotate Left"), N_("Rotate this page to the left"),
        I::RotateLeft, false, 1, {QPDF}, {}, "qpdf");
    add(Act::PageRotateRight, M, "page.rotateright", N_("Rotate Right"), N_("Rotate this page to the right"),
        I::RotateRight, false, 1, {QPDF}, {}, "qpdf");
    add(Act::PageDelete, M, "page.delete", N_("Delete"), N_("Delete pages"), I::Trash, false, 1, {QPDF}, {}, "qpdf");
    add(Act::PageMoveUp, M, "page.moveup", N_("Move Up"), N_("Move this page up (or drag thumbnails)"), I::Up, false, 2,
        {QPDF}, {}, "qpdf");
    add(Act::PageMoveDown, M, "page.movedown", N_("Move Down"), N_("Move this page down (or drag thumbnails)"), I::Down,
        false, 2, {QPDF}, {}, "qpdf");
    add(Act::PageNumbers, M, "page.numbers", N_("Page Numbers"), N_("Print page numbers on every page"),
        I::PageNumbers, false, 3, {QPDF}, {}, "qpdf");
    // Form
    M = Mode::Form;
    add(Act::FieldText, M, "form.text", N_("Text Field"), N_("Text field: drag its box on the page"), I::FieldText, true,
        0, {QPDF}, {}, "qpdf");
    add(Act::FieldCheck, M, "form.check", N_("Check Box"), N_("Check box: click on the page"), I::FieldCheck, true, 0,
        {QPDF}, {}, "qpdf");
    add(Act::FieldRadio, M, "form.radio", N_("Radio"), N_("Radio button: click on the page"), I::FieldRadio, true, 0,
        {QPDF}, {}, "qpdf");
    add(Act::FieldCombo, M, "form.combo", N_("Combo Box"), N_("Drop-down list: drag its box"), I::FieldCombo, true, 0,
        {QPDF}, {}, "qpdf");
    add(Act::FieldList, M, "form.list", N_("List Box"), N_("List box: drag its box"), I::FieldList, true, 0, {QPDF}, {},
        "qpdf");
    add(Act::FieldButton, M, "form.button", N_("Button"), N_("Push button: drag its box"), I::FieldButton, true, 0,
        {QPDF}, {}, "qpdf");
    add(Act::FieldSignature, M, "form.signature", N_("Signature Field"), N_("Signature field: drag its box"),
        I::FieldSign, true, 0, {QPDF}, {}, "qpdf");
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
    if (have_native_jpeg())
      add(Act::ToJpeg, M, "convert.jpeg", N_("To JPEG"), N_("Export pages as JPEG images"), I::ExportImage, false, 0);
    else
      add(Act::ToJpeg, M, "convert.jpeg", N_("To JPEG"), N_("Export pages as JPEG images"), I::ExportImage, false, 0,
          {"pdftoppm"}, {}, "poppler-utils");
    add(Act::ToText, M, "convert.text", N_("To Text"), N_("Export the text as a plain text file"), I::ExportText, false, 1);
    add(Act::ToHtml, M, "convert.html", N_("To HTML"), N_("Export the text as an HTML page"), I::Html, false, 1);
    add(Act::ToMarkdown, M, "convert.markdown", N_("To Markdown"), N_("Export the text as Markdown"), I::Markdown,
        false, 1);
    add(Act::ImagesToPdf, M, "convert.images", N_("Images to PDF"), N_("Make a PDF from images"), I::ImagesToPdf, false,
        2);
    add(Act::MergeFiles, M, "convert.merge", N_("Merge PDFs"), N_("Combine several PDFs into a new one"), I::Merge,
        false, 2, {QPDF}, {}, "qpdf");
    // Protect
    M = Mode::Protect;
    add(Act::Encrypt, M, "protect.encrypt", N_("Password"), N_("Protect with a password and set permissions"), I::Lock,
        false, 0, {QPDF}, {}, "qpdf");
    add(Act::Decrypt, M, "protect.decrypt", N_("Remove Password"), N_("Remove the password and the restrictions"),
        I::Unlock, false, 0, {QPDF}, {}, "qpdf");
    add(Act::Redact, M, "protect.redact", N_("Mark Redaction"), N_("Mark areas to black out: drag over text or images"),
        I::Redact, true, 1);
    add(Act::ApplyRedactions, M, "protect.apply", N_("Apply Redactions"),
        N_("Black out the marked areas for good: those pages become images, so nothing under the boxes remains"),
        I::Check, false, 1, {QPDF}, {}, "qpdf");
    add(Act::RemoveMetadata, M, "protect.metadata", N_("Remove Metadata"),
        N_("Remove the title, author and other metadata"), I::Shield, false, 2, {QPDF}, {}, "qpdf");
    // Tools
    M = Mode::Tools;
    add(Act::Compress, M, "tools.compress", N_("Compress"), N_("Make the file smaller"), I::Compress, false, 0, {},
        {QPDF, "gs"}, "qpdf ghostscript");
    add(Act::Ocr, M, "tools.ocr", N_("OCR"), N_("Recognize text in scanned pages"), I::Ocr, false, 0,
        {"tesseract", QPDF}, {}, "tesseract-ocr qpdf");
    add(Act::Compare, M, "tools.compare", N_("Compare"), N_("Compare with another PDF side by side"), I::Compare, false,
        0);
    add(Act::Print, M, "tools.print", N_("Print"), N_("Print the document (Ctrl+P)"), I::Print, false, 1, {"lp"}, {},
        "cups-client");
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

Availability tool_availability(const ToolSpec &spec, const std::function<bool(const std::string &)> &have) {
  Availability a;
  for (const char *p : spec.needs)
    if (!have(p)) {
      a.ok = false;
      a.missing = p;
      break;
    }
  if (a.ok && !spec.any.empty()) {
    bool one = false;
    for (const char *p : spec.any) one = one || have(p);
    if (!one) {
      a.ok = false;
      a.missing = spec.any.front();
    }
  }
  if (!a.ok) {
    std::string pkg = spec.package ? spec.package : a.missing;
    a.reason = format(_("Needs %s — install it with: sudo apt install %s"), a.missing.c_str(), pkg.c_str());
  }
  return a;
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

// ------------------------------------------------------- command planning

Argv qpdf_base(const std::string &password) {
  Argv a{"qpdf", "--warning-exit-0"};
  if (!password.empty()) a.push_back("--password=" + password);
  return a;
}

Argv qpdf_encrypt_args(const std::string &in, const std::string &out, const std::string &user_pw,
                       const std::string &owner_pw, const Permissions &perms, const std::string &password) {
  Argv a = qpdf_base(password);
  a.push_back(in);
  a.push_back("--encrypt");
  a.push_back("--user-password=" + user_pw);
  a.push_back("--owner-password=" + (owner_pw.empty() ? user_pw : owner_pw));
  a.push_back("--bits=256");
  a.push_back(std::string("--print=") + (perms.print ? "full" : "none"));
  a.push_back(std::string("--modify=") + (perms.modify ? "all" : perms.annotate ? "annotate"
                                                              : perms.forms   ? "form"
                                                              : perms.assemble ? "assembly"
                                                                               : "none"));
  a.push_back(std::string("--extract=") + (perms.copy ? "y" : "n"));
  a.push_back(std::string("--annotate=") + (perms.annotate ? "y" : "n"));
  a.push_back(std::string("--form=") + (perms.forms ? "y" : "n"));
  a.push_back(std::string("--assemble=") + (perms.assemble ? "y" : "n"));
  a.push_back("--");
  a.push_back(out);
  return a;
}

Argv qpdf_decrypt_args(const std::string &in, const std::string &out, const std::string &password) {
  Argv a = qpdf_base(password);
  a.push_back("--decrypt");
  a.push_back(in);
  a.push_back(out);
  return a;
}

Argv qpdf_merge_args(const std::vector<std::string> &files, const std::string &out) {
  Argv a{"qpdf", "--warning-exit-0", "--empty", "--pages"};
  for (const auto &f : files) a.push_back(f);
  a.push_back("--");
  a.push_back(out);
  return a;
}

Argv qpdf_overlay_args(const std::string &in, const std::string &stamp, const std::string &out, bool underlay,
                       const std::string &password) {
  Argv a = qpdf_base(password);
  a.push_back(in);
  a.push_back(underlay ? "--underlay" : "--overlay");
  a.push_back(stamp);
  a.push_back("--");
  a.push_back(out);
  return a;
}

Argv qpdf_compress_args(const std::string &in, const std::string &out, const std::string &password) {
  Argv a = qpdf_base(password);
  for (const char *o : {"--object-streams=generate", "--compress-streams=y", "--recompress-flate",
                        "--compression-level=9", "--optimize-images", "--remove-unreferenced-resources=yes"})
    a.push_back(o);
  a.push_back(in);
  a.push_back(out);
  return a;
}

Argv qpdf_split_args(const std::string &in, const std::string &out_pattern, int pages_per_file,
                     const std::string &password) {
  Argv a = qpdf_base(password);
  a.push_back(format("--split-pages=%d", std::max(1, pages_per_file)));
  a.push_back(in);
  a.push_back(out_pattern);
  return a;
}

Argv qpdf_update_args(const std::string &in, const std::string &json, const std::string &out,
                      const std::string &password) {
  Argv a = qpdf_base(password);
  a.push_back(in);
  a.push_back(out);
  a.push_back("--update-from-json=" + json);
  return a;
}

Argv qpdf_json_args(const std::string &in, const std::string &password) {
  Argv a = qpdf_base(password);
  for (const char *o : {"--json=2", "--json-key=pages", "--json-key=qpdf"}) a.push_back(o);
  a.push_back(in);
  return a;
}

Argv gs_compress_args(const std::string &in, const std::string &out, const std::string &quality) {
  return {"gs", "-q", "-dNOPAUSE", "-dBATCH", "-dSAFER", "-sDEVICE=pdfwrite", "-dCompatibilityLevel=1.6",
          "-dPDFSETTINGS=/" + (quality.empty() ? std::string("ebook") : quality), "-sOutputFile=" + out, in};
}

Argv tesseract_args(const std::string &image, const std::string &out_base, const std::string &lang) {
  Argv a{"tesseract", image, out_base};
  if (!lang.empty()) {
    a.push_back("-l");
    a.push_back(lang);
  }
  a.push_back("pdf");
  return a;
}

Argv pdftoppm_jpeg_args(const std::string &in, int page, int dpi, const std::string &out_base,
                        const std::string &password) {
  Argv a{"pdftoppm", "-jpeg", "-r", std::to_string(dpi), "-f", std::to_string(page + 1), "-l",
         std::to_string(page + 1), "-singlefile"};
  if (!password.empty()) {
    a.push_back("-upw");
    a.push_back(password);
  }
  a.push_back(in);
  a.push_back(out_base);
  return a;
}

Argv lp_args(const std::string &file, const std::string &title) { return {"lp", "-t", title, file}; }

Argv redaction_splice_args(const std::string &in, int count, const std::map<int, std::string> &raster,
                           const std::string &out, const std::string &password) {
  Argv a = qpdf_base(password);
  a.push_back(in);
  a.push_back("--pages");
  int run = -1;  // first page of a run of untouched pages
  auto flush = [&](int end) {  // pages run..end-1 from the original
    if (run < 0 || end <= run) return;
    a.push_back(".");
    a.push_back(end - 1 == run ? std::to_string(run + 1) : format("%d-%d", run + 1, end));
    run = -1;
  };
  for (int p = 0; p < count; ++p) {
    auto it = raster.find(p);
    if (it == raster.end()) {
      if (run < 0) run = p;
      continue;
    }
    flush(p);
    a.push_back(it->second);
    a.push_back("1");
  }
  flush(count);
  a.push_back("--");
  a.push_back(out);
  return a;
}

Argv ocr_join_args(const std::vector<std::string> &page_pdfs, const std::string &out) {
  return qpdf_merge_args(page_pdfs, out);
}

// ------------------------------------------------------------------ JSON

Json Json::number(double v) {
  Json j;
  j.type_ = Number;
  j.num_ = v;
  return j;
}
Json Json::string(const std::string &s) {
  Json j;
  j.type_ = String;
  j.str_ = s;
  return j;
}
Json Json::boolean(bool b) {
  Json j;
  j.type_ = Boolean;
  j.bool_ = b;
  return j;
}
Json Json::array(std::vector<Json> items) {
  Json j;
  j.type_ = Array;
  j.items_ = std::move(items);
  return j;
}
Json Json::object() {
  Json j;
  j.type_ = Object;
  return j;
}

const Json *Json::get(const std::string &key) const {
  for (const auto &m : members_)
    if (m.first == key) return &m.second;
  return nullptr;
}

Json *Json::get(const std::string &key) {
  for (auto &m : members_)
    if (m.first == key) return &m.second;
  return nullptr;
}

Json &Json::set(const std::string &key, Json value) {
  type_ = Object;
  for (auto &m : members_)
    if (m.first == key) {
      m.second = std::move(value);
      return m.second;
    }
  members_.push_back({key, std::move(value)});
  return members_.back().second;
}

bool Json::erase(const std::string &key) {
  for (size_t k = 0; k < members_.size(); ++k)
    if (members_[k].first == key) {
      members_.erase(members_.begin() + long(k));
      return true;
    }
  return false;
}

namespace {

void dump_string(const std::string &s, std::string &out) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  out += '"';
}

void dump_value(const Json &j, std::string &out) {
  switch (j.type()) {
    case Json::Null: out += "null"; break;
    case Json::Boolean: out += j.boolean_value() ? "true" : "false"; break;
    case Json::Number: {
      double v = j.num();
      char buf[40];
      if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)std::llround(v));
      else snprintf(buf, sizeof buf, "%.6f", v);
      std::string s = buf;
      if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
      }
      out += s;
      break;
    }
    case Json::String: dump_string(j.str(), out); break;
    case Json::Array: {
      out += '[';
      bool first = true;
      for (const auto &i : j.items()) {
        if (!first) out += ',';
        first = false;
        dump_value(i, out);
      }
      out += ']';
      break;
    }
    case Json::Object: {
      out += '{';
      bool first = true;
      for (const auto &m : j.members()) {
        if (!first) out += ',';
        first = false;
        dump_string(m.first, out);
        out += ':';
        dump_value(m.second, out);
      }
      out += '}';
      break;
    }
  }
}

struct Parser {
  const std::string &s;
  size_t i = 0;
  std::string error;
  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
  }
  bool fail(const char *what) {
    if (error.empty()) error = format("%s at offset %zu", what, i);
    return false;
  }
  static void utf8(unsigned cp, std::string &out) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(unsigned &v) {
    if (i + 4 > s.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool str(std::string &out) {
    if (i >= s.size() || s[i] != '"') return fail("expected a string");
    ++i;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) return fail("bad escape");
      char e = s[i++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return fail("bad \\u escape");
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            i += 2;
            unsigned lo;
            if (!hex4(lo)) return fail("bad surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(cp, out);
          break;
        }
        default: out += e;
      }
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;
    return true;
  }
  bool value(Json &out, int depth) {
    if (depth > 200) return fail("too deep");
    ws();
    if (i >= s.size()) return fail("unexpected end");
    char c = s[i];
    if (c == '{') {
      ++i;
      out = Json::object();
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
        return true;
      }
      for (;;) {
        ws();
        std::string key;
        if (!str(key)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return fail("expected ':'");
        ++i;
        Json v;
        if (!value(v, depth + 1)) return false;
        out.set(key, std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == '}') {
          ++i;
          return true;
        }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i;
      out = Json::array();
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
        return true;
      }
      for (;;) {
        Json v;
        if (!value(v, depth + 1)) return false;
        out.push(std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == ']') {
          ++i;
          return true;
        }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      std::string t;
      if (!str(t)) return false;
      out = Json::string(t);
      return true;
    }
    if (s.compare(i, 4, "true") == 0) {
      i += 4;
      out = Json::boolean(true);
      return true;
    }
    if (s.compare(i, 5, "false") == 0) {
      i += 5;
      out = Json::boolean(false);
      return true;
    }
    if (s.compare(i, 4, "null") == 0) {
      i += 4;
      out = Json();
      return true;
    }
    size_t start = i;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || strchr("+-.eE", s[i]))) ++i;
    if (i == start) return fail("unexpected character");
    out = Json::number(strtod(s.substr(start, i - start).c_str(), nullptr));
    return true;
  }
};

std::string base64(const std::string &in) {
  static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
    out += tbl[v >> 18];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
  }
  if (i < in.size()) {
    unsigned v = (unsigned char)in[i] << 16 | (i + 1 < in.size() ? (unsigned char)in[i + 1] << 8 : 0);
    out += tbl[v >> 18];
    out += tbl[(v >> 12) & 63];
    out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

}  // namespace

bool Json::parse(const std::string &text, Json &out, std::string &error) {
  Parser p{text, 0, {}};
  if (!p.value(out, 0)) {
    error = p.error;
    return false;
  }
  p.ws();
  if (p.i != text.size()) {
    error = "trailing characters";
    return false;
  }
  return true;
}

std::string Json::dump() const {
  std::string out;
  dump_value(*this, out);
  return out;
}

// --------------------------------------------------- structural PDF edits

std::string pdf_text(const std::string &s) { return "u:" + s; }
std::string pdf_name(const std::string &s) { return "/" + s; }

PdfRect display_to_user(const Box &b0, const PdfRect &m, int rotate) {
  Box b = b0.normalized();
  int r = ((rotate % 360) + 360) % 360;
  auto map = [&](double dx, double dy, double &ux, double &uy) {
    switch (r) {
      case 90: ux = m.x1 + dy; uy = m.y1 + dx; break;
      case 180: ux = m.x2 - dx; uy = m.y1 + dy; break;
      case 270: ux = m.x2 - dy; uy = m.y2 - dx; break;
      default: ux = m.x1 + dx; uy = m.y2 - dy; break;
    }
  };
  double ax, ay, bx, by;
  map(b.x1, b.y1, ax, ay);
  map(b.x2, b.y2, bx, by);
  return PdfRect{std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
}

bool PdfObjects::load(const std::string &text, std::string &error) {
  Json doc;
  if (!Json::parse(text, doc, error)) return false;
  const Json *pages = doc.get("pages");
  const Json *qpdf = doc.get("qpdf");
  if (!pages || !pages->is(Json::Array) || !qpdf || !qpdf->is(Json::Array) || qpdf->items().size() < 2) {
    error = "not qpdf JSON v2 output";
    return false;
  }
  pages_.clear();
  for (const auto &p : pages->items())
    if (const Json *o = p.get("object")) pages_.push_back(o->str());
  if (const Json *m = qpdf->items()[0].get("maxobjectid")) max_id_ = int(m->num());
  objects_.clear();
  for (const auto &m : qpdf->items()[1].members()) {
    if (m.first == "trailer") {
      if (const Json *v = m.second.get("value")) trailer_ = *v;
      continue;
    }
    if (m.first.compare(0, 4, "obj:") != 0) continue;
    std::string ref = m.first.substr(4);
    if (const Json *v = m.second.get("value")) objects_[ref] = *v;
    else if (const Json *st = m.second.get("stream"))
      if (const Json *d = st->get("dict")) objects_[ref] = *d;
  }
  if (const Json *r = trailer_.get("/Root")) root_ = r->str();
  if (pages_.empty()) {
    error = "the document has no pages";
    return false;
  }
  return true;
}

const Json *PdfObjects::object(const std::string &ref) const {
  auto it = objects_.find(ref);
  return it == objects_.end() ? nullptr : &it->second;
}

static bool rect_of(const Json *a, PdfRect &r) {
  if (!a || !a->is(Json::Array) || a->items().size() != 4) return false;
  double v[4];
  for (int k = 0; k < 4; ++k) {
    if (!a->items()[size_t(k)].is(Json::Number)) return false;
    v[k] = a->items()[size_t(k)].num();
  }
  r = PdfRect{std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3])};
  return true;
}

PdfRect PdfObjects::media_box(int page) const {
  PdfRect media{0, 0, 612, 792}, crop;
  bool have_media = false, have_crop = false;
  const Json *o = object(page_ref(page));
  for (int depth = 0; o && depth < 32 && !(have_media && have_crop); ++depth) {
    if (!have_media) have_media = rect_of(o->get("/MediaBox"), media);
    if (!have_crop) have_crop = rect_of(o->get("/CropBox"), crop);
    const Json *parent = o->get("/Parent");
    o = parent && parent->is(Json::String) ? object(parent->str()) : nullptr;
  }
  if (!have_crop) return media;
  // The visible area is the crop box clipped to the media box.
  return PdfRect{std::max(media.x1, crop.x1), std::max(media.y1, crop.y1), std::min(media.x2, crop.x2),
                 std::min(media.y2, crop.y2)};
}

int PdfObjects::rotation(int page) const {
  const Json *o = object(page_ref(page));
  for (int depth = 0; o && depth < 32; ++depth) {
    if (const Json *r = o->get("/Rotate"))
      if (r->is(Json::Number)) return ((int(r->num()) % 360) + 360) % 360;
    const Json *parent = o->get("/Parent");
    o = parent && parent->is(Json::String) ? object(parent->str()) : nullptr;
  }
  return 0;
}

std::string PdfUpdate::add(Json value) {
  std::string ref = format("%d 0 R", next_++);
  Json w = Json::object();
  w.set("value", std::move(value));
  changed_.push_back({ref, std::move(w)});
  return ref;
}

std::string PdfUpdate::add_stream(Json dict, const std::string &data) {
  std::string ref = format("%d 0 R", next_++);
  Json st = Json::object();
  st.set("dict", std::move(dict));
  st.set("data", Json::string(base64(data)));
  Json w = Json::object();
  w.set("stream", std::move(st));
  changed_.push_back({ref, std::move(w)});
  return ref;
}

Json PdfUpdate::value(const std::string &ref) const {
  for (const auto &c : changed_)
    if (c.first == ref)
      if (const Json *v = c.second.get("value")) return *v;
  if (const Json *o = src_.object(ref)) return *o;
  return Json::object();
}

void PdfUpdate::set(const std::string &ref, Json value) {
  for (auto &c : changed_)
    if (c.first == ref) {
      c.second.set("value", std::move(value));
      return;
    }
  Json w = Json::object();
  w.set("value", std::move(value));
  changed_.push_back({ref, std::move(w)});
}

void PdfUpdate::set_trailer(Json value) {
  trailer_ = std::move(value);
  trailer_set_ = true;
}

std::string PdfUpdate::dump() const {
  Json header = Json::object();
  header.set("jsonversion", Json::number(2));
  header.set("pushedinheritedpageresources", Json::boolean(false));
  header.set("calledgetallpages", Json::boolean(false));
  header.set("maxobjectid", Json::number(src_.max_id()));
  Json objs = Json::object();
  for (const auto &c : changed_) objs.set("obj:" + c.first, c.second);
  if (trailer_set_) {
    Json w = Json::object();
    w.set("value", trailer_);
    objs.set("trailer", std::move(w));
  }
  Json root = Json::object();
  root.set("qpdf", Json::array({header, objs}));
  return root.dump();
}

static Json rect_json(const PdfRect &r) {
  return Json::array({Json::number(r.x1), Json::number(r.y1), Json::number(r.x2), Json::number(r.y2)});
}

static Json nums(std::initializer_list<double> v) {
  Json a = Json::array();
  for (double d : v) a.push(Json::number(d));
  return a;
}

void append_annot(PdfUpdate &up, const PdfObjects &src, int page, const std::string &annot_ref) {
  std::string pref = src.page_ref(page);
  Json pg = up.value(pref);
  Json *annots = pg.get("/Annots");
  if (annots && annots->is(Json::String)) {
    std::string aref = annots->str();
    Json arr = up.value(aref);
    if (!arr.is(Json::Array)) arr = Json::array();
    arr.push(Json::string(annot_ref));
    up.set(aref, std::move(arr));
    return;
  }
  if (annots && annots->is(Json::Array)) annots->push(Json::string(annot_ref));
  else pg.set("/Annots", Json::array({Json::string(annot_ref)}));
  up.set(pref, std::move(pg));
}

bool plan_link(PdfUpdate &up, const PdfObjects &src, int page, const Box &box, const std::string &uri, int dest_page) {
  if (page < 0 || page >= src.page_count()) return false;
  if (uri.empty() && (dest_page < 0 || dest_page >= src.page_count())) return false;
  PdfRect r = display_to_user(box, src.media_box(page), src.rotation(page));
  Json a = Json::object();
  a.set("/Type", Json::string("/Annot"));
  a.set("/Subtype", Json::string("/Link"));
  a.set("/Rect", rect_json(r));
  a.set("/Border", nums({0, 0, 0}));
  a.set("/F", Json::number(4));
  a.set("/P", Json::string(src.page_ref(page)));
  if (!uri.empty()) {
    Json act = Json::object();
    act.set("/S", Json::string("/URI"));
    act.set("/URI", Json::string(pdf_text(uri)));
    a.set("/A", std::move(act));
  } else {
    a.set("/Dest", Json::array({Json::string(src.page_ref(dest_page)), Json::string("/Fit")}));
  }
  append_annot(up, src, page, up.add(std::move(a)));
  return true;
}

// PDF string literal with the characters PDF needs escaped; non-ASCII is
// dropped (the appearance uses the standard Helvetica encoding).
static std::string pdf_literal(const std::string &s) {
  std::string out = "(";
  for (unsigned char c : s) {
    if (c == '(' || c == ')' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c >= 0x20 && c < 0x7F) {
      out += char(c);
    }
  }
  return out + ")";
}

static std::string fmt(double v) {
  std::string s = format("%.2f", v);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s.empty() ? "0" : s;
}

bool plan_field(PdfUpdate &up, const PdfObjects &src, int page, const Box &box, const FieldSpec &spec) {
  if (page < 0 || page >= src.page_count() || spec.name.empty() || src.root_ref().empty()) return false;
  PdfRect r = display_to_user(box, src.media_box(page), src.rotation(page));
  double w = r.x2 - r.x1, h = r.y2 - r.y1;
  if (w < 4 || h < 4) return false;
  // The form: /AcroForm in the catalog, with Helvetica as default font.
  std::string root_ref = src.root_ref();
  Json root = up.value(root_ref);
  std::string form_ref;
  Json form = Json::object();
  if (Json *af = root.get("/AcroForm")) {
    if (af->is(Json::String)) {
      form_ref = af->str();
      form = up.value(form_ref);
    } else if (af->is(Json::Object)) {
      form = *af;
    }
  }
  std::string helv;
  auto font_ref = [&]() {
    if (helv.empty()) {
      Json f = Json::object();
      f.set("/Type", Json::string("/Font"));
      f.set("/Subtype", Json::string("/Type1"));
      f.set("/BaseFont", Json::string("/Helvetica"));
      f.set("/Encoding", Json::string("/WinAnsiEncoding"));
      helv = up.add(std::move(f));
    }
    return helv;
  };
  if (!form.get("/DR")) {
    Json fonts = Json::object();
    fonts.set("/Helv", Json::string(font_ref()));
    Json dr = Json::object();
    dr.set("/Font", std::move(fonts));
    form.set("/DR", std::move(dr));
  }
  if (!form.get("/DA")) form.set("/DA", Json::string(pdf_text("/Helv 0 Tf 0 g")));
  form.set("/NeedAppearances", Json::boolean(true));

  // Appearance streams.
  auto form_xobject = [&](double bw, double bh, const std::string &ops, bool with_font) {
    Json d = Json::object();
    d.set("/Type", Json::string("/XObject"));
    d.set("/Subtype", Json::string("/Form"));
    d.set("/BBox", nums({0, 0, bw, bh}));
    if (with_font) {
      Json fonts = Json::object();
      fonts.set("/Helv", Json::string(font_ref()));
      Json res = Json::object();
      res.set("/Font", std::move(fonts));
      d.set("/Resources", std::move(res));
    }
    return up.add_stream(std::move(d), ops);
  };
  std::string border = format("0.45 0.5 0.58 RG 1 w 0.5 0.5 %s %s re S\n", fmt(w - 1).c_str(), fmt(h - 1).c_str());
  std::string white = format("1 1 1 rg 0 0 %s %s re f\n", fmt(w).c_str(), fmt(h).c_str());

  Json wd = Json::object();
  wd.set("/Type", Json::string("/Annot"));
  wd.set("/Subtype", Json::string("/Widget"));
  wd.set("/Rect", rect_json(r));
  wd.set("/F", Json::number(4));
  wd.set("/P", Json::string(src.page_ref(page)));
  wd.set("/T", Json::string(pdf_text(spec.name)));
  Json mk = Json::object();
  mk.set("/BC", nums({0.45, 0.5, 0.58}));
  mk.set("/BG", nums({1, 1, 1}));
  Json ap = Json::object();
  switch (spec.kind) {
    case FieldKind::Text:
      wd.set("/FT", Json::string("/Tx"));
      wd.set("/V", Json::string(pdf_text("")));
      wd.set("/DA", Json::string(pdf_text("/Helv 0 Tf 0 g")));
      if (spec.multiline) wd.set("/Ff", Json::number(4096));
      ap.set("/N", Json::string(form_xobject(w, h, white + border, false)));
      break;
    case FieldKind::Check:
    case FieldKind::Radio: {
      bool radio = spec.kind == FieldKind::Radio;
      std::string on = radio ? "Choice" : "Yes";
      wd.set("/FT", Json::string("/Btn"));
      if (radio) wd.set("/Ff", Json::number(49152));
      wd.set("/V", Json::string("/Off"));
      wd.set("/AS", Json::string("/Off"));
      double s = std::min(w, h);
      std::string frame, mark;
      if (radio) {
        // A circle from four Bézier arcs.
        double cx = w / 2, cy = h / 2, rr = s / 2 - 1, k = 0.5523 * rr;
        auto circle = [&](double rad, double kk) {
          return format("%s %s m %s %s %s %s %s %s c %s %s %s %s %s %s c %s %s %s %s %s %s c %s %s %s %s %s %s c ",
                        fmt(cx + rad).c_str(), fmt(cy).c_str(), fmt(cx + rad).c_str(), fmt(cy + kk).c_str(),
                        fmt(cx + kk).c_str(), fmt(cy + rad).c_str(), fmt(cx).c_str(), fmt(cy + rad).c_str(),
                        fmt(cx - kk).c_str(), fmt(cy + rad).c_str(), fmt(cx - rad).c_str(), fmt(cy + kk).c_str(),
                        fmt(cx - rad).c_str(), fmt(cy).c_str(), fmt(cx - rad).c_str(), fmt(cy - kk).c_str(),
                        fmt(cx - kk).c_str(), fmt(cy - rad).c_str(), fmt(cx).c_str(), fmt(cy - rad).c_str(),
                        fmt(cx + kk).c_str(), fmt(cy - rad).c_str(), fmt(cx + rad).c_str(), fmt(cy - kk).c_str(),
                        fmt(cx + rad).c_str(), fmt(cy).c_str());
        };
        frame = "1 1 1 rg 0.45 0.5 0.58 RG 1 w " + circle(rr, k) + "b\n";
        double r2 = rr * 0.5;
        mark = "0 0 0 rg " + circle(r2, 0.5523 * r2) + "f\n";
      } else {
        frame = white + border;
        mark = format("0 0 0 RG %s w %s %s m %s %s l %s %s l S\n", fmt(std::max(1.0, s * 0.12)).c_str(),
                      fmt(w * 0.22).c_str(), fmt(h * 0.52).c_str(), fmt(w * 0.42).c_str(), fmt(h * 0.28).c_str(),
                      fmt(w * 0.8).c_str(), fmt(h * 0.76).c_str());
      }
      Json n = Json::object();
      n.set("/" + on, Json::string(form_xobject(w, h, frame + mark, false)));
      n.set("/Off", Json::string(form_xobject(w, h, frame, false)));
      ap.set("/N", std::move(n));
      break;
    }
    case FieldKind::Combo:
    case FieldKind::List: {
      wd.set("/FT", Json::string("/Ch"));
      if (spec.kind == FieldKind::Combo) wd.set("/Ff", Json::number(131072));
      Json opt = Json::array();
      for (const auto &o : spec.options) opt.push(Json::string(pdf_text(o)));
      wd.set("/Opt", std::move(opt));
      wd.set("/V", Json::string(pdf_text(spec.options.empty() ? "" : spec.options.front())));
      wd.set("/DA", Json::string(pdf_text("/Helv 0 Tf 0 g")));
      std::string text;
      if (!spec.options.empty()) {
        double fs = std::min(12.0, h * 0.6);
        text = format("BT /Helv %s Tf 0 g 3 %s Td %s Tj ET\n", fmt(fs).c_str(), fmt(h - fs - 2).c_str(),
                      pdf_literal(spec.options.front()).c_str());
      }
      ap.set("/N", Json::string(form_xobject(w, h, white + border + text, true)));
      break;
    }
    case FieldKind::Button: {
      wd.set("/FT", Json::string("/Btn"));
      wd.set("/Ff", Json::number(65536));
      std::string label = spec.label.empty() ? spec.name : spec.label;
      mk.set("/CA", Json::string(pdf_text(label)));
      mk.set("/BG", nums({0.88, 0.91, 0.95}));
      double fs = std::min(12.0, h * 0.55);
      double tw = fs * 0.5 * double(label.size());
      std::string ops = format("0.88 0.91 0.95 rg 0 0 %s %s re f\n", fmt(w).c_str(), fmt(h).c_str()) + border +
                        format("BT /Helv %s Tf 0 g %s %s Td %s Tj ET\n", fmt(fs).c_str(),
                               fmt(std::max(2.0, (w - tw) / 2)).c_str(), fmt((h - fs) / 2 + fs * 0.2).c_str(),
                               pdf_literal(label).c_str());
      ap.set("/N", Json::string(form_xobject(w, h, ops, true)));
      break;
    }
    case FieldKind::Signature:
      wd.set("/FT", Json::string("/Sig"));
      ap.set("/N", Json::string(form_xobject(
                       w, h, "0.96 0.97 1 rg 0 0 " + fmt(w) + " " + fmt(h) + " re f\n" + border, false)));
      break;
  }
  wd.set("/MK", std::move(mk));
  wd.set("/AP", std::move(ap));
  std::string wref = up.add(std::move(wd));
  // Register the field with the form.
  Json *fields = form.get("/Fields");
  if (fields && fields->is(Json::String)) {
    std::string fref = fields->str();
    Json arr = up.value(fref);
    if (!arr.is(Json::Array)) arr = Json::array();
    arr.push(Json::string(wref));
    up.set(fref, std::move(arr));
  } else if (fields && fields->is(Json::Array)) {
    fields->push(Json::string(wref));
  } else {
    form.set("/Fields", Json::array({Json::string(wref)}));
  }
  if (!form_ref.empty()) {
    up.set(form_ref, std::move(form));
  } else {
    root.set("/AcroForm", std::move(form));
    up.set(root_ref, std::move(root));
  }
  append_annot(up, src, page, wref);
  return true;
}

bool plan_crop(PdfUpdate &up, const PdfObjects &src, const std::vector<int> &pages, const Box &box) {
  bool any = false;
  for (int p : pages) {
    if (p < 0 || p >= src.page_count()) continue;
    PdfRect m = src.media_box(p);
    PdfRect r = display_to_user(box, m, src.rotation(p));
    r.x1 = std::max(r.x1, m.x1);
    r.y1 = std::max(r.y1, m.y1);
    r.x2 = std::min(r.x2, m.x2);
    r.y2 = std::min(r.y2, m.y2);
    if (r.x2 - r.x1 < 8 || r.y2 - r.y1 < 8) continue;
    Json pg = up.value(src.page_ref(p));
    pg.set("/CropBox", rect_json(r));
    up.set(src.page_ref(p), std::move(pg));
    any = true;
  }
  return any;
}

bool plan_strip_metadata(PdfUpdate &up, const PdfObjects &src) {
  bool changed = false;
  Json t = src.trailer();
  if (const Json *info = t.get("/Info")) {
    // Empty the information dictionary (qpdf keeps the trailer key).
    if (info->is(Json::String)) up.set(info->str(), Json::object());
    t.erase("/Info");
    up.set_trailer(std::move(t));
    changed = true;
  }
  if (!src.root_ref().empty()) {
    Json root = up.value(src.root_ref());
    if (root.erase("/Metadata")) {
      up.set(src.root_ref(), std::move(root));
      changed = true;
    }
  }
  return changed;
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
