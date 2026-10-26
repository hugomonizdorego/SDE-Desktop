// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Photo (sde-view): the SDE photo viewer and editor.
//
//   PhotoWindow  the window: header bar, actions, files and saving
//   Canvas       the image: zoom and pan, crop overlay, drawing, filmstrip, GIFs
//   SidePanel    photo information, and the editing tools (Adjust, Filters,
//                Crop, Draw, and Effects when its plugins are on)
//
// Photo views and edits one image at a time; the library (albums,
// favorites, slideshows, import, batch actions) is Gallery's job.
#pragma once

#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
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
#include "plugins.h"
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

// Drawing tools (the Draw tab).
enum class Tool { Select, Pen, Highlighter, Eraser, Line, Arrow, Rect, Ellipse, Text, Blur, Pixelate, Picker };
constexpr int kToolCount = 12;

struct DrawSettings {
  Tool tool = Tool::Pen;
  uint32_t color = 0xFFE5484D;
  // Per tool: the stroke width (block size for Pixelate, blur radius for
  // Blur, eraser size) in screen pixels at the zoom level when drawing,
  // and the opacity in percent.
  double width[kToolCount] = {4, 4, 22, 24, 4, 5, 4, 4, 4, 14, 14, 4};
  double opacity[kToolCount] = {100, 100, 40, 100, 100, 100, 100, 100, 100, 100, 100, 100};
  bool fill = false;
  FontFace font = FontFace::Sans;
  double text_size = 28;  // screen pixels
  bool text_box = false;
  double tool_width() const { return width[int(tool)]; }
  double tool_opacity() const { return opacity[int(tool)] / 100.0; }
};

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

  // Drawing (the Draw tab is open). The selected mark is an index into
  // the marks of the current edits, -1 none.
  int selected_mark() const { return selected_; }
  void select_mark(int i);
  void cancel_drawing();  // drops a stroke in progress and the selection
  bool drawing_busy() const { return live_on_ || mark_drag_ >= 0; }

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
  // Drawing (drawing.cpp).
  ViewMap screen_map() const;  // image coordinates to the screen
  int draw_event(int event, int mx, int my);
  void draw_overlay(sde::Painter &p);
  // The Effects tab (effects.cpp): clicks choose the sharp point of the
  // background blur, or fix red eyes.
  int effects_event(int event, int mx, int my);
  void draw_effects_overlay(sde::Painter &p);
  bool content_rect(double &x, double &y, double &w, double &h) const;  // the photo inside a frame
  void set_focus_point(int mx, int my, bool amend);
  void add_red_eye(int mx, int my);
  bool focus_drag_ = false;
  void commit_live();
  int handle_at(int mx, int my) const;  // corner of the selection box, -1
  Mark style_mark(MarkKind kind) const;  // a new mark with the panel's settings
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
  // Drawing.
  Mark live_;              // the stroke or shape being drawn
  bool live_on_ = false;
  int selected_ = -1;
  int mark_drag_ = -1;     // 0-3 corners, 4 move, -1 none
  Marks drag_marks_;       // the marks while a move or resize is dragged
  Box press_box_;
  Pt press_img_;
  bool erasing_ = false, erased_ = false;
  bool hover_in_ = false;  // pointer over the image (eraser outline)
  int hover_x_ = 0, hover_y_ = 0;
  bool middle_pan_ = false;
};

// The panel on the right: photo information, or the editing tools.
class SidePanel : public Fl_Group {
 public:
  enum Mode { Info, Edit };
  enum Tab { Adjust, Filters, Crop, Draw, Effects };
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
  // The Effects tab is shown when one of its plugins is on.
  bool effects_available() const;
  int tab_count() const { return effects_available() ? 5 : 4; }

  void layout();

 private:
  void draw_info(sde::Painter *p);
  void draw_filters(sde::Painter &p);
  int filter_at(int mx, int my) const;
  void slider_changed(int index);
  void filter_previews();
  // The Draw tab (custom drawn; see panels.cpp).
  void layout_draw(int px, int pw, int top);
  void draw_draw_tab(sde::Painter *p);
  int draw_tab_click(int mx, int my);  // 1 when handled
  void draw_slider_changed(int which);
  struct HitBox {
    double x = 0, y = 0, w = 0, h = 0;
    int id = 0;
    bool in(int mx, int my) const { return mx >= x && mx < x + w && my >= y && my < y + h; }
  };
  std::vector<HitBox> draw_boxes_;
  sde::Slider *draw_width_ = nullptr, *draw_opacity_ = nullptr;
  double draw_scroll_ = 0, draw_h_ = 0;
  double draw_tool_label_y_ = 0, draw_colour_label_y_ = 0;
  // Short panels (small screens, large scale) use tighter rows and no
  // footer note; the tab scrolls when it still does not fit.
  bool draw_compact() const { return h() < sde::S(600); }
  int draw_top() const { return y() + sde::S(56); }
  int draw_bottom() const { return y() + h() - (draw_compact() ? sde::S(68) : sde::S(96)); }
  int draw_hover_ = -1;
  // The Effects tab (effects.cpp), in the same area as the Draw tab.
  void layout_effects(int px, int pw, int top);
  void draw_effects_tab(sde::Painter *p);
  void draw_effects_hint();
  int effects_click(int mx, int my);  // 1 when handled
  void effects_slider_changed();
  void watermark_typed();
  struct FxLabel {
    double y = 0;
    const char *text = nullptr;  // untranslated
    int kind = 0;                // 0 section title, 1 slider label, 2 note
    sde::Slider *slider = nullptr;
  };
  std::vector<HitBox> fx_boxes_;
  std::vector<FxLabel> fx_labels_;
  sde::Slider *fx_blur_ = nullptr, *fx_focus_ = nullptr, *fx_frame_ = nullptr, *fx_wm_size_ = nullptr,
              *fx_wm_alpha_ = nullptr;
  Fl_Input *fx_text_ = nullptr;
  double fx_scroll_ = 0, fx_h_ = 0;
  int fx_hover_ = -1;
  bool fx_typing_ = false;
  // Auto Enhance (Adjust tab), size presets (Crop tab) and the actions of
  // Metadata & Privacy in the info view.
  sde::FlatButton *auto_ = nullptr, *presets_ = nullptr;
  double adjust_scroll_ = 0, adjust_h_ = 0;
  std::vector<HitBox> info_boxes_;
  int info_hover_ = -1;
  PhotoWindow *app_;
  Mode mode_ = Info;
  Tab tab_ = Adjust;
  std::vector<sde::Slider *> sliders_;
  std::vector<std::string> slider_labels_;
  sde::Slider *strength_ = nullptr;
  std::vector<sde::FlatButton *> crop_buttons_, aspect_buttons_;
  // The crop buttons' actions (their callback data), freed with the panel.
  std::vector<std::unique_ptr<std::function<void(PhotoWindow *)>>> crop_fns_;
  Fl_Int_Input *res_w_ = nullptr, *res_h_ = nullptr;
  sde::FlatButton *res_apply_ = nullptr, *reset_ = nullptr, *save_ = nullptr, *save_as_ = nullptr;
  bool res_lock_ = true;
  // Filter previews (rendered from a small copy of the photo).
  std::vector<cairo_surface_t *> previews_;
  cairo_surface_t *preview_source_ = nullptr;
  int filter_hover_ = -1;
  double info_scroll_ = 0, info_h_ = 0;
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
  static PhotoWindow *instance() { return instance_; }
  // Shows |path| (with its folder) in the Photo window, which is made when
  // there is none yet (e.g. after "Open in Photo" in the collage editor).
  static void show_file(const std::string &path);
  // Opening.
  void open(const std::string &path, const gallery::Album &album, bool explicit_album,
            gallery::Sort sort);
  void new_collage();
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
  // Drawing.
  bool drawing() const;  // the Draw tab is shown
  DrawSettings &draw_settings() { return draw_; }
  void set_tool(Tool t);
  void open_draw();
  void open_draw_when_loaded() { draw_on_load_ = true; if (source_) open_draw(); }
  void delete_selected_mark();
  // The panel's style changed: restyle the selected mark (one undo step
  // per slider drag when |amend|).
  void style_changed(bool amend);
  void edit_text_mark(int index);
  // The oriented, uncropped full size marks refer to.
  void mark_reference(int &w, int &h) const;
  // Edits with the marks set up for drawing (reference size known).
  Edits edits_for_marks() const;
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
  void open_map();

  // View.
  void toggle_fullscreen();
  void slideshow_in_gallery();
  void context_menu();  // right click on the photo
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

  // Plugins (tools.cpp): built-in ones that can be switched off, and
  // action plugins (*.plugin files) that run a program on the photo.
  bool plugin_on(const std::string &id) const;
  void load_plugins();
  void show_plugins();
  void run_action(size_t index);
  // The editing plugins (effects.cpp).
  bool effects_open() const;  // the Effects tab is shown
  void open_effects();
  bool red_eye_mode() const { return red_eye_mode_; }
  void set_red_eye_mode(bool on);
  void auto_enhance();
  void show_size_presets(Fl_Widget *anchor);
  void apply_size_preset(int index);
  // Metadata & Privacy: on the saved file, without encoding it again.
  void remove_location();
  void save_without_details();
  void show_all_details();
  bool has_removable_details();  // the saved file has metadata to remove
  std::string watermark_text() const;  // the last one used
  void remember_watermark(const std::string &text);

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
  int jpeg_quality_ = 92;
  DrawSettings draw_;
  bool draw_on_load_ = false;
  std::string toast_;
  std::map<std::string, bool> plugins_;         // built-in plugin id -> on
  std::vector<gallery::ActionPlugin> actions_;  // action plugins that are on
  bool red_eye_mode_ = false;                   // clicks on the photo fix red eyes
  std::string details_path_;                    // has_removable_details(), cached
  time_t details_mtime_ = 0;
  bool details_removable_ = false;
  std::string watermark_;                       // the last watermark text
  static void save_watermark_cb(void *p);

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
