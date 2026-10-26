// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE Gallery: a photo library for the Selviana Desktop Environment.
//
// Gallery organizes: it browses the library by date, folder, album and
// device, keeps favorites and the albums you create, imports from cameras
// and cards, searches, runs slideshows of a set of photos and batch
// actions (rotate, resize and export, copy or move, share). It does not
// edit single photos: opening a photo starts Photo (sde-view), and the
// built-in preview offers "Edit in Photo".
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
#include "album.h"
#include "collections.h"
#include "desktop_entry.h"
#include "editors.h"
#include "library.h"
#include "plugins.h"
#include "media.h"
#include "menu.h"
#include "menubar.h"
#include "line_icons.h"
#include "paint.h"
#include "theme.h"
#include "thumbnails.h"
#include "widgets.h"

namespace gallery {


class GalleryWindow;

// Device and Browse views show photos outside the library (removable
// media, or a folder opened with File > Open Folder) without adding them.
// Album views show the photos of an album the user made (its list file).
enum class ViewKind { All, Favorites, Recent, Screenshots, Folder, Device, Browse, Album };

struct View {
  ViewKind kind = ViewKind::All;
  std::string folder;  // for Folder, Device (mount point), Browse, and the Album's list file
  bool operator==(const View &o) const { return kind == o.kind && folder == o.folder; }
  bool external() const { return kind == ViewKind::Device || kind == ViewKind::Browse; }
  Album album() const;  // the same set as an album id for Photo
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
    bool header = false, add = false, removable = false, device = false, new_album = false, album = false;
    int y = 0, h = 0;
  };
  std::vector<Row> rows() const;
  int row_at(int my) const;
  GalleryWindow *app_;
  int hover_ = -1, focus_ = 0, tip_row_ = -1;
  bool hover_remove_ = false;
  void activate(const Row &row);
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
  void handle_select_all();
  void select_index(int i);
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
  // A slideshow from |index|, through |subset| (indices of the view) or
  // the whole view when empty.
  void start_slideshow(std::vector<int> subset = {});
  bool slideshow() const { return slideshow_; }
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
  bool info_ = true, slideshow_ = false;
  std::vector<int> subset_;  // the slideshow's photos (indices), empty = all
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
  const std::vector<MountEntry> &devices() const { return devices_; }
  bool external_scanning() const { return ext_scanning_; }
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
  void add_folder(const std::string &dir);
  void open_file_dialog();
  void open_folder_dialog();
  void browse(const std::string &dir);
  void import_dialog();
  // Albums.
  const std::vector<UserAlbum> &user_albums() const { return albums_; }
  void reload_albums();
  void new_album(const std::vector<std::string> &add = {});
  void add_to_album(const std::string &file, const std::vector<std::string> &paths);
  void remove_from_album(const std::vector<std::string> &paths);
  void rename_album(const std::string &file);
  void delete_album(const std::string &file);
  sde::ContextMenu album_menu(const std::vector<std::string> &paths);
  // Batch actions on several photos.
  void rotate_files(const std::vector<std::string> &paths, int quarter_turns);
  void export_files(const std::vector<std::string> &paths, int max_edge);
  void copy_files_to(const std::vector<std::string> &paths, bool move);
  void share_files(const std::vector<std::string> &paths);
  void slideshow(const std::vector<std::string> &paths);  // empty: the whole view
  void slideshow_from(const std::string &path);           // its folder or album, from it
  // The menu of actions for photos (right click, the "More" button).
  void photo_menu(int index, const std::vector<std::string> &paths, int x = -1, int y = -1);
  std::vector<std::string> selected_or_focused() const;
  void files_changed(const std::vector<std::string> &paths);  // rewritten on disk
  void import_from(const MountEntry &device, const std::vector<Photo> &only = {});
  void eject(const std::string &mount_point);
  void select_all();
  bool sidebar_shown() const { return show_sidebar_; }
  void toggle_sidebar();
  void toggle_fullscreen();
  void show_about_dialog();
  void show_shortcuts_dialog();
  // Selects |path| (switching views when needed) and raises the window.
  void select_path(const std::string &path);
  // Opens the photo at |index| of the current view: in Photo when the user
  // chose so (View menu), else in the built-in viewer.
  void open_photo(int index);
  bool prefer_photo() const { return prefer_photo_; }
  void remove_folder(const std::string &folder);
  void open_viewer(int index);
  void close_viewer();
  bool viewer_open() const;
  void toast(const std::string &message);
  void rescan();

  // Actions on photos.
  void edit_in_photo(const std::string &path);  // Photo with this view as album
  void preview(int index);                       // the built-in quick preview
  void open_with(const std::string &path);

  // Edit with (tools.cpp): Photo or another installed image editor. The
  // default editor ([gallery] editor, a desktop file ID; "" is Photo) is
  // what Ctrl+E uses. Folders of photos edited elsewhere are watched, so
  // what the other program saves shows up here.
  void edit_with_default(const std::vector<std::string> &paths);
  void edit_with(const std::vector<std::string> &paths, const std::string &app_id);  // "" Photo
  sde::ContextMenu edit_with_menu(const std::vector<std::string> &paths);
  void edit_with_popup(const std::vector<std::string> &paths, int x = -1, int y = -1);
  void edit_with_other(const std::vector<std::string> &paths);
  std::string default_editor_name() const;
  void set_default_editor(const std::string &app_id);
  const std::vector<sde::DesktopEntry> &applications();
  void watch_folder(const std::string &dir);
  void file_events(const std::vector<GalleryEvent> &events);

  // Plugins (tools.cpp): built-in ones that can be switched off, and
  // action plugins (*.plugin files) that run a program on photos.
  bool plugin_on(const std::string &id) const;
  void load_plugins();
  void show_plugins();
  void run_action(size_t index, const std::vector<std::string> &paths);
  void add_action_items(sde::ContextMenu &m, const std::vector<std::string> &paths);
  void create_collage(const std::vector<std::string> &paths);
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
  void build_menu();
  void update_menu();
  void refresh_devices();
  static void mounts_cb(int fd, void *p);
  static void mounts_timer_cb(void *p);
  void start_external_scan(const std::string &root);
  void external_scan_finished();
  static void ext_awake_cb(void *p);
  void handle_events();
  void forget_thumbnails(const std::string &path);
  const std::vector<Photo> &source() const {
    return view_kind_.kind == ViewKind::Album ? album_photos_ : view_kind_.external() ? ext_photos_ : all_;
  }
  LibrarySettings library_settings() const;
  void run_batch(const std::string &busy, std::vector<std::function<bool(std::string *)>> steps,
                 std::function<void(size_t done, size_t failed, const std::string &error)> after);
  static void menu_cb(Fl_Widget *w, void *data);
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
  std::vector<MountEntry> devices_;
  int mounts_fd_ = -1;
  // External scans (Device and Browse views).
  bool ext_scanning_ = false;
  std::string ext_root_, ext_wanted_;
  std::thread ext_thread_;
  std::vector<Photo> ext_photos_, ext_result_;
  std::string ext_result_root_;
  std::string pending_select_;
  bool importing_ = false, batch_running_ = false, pending_slideshow_ = false;
  std::vector<UserAlbum> albums_;
  std::vector<Photo> album_photos_;  // the Album view's photos
  bool show_sidebar_ = true, prefer_photo_ = false;
  MenuBar *menubar_ = nullptr;
  int top_ = 0;  // menu bar height
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
  sde::FlatButton *sel_album_ = nullptr, *sel_more_ = nullptr;
  sde::FlatButton *import_btn_ = nullptr;
  int header_h_ = 0;
  std::string toast_;
  static GalleryWindow *instance_;
  // Edit with and plugins (tools.cpp).
  std::string editor_;  // the default editor's desktop file ID, "" Photo
  std::vector<sde::DesktopEntry> apps_;
  std::string apps_stamp_;
  std::unique_ptr<FolderWatch> watch_;
  std::vector<GalleryEvent> watch_pending_;
  static void watch_cb(int fd, void *p);
  static void watch_timer_cb(void *p);
  std::map<std::string, bool> plugins_;
  std::vector<ActionPlugin> actions_;
  friend class Viewer;
};

}  // namespace gallery
