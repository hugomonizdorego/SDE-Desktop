// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo's collage editor (the Collage plugin): a window of its own, opened
// with File > New Collage, "sde-view --collage FILE..." or Gallery's
// "Create Collage".
//
//   CollageWindow  the window: header bar, photos, undo, export
//   CollageCanvas  the collage: select, pan and zoom photos in their cells,
//                  drag one photo onto another to swap them, drop files
//   CollagePanel   Design (the ten designs with live previews), Style (size,
//                  spacing, corners, border, background, title) and Photos
//                  (order by drag and drop, the selected photo)
//
// While editing, photos are shown from proxies decoded at the size the
// preview needs (sharper ones are loaded as cells grow or are zoomed);
// exporting decodes each photo at the size the full output needs.
#pragma once

#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>
#include <cairo.h>

#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "collage.h"
#include "line_icons.h"
#include "theme.h"
#include "thumbnails.h"
#include "widgets.h"

namespace photo {

class AppMenu;
class CollageWindow;

class CollageCanvas : public Fl_Widget {
 public:
  CollageCanvas(CollageWindow *app, int x, int y, int w, int h);
  ~CollageCanvas() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  // The collage or its photos changed: render the preview again.
  void invalidate();
  // Where the collage is shown, and screen pixels per output pixel.
  void picture_rect(double &x, double &y, double &w, double &h) const;
  double view_scale() const;
  bool busy() const { return dragging_; }
  // Cheaper rendering while a slider or a photo is dragged.
  void set_fast(bool on) { fast_ = on; }

 private:
  void render_preview();
  bool to_output(int mx, int my, double &ox, double &oy) const;
  void cell_screen_box(const CollageCell &c, double &x, double &y, double &w, double &h) const;
  void outline_cell(cairo_t *cr, const CollageCell &c, double grow) const;
  void context_menu(int cell);
  CollageWindow *app_;
  cairo_surface_t *image_ = nullptr;  // the preview at its screen size
  int image_w_ = 0, image_h_ = 0;
  bool dirty_ = true, fast_ = false;
  int hover_ = -1;                     // cell under the pointer
  int press_cell_ = -1, drop_cell_ = -1;
  bool dragging_ = false, swapping_ = false, panned_ = false;
  int last_x_ = 0, last_y_ = 0, ghost_x_ = 0, ghost_y_ = 0;
  Collage press_collage_;
};

class CollagePanel : public Fl_Group {
 public:
  enum Tab { Designs, Style, Photos };
  CollagePanel(CollageWindow *app, int x, int y, int w, int h);
  ~CollagePanel() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void set_tab(Tab t);
  Tab tab() const { return tab_; }
  // The collage changed: update the controls (and the design previews,
  // a moment later).
  void sync();
  void previews_changed();
  void layout();
  bool editing_text() const;

 private:
  struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    int id = 0;
    bool in(int mx, int my) const { return mx >= x && mx < x + w && my >= y && my < y + h; }
  };
  int top() const { return y() + sde::S(58); }
  int bottom() const { return y() + h() - sde::S(64); }
  void draw_content(sde::Painter *p);
  int click(int mx, int my);
  int box_at(int mx, int my) const;
  void slider_changed(int which);
  void render_previews();
  static void previews_cb(void *p);
  void pick_custom_size();
  void pick_color(bool border);
  CollageWindow *app_;
  Tab tab_ = Designs;
  std::vector<Box> boxes_;
  int hover_ = -1;
  double scroll_ = 0, content_h_ = 0;
  sde::Slider *spacing_ = nullptr, *radius_ = nullptr, *border_ = nullptr, *zoom_ = nullptr;
  Fl_Input *title_ = nullptr;
  sde::FlatButton *export_ = nullptr, *open_ = nullptr;
  std::vector<cairo_surface_t *> previews_;
  bool previews_dirty_ = true;
  bool gesture_ = false;  // a slider drag in progress
  // Reordering in the Photos tab.
  int drag_from_ = -1, drag_to_ = -1;
  bool drag_moved_ = false;
  int press_x_ = 0, press_y_ = 0;
  double label_y_[8] = {0};  // section titles of the Style tab
  friend class CollageWindow;
};

class CollageWindow : public sde::BufferedWindow {
 public:
  CollageWindow(int w, int h);
  ~CollageWindow() override;
  static CollageWindow *instance() { return instance_; }
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;

  const sde::Theme &theme() const { return theme_; }
  const Collage &collage() const { return history_.current(); }
  const CollageLayout &layout_info() const { return layout_; }

  // Changes. |amend| replaces the current state (the steps of a drag)
  // instead of adding an undo step.
  void apply(const Collage &c, bool amend = false);
  void undo();
  void redo();
  bool can_undo() const { return history_.can_undo(); }
  bool can_redo() const { return history_.can_redo(); }
  void add_photos(const std::vector<std::string> &paths);
  void add_dialog();
  void replace_dialog(int item);
  void remove_photo(int item);
  void select(int item);
  int selected() const { return selected_; }
  void choose_design(CollageDesign d);
  void shuffle();
  void reset_position(int item);
  void zoom_selected(double factor);
  void move_selected(int delta);
  void use_as_background(int item);

  // Images of the photos.
  cairo_surface_t *proxy(int item);
  cairo_surface_t *thumb(const std::string &path);
  cairo_surface_t *backdrop(int item);
  // Asks for proxies sharp enough for the preview at |scale| (screen
  // pixels per output pixel), and thumbnails for the panel.
  void want_images(double scale);

  // Output.
  void export_dialog();
  void open_in_photo();
  bool confirm_close();
  void toast(const std::string &message);
  void show_shortcuts();
  void layout();
  void apply_theme();

 private:
  struct Button {
    gallery::icon::Kind glyph;
    std::string tip;
    std::function<void()> action;
    double x = 0, y = 0, w = 0, h = 0;
    bool active = true, toggled = false;
  };
  struct Image {
    time_t mtime = 0;
    cairo_surface_t *proxy = nullptr;  // decoded for the preview
    int dim = 0, requested = 0;        // its size limit, and the largest asked for
    int natural = 0;                   // longest side of the photo (0: unknown)
    cairo_surface_t *thumb = nullptr;  // a small one for the panel and previews
    cairo_surface_t *backdrop = nullptr;
    bool failed = false;
  };
  void build_menu();
  void layout_buttons();
  void changed();  // after every change of the collage
  void export_to(const std::string &path, int quality, bool then_open);
  void exported(bool ok, const std::string &path, const std::string &error, bool then_open);
  std::string default_export_path(const char *ext) const;
  void refresh_title_suggestion(Collage &c) const;
  bool keyboard(int key, int state);
  static void awake_cb(void *p);
  static void toast_cb(void *p);
  static void menu_cb(Fl_Widget *w, void *data);

  static CollageWindow *instance_;
  sde::Theme theme_;
  CollageHistory history_;
  CollageLayout layout_;
  int selected_ = -1;
  bool unsaved_ = false;  // changed since the last export
  bool exporting_ = false;
  std::map<std::string, Image> images_;
  std::unique_ptr<gallery::ThumbnailService> service_;
  double wheel_time_ = 0;
  int wheel_item_ = -1;
  std::string toast_;
  CollageCanvas *canvas_ = nullptr;
  CollagePanel *panel_ = nullptr;
  AppMenu *menu_ = nullptr;
  std::vector<Button> buttons_;
  int hover_button_ = -1;
  int header_h_ = 0;
  friend class CollageCanvas;
  friend class CollagePanel;
};

// Opens the collage editor (or brings it to the front) and adds |paths|.
// With |choose| the file chooser asks for more photos at once.
CollageWindow *open_collage(const std::vector<std::string> &paths, bool choose = false);

}  // namespace photo
