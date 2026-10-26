// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Photo window: opening and navigating photos, the header bar and its
// menu, editing, saving and the actions on the file.
#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <sys/stat.h>
#include <unistd.h>
#ifdef SDE_GALLERY_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

#include "build_config.h"
#include "collage_ui.h"
#include "config.h"
#include "desktop_entry.h"
#include "i18n.h"
#include "link.h"
#include "menu.h"
#include "menubar.h"
#include "photo.h"
#include "process.h"
#include "spawn.h"
#include "util.h"
#include "wm.h"
#include "x11.h"

using namespace sde;
namespace icon = gallery::icon;
using gallery::Album;
using gallery::AlbumKind;
using gallery::Photo;

namespace photo {

using gallery::ask;

PhotoWindow *PhotoWindow::instance_ = nullptr;

namespace {

enum MenuId {
  M_OPEN = 1, M_SAVE, M_SAVE_AS, M_COPY, M_WALLPAPER, M_PRINT, M_OPEN_WITH, M_SHOW_FILES, M_SHOW_GALLERY,
  M_TRASH, M_CLOSE, M_UNDO, M_REDO, M_ROT_LEFT, M_ROT_RIGHT, M_FLIP_H, M_FLIP_V, M_CROP, M_RESIZE,
  M_ADJUST, M_FILTERS, M_DRAW, M_DELETE_MARK, M_RESET, M_ZOOM_IN, M_ZOOM_OUT, M_FIT, M_ACTUAL, M_FILL, M_INFO,
  M_FILMSTRIP, M_FULLSCREEN, M_SLIDESHOW, M_SHORTCUTS, M_ABOUT, M_NEW_COLLAGE, M_PLUGINS,
  M_ACTION = 1000  // + the index of an action plugin
};

// The Photo icon (see sdeview.svg).
void draw_logo(Painter &p, double x, double y, double s) {
  cairo_t *cr = p.cr();
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + s, y + s);
  cairo_pattern_add_color_stop_rgb(g, 0, 0xF5 / 255.0, 0x8A / 255.0, 0x4C / 255.0);
  cairo_pattern_add_color_stop_rgb(g, 1, 0xD9 / 255.0, 0x3A / 255.0, 0x6B / 255.0);
  p.rounded_rect(x, y, s, s, s * 0.26);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  double fx = x + s * 0.18, fy = y + s * 0.24, fw = s * 0.64, fh = s * 0.52;
  p.fill_rounded(fx, fy, fw, fh, s * 0.07, Color::hex(0xFFFFFF));
  cairo_save(cr);
  p.rounded_rect(fx + s * 0.04, fy + s * 0.04, fw - s * 0.08, fh - s * 0.08, s * 0.04);
  cairo_clip(cr);
  p.fill_rect(fx, fy, fw, fh, Color::hex(0xFFE3CF));
  p.set(Color::hex(0xD94F5C));
  cairo_move_to(cr, fx, fy + fh);
  cairo_line_to(cr, fx + fw * 0.38, fy + fh * 0.45);
  cairo_line_to(cr, fx + fw * 0.62, fy + fh * 0.75);
  cairo_line_to(cr, fx + fw * 0.76, fy + fh * 0.6);
  cairo_line_to(cr, fx + fw, fy + fh * 0.9);
  cairo_line_to(cr, fx + fw, fy + fh);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_restore(cr);
  p.circle(fx + fw * 0.74, fy + fh * 0.32, s * 0.065, Color::hex(0xF2B84B));
}

struct RenderJob {
  PhotoWindow *self;
  uint64_t gen;
  cairo_surface_t *result;
};

}  // namespace

// ------------------------------------------------------------------ AppMenu

AppMenu::AppMenu(int x, int y, int w, int h) : Fl_Menu_Button(x, y, w, h) {
  box(FL_NO_BOX);
  tooltip(_("Menu"));
}

void AppMenu::draw() {
  const Theme &t = active_theme();
  Painter p(window()->w(), window()->h());
  if (hover_) p.fill_rounded(x(), y(), w(), h(), Sd(10), t.hover);
  icon::draw(p, icon::Menu, x() + (w() - S(22)) / 2.0, y() + (h() - S(22)) / 2.0, S(22), t.text);
}

int AppMenu::handle(int event) {
  if (event == FL_ENTER || event == FL_LEAVE) {
    hover_ = event == FL_ENTER;
    redraw();
    return 1;
  }
  if (event == FL_PUSH) {
    const Fl_Menu_Item *m = menu()->pulldown(x() + w() - S(240), y() + h(), 0, 0, nullptr, this);
    hover_ = false;
    redraw();
    picked(m);
    return 1;
  }
  return Fl_Menu_Button::handle(event);
}

// --------------------------------------------------------------- the window

PhotoWindow::PhotoWindow(int w, int h) : BufferedWindow(w, h, _("Photo")) {
  instance_ = this;
  theme_ = active_theme();
  extra_formats_ = gallery::extra_formats();
  Config cfg;
  cfg.load();
  jpeg_quality_ = std::max(30, std::min(100, cfg.get_int("photo", "jpeg_quality", 92)));
  // The drawing tools remember their colour and options.
  draw_.color = uint32_t(std::stoul(cfg.get("photo", "draw_color", "FFE5484D"), nullptr, 16));
  draw_.fill = cfg.get_bool("photo", "draw_fill", false);
  draw_.text_box = cfg.get_bool("photo", "draw_text_box", false);
  draw_.text_size = std::max(8, std::min(160, cfg.get_int("photo", "draw_text_size", 28)));
  int font = cfg.get_int("photo", "draw_font", 0);
  draw_.font = font == 1 ? FontFace::Serif : font == 2 ? FontFace::Mono : FontFace::Sans;
  service_.reset(new gallery::ThumbnailService([this] { Fl::awake(awake_cb, this); }, 2));
  begin();
  canvas_ = new Canvas(this, 0, 0, w, h);
  canvas_->set_filmstrip(cfg.get_bool("photo", "filmstrip", true));
  panel_ = new SidePanel(this, w - S(320), 0, S(320), h);
  if (!cfg.get_bool("photo", "info", w >= S(1000))) panel_->hide();
  menu_ = new AppMenu(0, 0, S(40), S(40));
  end();
  resizable(nullptr);
  size_range(S(480), S(360));
  xclass("Sdeview");
  load_plugins();
  build_menu();
  apply_theme();
  layout();
  Fl::add_handler(x_event_handler);
}

PhotoWindow::~PhotoWindow() {
  Fl::remove_handler(x_event_handler);
  Fl::remove_timeout(toast_cb, this);
  instance_ = nullptr;
  if (album_thread_.joinable()) album_thread_.join();
  service_.reset();
  for (auto &kv : thumbs_) cairo_surface_destroy(kv.second);
  if (source_) cairo_surface_destroy(source_);
  if (preview_base_) cairo_surface_destroy(preview_base_);
  if (render_result_) cairo_surface_destroy(render_result_);
}

void PhotoWindow::hide() {
  if (modified() && !confirm_discard()) return;
  Config cfg;
  cfg.load();
  cfg.set("photo", "info", panel_->visible() && panel_->mode() == SidePanel::Info ? "true" : "false");
  cfg.set("photo", "filmstrip", canvas_->filmstrip() ? "true" : "false");
  char color[16];
  snprintf(color, sizeof color, "%08X", unsigned(draw_.color));
  cfg.set("photo", "draw_color", color);
  cfg.set("photo", "draw_fill", draw_.fill ? "true" : "false");
  cfg.set("photo", "draw_text_box", draw_.text_box ? "true" : "false");
  cfg.set("photo", "draw_text_size", std::to_string(int(draw_.text_size)));
  cfg.set("photo", "draw_font", std::to_string(int(draw_.font)));
  cfg.save();
  BufferedWindow::hide();
}

void PhotoWindow::apply_theme() {
  Config cfg;
  cfg.load();
  double scale = ui_scale();
  theme_ = Theme::from_config(cfg);
  set_ui_scale(scale);
  theme_.scale = scale;
  theme_.apply_to_fltk();
  color(theme_.background.fl());
  menu_->color(theme_.surface.fl());
  menu_->selection_color(theme_.accent.fl());
  menu_->textcolor(theme_.text.fl());
  menu_->textsize(theme_.font_size);
  if (panel_) panel_->layout();
  redraw();
}

int PhotoWindow::x_event_handler(int) {
  if (!instance_ || !fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL")) {
    instance_->apply_theme();
    instance_->layout();
  }
  return 0;
}

// ----------------------------------------------------------------- menu

void PhotoWindow::build_menu() {
  menu_->clear();  // rebuilt when plugins are switched on or off
  auto add = [this](const std::string &label, int sc, long id, int flags = 0) {
    std::string l = label;
    menu_->add(l.c_str(), sc, menu_cb, reinterpret_cast<void *>(id), flags);
  };
  std::string f = std::string(_("File")) + "/", e = std::string(_("Edit")) + "/", v = std::string(_("View")) + "/",
              h = std::string(_("Help")) + "/";
  add(f + _("Open…"), FL_CTRL + 'o', M_OPEN, plugin_on("collage") ? 0 : FL_MENU_DIVIDER);
  if (plugin_on("collage")) add(f + _("New Collage…"), FL_CTRL + 'n', M_NEW_COLLAGE, FL_MENU_DIVIDER);
  add(f + _("Save"), FL_CTRL + 's', M_SAVE);
  add(f + _("Save As…"), FL_CTRL + FL_SHIFT + 's', M_SAVE_AS);
  add(f + _("Copy Image"), FL_CTRL + 'c', M_COPY, FL_MENU_DIVIDER);
  add(f + _("Set as Wallpaper"), 0, M_WALLPAPER);
  add(f + _("Print…"), FL_CTRL + 'p', M_PRINT, FL_MENU_DIVIDER);
  add(f + _("Open With…"), 0, M_OPEN_WITH);
  add(f + _("Show in Files"), 0, M_SHOW_FILES);
  add(f + _("Show in Gallery"), FL_CTRL + 'g', M_SHOW_GALLERY, FL_MENU_DIVIDER);
  add(f + _("Move to Trash…"), 0, M_TRASH, FL_MENU_DIVIDER);
  add(f + _("Close"), FL_CTRL + 'w', M_CLOSE);
  add(e + _("Undo"), FL_CTRL + 'z', M_UNDO);
  add(e + _("Redo"), FL_CTRL + FL_SHIFT + 'z', M_REDO, FL_MENU_DIVIDER);
  add(e + _("Rotate Left"), FL_SHIFT + 'r', M_ROT_LEFT);
  add(e + _("Rotate Right"), 'r', M_ROT_RIGHT);
  add(e + _("Flip Horizontally"), 'h', M_FLIP_H);
  add(e + _("Flip Vertically"), 'v', M_FLIP_V, FL_MENU_DIVIDER);
  add(e + _("Crop…"), 'c', M_CROP);
  add(e + _("Resize…"), 0, M_RESIZE);
  add(e + _("Adjust Colours…"), 'e', M_ADJUST);
  add(e + _("Filters…"), 0, M_FILTERS);
  add(e + _("Draw and Annotate…"), 'd', M_DRAW);
  add(e + _("Delete Annotation"), 0, M_DELETE_MARK, FL_MENU_DIVIDER);
  add(e + _("Revert to Original"), 0, M_RESET);
  add(v + _("Zoom In"), '+', M_ZOOM_IN);
  add(v + _("Zoom Out"), '-', M_ZOOM_OUT);
  add(v + _("Best Fit"), '0', M_FIT);
  add(v + _("Actual Size"), '1', M_ACTUAL);
  add(v + _("Fill Window"), 'f', M_FILL, FL_MENU_DIVIDER);
  add(v + _("Information"), 'i', M_INFO, FL_MENU_TOGGLE);
  add(v + _("Filmstrip"), 'b', M_FILMSTRIP, FL_MENU_TOGGLE | FL_MENU_DIVIDER);
  add(v + _("Full Screen"), FL_F + 11, M_FULLSCREEN, FL_MENU_TOGGLE);
  add(v + _("Slideshow in Gallery"), FL_F + 5, M_SLIDESHOW);
  // Tools: what the plugins add, then the action plugins.
  std::string tl = std::string(_("Tools")) + "/";
  if (plugin_on("collage")) add(tl + _("New Collage…"), 0, M_NEW_COLLAGE, FL_MENU_DIVIDER);
  for (size_t i = 0; i < actions_.size(); ++i) {
    std::string name = actions_[i].name;
    for (auto &ch : name)
      if (ch == '/') ch = '|';  // a slash would start a submenu
    add(tl + name, 0, M_ACTION + long(i), i + 1 == actions_.size() ? FL_MENU_DIVIDER : 0);
  }
  add(tl + _("Plugins…"), 0, M_PLUGINS);
  add(h + _("Keyboard Shortcuts"), FL_F + 1, M_SHORTCUTS);
  add(h + _("About Photo"), 0, M_ABOUT);
}

void PhotoWindow::menu_cb(Fl_Widget *, void *data) {
  PhotoWindow *self = instance_;
  if (!self) return;
  switch (reinterpret_cast<long>(data)) {
    case M_OPEN: self->open_dialog(); break;
    case M_NEW_COLLAGE: self->new_collage(); break;
    case M_SAVE: self->save(); break;
    case M_SAVE_AS: self->save_as(); break;
    case M_COPY: self->copy_to_clipboard(); break;
    case M_WALLPAPER: self->set_wallpaper(); break;
    case M_PRINT: self->print(); break;
    case M_OPEN_WITH: self->open_with(); break;
    case M_SHOW_FILES: self->show_in_files(); break;
    case M_SHOW_GALLERY: self->show_in_gallery(); break;
    case M_TRASH: self->trash(); break;
    case M_CLOSE: self->hide(); break;
    case M_UNDO: self->undo(); break;
    case M_REDO: self->redo(); break;
    case M_ROT_LEFT: self->rotate(-1); break;
    case M_ROT_RIGHT: self->rotate(1); break;
    case M_FLIP_H: self->flip(true); break;
    case M_FLIP_V: self->flip(false); break;
    case M_CROP: self->start_crop(); break;
    case M_RESIZE:
      self->toggle_panel(SidePanel::Edit);
      self->panel_->set_mode(SidePanel::Edit);
      self->panel_->set_tab(SidePanel::Crop);
      if (!self->panel_->visible()) self->toggle_panel(SidePanel::Edit);
      Fl::focus(self->panel_->res_w_);
      break;
    case M_ADJUST:
    case M_FILTERS:
      if (!self->panel_->visible() || self->panel_->mode() != SidePanel::Edit) self->toggle_panel(SidePanel::Edit);
      self->panel_->set_tab(reinterpret_cast<long>(data) == M_ADJUST ? SidePanel::Adjust : SidePanel::Filters);
      break;
    case M_DRAW: self->open_draw(); break;
    case M_DELETE_MARK: self->delete_selected_mark(); break;
    case M_RESET: self->reset_edits(); break;
    case M_ZOOM_IN: self->canvas_->zoom_by(1.25); break;
    case M_ZOOM_OUT: self->canvas_->zoom_by(1 / 1.25); break;
    case M_FIT: self->canvas_->set_zoom_mode(ZoomMode::Fit); break;
    case M_ACTUAL: self->canvas_->set_zoom_mode(ZoomMode::Actual); break;
    case M_FILL: self->canvas_->set_zoom_mode(ZoomMode::Fill); break;
    case M_INFO: self->toggle_panel(SidePanel::Info); break;
    case M_FILMSTRIP: self->toggle_filmstrip(); break;
    case M_FULLSCREEN: self->toggle_fullscreen(); break;
    case M_SLIDESHOW: self->slideshow_in_gallery(); break;
    case M_SHORTCUTS: self->show_shortcuts(); break;
    case M_ABOUT: self->show_about(); break;
    case M_PLUGINS: self->show_plugins(); break;
    default:
      if (reinterpret_cast<long>(data) >= M_ACTION) self->run_action(size_t(reinterpret_cast<long>(data) - M_ACTION));
      break;
  }
  self->redraw();
}

// ------------------------------------------------------------- opening

const Photo *PhotoWindow::current() const {
  if (index_ < 0 || index_ >= int(photos_.size())) return nullptr;
  return &photos_[size_t(index_)];
}

void PhotoWindow::open(const std::string &path_in, const Album &album, bool explicit_album, gallery::Sort sort) {
  std::string path = path_in;
  album_ = album;
  sort_ = sort;
  path_ = path;
  photos_ = gallery::photos_for_paths({path});
  index_ = photos_.empty() ? -1 : 0;
  if (album.kind == AlbumKind::Directory) {
    // A plain folder is quick to list: do it now.
    auto list = gallery::scan_directory(album.path, extra_formats_);
    gallery::sort_photos(list, sort);
    if (gallery::index_of(list, path) < 0 && !photos_.empty()) list.insert(list.begin(), photos_[0]);
    photos_ = list;
    index_ = gallery::index_of(photos_, path);
  } else if (explicit_album) {
    load_album(album, sort, path);
  }
  load_current();
}

void PhotoWindow::load_album(const Album &album, gallery::Sort sort, const std::string &path) {
  if (album_thread_.joinable()) album_thread_.join();
  album_loading_ = true;
  Config cfg;
  cfg.load();
  auto lib = gallery::LibrarySettings::load(cfg);
  auto extra = extra_formats_;
  album_thread_ = std::thread([this, album, sort, lib, extra] {
    std::vector<Photo> result;
    if (album.kind == AlbumKind::List) {
      result = gallery::photos_for_paths(gallery::read_album_list(album.path));
    } else {
      bool recursive = true;
      auto roots = gallery::album_roots(album, lib, &recursive);
      std::vector<Photo> found;
      if (recursive) found = gallery::scan_folders(roots, extra);
      else
        for (const auto &r : roots) {
          auto part = gallery::scan_directory(r, extra);
          found.insert(found.end(), part.begin(), part.end());
        }
      result = gallery::album_photos(found, album, lib, sort, time(nullptr));
    }
    {
      std::lock_guard<std::mutex> lock(album_mu_);
      album_result_ = std::move(result);
    }
    Fl::awake(album_awake_cb, this);
  });
  (void)path;
}

void PhotoWindow::album_awake_cb(void *p) {
  if (instance_ == p) static_cast<PhotoWindow *>(p)->album_ready();
}

void PhotoWindow::album_ready() {
  std::vector<Photo> list;
  {
    std::lock_guard<std::mutex> lock(album_mu_);
    list = std::move(album_result_);
  }
  album_loading_ = false;
  std::string cur = current() ? current()->path : path_;
  if (gallery::index_of(list, cur) < 0) {
    auto self = gallery::photos_for_paths({cur});
    if (!self.empty()) list.insert(list.begin(), self[0]);
  }
  photos_ = std::move(list);
  index_ = gallery::index_of(photos_, cur);
  if (index_ < 0 && !photos_.empty()) {
    index_ = 0;
    load_current();
  }
  update_title();
  layout();
  redraw();
}

void PhotoWindow::open_dialog() {
  if (modified() && !confirm_discard()) return;
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.title(_("Open a Photo"));
  chooser.filter(_("Images\t*.{jpg,jpeg,png,gif,bmp,svg,svgz,webp,tif,tiff,avif,heic,jxl}"));
  const Photo *p = current();
  chooser.directory((p ? p->dir : gallery::pictures_dir()).c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  std::string path = chooser.filename();
  std::string dir = path.substr(0, path.rfind('/'));
  history_.reset();
  open(path, Album{AlbumKind::Directory, dir.empty() ? "/" : dir}, false, gallery::Sort::Name);
}

void PhotoWindow::show_file(const std::string &path) {
  std::string dir = path.substr(0, path.rfind('/'));
  Album album{AlbumKind::Directory, dir.empty() ? "/" : dir};
  if (!instance_) {
    int wx = 0, wy = 0, w = S(1280), h = S(840);
    fit_to_work_area(wx, wy, w, h);
    auto *win = new PhotoWindow(std::max(w, S(480)), std::max(h, S(360)));
    win->position(wx, wy);
    win->show();
    XClassHint hint{const_cast<char *>("sde-view"), const_cast<char *>("Sdeview")};
    XSetClassHint(fl_display, fl_xid(win), &hint);
  } else {
    if (instance_->modified() && !instance_->confirm_discard()) return;
    instance_->show();
    x11::activate_window(fl_xid(instance_));
  }
  instance_->history_.reset();
  instance_->open(path, album, false, gallery::Sort::Name);
}

void PhotoWindow::new_collage() {
  // The photo shown is the first one; the chooser asks for the others.
  std::vector<std::string> paths;
  if (const Photo *p = current()) paths.push_back(p->path);
  open_collage(paths, true);
}

void PhotoWindow::go(int index) {
  if (index < 0 || index >= int(photos_.size()) || index == index_) return;
  if (modified() && !confirm_discard()) return;
  index_ = index;
  load_current();
}

void PhotoWindow::step(int delta) {
  int n = int(photos_.size());
  if (!n) return;
  int next = index_ + delta;
  if (next < 0 || next >= n) {
    toast(next < 0 ? _("This is the first photo") : _("This is the last photo"));
    return;
  }
  go(next);
}

void PhotoWindow::load_current() {
  history_.reset();
  canvas_->cancel_drawing();
  canvas_->stop_animation();
  if (canvas_->cropping()) canvas_->end_crop();
  if (source_) cairo_surface_destroy(source_);
  if (preview_base_) cairo_surface_destroy(preview_base_);
  source_ = preview_base_ = nullptr;
  source_max_ = -1;
  failed_ = false;
  ++generation_;
  render_gen_ = 0;
  for (auto *s : panel_->previews_) cairo_surface_destroy(s);
  panel_->previews_.clear();
  if (panel_->preview_source_) cairo_surface_destroy(panel_->preview_source_);
  panel_->preview_source_ = nullptr;
  info_ = gallery::PhotoInfo();
  full_w_ = full_h_ = 0;
  const Photo *p = current();
  if (!p) {
    canvas_->clear();
    update_title();
    redraw();
    return;
  }
  path_ = p->path;
  gallery::read_photo_info(p->path, info_);
#ifdef SDE_GALLERY_PIXBUF
  int w = 0, h = 0;
  if (gdk_pixbuf_get_file_info(p->path.c_str(), &w, &h) && w > 0 && h > 0) {
    if (gallery::orient_from_exif(info_.orientation).k % 2 && sniff_format(p->path) == Format::Jpeg) std::swap(w, h);
    full_w_ = w;
    full_h_ = h;
  }
#endif
  // Show something at once: the embedded preview, or a cached thumbnail.
  cairo_surface_t *quick = gallery::decode_exif_preview(p->path);
  if (!quick) quick = gallery::load_cached_thumbnail(p->path, p->mtime, true);
  if (!quick) {
    auto it = thumbs_.find(p->path);
    if (it != thumbs_.end()) quick = cairo_surface_reference(it->second);
  }
  if (quick) {
    int qw = cairo_image_surface_get_width(quick), qh = cairo_image_surface_get_height(quick);
    // The preview must have the photo's shape; previews with black bars
    // (some cameras) would show a wrong frame for a moment.
    bool shape_ok = full_w_ <= 0 || std::fabs(double(qw) / qh - double(full_w_) / full_h_) < 0.05;
    if (shape_ok) canvas_->set_image(quick, full_w_ > 0 ? full_w_ : qw, full_h_ > 0 ? full_h_ : qh, false);
    else canvas_->clear();
    cairo_surface_destroy(quick);
  } else {
    canvas_->clear();
  }
  canvas_->reset_view();
  // Then a screen-sized decode, then the full image.
  int screen = int(std::max(canvas_->w(), canvas_->h()) * 1.25);
  std::vector<int> steps;
  if (full_w_ > screen * 2 || full_h_ > screen * 2) steps.push_back(screen);
  steps.push_back(0);
  service_->load_full_steps(p->path, steps, generation_);
  panel_->sync();
  update_title();
  redraw();
}

void PhotoWindow::awake_cb(void *p) {
  auto *self = static_cast<PhotoWindow *>(p);
  if (instance_ != self) return;
  bool thumbs = false;
  for (auto &r : self->service_->take_results()) {
    if (r.kind == gallery::JobKind::Full) {
      self->full_loaded(r);
      continue;
    }
    if (!r.surface) {
      self->thumb_failed_.insert(r.path);
      continue;
    }
    auto &slot = self->thumbs_[r.path];
    if (slot) cairo_surface_destroy(slot);
    slot = r.surface;
    thumbs = true;
  }
  if (thumbs) self->canvas_->redraw();
}

void PhotoWindow::full_loaded(gallery::Result &r) {
  if (r.generation != generation_ || !current() || r.path != current()->path) {
    if (r.surface) cairo_surface_destroy(r.surface);
    return;
  }
  if (source_max_ == 0) {  // already have the full image
    if (r.surface) cairo_surface_destroy(r.surface);
    return;
  }
  if (!r.surface) {
    if (r.max_dim == 0 && !source_) {
      failed_ = true;
      canvas_->clear();
      update_title();
      redraw();
    }
    return;
  }
  if (source_) cairo_surface_destroy(source_);
  source_ = r.surface;
  source_max_ = r.max_dim;
  if (r.width > 0 && r.height > 0) {
    full_w_ = r.width;
    full_h_ = r.height;
  } else {
    full_w_ = cairo_image_surface_get_width(source_);
    full_h_ = cairo_image_surface_get_height(source_);
  }
  if (preview_base_) cairo_surface_destroy(preview_base_);
  preview_base_ = downscale(source_, 1600);
  if (panel_->preview_source_) {
    cairo_surface_destroy(panel_->preview_source_);
    panel_->preview_source_ = nullptr;
  }
  refresh_preview();
  if (r.max_dim == 0 && sniff_format(r.path) == Format::Gif && edits().identity()) load_animation(r.path);
  panel_->sync();
  panel_->info_changed();
  update_title();
  if (draw_on_load_) {
    draw_on_load_ = false;
    open_draw();
  }
}

void PhotoWindow::load_animation(const std::string &path) {
#ifdef SDE_GALLERY_PIXBUF
  GdkPixbufAnimation *anim = gdk_pixbuf_animation_new_from_file(path.c_str(), nullptr);
  if (!anim) return;
  if (gdk_pixbuf_animation_is_static_image(anim)) {
    g_object_unref(anim);
    return;
  }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  GTimeVal tv{0, 0};
  GdkPixbufAnimationIter *it = gdk_pixbuf_animation_get_iter(anim, &tv);
  std::vector<std::pair<cairo_surface_t *, int>> frames;
  long long bytes = 0, total_ms = 0;
  for (int i = 0; i < 400; ++i) {
    GdkPixbuf *pb = gdk_pixbuf_animation_iter_get_pixbuf(it);
    int delay = gdk_pixbuf_animation_iter_get_delay_time(it);
    if (!pb) break;
    cairo_surface_t *s = gallery::pixbuf_to_surface(pb);
    frames.push_back({s, delay < 0 ? 100 : delay});
    bytes += (long long)cairo_image_surface_get_stride(s) * cairo_image_surface_get_height(s);
    if (delay < 0 || bytes > (256LL << 20)) break;
    total_ms += delay;
    if (total_ms > 120000) break;
    long us = tv.tv_usec + long(delay) * 1000;
    tv.tv_sec += us / 1000000;
    tv.tv_usec = us % 1000000;
    gdk_pixbuf_animation_iter_advance(it, &tv);
    // Stop after one loop: the first frame comes around again.
    if (i > 0 && gdk_pixbuf_animation_iter_get_pixbuf(it) == pb && delay == 0) break;
  }
#pragma GCC diagnostic pop
  g_object_unref(it);
  g_object_unref(anim);
  if (frames.size() > 1) canvas_->set_animation(std::move(frames));
  else
    for (auto &f : frames) cairo_surface_destroy(f.first);
#else
  (void)path;
#endif
}

cairo_surface_t *PhotoWindow::thumbnail(const Photo &p) {
  auto it = thumbs_.find(p.path);
  return it == thumbs_.end() ? nullptr : it->second;
}

void PhotoWindow::request_thumbnails(const std::vector<const Photo *> &wanted) {
  std::vector<gallery::ThumbnailService::Request> jobs;
  for (const auto *p : wanted)
    if (!thumbs_.count(p->path) && !thumb_failed_.count(p->path))
      jobs.push_back({p->path, p->mtime, gallery::JobKind::Normal});
  if (!jobs.empty()) service_->replace_queue(std::move(jobs));
}

// ------------------------------------------------------------------ edits

void PhotoWindow::apply(const Edits &e, bool amend) {
  if (amend) history_.amend(e);
  else history_.push(e);
  if (!e.tone_identity() || !(e.orient == gallery::Orient())) canvas_->stop_animation();
  refresh_preview();
  panel_->sync();
  update_title();
  redraw();
}

void PhotoWindow::undo() {
  if (canvas_->cropping()) {
    cancel_crop();
    return;
  }
  if (canvas_->drawing_busy() || canvas_->selected_mark() >= 0) canvas_->cancel_drawing();
  if (!history_.can_undo()) return;
  history_.undo();
  refresh_preview();
  panel_->sync();
  update_title();
  redraw();
}

void PhotoWindow::redo() {
  canvas_->cancel_drawing();
  if (!history_.can_redo()) return;
  history_.redo();
  refresh_preview();
  panel_->sync();
  update_title();
  redraw();
}

void PhotoWindow::reset_edits() {
  if (canvas_->cropping()) canvas_->end_crop();
  canvas_->cancel_drawing();
  apply(Edits());
}

void PhotoWindow::rotate(int q) {
  if (!source_) return;
  Edits e = edits();
  e.orient = gallery::orient_then(e.orient, gallery::Orient{(q % 4 + 4) % 4, false});
  // The crop rectangle turns with the picture.
  Rect c = e.crop;
  if (((q % 4) + 4) % 4 == 1) e.crop = {1 - c.y - c.h, c.x, c.h, c.w};
  else if (((q % 4) + 4) % 4 == 3) e.crop = {c.y, 1 - c.x - c.w, c.h, c.w};
  if (e.resize_w && e.resize_h && q % 2) std::swap(e.resize_w, e.resize_h);
  // Drawings turn with the picture.
  rotate_marks(e.marks, q);
  canvas_->cancel_drawing();
  apply(e);
  if (q) toast(q > 0 ? _("Rotated right") : _("Rotated left"));
}

void PhotoWindow::flip(bool horizontal) {
  if (!source_) return;
  Edits e = edits();
  // Horizontal flip = mirror; vertical = mirror then half turn.
  e.orient = gallery::orient_then(e.orient, horizontal ? gallery::Orient{0, true} : gallery::Orient{2, true});
  Rect c = e.crop;
  e.crop = horizontal ? Rect{1 - c.x - c.w, c.y, c.w, c.h} : Rect{c.x, 1 - c.y - c.h, c.w, c.h};
  flip_marks(e.marks, horizontal);
  canvas_->cancel_drawing();
  apply(e);
}

void PhotoWindow::start_crop() {
  if (!source_ || canvas_->cropping()) return;
  if (!panel_->visible() || panel_->mode() != SidePanel::Edit) toggle_panel(SidePanel::Edit);
  before_crop_ = edits();
  canvas_->begin_crop(edits().crop, 0);
  panel_->set_tab(SidePanel::Crop);
  refresh_preview();
  panel_->layout();
  redraw();
}

void PhotoWindow::apply_crop() {
  if (!canvas_->cropping()) return;
  Edits e = edits();
  e.crop = canvas_->crop_rect();
  e.resize_w = e.resize_h = 0;  // the old size no longer fits
  canvas_->end_crop();
  apply(e);
  panel_->layout();
}

void PhotoWindow::cancel_crop() {
  if (!canvas_->cropping()) return;
  canvas_->end_crop();
  refresh_preview();
  panel_->layout();
  redraw();
}

void PhotoWindow::set_crop_aspect(double a) {
  if (canvas_->cropping()) canvas_->set_crop_aspect(a);
}

void PhotoWindow::refresh_preview() {
  if (!source_) return;
  Edits e = edits();
  render_gen_++;
  if (canvas_->cropping()) {
    // Show the whole (rotated, toned) picture with the selection on top,
    // without the finishing touches (frame, watermark, background blur).
    e.crop = Rect();
    e.resize_w = e.resize_h = 0;
    e.finish = Finish();
    int ow, oh;
    result_size(e, full_w_, full_h_, ow, oh);
    cairo_surface_t *img = render(preview_base_, e, 1.0, kMarkEffects);
    canvas_->set_image(img, ow, oh, false);
    cairo_surface_destroy(img);
    return;
  }
  // Vector marks are drawn live by the canvas (a layer above the photo
  // until saving); only the pixel effects (blur, pixelate) are rendered.
  Edits base = e;
  base.marks.items.erase(std::remove_if(base.marks.items.begin(), base.marks.items.end(),
                                        [](const Mark &m) { return !m.effect(); }),
                         base.marks.items.end());
  if (base.identity()) {
    canvas_->set_image(source_, full_w_, full_h_, true);
    return;
  }
  int ow, oh;
  result_size(e, full_w_, full_h_, ow, oh);
  double scale = double(cairo_image_surface_get_width(preview_base_)) / std::max(1, full_w_);
  cairo_surface_t *img = render(preview_base_, base, scale, kMarkEffects | kFinish);
  canvas_->set_image(img, ow, oh, true);
  cairo_surface_destroy(img);
  // Sharp pixels for zooming in: render the full image in the background.
  if (preview_base_ != source_ && cairo_image_surface_get_width(preview_base_) < full_w_) start_full_render();
}

void PhotoWindow::start_full_render() {
  uint64_t gen = render_gen_;
  cairo_surface_t *src = cairo_surface_reference(source_);
  Edits e = edits();
  int ow, oh;
  result_size(e, full_w_, full_h_, ow, oh);
  double scale = double(cairo_image_surface_get_width(src)) / std::max(1, full_w_);
  std::thread([this, gen, src, e, scale] {
    // Wait a moment: slider drags produce many states.
    usleep(350 * 1000);
    if (render_gen_ != gen) {
      cairo_surface_destroy(src);
      return;
    }
    cairo_surface_t *out = render(src, e, scale, kMarkEffects | kFinish);
    cairo_surface_destroy(src);
    auto *job = new RenderJob{this, gen, out};
    Fl::awake(render_awake_cb, job);
  }).detach();
}

void PhotoWindow::render_awake_cb(void *p) {
  auto *job = static_cast<RenderJob *>(p);
  PhotoWindow *self = instance_;
  if (self && self == job->self && self->render_gen_ == job->gen && !self->canvas_->cropping()) {
    int ow, oh;
    result_size(self->edits(), self->full_w_, self->full_h_, ow, oh);
    self->canvas_->set_image(job->result, ow, oh, true);
  }
  cairo_surface_destroy(job->result);
  delete job;
}

cairo_surface_t *PhotoWindow::render_full() {
  const Photo *p = current();
  if (!p) return nullptr;
  cairo_surface_t *src = nullptr;
  if (source_ && source_max_ == 0) src = cairo_surface_reference(source_);
  else src = gallery::decode_image(p->path, 0);
  if (!src) return nullptr;
  cairo_surface_t *out = render(src, edits());
  cairo_surface_destroy(src);
  return out;
}

bool PhotoWindow::confirm_discard() {
  const Photo *p = current();
  int r = ask(_("Unsaved Changes"),
              format(_("Save the changes to “%s”?\nIf you do not save, the edits are lost."), p ? p->name.c_str() : ""),
              {_("Cancel"), _("Discard"), _("Save")});
  if (r == 0) return false;
  if (r == 1) {
    history_.reset();
    return true;
  }
  return save(false);
}

// ----------------------------------------------------------------- saving

void PhotoWindow::notify_gallery(const std::string &verb, const std::string &path) {
  gallery::send_gallery_events({{verb, path}});
}

void PhotoWindow::after_save(const std::string &path, SaveResult r) {
  if (r == SaveResult::Failed) return;
  toast(r == SaveResult::Lossless ? format(_("Saved “%s” without loss of quality"), basename_of(path).c_str())
                                  : format(_("Saved “%s”"), basename_of(path).c_str()));
}

bool PhotoWindow::save(bool ask_first) {
  const Photo *p = current();
  if (!p || !modified()) return true;
  if (canvas_->cropping()) apply_crop();
  std::string path = p->path;
  Format f = sniff_format(path);
  if (!can_write(f) || format_of(path) != f) return save_as();
  bool lossless = lossless_possible(path, edits(), path);
  int quality = jpeg_quality_;
  if (ask_first) {
    int r;
    if (lossless) {
      r = ask(_("Save Photo"), format(_("Save the changes to “%s”?\nRotation and flipping are saved without loss of quality."), p->name.c_str()),
              {_("Cancel"), _("Save a Copy…"), _("Save")});
    } else {
      bool q = f == Format::Jpeg || f == Format::Webp;
      r = ask(_("Save Photo"),
              format(_("Replace “%s” with the edited photo?\nThe original cannot be restored afterwards."), p->name.c_str()),
              {_("Cancel"), _("Save a Copy…"), _("Replace")}, q ? &quality : nullptr);
    }
    if (r == 0) return false;
    if (r == 1) return save_as();
  }
  if (quality != jpeg_quality_) {
    jpeg_quality_ = quality;
    Config cfg;
    cfg.load();
    cfg.set("photo", "jpeg_quality", std::to_string(quality));
    cfg.save();
  }
  cairo_surface_t *out = lossless ? nullptr : render_full();
  if (!lossless && !out) {
    toast(_("The photo could not be read again for saving"));
    return false;
  }
  std::string err;
  fl_cursor(FL_CURSOR_WAIT);
  Fl::check();
  SaveResult r = save_edits(path, edits(), out, path, quality, &err);
  fl_cursor(FL_CURSOR_DEFAULT);
  if (out) cairo_surface_destroy(out);
  if (r == SaveResult::Failed) {
    ask(_("Save Photo"), format(_("“%s” could not be saved:\n%s"), p->name.c_str(), err.c_str()), {_("Close")});
    return false;
  }
  // Reload: the file now holds the edits.
  struct stat st {};
  if (stat(path.c_str(), &st) == 0) {
    photos_[size_t(index_)].mtime = st.st_mtime;
    photos_[size_t(index_)].size = st.st_size;
  }
  thumbs_.erase(path);
  notify_gallery("changed", path);
  after_save(path, r);
  history_.reset();
  load_current();
  return true;
}

bool PhotoWindow::save_as() {
  const Photo *p = current();
  if (!p) return false;
  if (canvas_->cropping()) apply_crop();
  Format src = sniff_format(p->path);
  Format out_fmt = output_format(src);
  std::string stem = p->name;
  size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot > 0) stem.resize(dot);
  std::string suggestion = replace_extension(p->dir + "/" + stem + (modified() ? _(" (edited)") : _(" (copy)")), out_fmt);
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  chooser.title(_("Save Photo As"));
  chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::NEW_FOLDER);
  chooser.directory(p->dir.c_str());
  chooser.preset_file(basename_of(suggestion).c_str());
  chooser.filter(_("JPEG\t*.{jpg,jpeg}\nPNG\t*.png\nWebP\t*.webp\nTIFF\t*.{tif,tiff}"));
  if (chooser.show() != 0 || !chooser.filename()) return false;
  std::string dest = chooser.filename();
  Format f = format_of(dest);
  if (f == Format::Unknown) {
    dest = replace_extension(dest, out_fmt);
    f = out_fmt;
  }
  if (!can_write(f)) {
    toast(format(_("%s files cannot be written; saving as PNG"), format_name(f)));
    dest = replace_extension(dest, Format::Png);
    f = Format::Png;
  }
  bool lossless = lossless_possible(p->path, edits(), dest);
  int quality = jpeg_quality_;
  if (!lossless && (f == Format::Jpeg || f == Format::Webp)) {
    if (ask(_("Save Photo As"), format(_("Save as %s with this quality:"), format_name(f)), {_("Cancel"), _("Save")},
            &quality) == 0)
      return false;
  }
  cairo_surface_t *out = lossless ? nullptr : render_full();
  if (!lossless && !out) return false;
  std::string err;
  SaveResult r = save_edits(p->path, edits(), out, dest, quality, &err);
  if (out) cairo_surface_destroy(out);
  if (r == SaveResult::Failed) {
    ask(_("Save Photo As"), format(_("The photo could not be saved:\n%s"), err.c_str()), {_("Close")});
    return false;
  }
  notify_gallery(dest == p->path ? "changed" : "added", dest);
  after_save(dest, r);
  // Keep the copy next to the original in the list, and show it.
  std::string dir = dest.substr(0, dest.rfind('/'));
  if (dest != p->path && dir == p->dir && gallery::index_of(photos_, dest) < 0) {
    auto added = gallery::photos_for_paths({dest});
    if (!added.empty()) photos_.insert(photos_.begin() + index_ + 1, added[0]);
  }
  history_.reset();
  int i = gallery::index_of(photos_, dest);
  if (i >= 0) {
    index_ = i;
    load_current();
  } else {
    load_current();
  }
  return true;
}

// ---------------------------------------------------------------- drawing

bool PhotoWindow::drawing() const {
  return panel_->visible() && panel_->mode() == SidePanel::Edit && panel_->tab() == SidePanel::Draw &&
         source_ != nullptr;
}

void PhotoWindow::open_draw() {
  if (!source_) return;
  if (canvas_->cropping()) apply_crop();
  if (!panel_->visible() || panel_->mode() != SidePanel::Edit) toggle_panel(SidePanel::Edit);
  panel_->set_tab(SidePanel::Draw);
  canvas_->stop_animation();
  layout();
  redraw();
}

void PhotoWindow::set_tool(Tool t) {
  canvas_->cancel_drawing();
  draw_.tool = t;
  panel_->sync();
  canvas_->redraw();
}

void PhotoWindow::mark_reference(int &w, int &h) const {
  Edits g = edits();
  g.crop = Rect();
  g.resize_w = g.resize_h = 0;
  content_size(g, full_w_, full_h_, w, h);
}

Edits PhotoWindow::edits_for_marks() const {
  Edits e = edits();
  if (e.marks.w <= 0 || e.marks.h <= 0) mark_reference(e.marks.w, e.marks.h);
  return e;
}

void PhotoWindow::delete_selected_mark() {
  int i = canvas_->selected_mark();
  Edits e = edits();
  if (i < 0 || i >= int(e.marks.items.size())) return;
  e.marks.items.erase(e.marks.items.begin() + i);
  canvas_->cancel_drawing();
  apply(e);
  toast(_("Annotation deleted"));
}

void PhotoWindow::style_changed(bool amend) {
  int i = canvas_->selected_mark();
  Edits e = edits();
  if (i < 0 || i >= int(e.marks.items.size())) return;
  Mark &m = e.marks.items[size_t(i)];
  double k = canvas_->scale() > 0 ? 1 / canvas_->scale() : 1;
  // Screen pixels at the current zoom -> image pixels.
  Edits ref = edits_for_marks();
  int rw = 0, rh = 0;
  content_size(ref, full_w_, full_h_, rw, rh);
  MarkMap mm = mark_map(ref, rw, rh);
  if (mm.scale() > 0) k /= mm.scale();
  int tool = m.kind == MarkKind::Pen ? int(Tool::Pen) : m.kind == MarkKind::Highlighter ? int(Tool::Highlighter)
           : m.kind == MarkKind::Line ? int(Tool::Line) : m.kind == MarkKind::Arrow ? int(Tool::Arrow)
           : m.kind == MarkKind::Rect ? int(Tool::Rect) : m.kind == MarkKind::Ellipse ? int(Tool::Ellipse)
           : m.kind == MarkKind::Blur ? int(Tool::Blur) : m.kind == MarkKind::Pixelate ? int(Tool::Pixelate)
           : m.kind == MarkKind::RedEye ? int(Tool::Select)
                                      : int(Tool::Text);
  if (m.kind == MarkKind::Text) {
    m.size = draw_.text_size * k;
    m.font = draw_.font;
    m.box = draw_.text_box;
  } else {
    m.width = draw_.width[tool] * k;
  }
  if (!m.effect()) {
    m.color = m.kind == MarkKind::Highlighter ? (draw_.color | 0xFF000000u) : draw_.color;
    m.opacity = draw_.opacity[tool] / 100.0;
  }
  if (m.kind == MarkKind::Rect || m.kind == MarkKind::Ellipse) m.fill = draw_.fill;
  apply(e, amend);
}

void PhotoWindow::edit_text_mark(int index) {
  Edits e = edits_for_marks();
  bool fresh = index < 0 || index >= int(e.marks.items.size());
  std::string text = fresh ? std::string() : e.marks.items[size_t(index)].text;
  const char *r = fl_input("%s", text.c_str(), _("Text to place on the photo:"));
  if (!r) return;
  std::string t = r;
  if (fresh) return;  // new text is placed by the canvas, see Canvas::draw_event()
  if (t.empty()) {
    e.marks.items.erase(e.marks.items.begin() + index);
    canvas_->cancel_drawing();
  } else {
    e.marks.items[size_t(index)].text = t;
  }
  apply(e);
}

// ------------------------------------------------------------ file actions

void PhotoWindow::copy_to_clipboard() {
  const Photo *p = current();
  if (!p || !source_) return;
  cairo_surface_t *img = modified() ? render_full() : (source_max_ == 0 ? cairo_surface_reference(source_) : render_full());
  clipboard::copy_image(img, modified() ? std::string() : p->path);
  if (img) cairo_surface_destroy(img);
  toast(_("Image copied to the clipboard"));
}

void PhotoWindow::set_wallpaper() {
  const Photo *p = current();
  if (!p) return;
  if (modified()) {
    if (ask(_("Set as Wallpaper"), _("The wallpaper uses the saved photo.\nSave your edits first?"),
            {_("Cancel"), _("Save")}) == 0 || !save(false))
      return;
    p = current();
    if (!p) return;
  }
  Config cfg;
  cfg.load();
  cfg.set("appearance", "wallpaper", p->path);
  if (!cfg.save()) {
    toast(_("Could not save the wallpaper setting"));
    return;
  }
  x11::notify_config_changed();
  toast(_("Set as wallpaper"));
}

void PhotoWindow::trash() {
  const Photo *p = current();
  if (!p) return;
  std::string name = p->name, path = p->path;
  if (ask(_("Move to Trash"), format(_("Move “%s” to the Trash?"), name.c_str()), {_("Cancel"), _("Move to Trash")}) == 0)
    return;
  std::string err;
  if (!gallery::move_to_trash(path, &err)) {
    ask(_("Move to Trash"), format(_("“%s” could not be moved to the Trash:\n%s"), name.c_str(), err.c_str()), {_("Close")});
    return;
  }
  notify_gallery("removed", path);
  toast(format(_("“%s” was moved to the Trash"), name.c_str()));
  history_.reset();
  photos_.erase(photos_.begin() + index_);
  thumbs_.erase(path);
  if (index_ >= int(photos_.size())) index_ = int(photos_.size()) - 1;
  load_current();
}

void PhotoWindow::open_with() {
  const Photo *p = current();
  if (!p) return;
  std::vector<DesktopEntry> apps;
  for (auto &e : load_applications()) {
    bool graphics = std::find(e.categories.begin(), e.categories.end(), "Graphics") != e.categories.end();
    bool takes_files = e.exec.find("%f") != std::string::npos || e.exec.find("%F") != std::string::npos ||
                       e.exec.find("%u") != std::string::npos || e.exec.find("%U") != std::string::npos;
    if (graphics && takes_files && e.id != "sde-view.desktop") apps.push_back(e);
  }
  std::sort(apps.begin(), apps.end(), [](const DesktopEntry &a, const DesktopEntry &b) { return a.name < b.name; });
  std::vector<Fl_Menu_Item> items(apps.size() + 2);
  for (auto &it : items) memset(&it, 0, sizeof it);
  for (size_t i = 0; i < apps.size(); ++i) {
    items[i].text = apps[i].name.c_str();
    items[i].labelsize(theme_.font_size);
    items[i].labelcolor(theme_.text.fl());
  }
  if (!apps.empty()) items[apps.size() - 1].flags |= FL_MENU_DIVIDER;
  items[apps.size()].text = _("Default Application");
  items[apps.size()].labelsize(theme_.font_size);
  items[apps.size()].labelcolor(theme_.text.fl());
  const Fl_Menu_Item *m = items.data()->popup(menu_->x() - S(200), menu_->y() + menu_->h(), _("Open With"));
  if (!m) return;
  size_t i = size_t(m - items.data());
  if (modified()) toast(_("The other application opens the saved file, without your edits"));
  if (i < apps.size()) {
    if (!launch_entry(apps[i], {p->path})) toast(format(_("%s could not be started"), apps[i].name.c_str()));
  } else if (!open_uri(p->path)) {
    toast(_("No application is available"));
  }
}

void PhotoWindow::show_in_files() {
  const Photo *p = current();
  if (!p) return;
  if (!find_program("liria").empty() && spawn_detached({"liria", "--no-desktop", p->dir})) return;
  if (!open_uri(p->dir)) toast(_("No file manager is available"));
}

void PhotoWindow::show_in_gallery() {
  const Photo *p = current();
  if (!p) return;
  if (!gallery::show_in_gallery(p->path)) toast(_("Gallery (sde-gallery) is not installed"));
}

void PhotoWindow::print() {
  const Photo *p = current();
  if (!p) return;
  if (find_program("lp").empty()) {
    toast(_("Printing needs the CUPS client (lp)"));
    return;
  }
  std::string file = p->path;
  if (modified()) {
    cairo_surface_t *img = render_full();
    file = runtime_dir() + "/sde-view-print-" + std::to_string(getpid()) + ".png";
    bool ok = img && write_image(img, file, Format::Png, 100);
    if (img) cairo_surface_destroy(img);
    if (!ok) {
      toast(_("The photo could not be prepared for printing"));
      return;
    }
  }
  std::string name = p->name;
  run_async({"lp", "-t", name, "-o", "fit-to-page", "--", file}, [name](const std::string &, int status) {
    if (PhotoWindow::instance_)
      PhotoWindow::instance_->toast(status == 0 ? format(_("“%s” was sent to the printer"), name.c_str())
                                                : _("Printing failed; check the printer in Settings"));
  });
}

void PhotoWindow::open_map() {
  if (!info_.has_gps) return;
  if (!open_uri(gallery::osm_url(info_.latitude, info_.longitude))) toast(_("No web browser is available"));
}

// ------------------------------------------------------------------- view

void PhotoWindow::toggle_fullscreen() {
  if (fullscreen_active()) fullscreen_off();
  else fullscreen();
}

void PhotoWindow::context_menu() {
  bool have = current() != nullptr, loaded = source_ != nullptr;
  ContextMenu m;
  m.item(_("Copy Image"), [this] { copy_to_clipboard(); }, {"copy", FL_CTRL + 'c', loaded});
  m.item(_("Draw and Annotate…"), [this] { open_draw(); }, {"", 'd', loaded});
  m.item(_("Adjust Colours…"), [this] {
    if (!panel_->visible() || panel_->mode() != SidePanel::Edit) toggle_panel(SidePanel::Edit);
    panel_->set_tab(SidePanel::Adjust);
  }, {"", 'e', loaded});
  m.item(_("Crop…"), [this] { start_crop(); }, {"", 'c', loaded});
  m.item(_("Rotate Right"), [this] { rotate(1); }, {"", 'r', loaded, true});
  if (drawing() && canvas_->selected_mark() >= 0)
    m.item(_("Delete Annotation"), [this] { delete_selected_mark(); }, {"delete", FL_Delete, true, true});
  m.item(_("Show in Gallery"), [this] { show_in_gallery(); }, {"image", FL_CTRL + 'g', have});
  m.item(_("Slideshow in Gallery"), [this] { slideshow_in_gallery(); }, {"", FL_F + 5, have && photos_.size() > 1});
  m.item(_("Open With…"), [this] { open_with(); }, {"open", 0, have});
  m.item(_("Show in Files"), [this] { show_in_files(); }, {"folder", 0, have});
  m.item(_("Set as Wallpaper"), [this] { set_wallpaper(); }, {"", 0, have, true});
  m.item(_("Information"), [this] { toggle_panel(SidePanel::Info); }, {"info", 'i', have});
  m.item(_("Move to Trash…"), [this] { trash(); }, {"trash", FL_Delete, have});
  m.popup();
  redraw();
}

void PhotoWindow::slideshow_in_gallery() {
  // Slideshows of a folder or album are Gallery's: hand over the photo.
  const Photo *p = current();
  if (!p) return;
  if (modified() && !confirm_discard()) return;
  if (!gallery::slideshow_in_gallery(p->path)) toast(_("Gallery (sde-gallery) is not installed"));
}

bool PhotoWindow::panel_open() const { return panel_->visible(); }

void PhotoWindow::toggle_panel(SidePanel::Mode m) {
  if (panel_->visible() && panel_->mode() == m) {
    if (canvas_->cropping()) cancel_crop();
    panel_->hide();
  } else {
    if (panel_->mode() == SidePanel::Edit && m != SidePanel::Edit && canvas_->cropping()) cancel_crop();
    panel_->set_mode(m);
    panel_->show();
  }
  layout();
  redraw();
}

void PhotoWindow::toggle_filmstrip() {
  canvas_->set_filmstrip(!canvas_->filmstrip());
  redraw();
}

void PhotoWindow::show_about() {
  gallery::AboutInfo info;
  info.name = _("Photo");
  info.program = "sde-view";
  info.icon = "sde-photo";
  info.icon_fallbacks = {"sdeview"};
  info.version = SDE_VERSION;
  info.description = _("View and edit one image at a time: read its camera details and location, rotate, "
                       "crop, resize, adjust the colours, apply filters, and draw on it with pens, highlighters, "
                       "shapes, arrows and text, or blur and pixelate private details. "
                       "Your photo library, albums and slideshows are in Gallery.");
  info.logo = draw_logo;
  info.credits.push_back(format(_("Built with SDE-FLTK %s, cairo and gdk-pixbuf"), SDE_FLTK_VERSION));
  if (gallery::have_libexif()) info.credits.push_back(_("Camera details read with libexif"));
  gallery::show_about(info);
}

void PhotoWindow::show_shortcuts() {
  gallery::show_shortcuts(_("Photo Keyboard Shortcuts"),
                          {{"", _("Viewing")},
                           {_("Left, Right"), _("Previous or next photo")},
                           {_("Home, End"), _("First or last photo")},
                           {_("Wheel, + −"), _("Zoom in or out")},
                           {_("0 / 1 / F"), _("Best fit / actual size / fill")},
                           {_("Drag"), _("Move a zoomed photo")},
                           {_("I"), _("Information")},
                           {_("B"), _("Filmstrip")},
                           {_("F11"), _("Full screen")},
                           {_("F5"), _("Slideshow in Gallery")},
                           {"", _("Editing")},
                           {_("R, Shift+R"), _("Rotate right or left")},
                           {_("H, V"), _("Flip horizontally or vertically")},
                           {_("C"), _("Crop")},
                           {_("E"), _("Adjust colours and filters")},
                           {_("D"), _("Draw and annotate")},
                           {"", _("Drawing (Draw tab open)")},
                           {_("S, P, M, X"), _("Select, pen, highlighter, eraser")},
                           {_("L, A, U, O, T, K"), _("Line, arrow, rectangle, ellipse, text, pick colour")},
                           {_("Shift+drag"), _("Straight angles, squares and circles")},
                           {_("Middle drag"), _("Move a zoomed photo while drawing")},
                           {_("Delete"), _("Delete the selected annotation")},
                           {_("Ctrl+Z, Ctrl+Shift+Z"), _("Undo, redo")},
                           {_("Ctrl+S"), _("Save")},
                           {_("Ctrl+Shift+S"), _("Save as")},
                           {"", _("File")},
                           {_("Ctrl+C"), _("Copy the image")},
                           {_("Ctrl+G"), _("Show in Gallery")},
                           {_("Delete"), _("Move to the Trash")},
                           {_("Ctrl+O"), _("Open a photo")},
                           {_("Ctrl+W"), _("Close")}});
}

void PhotoWindow::toast(const std::string &message) {
  toast_ = message;
  Fl::remove_timeout(toast_cb, this);
  Fl::add_timeout(2.8, toast_cb, this);
  redraw();
}

void PhotoWindow::toast_cb(void *p) {
  auto *self = static_cast<PhotoWindow *>(p);
  self->toast_.clear();
  self->redraw();
}

void PhotoWindow::update_title() {
  const Photo *p = current();
  std::string t = p ? p->name + (modified() ? " *" : "") + " – " + _("Photo") : std::string(_("Photo"));
  copy_label(t.c_str());
  redraw();
}

// ----------------------------------------------------------------- layout

void PhotoWindow::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  layout();
}

void PhotoWindow::layout() {
  bool chrome = true;
  header_h_ = chrome ? S(56) : 0;
  int pw = chrome && panel_->visible() && w() >= S(640) ? S(320) : 0;
  if (!chrome) panel_->hide();
  canvas_->resize(0, header_h_, w() - pw, h() - header_h_);
  if (pw) panel_->resize(w() - pw, header_h_, pw, h() - header_h_);
  if (chrome) {
    menu_->resize(w() - S(50), S(8), S(40), S(40));
    menu_->show();
  } else {
    menu_->hide();
  }
}

void PhotoWindow::layout_buttons() {
  buttons_.clear();
  double bs = S(40), by = S(8);
  auto add = [&](icon::Kind g, const std::string &tip, std::function<void()> fn) {
    Button b;
    b.glyph = g;
    b.tip = tip;
    b.action = std::move(fn);
    b.w = b.h = bs;
    b.y = by;
    buttons_.push_back(std::move(b));
    return &buttons_.back();
  };
  bool have = current() != nullptr;
  add(icon::Gallery, _("Show in Gallery (Ctrl+G)"), [this] { show_in_gallery(); })->x = S(8);
  size_t right = buttons_.size();
  // Right side, listed left to right; groups get extra space.
  std::vector<int> group_after;
  if (modified()) {
    auto *u = add(icon::Undo, _("Undo (Ctrl+Z)"), [this] { undo(); });
    u->active = history_.can_undo() || canvas_->cropping();
    auto *r = add(icon::Redo, _("Redo (Ctrl+Shift+Z)"), [this] { redo(); });
    r->active = history_.can_redo();
    add(icon::Save, _("Save (Ctrl+S)"), [this] { save(); })->toggled = true;
    group_after.push_back(int(buttons_.size()));
  }
  // Narrow windows keep the name readable: zooming stays on the wheel and
  // the keys, full screen on F11.
  bool narrow = w() < S(820);
  if (!narrow) add(icon::ZoomOut, _("Zoom Out (−)"), [this] { canvas_->zoom_by(1 / 1.25); });
  bool fit = canvas_->zoom_mode() == ZoomMode::Fit;
  add(icon::Fit, fit ? _("Actual Size (1)") : _("Best Fit (0)"),
      [this, fit] { canvas_->set_zoom_mode(fit ? ZoomMode::Actual : ZoomMode::Fit); });
  if (!narrow) add(icon::ZoomIn, _("Zoom In (+)"), [this] { canvas_->zoom_by(1.25); });
  group_after.push_back(int(buttons_.size()));
  add(icon::Rotate, _("Rotate Right (R)"), [this] { rotate(1); });
  if (!narrow)
    add(fullscreen_active() ? icon::Unfullscreen : icon::Fullscreen, _("Full Screen (F11)"), [this] { toggle_fullscreen(); });
  group_after.push_back(int(buttons_.size()));
  auto *dr = add(icon::Edit, _("Draw and Annotate (D)"), [this] {
    if (drawing()) toggle_panel(SidePanel::Edit);
    else open_draw();
  });
  dr->toggled = drawing();
  auto *ed = add(icon::Adjust, _("Edit (E)"), [this] {
    if (panel_->visible() && panel_->mode() == SidePanel::Edit && panel_->tab() == SidePanel::Draw) {
      panel_->set_tab(SidePanel::Adjust);
      redraw();
      return;
    }
    toggle_panel(SidePanel::Edit);
  });
  ed->toggled = panel_->visible() && panel_->mode() == SidePanel::Edit && !drawing();
  auto *in = add(icon::Info, _("Information (I)"), [this] { toggle_panel(SidePanel::Info); });
  in->toggled = panel_->visible() && panel_->mode() == SidePanel::Info;
  double rx = menu_->x() - S(6);
  for (size_t i = buttons_.size(); i-- > right;) {
    if (std::find(group_after.begin(), group_after.end(), int(i + 1)) != group_after.end() && i + 1 < buttons_.size())
      rx -= S(10);
    rx -= bs;
    buttons_[i].x = rx;
    rx -= S(2);
  }
  for (size_t i = 1; i < buttons_.size(); ++i)
    if (!have) buttons_[i].active = false;
}

void PhotoWindow::draw() {
  layout_buttons();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    if (header_h_) {
      p.fill_rect(0, 0, w(), header_h_, theme_.surface);
      if (theme_.glass) p.glass_sheen(0, 0, w(), header_h_, 0, theme_.dark);
      p.line(0, header_h_ - 0.5, w(), header_h_ - 0.5, theme_.border);
      for (size_t i = 0; i < buttons_.size(); ++i) {
        const Button &b = buttons_[i];
        bool hot = int(i) == hover_button_ && b.active;
        if (b.toggled) p.fill_rounded(b.x, b.y, b.w, b.h, Sd(10), theme_.accent.alpha(theme_.dark ? 0.3 : 0.16));
        else if (hot) p.fill_rounded(b.x, b.y, b.w, b.h, Sd(10), theme_.hover);
        Color c = !b.active ? theme_.text_dim.alpha(0.45) : b.toggled ? theme_.accent_text : theme_.text;
        icon::draw(p, b.glyph, b.x + S(9), b.y + S(9), S(22), c);
      }
    }
  }
  draw_children();
  if (header_h_) {
    int nx = buttons_.empty() ? S(16) : int(buttons_[0].x + buttons_[0].w + S(10));
    int nw = (buttons_.size() > 1 ? int(buttons_[1].x) : w() - S(60)) - S(12) - nx;
    const Photo *ph = current();
    if (ph) {
      text_font(theme_.font_size + S(1), true);
      draw_text(ph->name + (modified() ? "  •" : ""), nx, S(9), nw, S(22), theme_.text);
      text_font(theme_.font_size - S(1));
      std::string sub;
      if (!photos_.empty()) sub = format(_("%d of %zu"), index_ + 1, photos_.size());
      if (album_loading_) sub += "  ·  " + std::string(_("loading album…"));
      int ow = full_w_, oh = full_h_;
      if (modified()) result_size(edits(), full_w_, full_h_, ow, oh);
      if (ow > 0) sub += "  ·  " + format("%d × %d", ow, oh);
      if (source_ || ow > 0) sub += "  ·  " + format("%d%%", int(std::lround(canvas_->scale() * 100)));
      if (canvas_->animated()) sub += "  ·  " + std::string(_("animated"));
      draw_text(sub, nx, S(30), nw, S(18), theme_.text_dim);
    } else {
      text_font(theme_.font_size + S(1), true);
      draw_text(_("Photo"), nx, S(8), nw, S(40), theme_.text);
    }
    if (hover_button_ >= 0 && hover_button_ < int(buttons_.size())) {
      const Button &b = buttons_[size_t(hover_button_)];
      text_font(theme_.font_size - S(1));
      int tw = text_width(b.tip) + S(20), th = S(28);
      int tx = int(std::max(double(S(6)), std::min(b.x + b.w / 2 - tw / 2, double(w() - tw - S(6)))));
      int ty = int(b.y + b.h + S(8));
      {
        Painter p(w(), h());
        p.shadow(tx, ty, tw, th, Sd(8), theme_.shadow, S(6));
        p.fill_rounded(tx, ty, tw, th, Sd(8), theme_.dark ? theme_.surface_alt : theme_.text.mix(theme_.background, 0.08));
      }
      draw_text(b.tip, tx, ty, tw, th, theme_.dark ? theme_.text : theme_.background, 1);
    }
  }
  if (!toast_.empty()) {
    text_font(theme_.font_size);
    int tw = std::min(text_width(toast_) + S(40), w() - S(40));
    int th = S(40);
    int tx = (canvas_->w() - tw) / 2, ty = canvas_->y() + canvas_->h() - canvas_->filmstrip_height() - th - S(20);
    Color bg = theme_.dark ? theme_.surface_alt.mix(theme_.text, 0.12) : theme_.text.mix(theme_.background, 0.08);
    Color fg = theme_.dark ? theme_.text : theme_.background;
    {
      Painter p(w(), h());
      p.shadow(tx, ty, tw, th, th / 2.0, theme_.shadow, S(8));
      p.fill_rounded(tx, ty, tw, th, th / 2.0, bg);
    }
    draw_text(toast_, tx + S(20), ty, tw - S(40), th, fg, 1);
  }
}

// ----------------------------------------------------------------- events

bool PhotoWindow::keyboard(int key, int state) {
  bool ctrl = state & FL_CTRL, shift = state & FL_SHIFT, alt = state & FL_ALT;
  Fl_Widget *f = Fl::focus();
  bool typing = f && (f == panel_->res_w_ || f == panel_->res_h_);
  if (ctrl) {
    switch (key) {
      case 'z': shift ? redo() : undo(); return true;
      case 'y': redo(); return true;
      case 's': shift ? save_as() : save(); return true;
      case 'c': if (!typing) { copy_to_clipboard(); return true; } return false;
      case 'o': open_dialog(); return true;
      case 'n': if (plugin_on("collage")) new_collage(); return true;
      case 'w': case 'q': hide(); return true;
      case 'g': show_in_gallery(); return true;
      case 'p': print(); return true;
      case '=': case '+': canvas_->zoom_by(1.25); return true;
      case '-': canvas_->zoom_by(1 / 1.25); return true;
      case '0': canvas_->set_zoom_mode(ZoomMode::Fit); return true;
    }
    return false;
  }
  if (alt) return false;
  switch (key) {
    case FL_Escape:
      if (canvas_->cropping()) cancel_crop();
      else if (drawing() && (canvas_->drawing_busy() || canvas_->selected_mark() >= 0)) canvas_->cancel_drawing();
      else if (fullscreen_active()) fullscreen_off();
      else if (typing) Fl::focus(canvas_);
      return true;
    case FL_F + 5: slideshow_in_gallery(); return true;
    case FL_F + 11: toggle_fullscreen(); return true;
    case FL_F + 1: show_shortcuts(); return true;
    case FL_Delete:
      if (typing) return false;
      if (drawing() && canvas_->selected_mark() >= 0) delete_selected_mark();
      else trash();
      return true;
    case FL_Enter:
    case FL_KP_Enter:
      if (canvas_->cropping()) {
        apply_crop();
        return true;
      }
      return false;
  }
  if (typing) return false;
  if (drawing() && !shift) {
    // Tool keys while the Draw tab is open.
    switch (key) {
      case 's': set_tool(Tool::Select); return true;
      case 'p': set_tool(Tool::Pen); return true;
      case 'm': set_tool(Tool::Highlighter); return true;
      case 'x': set_tool(Tool::Eraser); return true;
      case 'l': set_tool(Tool::Line); return true;
      case 'a': set_tool(Tool::Arrow); return true;
      case 'u': set_tool(Tool::Rect); return true;
      case 'o': set_tool(Tool::Ellipse); return true;
      case 't': set_tool(Tool::Text); return true;
      case 'k': set_tool(Tool::Picker); return true;
      case 'd': toggle_panel(SidePanel::Edit); return true;
    }
  }
  switch (key) {
    case FL_Left: case FL_Page_Up: case FL_BackSpace: step(-1); return true;
    case FL_Right: case FL_Page_Down: case ' ': step(1); return true;
    case FL_Home: go(0); return true;
    case FL_End: go(int(photos_.size()) - 1); return true;
    case '+': case '=': case FL_KP + '+': canvas_->zoom_by(1.25); return true;
    case '-': case FL_KP + '-': canvas_->zoom_by(1 / 1.25); return true;
    case '0': canvas_->set_zoom_mode(ZoomMode::Fit); return true;
    case '1': canvas_->set_zoom_mode(ZoomMode::Actual); return true;
    case 'f': canvas_->set_zoom_mode(ZoomMode::Fill); return true;
    case 'r': rotate(shift ? -1 : 1); return true;
    case 'l': rotate(-1); return true;
    case 'h': flip(true); return true;
    case 'v': flip(false); return true;
    case 'c': start_crop(); return true;
    case 'e': toggle_panel(SidePanel::Edit); return true;
    case 'd': open_draw(); return true;
    case 'i': toggle_panel(SidePanel::Info); return true;
    case 'b': toggle_filmstrip(); return true;
  }
  return false;
}

int PhotoWindow::handle(int event) {
  switch (event) {
    case FL_KEYBOARD:
    case FL_SHORTCUT:
      if (keyboard(Fl::event_key(), Fl::event_state())) {
        redraw();
        return 1;
      }
      break;
    case FL_MOVE:
    case FL_ENTER: {
      int mx = Fl::event_x(), my = Fl::event_y(), hb = -1;
      if (my < header_h_)
        for (size_t i = 0; i < buttons_.size(); ++i)
          if (mx >= buttons_[i].x && mx < buttons_[i].x + buttons_[i].w && my >= buttons_[i].y &&
              my < buttons_[i].y + buttons_[i].h)
            hb = int(i);
      if (hb != hover_button_) {
        hover_button_ = hb;
        redraw();
      }
      break;
    }
    case FL_LEAVE:
      if (hover_button_ >= 0) {
        hover_button_ = -1;
        redraw();
      }
      break;
    case FL_PUSH: {
      int mx = Fl::event_x(), my = Fl::event_y();
      if (my < header_h_ && !(mx >= menu_->x() && mx < menu_->x() + menu_->w())) {
        for (auto &b : buttons_)
          if (b.active && mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) {
            auto fn = b.action;
            hover_button_ = -1;
            fn();
            redraw();
            return 1;
          }
        return 1;
      }
      break;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
      return 1;
    case FL_PASTE: {
      // A file dropped on the window.
      std::string text = Fl::event_text() ? Fl::event_text() : "";
      std::string first = text.substr(0, text.find_first_of("\r\n"));
      if (starts_with(first, "file://")) {
        gchar *path = g_filename_from_uri(first.c_str(), nullptr, nullptr);
        if (path) first = path;
        g_free(path);
      }
      if (!first.empty() && first[0] == '/' && file_exists(first)) {
        if (modified() && !confirm_discard()) return 1;
        std::string dir = first.substr(0, first.rfind('/'));
        open(first, Album{AlbumKind::Directory, dir.empty() ? "/" : dir}, false, gallery::Sort::Name);
      }
      return 1;
    }
  }
  int r = BufferedWindow::handle(event);
  if (event == FL_PUSH) {
    Fl_Widget *f = Fl::focus();
    if (!f || f == this) Fl::focus(canvas_);
  }
  return r;
}

}  // namespace photo
