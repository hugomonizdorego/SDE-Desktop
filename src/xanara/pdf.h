// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Xanara's PDF engine: the objects of a document through libqpdf (cross
// reference tables and streams, object streams, incremental updates,
// repair of damaged files, every standard encryption), the geometry of
// its pages, and writing the result. A change is written as an
// incremental update behind the unchanged original bytes when that is
// possible, otherwise as a complete rewrite; either way into a new file
// that is checked with poppler before it replaces anything. The
// structural operations of the modes (pages, merge, split, stamps, links,
// form fields, crop, metadata, encryption, compression) are in ops.cpp.
// Nothing here touches FLTK, so the unit tests use it directly.
#pragma once

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core.h"

namespace xanara {

// A 2D affine transform as PDF writes it, [a b c d e f]: a point (x, y)
// becomes (a x + c y + e, b x + d y + f). A * B applies A first.
struct Matrix {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  Matrix operator*(const Matrix &o) const;
  Point apply(double x, double y) const { return Point{a * x + c * y + e, b * x + d * y + f}; }
  Point apply(Point p) const { return apply(p.x, p.y); }
  Point vector(double x, double y) const { return Point{a * x + c * y, b * x + d * y}; }
  bool invert(Matrix &out) const;
  double det() const { return a * d - b * c; }
  static Matrix translate(double x, double y) { return Matrix{1, 0, 0, 1, x, y}; }
  static Matrix scale(double sx, double sy) { return Matrix{sx, 0, 0, sy, 0, 0}; }
};

// A rectangle in PDF user space, y up.
struct PdfRect {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// Maps a box in Xanara's page space (top-left origin of the page as shown,
// i.e. after /Rotate) to PDF user space of a page with |media| and |rotate|.
PdfRect display_to_user(const Box &b, const PdfRect &media, int rotate);

// How a page is shown: its visible box (the crop box clipped to the media
// box) and its /Rotate. Converts between PDF user space and Xanara's page
// space (the Box of core.h: points, origin at the top-left corner of the
// page as shown, y down).
struct PageGeometry {
  PdfRect box{0, 0, 612, 792};
  int rotate = 0;
  double width() const;   // as shown
  double height() const;
  Matrix to_display() const;  // user space -> page space
  Matrix to_user() const;     // page space -> user space
};

enum class FieldKind { Text, Check, Radio, Combo, List, Button, Signature };
struct FieldSpec {
  FieldKind kind = FieldKind::Text;
  std::string name;                  // fully qualified field name (/T)
  std::vector<std::string> options;  // choice items
  std::string label;                 // button caption
  bool multiline = false;
};

struct Permissions {
  bool print = true, modify = true, copy = true, annotate = true, forms = true, assemble = true;
};

// One document under edit. Every change goes through this object, which
// remembers the indirect objects it touched so that the result can be
// written as an incremental update.
class PdfFile {
 public:
  ~PdfFile();
  PdfFile(const PdfFile &) = delete;
  PdfFile &operator=(const PdfFile &) = delete;

  // Opens |path| (with |password| when it is encrypted). On failure returns
  // nullptr and explains why in |error|; |encrypted| tells that a
  // (different) password is needed.
  static std::unique_ptr<PdfFile> open(const std::string &path, const std::string &password, std::string &error,
                                       bool *encrypted = nullptr);
  // A new document without pages (merging, extracting, splitting).
  static std::unique_ptr<PdfFile> create();

  QPDF &qpdf() { return *qpdf_; }
  const std::string &path() const { return path_; }
  const std::string &password() const { return password_; }
  int count();
  QPDFObjectHandle page(int i);
  PageGeometry geometry(int i);
  bool encrypted() const;
  // qpdf repaired the file while reading it (a damaged cross-reference
  // table, for example); such files are always written completely.
  bool repaired() const { return repaired_; }
  // The document carries digital signatures (a signed /Sig field).
  bool has_signatures();

  // Records a changed indirect object. Changes to direct objects inside
  // it (a page's /Resources, say) are recorded on the indirect owner.
  void changed(QPDFObjectHandle indirect);
  // Makes |direct| a new indirect object of this file.
  QPDFObjectHandle add(QPDFObjectHandle direct);
  // A new stream; |data| is compressed with Flate unless |compress| is false.
  QPDFObjectHandle add_stream(const std::string &data, QPDFObjectHandle dict, bool compress = true);
  // Replaces the data of an existing stream (compressed with Flate).
  void set_stream_data(QPDFObjectHandle stream, const std::string &data, bool compress = true);
  bool modified() const { return !changed_.empty() || structural_; }
  // An operation changed the document in a way that is written completely
  // (pages removed, objects that must not survive in an older revision).
  void structural() { structural_ = true; }

  enum class Write {
    Auto,         // incremental update when possible, else a complete rewrite
    Rewrite,      // every object that is still used, stream data unchanged
    Compact,      // like Rewrite, with object streams and recompressed streams
  };
  // Writes the document to |out| (which must not be the opened file).
  bool write(const std::string &out, Write mode, std::string &error);
  // The same into memory (previews of edits).
  bool write_memory(std::string &out, Write mode, std::string &error);
  // Would Write::Auto produce an incremental update?
  bool can_append() const;
  // Keeps a document whose pages or objects were copied into this one
  // alive until this one is written.
  void keep(std::unique_ptr<PdfFile> source) { sources_.push_back(std::move(source)); }

 private:
  PdfFile();
  bool append_update(std::string &out_tail, std::string &error);
  bool rewrite(const std::string *out_path, std::string *out_mem, Write mode, std::string &error);

  std::unique_ptr<QPDF> qpdf_;
  std::string path_, password_;
  bool repaired_ = false, structural_ = false, created_ = false;
  long long base_size_ = 0, base_startxref_ = -1;
  bool base_xref_stream_ = false;
  std::set<QPDFObjGen> changed_;
  std::vector<std::unique_ptr<PdfFile>> sources_;
};

// The decoded content of a page (all its content streams, joined).
std::string page_content(QPDFObjectHandle page);
// Replaces a page's content by one new stream.
void set_page_content(PdfFile &pdf, QPDFObjectHandle page, const std::string &data);
// The page's own resource dictionary: an inherited or shared /Resources is
// copied into the page first, so that adding to it changes no other page.
QPDFObjectHandle page_resources(PdfFile &pdf, QPDFObjectHandle page);
// Adds |obj| to the |category| (/Font, /XObject, /ExtGState) of a resource
// dictionary owned by |owner| and returns its name ("/Xn1"); an object that
// is already there keeps its name.
std::string add_resource(PdfFile &pdf, QPDFObjectHandle owner, QPDFObjectHandle resources,
                         const std::string &category, QPDFObjectHandle obj, const std::string &prefix);

// zlib (Flate) compression for new streams.
std::string flate(const std::string &data, int level = 6);

// ------------------------------------------- structural operations (ops.cpp)

// Turns pages by |degrees| (a multiple of 90) more.
bool rotate_pages(PdfFile &pdf, const std::vector<int> &pages, int degrees);
// The pages in a new order: |order| lists old page indices; pages that
// are not listed are deleted, with their form fields and the bookmarks and
// links that pointed at them.
bool reorder_pages(PdfFile &pdf, const std::vector<int> &order, std::string &error);
// Inserts every page of |files| after page |after| (-1: before the first).
bool insert_pages(PdfFile &pdf, int after, const std::vector<std::string> &files, std::string &error);
// Writes |pages| of |pdf| as a new document |out|.
bool extract_pages(PdfFile &pdf, const std::vector<int> &pages, const std::string &out, std::string &error);
// One document per |per_file| pages; |pattern| holds one %d (1, 2, ...).
bool split_pages(PdfFile &pdf, int per_file, const std::string &pattern, std::vector<std::string> &written,
                 std::string &error);
// All pages of |files| in order, as the new document |out|.
bool merge_files(const std::vector<std::string> &files, const std::string &out, std::string &error);
// Draws page k of |stamp_pdf| over (or under) page k of |pdf|.
bool overlay_pages(PdfFile &pdf, const std::string &stamp_pdf, bool underlay, std::string &error);
// Replaces page |page| by the only page of |single|: its look, size and
// content (redaction by rasterizing, OCR).
bool replace_page(PdfFile &pdf, int page, const std::string &single, std::string &error);
// A link annotation to a URI, or to a page (0 based) when |uri| is empty.
bool add_link(PdfFile &pdf, int page, const Box &box, const std::string &uri, int dest_page);
// A new AcroForm field with its widget on |page|.
bool add_field(PdfFile &pdf, int page, const Box &box, const FieldSpec &spec);
// Sets /CropBox of |pages| to |box| (page space of each page).
bool crop_pages(PdfFile &pdf, const std::vector<int> &pages, const Box &box);
// Drops the document information dictionary and the XMP metadata.
bool strip_metadata(PdfFile &pdf);
// Copies with AES-256 (protect) or without encryption (unprotect).
bool write_protected(PdfFile &pdf, const std::string &out, const std::string &user, const std::string &owner,
                     const Permissions &perms, std::string &error);
bool write_unprotected(PdfFile &pdf, const std::string &out, std::string &error);
// A smaller copy: images above |dpi| (72, 150, 300) resampled and stored as
// JPEG, unused resources dropped, object streams and full compression.
bool write_compressed(PdfFile &pdf, const std::string &out, int dpi, std::string &error);

// Replaces |target| by |tmp| (renamed, same directory) after checking with
// poppler that |tmp| opens with |password| and has |pages| pages (-1:
// any). |tmp| is removed when the check fails. The target's file mode is
// kept.
bool commit_file(const std::string &tmp, const std::string &target, const std::string &password, int pages,
                 std::string &error);
// A temporary name next to |target| for commit_file().
std::string temp_beside(const std::string &target);

}  // namespace xanara
