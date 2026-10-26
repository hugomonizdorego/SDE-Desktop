// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The PDF engine's file layer; see pdf.h. Reading, repairing and
// decrypting is libqpdf's work. Xanara writes changes itself as an
// incremental update (the changed and new objects, a cross-reference
// section in the style of the last one and a trailer that points back to
// it) behind a byte-identical copy of the original, or lets QPDFWriter
// rewrite the whole file when an update is not possible: damaged and
// encrypted files, and changes that must not leave the old objects
// behind (deleted pages, redaction).
#include "pdf.h"

#include <fcntl.h>
#include <poppler.h>
#include <qpdf/Buffer.hh>
#include <qpdf/Pl_String.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "i18n.h"
#include "util.h"

using namespace sde;

namespace xanara {

// ---------------------------------------------------------------- matrix

Matrix Matrix::operator*(const Matrix &o) const {
  return Matrix{a * o.a + b * o.c,       a * o.b + b * o.d,       c * o.a + d * o.c,
                c * o.b + d * o.d,       e * o.a + f * o.c + o.e, e * o.b + f * o.d + o.f};
}

bool Matrix::invert(Matrix &out) const {
  double dt = det();
  if (std::fabs(dt) < 1e-12) return false;
  out.a = d / dt;
  out.b = -b / dt;
  out.c = -c / dt;
  out.d = a / dt;
  out.e = (c * f - d * e) / dt;
  out.f = (b * e - a * f) / dt;
  return true;
}

// -------------------------------------------------------------- geometry

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

double PageGeometry::width() const {
  return rotate == 90 || rotate == 270 ? box.y2 - box.y1 : box.x2 - box.x1;
}

double PageGeometry::height() const {
  return rotate == 90 || rotate == 270 ? box.x2 - box.x1 : box.y2 - box.y1;
}

Matrix PageGeometry::to_display() const {
  switch (rotate) {
    case 90: return Matrix{0, 1, 1, 0, -box.y1, -box.x1};
    case 180: return Matrix{-1, 0, 0, 1, box.x2, -box.y1};
    case 270: return Matrix{0, -1, -1, 0, box.y2, box.x2};
    default: return Matrix{1, 0, 0, -1, -box.x1, box.y2};
  }
}

Matrix PageGeometry::to_user() const {
  Matrix inv;
  to_display().invert(inv);
  return inv;
}

// ------------------------------------------------------------- helpers

std::string flate(const std::string &data, int level) {
  uLongf n = compressBound(uLong(data.size()));
  std::string out(n, '\0');
  if (compress2(reinterpret_cast<Bytef *>(&out[0]), &n, reinterpret_cast<const Bytef *>(data.data()),
                uLong(data.size()), level) != Z_OK)
    return "";
  out.resize(n);
  return out;
}

static bool read_tail(const std::string &path, long long &size, long long &startxref, bool &xref_stream) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  size = in.tellg();
  if (size < 32) return false;
  long long n = std::min<long long>(size, 2048);
  std::string tail(size_t(n), '\0');
  in.seekg(size - n);
  in.read(&tail[0], n);
  size_t at = tail.rfind("startxref");
  if (at == std::string::npos) return false;
  const char *p = tail.c_str() + at + 9;
  while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') ++p;
  if (*p < '0' || *p > '9') return false;
  startxref = strtoll(p, nullptr, 10);
  if (startxref <= 0 || startxref >= size) return false;
  char head[32] = {0};
  in.seekg(startxref);
  in.read(head, sizeof head - 1);
  const char *h = head;
  while (*h == ' ' || *h == '\r' || *h == '\n' || *h == '\t') ++h;
  xref_stream = strncmp(h, "xref", 4) != 0;
  if (xref_stream && (*h < '0' || *h > '9')) return false;  // neither a table nor an object
  return true;
}

static bool copy_file(const std::string &from, const std::string &to, std::string &error) {
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!in || !out) {
    error = strerror(errno);
    return false;
  }
  out << in.rdbuf();
  if (!out) {
    error = strerror(errno);
    return false;
  }
  return true;
}

// ----------------------------------------------------------------- file

PdfFile::PdfFile() = default;
PdfFile::~PdfFile() = default;

std::unique_ptr<PdfFile> PdfFile::open(const std::string &path, const std::string &password, std::string &error,
                                       bool *encrypted) {
  if (encrypted) *encrypted = false;
  std::unique_ptr<PdfFile> f(new PdfFile());
  f->qpdf_.reset(new QPDF());
  QPDF &q = *f->qpdf_;
  q.setSuppressWarnings(true);
  q.setAttemptRecovery(true);
  try {
    q.processFile(path.c_str(), password.empty() ? nullptr : password.c_str());
    // Walk the page tree now, so that damage there shows up here.
    if (q.getAllPages().empty()) {
      error = _("the document has no pages");
      return nullptr;
    }
  } catch (QPDFExc &e) {
    if (e.getErrorCode() == qpdf_e_password) {
      if (encrypted) *encrypted = true;
      error = _("the password is not right");
    } else {
      error = e.getMessageDetail().empty() ? e.what() : e.getMessageDetail();
    }
    return nullptr;
  } catch (std::exception &e) {
    error = e.what();
    return nullptr;
  }
  f->path_ = path;
  f->password_ = password;
  f->repaired_ = q.anyWarnings();
  for (const auto &kv : q.getXRefTable()) f->base_max_id_ = std::max(f->base_max_id_, kv.first.getObj());
  if (!read_tail(path, f->base_size_, f->base_startxref_, f->base_xref_stream_)) f->base_startxref_ = -1;
  return f;
}

std::unique_ptr<PdfFile> PdfFile::create() {
  std::unique_ptr<PdfFile> f(new PdfFile());
  f->qpdf_.reset(new QPDF());
  f->qpdf_->setSuppressWarnings(true);
  f->qpdf_->emptyPDF();
  f->created_ = true;
  return f;
}

int PdfFile::count() { return int(qpdf_->getAllPages().size()); }

QPDFObjectHandle PdfFile::page(int i) {
  const auto &pages = qpdf_->getAllPages();
  if (i < 0 || i >= int(pages.size())) return QPDFObjectHandle::newNull();
  return pages[size_t(i)];
}

static bool rect_of(QPDFObjectHandle a, PdfRect &r) {
  if (!a.isArray() || a.getArrayNItems() != 4) return false;
  double v[4];
  for (int k = 0; k < 4; ++k) {
    QPDFObjectHandle x = a.getArrayItem(k);
    if (!x.isNumber()) return false;
    v[k] = x.getNumericValue();
  }
  r = PdfRect{std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3])};
  return true;
}

PageGeometry PdfFile::geometry(int i) {
  PageGeometry g;
  QPDFObjectHandle pg = page(i);
  if (!pg.isDictionary()) return g;
  QPDFPageObjectHelper ph(pg);
  PdfRect media{0, 0, 612, 792}, crop;
  rect_of(ph.getMediaBox(), media);
  if (rect_of(ph.getCropBox(), crop)) {
    PdfRect c{std::max(media.x1, crop.x1), std::max(media.y1, crop.y1), std::min(media.x2, crop.x2),
              std::min(media.y2, crop.y2)};
    if (c.x2 > c.x1 && c.y2 > c.y1) media = c;
  }
  g.box = media;
  QPDFObjectHandle rot = ph.getAttribute("/Rotate", false);
  int r = rot.isNumber() ? int(std::lround(rot.getNumericValue())) : 0;
  g.rotate = ((r % 360) + 360) % 360 / 90 * 90;
  return g;
}

bool PdfFile::encrypted() const { return qpdf_->isEncrypted(); }

bool PdfFile::has_signatures() {
  QPDFObjectHandle form = qpdf_->getRoot().getKey("/AcroForm");
  if (!form.isDictionary()) return false;
  QPDFObjectHandle flags = form.getKey("/SigFlags");
  if (flags.isInteger() && (flags.getIntValue() & 1)) return true;
  // A signed /Sig field without the flag.
  std::vector<QPDFObjectHandle> todo{form.getKey("/Fields")};
  std::set<QPDFObjGen> seen;
  for (size_t k = 0; k < todo.size() && k < 20000; ++k) {
    QPDFObjectHandle o = todo[k];
    if (o.isIndirect() && !seen.insert(o.getObjGen()).second) continue;
    if (o.isArray()) {
      for (auto &it : o.getArrayAsVector()) todo.push_back(it);
    } else if (o.isDictionary()) {
      QPDFObjectHandle ft = o.getKey("/FT");
      if (ft.isName() && ft.getName() == "/Sig" && o.getKey("/V").isDictionary()) return true;
      if (o.hasKey("/Kids")) todo.push_back(o.getKey("/Kids"));
    }
  }
  return false;
}

void PdfFile::changed(QPDFObjectHandle obj) {
  if (obj.isIndirect()) changed_.insert(obj.getObjGen());
}

QPDFObjectHandle PdfFile::add(QPDFObjectHandle direct) {
  QPDFObjectHandle h = qpdf_->makeIndirectObject(direct);
  changed_.insert(h.getObjGen());
  return h;
}

void PdfFile::adopt(QPDFObjectHandle obj) {
  std::vector<QPDFObjectHandle> todo{obj};
  while (!todo.empty()) {
    QPDFObjectHandle o = todo.back();
    todo.pop_back();
    if (o.isIndirect() && (o.getObjGen().getObj() <= base_max_id_ || !changed_.insert(o.getObjGen()).second))
      continue;
    if (o.isArray()) {
      for (auto x : o.getArrayAsVector()) todo.push_back(x);
    } else if (o.isDictionary()) {
      for (const auto &k : o.getKeys()) todo.push_back(o.getKey(k));
    } else if (o.isStream()) {
      QPDFObjectHandle d = o.getDict();
      for (const auto &k : d.getKeys()) todo.push_back(d.getKey(k));
    }
  }
}

QPDFObjectHandle PdfFile::add_stream(const std::string &data, QPDFObjectHandle dict, bool compress) {
  QPDFObjectHandle s = qpdf_->newStream();
  if (dict.isDictionary())
    for (const auto &key : dict.getKeys())
      if (key != "/Length" && key != "/Filter" && key != "/DecodeParms") s.getDict().replaceKey(key, dict.getKey(key));
  set_stream_data(s, data, compress);
  return s;
}

void PdfFile::set_stream_data(QPDFObjectHandle stream, const std::string &data, bool compress) {
  std::string z = compress ? flate(data) : std::string();
  if (compress && !z.empty())
    stream.replaceStreamData(z, QPDFObjectHandle::newName("/FlateDecode"), QPDFObjectHandle::newNull());
  else
    stream.replaceStreamData(data, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
  changed(stream);
}

bool PdfFile::can_append() const {
  return !created_ && !structural_ && !repaired_ && !qpdf_->isEncrypted() && base_startxref_ > 0;
}

// The incremental update: objects, cross-reference section, trailer.
bool PdfFile::append_update(std::string &out, std::string &error) {
  out = "\n";  // the original may end without a line break
  long long origin = base_size_;
  std::map<int, std::pair<long long, int>> xref;  // number -> (offset, generation)
  int max_id = 0;
  try {
    for (const QPDFObjGen &og : changed_) {
      QPDFObjectHandle obj = qpdf_->getObject(og);
      long long off = origin + (long long)out.size();
      out += format("%d %d obj\n", og.getObj(), og.getGen());
      if (obj.isStream()) {
        std::shared_ptr<Buffer> raw = obj.getRawStreamData();
        QPDFObjectHandle dict = obj.getDict().shallowCopy();
        dict.replaceKey("/Length", QPDFObjectHandle::newInteger((long long)raw->getSize()));
        out += dict.unparse();
        out += "\nstream\n";
        out.append(reinterpret_cast<const char *>(raw->getBuffer()), raw->getSize());
        out += "\nendstream";
      } else {
        out += obj.unparseResolved();
      }
      out += "\nendobj\n";
      xref[og.getObj()] = {off, og.getGen()};
      max_id = std::max(max_id, og.getObj());
    }
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  QPDFObjectHandle old = qpdf_->getTrailer();
  QPDFObjectHandle trailer = QPDFObjectHandle::newDictionary();
  for (const char *key : {"/Root", "/Info", "/ID"})
    if (old.hasKey(key)) trailer.replaceKey(key, old.getKey(key));
  QPDFObjectHandle size0 = old.getKey("/Size");
  int size = std::max(size0.isInteger() ? size0.getIntValueAsInt() : 0, max_id + 1);
  trailer.replaceKey("/Prev", QPDFObjectHandle::newInteger(base_startxref_));
  long long xref_at = origin + (long long)out.size();
  if (!base_xref_stream_) {
    out += "xref\n";
    for (auto it = xref.begin(); it != xref.end();) {
      auto run = it;
      int first = it->first, n = 0;
      while (it != xref.end() && it->first == first + n) {
        ++it;
        ++n;
      }
      out += format("%d %d\n", first, n);
      for (auto e = run; e != it; ++e) out += format("%010lld %05d n\r\n", e->second.first, e->second.second);
    }
    trailer.replaceKey("/Size", QPDFObjectHandle::newInteger(size));
    out += "trailer\n" + trailer.unparse() + "\n";
  } else {
    // A cross-reference stream, like the section it follows.
    int self = size;
    size += 1;
    xref[self] = {xref_at, 0};
    int width = xref_at >= 0xFFFFFFFFLL ? 8 : 4;
    std::string rows;
    QPDFObjectHandle index = QPDFObjectHandle::newArray();
    for (auto it = xref.begin(); it != xref.end();) {
      int first = it->first, n = 0;
      while (it != xref.end() && it->first == first + n) {
        rows += char(1);
        for (int k = width - 1; k >= 0; --k) rows += char((it->second.first >> (8 * k)) & 0xFF);
        rows += char((it->second.second >> 8) & 0xFF);
        rows += char(it->second.second & 0xFF);
        ++it;
        ++n;
      }
      index.appendItem(QPDFObjectHandle::newInteger(first));
      index.appendItem(QPDFObjectHandle::newInteger(n));
    }
    std::string z = flate(rows);
    trailer.replaceKey("/Type", QPDFObjectHandle::newName("/XRef"));
    trailer.replaceKey("/Size", QPDFObjectHandle::newInteger(size));
    trailer.replaceKey("/Index", index);
    QPDFObjectHandle w = QPDFObjectHandle::newArray();
    for (int v : {1, width, 2}) w.appendItem(QPDFObjectHandle::newInteger(v));
    trailer.replaceKey("/W", w);
    trailer.replaceKey("/Filter", QPDFObjectHandle::newName("/FlateDecode"));
    trailer.replaceKey("/Length", QPDFObjectHandle::newInteger((long long)z.size()));
    out += format("%d 0 obj\n", self) + trailer.unparse() + "\nstream\n" + z + "\nendstream\nendobj\n";
  }
  out += format("startxref\n%lld\n%%%%EOF\n", xref_at);
  return true;
}

bool PdfFile::rewrite(const std::string *out_path, std::string *out_mem, Write mode, std::string &error) {
  try {
    QPDFWriter w(*qpdf_);
    if (out_path) w.setOutputFilename(out_path->c_str());
    else w.setOutputMemory();
    if (mode == Write::Compact) {
      w.setObjectStreamMode(qpdf_o_generate);
      w.setCompressStreams(true);
      w.setDecodeLevel(qpdf_dl_generalized);
      w.setRecompressFlate(true);
    } else {
      // Stream data byte for byte as it is.
      w.setCompressStreams(false);
      w.setDecodeLevel(qpdf_dl_none);
      w.setObjectStreamMode(qpdf_o_preserve);
    }
    w.write();
    if (out_mem) {
      std::shared_ptr<Buffer> b = w.getBufferSharedPointer();
      out_mem->assign(reinterpret_cast<const char *>(b->getBuffer()), b->getSize());
    }
  } catch (std::exception &e) {
    error = e.what();
    return false;
  }
  return true;
}

bool PdfFile::write(const std::string &out, Write mode, std::string &error) {
  if (mode == Write::Auto && can_append()) {
    std::string tail;
    if (!append_update(tail, error)) return false;
    if (!copy_file(path_, out, error)) return false;
    std::ofstream o(out, std::ios::binary | std::ios::app);
    o.write(tail.data(), std::streamsize(tail.size()));
    if (!o) {
      error = strerror(errno);
      return false;
    }
    return true;
  }
  return rewrite(&out, nullptr, mode == Write::Auto ? Write::Rewrite : mode, error);
}

bool PdfFile::write_memory(std::string &out, Write mode, std::string &error) {
  if (mode == Write::Auto && can_append()) {
    std::string tail;
    if (!append_update(tail, error)) return false;
    if (!read_file(path_, out) || (long long)out.size() != base_size_) {
      error = _("the document changed on disk");
      return false;
    }
    out += tail;
    return true;
  }
  return rewrite(nullptr, &out, mode == Write::Auto ? Write::Rewrite : mode, error);
}

// --------------------------------------------------------- page content

std::string page_content(QPDFObjectHandle page) {
  std::string out;
  try {
    Pl_String sink("page content", nullptr, out);
    QPDFPageObjectHelper(page).pipeContents(&sink);
  } catch (std::exception &) {
  }
  return out;
}

void set_page_content(PdfFile &pdf, QPDFObjectHandle page, const std::string &data) {
  QPDFObjectHandle s = pdf.add_stream(data, QPDFObjectHandle::newNull());
  page.replaceKey("/Contents", s);
  pdf.changed(page);
}

QPDFObjectHandle page_resources(PdfFile &pdf, QPDFObjectHandle page) {
  QPDFObjectHandle res = page.getKey("/Resources");
  if (res.isDictionary() && !res.isIndirect()) return res;
  // Inherited from the page tree or an indirect (maybe shared) object: the
  // page gets a copy of its own; the objects inside stay shared.
  QPDFObjectHandle src = QPDFPageObjectHelper(page).getAttribute("/Resources", false);
  QPDFObjectHandle copy = src.isDictionary() ? src.shallowCopy() : QPDFObjectHandle::newDictionary();
  page.replaceKey("/Resources", copy);
  pdf.changed(page);
  return page.getKey("/Resources");
}

std::string add_resource(PdfFile &pdf, QPDFObjectHandle owner, QPDFObjectHandle resources, const std::string &category,
                         QPDFObjectHandle obj, const std::string &prefix) {
  QPDFObjectHandle cat = resources.getKey(category);
  if (cat.isDictionary() && cat.isIndirect()) {
    // A shared dictionary of fonts or images: copy it before adding.
    cat = cat.shallowCopy();
    resources.replaceKey(category, cat);
    cat = resources.getKey(category);
  } else if (!cat.isDictionary()) {
    resources.replaceKey(category, QPDFObjectHandle::newDictionary());
    cat = resources.getKey(category);
  }
  pdf.changed(owner);
  if (obj.isIndirect())
    for (const auto &key : cat.getKeys()) {
      QPDFObjectHandle v = cat.getKey(key);
      if (v.isIndirect() && v.getObjGen() == obj.getObjGen()) return key;
    }
  for (int n = 1;; ++n) {
    std::string name = "/" + prefix + std::to_string(n);
    if (!cat.hasKey(name)) {
      cat.replaceKey(name, obj);
      return name;
    }
  }
}

// --------------------------------------------------------- committing

std::string temp_beside(const std::string &target) {
  size_t slash = target.rfind('/');
  std::string dir = slash == std::string::npos ? "." : target.substr(0, slash);
  if (dir.empty()) dir = "/";
  std::string base = slash == std::string::npos ? target : target.substr(slash + 1);
  std::string tmpl = dir + "/." + base + ".xanara-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data());
  if (fd < 0) return "";
  ::close(fd);
  return buf.data();
}

bool commit_file(const std::string &tmp, const std::string &target, const std::string &password, int pages,
                 std::string &error) {
  {
    GError *err = nullptr;
    PopplerDocument *check =
        poppler_document_new_from_file(file_uri(tmp).c_str(), password.empty() ? nullptr : password.c_str(), &err);
    if (!check) {
      error = err ? err->message : _("the written file is not readable");
      if (err) g_error_free(err);
      unlink(tmp.c_str());
      return false;
    }
    int n = poppler_document_get_n_pages(check);
    g_object_unref(check);
    if (n <= 0 || (pages >= 0 && n != pages)) {
      error = format(_("the written file has %d pages instead of %d"), n, pages);
      unlink(tmp.c_str());
      return false;
    }
  }
  struct stat st;
  if (stat(target.c_str(), &st) == 0) chmod(tmp.c_str(), st.st_mode & 07777);
  else chmod(tmp.c_str(), 0644);
  int fd = ::open(tmp.c_str(), O_RDONLY);
  if (fd >= 0) {
    fsync(fd);
    ::close(fd);
  }
  if (rename(tmp.c_str(), target.c_str()) != 0) {
    error = strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace xanara
