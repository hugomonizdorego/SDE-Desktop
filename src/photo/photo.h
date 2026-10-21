// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Photo (sde-view): the SDE photo viewer and editor.
//
//   PhotoWindow  the window: header bar, actions, files and saving
//   Canvas       the image: zoom and pan, crop overlay, filmstrip, GIFs
//   SidePanel    photo information, and the editing tools
#pragma once

#include <FL/Fl_Group.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Widget.H>
#include <cairo.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "album.h"
#include "buffered_window.h"
#include "edit.h"
#include "exif.h"
#include "library.h"
#include "line_icons.h"
#include "paint.h"
#include "save.h"
#include "theme.h"
#include "thumbnails.h"
#include "widgets.h"

namespace photo {

class PhotoWindow;

// Serves an image on the X clipboard as image/png (plus the file as
// text/uri-list and its path as text) to other programs.
namespace clipboard {
void copy_image(cairo_surface_t *image, const std::string &path);
}

enum class ZoomMode { Fit, Fill, Actual, Custom };

class Canvas : public Fl_Widget {
 public:
  Canvas(PhotoWindow *app, int x, int y, int w, int h);
  ~Canvas() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;

  // The image shown; |w|,|h| its full size (the image may be a preview).
  void set_image(cairo_surface_t *s, int full_w, int full_h, bool keep_view);
  void clear();
  void set_zoom_mode(ZoomMode m);
  ZoomMode zoom_mode() const { return mode_; }
  void zoom_by(double factor);  // around the centre
  double scale() const;         // screen pixels per image pixel (full size)
  void reset_view() { mode_ = ZoomMode::Fit; pan_x_ = pan_y_ = 0; }
  void set_chrome(bool on) { chrome_ = on; redraw(); }
  bool filmstrip() const { return filmstrip_; }
  void set_filmstrip(bool on);

  // Crop mode: the image shown is uncropped; |r| is the selection.
  void begin_crop(const Rect &r, double aspect);
  void set_crop_aspect(double aspect);  // 0 = free
  void end_crop();
  bool cropping() const { return cropping_; }
  const Rect &crop_rect() const { return crop_; }

  // Animated GIF frames (owned): each frame with its delay in ms.
  void set_animation(std::vector<std::pair<cairo_surface_t *, int>> frames);
  void stop_animation();
  bool animated() const { return !frames_.empty(); }

  // The image area (without the filmstrip).
  void image_area(double &ax, double &ay, double &aw, double &ah) const;
  int filmstrip_height() const;

 private:
  double fit_scale(bool fill) const;
  double target_scale() const;
  void clamp_pan();
  void zoom_at(double new_scale, double cx, double cy, bool animate);
  static void anim_cb(void *p);
  static void frame_cb(void *p);
  void draw_filmstrip(sde::Painter &p);
  int filmstrip_at(int mx, int my) const;
  void image_rect(double &ix, double &iy, double &iw, double &ih) const;
  int crop_handle_at(int mx, int my) const;

  void drop_caches();
  PhotoWindow *app_;
  cairo_surface_t *image_ = nullptr;
  // A screen-sized copy of the image, and the same kept on the X server so
  // that zooming and panning do not upload the photo for every frame.
  cairo_surface_t *mip_ = nullptr, *server_ = nullptr;
  int full_w_ = 0, full_h_ = 0;
  ZoomMode mode_ = ZoomMode::Fit;
  double custom_ = 1, shown_scale_ = 0;  // animated towards target_scale()
  double pan_x_ = 0, pan_y_ = 0;
  bool animating_ = false;
  double anchor_x_ = 0, anchor_y_ = 0;  // zoom anchor (screen)
  bool chrome_ = true, filmstrip_ = true;
  bool dragging_ = false;
  int press_x_ = 0, press_y_ = 0;
  double press_pan_x_ = 0, press_pan_y_ = 0;
  double film_scroll_ = 0;
  int film_hover_ = -1;
  // Crop.
  bool cropping_ = false;
  Rect crop_, press_crop_;
  double aspect_ = 0;
  int crop_drag_ = -1;  // 0-7 handles, 8 move, -1 none
  // Animation.
  std::vector<std::pair<cairo_surface_t *, int>> frames_;
  size_t frame_ = 0;
};

// The panel on the right: photo information, or the editing tools.
class SidePanel : public Fl_Group {
 public:
  enum Mode { Info, Edit };
  enum Tab { Adjust, Filters, Crop };
  SidePanel(PhotoWindow *app, int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void set_mode(Mode m);
  Mode mode() const { return mode_; }
  void set_tab(Tab t);
  Tab tab() const { return tab_; }
  // The edits or the photo changed: update the controls.
  void sync();
  void info_changed() { redraw(); }

 private:
  void layout();
  void draw_info(sde::Painter *p);
  void draw_filters(sde::Painter &p);
  int filter_at(int mx, int my) const;
  void slider_changed(int index);
  void filter_previews();
  PhotoWindow *app_;
  Mode mode_ = Info;
  Tab tab_ = Adjust;
  std::vector<sde::Slider *> sliders_;
  std::vector<std::string> slider_labels_;
  sde::Slider *strength_ = nullptr;
  std::vector<sde::FlatButton *> crop_buttons_, aspect_buttons_;
  Fl_Int_Input *res_w_ = nullptr, *res_h_ = nullptr;
  sde::FlatButton *res_apply_ = nullptr, *reset_ = nullptr, *save_ = nullptr, *save_as_ = nullptr;
  bool res_lock_ = true;
  // Filter previews (rendered from a small copy of the photo).
  std::vector<cairo_surface_t *> previews_;
  cairo_surface_t *preview_source_ = nullptr;
  int filter_hover_ = -1;
  double info_scroll_ = 0;
  // Clickable link in the info view.
  double link_x_ = 0, link_y_ = 0, link_w_ = 0, link_h_ = 0;
  bool gesture_ = false;  // a slider drag in progress
  friend class PhotoWindow;
};

class AppMenu : public Fl_Menu_Button {
 public:
  AppMenu(int x, int y, int w, int h);
  void draw() override;
  int handle(int event) override;

 private:
  bool hover_ = false;
};

class PhotoWindow : public sde::BufferedWindow {
 public:
  PhotoWindow(int w, int h);
  ~PhotoWindow() override;
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;
  void hide() override;

  const sde::Theme &theme() const { return theme_; }
  // Opening.
  void open(const std::string &path, const gallery::Album &album, bool explicit_album,
            gallery::Sort sort);
  void open_dialog();
  void go(int index);
  void step(int delta);
  int index() const { return index_; }
  const std::vector<gallery::Photo> &photos() const { return photos_; }
  const gallery::Photo *current() const;
  const gallery::PhotoInfo &info() const { return info_; }
  int image_w() const { return full_w_; }
  int image_h() const { return full_h_; }
  bool image_loaded() const { return source_ != nullptr; }
  bool load_failed() const { return failed_; }
  cairo_surface_t *thumbnail(const gallery::Photo &p);
  void request_thumbnails(const std::vector<const gallery::Photo *> &wanted);

  // Editing.
  const Edits &edits() const { return history_.current(); }
  void apply(const Edits &e, bool amend = false);
  void undo();
  void redo();
  void reset_edits();
  bool modified() const { return !history_.current().identity(); }
  void rotate(int quarter_turns);
  void flip(bool horizontal);
  void start_crop();
  void apply_crop();
  void cancel_crop();
  void set_crop_aspect(double a);
  cairo_surface_t *preview_base() const { return preview_base_; }

  // File actions.
  bool save(bool ask = true);
  bool save_as();
  void copy_to_clipboard();
  void set_wallpaper();
  void trash();
  void open_with();
  void show_in_files();
  void show_in_gallery();
  void print();
  void toggle_favorite();
  bool is_favorite() const;
  void open_map();

  // View.
  void toggle_fullscreen();
  void toggle_slideshow();
  bool slideshow() const { return slideshow_; }
  void toggle_panel(SidePanel::Mode m);
  bool panel_open() const;
  void toggle_filmstrip();
  void show_shortcuts();
  void show_about();
  void toast(const std::string &message);
  void layout();
  void apply_theme();
  void update_title();
  Canvas *canvas() { return canvas_; }

 private:
  struct Button {
    gallery::icon::Kind glyph;
    std::string tip;
    std::function<void()> action;
    double x = 0, y = 0, w = 0, h = 0;
    bool active = true, toggled = false;
  };
  void build_menu();
  void layout_buttons();
  void load_current();
  void load_album(const gallery::Album &album, gallery::Sort sort, const std::string &path);
  void album_ready();
  void full_loaded(gallery::Result &r);
  void refresh_preview();
  void start_full_render();
  void full_render_done();
  bool confirm_discard();
  cairo_surface_t *render_full();  // caller owns
  void after_save(const std::string &path, SaveResult r);
  void notify_gallery(const std::string &verb, const std::string &path);
  void load_animation(const std::string &path);
  bool keyboard(int key, int state);
  static void awake_cb(void *p);
  static void album_awake_cb(void *p);
  static void slideshow_cb(void *p);
  static void toast_cb(void *p);
  static void render_awake_cb(void *p);
  static int x_event_handler(int event);
  static void menu_cb(Fl_Widget *w, void *data);

  sde::Theme theme_;
  std::vector<gallery::Photo> photos_;
  int index_ = -1;
  std::string path_;  // the photo shown (also before the album is known)
  gallery::Album album_;
  gallery::Sort sort_ = gallery::Sort::Name;
  std::set<std::string> extra_formats_;
  gallery::PhotoInfo info_;
  int full_w_ = 0, full_h_ = 0;
  bool failed_ = false;
  cairo_surface_t *source_ = nullptr;        // decoded original (upright)
  cairo_surface_t *preview_base_ = nullptr;  // source downscaled for live edits
  int source_max_ = 0;                       // max_dim source_ was decoded with
  uint64_t generation_ = 0;
  History history_;
  Edits before_crop_;
  std::unique_ptr<gallery::ThumbnailService> service_;
  std::map<std::string, cairo_surface_t *> thumbs_;
  std::set<std::string> thumb_failed_;
  // Album scanning.
  std::thread album_thread_;
  std::mutex album_mu_;
  std::vector<gallery::Photo> album_result_;
  bool album_loading_ = false;
  // Full-size rendering of the edits for zooming in.
  std::thread render_thread_;
  std::atomic<uint64_t> render_gen_{0};
  cairo_surface_t *render_result_ = nullptr;
  uint64_t render_result_gen_ = 0;
  bool slideshow_ = false, was_fullscreen_ = false;
  double slideshow_interval_ = 5;
  int jpeg_quality_ = 92;
  std::string toast_;

  Canvas *canvas_ = nullptr;
  SidePanel *panel_ = nullptr;
  AppMenu *menu_ = nullptr;
  std::vector<Button> buttons_;
  int hover_button_ = -1;
  int header_h_ = 0;
  static PhotoWindow *instance_;
  friend class Canvas;
  friend class SidePanel;
};

}  // namespace photo
