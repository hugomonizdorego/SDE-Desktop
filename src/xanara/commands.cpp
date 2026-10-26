// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The commands of Xanara's modes. Annotations are made by poppler in
// memory; everything that changes the PDF itself (text and images, pages,
// links, form fields, crop boxes, stamps, encryption, redaction) is done
// by Xanara's own engine (pdf.h, edit.h) on a working copy, so nothing
// touches the original until the user saves. Each such change is one
// step of undo: the working copy before it is kept until the tab closes.
// No other program is run.
#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <thread>

#include "edit.h"
#include "i18n.h"
#include "icons.h"
#include "layout.h"
#include "ocr.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

namespace {

std::string stem(const std::string &path) {
  std::string base = display_name(path);
  if (ends_with(lower(base), ".pdf")) base = base.substr(0, base.size() - 4);
  return base;
}

std::string dir_of(const std::string &path) {
  size_t s = path.rfind('/');
  return s == std::string::npos ? home_dir() : path.substr(0, s);
}

// A save dialog for a new PDF (or another |ext|) next to |near|.
std::string ask_save(const char *title, const std::string &near, const std::string &name, const char *filter,
                     const std::string &ext) {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::NEW_FOLDER);
  chooser.title(title);
  chooser.filter(filter);
  std::string dir = near.empty() ? user_dir("DOCUMENTS") : dir_of(near);
  if (!dir_exists(dir)) dir = home_dir();
  chooser.directory(dir.c_str());
  chooser.preset_file(name.c_str());
  if (chooser.show() != 0 || !chooser.filename()) return "";
  std::string out = chooser.filename();
  if (!ends_with(lower(out), "." + ext)) out += "." + ext;
  return out;
}

std::vector<std::string> ask_open_many(const char *title, const char *filter, const std::string &dir) {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
  chooser.title(title);
  chooser.filter(filter);
  chooser.directory((dir.empty() || !dir_exists(dir) ? home_dir() : dir).c_str());
  std::vector<std::string> out;
  if (chooser.show() != 0) return out;
  for (int k = 0; k < chooser.count(); ++k) out.push_back(path_from_argument(chooser.filename(k)));
  return out;
}

std::string today_text() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

long file_size(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? long(st.st_size) : 0;
}

std::string size_text(long b) {
  return b > (1 << 20) ? format("%.1f MB", b / 1048576.0) : format("%.0f KB", b / 1024.0);
}

void report(const std::string &what, const std::string &error) {
  fl_alert("%s", format(_("%s did not work:\n%s"), what.c_str(), error.c_str()).c_str());
}

}  // namespace

// ------------------------------------------------------------- plumbing

std::string XanaraWindow::current_source() {
  DocTab *t = tab();
  if (!t) return "";
  if (!t->doc->modified()) return t->doc->path();
  std::string error, p = temp_path("edit");
  if (!t->doc->save_to(p, error)) {
    fl_alert("%s", error.c_str());
    return "";
  }
  return p;
}

bool XanaraWindow::working_change(const std::string &out, const std::string &message, const std::string &before,
                                  const std::vector<int> &changed_pages) {
  DocTab *t = tab();
  if (!t) return false;
  int cur = t->view->current_page();
  std::string pw = t->doc->password();
  std::string doc_path = t->doc->path();
  bool doc_working = t->working_copy;
  std::string from = before.empty() ? doc_path : before;
  // Annotations made since the last copy were saved into |from|: they
  // become one step of undo of their own.
  std::vector<int> annot_pages = t->view->annotation_step_pages();
  if (!replace_document(*t, out, pw, true, cur, true, changed_pages)) return false;
  if (from != doc_path) t->view->push_snapshot(doc_path, doc_working, from, annot_pages);
  // One step of undo: back to the file the change started from.
  t->view->push_snapshot(from, from != t->target, out, changed_pages);
  drop_temps(t->view);
  update_chrome();
  if (!message.empty()) toast(message);
  return true;
}

bool XanaraWindow::commit_page_edit(DocView *v, int page,
                                    const std::function<bool(PdfFile &, PageLayout &, std::string &)> &op,
                                    const PageLayout *expected, const std::string &message) {
  DocTab *t = tab();
  if (!t || t->view != v) return false;
  commit_field();
  std::string src = current_source();
  if (src.empty()) return false;
  std::string error;
  std::unique_ptr<PdfFile> pdf = PdfFile::open(src, t->doc->password(), error);
  std::unique_ptr<PageLayout> layout = pdf ? PageLayout::read(*pdf, page) : nullptr;
  if (!layout) {
    report(_("Edit"), error.empty() ? std::string(_("the page could not be read")) : error);
    return false;
  }
  if (expected && !same_content(*layout, *expected)) {
    report(_("Edit"), _("the page is not the one that was edited"));
    return false;
  }
  if (!op(*pdf, *layout, error)) {
    report(_("Edit"), error.empty() ? std::string(_("the change could not be made")) : error);
    return false;
  }
  std::string out = temp_path("edit");
  bool ok = pdf->write(out, PdfFile::Write::Auto, error);
  layout.reset();
  pdf.reset();
  if (!ok) {
    report(_("Edit"), error);
    return false;
  }
  return working_change(out, message, src, {page});
}

bool XanaraWindow::engine_change(const std::function<bool(PdfFile &, std::string &)> &op, const std::string &what,
                                 const std::string &message, const std::vector<int> &changed_pages) {
  DocTab *t = tab();
  if (!t) return false;
  commit_field();
  std::string src = current_source();
  if (src.empty()) return false;
  std::string error;
  std::unique_ptr<PdfFile> pdf = PdfFile::open(src, t->doc->password(), error);
  if (!pdf) {
    report(what, error);
    return false;
  }
  if (!op(*pdf, error)) {
    if (error.empty()) toast(_("Nothing to change"));
    else report(what, error);
    return false;
  }
  std::string out = temp_path("change");
  if (!pdf->write(out, PdfFile::Write::Auto, error)) {
    report(what, error);
    return false;
  }
  return working_change(out, message, src, changed_pages);
}

void XanaraWindow::restore_snapshot(const std::string &path, bool working, const std::vector<int> &changed_pages) {
  DocTab *t = tab();
  if (!t) return;
  if (path.empty() || !file_exists(path)) {
    toast(_("This step cannot be undone: its copy is gone"));
    return;
  }
  int cur = t->view->current_page();
  if (!replace_document(*t, path, t->doc->password(), working, cur, true, changed_pages)) return;
  update_chrome();
}

bool XanaraWindow::choose_image() {
  DocView *v = view();
  if (!v) return false;
  std::string file = choose_image_file();
  if (file.empty()) return false;
  cairo_surface_t *img = load_image(file, 0, 0);
  if (!img) {
    toast(_("This image could not be read"));
    return false;
  }
  v->set_image(img, file);
  return true;
}

std::string XanaraWindow::choose_image_file() {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.title(_("Choose an Image"));
  chooser.filter(_("Images\t*.{png,PNG,jpg,jpeg,JPG,JPEG,svg,gif,bmp}"));
  chooser.directory(user_dir("PICTURES").c_str());
  if (chooser.show() != 0 || !chooser.filename()) return "";
  return chooser.filename();
}

// ------------------------------------------------------------- commands

void XanaraWindow::run(Act a) {
  DocView *v = view();
  const ToolSpec *spec = tool_spec(a);
  if (!v || !spec) return;
  std::string why;
  if (!tool_enabled(*spec, &why)) {
    if (!why.empty()) {
      fl_message_title(_(spec->label));
      fl_alert("%s", why.c_str());
    }
    return;
  }
  v->finish_text_edit();
  if (spec->canvas) {
    FieldKind kind = FieldKind::Text;
    Tool t = tool_for(a, &kind);
    if (tool_toggled(*spec)) {
      if (t != Tool::Edit) v->set_tool(mode_ == Mode::Edit ? Tool::Edit : Tool::Select);
      return;
    }
    if (t == Tool::Image && !choose_image()) return;
    if (t == Tool::Field) v->set_field_kind(kind);
    if (t == Tool::Signature && !v->signature()) {
      // Use the newest saved signature, or make one.
      auto files = list_dir(signatures_dir());
      std::sort(files.begin(), files.end());
      for (auto it = files.rbegin(); it != files.rend(); ++it)
        if (ends_with(lower(*it), ".png"))
          if (cairo_surface_t *img = load_image(signatures_dir() + "/" + *it, 0, 0)) {
            v->set_signature(img);
            break;
          }
      if (!v->signature()) {
        int c = 0;
        if (!ask_choice(_("Signature"), _("You have no signature yet"),
                        {_("Draw a Signature…"), _("Use an Image…")}, c))
          return;
        if (c == 0) draw_signature();
        else choose_signature();
        return;
      }
    }
    v->set_tool(t);
    if (t == Tool::Redact) toast(_("Drag over what must disappear; Apply Redactions makes it final"));
    return;
  }
  switch (a) {
    case Act::ToggleAnnots:
      v->set_show_annots(!v->show_annots());
      toast(v->show_annots() ? _("Annotations shown") : _("Annotations hidden"));
      update_chrome();
      break;
    case Act::ShowFields:
      v->set_highlight_fields(!v->highlight_fields());
      update_chrome();
      break;
    case Act::Watermark: stamp_pages(StampSpec::Watermark); break;
    case Act::Background: stamp_pages(StampSpec::Background); break;
    case Act::HeaderFooter: stamp_pages(StampSpec::HeaderFooter); break;
    case Act::Bates: stamp_pages(StampSpec::Bates); break;
    case Act::PageNumbers: stamp_pages(StampSpec::PageNumbers); break;
    case Act::PageInsert: page_operation("insert"); break;
    case Act::PageExtract: page_operation("extract"); break;
    case Act::PageSplit: page_operation("split"); break;
    case Act::PageMerge: page_operation("append"); break;
    case Act::PageRotateLeft: page_operation("rotate-left"); break;
    case Act::PageRotateRight: page_operation("rotate-right"); break;
    case Act::PageDelete: page_operation("delete"); break;
    case Act::PageMoveUp: page_operation("move-up"); break;
    case Act::PageMoveDown: page_operation("move-down"); break;
    case Act::ToPng: export_images(false); break;
    case Act::ToJpeg: export_images(true); break;
    case Act::ToText: export_text(0); break;
    case Act::ToHtml: export_text(1); break;
    case Act::ToMarkdown: export_text(2); break;
    case Act::ImagesToPdf: images_to_pdf_dialog(); break;
    case Act::MergeFiles: merge_files(); break;
    case Act::Encrypt: encrypt(); break;
    case Act::Decrypt: decrypt(); break;
    case Act::ApplyRedactions: apply_redactions(); break;
    case Act::RemoveMetadata:
      if (engine_change([](PdfFile &pdf, std::string &) { return strip_metadata(pdf); }, _("Remove Metadata"),
                        _("Metadata removed — save to keep the result"))) {
        // poppler's own copy of the information dictionary goes too.
        if (Document *d = document()) {
          std::lock_guard<std::mutex> lock(d->mutex());
          for (auto fn : {poppler_document_set_title, poppler_document_set_author, poppler_document_set_subject,
                          poppler_document_set_keywords, poppler_document_set_creator, poppler_document_set_producer})
            fn(d->raw(), "");
        }
      }
      break;
    case Act::Compress: compress(); break;
    case Act::Ocr: ocr(); break;
    case Act::Compare: compare(); break;
    case Act::Print: print(); break;
    default: break;
  }
}

void XanaraWindow::canvas_box(Tool t, int page, const Box &box) {
  DocView *v = view();
  if (!v) return;
  if (t == Tool::Link) {
    std::string target;
    if (!ask_text(_("Link to a web address, or to a page number"), target, false) || trim(target).empty()) return;
    target = trim(target);
    bool number = std::all_of(target.begin(), target.end(), [](char c) { return c >= '0' && c <= '9'; });
    int dest = number ? atoi(target.c_str()) - 1 : -1;
    if (!number && target.find("://") == std::string::npos && target.rfind("mailto:", 0) != 0) target = "https://" + target;
    std::string uri = number ? "" : target;
    engine_change([&](PdfFile &pdf, std::string &) { return add_link(pdf, page, box, uri, dest); }, _("Link"),
                  _("Link added — save to keep it"), {page});
    return;
  }
  if (t == Tool::Crop) {
    int c = 0;
    if (!ask_choice(_("Crop"), _("Crop which pages?"), {_("This page"), _("All pages")}, c)) return;
    std::vector<int> pages;
    if (c == 0) pages.push_back(page);
    else
      for (int p = 0; p < document()->count(); ++p) pages.push_back(p);
    v->set_tool(mode_ == Mode::Edit ? Tool::Edit : Tool::Select);
    engine_change([&](PdfFile &pdf, std::string &) { return crop_pages(pdf, pages, box); }, _("Crop"),
                  _("Cropped — save to keep the result"));
    return;
  }
  if (t == Tool::Field) {
    FieldSpec f;
    f.kind = v->field_kind();
    static const char *base[] = {"Text", "Check", "Radio", "Choice", "List", "Button", "Signature"};
    int n = 1;
    for (int p = 0; p < document()->count(); ++p) n += int(document()->fields(p).size());
    f.name = format("%s%d", base[int(f.kind)], n);
    if (!ask_text(_("Field Name"), f.name, false) || trim(f.name).empty()) return;
    f.name = trim(f.name);
    if (f.kind == FieldKind::Combo || f.kind == FieldKind::List) {
      std::string opts = "Option 1, Option 2, Option 3";
      if (!ask_text(_("Choices, separated by commas"), opts, false)) return;
      for (auto &o : split(opts, ',')) {
        std::string s = trim(o);
        if (!s.empty()) f.options.push_back(s);
      }
    }
    if (f.kind == FieldKind::Button) {
      f.label = _("Submit");
      if (!ask_text(_("Button Caption"), f.label, false)) return;
    }
    if (f.kind == FieldKind::Text) f.multiline = box.h() > 40;
    engine_change([&](PdfFile &pdf, std::string &) { return add_field(pdf, page, box, f); }, _("Form Field"),
                  format(_("Field “%s” added — save to keep it"), f.name.c_str()), {page});
  }
}

// ------------------------------------------------------------------ pages

void XanaraWindow::page_operation(const std::string &op, int page, int to) {
  DocTab *t = tab();
  if (!t) return;
  commit_field();
  t->view->finish_text_edit();
  Document *doc = t->doc.get();
  int n = doc->count(), cur = page >= 0 ? page : t->view->current_page();
  std::vector<int> order;
  for (int i = 0; i < n; ++i) order.push_back(i);
  std::vector<int> pages;
  if (op == "delete" || op == "extract") {
    std::string ranges = std::to_string(cur + 1);
    if (!ask_text(op == "delete" ? _("Delete Pages (for example 3 or 2-5,9)") : _("Extract Pages (for example 1-3,7)"),
                  ranges, false))
      return;
    if (!parse_page_ranges(ranges, n, pages)) {
      fl_alert("%s", format(_("“%s” is not a valid page selection for pages 1 to %d."), ranges.c_str(), n).c_str());
      return;
    }
    if (op == "delete") {
      std::vector<int> keep;
      for (int i = 0; i < n; ++i)
        if (std::find(pages.begin(), pages.end(), i) == pages.end()) keep.push_back(i);
      if (keep.empty()) {
        fl_alert("%s", _("A document needs at least one page."));
        return;
      }
      order = keep;
    }
  } else if (op == "move-up" || op == "move-down" || op == "move") {
    int dest = op == "move-up" ? cur - 1 : op == "move-down" ? cur + 1 : to;
    if (dest < 0 || dest >= n || dest == cur) return;
    order.erase(order.begin() + cur);
    order.insert(order.begin() + dest, cur);
    cur = dest;
  }
  std::string error;
  if (op == "extract") {
    std::string out = ask_save(_("Save Extracted Pages As"), t->target,
                               stem(t->target) + " (" + format_page_ranges(pages) + ").pdf", _("PDF Documents\t*.pdf"), "pdf");
    if (out.empty()) return;
    std::string src = current_source();
    std::unique_ptr<PdfFile> pdf = src.empty() ? nullptr : PdfFile::open(src, doc->password(), error);
    if (!pdf || !extract_pages(*pdf, pages, out, error)) {
      report(_("Extract"), error);
      return;
    }
    toast(format(_("Saved %zu page(s) to “%s”"), pages.size(), display_name(out).c_str()));
    return;
  }
  if (op == "split") {
    std::string every = "1";
    if (!ask_text(_("Split into files of how many pages each?"), every, false)) return;
    int k = std::max(1, atoi(every.c_str()));
    Fl_Native_File_Chooser chooser;
    chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    chooser.title(_("Folder for the Split Files"));
    chooser.directory(dir_of(t->target).c_str());
    if (chooser.show() != 0 || !chooser.filename()) return;
    std::string pattern = std::string(chooser.filename()) + "/" + stem(t->target) + "-%d.pdf";
    std::string src = current_source();
    std::unique_ptr<PdfFile> pdf = src.empty() ? nullptr : PdfFile::open(src, doc->password(), error);
    std::vector<std::string> written;
    // A % in the file name would be read as a format.
    for (size_t p = pattern.find('%'); p != std::string::npos && p != pattern.size() - 6; p = pattern.find('%', p + 2))
      pattern.insert(p, "%");
    if (!pdf || !split_pages(*pdf, k, pattern, written, error)) {
      report(_("Split"), error);
      return;
    }
    toast(format(_("Split into %zu file(s) in %s"), written.size(), chooser.filename()));
    return;
  }
  if (op == "rotate-left" || op == "rotate-right") {
    int deg = op == "rotate-left" ? -90 : 90;
    engine_change([&](PdfFile &pdf, std::string &) { return rotate_pages(pdf, {cur}, deg); }, _("Rotate"),
                  _("Pages changed — save to keep the result"), {cur});
  } else if (op == "insert" || op == "append") {
    std::vector<std::string> files =
        ask_open_many(op == "insert" ? _("Insert Pages from PDFs") : _("Append PDFs"), _("PDF Documents\t*.{pdf,PDF}"),
                      dir_of(t->target));
    if (files.empty()) return;
    int after = op == "append" ? n - 1 : cur;
    if (!engine_change([&](PdfFile &pdf, std::string &e) { return insert_pages(pdf, after, files, e); }, _("Insert"),
                       _("Pages changed — save to keep the result"), {kPagesMoved}))
      return;
    cur = after + 1;
  } else {
    if (!engine_change([&](PdfFile &pdf, std::string &e) { return reorder_pages(pdf, order, e); }, _("Pages"),
                       _("Pages changed — save to keep the result"), {kPagesMoved}))
      return;
    if (op == "delete") cur = std::min(cur, int(order.size()) - 1);
  }
  if (tab()) tab()->view->go_to(cur);
}

void XanaraWindow::merge_files() {
  auto files = ask_open_many(_("Choose the PDFs to Merge (in order)"), _("PDF Documents\t*.{pdf,PDF}"),
                             user_dir("DOCUMENTS"));
  if (files.size() < 2) {
    if (files.size() == 1) toast(_("Choose two or more files to merge"));
    return;
  }
  std::string out = ask_save(_("Save the Merged PDF As"), files[0], _("Merged.pdf"), _("PDF Documents\t*.pdf"), "pdf");
  if (out.empty()) return;
  toast(_("Merging…"));
  Fl::flush();
  std::string error;
  if (!xanara::merge_files(files, out, error)) {
    report(_("Merge"), error);
    return;
  }
  open_path(out);
  toast(format(_("Merged %zu files"), files.size()));
}

void XanaraWindow::images_to_pdf_dialog() {
  auto files = ask_open_many(_("Choose Images"), _("Images\t*.{png,PNG,jpg,jpeg,JPG,JPEG,gif,bmp,svg}"),
                             user_dir("PICTURES"));
  if (files.empty()) return;
  int fit = 0;
  if (!ask_choice(_("Images to PDF"), _("Page size"), {_("A4, image fitted"), _("Same as each image")}, fit)) return;
  std::vector<cairo_surface_t *> images;
  for (const auto &f : files)
    if (cairo_surface_t *img = load_image(f, 0, 0)) {
      // A JPEG goes into the PDF as it is, without a second compression.
      std::string data;
      if ((ends_with(lower(f), ".jpg") || ends_with(lower(f), ".jpeg")) && read_file(f, data)) {
        auto *copy = new std::string(std::move(data));
        cairo_surface_set_mime_data(img, CAIRO_MIME_TYPE_JPEG, reinterpret_cast<const unsigned char *>(copy->data()),
                                    copy->size(), [](void *p) { delete static_cast<std::string *>(p); }, copy);
      }
      images.push_back(img);
    }
  if (images.empty()) {
    toast(_("These images could not be read"));
    return;
  }
  std::string out = ask_save(_("Save the PDF As"), files[0], stem(files[0]) + ".pdf", _("PDF Documents\t*.pdf"), "pdf");
  std::string error;
  bool ok = !out.empty() && xanara::images_to_pdf(images, out, fit == 0, error);
  for (auto *s : images) cairo_surface_destroy(s);
  if (out.empty()) return;
  if (!ok) {
    fl_alert("%s", error.c_str());
    return;
  }
  open_path(out);
}

// ------------------------------------------------------------------ edit

void XanaraWindow::stamp_pages(StampSpec::Kind kind) {
  DocTab *t = tab();
  if (!t) return;
  StampSpec spec;
  spec.kind = kind;
  spec.file_name = display_name(t->target);
  spec.date = today_text();
  std::string message;
  switch (kind) {
    case StampSpec::Watermark: {
      spec.text = _("CONFIDENTIAL");
      if (!ask_text(_("Watermark Text"), spec.text, false) || trim(spec.text).empty()) return;
      int c = 0;
      if (!ask_choice(_("Watermark"), _("Color"), {_("Red"), _("Gray"), _("Blue")}, c)) return;
      spec.color = c == 0 ? Rgb{0.85, 0.1, 0.1} : c == 1 ? Rgb{0.4, 0.4, 0.4} : Rgb{0.11, 0.35, 0.75};
      spec.opacity = 0.22;
      spec.font_size = std::clamp(560.0 / std::max<size_t>(4, spec.text.size()), 28.0, 96.0);
      message = _("Watermark added — save to keep it");
      break;
    }
    case StampSpec::Background: {
      int c = 0;
      if (!ask_choice(_("Background"), _("Background color"), {_("Cream"), _("Light blue"), _("Light gray"), _("Mint")}, c))
        return;
      static const Rgb colors[] = {{1.0, 0.97, 0.88}, {0.9, 0.95, 1.0}, {0.93, 0.93, 0.93}, {0.9, 0.98, 0.93}};
      spec.color = colors[c];
      spec.opacity = 1.0;
      message = _("Background added — save to keep it");
      break;
    }
    case StampSpec::HeaderFooter: {
      std::string head = "{file}", foot = _("Page {page} of {pages}");
      if (!ask_text(_("Header (left) — {file} {date} {page} {pages}"), head, false)) return;
      if (!ask_text(_("Footer (center) — {file} {date} {page} {pages}"), foot, false)) return;
      spec.header[0] = head;
      spec.header[2] = "{date}";
      spec.footer[1] = foot;
      spec.color = Rgb{0.25, 0.25, 0.25};
      spec.opacity = 1;
      spec.font_size = 9;
      message = _("Header and footer added — save to keep them");
      break;
    }
    case StampSpec::Bates: {
      std::string prefix = stem(t->target).substr(0, 6) + "-", start = "1";
      for (auto &ch : prefix) ch = char(toupper((unsigned char)ch));
      if (!ask_text(_("Bates Prefix"), prefix, false)) return;
      if (!ask_text(_("First Number"), start, false)) return;
      spec.prefix = prefix;
      spec.start = std::max(0, atoi(start.c_str()));
      spec.color = Rgb{0.1, 0.1, 0.1};
      spec.opacity = 1;
      spec.font_size = 9;
      message = _("Bates numbers added — save to keep them");
      break;
    }
    case StampSpec::PageNumbers: {
      int c = 0;
      if (!ask_choice(_("Page Numbers"), _("Format"), {"1", _("Page 1"), _("Page 1 of N"), "1 / N"}, c)) return;
      static const char *fmts[] = {"{page}", N_("Page {page}"), N_("Page {page} of {pages}"), "{page} / {pages}"};
      spec.footer[1] = _(fmts[c]);
      spec.color = Rgb{0.2, 0.2, 0.2};
      spec.opacity = 1;
      spec.font_size = 10;
      message = _("Page numbers added — save to keep them");
      break;
    }
  }
  std::vector<Point> sizes;
  for (int p = 0; p < t->doc->count(); ++p) sizes.push_back(Point{t->doc->width(p), t->doc->height(p)});
  std::string stamp = temp_path("stamp"), error;
  if (!write_stamp_pdf(stamp, sizes, spec, error)) {
    fl_alert("%s", error.c_str());
    return;
  }
  bool under = kind == StampSpec::Background;
  engine_change([&](PdfFile &pdf, std::string &e) { return overlay_pages(pdf, stamp, under, e); }, _("Stamp"), message);
}

// --------------------------------------------------------------- convert

void XanaraWindow::export_images(bool jpeg) {
  DocTab *t = tab();
  if (!t) return;
  int n = t->doc->count();
  std::string ranges = n == 1 ? "1" : format("1-%d", n);
  if (!ask_text(jpeg ? _("Export Pages as JPEG (for example 1-3,7)") : _("Export Pages as PNG (for example 1-3,7)"),
                ranges, false))
    return;
  std::vector<int> pages;
  if (!parse_page_ranges(ranges, n, pages)) {
    fl_alert("%s", format(_("“%s” is not a valid page selection for pages 1 to %d."), ranges.c_str(), n).c_str());
    return;
  }
  int q = 1;
  if (!ask_choice(_("Resolution"), _("Resolution"), {_("72 dpi (screen)"), _("150 dpi"), _("300 dpi (print)")}, q)) return;
  int dpi = q == 0 ? 72 : q == 1 ? 150 : 300;
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
  chooser.title(_("Folder for the Images"));
  chooser.directory(dir_of(t->target).c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  std::string base = std::string(chooser.filename()) + "/" + display_name(t->target);
  const char *ext = jpeg ? "jpg" : "png";
  int done = 0;
  toast(_("Exporting…"));
  Fl::flush();
  for (int p : pages) {
    std::string out = numbered_name(base, ext, p, n), error;
    if (!export_page_image(*t->doc, p, dpi, out, jpeg, error)) {
      fl_alert("%s", format(_("Page %d could not be exported: %s"), p + 1, error.c_str()).c_str());
      break;
    }
    ++done;
  }
  toast(format(_("Exported %d image(s) to %s"), done, chooser.filename()));
}

void XanaraWindow::export_text(int kind) {
  DocTab *t = tab();
  if (!t) return;
  static const char *exts[] = {"txt", "html", "md"};
  static const char *filters[] = {N_("Text\t*.txt"), N_("HTML\t*.html"), N_("Markdown\t*.md")};
  std::string out = ask_save(_("Export As"), t->target, stem(t->target) + "." + exts[kind], _(filters[kind]), exts[kind]);
  if (out.empty()) return;
  std::vector<std::string> pages;
  for (int p = 0; p < t->doc->count(); ++p) pages.push_back(t->doc->text(p));
  std::string title = t->doc->title();
  if (title.empty()) title = stem(t->target);
  std::string text = kind == 0 ? export_plain(pages) : kind == 1 ? export_html(title, pages) : export_markdown(title, pages);
  bool empty = true;
  for (auto &p : pages) empty = empty && trim(p).empty();
  if (!write_file_atomic(out, text)) {
    fl_alert("%s", format(_("“%s” could not be written."), out.c_str()).c_str());
    return;
  }
  toast(empty ? std::string(_("Exported — but the pages have no text; try Tools › OCR"))
              : format(_("Exported “%s”"), display_name(out).c_str()));
}

// ---------------------------------------------------------------- protect

void XanaraWindow::encrypt() {
  DocTab *t = tab();
  if (!t) return;
  std::string user, owner;
  if (!ask_text(_("Password to Open the Document"), user, false) || user.empty()) return;
  if (!ask_text(_("Owner Password for Changing Permissions (optional)"), owner, false)) return;
  int c = 0;
  if (!ask_choice(_("Permissions"), _("What may readers do?"),
                  {_("Everything"), _("Everything but printing"), _("Everything but copying text"),
                   _("Only read (no printing, copying or changes)")},
                  c))
    return;
  Permissions perms;
  perms.print = c != 1 && c != 3;
  perms.copy = c != 2 && c != 3;
  if (c == 3) perms.modify = perms.annotate = perms.forms = perms.assemble = false;
  t->view->finish_text_edit();
  std::string src = current_source();
  if (src.empty()) return;
  std::string out = ask_save(_("Save the Protected Copy As"), t->target, stem(t->target) + " (protected).pdf",
                             _("PDF Documents\t*.pdf"), "pdf");
  if (out.empty()) return;
  std::string error;
  std::unique_ptr<PdfFile> pdf = PdfFile::open(src, t->doc->password(), error);
  if (!pdf || !write_protected(*pdf, out, user, owner, perms, error)) {
    report(_("Password"), error);
    return;
  }
  toast(format(_("Saved the protected copy “%s” (AES-256)"), display_name(out).c_str()));
}

void XanaraWindow::decrypt() {
  DocTab *t = tab();
  if (!t) return;
  t->view->finish_text_edit();
  std::string pw = t->doc->password();
  std::string src = current_source();
  if (src.empty()) return;
  std::string out = temp_path("decrypted"), error;
  std::unique_ptr<PdfFile> pdf = PdfFile::open(src, pw, error);
  if (!pdf || !write_unprotected(*pdf, out, error)) {
    report(_("Remove Password"), error);
    return;
  }
  pdf.reset();
  working_change(out, _("Password and restrictions removed — save to keep the result"), src, {kSameContent});
}

void XanaraWindow::apply_redactions() {
  DocTab *t = tab();
  if (!t || t->view->redactions().empty()) return;
  std::map<int, std::vector<Box>> by_page;
  for (const auto &r : t->view->redactions()) by_page[r.first].push_back(r.second);
  fl_message_title(_("Apply Redactions"));
  if (fl_choice("%s", _("Cancel"), _("Black Out"), nullptr,
                format(_("Black out %zu marked area(s) on %zu page(s)?\n\n"
                         "The text, pictures and drawings under the boxes are removed from the document for "
                         "good, not only covered; the rest of the pages stays as it is."),
                       t->view->redactions().size(), by_page.size())
                    .c_str()) != 1)
    return;
  toast(_("Redacting…"));
  Fl::flush();
  // Pages whose content Xanara cannot take apart under a box (an image
  // inside a form, say) are replaced by a 200 dpi image of themselves.
  int rasterized = 0;
  bool done = engine_change(
      [&](PdfFile &pdf, std::string &e) {
        for (const auto &kv : by_page) {
          std::string why;
          if (redact_page(pdf, kv.first, kv.second, why)) continue;
          std::string img = temp_path(format("redact-%d", kv.first));
          if (!rasterize_page_pdf(*t->doc, kv.first, 200, kv.second, img, e) || !replace_page(pdf, kv.first, img, e))
            return false;
          ++rasterized;
        }
        pdf.structural();  // nothing of the old content may stay in the file
        return true;
      },
      _("Apply Redactions"),
      rasterized ? std::string() : std::string(_("Redactions applied — save to keep the result")));
  if (done && rasterized)
    toast(format(_("Redactions applied; %d page(s) became images — save to keep the result"), rasterized));
  if (done) t->view->clear_redactions();
}

// ------------------------------------------------------------------ tools

void XanaraWindow::compress() {
  DocTab *t = tab();
  if (!t) return;
  int q = 1;
  if (!ask_choice(_("Compress"), _("Image quality"),
                  {_("Small (screen, 72 dpi)"), _("Medium (ebook, 150 dpi)"), _("High (print, 300 dpi)")}, q))
    return;
  t->view->finish_text_edit();
  std::string src = current_source();
  if (src.empty()) return;
  std::string out = ask_save(_("Save the Smaller Copy As"), t->target, stem(t->target) + " (smaller).pdf",
                             _("PDF Documents\t*.pdf"), "pdf");
  if (out.empty()) return;
  toast(_("Compressing…"));
  Fl::flush();
  std::string error;
  std::unique_ptr<PdfFile> pdf = PdfFile::open(src, t->doc->password(), error);
  static const int dpis[] = {72, 150, 300};
  if (!pdf || !write_compressed(*pdf, out, dpis[q], error)) {
    report(_("Compress"), error);
    return;
  }
  long before = file_size(src), after = file_size(out);
  toast(format(_("%s → %s (%s)"), size_text(before).c_str(), size_text(after).c_str(),
               after < before ? format("-%d%%", int(100 - after * 100 / std::max(1L, before))).c_str() : _("no gain")));
}

// Text recognition takes seconds per page: it runs on a thread of its own
// with its own copy of the document, and the window stays usable.
namespace {

struct OcrJob {
  std::string src, password, lang, text_pdf, out;
  int pages = 0;
  std::atomic<int> page{0};
  std::atomic<bool> cancel{false}, finished{false};
  bool ok = false;
  std::string error;
  std::thread worker;
};
std::unique_ptr<OcrJob> g_ocr;

}  // namespace

void XanaraWindow::ocr() {
  DocTab *t = tab();
  if (!t) return;
  if (g_ocr) {
    fl_message_title(_("OCR"));
    if (fl_choice("%s", _("Continue"), _("Stop"), nullptr,
                  format(_("Text is being recognized (page %d of %d). Stop it?"), g_ocr->page + 1, g_ocr->pages)
                      .c_str()) == 1)
      g_ocr->cancel = true;
    return;
  }
  std::string why;
  if (!ocr_available(why)) {
    fl_message_title(_("OCR"));
    fl_alert("%s", why.c_str());
    return;
  }
  std::string lang = ocr_default_language();
  if (!ask_text(format(_("OCR Language (%s)"), ocr_languages().c_str()), lang, false)) return;
  t->view->finish_text_edit();
  std::string out = ask_save(_("Save the Searchable PDF As"), t->target, stem(t->target) + " (OCR).pdf",
                             _("PDF Documents\t*.pdf"), "pdf");
  if (out.empty()) return;
  std::string src = current_source();
  if (src.empty()) return;
  auto job = std::make_unique<OcrJob>();
  job->src = src;
  job->password = t->doc->password();
  job->lang = trim(lang);
  job->text_pdf = temp_path("ocr-text");
  job->out = out;
  job->pages = t->doc->count();
  OcrJob *j = job.get();
  j->worker = std::thread([j] {
    std::string error;
    bool encrypted = false;
    std::unique_ptr<Document> doc = Document::open(j->src, j->password, error, encrypted);
    if (doc)
      j->ok = ocr_text_layer(*doc, j->lang, j->text_pdf,
                             [j](int page) {
                               j->page = page;
                               return !j->cancel;
                             },
                             error);
    j->error = error;
    j->finished = true;
  });
  g_ocr = std::move(job);
  toast(format(_("Recognizing page %d of %d…"), 1, j->pages));
  Fl::add_timeout(0.3, ocr_poll, this);
}

void XanaraWindow::ocr_poll(void *data) {
  auto *self = static_cast<XanaraWindow *>(data);
  if (!g_ocr) return;
  if (!g_ocr->finished) {
    self->toast(format(_("Recognizing page %d of %d…"), g_ocr->page + 1, g_ocr->pages));
    Fl::repeat_timeout(0.3, ocr_poll, data);
    return;
  }
  g_ocr->worker.join();
  std::unique_ptr<OcrJob> j = std::move(g_ocr);
  std::string error = j->error;
  bool ok = j->ok;
  if (!ok) {
    unlink(j->text_pdf.c_str());
    if (j->cancel) self->toast(_("Text recognition stopped"));
    else report(_("OCR"), error);
    return;
  }
  std::unique_ptr<PdfFile> pdf = PdfFile::open(j->src, j->password, error);
  ok = pdf && overlay_pages(*pdf, j->text_pdf, false, error);
  std::string tmp = ok ? temp_beside(j->out) : "";
  ok = ok && !tmp.empty() && pdf->write(tmp, PdfFile::Write::Rewrite, error) &&
       commit_file(tmp, j->out, j->password, j->pages, error);
  pdf.reset();
  unlink(j->text_pdf.c_str());
  if (!ok) {
    report(_("OCR"), error);
    return;
  }
  self->open_path(j->out);
  self->toast(_("Text recognized — the new PDF is searchable"));
}

void XanaraWindow::ocr_shutdown() {
  Fl::remove_timeout(ocr_poll, this);
  if (!g_ocr) return;
  g_ocr->cancel = true;
  g_ocr->worker.join();
  unlink(g_ocr->text_pdf.c_str());
  g_ocr.reset();
}

void XanaraWindow::compare() {
  DocTab *t = tab();
  if (!t) return;
  if (compare_id_ >= 0) {
    compare_id_ = -1;
    for (auto &x : tabs_)
      if (x->view != view()) x->view->hide();
    layout();
    update_chrome();
    return;
  }
  auto files = ask_open_many(_("Compare With"), _("PDF Documents\t*.{pdf,PDF}"), dir_of(t->target));
  if (files.empty()) return;
  int base = tablist_.active();
  int base_id = tablist_.id(base);
  if (!open_path(files[0])) return;
  int other_id = tablist_.id(tablist_.active());
  activate_tab(tablist_.index_of_id(base_id));
  compare_id_ = other_id;
  // A quick textual comparison, page by page.
  Document *a = document();
  Document *b = tabs_[size_t(tablist_.index_of_id(other_id))]->doc.get();
  int n = std::max(a->count(), b->count()), differ = 0;
  for (int p = 0; p < n; ++p) {
    std::string ta = p < a->count() ? a->text(p) : "", tb = p < b->count() ? b->text(p) : "";
    if (trim(ta) != trim(tb)) ++differ;
  }
  layout();
  update_chrome();
  toast(differ ? format(_("%d of %d page(s) differ in their text"), differ, n) : std::string(_("The text of all pages is the same")));
}

}  // namespace xanara
