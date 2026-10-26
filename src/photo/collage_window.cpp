// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The collage editor's window and its preview (see collage_ui.h).
#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <glib.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef SDE_GALLERY_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include <algorithm>
#include <cmath>
#include <ctime>
#include <random>
#include <set>
#include <thread>

#include "collage_ui.h"
#include "collections.h"
#include "config.h"
#include "exif.h"
#include "i18n.h"
#include "link.h"
#include "menu.h"
#include "menubar.h"
#include "perf.h"
#include "photo.h"
#include "save.h"
#include "util.h"
#include "wm.h"
#include "x11.h"

using namespace sde;
namespace icon = gallery::icon;

namespace photo {

CollageWindow *CollageWindow::instance_ = nullptr;

namespace {

enum MenuId { M_ADD = 1, M_EXPORT, M_OPEN, M_CLOSE, M_UNDO, M_REDO, M_SHUFFLE, M_REMOVE, M_SHORTCUTS };

// The photo's upright size, from its header.
void image_size(const std::string &path, int &w, int &h) {
  w = h = 0;
#ifdef SDE_GALLERY_PIXBUF
  if (gdk_pixbuf_get_file_info(path.c_str(), &w, &h) && w > 0 && h > 0) {
    if (sniff_format(path) == Format::Jpeg && gallery::orient_from_exif(gallery::jpeg_orientation(path)).k % 2)
      std::swap(w, h);
    return;
  }
#endif
  w = h = 0;
}

// Sizes of the proxies: one step up from what the preview needs.
int proxy_step(int need) {
  for (int s : {256, 384, 512, 768, 1024, 1536, 2048})
    if (need <= s) return s;
  return 2048;
}

struct ExportJob {
  CollageWindow *self;
  bool ok;
  std::string path, error;
  bool then_open;
};

void outline_path(cairo_t *cr, const CollageCell &c, double ox, double oy, double k, double grow) {
  cairo_save(cr);
  cairo_translate(cr, ox + c.cx() * k, oy + c.cy() * k);
  if (c.angle != 0) cairo_rotate(cr, c.angle);
  double w = c.w * k + 2 * grow, h = c.h * k + 2 * grow;
  if (c.clip == CellClip::Circle) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0, 0, std::min(w, h) / 2, 0, 2 * M_PI);
  } else {
    double r = std::min(c.radius * k + grow, std::min(w, h) / 2);
    cairo_new_sub_path(cr);
    if (r < 1) {
      cairo_rectangle(cr, -w / 2, -h / 2, w, h);
    } else {
      cairo_arc(cr, w / 2 - r, -h / 2 + r, r, -M_PI / 2, 0);
      cairo_arc(cr, w / 2 - r, h / 2 - r, r, 0, M_PI / 2);
      cairo_arc(cr, -w / 2 + r, h / 2 - r, r, M_PI / 2, M_PI);
      cairo_arc(cr, -w / 2 + r, -h / 2 + r, r, M_PI, 3 * M_PI / 2);
      cairo_close_path(cr);
    }
  }
  cairo_restore(cr);
}

}  // namespace

// ================================================================= canvas

CollageCanvas::CollageCanvas(CollageWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

CollageCanvas::~CollageCanvas() {
  if (image_) cairo_surface_destroy(image_);
}

void CollageCanvas::resize(int x, int y, int w, int h) {
  bool changed = w != this->w() || h != this->h();
  Fl_Widget::resize(x, y, w, h);
  if (changed) invalidate();
}

void CollageCanvas::invalidate() {
  dirty_ = true;
  redraw();
}

void CollageCanvas::picture_rect(double &px, double &py, double &pw, double &ph) const {
  const Collage &c = app_->collage();
  double margin = S(28);
  double aw = std::max(10.0, w() - 2 * margin), ah = std::max(10.0, h() - 2 * margin);
  double k = std::min(aw / std::max(1, c.width), ah / std::max(1, c.height));
  pw = std::max(1.0, std::floor(c.width * k));
  ph = std::max(1.0, std::floor(c.height * k));
  px = std::floor(x() + (w() - pw) / 2);
  py = std::floor(y() + (h() - ph) / 2);
}

double CollageCanvas::view_scale() const {
  double px, py, pw, ph;
  picture_rect(px, py, pw, ph);
  return pw / std::max(1, app_->collage().width);
}

bool CollageCanvas::to_output(int mx, int my, double &ox, double &oy) const {
  double px, py, pw, ph;
  picture_rect(px, py, pw, ph);
  double k = view_scale();
  if (k <= 0) return false;
  ox = (mx - px) / k;
  oy = (my - py) / k;
  return mx >= px && mx < px + pw && my >= py && my < py + ph;
}

void CollageCanvas::render_preview() {
  double px, py, pw, ph;
  picture_rect(px, py, pw, ph);
  int iw = int(pw), ih = int(ph);
  if (image_ && (iw != image_w_ || ih != image_h_)) {
    cairo_surface_destroy(image_);
    image_ = nullptr;
  }
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
  cairo_t *cr = cairo_create(s);
  CollageImages img;
  img.photo = [this](int i) { return app_->proxy(i); };
  img.backdrop = [this](int i) { return app_->backdrop(i); };
  const Collage &c = app_->collage();
  draw_collage(cr, c, app_->layout_info(), pw / std::max(1, c.width), img, fast_);
  cairo_destroy(cr);
  cairo_surface_flush(s);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (perf::enabled())
    fprintf(stderr, "sde-perf: collage preview %dx%d%s, %zu photos: %.1f ms\n", iw, ih, fast_ ? " (fast)" : "",
            c.items.size(), (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
  // A new surface each time: Painter keeps a server-side copy of it.
  if (image_) cairo_surface_destroy(image_);
  image_ = s;
  image_w_ = iw;
  image_h_ = ih;
  dirty_ = false;
  if (!fast_) app_->want_images(view_scale());
}

void CollageCanvas::outline_cell(cairo_t *cr, const CollageCell &c, double grow) const {
  double px, py, pw, ph;
  picture_rect(px, py, pw, ph);
  outline_path(cr, c, px, py, view_scale(), grow);
}

void CollageCanvas::cell_screen_box(const CollageCell &c, double &bx, double &by, double &bw, double &bh) const {
  double px, py, pw, ph;
  picture_rect(px, py, pw, ph);
  double k = view_scale();
  bx = px + c.x * k;
  by = py + c.y * k;
  bw = c.w * k;
  bh = c.h * k;
}

void CollageCanvas::draw() {
  const Theme &t = app_->theme();
  const Collage &c = app_->collage();
  Color stage = t.dark ? t.background.mix(Color::hex(0x000000), 0.35) : t.surface_alt.mix(t.background, 0.4);
  std::vector<std::pair<std::string, int>> texts;
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), stage);
    if (c.items.empty()) {
      // The empty state: a drop zone.
      double zw = std::min(w() - S(64), S(460)), zh = std::min(h() - S(64), S(260));
      double zx = x() + (w() - zw) / 2, zy = y() + (h() - zh) / 2;
      p.fill_rounded(zx, zy, zw, zh, Sd(16), t.surface.alpha(0.7));
      double dash[] = {Sd(7), Sd(6)};
      cairo_set_dash(cr, dash, 2, 0);
      p.stroke_rounded(zx + 1, zy + 1, zw - 2, zh - 2, Sd(16), t.text_dim.alpha(0.6), Sd(1.6));
      cairo_set_dash(cr, nullptr, 0, 0);
      icon::draw(p, icon::Photos, zx + zw / 2 - S(24), zy + S(40), S(48), t.accent_text);
    } else {
      if (dirty_ || !image_) render_preview();
      double px, py, pw, ph;
      picture_rect(px, py, pw, ph);
      p.shadow(px, py, pw, ph, 2, t.shadow.alpha(t.shadow.a * 0.8), S(12));
      p.surface(image_, px, py);
      const CollageLayout &l = app_->layout_info();
      int sel = cell_of_item(l, app_->selected());
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      if (hover_ >= 0 && hover_ < int(l.cells.size()) && hover_ != sel && !swapping_) {
        outline_cell(cr, l.cells[size_t(hover_)], Sd(1));
        p.set(Color::hex(0xFFFFFF, 0.85));
        cairo_set_line_width(cr, Sd(2));
        cairo_stroke(cr);
      }
      if (sel >= 0 && !(swapping_ && dragging_)) {
        outline_cell(cr, l.cells[size_t(sel)], Sd(2));
        p.set(Color::hex(0xFFFFFF, 0.9));
        cairo_set_line_width(cr, Sd(5));
        cairo_stroke_preserve(cr);
        p.set(t.accent);
        cairo_set_line_width(cr, Sd(2.5));
        cairo_stroke(cr);
      }
      if (swapping_ && drop_cell_ >= 0 && drop_cell_ < int(l.cells.size())) {
        outline_cell(cr, l.cells[size_t(drop_cell_)], 0);
        p.set(t.accent.alpha(0.28));
        cairo_fill_preserve(cr);
        p.set(t.accent);
        cairo_set_line_width(cr, Sd(3));
        cairo_stroke(cr);
      }
      if (swapping_ && dragging_ && press_cell_ >= 0 && press_cell_ < int(l.cells.size())) {
        // The photo being moved follows the pointer.
        int item = l.cells[size_t(press_cell_)].item;
        cairo_surface_t *s = app_->proxy(item);
        double gs = S(84);
        double gx = ghost_x_ - gs / 2, gy = ghost_y_ - gs / 2;
        p.shadow(gx, gy, gs, gs, Sd(12), t.shadow, S(10));
        cairo_save(cr);
        p.rounded_rect(gx, gy, gs, gs, Sd(12));
        cairo_clip(cr);
        p.fill_rect(gx, gy, gs, gs, t.surface_alt);
        if (s) {
          double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
          double k = std::max(gs / sw, gs / sh);
          cairo_translate(cr, gx + (gs - sw * k) / 2, gy + (gs - sh * k) / 2);
          cairo_scale(cr, k, k);
          cairo_set_source_surface(cr, s, 0, 0);
          cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
          cairo_paint_with_alpha(cr, 0.92);
        }
        cairo_restore(cr);
        p.stroke_rounded(gx, gy, gs, gs, Sd(12), Color::hex(0xFFFFFF), Sd(2));
      }
    }
    cairo_restore(cr);
  }
  text_font(t.font_size + S(1), true);
  if (c.items.empty()) {
    double zh = std::min(h() - S(64), S(260));
    double zy = y() + (h() - zh) / 2;
    draw_text(_("Add photos to make a collage"), x(), int(zy + S(104)), w(), S(28), t.text, 1);
    text_font(t.font_size);
    draw_text(_("Drop photos here, or click + to choose them"), x(), int(zy + S(136)), w(), S(22), t.text_dim, 1);
    draw_text(_("Any number of photos · ten designs"), x(), int(zy + S(160)), w(), S(22), t.text_dim, 1);
  }
}

void CollageCanvas::context_menu(int cell) {
  const CollageLayout &l = app_->layout_info();
  if (cell < 0 || cell >= int(l.cells.size())) return;
  int item = l.cells[size_t(cell)].item;
  int n = int(app_->collage().items.size());
  app_->select(item);
  ContextMenu m;
  m.item(_("Replace Photo…"), [this, item] { app_->replace_dialog(item); }, {"image"});
  m.item(_("Reset Position"), [this, item] { app_->reset_position(item); }, {"", '0'});
  m.item(_("Zoom In"), [this] { app_->zoom_selected(1.25); }, {"", '+'});
  m.item(_("Zoom Out"), [this] { app_->zoom_selected(1 / 1.25); }, {"", '-', true, true});
  m.item(_("Move Earlier"), [this] { app_->move_selected(-1); }, {"", FL_CTRL + FL_Left, item > 0});
  m.item(_("Move Later"), [this] { app_->move_selected(1); }, {"", FL_CTRL + FL_Right, item + 1 < n, true});
  bool blurred = app_->collage().blur_background == item;
  m.check(_("Blurred Background"), blurred, [this, item] { app_->use_as_background(item); });
  m.separator();
  m.item(_("Remove from Collage"), [this, item] { app_->remove_photo(item); }, {"delete", FL_Delete});
  m.popup();
  redraw();
}

int CollageCanvas::handle(int event) {
  const CollageLayout &l = app_->layout_info();
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      double ox, oy;
      int cell = to_output(mx, my, ox, oy) ? cell_at(l, ox, oy) : -1;
      if (cell != hover_) {
        hover_ = cell;
        redraw();
      }
      window()->cursor(cell >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      window()->cursor(FL_CURSOR_DEFAULT);
      redraw();
      return 1;
    case FL_PUSH: {
      if (Fl::focus() != this) Fl::focus(this);
      if (app_->collage().items.empty()) {
        app_->add_dialog();
        return 1;
      }
      double ox, oy;
      int cell = to_output(mx, my, ox, oy) ? cell_at(l, ox, oy) : -1;
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        context_menu(cell);
        return 1;
      }
      if (cell < 0) {
        app_->select(-1);
        return 1;
      }
      int item = l.cells[size_t(cell)].item;
      app_->select(item);
      if (Fl::event_clicks()) {
        Fl::event_clicks(0);
        app_->reset_position(item);
        return 1;
      }
      dragging_ = true;
      swapping_ = panned_ = false;
      press_cell_ = cell;
      drop_cell_ = -1;
      last_x_ = ghost_x_ = mx;
      last_y_ = ghost_y_ = my;
      press_collage_ = app_->collage();
      return 1;
    }
    case FL_DRAG: {
      if (!dragging_ || press_cell_ < 0 || press_cell_ >= int(l.cells.size())) return 1;
      double ox, oy;
      int target = to_output(mx, my, ox, oy) ? cell_at(l, ox, oy) : -1;
      ghost_x_ = mx;
      ghost_y_ = my;
      if (target >= 0 && target != press_cell_) {
        // Onto another photo: swap them on release; undo the panning.
        if (!swapping_) {
          swapping_ = true;
          if (panned_) app_->apply(press_collage_, true);
          panned_ = false;
        }
        drop_cell_ = target;
        window()->cursor(FL_CURSOR_MOVE);
        redraw();
        return 1;
      }
      if (swapping_) {
        swapping_ = false;
        drop_cell_ = -1;
        last_x_ = mx;
        last_y_ = my;
        window()->cursor(FL_CURSOR_HAND);
        redraw();
        if (target != press_cell_) return 1;
      }
      // Within its cell: move the photo in its frame.
      const CollageCell &cell = l.cells[size_t(press_cell_)];
      double k = view_scale();
      double dx = (mx - last_x_) / k, dy = (my - last_y_) / k;
      last_x_ = mx;
      last_y_ = my;
      if (cell.angle != 0) {
        double cs = std::cos(-cell.angle), sn = std::sin(-cell.angle);
        double rx = dx * cs - dy * sn, ry = dx * sn + dy * cs;
        dx = rx;
        dy = ry;
      }
      Collage c = app_->collage();
      if (cell.item < 0 || cell.item >= int(c.items.size())) return 1;
      double inner_w = std::max(1.0, cell.w - 2 * cell.border), inner_h = std::max(1.0, cell.h - 2 * cell.border);
      pan_item(c.items[size_t(cell.item)], inner_w, inner_h, dx, dy);
      fast_ = true;
      app_->apply(c, panned_);
      panned_ = true;
      return 1;
    }
    case FL_RELEASE: {
      if (!dragging_) return 1;
      dragging_ = false;
      bool was_fast = fast_;
      fast_ = false;
      if (swapping_ && drop_cell_ >= 0 && drop_cell_ < int(l.cells.size()) && press_cell_ >= 0) {
        Collage c = app_->collage();
        int a = l.cells[size_t(press_cell_)].item, b = l.cells[size_t(drop_cell_)].item;
        swap_items(c, a, b);
        app_->apply(c);
        app_->select(b);
        app_->toast(_("Photos swapped · Ctrl+Z undoes it"));
      }
      swapping_ = false;
      drop_cell_ = press_cell_ = -1;
      window()->cursor(hover_ >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
      if (was_fast) invalidate();
      redraw();
      return 1;
    }
    case FL_MOUSEWHEEL: {
      double ox, oy;
      int cell = to_output(mx, my, ox, oy) ? cell_at(l, ox, oy) : -1;
      if (cell < 0 || !Fl::event_dy()) return 1;
      const CollageCell &cl = l.cells[size_t(cell)];
      double dx = ox - cl.cx(), dy = oy - cl.cy();
      if (cl.angle != 0) {
        double cs = std::cos(-cl.angle), sn = std::sin(-cl.angle);
        double rx = dx * cs - dy * sn, ry = dx * sn + dy * cs;
        dx = rx;
        dy = ry;
      }
      double fx = dx / cl.w + 0.5, fy = dy / cl.h + 0.5;
      Collage c = app_->collage();
      if (cl.item < 0 || cl.item >= int(c.items.size())) return 1;
      zoom_item(c.items[size_t(cl.item)], cl.w, cl.h, std::pow(1.15, -Fl::event_dy()), fx, fy);
      // One undo step for a series of wheel turns on the same photo.
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      double now = ts.tv_sec + ts.tv_nsec / 1e9;
      bool amend = app_->wheel_item_ == cl.item && now - app_->wheel_time_ < 0.8;
      app_->wheel_item_ = cl.item;
      app_->wheel_time_ = now;
      app_->select(cl.item);
      app_->apply(c, amend);
      return 1;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
      return 1;
    case FL_PASTE: {
      std::vector<std::string> paths;
      for (const auto &line : split(Fl::event_text() ? Fl::event_text() : "", '\n')) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        if (starts_with(s, "file://")) {
          gchar *p = g_filename_from_uri(s.c_str(), nullptr, nullptr);
          s = p ? p : "";
          g_free(p);
        }
        if (!s.empty() && s[0] == '/') paths.push_back(s);
      }
      app_->add_photos(paths);
      return 1;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
      return 1;
  }
  return Fl_Widget::handle(event);
}

// ================================================================= window

CollageWindow::CollageWindow(int w, int h) : BufferedWindow(w, h, _("Collage")) {
  instance_ = this;
  theme_ = active_theme();
  service_.reset(new gallery::ThumbnailService([this] { Fl::awake(awake_cb, this); },
                                               int(std::max(2u, std::min(4u, std::thread::hardware_concurrency())))));
  Collage c;
  Config cfg;
  cfg.load();
  CollageDesign d = CollageDesign::Grid;
  parse_design(cfg.get("photo", "collage_design", "grid"), d);
  set_design(c, d);
  history_.reset(c);
  begin();
  canvas_ = new CollageCanvas(this, 0, 0, w, h);
  panel_ = new CollagePanel(this, w - S(320), 0, S(320), h);
  menu_ = new AppMenu(0, 0, S(40), S(40));
  end();
  resizable(nullptr);
  size_range(S(560), S(400));
  xclass("Sdeview");
  build_menu();
  apply_theme();
  layout();
  changed();
}

CollageWindow::~CollageWindow() {
  Fl::remove_timeout(toast_cb, this);
  instance_ = nullptr;
  service_.reset();
  for (auto &kv : images_) {
    if (kv.second.proxy) cairo_surface_destroy(kv.second.proxy);
    if (kv.second.thumb) cairo_surface_destroy(kv.second.thumb);
    if (kv.second.backdrop) cairo_surface_destroy(kv.second.backdrop);
  }
}

void CollageWindow::hide() {
  if (!confirm_close()) return;
  Config cfg;
  cfg.load();
  cfg.set("photo", "collage_design", design_info(collage().design).key);
  cfg.save();
  BufferedWindow::hide();
}

bool CollageWindow::confirm_close() {
  if (!unsaved_ || collage().items.empty() || exporting_) return true;
  int r = gallery::ask(_("Close the Collage"),
                       _("The collage has not been saved.\nIf you close it without exporting, it is lost."),
                       {_("Cancel"), _("Close Without Saving"), _("Export…")});
  if (r == 0) return false;
  if (r == 2) {
    export_dialog();
    return false;
  }
  return true;
}

void CollageWindow::apply_theme() {
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

void CollageWindow::build_menu() {
  menu_->clear();
  auto add = [this](const std::string &label, int sc, long id, int flags = 0) {
    menu_->add(label.c_str(), sc, menu_cb, reinterpret_cast<void *>(id), flags);
  };
  std::string f = std::string(_("Collage")) + "/", e = std::string(_("Edit")) + "/", h = std::string(_("Help")) + "/";
  add(f + _("Add Photos…"), FL_CTRL + 'o', M_ADD);
  add(f + _("Export…"), FL_CTRL + 's', M_EXPORT);
  add(f + _("Open in Photo"), FL_CTRL + 'e', M_OPEN, FL_MENU_DIVIDER);
  add(f + _("Close"), FL_CTRL + 'w', M_CLOSE);
  add(e + _("Undo"), FL_CTRL + 'z', M_UNDO);
  add(e + _("Redo"), FL_CTRL + FL_SHIFT + 'z', M_REDO, FL_MENU_DIVIDER);
  add(e + _("Shuffle Photos"), 0, M_SHUFFLE);
  add(e + _("Remove Photo"), FL_Delete, M_REMOVE);
  add(h + _("Keyboard Shortcuts"), FL_F + 1, M_SHORTCUTS);
}

void CollageWindow::menu_cb(Fl_Widget *, void *data) {
  CollageWindow *self = instance_;
  if (!self) return;
  switch (reinterpret_cast<long>(data)) {
    case M_ADD: self->add_dialog(); break;
    case M_EXPORT: self->export_dialog(); break;
    case M_OPEN: self->open_in_photo(); break;
    case M_CLOSE: self->hide(); break;
    case M_UNDO: self->undo(); break;
    case M_REDO: self->redo(); break;
    case M_SHUFFLE: self->shuffle(); break;
    case M_REMOVE: self->remove_photo(self->selected_); break;
    case M_SHORTCUTS: self->show_shortcuts(); break;
  }
  self->redraw();
}

// ----------------------------------------------------------------- changes

void CollageWindow::changed() {
  layout_ = layout_collage(collage());
  if (selected_ >= int(collage().items.size())) selected_ = -1;
  canvas_->invalidate();
  panel_->sync();
  int n = int(collage().items.size());
  std::string t = n ? format(n == 1 ? _("Collage of %d photo") : _("Collage of %d photos"), n) : _("New Collage");
  t += std::string(unsaved_ ? " *" : "") + " – " + _("Photo");
  copy_label(t.c_str());
  redraw();
}

void CollageWindow::apply(const Collage &c, bool amend) {
  if (amend) history_.amend(c);
  else history_.push(c);
  unsaved_ = true;
  changed();
}

void CollageWindow::undo() {
  if (!history_.can_undo()) return;
  history_.undo();
  unsaved_ = true;
  changed();
}

void CollageWindow::redo() {
  if (!history_.can_redo()) return;
  history_.redo();
  unsaved_ = true;
  changed();
}

void CollageWindow::refresh_title_suggestion(Collage &c) const {
  // The folder the photos share, else the month of the newest.
  std::string dir;
  bool same = !c.items.empty();
  time_t newest = 0;
  for (const auto &it : c.items) {
    std::string d = it.path.substr(0, it.path.rfind('/'));
    if (dir.empty()) dir = d;
    else if (d != dir) same = false;
    auto f = images_.find(it.path);
    if (f != images_.end()) newest = std::max(newest, f->second.mtime);
  }
  std::string pictures = gallery::pictures_dir();
  if (same && !dir.empty() && dir != pictures && dir != home_dir() && basename_of(dir) != "DCIM")
    c.suggested_title = basename_of(dir);
  else
    c.suggested_title = newest ? gallery::month_label(newest) : std::string();
}

void CollageWindow::add_photos(const std::vector<std::string> &paths) {
  Collage c = collage();
  int added = 0, skipped = 0;
  std::set<std::string> extra = gallery::extra_formats();
  for (const auto &p : paths) {
    struct stat st {};
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      ++skipped;
      continue;
    }
    CollageItem it;
    it.path = p;
    image_size(p, it.w, it.h);
    // A picture by its name, or one that the image loaders can read (Photo
    // opens it as well: PNM, uncommon names).
    if (!gallery::is_image_name(basename_of(p), extra) && (it.w <= 0 || it.h <= 0)) {
      ++skipped;
      continue;
    }
    Image &img = images_[p];
    img.mtime = st.st_mtime;
    img.natural = std::max(it.w, it.h);
    c.items.push_back(it);
    ++added;
  }
  if (!added) {
    if (skipped) toast(_("These files are not photos Photo can read"));
    return;
  }
  refresh_title_suggestion(c);
  apply(c);
  if (selected_ < 0) select(int(c.items.size()) - added);
  if (skipped) toast(format(_("%d files were not added: they are not photos"), skipped));
  else if (added > 1 || c.items.size() > 1)
    toast(format(added == 1 ? _("%d photo added") : _("%d photos added"), added));
}

void CollageWindow::add_dialog() {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
  chooser.title(_("Add Photos to the Collage"));
  chooser.filter(_("Images\t*.{jpg,jpeg,png,gif,bmp,svg,webp,tif,tiff,avif,heic,jxl}"));
  std::string dir = gallery::pictures_dir();
  if (!collage().items.empty()) {
    const std::string &last = collage().items.back().path;
    dir = last.substr(0, last.rfind('/'));
  }
  chooser.directory(dir.c_str());
  if (chooser.show() != 0) return;
  std::vector<std::string> paths;
  for (int i = 0; i < chooser.count(); ++i)
    if (chooser.filename(i)) paths.push_back(chooser.filename(i));
  add_photos(paths);
}

void CollageWindow::replace_dialog(int item) {
  if (item < 0 || item >= int(collage().items.size())) return;
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.title(_("Replace the Photo"));
  chooser.filter(_("Images\t*.{jpg,jpeg,png,gif,bmp,svg,webp,tif,tiff,avif,heic,jxl}"));
  const std::string &cur = collage().items[size_t(item)].path;
  chooser.directory(cur.substr(0, cur.rfind('/')).c_str());
  if (chooser.show() != 0 || !chooser.filename()) return;
  std::string p = chooser.filename();
  struct stat st {};
  if (stat(p.c_str(), &st) != 0) return;
  Collage c = collage();
  CollageItem it;
  it.path = p;
  image_size(p, it.w, it.h);
  images_[p].mtime = st.st_mtime;
  images_[p].natural = std::max(it.w, it.h);
  c.items[size_t(item)] = it;
  refresh_title_suggestion(c);
  apply(c);
}

void CollageWindow::remove_photo(int item) {
  if (item < 0 || item >= int(collage().items.size())) return;
  Collage c = collage();
  remove_item(c, item);
  refresh_title_suggestion(c);
  apply(c);
  if (selected_ >= int(c.items.size())) selected_ = int(c.items.size()) - 1;
  toast(_("Photo removed · Ctrl+Z brings it back"));
  changed();
}

void CollageWindow::select(int item) {
  if (item >= int(collage().items.size())) item = -1;
  if (item == selected_) return;
  selected_ = item;
  panel_->sync();
  canvas_->redraw();
}

void CollageWindow::choose_design(CollageDesign d) {
  if (collage().design == d) return;
  Collage c = collage();
  set_design(c, d);
  apply(c);
}

void CollageWindow::shuffle() {
  Collage c = collage();
  if (c.items.size() < 2) return;
  std::mt19937 rng(unsigned(time(nullptr)) ^ c.seed * 2654435761u);
  std::string blurred = c.blur_background >= 0 ? c.items[size_t(c.blur_background)].path : "";
  std::shuffle(c.items.begin(), c.items.end(), rng);
  for (auto &it : c.items) it.pan_x = it.pan_y = 0, it.zoom = 1;
  c.blur_background = -1;
  for (size_t i = 0; i < c.items.size(); ++i)
    if (!blurred.empty() && c.items[i].path == blurred) c.blur_background = int(i);
  c.seed = c.seed * 1103515245u + 12345u;
  apply(c);
  toast(_("Photos shuffled · Ctrl+Z undoes it"));
}

void CollageWindow::reset_position(int item) {
  if (item < 0 || item >= int(collage().items.size())) return;
  Collage c = collage();
  CollageItem &it = c.items[size_t(item)];
  it.pan_x = it.pan_y = 0;
  it.zoom = 1;
  apply(c);
}

void CollageWindow::zoom_selected(double factor) {
  if (selected_ < 0 || selected_ >= int(collage().items.size())) return;
  int cell = cell_of_item(layout_, selected_);
  if (cell < 0) return;
  Collage c = collage();
  const CollageCell &cl = layout_.cells[size_t(cell)];
  zoom_item(c.items[size_t(selected_)], cl.w, cl.h, factor, 0.5, 0.5);
  apply(c);
}

void CollageWindow::move_selected(int delta) {
  int n = int(collage().items.size()), to = selected_ + delta;
  if (selected_ < 0 || to < 0 || to >= n) return;
  Collage c = collage();
  move_item(c, selected_, to);
  apply(c);
  selected_ = to;
  changed();
}

void CollageWindow::use_as_background(int item) {
  Collage c = collage();
  c.blur_background = c.blur_background == item ? -1 : item;
  apply(c);
}

// ------------------------------------------------------------------ images

cairo_surface_t *CollageWindow::proxy(int item) {
  if (item < 0 || item >= int(collage().items.size())) return nullptr;
  auto it = images_.find(collage().items[size_t(item)].path);
  if (it == images_.end()) return nullptr;
  return it->second.proxy ? it->second.proxy : it->second.thumb;
}

cairo_surface_t *CollageWindow::thumb(const std::string &path) {
  auto it = images_.find(path);
  if (it == images_.end()) return nullptr;
  return it->second.thumb ? it->second.thumb : it->second.proxy;
}

cairo_surface_t *CollageWindow::backdrop(int item) {
  if (item < 0 || item >= int(collage().items.size())) return nullptr;
  auto it = images_.find(collage().items[size_t(item)].path);
  if (it == images_.end()) return nullptr;
  Image &img = it->second;
  if (!img.backdrop) {
    cairo_surface_t *src = img.proxy ? img.proxy : img.thumb;
    if (src) img.backdrop = blurred_backdrop(src);
  }
  return img.backdrop;
}

void CollageWindow::want_images(double scale) {
  const Collage &c = collage();
  std::vector<gallery::ThumbnailService::Request> thumbs;
  for (size_t i = 0; i < c.items.size(); ++i) {
    const std::string &path = c.items[i].path;
    Image &img = images_[path];
    if (img.failed) continue;
    if (!img.thumb) thumbs.push_back({path, img.mtime, gallery::JobKind::Large});
    int need = proxy_step(int(needed_pixels(layout_, c, int(i), scale) * 1.15));
    if (img.natural > 0) need = std::min(need, std::max(256, img.natural));
    if (need > img.dim && need > img.requested) {
      img.requested = need;
      service_->load_full(path, need, uint64_t(need));
    }
  }
  if (!thumbs.empty()) service_->replace_queue(std::move(thumbs));
}

void CollageWindow::awake_cb(void *p) {
  auto *self = static_cast<CollageWindow *>(p);
  if (instance_ != self) return;
  bool any = false, thumbs = false;
  for (auto &r : self->service_->take_results()) {
    auto it = self->images_.find(r.path);
    if (it == self->images_.end()) {
      if (r.surface) cairo_surface_destroy(r.surface);
      continue;
    }
    Image &img = it->second;
    if (r.width > 0 && r.height > 0) img.natural = std::max(r.width, r.height);
    if (!r.surface) {
      if (r.kind == gallery::JobKind::Full && !img.proxy) img.failed = true;
      continue;
    }
    if (r.kind == gallery::JobKind::Full) {
      if (r.max_dim >= img.dim) {
        if (img.proxy) cairo_surface_destroy(img.proxy);
        img.proxy = r.surface;
        img.dim = r.max_dim;
        if (img.backdrop) cairo_surface_destroy(img.backdrop);
        img.backdrop = nullptr;
        any = true;
      } else {
        cairo_surface_destroy(r.surface);
      }
    } else {
      if (img.thumb) cairo_surface_destroy(img.thumb);
      img.thumb = r.surface;
      thumbs = true;
      if (!img.proxy) any = true;
    }
    // Photos whose size was not known: the layout may change.
    for (auto &item : self->history_.current().items)
      if (item.path == r.path && (item.w <= 0 || item.h <= 0) && r.width > 0) {
        Collage c = self->collage();
        for (auto &ci : c.items)
          if (ci.path == r.path) {
            ci.w = r.width;
            ci.h = r.height;
          }
        self->history_.amend(c);
        self->layout_ = layout_collage(c);
        break;
      }
  }
  if (any && !self->canvas_->busy()) self->canvas_->invalidate();
  if (thumbs || any) self->panel_->previews_changed();
}

// ------------------------------------------------------------------ output

std::string CollageWindow::default_export_path(const char *ext) const {
  std::string dir = gallery::pictures_dir() + "/" + _("Collages");
  make_dirs(dir);
  char date[32];
  time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  strftime(date, sizeof date, "%Y-%m-%d", &tm);
  std::string name = format(_("Collage %s"), date) + "." + ext;
  return gallery::unique_destination(dir, name, [](const std::string &p) { return file_exists(p); });
}

void CollageWindow::export_dialog() {
  if (collage().items.empty()) {
    toast(_("Add photos first"));
    return;
  }
  if (exporting_) return;
  std::string suggestion = default_export_path("jpg");
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  chooser.title(_("Export the Collage"));
  chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::NEW_FOLDER);
  chooser.directory(suggestion.substr(0, suggestion.rfind('/')).c_str());
  chooser.preset_file(basename_of(suggestion).c_str());
  chooser.filter(_("JPEG\t*.{jpg,jpeg}\nPNG\t*.png"));
  if (chooser.show() != 0 || !chooser.filename()) return;
  std::string dest = chooser.filename();
  Format f = format_of(dest);
  if (f != Format::Jpeg && f != Format::Png) {
    dest = replace_extension(dest, chooser.filter_value() == 1 ? Format::Png : Format::Jpeg);
    f = format_of(dest);
  }
  if (f == Format::Jpeg && !can_write(Format::Jpeg)) {
    dest = replace_extension(dest, Format::Png);
    f = Format::Png;
  }
  int quality = 92;
  if (f == Format::Jpeg) {
    Config cfg;
    cfg.load();
    quality = std::max(30, std::min(100, cfg.get_int("photo", "jpeg_quality", 92)));
    if (gallery::ask(_("Export the Collage"),
                     format(_("Save “%s” (%d × %d) with this quality:"), basename_of(dest).c_str(), collage().width,
                            collage().height),
                     {_("Cancel"), _("Export")}, &quality) == 0)
      return;
  }
  export_to(dest, quality, false);
}

void CollageWindow::open_in_photo() {
  if (collage().items.empty()) {
    toast(_("Add photos first"));
    return;
  }
  if (exporting_) return;
  export_to(default_export_path("jpg"), 92, true);
}

void CollageWindow::export_to(const std::string &path, int quality, bool then_open) {
  exporting_ = true;
  toast(_("Making the collage at full size…"));
  fl_cursor(FL_CURSOR_WAIT);
  Collage c = collage();
  auto *job = new ExportJob{this, false, path, "", then_open};
  std::thread([job, c, quality] {
    CollageLayout l = layout_collage(c);
    // Every photo decoded at the size the output needs, no larger.
    std::vector<cairo_surface_t *> images(c.items.size(), nullptr);
    for (size_t i = 0; i < c.items.size(); ++i) {
      int need = needed_pixels(l, c, int(i), 1.0);
      if (c.blur_background == int(i)) need = std::max(need, 256);
      if (need > 0) images[i] = gallery::decode_image(c.items[i].path, std::min(8192, need + 8));
    }
    CollageImages img;
    img.photo = [&](int i) { return i >= 0 && i < int(images.size()) ? images[size_t(i)] : nullptr; };
    cairo_surface_t *out = render_collage(c, 1.0, img);
    for (auto *s : images)
      if (s) cairo_surface_destroy(s);
    if (out) {
      job->ok = write_image(out, job->path, format_of(job->path), quality, "", &job->error);
      cairo_surface_destroy(out);
    } else {
      job->error = "out of memory";
    }
    Fl::awake([](void *p) {
      auto *job = static_cast<ExportJob *>(p);
      if (instance_ == job->self) job->self->exported(job->ok, job->path, job->error, job->then_open);
      delete job;
    }, job);
  }).detach();
}

void CollageWindow::exported(bool ok, const std::string &path, const std::string &error, bool then_open) {
  exporting_ = false;
  fl_cursor(FL_CURSOR_DEFAULT);
  if (!ok) {
    gallery::ask(_("Export the Collage"), format(_("The collage could not be saved:\n%s"), error.c_str()),
                 {_("Close")});
    return;
  }
  unsaved_ = false;
  gallery::send_gallery_events({{"added", path}});
  changed();
  if (then_open) {
    PhotoWindow::show_file(path);
    toast(format(_("Saved “%s” and opened it in Photo"), basename_of(path).c_str()));
  } else {
    toast(format(_("Saved “%s”"), basename_of(path).c_str()));
  }
}

void CollageWindow::toast(const std::string &message) {
  toast_ = message;
  Fl::remove_timeout(toast_cb, this);
  Fl::add_timeout(3.2, toast_cb, this);
  redraw();
}

void CollageWindow::toast_cb(void *p) {
  auto *self = static_cast<CollageWindow *>(p);
  self->toast_.clear();
  self->redraw();
}

void CollageWindow::show_shortcuts() {
  gallery::show_shortcuts(_("Collage Keyboard Shortcuts"),
                          {{"", _("Photos")},
                           {_("Ctrl+O"), _("Add photos")},
                           {_("Drag a photo"), _("Move it in its frame")},
                           {_("Drag onto another"), _("Swap the two photos")},
                           {_("Wheel, + −"), _("Zoom the photo in its frame")},
                           {_("Double click, 0"), _("Reset the photo's position")},
                           {_("Left, Right"), _("Select the previous or next photo")},
                           {_("Ctrl+Left, Ctrl+Right"), _("Move the photo earlier or later")},
                           {_("Delete"), _("Remove the photo from the collage")},
                           {"", _("Collage")},
                           {_("Ctrl+1 … Ctrl+0"), _("Choose one of the ten designs")},
                           {_("Ctrl+Z, Ctrl+Shift+Z"), _("Undo, redo")},
                           {_("Ctrl+S"), _("Export")},
                           {_("Ctrl+E"), _("Save and open in Photo")},
                           {_("Ctrl+W"), _("Close")}});
}

// ------------------------------------------------------------------ layout

void CollageWindow::resize(int x, int y, int w, int h) {
  BufferedWindow::resize(x, y, w, h);
  layout();
}

void CollageWindow::layout() {
  header_h_ = S(56);
  // Small screens keep more room for the collage.
  int pw = w() >= S(900) ? S(320) : S(272);
  canvas_->resize(0, header_h_, w() - pw, h() - header_h_);
  panel_->resize(w() - pw, header_h_, pw, h() - header_h_);
  menu_->resize(w() - S(50), S(8), S(40), S(40));
}

void CollageWindow::layout_buttons() {
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
  add(icon::Plus, _("Add Photos (Ctrl+O)"), [this] { add_dialog(); })->x = S(8);
  size_t right = buttons_.size();
  bool have = !collage().items.empty();
  auto *u = add(icon::Undo, _("Undo (Ctrl+Z)"), [this] { undo(); });
  u->active = history_.can_undo();
  auto *r = add(icon::Redo, _("Redo (Ctrl+Shift+Z)"), [this] { redo(); });
  r->active = history_.can_redo();
  auto *o = add(icon::Edit, _("Save and Open in Photo (Ctrl+E)"), [this] { open_in_photo(); });
  o->active = have && !exporting_;
  auto *s = add(icon::Save, _("Export (Ctrl+S)"), [this] { export_dialog(); });
  s->active = have && !exporting_;
  s->toggled = have;
  double rx = menu_->x() - S(6);
  for (size_t i = buttons_.size(); i-- > right;) {
    if (i + 1 == buttons_.size() - 1) rx -= S(10);
    rx -= bs;
    buttons_[i].x = rx;
    rx -= S(2);
  }
}

void CollageWindow::draw() {
  layout_buttons();
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), theme_.background);
    p.fill_rect(0, 0, w(), header_h_, theme_.surface);
    if (theme_.glass) p.glass_sheen(0, 0, w(), header_h_, 0, theme_.dark);
    p.line(0, header_h_ - 0.5, w(), header_h_ - 0.5, theme_.border);
    for (size_t i = 0; i < buttons_.size(); ++i) {
      const Button &b = buttons_[i];
      bool hot = int(i) == hover_button_ && b.active;
      if (b.toggled && b.active) p.fill_rounded(b.x, b.y, b.w, b.h, Sd(10), theme_.accent.alpha(theme_.dark ? 0.3 : 0.16));
      else if (hot) p.fill_rounded(b.x, b.y, b.w, b.h, Sd(10), theme_.hover);
      Color c = !b.active ? theme_.text_dim.alpha(0.45) : b.toggled ? theme_.accent_text : theme_.text;
      icon::draw(p, b.glyph, b.x + S(9), b.y + S(9), S(22), c);
    }
  }
  draw_children();
  int nx = buttons_.empty() ? S(16) : int(buttons_[0].x + buttons_[0].w + S(10));
  int nw = (buttons_.size() > 1 ? int(buttons_[1].x) : w() - S(60)) - S(12) - nx;
  const Collage &c = collage();
  text_font(theme_.font_size + S(1), true);
  std::string name = c.shown_title().empty() ? std::string(_("Collage")) : format(_("Collage: %s"), c.shown_title().c_str());
  draw_text(name + (unsaved_ ? "  •" : ""), nx, S(9), nw, S(22), theme_.text);
  text_font(theme_.font_size - S(1));
  int preset = size_preset(c.width, c.height);
  std::string sub = format(c.items.size() == 1 ? _("%zu photo") : _("%zu photos"), c.items.size());
  sub += "  ·  " + std::string(_(design_info(c.design).name));
  sub += "  ·  " + (preset >= 0 ? std::string(_(collage_sizes()[size_t(preset)].name)) + "  " : std::string()) +
         format("%d × %d", c.width, c.height);
  if (exporting_) sub += "  ·  " + std::string(_("saving…"));
  draw_text(sub, nx, S(30), nw, S(18), theme_.text_dim);
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
  if (!toast_.empty()) {
    text_font(theme_.font_size);
    int tw = std::min(text_width(toast_) + S(40), canvas_->w() - S(40));
    int th = S(40);
    int tx = canvas_->x() + (canvas_->w() - tw) / 2, ty = canvas_->y() + canvas_->h() - th - S(20);
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

// ------------------------------------------------------------------ events

bool CollageWindow::keyboard(int key, int state) {
  bool ctrl = state & FL_CTRL, shift = state & FL_SHIFT, alt = state & FL_ALT;
  bool typing = panel_->editing_text();
  if (ctrl) {
    if (key >= '1' && key <= '9' && !shift) {
      choose_design(collage_designs()[size_t(key - '1')].id);
      return true;
    }
    switch (key) {
      case '0': choose_design(collage_designs()[9].id); return true;
      case 'z': shift ? redo() : undo(); return true;
      case 'y': redo(); return true;
      case 's': export_dialog(); return true;
      case 'e': open_in_photo(); return true;
      case 'o': add_dialog(); return true;
      case 'w': case 'q': hide(); return true;
      case FL_Left: move_selected(-1); return true;
      case FL_Right: move_selected(1); return true;
    }
    return false;
  }
  if (alt || typing) return false;
  int n = int(collage().items.size());
  switch (key) {
    case FL_F + 1: show_shortcuts(); return true;
    case FL_Escape: select(-1); return true;
    case FL_Delete: case FL_BackSpace: remove_photo(selected_); return true;
    case FL_Left: case FL_Up: if (n) select(selected_ <= 0 ? n - 1 : selected_ - 1); return true;
    case FL_Right: case FL_Down: if (n) select(selected_ < 0 || selected_ + 1 >= n ? 0 : selected_ + 1); return true;
    case '+': case '=': case FL_KP + '+': zoom_selected(1.25); return true;
    case '-': case FL_KP + '-': zoom_selected(1 / 1.25); return true;
    case '0': reset_position(selected_); return true;
  }
  return false;
}

int CollageWindow::handle(int event) {
  switch (event) {
    case FL_KEYBOARD:
    case FL_SHORTCUT:
      if (keyboard(Fl::event_key(), Fl::event_state())) {
        redraw();
        return 1;
      }
      if (Fl::event_key() == FL_Escape) return 1;  // never close on Escape
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
    case FL_PASTE:
      return canvas_->handle(FL_PASTE);
  }
  return BufferedWindow::handle(event);
}

// ---------------------------------------------------------------- opening

CollageWindow *open_collage(const std::vector<std::string> &paths, bool choose) {
  CollageWindow *win = CollageWindow::instance();
  if (!win) {
    int wx = 0, wy = 0, w = S(1280), h = S(840);
    fit_to_work_area(wx, wy, w, h);
    win = new CollageWindow(std::max(w, S(560)), std::max(h, S(400)));
    win->position(wx, wy);
    win->show();
    XClassHint hint{const_cast<char *>("sde-view"), const_cast<char *>("Sdeview")};
    XSetClassHint(fl_display, fl_xid(win), &hint);
  } else {
    win->show();
    x11::activate_window(fl_xid(win));
  }
  win->add_photos(paths);
  if (choose) win->add_dialog();
  return win;
}

}  // namespace photo
