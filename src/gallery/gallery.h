// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Gallery: a photo library for the Selviana Desktop Environment.
#pragma once

#include <FL/Fl_Input.H>
#include <FL/Fl_Widget.H>
#include <cairo.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "buffered_window.h"
#include "library.h"
#include "paint.h"
#include "theme.h"
#include "thumbnails.h"
#include "widgets.h"

namespace gallery {

// Vector icons in the SDE line style, drawn in a size x size box at (x, y).
namespace icon {
enum Kind {
  Photos, Heart, HeartFilled, Clock, Screenshot, Folder, Plus, Close, Search, Back, Prev, Next,
  ZoomIn, ZoomOut, Fit, Rotate, Play, Pause, Info, Trash, Edit, OpenWith, Wallpaper, Files, Copy,
  Picture, Broken, Check, Small, Large
};
void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c);
}  // namespace icon

class GalleryWindow;

enum class ViewKind { All, Favorites, Recent, Screenshots, Folder };

struct View {
  ViewKind kind = ViewKind::All;
  std::string folder;  // for ViewKind::Folder
  bool operator==(const View &o) const { return kind == o.kind && folder == o.folder; }
};

class Sidebar : public Fl_Widget {
 public:
  Sidebar(GalleryWindow *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  bool compact() const { return w() < sde::S(120); }

 private:
  struct Row {
    std::string label;
    icon::Kind glyph = icon::Photos;
    size_t count = 0;
    View view;
    bool header = false, add = false, removable = false;
    int y = 0, h = 0;
  };
  std::vector<Row> rows() const;
  int row_at(int my) const;
  GalleryWindow *app_;
  int hover_ = -1, focus_ = 0, tip_row_ = -1;
  bool hover_remove_ = false;
  std::string tip_;
};

class Segmented : public Fl_Widget {
 public:
  Segmented(int x, int y, int w, int h, std::vector<std::string> labels);
  void draw() override;
  int handle(int event) override;
  int value() const { return value_; }
  void value(int v) {
    value_ = v;
    redraw();
  }

 private:
  std::vector<std::string> labels_;
  int value_ = 0, hover_ = -1;
};

class Grid : public Fl_Widget {
 public:
  Grid(GalleryWindow *app, int x, int y, int w, int h);
  ~Grid() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;

  void reset();                  // new view: clear selection, scroll to top
  void relayout();               // tile size or content changed
  void ensure_visible(int index);
  int focus_index() const { return focus_; }
  void set_focus_index(int i);
  std::vector<size_t> selection() const;
  void clear_selection();
  void scroll_to_top() { target_ = scroll_ = 0; }

 private:
  struct Row {
    size_t first = 0, count = 0;
    double y = 0;
  };
  struct Header {
    std::string label;
    size_t count = 0;
    double y = 0;
    size_t first_row = 0;
  };
  void layout_items();
  bool tile_rect(size_t index, double &tx, double &ty, double &ts) const;
  int item_at(int mx, int my) const;
  size_t row_of(size_t index) const;
  void move_focus(int index, bool extend, bool toggle);
  void select_only(int index);
  double max_scroll() const;
  void animate();
  static void animate_cb(void *p);
  void request_visible();
  void open_menu(int index);
  bool keyboard(int key, int state);

  GalleryWindow *app_;
  std::vector<Row> rows_;
  std::vector<Header> headers_;
  std::vector<size_t> item_row_;
  std::vector<char> selected_;
  double tile_ = 0, gap_ = 0, pad_ = 0, header_h_ = 0, content_h_ = 0;
  int cols_ = 1;
  double scroll_ = 0, target_ = 0;
  bool animating_ = false;
  int hover_ = -1, focus_ = -1, anchor_ = -1;
  bool keyboard_used_ = false;
  bool drag_bar_ = false;
  double drag_offset_ = 0;
  int bar_hover_ = 0;
};

class Viewer : public Fl_Widget {
 public:
  Viewer(GalleryWindow *app, int x, int y, int w, int h);
  ~Viewer() override;
  void draw() override;
  int handle(int event) override;

  void open(int index);
  void close();
  int index() const { return index_; }
  void full_loaded(const Result &r);
  void stop_slideshow();
  void photos_changed();  // the view list changed under the viewer

 private:
  struct Button {
    icon::Kind glyph = icon::Photos;
    std::string tip;
    std::function<void()> action;
    double x = 0, y = 0, w = 0, h = 0;
    bool active = true, toggled = false;
    std::string text;  // shown next to the icon in the info panel
  };
  void layout_buttons();
  void image_area(double &ax, double &ay, double &aw, double &ah) const;
  double fit_scale() const;
  double current_scale() const;
  void set_zoom(double scale, double cx, double cy);
  void step(int delta);
  void load_current();
  void toggle_slideshow();
  static void slideshow_cb(void *p);
  bool keyboard(int key, int state);
  void draw_info(sde::Painter &p, double x, double y, double w, double h);
  const Photo *photo() const;

  GalleryWindow *app_;
  int index_ = -1;
  cairo_surface_t *full_ = nullptr;
  std::string full_path_;
  uint64_t generation_ = 0;
  int img_w_ = 0, img_h_ = 0;
  bool failed_ = false;
  double zoom_ = 0;  // 0 = fit
  double pan_x_ = 0, pan_y_ = 0;
  int rotation_ = 0;  // quarter turns clockwise
  bool info_ = true, slideshow_ = false;
  bool panning_ = false;
  int press_x_ = 0, press_y_ = 0;
  double press_pan_x_ = 0, press_pan_y_ = 0;
  std::vector<Button> top_, actions_;
  int hover_button_ = -1;  // index into top_ (or 1000 + index into actions_)
  int hover_side_ = 0;     // -1 prev, 1 next
};

class GalleryWindow : public sde::BufferedWindow {
 public:
  GalleryWindow(int w, int h);
  ~GalleryWindow() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;

  const sde::Theme &theme() const { return theme_; }
  const std::vector<Photo> &photos() const { return view_; }
  const std::vector<Group> &groups() const { return groups_; }
  const std::vector<Photo> &library() const { return all_; }
  const View &view() const { return view_kind_; }
  void set_view(const View &v);
  Sort sort() const { return sort_; }
  int zoom() const { return zoom_; }
  void set_zoom(int z);
  bool scanning() const { return scanning_; }
  const std::string &pictures() const { return pictures_; }
  const std::vector<std::string> &user_folders() const { return user_folders_; }
  std::string screenshots_dir() const { return pictures_ + "/Screenshots"; }
  size_t count_for(const View &v) const;
  std::string view_title(const View &v) const;
  bool searching() const;

  // Thumbnails: returns the best cached surface (or nullptr) and queues
  // the wanted size. |w|,|h| receive the image's original size if known.
  cairo_surface_t *thumbnail(const Photo &p, bool large, bool *failed = nullptr);
  void request_thumbnails(const std::vector<std::pair<const Photo *, bool>> &wanted);
  void dimensions(const std::string &path, int &w, int &h) const;
  ThumbnailService &service() { return *service_; }

  bool is_favorite(const std::string &path) const { return favorites_.count(path) != 0; }
  void toggle_favorite(const std::vector<std::string> &paths);
  void add_folder();
  void remove_folder(const std::string &folder);
  void open_viewer(int index);
  void close_viewer();
  bool viewer_open() const;
  void toast(const std::string &message);
  void rescan();

  // Actions on photos.
  void edit_in_photo(const std::string &path);
  void open_with(const std::string &path);
  void set_wallpaper(const std::string &path);
  void show_in_files(const std::string &path);
  void copy_paths(const std::vector<std::string> &paths);
  void trash(const std::vector<std::string> &paths);

  void layout();
  void apply_theme();
  void reload_config();
  void focus_search();

 private:
  void rebuild_view(bool keep_scroll = false);
  void start_scan();
  void scan_finished();
  void results_ready();
  void update_header_widgets();
  void save_settings();
  static void awake_cb(void *p);
  static void scan_awake_cb(void *p);
  static void toast_cb(void *p);
  static void save_cb(void *p);
  static int x_event_handler(int event);
  void draw_header(sde::Painter &p);
  void draw_header_text();

  sde::Theme theme_;
  std::string pictures_;
  std::vector<std::string> user_folders_;
  std::set<std::string> favorites_;
  std::set<std::string> extra_formats_;
  Sort sort_ = Sort::Newest;
  int zoom_ = 168;
  View view_kind_;

  std::vector<Photo> all_, view_;
  std::vector<Group> groups_;

  bool scanning_ = false, rescan_wanted_ = false;
  std::thread scan_thread_;
  std::mutex scan_mu_;
  std::vector<Photo> scan_result_;

  std::unique_ptr<ThumbnailService> service_;
  struct Thumb {
    cairo_surface_t *surface = nullptr;
    unsigned long long used = 0;
  };
  std::map<std::string, Thumb> thumbs_;  // key: path + "\n" + class
  std::set<std::string> failed_;
  std::map<std::string, std::pair<int, int>> dims_;
  mutable std::map<std::string, size_t> counts_;
  unsigned long long frame_ = 0;

  Sidebar *sidebar_ = nullptr;
  Grid *grid_ = nullptr;
  Viewer *viewer_ = nullptr;
  Fl_Input *search_ = nullptr;
  Segmented *sort_ctl_ = nullptr;
  sde::Slider *zoom_ctl_ = nullptr;
  sde::FlatButton *sel_fav_ = nullptr, *sel_trash_ = nullptr, *sel_clear_ = nullptr;
  int header_h_ = 0;
  std::string toast_;
  static GalleryWindow *instance_;
};

}  // namespace gallery
