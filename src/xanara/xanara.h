// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Xanara: the document viewer and PDF editor of the Selviana Desktop
// Environment. The window holds a command bar, a sidebar (page thumbnails,
// outline and the list of notes) and the page canvas.
#pragma once

#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>
#include <cairo.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "core.h"
#include "paint.h"
#include "theme.h"
#include "widgets.h"

namespace xanara {

namespace icon {
enum Kind {
  Sidebar, Open, Save, Print, Find, Menu, ZoomIn, ZoomOut, FitWidth, FitPage, Prev, Next, Up, Down,
  Select, Highlight, Underline, Strike, Note, TextBox, Pen, Rect, Line, Signature, Close, RotateLeft,
  RotateRight, Moon, Sun, Sepia, Trash, Undo, Present, Pages, Outline, Notes, Book, Single, Continuous,
  Check, Document, Chevron, ChevronDown, Lock
};
void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c);
}  // namespace icon

enum class Tool { Select, Highlight, Underline, Strike, Note, Text, Pen, Rect, Line, Signature };
enum class Layout { Continuous = 0, Single = 1, Book = 2 };
enum class Fit { Free = 0, Width = 1, Page = 2 };

class XanaraWindow;

// The page canvas: layout, zoom, smooth scrolling, sharp tiled rendering,
// selection, find highlights, links, forms and the annotation tools.
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
  void set_scroll_pos(double sx, double sy) {
    sx_ = tx_ = sx;
    sy_ = ty_ = sy;
    clamp_scroll();
    update_current();
    redraw();
  }

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
  void set_highlight_fields(bool on) { highlight_fields_ = on; redraw(); }
  bool highlight_fields() const { return highlight_fields_; }

  // Tools and editing.
  Tool tool() const { return tool_; }
  void set_tool(Tool t);
  Rgb ink() const;
  void set_ink(Rgb c);
  void set_cache_budget(size_t bytes);
  double stroke() const { return stroke_; }
  void set_stroke(double w) { stroke_ = w; }
  bool has_selection() const { return sel_active_ && !sel_boxes_.empty(); }
  std::string selection_text();
  void copy_selection();
  void select_all();
  void clear_selection();
  void apply_markup(MarkupKind kind);
  bool has_selected_annot() const { return annot_sel_page_ >= 0; }
  const Annotation *selected_annot();
  void delete_selected_annot();
  void edit_selected_note();
  void undo();
  bool can_undo() const { return !undo_.empty(); }
  void set_signature(cairo_surface_t *image);  // takes ownership

  // Find.
  void find(const std::string &query, bool match_case);
  void find_next(int dir);
  int find_count() const { return int(hits_.size()); }
  int find_index() const { return hit_; }
  bool find_running() const { return find_page_ >= 0; }
  void find_clear();

  // Thumbnails for the sidebar: the cached surface (or nullptr, queued).
  cairo_surface_t *thumbnail(int page, int width);
  int thumb_height(int page, int width) const;
  void schedule();  // send the wanted tiles to the renderer
  void results_ready();
  void edited(int page);  // a page's content changed

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
    enum Kind { Added, Removed, Moved, Recolored } kind;
    int page;
    PopplerAnnot *annot;
    UndoStep(Kind k, int p, PopplerAnnot *a) : kind(k), page(p), annot(a) {}
    double dx = 0, dy = 0;
    Rgb color;
  };

  RenderParams params() const;
  RenderParams thumb_params(int page, int width) const;
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
  void draw_start_screen(sde::Painter &p);
  void draw_start_text();
  void draw_overlays(sde::Painter &p, int page);
  void draw_scrollbars(sde::Painter &p);
  void animate();
  static void animate_cb(void *p);
  static void settle_cb(void *p);
  static void find_cb(void *p);
  static void schedule_cb(void *p);
  void find_step();
  void update_selection();
  void push_undo(UndoStep s);
  void clear_undo();
  int annot_at(int page, double px, double py) const;
  const FormField *field_at(int page, double px, double py) const;
  const Link *link_at(int page, double px, double py) const;
  void activate_field(int page, const FormField &f);
  void finish_drag(int mx, int my);
  void open_context_menu(int mx, int my);
  void reselect_annot(int page, const Box &box, PopplerAnnotType type);
  void update_cursor(int mx, int my);
  bool keyboard(int key, int state);

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
  bool settled_ = true;  // zoom finished changing: render at the exact scale
  bool highlight_fields_ = true;

  Tool tool_ = Tool::Select;
  Rgb tool_ink_[10];
  double stroke_ = 2.0;

  // Text selection.
  bool sel_active_ = false, selecting_ = false;
  int sel_page0_ = 0, sel_page1_ = 0;
  Point sel_p0_, sel_p1_;
  PopplerSelectionStyle sel_style_ = POPPLER_SELECTION_GLYPH;
  std::vector<std::pair<int, Box>> sel_boxes_;

  // Mouse drags.
  enum class Drag { Idle, Select, Pan, MoveAnnot, Shape, Ink, ScrollV, ScrollH, Link } drag_ = Drag::Idle;
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

  std::vector<UndoStep> undo_;
  cairo_surface_t *signature_ = nullptr;

  // Find.
  std::string query_;
  bool match_case_ = false;
  std::vector<Hit> hits_;
  int hit_ = -1;
  int find_page_ = -1, find_done_ = 0;

  std::string hover_tip_;
  int hover_x_ = 0, hover_y_ = 0;
  int hover_recent_ = -1;
  std::array<int, 4> start_open_{{0, 0, 0, 0}};
  std::vector<std::array<int, 4>> recent_rects_;
};

class Sidebar : public Fl_Widget {
 public:
  enum Tab { Pages = 0, Outline = 1, Notes = 2 };
  Sidebar(XanaraWindow *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void set_tab(Tab t);
  Tab tab() const { return tab_; }
  void document_changed();  // new document: rebuild outline and notes
  void notes_changed() { notes_dirty_ = true; redraw(); }
  void page_changed(int page);  // keep the current thumbnail visible

 private:
  struct Row {
    int depth = 0;
    std::string title, detail;
    LinkTarget target;
    OutlineItem *item = nullptr;
    int page = -1;
  };
  int tabs_h() const;
  void rebuild_rows();
  void rebuild_notes();
  int content_h() const;
  void clamp();
  int thumb_w() const;
  XanaraWindow *app_;
  Tab tab_ = Pages;
  std::vector<OutlineItem> outline_;
  std::vector<Row> rows_, notes_;
  bool notes_dirty_ = true;
  double scroll_ = 0;
  int hover_ = -1, hover_tab_ = -1;
  int follow_page_ = -1;
};

// A command bar button.
struct Button {
  icon::Kind glyph = icon::Open;
  std::string tip, text;
  std::function<void()> action;
  int x = 0, y = 0, w = 0, h = 0;
  bool toggled = false, active = true, visible = true;
  int group = 0;  // buttons of a group share one pill
};

class XanaraWindow : public sde::BufferedWindow {
 public:
  XanaraWindow(int w, int h);
  ~XanaraWindow() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;

  const sde::Theme &theme() const { return theme_; }
  DocView *view() const { return view_; }
  Sidebar *sidebar() const { return sidebar_; }
  Document *document() const { return doc_.get(); }
  History &history() { return history_; }

  bool open_path(const std::string &path, int page = -1);
  void open_dialog();
  bool close_document(bool ask = true);  // false: the user cancelled
  bool save();
  bool save_as();
  void print();
  void toast(const std::string &message);
  void update_chrome();  // page box, zoom label, toggles
  void page_changed();
  void tool_changed();
  void document_edited();
  void toggle_sidebar();
  void toggle_presentation();
  void toggle_fullscreen();
  void show_find(bool show);
  void cycle_color_mode();
  void choose_signature();
  void edit_field(int page, const FormField &f, int x, int y, int w, int h);
  void commit_field();
  bool ask_text(const std::string &title, std::string &text, bool multiline);
  void show_menu(int x, int y);
  void page_operation(const std::string &op);
  void save_settings();
  void about();
  std::vector<HistoryEntry> recent() const;
  static int x_event_handler(int event);
  static XanaraWindow *instance() { return instance_; }

 private:
  void build_buttons();
  void layout();
  void draw_bar(sde::Painter &p);
  void draw_bar_text();
  void draw_strip(sde::Painter &p);
  void draw_find(sde::Painter &p);
  void draw_find_text();
  int button_at(int mx, int my) const;
  void remember_position();
  void apply_theme();
  void load_settings();
  bool replace_document(const std::string &path, const std::string &password, bool working,
                        int page, bool keep_view);
  std::string temp_path(const std::string &tag);
  bool run_qpdf(std::vector<std::string> args, const std::string &out);
  std::string current_source(bool &temp);
  static void toast_cb(void *p);
  static void remember_cb(void *p);
  static void settings_cb(void *p);

  sde::Theme theme_;
  std::unique_ptr<Document> doc_;
  std::string target_;  // the file Save writes to
  bool working_copy_ = false;  // doc_ is a temporary copy after page operations
  std::vector<std::string> temps_;
  History history_;

  DocView *view_ = nullptr;
  Sidebar *sidebar_ = nullptr;
  Fl_Input *page_input_ = nullptr;
  Fl_Input *find_input_ = nullptr;
  Fl_Input *field_input_ = nullptr;
  int field_page_ = -1;
  PopplerFormField *field_ = nullptr;
  std::vector<Button> buttons_;
  int hover_button_ = -1, press_button_ = -1;
  int bar_h_ = 0, strip_h_ = 0, sidebar_w_ = 0;
  bool sidebar_on_ = true, find_on_ = false, find_case_ = false;
  bool fullscreen_ = false;
  int saved_x_ = 0, saved_y_ = 0, saved_w_ = 0, saved_h_ = 0;
  std::string toast_;
  std::string tip_;
  int tip_x_ = 0, tip_y_ = 0;
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
