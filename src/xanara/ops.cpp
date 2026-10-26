// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The structural operations of Xanara's modes on libqpdf objects: pages
// (rotate, reorder, delete, insert, extract, split, merge), stamps drawn
// over or under every page, links, form fields, crop boxes, metadata,
// encryption and compression. Each changes a PdfFile in memory; writing
// it is the caller's choice (see pdf.h).
#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFAnnotationObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <map>

#include "content.h"
#include "i18n.h"
#include "layout.h"
#include "pdf.h"
#include "util.h"

using namespace sde;

namespace xanara {

namespace {

QPDFObjectHandle name(const std::string &n) { return QPDFObjectHandle::newName(n); }
QPDFObjectHandle real(double v) { return QPDFObjectHandle::newReal(v, 3, true); }
QPDFObjectHandle rect_array(const PdfRect &r) {
  QPDFObjectHandle a = QPDFObjectHandle::newArray();
  for (double v : {r.x1, r.y1, r.x2, r.y2}) a.appendItem(real(v));
  return a;
}
QPDFObjectHandle numbers(std::initializer_list<double> v) {
  QPDFObjectHandle a = QPDFObjectHandle::newArray();
  for (double d : v) a.appendItem(real(d));
  return a;
}

// Writes |pdf| to a temporary file beside |out| and puts it in place.
bool write_to(PdfFile &pdf, const std::string &out, PdfFile::Write mode, std::string &error) {
  std::string tmp = temp_beside(out);
  if (tmp.empty()) {
    error = strerror(errno);
    return false;
  }
  if (!pdf.write(tmp, mode, error)) {
    unlink(tmp.c_str());
    return false;
  }
  return commit_file(tmp, out, "", pdf.count(), error);
}

// Adds |src| page to |dst| (after |ref|, or at the end), with its form
// fields. Returns the new page.
QPDFObjectHandle copy_page(PdfFile &dst, QPDFObjectHandle page, QPDFAcroFormDocumentHelper &from_forms,
                           QPDFObjectHandle *after, bool at_start) {
  QPDFPageDocumentHelper dh(dst.qpdf());
  if (at_start) dh.addPage(QPDFPageObjectHelper(page), true);
  else if (after && after->isInitialized() && !after->isNull()) dh.addPageAt(QPDFPageObjectHelper(page), false, QPDFPageObjectHelper(*after));
  else dh.addPage(QPDFPageObjectHelper(page), false);
  // The page just added: the one after |after|, the first or the last.
  const auto &all = dst.qpdf().getAllPages();
  QPDFObjectHandle added;
  if (at_start) {
    added = all.front();
  } else if (after && after->isInitialized() && !after->isNull()) {
    for (size_t k = 0; k + 1 < all.size(); ++k)
      if (all[k].getObjGen() == after->getObjGen()) added = all[k + 1];
  } else {
    added = all.back();
  }
  if (added.isInitialized() && page.getOwningQPDF() != &dst.qpdf()) {
    QPDFAcroFormDocumentHelper to(dst.qpdf());
    to.fixCopiedAnnotations(added, page, from_forms);
  }
  return added;
}

}  // namespace

// ------------------------------------------------------------------ pages

bool rotate_pages(PdfFile &pdf, const std::vector<int> &pages, int degrees) {
  bool any = false;
  for (int p : pages) {
    QPDFObjectHandle pg = pdf.page(p);
    if (!pg.isDictionary()) continue;
    QPDFPageObjectHelper(pg).rotatePage(degrees, true);
    pdf.changed(pg);
    any = true;
  }
  return any;
}

namespace {

// Bookmarks, named destinations and links that point at deleted pages
// lose their target, and the form fields of those pages go, so that
// nothing of a deleted page stays reachable in the file.
void forget_pages(PdfFile &pdf, const std::set<QPDFObjGen> &gone) {
  QPDF &q = pdf.qpdf();
  auto points_away = [&](QPDFObjectHandle dest) {
    if (dest.isArray() && dest.getArrayNItems() > 0) {
      QPDFObjectHandle p = dest.getArrayItem(0);
      return p.isIndirect() && gone.count(p.getObjGen()) > 0;
    }
    return false;
  };
  auto scrub = [&](QPDFObjectHandle item) {
    if (!item.isDictionary()) return;
    if (points_away(item.getKey("/Dest"))) item.removeKey("/Dest");
    QPDFObjectHandle a = item.getKey("/A");
    if (a.isDictionary() && points_away(a.getKey("/D"))) item.removeKey("/A");
  };
  // Outlines.
  std::vector<QPDFObjectHandle> todo;
  QPDFObjectHandle outlines = q.getRoot().getKey("/Outlines");
  if (outlines.isDictionary()) todo.push_back(outlines.getKey("/First"));
  std::set<QPDFObjGen> seen;
  while (!todo.empty() && seen.size() < 100000) {
    QPDFObjectHandle o = todo.back();
    todo.pop_back();
    if (!o.isDictionary() || (o.isIndirect() && !seen.insert(o.getObjGen()).second)) continue;
    scrub(o);
    todo.push_back(o.getKey("/First"));
    todo.push_back(o.getKey("/Next"));
  }
  // Named destinations (the old /Dests dictionary and the name tree).
  QPDFObjectHandle dests = q.getRoot().getKey("/Dests");
  if (dests.isDictionary())
    for (const auto &k : dests.getKeys()) {
      QPDFObjectHandle d = dests.getKey(k);
      if (points_away(d) || (d.isDictionary() && points_away(d.getKey("/D")))) dests.removeKey(k);
    }
  QPDFObjectHandle names = q.getRoot().getKey("/Names");
  if (names.isDictionary() && names.getKey("/Dests").isDictionary()) {
    std::vector<QPDFObjectHandle> nodes{names.getKey("/Dests")};
    std::set<QPDFObjGen> nseen;
    while (!nodes.empty() && nseen.size() < 100000) {
      QPDFObjectHandle n = nodes.back();
      nodes.pop_back();
      if (!n.isDictionary() || (n.isIndirect() && !nseen.insert(n.getObjGen()).second)) continue;
      QPDFObjectHandle arr = n.getKey("/Names");
      if (arr.isArray()) {
        QPDFObjectHandle keep = QPDFObjectHandle::newArray();
        auto v = arr.getArrayAsVector();
        for (size_t k = 0; k + 1 < v.size(); k += 2) {
          QPDFObjectHandle d = v[k + 1];
          if (points_away(d) || (d.isDictionary() && points_away(d.getKey("/D")))) continue;
          keep.appendItem(v[k]);
          keep.appendItem(v[k + 1]);
        }
        n.replaceKey("/Names", keep);
      }
      QPDFObjectHandle kids = n.getKey("/Kids");
      if (kids.isArray())
        for (auto k : kids.getArrayAsVector()) nodes.push_back(k);
    }
  }
  // Links on the remaining pages.
  for (const auto &pg : q.getAllPages())
    for (auto &a : QPDFPageObjectHelper(pg).getAnnotations("/Link")) scrub(a.getObjectHandle());
}

}  // namespace

bool reorder_pages(PdfFile &pdf, const std::vector<int> &order, std::string &error) {
  QPDF &q = pdf.qpdf();
  try {
    QPDFPageDocumentHelper dh(q);
    std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
    std::set<int> kept(order.begin(), order.end());
    std::set<QPDFObjGen> gone;
    std::set<QPDFObjGen> fields;
    QPDFAcroFormDocumentHelper forms(q);
    for (int i = 0; i < int(pages.size()); ++i) {
      if (kept.count(i)) continue;
      gone.insert(pages[size_t(i)].getObjectHandle().getObjGen());
      for (auto &a : pages[size_t(i)].getAnnotations("/Widget")) {
        QPDFFormFieldObjectHelper f = forms.getFieldForAnnotation(a);
        if (!f.getObjectHandle().isNull()) fields.insert(f.getObjectHandle().getObjGen());
      }
    }
    for (auto &p : pages) dh.removePage(p);
    for (int i : order) {
      if (i < 0 || i >= int(pages.size())) continue;
      dh.addPage(pages[size_t(i)], false);
    }
    if (!fields.empty()) forms.removeFormFields(fields);
    if (!gone.empty()) forget_pages(pdf, gone);
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  pdf.structural();
  return true;
}

bool insert_pages(PdfFile &pdf, int after, const std::vector<std::string> &files, std::string &error) {
  try {
    QPDFObjectHandle ref = after >= 0 ? pdf.page(after) : QPDFObjectHandle();
    bool at_start = after < 0;
    for (const auto &f : files) {
      std::unique_ptr<PdfFile> src = PdfFile::open(f, "", error);
      if (!src) {
        error = format("“%s”: %s", basename_of(f).c_str(), error.c_str());
        return false;
      }
      QPDFAcroFormDocumentHelper from(src->qpdf());
      std::vector<QPDFObjectHandle> pages = src->qpdf().getAllPages();
      for (auto &p : pages) {
        ref = copy_page(pdf, p, from, &ref, at_start);
        at_start = false;
      }
      pdf.keep(std::move(src));
    }
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  pdf.structural();
  return true;
}

bool extract_pages(PdfFile &pdf, const std::vector<int> &pages, const std::string &out, std::string &error) {
  std::unique_ptr<PdfFile> dst = PdfFile::create();
  try {
    QPDFAcroFormDocumentHelper from(pdf.qpdf());
    for (int p : pages) {
      QPDFObjectHandle pg = pdf.page(p);
      if (pg.isDictionary()) copy_page(*dst, pg, from, nullptr, false);
    }
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  if (dst->count() == 0) {
    error = _("no pages were selected");
    return false;
  }
  return write_to(*dst, out, PdfFile::Write::Rewrite, error);
}

bool split_pages(PdfFile &pdf, int per_file, const std::string &pattern, std::vector<std::string> &written,
                 std::string &error) {
  int n = pdf.count(), k = std::max(1, per_file);
  for (int first = 0, part = 1; first < n; first += k, ++part) {
    std::vector<int> pages;
    for (int p = first; p < std::min(n, first + k); ++p) pages.push_back(p);
    std::string out = format(pattern.c_str(), part);
    if (!extract_pages(pdf, pages, out, error)) return false;
    written.push_back(out);
  }
  return true;
}

bool merge_files(const std::vector<std::string> &files, const std::string &out, std::string &error) {
  std::unique_ptr<PdfFile> dst = PdfFile::create();
  try {
    for (const auto &f : files) {
      std::unique_ptr<PdfFile> src = PdfFile::open(f, "", error);
      if (!src) {
        error = format("“%s”: %s", basename_of(f).c_str(), error.c_str());
        return false;
      }
      QPDFAcroFormDocumentHelper from(src->qpdf());
      for (auto &p : src->qpdf().getAllPages()) copy_page(*dst, p, from, nullptr, false);
      dst->keep(std::move(src));
    }
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  return write_to(*dst, out, PdfFile::Write::Rewrite, error);
}

bool overlay_pages(PdfFile &pdf, const std::string &stamp_pdf, bool underlay, std::string &error) {
  std::unique_ptr<PdfFile> stamp = PdfFile::open(stamp_pdf, "", error);
  if (!stamp) return false;
  try {
    std::vector<QPDFObjectHandle> stamps = stamp->qpdf().getAllPages();
    int n = pdf.count();
    for (int i = 0; i < n && !stamps.empty(); ++i) {
      QPDFObjectHandle pg = pdf.page(i);
      QPDFObjectHandle fo = QPDFPageObjectHelper(stamps[std::min(size_t(i), stamps.size() - 1)]).getFormXObjectForPage();
      QPDFObjectHandle copy = pdf.qpdf().copyForeignObject(fo);
      QPDFObjectHandle res = page_resources(pdf, pg);
      std::string nm = add_resource(pdf, pg, res, "/XObject", copy, "Fx");
      QPDFPageObjectHelper ph(pg);
      QPDFObjectHandle box = ph.getCropBox(false, false);
      if (!box.isRectangle()) box = ph.getMediaBox();
      std::string ops = ph.placeFormXObject(copy, nm, box.getArrayAsRectangle());
      if (underlay) {
        ph.addPageContents(pdf.add_stream("q\n" + ops + "\nQ\n", QPDFObjectHandle::newNull()), true);
      } else {
        // The old content in q/Q, so that it cannot move the stamp.
        ph.addPageContents(pdf.add_stream("q\n", QPDFObjectHandle::newNull(), false), true);
        ph.addPageContents(pdf.add_stream("\nQ\n" + ops + "\n", QPDFObjectHandle::newNull()), false);
      }
      pdf.changed(pg);
    }
    pdf.keep(std::move(stamp));
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  return true;
}

bool replace_page(PdfFile &pdf, int page, const std::string &single, std::string &error) {
  std::unique_ptr<PdfFile> src = PdfFile::open(single, "", error);
  if (!src) return false;
  try {
    QPDFObjectHandle from = src->page(0), to = pdf.page(page);
    if (!from.isDictionary() || !to.isDictionary()) {
      error = "no such page";
      return false;
    }
    QPDFObjectHandle media = QPDFPageObjectHelper(from).getMediaBox();
    QPDFObjectHandle res = QPDFPageObjectHelper(from).getAttribute("/Resources", false);
    for (const char *key : {"/CropBox", "/TrimBox", "/BleedBox", "/ArtBox", "/Rotate", "/Annots", "/Group", "/Thumb"})
      to.removeKey(key);
    to.replaceKey("/MediaBox", media.shallowCopy());
    to.replaceKey("/Resources", res.isDictionary() ? pdf.qpdf().copyForeignObject(res) : QPDFObjectHandle::newDictionary());
    std::string content = page_content(from);
    to.replaceKey("/Contents", pdf.add_stream(content, QPDFObjectHandle::newNull()));
    pdf.changed(to);
    pdf.keep(std::move(src));
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  pdf.structural();
  return true;
}

// ------------------------------------------------------- links and forms

static void append_annot(PdfFile &pdf, QPDFObjectHandle page, QPDFObjectHandle annot) {
  QPDFObjectHandle annots = page.getKey("/Annots");
  if (annots.isArray()) {
    annots.appendItem(annot);
    if (annots.isIndirect()) pdf.changed(annots);
  } else {
    QPDFObjectHandle a = QPDFObjectHandle::newArray();
    a.appendItem(annot);
    page.replaceKey("/Annots", a);
  }
  pdf.changed(page);
}

bool add_link(PdfFile &pdf, int page, const Box &box, const std::string &uri, int dest_page) {
  QPDFObjectHandle pg = pdf.page(page);
  if (!pg.isDictionary()) return false;
  if (uri.empty() && (dest_page < 0 || dest_page >= pdf.count())) return false;
  PageGeometry g = pdf.geometry(page);
  PdfRect r = display_to_user(box, g.box, g.rotate);
  QPDFObjectHandle a = QPDFObjectHandle::newDictionary();
  a.replaceKey("/Type", name("/Annot"));
  a.replaceKey("/Subtype", name("/Link"));
  a.replaceKey("/Rect", rect_array(r));
  a.replaceKey("/Border", numbers({0, 0, 0}));
  a.replaceKey("/F", QPDFObjectHandle::newInteger(4));
  a.replaceKey("/P", pg);
  if (!uri.empty()) {
    QPDFObjectHandle act = QPDFObjectHandle::newDictionary();
    act.replaceKey("/S", name("/URI"));
    act.replaceKey("/URI", QPDFObjectHandle::newString(uri));
    a.replaceKey("/A", act);
  } else {
    QPDFObjectHandle dest = QPDFObjectHandle::newArray();
    dest.appendItem(pdf.page(dest_page));
    dest.appendItem(name("/Fit"));
    a.replaceKey("/Dest", dest);
  }
  append_annot(pdf, pg, pdf.add(a));
  return true;
}

static std::string num(double v) { return pdf_num(v, 2); }

// Latin-1 text for the Helvetica appearance of a field (WinAnsi).
static std::string winansi(const std::string &s) {
  std::string out;
  for (char32_t c : utf32(s)) out += c < 256 && c >= 32 ? char(c) : '?';
  return out;
}

bool add_field(PdfFile &pdf, int page, const Box &box, const FieldSpec &spec) {
  QPDFObjectHandle pg = pdf.page(page);
  if (!pg.isDictionary() || spec.name.empty()) return false;
  PageGeometry g = pdf.geometry(page);
  PdfRect r = display_to_user(box, g.box, g.rotate);
  double w = r.x2 - r.x1, h = r.y2 - r.y1;
  if (w < 4 || h < 4) return false;
  QPDF &q = pdf.qpdf();
  QPDFObjectHandle root = q.getRoot();
  QPDFObjectHandle form = root.getKey("/AcroForm");
  if (!form.isDictionary()) {
    form = pdf.add(QPDFObjectHandle::newDictionary());
    root.replaceKey("/AcroForm", form);
    pdf.changed(root);
  } else if (form.isIndirect()) {
    pdf.changed(form);
  } else {
    pdf.changed(root);
  }
  QPDFObjectHandle helv = QPDFObjectHandle::newDictionary();
  helv.replaceKey("/Type", name("/Font"));
  helv.replaceKey("/Subtype", name("/Type1"));
  helv.replaceKey("/BaseFont", name("/Helvetica"));
  helv.replaceKey("/Encoding", name("/WinAnsiEncoding"));
  QPDFObjectHandle helv_ref = pdf.add(helv);
  if (!form.getKey("/DR").isDictionary()) {
    QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
    fonts.replaceKey("/Helv", helv_ref);
    QPDFObjectHandle dr = QPDFObjectHandle::newDictionary();
    dr.replaceKey("/Font", fonts);
    form.replaceKey("/DR", dr);
  }
  if (!form.hasKey("/DA")) form.replaceKey("/DA", QPDFObjectHandle::newString("/Helv 0 Tf 0 g"));
  form.replaceKey("/NeedAppearances", QPDFObjectHandle::newBool(true));
  auto xobject = [&](const std::string &ops, bool with_font) {
    QPDFObjectHandle d = QPDFObjectHandle::newDictionary();
    d.replaceKey("/Type", name("/XObject"));
    d.replaceKey("/Subtype", name("/Form"));
    d.replaceKey("/BBox", numbers({0, 0, w, h}));
    if (with_font) {
      QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
      fonts.replaceKey("/Helv", helv_ref);
      QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
      res.replaceKey("/Font", fonts);
      d.replaceKey("/Resources", res);
    }
    return pdf.add_stream(ops, d);
  };
  std::string border = "0.45 0.5 0.58 RG 1 w 0.5 0.5 " + num(w - 1) + " " + num(h - 1) + " re S\n";
  std::string white = "1 1 1 rg 0 0 " + num(w) + " " + num(h) + " re f\n";
  QPDFObjectHandle wd = QPDFObjectHandle::newDictionary();
  wd.replaceKey("/Type", name("/Annot"));
  wd.replaceKey("/Subtype", name("/Widget"));
  wd.replaceKey("/Rect", rect_array(r));
  wd.replaceKey("/F", QPDFObjectHandle::newInteger(4));
  wd.replaceKey("/P", pg);
  wd.replaceKey("/T", QPDFObjectHandle::newUnicodeString(spec.name));
  QPDFObjectHandle mk = QPDFObjectHandle::newDictionary();
  mk.replaceKey("/BC", numbers({0.45, 0.5, 0.58}));
  mk.replaceKey("/BG", numbers({1, 1, 1}));
  QPDFObjectHandle ap = QPDFObjectHandle::newDictionary();
  switch (spec.kind) {
    case FieldKind::Text:
      wd.replaceKey("/FT", name("/Tx"));
      wd.replaceKey("/V", QPDFObjectHandle::newString(""));
      wd.replaceKey("/DA", QPDFObjectHandle::newString("/Helv 0 Tf 0 g"));
      if (spec.multiline) wd.replaceKey("/Ff", QPDFObjectHandle::newInteger(4096));
      ap.replaceKey("/N", xobject(white + border, false));
      break;
    case FieldKind::Check:
    case FieldKind::Radio: {
      bool radio = spec.kind == FieldKind::Radio;
      std::string on = radio ? "/Choice" : "/Yes";
      wd.replaceKey("/FT", name("/Btn"));
      if (radio) wd.replaceKey("/Ff", QPDFObjectHandle::newInteger(49152));
      wd.replaceKey("/V", name("/Off"));
      wd.replaceKey("/AS", name("/Off"));
      double s = std::min(w, h);
      std::string frame, mark;
      if (radio) {
        double cx = w / 2, cy = h / 2, rr = s / 2 - 1;
        auto circle = [&](double rad) {
          double k = 0.5523 * rad;
          return num(cx + rad) + " " + num(cy) + " m " + num(cx + rad) + " " + num(cy + k) + " " + num(cx + k) + " " +
                 num(cy + rad) + " " + num(cx) + " " + num(cy + rad) + " c " + num(cx - k) + " " + num(cy + rad) + " " +
                 num(cx - rad) + " " + num(cy + k) + " " + num(cx - rad) + " " + num(cy) + " c " + num(cx - rad) + " " +
                 num(cy - k) + " " + num(cx - k) + " " + num(cy - rad) + " " + num(cx) + " " + num(cy - rad) + " c " +
                 num(cx + k) + " " + num(cy - rad) + " " + num(cx + rad) + " " + num(cy - k) + " " + num(cx + rad) + " " +
                 num(cy) + " c ";
        };
        frame = "1 1 1 rg 0.45 0.5 0.58 RG 1 w " + circle(rr) + "b\n";
        mark = "0 0 0 rg " + circle(rr * 0.5) + "f\n";
      } else {
        frame = white + border;
        mark = "0 0 0 RG " + num(std::max(1.0, s * 0.12)) + " w " + num(w * 0.22) + " " + num(h * 0.52) + " m " +
               num(w * 0.42) + " " + num(h * 0.28) + " l " + num(w * 0.8) + " " + num(h * 0.76) + " l S\n";
      }
      QPDFObjectHandle n = QPDFObjectHandle::newDictionary();
      n.replaceKey(on, xobject(frame + mark, false));
      n.replaceKey("/Off", xobject(frame, false));
      ap.replaceKey("/N", n);
      break;
    }
    case FieldKind::Combo:
    case FieldKind::List: {
      wd.replaceKey("/FT", name("/Ch"));
      if (spec.kind == FieldKind::Combo) wd.replaceKey("/Ff", QPDFObjectHandle::newInteger(131072));
      QPDFObjectHandle opt = QPDFObjectHandle::newArray();
      for (const auto &o : spec.options) opt.appendItem(QPDFObjectHandle::newUnicodeString(o));
      wd.replaceKey("/Opt", opt);
      wd.replaceKey("/V", QPDFObjectHandle::newUnicodeString(spec.options.empty() ? "" : spec.options.front()));
      wd.replaceKey("/DA", QPDFObjectHandle::newString("/Helv 0 Tf 0 g"));
      std::string text;
      if (!spec.options.empty()) {
        double fs = std::min(12.0, h * 0.6);
        text = "BT /Helv " + num(fs) + " Tf 0 g 3 " + num(h - fs - 2) + " Td " +
               pdf_literal(winansi(spec.options.front())) + " Tj ET\n";
      }
      ap.replaceKey("/N", xobject(white + border + text, true));
      break;
    }
    case FieldKind::Button: {
      wd.replaceKey("/FT", name("/Btn"));
      wd.replaceKey("/Ff", QPDFObjectHandle::newInteger(65536));
      std::string label = spec.label.empty() ? spec.name : spec.label;
      mk.replaceKey("/CA", QPDFObjectHandle::newUnicodeString(label));
      mk.replaceKey("/BG", numbers({0.88, 0.91, 0.95}));
      double fs = std::min(12.0, h * 0.55), tw = fs * 0.5 * double(utf32(label).size());
      std::string ops = "0.88 0.91 0.95 rg 0 0 " + num(w) + " " + num(h) + " re f\n" + border + "BT /Helv " + num(fs) +
                        " Tf 0 g " + num(std::max(2.0, (w - tw) / 2)) + " " + num((h - fs) / 2 + fs * 0.2) + " Td " +
                        pdf_literal(winansi(label)) + " Tj ET\n";
      ap.replaceKey("/N", xobject(ops, true));
      break;
    }
    case FieldKind::Signature:
      wd.replaceKey("/FT", name("/Sig"));
      ap.replaceKey("/N", xobject("0.96 0.97 1 rg 0 0 " + num(w) + " " + num(h) + " re f\n" + border, false));
      break;
  }
  wd.replaceKey("/MK", mk);
  wd.replaceKey("/AP", ap);
  QPDFObjectHandle widget = pdf.add(wd);
  QPDFObjectHandle fields = form.getKey("/Fields");
  if (fields.isArray()) {
    fields.appendItem(widget);
    if (fields.isIndirect()) pdf.changed(fields);
  } else {
    QPDFObjectHandle a = QPDFObjectHandle::newArray();
    a.appendItem(widget);
    form.replaceKey("/Fields", a);
  }
  append_annot(pdf, pg, widget);
  return true;
}

bool crop_pages(PdfFile &pdf, const std::vector<int> &pages, const Box &box) {
  bool any = false;
  for (int p : pages) {
    QPDFObjectHandle pg = pdf.page(p);
    if (!pg.isDictionary()) continue;
    PageGeometry g = pdf.geometry(p);
    PdfRect r = display_to_user(box, g.box, g.rotate);
    r.x1 = std::max(r.x1, g.box.x1);
    r.y1 = std::max(r.y1, g.box.y1);
    r.x2 = std::min(r.x2, g.box.x2);
    r.y2 = std::min(r.y2, g.box.y2);
    if (r.x2 - r.x1 < 8 || r.y2 - r.y1 < 8) continue;
    pg.replaceKey("/CropBox", rect_array(r));
    pdf.changed(pg);
    any = true;
  }
  return any;
}

bool strip_metadata(PdfFile &pdf) {
  QPDF &q = pdf.qpdf();
  bool changed = false;
  QPDFObjectHandle trailer = q.getTrailer();
  if (trailer.hasKey("/Info")) {
    trailer.removeKey("/Info");
    changed = true;
  }
  QPDFObjectHandle root = q.getRoot();
  if (root.hasKey("/Metadata")) {
    root.removeKey("/Metadata");
    pdf.changed(root);
    changed = true;
  }
  // Nothing of it may survive in an older revision of the file.
  if (changed) pdf.structural();
  return changed;
}

// ------------------------------------------------------- whole documents

bool write_protected(PdfFile &pdf, const std::string &out, const std::string &user, const std::string &owner,
                     const Permissions &perms, std::string &error) {
  std::string tmp = temp_beside(out);
  if (tmp.empty()) {
    error = strerror(errno);
    return false;
  }
  try {
    QPDFWriter w(pdf.qpdf(), tmp.c_str());
    w.setR6EncryptionParameters(user.c_str(), (owner.empty() ? user : owner).c_str(), true, perms.copy,
                                perms.assemble, perms.annotate, perms.forms, perms.modify,
                                perms.print ? qpdf_r3p_full : qpdf_r3p_none, true);
    w.write();
  } catch (std::exception &e) {
    error = e.what();
    unlink(tmp.c_str());
    return false;
  }
  return commit_file(tmp, out, user, pdf.count(), error);
}

bool write_unprotected(PdfFile &pdf, const std::string &out, std::string &error) {
  try {
    QPDFWriter w(pdf.qpdf(), out.c_str());
    w.setPreserveEncryption(false);
    w.setCompressStreams(false);
    w.setDecodeLevel(qpdf_dl_none);
    w.write();
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  return true;
}

namespace {

struct JpegError {
  jpeg_error_mgr mgr;
  jmp_buf jump;
};

void jpeg_fail(j_common_ptr c) { longjmp(reinterpret_cast<JpegError *>(c->err)->jump, 1); }

bool encode_jpeg(const std::string &px, int w, int h, int comps, int quality, std::string &out) {
  jpeg_compress_struct c;
  JpegError err;
  c.err = jpeg_std_error(&err.mgr);
  err.mgr.error_exit = jpeg_fail;
  unsigned char *buf = nullptr;
  unsigned long size = 0;
  if (setjmp(err.jump)) {
    jpeg_destroy_compress(&c);
    free(buf);
    return false;
  }
  jpeg_create_compress(&c);
  jpeg_mem_dest(&c, &buf, &size);
  c.image_width = JDIMENSION(w);
  c.image_height = JDIMENSION(h);
  c.input_components = comps;
  c.in_color_space = comps == 1 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(&c);
  jpeg_set_quality(&c, quality, TRUE);
  jpeg_start_compress(&c, TRUE);
  while (c.next_scanline < c.image_height) {
    JSAMPROW row = reinterpret_cast<JSAMPROW>(const_cast<char *>(px.data() + size_t(c.next_scanline) * size_t(w) * size_t(comps)));
    jpeg_write_scanlines(&c, &row, 1);
  }
  jpeg_finish_compress(&c);
  jpeg_destroy_compress(&c);
  out.assign(reinterpret_cast<char *>(buf), size);
  free(buf);
  return true;
}

// Box filter resampling of 8 bit samples.
std::string resample(const std::string &px, int w, int h, int comps, int nw, int nh) {
  std::string out(size_t(nw) * size_t(nh) * size_t(comps), '\0');
  for (int y = 0; y < nh; ++y) {
    int y1 = y * h / nh, y2 = std::max(y1 + 1, (y + 1) * h / nh);
    for (int x = 0; x < nw; ++x) {
      int x1 = x * w / nw, x2 = std::max(x1 + 1, (x + 1) * w / nw);
      for (int k = 0; k < comps; ++k) {
        unsigned long sum = 0, n = 0;
        for (int yy = y1; yy < y2; ++yy)
          for (int xx = x1; xx < x2; ++xx) {
            sum += (unsigned char)px[(size_t(yy) * size_t(w) + size_t(xx)) * size_t(comps) + size_t(k)];
            ++n;
          }
        out[(size_t(y) * size_t(nw) + size_t(x)) * size_t(comps) + size_t(k)] = char(sum / std::max(1ul, n));
      }
    }
  }
  return out;
}

int image_components(QPDFObjectHandle cs) {
  if (cs.isName()) return cs.getName() == "/DeviceGray" ? 1 : cs.getName() == "/DeviceRGB" ? 3 : 0;
  if (cs.isArray() && cs.getArrayNItems() == 2 && cs.getArrayItem(0).isName() &&
      cs.getArrayItem(0).getName() == "/ICCBased" && cs.getArrayItem(1).isStream()) {
    QPDFObjectHandle n = cs.getArrayItem(1).getDict().getKey("/N");
    int c = n.isInteger() ? n.getIntValueAsInt() : 0;
    return c == 1 || c == 3 ? c : 0;
  }
  return 0;
}

}  // namespace

bool write_compressed(PdfFile &pdf, const std::string &out, int dpi, std::string &error) {
  int quality = dpi <= 72 ? 50 : dpi <= 150 ? 70 : 85;
  try {
    // How large each image is shown, at most, over all pages.
    std::map<QPDFObjGen, double> shown_dpi;  // the lowest resolution it needs
    std::map<QPDFObjGen, QPDFObjectHandle> images;
    for (int p = 0; p < pdf.count(); ++p) {
      std::unique_ptr<PageLayout> layout = PageLayout::read(pdf, p);
      for (const ImageItem &im : layout->images) {
        if (im.inline_image || !im.xobject.isIndirect() || im.pixels_w <= 0) continue;
        double inches = std::max(im.box.w(), im.box.h()) / 72.0;
        double px = std::max(im.pixels_w, im.pixels_h);
        double need = inches > 0 ? px / inches : 1e9;
        QPDFObjGen og = im.xobject.getObjGen();
        auto it = shown_dpi.find(og);
        shown_dpi[og] = it == shown_dpi.end() ? need : std::min(it->second, need);
        images[og] = im.xobject;
      }
    }
    for (auto &kv : images) {
      QPDFObjectHandle x = kv.second;
      QPDFObjectHandle d = x.getDict();
      double have = shown_dpi[kv.first];
      if (have < dpi * 1.4) continue;
      int w = d.getKey("/Width").isInteger() ? d.getKey("/Width").getIntValueAsInt() : 0;
      int h = d.getKey("/Height").isInteger() ? d.getKey("/Height").getIntValueAsInt() : 0;
      int bpc = d.getKey("/BitsPerComponent").isInteger() ? d.getKey("/BitsPerComponent").getIntValueAsInt() : 0;
      int comps = image_components(d.getKey("/ColorSpace"));
      if (w < 16 || h < 16 || bpc != 8 || comps == 0 || d.hasKey("/Decode") || d.hasKey("/Mask") ||
          (d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue()))
        continue;
      std::shared_ptr<Buffer> raw = x.getRawStreamData();
      std::string px;
      try {
        std::shared_ptr<Buffer> b = x.getStreamData(qpdf_dl_all);
        px.assign(reinterpret_cast<const char *>(b->getBuffer()), b->getSize());
      } catch (std::exception &) {
        continue;
      }
      if (px.size() < size_t(w) * size_t(h) * size_t(comps)) continue;
      double k = dpi / have;
      int nw = std::max(1, int(std::lround(w * k))), nh = std::max(1, int(std::lround(h * k)));
      std::string small = resample(px, w, h, comps, nw, nh), jpeg;
      if (!encode_jpeg(small, nw, nh, comps, quality, jpeg) || jpeg.size() >= raw->getSize()) continue;
      x.replaceStreamData(jpeg, name("/DCTDecode"), QPDFObjectHandle::newNull());
      d.replaceKey("/Width", QPDFObjectHandle::newInteger(nw));
      d.replaceKey("/Height", QPDFObjectHandle::newInteger(nh));
      d.removeKey("/DecodeParms");
      QPDFObjectHandle smask = d.getKey("/SMask");
      if (smask.isStream()) {
        QPDFObjectHandle sd = smask.getDict();
        int mw = sd.getKey("/Width").isInteger() ? sd.getKey("/Width").getIntValueAsInt() : 0;
        int mh = sd.getKey("/Height").isInteger() ? sd.getKey("/Height").getIntValueAsInt() : 0;
        std::string mpx;
        try {
          std::shared_ptr<Buffer> b = smask.getStreamData(qpdf_dl_all);
          mpx.assign(reinterpret_cast<const char *>(b->getBuffer()), b->getSize());
        } catch (std::exception &) {
          mpx.clear();
        }
        if (mw == w && mh == h && mpx.size() >= size_t(w) * size_t(h) &&
            sd.getKey("/BitsPerComponent").isInteger() && sd.getKey("/BitsPerComponent").getIntValueAsInt() == 8) {
          std::string ms = resample(mpx, w, h, 1, nw, nh);
          smask.replaceStreamData(flate(ms, 9), name("/FlateDecode"), QPDFObjectHandle::newNull());
          sd.replaceKey("/Width", QPDFObjectHandle::newInteger(nw));
          sd.replaceKey("/Height", QPDFObjectHandle::newInteger(nh));
          sd.removeKey("/DecodeParms");
        }
      }
    }
    QPDFPageDocumentHelper(pdf.qpdf()).removeUnreferencedResources();
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  std::string tmp = temp_beside(out);
  if (tmp.empty()) {
    error = strerror(errno);
    return false;
  }
  try {
    Pl_Flate::setCompressionLevel(9);
    QPDFWriter w(pdf.qpdf(), tmp.c_str());
    w.setObjectStreamMode(qpdf_o_generate);
    w.setCompressStreams(true);
    w.setDecodeLevel(qpdf_dl_generalized);
    w.setRecompressFlate(true);
    w.write();
    Pl_Flate::setCompressionLevel(-1);
  } catch (std::exception &e) {
    Pl_Flate::setCompressionLevel(-1);
    error = e.what();
    unlink(tmp.c_str());
    return false;
  }
  return commit_file(tmp, out, pdf.password(), pdf.count(), error);
}

}  // namespace xanara
