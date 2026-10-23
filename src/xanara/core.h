// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Xanara core: the PDF document model (poppler-glib), sharp tile rendering,
// the memory bounded tile cache, the background renderer and the reading
// history. Nothing in here touches FLTK, so the unit tests use it directly.
#pragma once

#include <cairo.h>
#include <poppler.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xanara {

// A rectangle in page space: PDF points, origin at the top-left corner of
// the (already /Rotate-d) page, y growing downwards.
struct Box {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  double w() const { return x2 - x1; }
  double h() const { return y2 - y1; }
  bool contains(double x, double y, double slack = 0) const {
    return x >= x1 - slack && x <= x2 + slack && y >= y1 - slack && y <= y2 + slack;
  }
  bool empty() const { return x2 <= x1 || y2 <= y1; }
  Box normalized() const;
  Box united(const Box &o) const;
};

struct Point {
  double x = 0, y = 0;
};

struct Rgb {
  double r = 0, g = 0, b = 0;
};

enum class ColorMode { Normal = 0, Night = 1, Sepia = 2 };

// How a page is turned into device pixels. |scale| is device pixels per PDF
// point (zoom x 96/72 x UI scale); |rotation| is the view rotation in
// degrees clockwise (0, 90, 180, 270), applied on top of the page's /Rotate.
struct RenderParams {
  double scale = 1.0;
  int rotation = 0;
  ColorMode mode = ColorMode::Normal;
  bool annots = true;  // false: draw the page without its markup annotations
};

// Pixel size of a whole page of |pw| x |ph| points.
void page_pixels(double pw, double ph, const RenderParams &rp, int &w, int &h);
// Page point <-> device pixel inside the page's own pixel box.
void page_to_device(double pw, double ph, const RenderParams &rp, double px, double py, double &dx,
                    double &dy);
void device_to_page(double pw, double ph, const RenderParams &rp, double dx, double dy, double &px,
                    double &py);

// Renders the device pixel rectangle (tx, ty, tw, th) of |page| into a new
// opaque image surface (caller owns it). Text uses grayscale antialiasing at
// the exact device resolution, so glyph edges land on real pixels.
cairo_surface_t *render_area(PopplerPage *page, double pw, double ph, const RenderParams &rp, int tx,
                             int ty, int tw, int th);
// Night (inverted, soft contrast) and sepia reading modes, in place.
void apply_color_mode(cairo_surface_t *surface, ColorMode mode);

// ------------------------------------------------------------------ links

struct LinkTarget {
  enum Kind { Nothing, Page, Uri, File, Named } kind = Nothing;
  int page = -1;          // 0-based, for Page
  double top = -1;        // page space y to scroll to, or -1
  std::string text;       // URI, file name or named action
};

struct Link {
  Box box;
  LinkTarget target;
};

struct OutlineItem {
  std::string title;
  LinkTarget target;
  int level = 0;
  bool expanded = false;
  std::vector<OutlineItem> children;
};

// --------------------------------------------------------------- editing

enum class MarkupKind { Highlight, Underline, StrikeOut, Squiggly };

// One annotation of a page as the viewer shows and edits it.
struct Annotation {
  PopplerAnnot *annot = nullptr;  // a reference owned by the page cache
  PopplerAnnotType type = POPPLER_ANNOT_UNKNOWN;
  Box box;
  Rgb color;
  bool has_color = false;
  double opacity = 1.0;
  std::string contents, author;
  std::vector<std::vector<Point>> strokes;  // ink created in this session
};

struct FormField {
  PopplerFormField *field = nullptr;  // owned reference
  PopplerFormFieldType type = POPPLER_FORM_FIELD_UNKNOWN;
  Box box;
  int id = 0;
  bool read_only = false;
};

// True when the running poppler can create these annotations natively;
// otherwise Xanara stores an image stamp that looks the same.
bool native_free_text();
bool native_ink();

class Document {
 public:
  ~Document();
  Document(const Document &) = delete;
  Document &operator=(const Document &) = delete;

  // Opens |path|. On failure returns nullptr, sets |error| and sets
  // |encrypted| when a (different) password is needed.
  static std::unique_ptr<Document> open(const std::string &path, const std::string &password,
                                        std::string &error, bool &encrypted);

  int count() const { return int(sizes_.size()); }
  double width(int i) const { return sizes_[size_t(i)].x; }
  double height(int i) const { return sizes_[size_t(i)].y; }
  const std::string &path() const { return path_; }
  const std::string &password() const { return password_; }
  std::string title() const;
  std::string page_label(int i) const;

  // Every poppler call on this document must hold this lock; the renderer
  // thread takes it for each tile.
  std::mutex &mutex() const { return mu_; }
  PopplerDocument *raw() const { return doc_; }
  PopplerPage *page(int i);  // lock held; owned by the document

  // Renders under the lock. Thread safe.
  cairo_surface_t *render(int page, const RenderParams &rp, int tx, int ty, int tw, int th);

  // Content revision of a page: bumped by every edit so cached tiles of the
  // old content are never shown again.
  uint64_t revision(int page) const;
  bool modified() const { return modified_; }
  void set_modified(bool m) { modified_ = m; }

  // Writes the document (with its annotations and form values) to a
  // temporary file next to |target| and renames it over |target|, so the
  // original is never left half written.
  bool save_to(const std::string &target, std::string &error);

  // Page queries (take the lock themselves; results are cached).
  const std::vector<Link> &links(int page);
  const std::vector<Annotation> &annotations(int page);
  const std::vector<FormField> &fields(int page);
  std::vector<Box> find(int page, const std::string &text, bool match_case, bool whole_words);
  std::vector<Box> selection_boxes(int page, const Box &sel, PopplerSelectionStyle style);
  std::string selected_text(int page, const Box &sel, PopplerSelectionStyle style);
  std::string text(int page);
  std::vector<OutlineItem> outline();
  LinkTarget resolve(PopplerAction *action);

  // Editing. Each returns the new annotation (or nullptr) and marks the page
  // and the document modified.
  PopplerAnnot *add_markup(int page, MarkupKind kind, const std::vector<Box> &boxes, Rgb color);
  PopplerAnnot *add_note(int page, double x, double y, const std::string &text, Rgb color);
  PopplerAnnot *add_free_text(int page, const Box &box, const std::string &text, Rgb color,
                              double font_size);
  PopplerAnnot *add_ink(int page, const std::vector<std::vector<Point>> &strokes, Rgb color,
                        double width);
  PopplerAnnot *add_square(int page, const Box &box, Rgb color, double width);
  PopplerAnnot *add_line(int page, Point a, Point b, Rgb color, double width);
  PopplerAnnot *add_circle(int page, const Box &box, Rgb color, double width);
  // |image| is any cairo image surface (e.g. a signature); it is stretched to |box|.
  PopplerAnnot *add_image(int page, const Box &box, cairo_surface_t *image, const std::string &label);
  bool remove_annot(int page, PopplerAnnot *annot);
  bool restore_annot(int page, PopplerAnnot *annot);  // undo a removal
  bool move_annot(int page, PopplerAnnot *annot, double dx, double dy);
  bool set_annot_color(int page, PopplerAnnot *annot, Rgb color);
  bool set_annot_contents(int page, PopplerAnnot *annot, const std::string &text);
  bool set_annot_opacity(int page, PopplerAnnot *annot, double opacity);  // markup annotations only
  // The document information (title, author, subject, keywords, creator,
  // producer) as label/value pairs, for the properties panel.
  std::vector<std::pair<std::string, std::string>> info();

  // Forms.
  bool set_field_text(int page, PopplerFormField *f, const std::string &text);
  bool toggle_field(int page, PopplerFormField *f);
  bool set_field_choice(int page, PopplerFormField *f, int item);  // -1 clears

  // Invalidates the cached queries and bumps the page revision.
  void touch(int page);

 private:
  Document() = default;
  void clear_page_cache(int page);
  PopplerRectangle to_pdf(int page, const Box &b) const;
  Box from_pdf(int page, const PopplerRectangle &r) const;
  PopplerAnnot *finish_annot(int page, PopplerAnnot *annot, Rgb color);

  PopplerDocument *doc_ = nullptr;
  std::string path_, password_;
  std::vector<Point> sizes_;
  std::vector<PopplerPage *> pages_;
  std::vector<uint64_t> revisions_;
  struct PageInfo {
    bool links_ok = false, annots_ok = false, fields_ok = false;
    std::vector<Link> links;
    std::vector<Annotation> annots;
    std::vector<FormField> fields;
  };
  std::vector<PageInfo> info_;
  // Geometry poppler keeps outside the rectangle (line vertices, ink
  // paths) for annotations made in this session, found by page + rectangle
  // because poppler hands out a new wrapper object on every query.
  struct Geometry {
    int page;
    PopplerRectangle rect;
    std::vector<std::vector<Point>> strokes;
  };
  std::vector<Geometry> geometry_;
  bool modified_ = false;
  mutable std::mutex mu_;
};

// ------------------------------------------------------------- tile cache

constexpr int kTileSize = 512;  // device pixels

struct TileKey {
  int page = 0;
  int col = 0, row = 0;   // col < 0: whole page thumbnail
  int scale_q = 0;        // lround(scale * 10000)
  int rotation = 0;
  int mode = 0;
  uint64_t revision = 0;
  bool operator==(const TileKey &o) const {
    return page == o.page && col == o.col && row == o.row && scale_q == o.scale_q &&
           rotation == o.rotation && mode == o.mode && revision == o.revision;
  }
};

struct TileKeyHash {
  size_t operator()(const TileKey &k) const;
};

int quantize_scale(double scale);

// Least recently used cache of rendered tiles, bounded in bytes. Not thread
// safe: the main thread owns it.
class TileCache {
 public:
  explicit TileCache(size_t budget_bytes);
  ~TileCache();
  TileCache(const TileCache &) = delete;
  TileCache &operator=(const TileCache &) = delete;

  cairo_surface_t *get(const TileKey &key);  // marks it recently used
  bool contains(const TileKey &key) const { return map_.count(key) != 0; }
  // Takes ownership. Evicts the least recently used tiles over budget, but
  // never the tile just inserted.
  void put(const TileKey &key, cairo_surface_t *surface);
  void drop_page(int page);
  void clear();
  void set_budget(size_t bytes);
  size_t budget() const { return budget_; }
  size_t bytes() const { return bytes_; }
  size_t size() const { return map_.size(); }
  // Visits the tiles of a page (any scale), most recent first.
  void each_of_page(int page, const std::function<void(const TileKey &, cairo_surface_t *)> &fn) const;

  static size_t surface_bytes(cairo_surface_t *s);

 private:
  struct Entry {
    TileKey key;
    cairo_surface_t *surface;
    size_t bytes;
  };
  void evict(const TileKey *keep);
  std::list<Entry> lru_;  // front = most recent
  std::unordered_map<TileKey, std::list<Entry>::iterator, TileKeyHash> map_;
  size_t budget_, bytes_ = 0;
};

// ---------------------------------------------------------------- renderer

struct RenderJob {
  TileKey key;
  RenderParams params;
  int tx = 0, ty = 0, tw = 0, th = 0;
};

struct RenderResult {
  TileKey key;
  cairo_surface_t *surface = nullptr;
};

// One background thread that renders tiles in priority order. The queue is
// replaced on every frame with what is visible now, so stale work is
// dropped as the user scrolls or zooms.
class Renderer {
 public:
  Renderer(Document *doc, std::function<void()> notify);
  ~Renderer();
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

  void replace_queue(std::vector<RenderJob> jobs);
  std::vector<RenderResult> take_results();
  bool busy() const;

 private:
  void run();
  Document *doc_;
  std::function<void()> notify_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<RenderJob> queue_;
  bool has_current_ = false;
  TileKey current_;
  std::vector<RenderResult> results_;
  bool stop_ = false;
  std::atomic<bool> notified_{false};
  std::thread thread_;
};

// ----------------------------------------------------------------- history

// Recently opened documents and the last position in each, kept in
// $XDG_STATE_HOME/sde/xanara-history (tab separated, newest first).
struct HistoryEntry {
  std::string path;
  int page = 0;
  std::string zoom = "width";  // width, page or a scale factor
  long long time = 0;
};

class History {
 public:
  explicit History(std::string file = "");
  void load();
  bool save() const;
  const HistoryEntry *find(const std::string &path) const;
  void remember(const HistoryEntry &e);  // moves it to the front
  void forget(const std::string &path);
  std::vector<HistoryEntry> recent(size_t n, bool existing_only = true) const;
  static std::string default_file();

 private:
  std::string file_;
  std::vector<HistoryEntry> entries_;
};

// Helpers shared with the UI.
std::string file_uri(const std::string &path);
std::string path_from_argument(const std::string &arg);  // file:// URIs and relative paths
// Page selections such as "1-3,7,9-z" (1-based in text, 0-based out).
bool parse_page_ranges(const std::string &text, int count, std::vector<int> &pages);
std::string format_page_ranges(const std::vector<int> &pages);  // 0-based in, 1-based text

}  // namespace xanara
