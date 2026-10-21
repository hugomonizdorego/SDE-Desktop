// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Xanara: the document viewer and PDF editor of the Selviana Desktop
// Environment. Since alpha 16 one window holds several documents in tabs
// (plus a Home tab), a main toolbar with editing modes (Annotate, Edit,
// Page, Form, Fill, Convert, Protect, Tools), a context toolbar with the
// tools of the mode, a left icon rail with its side panel (search,
// thumbnails, outline, bookmarks, annotations, signatures), the page canvas
// and an optional properties panel on the right.
#pragma once

#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>
#include <cairo.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "core.h"
#include "model.h"
#include "paint.h"
#include "theme.h"
#include "widgets.h"

namespace xanara {

namespace icon {
void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c);
}  // namespace icon

// What a press on the page does.
enum class Tool {
  Select, Hand, Highlight, Underline, Squiggly, Strike, Note, Text, Pen, Eraser, Rect, Ellipse, Arrow, Line,
  Measure, Stamp, Signature, Image, Link, Crop, Field, FillText, FillCheck, FillCross, FillDot, FillDate,
  FillInitials, Redact, Count
};
constexpr int kToolCount = int(Tool::Count);
// The canvas tool a context toolbar command arms (Select when it has none).
Tool tool_for(Act a, FieldKind *kind = nullptr);
const char *tool_name(Tool t);
const char *tool_hint(Tool t);
bool tool_has_width(Tool t);
bool tool_has_font(Tool t);
bool tool_has_color(Tool t);

enum class Layout { Continuous = 0, Single = 1, Book = 2 };
enum class Fit { Free = 0, Width = 1, Page = 2 };

class XanaraWindow;

// The page canvas of one document: layout, zoom, smooth scrolling, sharp
// tiled rendering, selection, find highlights, links, forms and the tools.
class DocView : public Fl_Widget {
 public:
  DocView(XanaraWindow *app, int x, int y, int w, int h);
  ~DocView() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;

  void set_document(Document *doc);
  Document *document() const { return doc_; }

  // Navigation.
  int current_page() const { return current_; }
  void go_to(int page, double top = -1, bool smooth = false);
  void next_page();
  void prev_page();
  void scroll_by(double dx, double dy, bool smooth = true);
  void scroll_pages(int dir);  // Page Up / Page Down
  void follow(const LinkTarget &t);
  void scroll_pos(double &sx, double &sy) const {
    sx = sx_;
    sy = sy_;
  }
  void set_scroll_pos(double sx, double sy);

  // Zoom and layout.
  double zoom() const { return zoom_; }  // 1.0 = 100 %
  Fit fit() const { return fit_; }
  void set_fit(Fit f);
  void set_zoom(double z, int anchor_x = -1, int anchor_y = -1);
  void zoom_step(int dir, int anchor_x = -1, int anchor_y = -1);
  Layout layout_mode() const { return layout_; }
  void set_layout(Layout l);
  int rotation() const { return rotation_; }
  void rotate(int delta);
  ColorMode color_mode() const { return mode_; }
  void set_color_mode(ColorMode m);
  void set_presentation(bool on);
  bool presentation() const { return presentation_; }
  void set_highlight_fields(bool on) {
    highlight_fields_ = on;
    redraw();
  }
  bool highlight_fields() const { return highlight_fields_; }
  void set_show_annots(bool on);
  bool show_annots() const { return show_annots_; }

  // Tools and editing.
  Tool tool() const { return tool_; }
  void set_tool(Tool t);
  FieldKind field_kind() const { return field_kind_; }
  void set_field_kind(FieldKind k) { field_kind_ = k; }
  Rgb ink() const;
  void set_ink(Rgb c);
  double opacity() const { return opacity_[int(tool_)]; }
  void set_opacity(double o);
  double stroke() const { return stroke_; }
  void set_stroke(double w) { stroke_ = w; }
  double font_size() const { return font_size_; }
  void set_font_size(double s) { font_size_ = s; }
  int align() const { return align_; }
  void set_align(int a) { align_ = a; }
  void set_stamp_text(const std::string &s) { stamp_text_ = s; }
  const std::string &stamp_text() const { return stamp_text_; }
  void set_cache_budget(size_t bytes);
  bool has_selection() const { return sel_active_ && !sel_boxes_.empty(); }
  std::string selection_text();
  void copy_selection();
  void select_all();
  void clear_selection();
  void apply_markup(MarkupKind kind);
  bool has_selected_annot() const { return annot_sel_page_ >= 0; }
  const Annotation *selected_annot();
  int selected_annot_page() const { return annot_sel_page_; }
  void select_annot(int page, const Annotation &a);
  void delete_selected_annot();
  void edit_selected_note();
  void set_selected_opacity(double o);
  void undo();
  void redo();
  bool can_undo() const { return !undo_.empty(); }
  bool can_redo() const { return !redo_.empty(); }
  void set_signature(cairo_surface_t *image);  // takes ownership
  cairo_surface_t *signature() const { return signature_; }
  void set_image(cairo_surface_t *image);  // the picture the Image tool places; takes ownership

  // Redaction marks (not yet applied).
  const std::vector<std::pair<int, Box>> &redactions() const { return redactions_; }
  void clear_redactions() {
    redactions_.clear();
    redraw();
  }

  // Find.
  void find(const std::string &query, bool match_case);
  void find_next(int dir);
  int find_count() const { return int(hits_.size()); }
  int find_index() const { return hit_; }
  bool find_running() const { return find_page_ >= 0; }
  void find_clear();
  const std::string &find_query() const { return query_; }
  int hit_page(int k) const { return hits_[size_t(k)].page; }
  Box hit_box(int k) const { return hits_[size_t(k)].box; }
  void show_hit(int k);

  // Thumbnails for the sidebar: the cached surface (or nullptr, queued).
  cairo_surface_t *thumbnail(int page, int width);
  int thumb_height(int page, int width) const;
  void schedule();  // send the wanted tiles to the renderer
  void results_ready();
  void edited(int page);  // a page's content changed
  void forget_history();  // drops undo/redo (the document was replaced)

 private:
  struct Slot {
    int x = 0, y = 0, w = 0, h = 0;
    bool shown = false;
  };
  struct Hit {
    int page;
    Box box;
  };
  struct UndoStep {
    enum Kind { Added, Removed, Moved, Recolored, Faded } kind;
    int page;
    PopplerAnnot *annot;
    UndoStep(Kind k, int p, PopplerAnnot *a) : kind(k), page(p), annot(a) {}
    double dx = 0, dy = 0;
    Rgb color, color2;
    double opacity = 1, opacity2 = 1;
  };

  RenderParams params() const;
  RenderParams thumb_params(int page, int width) const;
  int mode_key() const { return int(mode_) | (show_annots_ ? 0 : 16); }
  double base_scale() const;
  double fit_zoom(Fit f) const;
  void relayout(bool keep_position = true);
  void clamp_scroll();
  int view_w() const { return w(); }
  int view_h() const { return h(); }
  void page_box_device(int page, const Box &b, double &x, double &y, double &w, double &h) const;
  bool screen_to_page(int mx, int my, int &page, double &px, double &py, bool nearest) const;
  void update_current();
  void draw_page(sde::Painter &p, int page, std::vector<RenderJob> &jobs);
  void draw_overlays(sde::Painter &p, int page);
  void draw_preview(sde::Painter &p);
  void draw_scrollbars(sde::Painter &p);
  void animate();
  static void animate_cb(void *p);
  static void settle_cb(void *p);
  static void find_cb(void *p);
  static void schedule_cb(void *p);
  void find_step();
  void update_selection();
  void push_undo(UndoStep s);
  void clear_steps(std::vector<UndoStep> &steps);
  void apply_step(const UndoStep &u, bool forward);
  int annot_at(int page, double px, double py) const;
  const FormField *field_at(int page, double px, double py) const;
  const Link *link_at(int page, double px, double py) const;
  void activate_field(int page, const FormField &f);
  void finish_drag(int mx, int my);
  void place_at(Tool t, int page, Point at, const Box &dragged, bool moved);
  PopplerAnnot *finish_added(int page, PopplerAnnot *a);
  void erase_at(int page, double px, double py);
  void open_context_menu(int mx, int my);
  void reselect_annot(int page, const Box &box, PopplerAnnotType type);
  void update_cursor(int mx, int my);
  bool keyboard(int key, int state);
  bool is_text_tool(Tool t) const;
  bool is_box_tool(Tool t) const;

  XanaraWindow *app_;
  Document *doc_ = nullptr;
  std::unique_ptr<TileCache> cache_;
  std::unique_ptr<Renderer> renderer_;
  std::vector<RenderJob> thumb_jobs_, view_jobs_;
  bool schedule_pending_ = false;

  std::vector<Slot> slots_;
  int canvas_w_ = 0, canvas_h_ = 0;
  double sx_ = 0, sy_ = 0, tx_ = 0, ty_ = 0;  // scroll and smooth-scroll target
  bool animating_ = false;
  int current_ = 0;
  double zoom_ = 1.0;
  Fit fit_ = Fit::Width;
  Layout layout_ = Layout::Continuous;
  int rotation_ = 0;
  ColorMode mode_ = ColorMode::Normal;
  bool presentation_ = false;
  Layout saved_layout_ = Layout::Continuous;
  Fit saved_fit_ = Fit::Width;
  double saved_zoom_ = 1.0;
  bool settled_ = true;  // zoom finished changing: render at the exact scale
  bool highlight_fields_ = true;
  bool show_annots_ = true;

  Tool tool_ = Tool::Select;
  FieldKind field_kind_ = FieldKind::Text;
  Rgb tool_ink_[kToolCount];
  double opacity_[kToolCount];
  double stroke_ = 2.0, font_size_ = 12.0;
  int align_ = 0;
  std::string stamp_text_ = "APPROVED";

  // Text selection.
  bool sel_active_ = false, selecting_ = false;
  int sel_page0_ = 0, sel_page1_ = 0;
  Point sel_p0_, sel_p1_;
  PopplerSelectionStyle sel_style_ = POPPLER_SELECTION_GLYPH;
  std::vector<std::pair<int, Box>> sel_boxes_;

  // Mouse drags.
  enum class Drag { Idle, Select, Pan, MoveAnnot, Shape, Ink, Erase, ScrollV, ScrollH, Link } drag_ = Drag::Idle;
  int press_x_ = 0, press_y_ = 0, drag_page_ = -1;
  Point drag_p0_, drag_p1_;
  double press_sx_ = 0, press_sy_ = 0;
  std::vector<Point> stroke_pts_;
  LinkTarget press_link_;

  // The selected annotation.
  int annot_sel_page_ = -1;
  Box annot_sel_box_;
  PopplerAnnotType annot_sel_type_ = POPPLER_ANNOT_UNKNOWN;
  double move_dx_ = 0, move_dy_ = 0;

  std::vector<UndoStep> undo_, redo_;
  cairo_surface_t *signature_ = nullptr, *image_ = nullptr;
  std::vector<std::pair<int, Box>> redactions_;

  // Find.
  std::string query_;
  bool match_case_ = false;
  std::vector<Hit> hits_;
  int hit_ = -1;
  int find_page_ = -1, find_done_ = 0;

  std::string hover_tip_;
  int hover_x_ = 0, hover_y_ = 0;
};

// The left icon rail and the side panel it controls.
class Sidebar : public Fl_Widget {
 public:
  enum Panel { Search = 0, Thumbnails, Outline, Bookmarks, Annotations, Signatures, PanelCount };
  Sidebar(XanaraWindow *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void set_panel(Panel p);
  Panel panel() const { return panel_; }
  bool panel_open() const { return open_; }
  void set_panel_open(bool on) { open_ = on; }
  int rail_w() const;
  void document_changed();  // new document: rebuild outline and notes
  void notes_changed() {
    notes_dirty_ = true;
    redraw();
  }
  void page_changed(int page);  // keep the current thumbnail visible
  void search_changed() { redraw(); }
  // Where the rail's page box and the search field go.
  void page_box(int &x, int &y, int &w, int &h) const;
  void search_box(int &x, int &y, int &w, int &h) const;
  static const char *panel_title(Panel p);
  static const char *panel_id(Panel p);

 private:
  struct Row {
    int depth = 0;
    std::string title, detail;
    LinkTarget target;
    OutlineItem *item = nullptr;
    int page = -1;
    int hit = -1;  // search result index
    Box box;
    PopplerAnnotType type = POPPLER_ANNOT_UNKNOWN;
    std::string path;  // signatures
  };
  int header_h() const;
  int panel_x() const { return x() + rail_w(); }
  int panel_w() const { return w() - rail_w(); }
  int content_top() const;
  void rebuild_rows();
  void rebuild_notes();
  void rebuild_search();
  void rebuild_bookmarks();
  void rebuild_signatures();
  const std::vector<Row> &rows() const;
  int row_h() const;
  int content_h() const;
  void clamp();
  int thumb_cols() const;
  int thumb_w() const;
  int thumb_cell_h(int page) const;
  int item_at(int mx, int my) const;
  int rail_item_at(int mx, int my) const;
  void draw_rail(sde::Painter &p);
  void draw_rail_text();
  XanaraWindow *app_;
  Panel panel_ = Thumbnails;
  bool open_ = true;
  std::vector<OutlineItem> outline_;
  std::vector<Row> rows_, notes_, search_, bookmarks_, signatures_;
  std::string search_key_;
  int search_count_ = -1;
  bool notes_dirty_ = true;
  double scroll_ = 0;
  int hover_ = -1, hover_rail_ = -1;
  int drag_from_ = -1, drag_to_ = -1;  // thumbnail reordering
  int press_x_ = 0, press_y_ = 0;
  bool resizing_ = false;
};

// The properties panel on the right.
class PropsPanel : public Fl_Widget {
 public:
  PropsPanel(XanaraWindow *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;

 private:
  struct Item {
    enum Kind { Swatch, Opacity, Width, FontSize, Align, Action } kind;
    int x, y, w, h;
    int index;  // swatch / width / align / action number
  };
  void build();
  XanaraWindow *app_;
  std::vector<Item> items_;
  int hover_ = -1;
  bool dragging_opacity_ = false;
  double scroll_ = 0;
};

// The Home tab: quick actions and the recent documents as a grid of
// thumbnails.
class HomeView : public Fl_Widget {
 public:
  HomeView(XanaraWindow *app, int x, int y, int w, int h);
  ~HomeView() override;
  void draw() override;
  int handle(int event) override;
  void refresh();  // the recent list changed

 private:
  struct Card {
    int x, y, w, h;
    int action;  // >= 0: recent index; < 0: quick action
  };
  static void thumb_cb(void *p);
  void build();
  XanaraWindow *app_;
  std::vector<Card> cards_;
  std::map<std::string, cairo_surface_t *> thumbs_;
  std::vector<std::string> pending_;
  int hover_ = -1000;
  double scroll_ = 0;
  int content_h_ = 0;
};

// One open document.
struct DocTab {
  std::unique_ptr<Document> doc;
  DocView *view = nullptr;
  std::string target;  // the file Save writes to
  bool working_copy = false;  // doc is a temporary copy after page operations
  std::vector<std::string> temps;
  bool dirty() const { return doc && (doc->modified() || working_copy); }
};

// A clickable spot of the window chrome (tab strip, toolbar, context bar).
struct Hot {
  enum Kind { Icon, Label, TabHome, Tab, TabClose, ModeTab, Overflow, Zoom, Pointer } kind = Icon;
  icon::Kind glyph = icon::Open;
  std::string text, tip;
  std::function<void()> action;
  int x = 0, y = 0, w = 0, h = 0;
  bool toggled = false, enabled = true, chevron = false;
  int tab = -1;  // tab index
  int row = 0;   // 0 tabs, 1 toolbar, 2 context bar
};

class XanaraWindow : public sde::BufferedWindow {
 public:
  XanaraWindow(int w, int h);
  ~XanaraWindow() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;
  void quit();  // asks about unsaved changes, then closes

  const sde::Theme &theme() const { return theme_; }
  DocView *view() const;          // the active document's view (nullptr on Home)
  Sidebar *sidebar() const { return sidebar_; }
  Document *document() const;     // the active document (nullptr on Home)
  DocTab *tab() const;
  History &history() { return history_; }
  Mode mode() const { return mode_; }
  void set_mode(Mode m);
  bool props_on() const { return props_on_ && props_->visible(); }
  bool have(const std::string &program);  // cached find_program

  // Tabs and files.
  bool open_path(const std::string &path, int page = -1);
  void open_dialog();
  void activate_tab(int index);  // -1: Home
  bool close_tab(int index, bool ask = true);  // false: the user cancelled
  bool save();
  bool save_as();
  void print();
  void toast(const std::string &message);
  void update_chrome();  // page box, zoom label, toggles, title
  void page_changed();
  void tool_changed();
  void document_edited();
  void toggle_sidebar();
  void toggle_props();
  void toggle_presentation();
  void toggle_fullscreen();
  void show_find(bool show);
  void cycle_color_mode();
  void choose_signature();
  void draw_signature();
  void edit_field(int page, const FormField &f, int x, int y, int w, int h);
  void commit_field();
  bool ask_text(const std::string &title, std::string &text, bool multiline);
  bool ask_choice(const std::string &title, const std::string &message, const std::vector<std::string> &options,
                  int &choice);
  void show_menu(int x, int y);
  void save_settings();
  void settings_changed();  // saves soon
  void about();
  std::vector<HistoryEntry> recent() const;
  void forget_recent(const std::string &path);
  static int x_event_handler(int event);
  static XanaraWindow *instance() { return instance_; }
  void layout();
  void find_input_changed();
  int side_w() const { return side_w_; }
  void set_side_w(int w);
  std::vector<std::string> bookmarks(const std::string &path);
  void toggle_bookmark();
  std::string signatures_dir() const;

  // Commands (commands.cpp).
  void run(Act a);  // a context toolbar command
  void canvas_box(Tool t, int page, const Box &box);  // Link, Crop and form fields
  void page_operation(const std::string &op, int page = -1, int to = -1);
  void merge_files();
  void images_to_pdf_dialog();
  void apply_redactions();
  bool working_change(const std::string &out, const std::string &message);

 private:
  void build_chrome();
  void layout_toolbar(int y0, int h);
  void layout_context(int y0, int h);
  void layout_tabs(int y0, int h);
  void draw_chrome(sde::Painter &p);
  void draw_chrome_text();
  void draw_hot(sde::Painter &p, const Hot &h, int index);
  int hot_at(int mx, int my) const;
  void remember_position();
  void apply_theme();
  void load_settings();
  void save_session();
  bool replace_document(DocTab &t, const std::string &path, const std::string &password, bool working, int page,
                        bool keep_view);
  std::string temp_path(const std::string &tag);
  bool run_tool(const std::vector<std::string> &argv, const std::string &out, const std::string &what);
  std::string current_source();
  bool qpdf_update(const std::function<bool(PdfUpdate &, const PdfObjects &)> &plan, const std::string &message);
  void stamp_pages(StampSpec::Kind kind);
  void export_images(bool jpeg);
  void export_text(int kind);
  void encrypt();
  void decrypt();
  void compress();
  void ocr();
  void compare();
  void zoom_menu(int x, int y);
  void pointer_menu(int x, int y);
  void export_menu(int x, int y);
  void overflow_menu(int x, int y, const std::vector<const ToolSpec *> &tools);
  void mode_menu(int x, int y, const std::vector<int> &modes);
  bool tool_enabled(const ToolSpec &s, std::string *why = nullptr);
  bool tool_toggled(const ToolSpec &s) const;
  int tab_x(int index) const;
  static void toast_cb(void *p);
  static void remember_cb(void *p);
  static void settings_cb(void *p);

  sde::Theme theme_;
  std::vector<std::unique_ptr<DocTab>> tabs_;  // parallel to tablist_
  TabList tablist_;
  History history_;
  Mode mode_ = Mode::Annotate;
  std::map<std::string, bool> programs_;

  HomeView *home_ = nullptr;
  Sidebar *sidebar_ = nullptr;
  PropsPanel *props_ = nullptr;
  Fl_Input *page_input_ = nullptr;
  Fl_Input *find_input_ = nullptr;
  Fl_Input *field_input_ = nullptr;
  int field_page_ = -1;
  PopplerFormField *field_ = nullptr;
  DocView *field_view_ = nullptr;

  std::vector<Hot> hots_;
  int hover_hot_ = -1, press_hot_ = -1;
  int tabs_h_ = 0, bar_h_ = 0, ctx_h_ = 0, side_w_ = 0, props_w_ = 0;
  int tab_w_ = 0, tabs_x0_ = 0;
  bool sidebar_on_ = true, props_on_ = false, find_case_ = false;
  bool fullscreen_ = false;
  int drag_tab_ = -1, drag_tab_x_ = 0, drag_tab_to_ = -1;
  int saved_x_ = 0, saved_y_ = 0, saved_w_ = 0, saved_h_ = 0;
  int last_mx_ = -1, last_my_ = -1;
  std::string toast_;
  std::string tip_;
  int tip_x_ = 0, tip_y_ = 0;
  std::string initials_;
  static XanaraWindow *instance_;
};

// Colours offered for annotations.
struct Swatch {
  const char *name;
  Rgb color;
};
extern const Swatch kSwatches[];
extern const int kSwatchCount;

std::string display_name(const std::string &path);

}  // namespace xanara
