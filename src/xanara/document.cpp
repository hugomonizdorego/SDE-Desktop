// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The PDF document model: opening, sharp rendering, page queries and
// annotation / form editing through poppler-glib.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core.h"

namespace xanara {

// ------------------------------------------------------------- geometry

Box Box::normalized() const {
  return Box{std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2)};
}

Box Box::united(const Box &o) const {
  if (empty()) return o;
  if (o.empty()) return *this;
  return Box{std::min(x1, o.x1), std::min(y1, o.y1), std::max(x2, o.x2), std::max(y2, o.y2)};
}

static int norm_rotation(int r) { return ((r % 360) + 360) % 360 / 90 * 90; }

void page_pixels(double pw, double ph, const RenderParams &rp, int &w, int &h) {
  int r = norm_rotation(rp.rotation);
  double a = (r == 90 || r == 270) ? ph : pw, b = (r == 90 || r == 270) ? pw : ph;
  // A tiny tolerance keeps 612 pt x 1.0 from turning into 613 pixels.
  w = std::max(1, int(std::ceil(a * rp.scale - 1e-6)));
  h = std::max(1, int(std::ceil(b * rp.scale - 1e-6)));
}

void page_to_device(double pw, double ph, const RenderParams &rp, double px, double py, double &dx,
                    double &dy) {
  double s = rp.scale;
  switch (norm_rotation(rp.rotation)) {
    case 90: dx = (ph - py) * s; dy = px * s; break;
    case 180: dx = (pw - px) * s; dy = (ph - py) * s; break;
    case 270: dx = py * s; dy = (pw - px) * s; break;
    default: dx = px * s; dy = py * s; break;
  }
}

void device_to_page(double pw, double ph, const RenderParams &rp, double dx, double dy, double &px,
                    double &py) {
  double s = rp.scale > 0 ? rp.scale : 1;
  double x = dx / s, y = dy / s;
  switch (norm_rotation(rp.rotation)) {
    case 90: px = y; py = ph - x; break;
    case 180: px = pw - x; py = ph - y; break;
    case 270: px = pw - y; py = x; break;
    default: px = x; py = y; break;
  }
}

// ------------------------------------------------------------ rendering

cairo_surface_t *render_area(PopplerPage *page, double pw, double ph, const RenderParams &rp, int tx,
                             int ty, int tw, int th) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, std::max(1, tw), std::max(1, th));
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);
  if (page) {
    cairo_translate(cr, -tx, -ty);
    double s2 = rp.scale;
    switch (norm_rotation(rp.rotation)) {
      case 90: cairo_translate(cr, ph * s2, 0); cairo_rotate(cr, M_PI / 2); break;
      case 180: cairo_translate(cr, pw * s2, ph * s2); cairo_rotate(cr, M_PI); break;
      case 270: cairo_translate(cr, 0, pw * s2); cairo_rotate(cr, 3 * M_PI / 2); break;
      default: break;
    }
    cairo_scale(cr, s2, s2);
    // Text tuned for screens: grayscale coverage (no colour fringes when the
    // page is rotated, inverted for night mode or captured in screenshots)
    // and unhinted metrics so that glyph positions match the PDF exactly.
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    cairo_set_font_options(cr, fo);
    cairo_font_options_destroy(fo);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
    poppler_page_render(page, cr);
  }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  apply_color_mode(s, rp.mode);
  return s;
}

void apply_color_mode(cairo_surface_t *surface, ColorMode mode) {
  if (mode == ColorMode::Normal || !surface) return;
  cairo_surface_flush(surface);
  unsigned char *data = cairo_image_surface_get_data(surface);
  if (!data) return;
  int w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
  int stride = cairo_image_surface_get_stride(surface);
  for (int y = 0; y < h; ++y) {
    uint32_t *row = reinterpret_cast<uint32_t *>(data + size_t(y) * size_t(stride));
    for (int x = 0; x < w; ++x) {
      uint32_t p = row[x];
      int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
      if (mode == ColorMode::Night) {
        // Flip lightness but keep the hue (invert + 180 degree hue turn),
        // then compress the range: paper #1D2026, ink #DCDFE4.
        int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
        int k = 255 - mx - mn;
        r += k; g += k; b += k;
        r = 0x1D + (r * (0xDC - 0x1D)) / 255;
        g = 0x20 + (g * (0xDF - 0x20)) / 255;
        b = 0x26 + (b * (0xE4 - 0x26)) / 255;
      } else {
        // Warm paper #F4ECD8 and brown-black ink #3A2F26.
        r = 0x3A + (r * (0xF4 - 0x3A)) / 255;
        g = 0x2F + (g * (0xEC - 0x2F)) / 255;
        b = 0x26 + (b * (0xD8 - 0x26)) / 255;
      }
      r = std::max(0, std::min(255, r));
      g = std::max(0, std::min(255, g));
      b = std::max(0, std::min(255, b));
      row[x] = 0xff000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
    }
  }
  cairo_surface_mark_dirty(surface);
}

// ------------------------------------------------------------- features

bool native_free_text() {
#ifdef XANARA_HAVE_FREE_TEXT
  return true;
#else
  return false;
#endif
}

bool native_ink() {
#ifdef XANARA_HAVE_INK
  return true;
#else
  return false;
#endif
}

// ------------------------------------------------------------- document

Document::~Document() {
  for (size_t i = 0; i < info_.size(); ++i) clear_page_cache(int(i));
  for (auto *p : pages_)
    if (p) g_object_unref(p);
  if (doc_) g_object_unref(doc_);
}

std::unique_ptr<Document> Document::open(const std::string &path, const std::string &password,
                                         std::string &error, bool &encrypted) {
  encrypted = false;
  error.clear();
  std::string uri = file_uri(path);
  GError *err = nullptr;
  PopplerDocument *doc =
      poppler_document_new_from_file(uri.c_str(), password.empty() ? nullptr : password.c_str(), &err);
  if (!doc) {
    if (err && err->domain == POPPLER_ERROR && err->code == POPPLER_ERROR_ENCRYPTED) encrypted = true;
    error = err ? err->message : "unknown error";
    if (err) g_error_free(err);
    return nullptr;
  }
  std::unique_ptr<Document> d(new Document());
  d->doc_ = doc;
  d->path_ = path;
  d->password_ = password;
  int n = poppler_document_get_n_pages(doc);
  d->pages_.assign(size_t(std::max(0, n)), nullptr);
  d->sizes_.resize(size_t(std::max(0, n)));
  d->revisions_.assign(size_t(std::max(0, n)), 1);
  d->info_.resize(size_t(std::max(0, n)));
  for (int i = 0; i < n; ++i) {
    PopplerPage *p = poppler_document_get_page(doc, i);
    d->pages_[size_t(i)] = p;
    double w = 612, h = 792;
    if (p) poppler_page_get_size(p, &w, &h);
    d->sizes_[size_t(i)] = Point{w > 0 ? w : 612, h > 0 ? h : 792};
  }
  if (n <= 0) {
    error = "the document has no pages";
    return nullptr;
  }
  return d;
}

std::string Document::title() const {
  std::lock_guard<std::mutex> lock(mu_);
  gchar *t = poppler_document_get_title(doc_);
  std::string s = t ? t : "";
  g_free(t);
  return s;
}

std::string Document::page_label(int i) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (i < 0 || i >= count() || !pages_[size_t(i)]) return std::to_string(i + 1);
  gchar *l = poppler_page_get_label(pages_[size_t(i)]);
  std::string s = l ? l : "";
  g_free(l);
  return s.empty() ? std::to_string(i + 1) : s;
}

PopplerPage *Document::page(int i) {
  if (i < 0 || i >= count()) return nullptr;
  return pages_[size_t(i)];
}

cairo_surface_t *Document::render(int i, const RenderParams &rp, int tx, int ty, int tw, int th) {
  std::lock_guard<std::mutex> lock(mu_);
  if (i < 0 || i >= count()) return nullptr;
  return render_area(pages_[size_t(i)], width(i), height(i), rp, tx, ty, tw, th);
}

uint64_t Document::revision(int page) const {
  if (page < 0 || page >= count()) return 0;
  return revisions_[size_t(page)];
}

void Document::touch(int page) {
  if (page < 0 || page >= count()) return;
  ++revisions_[size_t(page)];
  clear_page_cache(page);
}

void Document::clear_page_cache(int page) {
  auto &pi = info_[size_t(page)];
  for (auto &a : pi.annots)
    if (a.annot) g_object_unref(a.annot);
  for (auto &f : pi.fields)
    if (f.field) g_object_unref(f.field);
  pi = PageInfo{};
}

PopplerRectangle Document::to_pdf(int page, const Box &b0) const {
  Box b = b0.normalized();
  double ph = height(page);
  PopplerRectangle r;
  r.x1 = b.x1;
  r.y1 = ph - b.y2;
  r.x2 = b.x2;
  r.y2 = ph - b.y1;
  return r;
}

Box Document::from_pdf(int page, const PopplerRectangle &r) const {
  double ph = height(page);
  return Box{r.x1, ph - r.y2, r.x2, ph - r.y1}.normalized();
}

static bool write_all_sync(const std::string &path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  bool ok = fsync(fd) == 0;
  ::close(fd);
  return ok;
}

bool Document::save_to(const std::string &target, std::string &error) {
  std::string dir = target.substr(0, target.rfind('/'));
  if (dir.empty()) dir = "/";
  std::string base = target.substr(target.rfind('/') + 1);
  std::string tmpl = dir + "/." + base + ".xanara-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data());
  if (fd < 0) {
    error = strerror(errno);
    return false;
  }
  ::close(fd);
  std::string tmp(buf.data());
  GError *err = nullptr;
  bool ok;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ok = poppler_document_save(doc_, file_uri(tmp).c_str(), &err);
  }
  if (!ok) {
    error = err ? err->message : "could not write the document";
    if (err) g_error_free(err);
    unlink(tmp.c_str());
    return false;
  }
  struct stat st;
  if (stat(target.c_str(), &st) == 0) chmod(tmp.c_str(), st.st_mode & 07777);
  else chmod(tmp.c_str(), 0644);
  // A quick sanity check: the new file must open again before it replaces
  // anything.
  {
    GError *e2 = nullptr;
    PopplerDocument *check = poppler_document_new_from_file(
        file_uri(tmp).c_str(), password_.empty() ? nullptr : password_.c_str(), &e2);
    if (!check) {
      error = e2 ? e2->message : "the written file is not readable";
      if (e2) g_error_free(e2);
      unlink(tmp.c_str());
      return false;
    }
    g_object_unref(check);
  }
  write_all_sync(tmp);
  if (rename(tmp.c_str(), target.c_str()) != 0) {
    error = strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------- page queries

LinkTarget Document::resolve(PopplerAction *action) {
  LinkTarget t;
  if (!action) return t;
  switch (action->type) {
    case POPPLER_ACTION_GOTO_DEST: {
      PopplerDest *dest = action->goto_dest.dest;
      PopplerDest *found = nullptr;
      if (dest && dest->type == POPPLER_DEST_NAMED && dest->named_dest) {
        found = poppler_document_find_dest(doc_, dest->named_dest);
        dest = found;
      }
      if (dest && dest->page_num > 0) {
        t.kind = LinkTarget::Page;
        t.page = dest->page_num - 1;
        if (t.page >= count()) t.page = count() - 1;
        if ((dest->type == POPPLER_DEST_XYZ || dest->type == POPPLER_DEST_FITH ||
             dest->type == POPPLER_DEST_FITBH) &&
            dest->change_top)
          t.top = std::max(0.0, height(t.page) - dest->top);
      }
      if (found) poppler_dest_free(found);
      break;
    }
    case POPPLER_ACTION_URI:
      if (action->uri.uri) {
        t.kind = LinkTarget::Uri;
        t.text = action->uri.uri;
      }
      break;
    case POPPLER_ACTION_GOTO_REMOTE:
      if (action->goto_remote.file_name) {
        t.kind = LinkTarget::File;
        t.text = action->goto_remote.file_name;
      }
      break;
    case POPPLER_ACTION_LAUNCH:
      if (action->launch.file_name) {
        t.kind = LinkTarget::File;
        t.text = action->launch.file_name;
      }
      break;
    case POPPLER_ACTION_NAMED:
      if (action->named.named_dest) {
        t.kind = LinkTarget::Named;
        t.text = action->named.named_dest;
      }
      break;
    default: break;
  }
  return t;
}

const std::vector<Link> &Document::links(int page) {
  static const std::vector<Link> none;
  if (page < 0 || page >= count()) return none;
  auto &pi = info_[size_t(page)];
  if (pi.links_ok) return pi.links;
  std::lock_guard<std::mutex> lock(mu_);
  pi.links_ok = true;
  if (!pages_[size_t(page)]) return pi.links;
  GList *list = poppler_page_get_link_mapping(pages_[size_t(page)]);
  for (GList *l = list; l; l = l->next) {
    auto *m = static_cast<PopplerLinkMapping *>(l->data);
    Link link;
    link.box = from_pdf(page, m->area);
    link.target = resolve(m->action);
    if (link.target.kind != LinkTarget::Nothing) pi.links.push_back(link);
  }
  poppler_page_free_link_mapping(list);
  return pi.links;
}

const std::vector<Annotation> &Document::annotations(int page) {
  static const std::vector<Annotation> none;
  if (page < 0 || page >= count()) return none;
  auto &pi = info_[size_t(page)];
  if (pi.annots_ok) return pi.annots;
  std::lock_guard<std::mutex> lock(mu_);
  pi.annots_ok = true;
  if (!pages_[size_t(page)]) return pi.annots;
  GList *list = poppler_page_get_annot_mapping(pages_[size_t(page)]);
  for (GList *l = list; l; l = l->next) {
    auto *m = static_cast<PopplerAnnotMapping *>(l->data);
    PopplerAnnotType type = poppler_annot_get_annot_type(m->annot);
    if (type == POPPLER_ANNOT_LINK || type == POPPLER_ANNOT_WIDGET || type == POPPLER_ANNOT_POPUP)
      continue;
    Annotation a;
    a.annot = POPPLER_ANNOT(g_object_ref(m->annot));
    a.type = type;
    a.box = from_pdf(page, m->area);
    if (PopplerColor *c = poppler_annot_get_color(m->annot)) {
      a.color = Rgb{c->red / 65535.0, c->green / 65535.0, c->blue / 65535.0};
      a.has_color = true;
      g_free(c);
    }
    if (gchar *t = poppler_annot_get_contents(m->annot)) {
      a.contents = t;
      g_free(t);
    }
    pi.annots.push_back(a);
  }
  poppler_page_free_annot_mapping(list);
  return pi.annots;
}

const std::vector<FormField> &Document::fields(int page) {
  static const std::vector<FormField> none;
  if (page < 0 || page >= count()) return none;
  auto &pi = info_[size_t(page)];
  if (pi.fields_ok) return pi.fields;
  std::lock_guard<std::mutex> lock(mu_);
  pi.fields_ok = true;
  if (!pages_[size_t(page)]) return pi.fields;
  GList *list = poppler_page_get_form_field_mapping(pages_[size_t(page)]);
  for (GList *l = list; l; l = l->next) {
    auto *m = static_cast<PopplerFormFieldMapping *>(l->data);
    FormField f;
    f.field = POPPLER_FORM_FIELD(g_object_ref(m->field));
    f.type = poppler_form_field_get_field_type(m->field);
    f.box = from_pdf(page, m->area);
    f.id = poppler_form_field_get_id(m->field);
    f.read_only = poppler_form_field_is_read_only(m->field);
    pi.fields.push_back(f);
  }
  poppler_page_free_form_field_mapping(list);
  return pi.fields;
}

std::vector<Box> Document::find(int page, const std::string &text, bool match_case, bool whole_words) {
  std::vector<Box> out;
  if (page < 0 || page >= count() || text.empty()) return out;
  std::lock_guard<std::mutex> lock(mu_);
  if (!pages_[size_t(page)]) return out;
  unsigned flags = POPPLER_FIND_DEFAULT;
  if (match_case) flags |= POPPLER_FIND_CASE_SENSITIVE;
  if (whole_words) flags |= POPPLER_FIND_WHOLE_WORDS_ONLY;
#if POPPLER_CHECK_VERSION(0, 85, 0)
  flags |= POPPLER_FIND_MULTILINE;
#endif
  GList *list = poppler_page_find_text_with_options(pages_[size_t(page)], text.c_str(), PopplerFindFlags(flags));
  for (GList *l = list; l; l = l->next) {
    auto *r = static_cast<PopplerRectangle *>(l->data);
    out.push_back(from_pdf(page, *r));
    poppler_rectangle_free(r);
  }
  g_list_free(list);
  std::sort(out.begin(), out.end(), [](const Box &a, const Box &b) {
    if (std::fabs(a.y1 - b.y1) > 2) return a.y1 < b.y1;
    return a.x1 < b.x1;
  });
  return out;
}

std::vector<Box> Document::selection_boxes(int page, const Box &sel, PopplerSelectionStyle style) {
  std::vector<Box> out;
  if (page < 0 || page >= count()) return out;
  std::lock_guard<std::mutex> lock(mu_);
  if (!pages_[size_t(page)]) return out;
  PopplerRectangle r{sel.x1, sel.y1, sel.x2, sel.y2};
  const double k = 8.0;  // region precision: 1/8 point
  cairo_region_t *region = poppler_page_get_selected_region(pages_[size_t(page)], k, style, &r);
  if (!region) return out;
  int n = cairo_region_num_rectangles(region);
  for (int i = 0; i < n; ++i) {
    cairo_rectangle_int_t rc;
    cairo_region_get_rectangle(region, i, &rc);
    out.push_back(Box{rc.x / k, rc.y / k, (rc.x + rc.width) / k, (rc.y + rc.height) / k});
  }
  cairo_region_destroy(region);
  // Merge the per-glyph slivers of one line into a single box.
  std::sort(out.begin(), out.end(), [](const Box &a, const Box &b) {
    if (std::fabs(a.y1 - b.y1) > 0.5) return a.y1 < b.y1;
    return a.x1 < b.x1;
  });
  std::vector<Box> merged;
  for (const auto &b : out) {
    if (!merged.empty()) {
      Box &m = merged.back();
      bool same_line = std::fabs(m.y1 - b.y1) < 0.6 && std::fabs(m.y2 - b.y2) < 0.6;
      if (same_line && b.x1 <= m.x2 + 0.6) {
        m.x2 = std::max(m.x2, b.x2);
        continue;
      }
    }
    merged.push_back(b);
  }
  return merged;
}

std::string Document::selected_text(int page, const Box &sel, PopplerSelectionStyle style) {
  if (page < 0 || page >= count()) return "";
  std::lock_guard<std::mutex> lock(mu_);
  if (!pages_[size_t(page)]) return "";
  PopplerRectangle r{sel.x1, sel.y1, sel.x2, sel.y2};
  gchar *t = poppler_page_get_selected_text(pages_[size_t(page)], style, &r);
  std::string s = t ? t : "";
  g_free(t);
  return s;
}

std::string Document::text(int page) {
  if (page < 0 || page >= count()) return "";
  std::lock_guard<std::mutex> lock(mu_);
  if (!pages_[size_t(page)]) return "";
  gchar *t = poppler_page_get_text(pages_[size_t(page)]);
  std::string s = t ? t : "";
  g_free(t);
  return s;
}

static void walk_outline(Document *d, PopplerIndexIter *iter, int level, std::vector<OutlineItem> &out) {
  do {
    PopplerAction *action = poppler_index_iter_get_action(iter);
    OutlineItem item;
    item.level = level;
    if (action) {
      if (action->any.title) item.title = action->any.title;
      item.target = d->resolve(action);
      poppler_action_free(action);
    }
    item.expanded = poppler_index_iter_is_open(iter);
    if (PopplerIndexIter *child = poppler_index_iter_get_child(iter)) {
      walk_outline(d, child, level + 1, item.children);
      poppler_index_iter_free(child);
    }
    out.push_back(std::move(item));
  } while (poppler_index_iter_next(iter));
}

std::vector<OutlineItem> Document::outline() {
  std::vector<OutlineItem> out;
  std::lock_guard<std::mutex> lock(mu_);
  PopplerIndexIter *iter = poppler_index_iter_new(doc_);
  if (!iter) return out;
  walk_outline(this, iter, 0, out);
  poppler_index_iter_free(iter);
  return out;
}

// --------------------------------------------------------------- editing

static PopplerColor to_color(Rgb c) {
  PopplerColor p;
  p.red = guint16(std::lround(std::clamp(c.r, 0.0, 1.0) * 65535));
  p.green = guint16(std::lround(std::clamp(c.g, 0.0, 1.0) * 65535));
  p.blue = guint16(std::lround(std::clamp(c.b, 0.0, 1.0) * 65535));
  return p;
}

static std::string author_name() {
  const char *real = g_get_real_name();
  if (real && *real && strcmp(real, "Unknown") != 0) return real;
  const char *user = g_get_user_name();
  return user ? user : "";
}

PopplerAnnot *Document::finish_annot(int page, PopplerAnnot *annot, Rgb color) {
  if (!annot) return nullptr;
  PopplerColor c = to_color(color);
  poppler_annot_set_color(annot, &c);
  poppler_annot_set_flags(annot, POPPLER_ANNOT_FLAG_PRINT);
  if (POPPLER_IS_ANNOT_MARKUP(annot)) {
    std::string who = author_name();
    if (!who.empty()) poppler_annot_markup_set_label(POPPLER_ANNOT_MARKUP(annot), who.c_str());
  }
  poppler_page_add_annot(pages_[size_t(page)], annot);
  modified_ = true;
  return annot;
}

PopplerAnnot *Document::add_markup(int page, MarkupKind kind, const std::vector<Box> &boxes, Rgb color) {
  if (page < 0 || page >= count() || boxes.empty()) return nullptr;
  PopplerAnnot *annot = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    double ph = height(page);
    GArray *quads = g_array_sized_new(FALSE, FALSE, sizeof(PopplerQuadrilateral), guint(boxes.size()));
    Box all;
    for (const auto &b0 : boxes) {
      Box b = b0.normalized();
      all = all.united(b);
      PopplerQuadrilateral q;
      q.p1.x = b.x1; q.p1.y = ph - b.y1;
      q.p2.x = b.x2; q.p2.y = ph - b.y1;
      q.p3.x = b.x1; q.p3.y = ph - b.y2;
      q.p4.x = b.x2; q.p4.y = ph - b.y2;
      g_array_append_val(quads, q);
    }
    PopplerRectangle rect = to_pdf(page, all);
    switch (kind) {
      case MarkupKind::Highlight: annot = poppler_annot_text_markup_new_highlight(doc_, &rect, quads); break;
      case MarkupKind::Underline: annot = poppler_annot_text_markup_new_underline(doc_, &rect, quads); break;
      case MarkupKind::StrikeOut: annot = poppler_annot_text_markup_new_strikeout(doc_, &rect, quads); break;
      case MarkupKind::Squiggly: annot = poppler_annot_text_markup_new_squiggly(doc_, &rect, quads); break;
    }
    g_array_unref(quads);
    finish_annot(page, annot, color);
  }
  touch(page);
  return annot;
}

PopplerAnnot *Document::add_note(int page, double x, double y, const std::string &text, Rgb color) {
  if (page < 0 || page >= count()) return nullptr;
  PopplerAnnot *annot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    PopplerRectangle rect = to_pdf(page, Box{x, y, x + 20, y + 20});
    annot = poppler_annot_text_new(doc_, &rect);
    poppler_annot_set_contents(annot, text.c_str());
    poppler_annot_text_set_icon(POPPLER_ANNOT_TEXT(annot), POPPLER_ANNOT_TEXT_ICON_NOTE);
    finish_annot(page, annot, color);
  }
  touch(page);
  return annot;
}

#ifndef XANARA_HAVE_FREE_TEXT
// Paints wrapped text for a free text stamp (used when poppler cannot make
// a native FreeText annotation).
static cairo_surface_t *text_image(const Box &box, const std::string &text, Rgb color, double font_size) {
  const double k = 4.0;  // 288 dpi: crisp at every reasonable zoom
  int w = std::max(1, int(std::ceil(box.w() * k))), h = std::max(1, int(std::ceil(box.h() * k)));
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, k, k);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, font_size);
  cairo_set_source_rgb(cr, color.r, color.g, color.b);
  cairo_font_extents_t fe;
  cairo_font_extents(cr, &fe);
  double pad = 2, y = pad + fe.ascent, maxw = box.w() - 2 * pad;
  auto emit = [&](const std::string &line) {
    cairo_move_to(cr, pad, y);
    cairo_show_text(cr, line.c_str());
    y += fe.height;
  };
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    std::string line;
    size_t i = 0;
    while (i < para.size()) {
      size_t sp = para.find(' ', i);
      std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i + 1);
      std::string trial = line + word;
      cairo_text_extents_t te;
      cairo_text_extents(cr, trial.c_str(), &te);
      if (te.x_advance > maxw && !line.empty()) {
        emit(line);
        line = word;
      } else {
        line = trial;
      }
      if (sp == std::string::npos) break;
      i = sp + 1;
    }
    emit(line);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}
#endif

PopplerAnnot *Document::add_free_text(int page, const Box &box0, const std::string &text, Rgb color,
                                      double font_size) {
  if (page < 0 || page >= count() || text.empty()) return nullptr;
  Box box = box0.normalized();
#ifdef XANARA_HAVE_FREE_TEXT
  PopplerAnnot *annot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    PopplerRectangle rect = to_pdf(page, box);
    annot = poppler_annot_free_text_new(doc_, &rect);
    poppler_annot_set_contents(annot, text.c_str());
    PopplerFontDescription *fd = poppler_font_description_new("Sans");
    fd->size_pt = font_size;
    poppler_annot_free_text_set_font_desc(POPPLER_ANNOT_FREE_TEXT(annot), fd);
    poppler_font_description_free(fd);
    PopplerColor fc = to_color(color);
    poppler_annot_free_text_set_font_color(POPPLER_ANNOT_FREE_TEXT(annot), &fc);
    poppler_annot_set_flags(annot, POPPLER_ANNOT_FLAG_PRINT);
    poppler_page_add_annot(pages_[size_t(page)], annot);
    modified_ = true;
  }
  touch(page);
  return annot;
#else
  cairo_surface_t *img = text_image(box, text, color, font_size);
  PopplerAnnot *annot = add_image(page, box, img, text);
  cairo_surface_destroy(img);
  return annot;
#endif
}

#ifndef XANARA_HAVE_INK
static cairo_surface_t *ink_image(const Box &box, const std::vector<std::vector<Point>> &strokes, Rgb color,
                                  double width) {
  const double k = 4.0;
  int w = std::max(1, int(std::ceil(box.w() * k))), h = std::max(1, int(std::ceil(box.h() * k)));
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, k, k);
  cairo_translate(cr, -box.x1, -box.y1);
  cairo_set_source_rgb(cr, color.r, color.g, color.b);
  cairo_set_line_width(cr, width);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  for (const auto &st : strokes) {
    if (st.empty()) continue;
    cairo_move_to(cr, st[0].x, st[0].y);
    if (st.size() == 1) cairo_line_to(cr, st[0].x + 0.01, st[0].y);
    for (size_t i = 1; i < st.size(); ++i) cairo_line_to(cr, st[i].x, st[i].y);
    cairo_stroke(cr);
  }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}
#endif

PopplerAnnot *Document::add_ink(int page, const std::vector<std::vector<Point>> &strokes, Rgb color,
                                double width) {
  if (page < 0 || page >= count() || strokes.empty()) return nullptr;
  Box box;
  bool first = true;
  for (const auto &st : strokes)
    for (const auto &p : st) {
      if (first) box = Box{p.x, p.y, p.x, p.y};
      first = false;
      box.x1 = std::min(box.x1, p.x); box.y1 = std::min(box.y1, p.y);
      box.x2 = std::max(box.x2, p.x); box.y2 = std::max(box.y2, p.y);
    }
  if (first) return nullptr;
  double pad = width / 2 + 1;
  box = Box{box.x1 - pad, box.y1 - pad, box.x2 + pad, box.y2 + pad};
#ifdef XANARA_HAVE_INK
  PopplerAnnot *annot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    double ph = height(page);
    PopplerRectangle rect = to_pdf(page, box);
    annot = poppler_annot_ink_new(doc_, &rect);
    std::vector<PopplerPath *> paths;
    for (const auto &st : strokes) {
      std::vector<PopplerPoint> pts;
      for (const auto &p : st) pts.push_back(PopplerPoint{p.x, ph - p.y});
      if (pts.size() == 1) pts.push_back(PopplerPoint{pts[0].x + 0.01, pts[0].y});
      paths.push_back(poppler_path_new_from_array(pts.data(), pts.size()));
    }
    poppler_annot_ink_set_ink_list(POPPLER_ANNOT_INK(annot), paths.data(), paths.size());
    for (auto *p : paths) poppler_path_free(p);
#ifdef XANARA_HAVE_BORDER_WIDTH
    poppler_annot_set_border_width(annot, width);
#endif
    finish_annot(page, annot, color);
    geometry_.push_back(Geometry{page, rect, strokes});
  }
  touch(page);
  return annot;
#else
  cairo_surface_t *img = ink_image(box, strokes, color, width);
  PopplerAnnot *annot = add_image(page, box, img, "");
  cairo_surface_destroy(img);
  return annot;
#endif
}

PopplerAnnot *Document::add_square(int page, const Box &box, Rgb color, double width) {
  if (page < 0 || page >= count()) return nullptr;
  PopplerAnnot *annot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    PopplerRectangle rect = to_pdf(page, box);
    annot = poppler_annot_square_new(doc_, &rect);
#ifdef XANARA_HAVE_BORDER_WIDTH
    poppler_annot_set_border_width(annot, width);
#endif
    finish_annot(page, annot, color);
  }
  touch(page);
  return annot;
}

PopplerAnnot *Document::add_line(int page, Point a, Point b, Rgb color, double width) {
  if (page < 0 || page >= count()) return nullptr;
  PopplerAnnot *annot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    double ph = height(page), pad = width + 2;
    Box box{std::min(a.x, b.x) - pad, std::min(a.y, b.y) - pad, std::max(a.x, b.x) + pad,
            std::max(a.y, b.y) + pad};
    PopplerRectangle rect = to_pdf(page, box);
    PopplerPoint start{a.x, ph - a.y}, end{b.x, ph - b.y};
    annot = poppler_annot_line_new(doc_, &rect, &start, &end);
#ifdef XANARA_HAVE_BORDER_WIDTH
    poppler_annot_set_border_width(annot, width);
#endif
    finish_annot(page, annot, color);
    geometry_.push_back(Geometry{page, rect, {{a, b}}});
  }
  touch(page);
  return annot;
}

PopplerAnnot *Document::add_image(int page, const Box &box, cairo_surface_t *image, const std::string &label) {
  if (page < 0 || page >= count() || !image) return nullptr;
#if POPPLER_CHECK_VERSION(22, 7, 0)
  PopplerAnnot *annot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    PopplerRectangle rect = to_pdf(page, box);
    annot = poppler_annot_stamp_new(doc_, &rect);
    GError *err = nullptr;
    if (!poppler_annot_stamp_set_custom_image(POPPLER_ANNOT_STAMP(annot), image, &err)) {
      if (err) g_error_free(err);
      g_object_unref(annot);
      return nullptr;
    }
    if (!label.empty()) poppler_annot_set_contents(annot, label.c_str());
    poppler_annot_set_flags(annot, POPPLER_ANNOT_FLAG_PRINT);
    poppler_page_add_annot(pages_[size_t(page)], annot);
    modified_ = true;
  }
  touch(page);
  return annot;
#else
  (void)box;
  (void)label;
  return nullptr;
#endif
}

bool Document::remove_annot(int page, PopplerAnnot *annot) {
  if (page < 0 || page >= count() || !annot) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    poppler_page_remove_annot(pages_[size_t(page)], annot);
    modified_ = true;
  }
  touch(page);
  return true;
}

bool Document::restore_annot(int page, PopplerAnnot *annot) {
  if (page < 0 || page >= count() || !annot) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    poppler_page_add_annot(pages_[size_t(page)], annot);
    modified_ = true;
  }
  touch(page);
  return true;
}

bool Document::move_annot(int page, PopplerAnnot *annot, double dx, double dy) {
  if (page < 0 || page >= count() || !annot) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    PopplerRectangle r;
    poppler_annot_get_rectangle(annot, &r);
    PopplerAnnotType type = poppler_annot_get_annot_type(annot);
    if (POPPLER_IS_ANNOT_TEXT_MARKUP(annot)) {
      GArray *q = poppler_annot_text_markup_get_quadrilaterals(POPPLER_ANNOT_TEXT_MARKUP(annot));
      if (q) {
        for (guint i = 0; i < q->len; ++i) {
          auto &quad = g_array_index(q, PopplerQuadrilateral, i);
          PopplerPoint *pts[] = {&quad.p1, &quad.p2, &quad.p3, &quad.p4};
          for (auto *p : pts) {
            p->x += dx;
            p->y -= dy;
          }
        }
        poppler_annot_text_markup_set_quadrilaterals(POPPLER_ANNOT_TEXT_MARKUP(annot), q);
        g_array_unref(q);
      }
    }
    PopplerRectangle old = r;
    r.x1 += dx; r.x2 += dx;
    r.y1 -= dy; r.y2 -= dy;
    poppler_annot_set_rectangle(annot, &r);
    double ph = height(page);
    for (auto &g : geometry_) {
      if (g.page != page || std::fabs(g.rect.x1 - old.x1) > 0.01 || std::fabs(g.rect.y1 - old.y1) > 0.01 ||
          std::fabs(g.rect.x2 - old.x2) > 0.01 || std::fabs(g.rect.y2 - old.y2) > 0.01)
        continue;
      g.rect = r;
      for (auto &st : g.strokes)
        for (auto &p : st) {
          p.x += dx;
          p.y += dy;
        }
      if (type == POPPLER_ANNOT_LINE && g.strokes.size() == 1 && g.strokes[0].size() == 2) {
        PopplerPoint a{g.strokes[0][0].x, ph - g.strokes[0][0].y}, b{g.strokes[0][1].x, ph - g.strokes[0][1].y};
        poppler_annot_line_set_vertices(POPPLER_ANNOT_LINE(annot), &a, &b);
      }
#ifdef XANARA_HAVE_INK
      if (type == POPPLER_ANNOT_INK) {
        std::vector<PopplerPath *> paths;
        for (const auto &st : g.strokes) {
          std::vector<PopplerPoint> pts;
          for (const auto &p : st) pts.push_back(PopplerPoint{p.x, ph - p.y});
          paths.push_back(poppler_path_new_from_array(pts.data(), pts.size()));
        }
        poppler_annot_ink_set_ink_list(POPPLER_ANNOT_INK(annot), paths.data(), paths.size());
        for (auto *p : paths) poppler_path_free(p);
      }
#endif
      break;
    }
    modified_ = true;
  }
  touch(page);
  return true;
}

bool Document::set_annot_color(int page, PopplerAnnot *annot, Rgb color) {
  if (page < 0 || page >= count() || !annot) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    PopplerColor c = to_color(color);
    poppler_annot_set_color(annot, &c);
    modified_ = true;
  }
  touch(page);
  return true;
}

bool Document::set_annot_contents(int page, PopplerAnnot *annot, const std::string &text) {
  if (page < 0 || page >= count() || !annot) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    poppler_annot_set_contents(annot, text.c_str());
    modified_ = true;
  }
  touch(page);
  return true;
}

bool Document::set_field_text(int page, PopplerFormField *f, const std::string &text) {
  if (!f) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (poppler_form_field_get_field_type(f) == POPPLER_FORM_FIELD_TEXT) {
      gchar *old = poppler_form_field_text_get_text(f);
      bool same = (old ? std::string(old) : std::string()) == text;
      g_free(old);
      if (same) return false;
      poppler_form_field_text_set_text(f, text.c_str());
    } else if (poppler_form_field_get_field_type(f) == POPPLER_FORM_FIELD_CHOICE) {
      poppler_form_field_choice_set_text(f, text.c_str());
    } else {
      return false;
    }
    modified_ = true;
  }
  touch(page);
  return true;
}

bool Document::toggle_field(int page, PopplerFormField *f) {
  if (!f) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (poppler_form_field_get_field_type(f) != POPPLER_FORM_FIELD_BUTTON) return false;
    PopplerFormButtonType bt = poppler_form_field_button_get_button_type(f);
    if (bt == POPPLER_FORM_BUTTON_PUSH) return false;
    gboolean state = poppler_form_field_button_get_state(f);
    if (bt == POPPLER_FORM_BUTTON_RADIO && state) return false;  // radios turn on only
    poppler_form_field_button_set_state(f, !state);
    modified_ = true;
  }
  // Radio buttons switch other widgets, possibly on other pages.
  for (int i = 0; i < count(); ++i)
    if (i == page || info_[size_t(i)].fields_ok) touch(i);
  return true;
}

bool Document::set_field_choice(int page, PopplerFormField *f, int item) {
  if (!f) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (poppler_form_field_get_field_type(f) != POPPLER_FORM_FIELD_CHOICE) return false;
    poppler_form_field_choice_unselect_all(f);
    if (item >= 0 && item < poppler_form_field_choice_get_n_items(f))
      poppler_form_field_choice_select_item(f, item);
    modified_ = true;
  }
  touch(page);
  return true;
}

// --------------------------------------------------------------- helpers

std::string file_uri(const std::string &path) {
  gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  std::string s = uri ? uri : "file://" + path;
  g_free(uri);
  return s;
}

std::string path_from_argument(const std::string &arg) {
  std::string p = arg;
  if (p.rfind("file://", 0) == 0) {
    gchar *f = g_filename_from_uri(p.c_str(), nullptr, nullptr);
    if (f) {
      p = f;
      g_free(f);
    }
  }
  if (!p.empty() && p[0] != '/') {
    char *cwd = getcwd(nullptr, 0);
    if (cwd) {
      p = std::string(cwd) + "/" + p;
      free(cwd);
    }
  }
  if (char *real = realpath(p.c_str(), nullptr)) {
    p = real;
    free(real);
  }
  return p;
}

bool parse_page_ranges(const std::string &text, int count, std::vector<int> &pages) {
  pages.clear();
  auto number = [count](std::string s, int &out) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s == "z" || s == "end" || s == "last") {
      out = count;
      return true;
    }
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 7) return false;
    out = atoi(s.c_str());
    return out >= 1 && out <= count;
  };
  size_t start = 0;
  while (start <= text.size()) {
    size_t comma = text.find(',', start);
    std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    size_t dash = part.find('-');
    int a, b;
    if (dash == std::string::npos) {
      if (!number(part, a)) return false;
      b = a;
    } else if (!number(part.substr(0, dash), a) || !number(part.substr(dash + 1), b)) {
      return false;
    }
    int step = a <= b ? 1 : -1;
    for (int p = a;; p += step) {
      pages.push_back(p - 1);
      if (p == b) break;
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return !pages.empty();
}

std::string format_page_ranges(const std::vector<int> &pages) {
  std::string out;
  size_t i = 0;
  while (i < pages.size()) {
    size_t j = i;
    while (j + 1 < pages.size() && pages[j + 1] == pages[j] + 1) ++j;
    if (!out.empty()) out += ",";
    out += std::to_string(pages[i] + 1);
    if (j > i) out += "-" + std::to_string(pages[j] + 1);
    i = j + 1;
  }
  return out;
}

}  // namespace xanara
