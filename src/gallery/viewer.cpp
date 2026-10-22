// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Gallery's quick preview: a large view with zoom, slideshows, details and
// actions. Editing is Photo's: "Edit in Photo" opens the photo there.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "gallery.h"
#include "i18n.h"
#include "util.h"

using namespace sde;

namespace gallery {

namespace {

constexpr int kFullMax = 6144;

// Greedy line breaking that also splits long words (paths, file names).
std::vector<std::string> wrap(const std::string &text, int width, size_t max_lines) {
  std::vector<std::string> lines;
  std::string line;
  size_t i = 0;
  while (i < text.size()) {
    size_t n = 1;
    while (i + n < text.size() && (uchar(text[i + n]) & 0xC0) == 0x80) ++n;
    std::string cp = text.substr(i, n);
    if (text_width(line + cp) > width && !line.empty()) {
      // Prefer breaking after a space or slash.
      size_t cut = line.find_last_of(" /_-");
      std::string rest;
      if (cut != std::string::npos && cut > line.size() / 2) {
        rest = line.substr(cut + 1);
        line = line.substr(0, cut + 1);
      }
      lines.push_back(line);
      line = rest;
      if (lines.size() + 1 == max_lines) {
        lines.push_back(ellipsize(line + text.substr(i), width));
        return lines;
      }
    }
    line += cp;
    i += n;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

std::string type_name(const std::string &path) {
  std::string e = extension_of(path);
  if (e == "jpg" || e == "jpeg" || e == "jpe" || e == "jfif") return _("JPEG image");
  if (e == "png") return _("PNG image");
  if (e == "gif") return _("GIF image");
  if (e == "bmp") return _("BMP image");
  if (e == "svg" || e == "svgz") return _("SVG image");
  if (e == "webp") return _("WebP image");
  if (e == "tif" || e == "tiff") return _("TIFF image");
  std::string up = e;
  for (auto &c : up) c = char(toupper(uchar(c)));
  return format(_("%s image"), up.c_str());
}

}  // namespace

Viewer::Viewer(GalleryWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

Viewer::~Viewer() {
  Fl::remove_timeout(slideshow_cb, this);
  if (full_) cairo_surface_destroy(full_);
}

const Photo *Viewer::photo() const {
  const auto &photos = app_->photos();
  if (index_ < 0 || index_ >= int(photos.size())) return nullptr;
  return &photos[size_t(index_)];
}

void Viewer::open(int index) {
  index_ = index;
  info_ = w() >= S(900) ? info_ : false;
  load_current();
}

void Viewer::close() {
  stop_slideshow();
  if (full_) cairo_surface_destroy(full_);
  full_ = nullptr;
  full_path_.clear();
  ++generation_;
}

void Viewer::load_current() {
  const Photo *p = photo();
  if (full_) cairo_surface_destroy(full_);
  full_ = nullptr;
  failed_ = false;
  zoom_ = 0;
  pan_x_ = pan_y_ = 0;
  if (!p) return;
  full_path_ = p->path;
  app_->dimensions(p->path, img_w_, img_h_);
  ++generation_;
  app_->service().load_full(p->path, kFullMax, generation_);
  // Make sure a placeholder is on its way as well.
  app_->request_thumbnails({{p, true}});
  redraw();
}

void Viewer::full_loaded(const Result &r) {
  if (r.generation != generation_ || r.path != full_path_) {
    if (r.surface) cairo_surface_destroy(r.surface);
    return;
  }
  if (full_) cairo_surface_destroy(full_);
  full_ = r.surface;
  failed_ = !r.surface;
  if (r.width > 0 && r.height > 0) {
    img_w_ = r.width;
    img_h_ = r.height;
  }
  redraw();
}

void Viewer::photos_changed() {
  int n = int(app_->photos().size());
  if (n == 0) {
    Fl::add_timeout(0, [](void *p) { static_cast<GalleryWindow *>(p)->close_viewer(); }, app_);
    return;
  }
  // Follow the photo shown before, or stay at the same position.
  for (int i = 0; i < n; ++i)
    if (app_->photos()[size_t(i)].path == full_path_) {
      index_ = i;
      redraw();
      return;
    }
  index_ = std::max(0, std::min(n - 1, index_));
  load_current();
}

void Viewer::step(int delta) {
  int n = int(app_->photos().size());
  if (n == 0) return;
  if (slideshow_ && !subset_.empty()) {
    // A slideshow of a set of photos.
    auto it = std::find(subset_.begin(), subset_.end(), index_);
    int pos = it == subset_.end() ? -1 : int(it - subset_.begin());
    int m = int(subset_.size());
    int next = ((pos + delta) % m + m) % m;
    if (subset_[size_t(next)] < n && subset_[size_t(next)] != index_) {
      index_ = subset_[size_t(next)];
      load_current();
    }
    return;
  }
  int next = index_ + delta;
  if (slideshow_) next = (next % n + n) % n;
  if (next < 0 || next >= n || next == index_) return;
  index_ = next;
  load_current();
}

void Viewer::image_area(double &ax, double &ay, double &aw, double &ah) const {
  bool chrome = !slideshow_;
  double top = chrome ? S(56) : 0;
  double info = chrome && info_ && w() >= S(760) ? S(300) : 0;
  ax = x();
  ay = y() + top;
  aw = w() - info;
  ah = h() - top;
}

double Viewer::fit_scale() const {
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  double iw = img_w_, ih = img_h_;
  if (iw <= 0 || ih <= 0) {
    iw = ih = 512;
  }
  double margin = slideshow_ ? 0 : S(48);
  double f = std::min((aw - margin) / iw, (ah - margin) / ih);
  // Photos are never enlarged to fit; vector images scale freely.
  const Photo *p = photo();
  std::string ext = p ? extension_of(p->name) : "";
  double limit = ext == "svg" || ext == "svgz" ? 16.0 : 1.0;
  return std::max(0.01, std::min(limit, f));
}

double Viewer::current_scale() const { return zoom_ > 0 ? zoom_ : fit_scale(); }

void Viewer::set_zoom(double scale, double cx, double cy) {
  double fit = fit_scale();
  double old = current_scale();
  scale = std::max(0.02, std::min(16.0, scale));
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  if (scale <= fit * 1.001) {
    zoom_ = 0;
    pan_x_ = pan_y_ = 0;
  } else {
    double ccx = ax + aw / 2 + pan_x_, ccy = ay + ah / 2 + pan_y_;
    double r = scale / old;
    pan_x_ = cx - (cx - ccx) * r - (ax + aw / 2);
    pan_y_ = cy - (cy - ccy) * r - (ay + ah / 2);
    zoom_ = scale;
    double iw = img_w_ > 0 ? img_w_ : 512, ih = img_h_ > 0 ? img_h_ : 512;
    double mx = std::max(0.0, (iw * scale - aw) / 2), my = std::max(0.0, (ih * scale - ah) / 2);
    pan_x_ = std::max(-mx, std::min(mx, pan_x_));
    pan_y_ = std::max(-my, std::min(my, pan_y_));
  }
  redraw();
}

void Viewer::toggle_slideshow() {
  slideshow_ = !slideshow_;
  Fl::remove_timeout(slideshow_cb, this);
  if (slideshow_) {
    window()->fullscreen();
    Fl::add_timeout(4.0, slideshow_cb, this);
    app_->toast(_("Slideshow · press Esc to stop"));
  } else {
    window()->fullscreen_off();
    subset_.clear();
  }
  zoom_ = 0;
  pan_x_ = pan_y_ = 0;
  redraw();
}

void Viewer::start_slideshow(std::vector<int> subset) {
  subset_ = std::move(subset);
  if (!slideshow_) toggle_slideshow();
}

void Viewer::stop_slideshow() {
  if (slideshow_) toggle_slideshow();
}

void Viewer::slideshow_cb(void *p) {
  auto *v = static_cast<Viewer *>(p);
  if (!v->slideshow_) return;
  v->step(1);
  Fl::repeat_timeout(4.0, slideshow_cb, p);
}

void Viewer::layout_buttons() {
  const Photo *p = photo();
  std::string path = p ? p->path : "";
  top_.clear();
  actions_.clear();
  double bs = S(40), gap = S(4), by = y() + S(8);
  auto add = [&](icon::Kind glyph, const std::string &tip, std::function<void()> fn) {
    Button b;
    b.glyph = glyph;
    b.tip = tip;
    b.action = std::move(fn);
    b.w = b.h = bs;
    b.y = by;
    top_.push_back(std::move(b));
  };
  // Left: back.
  add(icon::Back, _("Back to Gallery (Esc)"), [this] { app_->close_viewer(); });
  top_.back().x = x() + S(10);
  size_t right_start = top_.size();
  bool fav = p && app_->is_favorite(p->path);
  add(fav ? icon::HeartFilled : icon::Heart, fav ? _("Remove from Favorites (Ctrl+D)") : _("Add to Favorites (Ctrl+D)"),
      [this, path] { app_->toggle_favorite({path}); });
  top_.back().toggled = fav;
  add(icon::ZoomOut, _("Zoom Out (−)"), [this] {
    set_zoom(current_scale() / 1.25, x() + w() / 2.0, y() + h() / 2.0);
  });
  add(icon::Fit, zoom_ > 0 ? _("Fit to Window (0)") : _("Actual Size (1)"), [this] {
    double ax, ay, aw, ah;
    image_area(ax, ay, aw, ah);
    if (zoom_ > 0) set_zoom(0, 0, 0);
    else set_zoom(1.0, ax + aw / 2, ay + ah / 2);
  });
  add(icon::ZoomIn, _("Zoom In (+)"), [this] {
    double ax, ay, aw, ah;
    image_area(ax, ay, aw, ah);
    set_zoom(current_scale() * 1.25, ax + aw / 2, ay + ah / 2);
  });
  add(icon::Play, _("Slideshow (F5)"), [this] { toggle_slideshow(); });
  add(icon::Edit, _("Edit in Photo (Ctrl+E)"), [this, path] { app_->edit_in_photo(path); });
  add(icon::Trash, _("Move to Trash (Delete)"), [this, path] { app_->trash({path}); });
  add(icon::Info, _("Details (I)"), [this] {
    info_ = !info_;
    zoom_ = 0;
    pan_x_ = pan_y_ = 0;
    redraw();
  });
  top_.back().toggled = info_ && w() >= S(760);
  double rx = x() + w() - S(10);
  for (size_t i = top_.size(); i-- > right_start;) {
    rx -= bs;
    top_[i].x = rx;
    rx -= gap;
    if (i == right_start + 1 || i == right_start + 4 || i == right_start + 6) rx -= S(10);  // groups
  }
  if (!p) for (auto &b : top_) b.active = b.glyph == icon::Back;
  // Info panel actions.
  if (!(info_ && w() >= S(760))) return;
  double px = x() + w() - S(300) + S(16), pw = S(300) - S(32);
  double ay = y() + h() - S(16) - 5 * S(40);
  auto action = [&](icon::Kind glyph, const std::string &text, std::function<void()> fn) {
    Button b;
    b.glyph = glyph;
    b.action = std::move(fn);
    b.text = text;
    b.x = px;
    b.y = ay;
    b.w = pw;
    b.h = S(36);
    ay += S(40);
    b.active = p != nullptr;
    actions_.push_back(std::move(b));
  };
  action(icon::Edit, _("Edit in Photo"), [this, path] { app_->edit_in_photo(path); });
  action(icon::OpenWith, _("Open With Other Application"), [this, path] { app_->open_with(path); });
  action(icon::Wallpaper, _("Set as Wallpaper"), [this, path] { app_->set_wallpaper(path); });
  action(icon::Files, _("Show in Files"), [this, path] { app_->show_in_files(path); });
  action(icon::Copy, _("Copy Path"), [this, path] { app_->copy_paths({path}); });
}

void Viewer::draw_info(Painter &p, double x0, double y0, double w0, double h0) {
  const Theme &t = app_->theme();
  p.fill_rect(x0, y0, w0, h0, t.surface);
  p.line(x0 + 0.5, y0, x0 + 0.5, y0 + h0, t.border);
}

void Viewer::draw() {
  const Theme &t = app_->theme();
  layout_buttons();
  const Photo *ph = photo();
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  bool chrome = !slideshow_;
  bool info = chrome && info_ && w() >= S(760);
  Color canvas = slideshow_ ? Color::hex(0x000000)
                 : t.dark   ? t.background.mix(Color::hex(0x000000), 0.35)
                            : t.surface_alt.mix(t.background, 0.4);
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    p.fill_rect(x(), y(), w(), h(), canvas);
    // Image.
    cairo_surface_t *s = full_;
    if (!s && ph) s = app_->thumbnail(*ph, true);
    cairo_save(cr);
    cairo_rectangle(cr, ax, ay, aw, ah);
    cairo_clip(cr);
    if (s) {
      double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
      double iw = img_w_ > 0 ? img_w_ : sw, ih = img_h_ > 0 ? img_h_ : sh;
      if (img_w_ <= 0 && !full_) {
        iw = sw;
        ih = sh;
      }
      double scale = current_scale();
      if (img_w_ <= 0) {
        double fiw = iw, fih = ih;
        double margin = slideshow_ ? 0 : S(48);
        scale = zoom_ > 0 ? zoom_ : std::min(1.0, std::min((aw - margin) / fiw, (ah - margin) / fih));
      }
      double cx = ax + aw / 2 + pan_x_, cy = ay + ah / 2 + pan_y_;
      double ew = iw * scale, eh = ih * scale;
      if (!slideshow_) p.shadow(cx - ew / 2, cy - eh / 2, ew, eh, 2, t.shadow.alpha(t.shadow.a * 0.8), S(10));
      cairo_save(cr);
      cairo_translate(cr, cx, cy);
      double k = scale * iw / sw;
      cairo_scale(cr, k, k);
      cairo_translate(cr, -sw / 2, -sh / 2);
      // Checkerboard-free: transparent images sit on a neutral card.
      cairo_rectangle(cr, 0, 0, sw, sh);
      p.set(t.dark ? Color::hex(0x2A313B) : Color::hex(0xFFFFFF));
      cairo_fill(cr);
      cairo_set_source_surface(cr, s, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), k >= 3 ? CAIRO_FILTER_FAST : CAIRO_FILTER_GOOD);
      cairo_paint(cr);
      cairo_restore(cr);
    } else if (ph) {
      double gs = S(64);
      icon::draw(p, failed_ ? icon::Broken : icon::Picture, ax + (aw - gs) / 2, ay + (ah - gs) / 2 - S(16), gs,
                 t.text_dim.alpha(0.7));
    }
    // Previous / next.
    if (ph && !slideshow_) {
      int n = int(app_->photos().size());
      for (int side : {-1, 1}) {
        bool enabled = side < 0 ? index_ > 0 : index_ < n - 1;
        if (!enabled) continue;
        double r = S(22), bx = side < 0 ? ax + S(18) + r : ax + aw - S(18) - r, byc = ay + ah / 2;
        bool hot = hover_side_ == side;
        p.circle(bx, byc, r, (t.dark ? t.surface : Color::hex(0xFFFFFF)).alpha(hot ? 0.95 : 0.7));
        icon::draw(p, side < 0 ? icon::Prev : icon::Next, bx - S(11), byc - S(11), S(22), t.text);
      }
    }
    cairo_restore(cr);

    if (chrome) {
      // Top bar.
      p.fill_rect(x(), y(), w(), S(56), t.surface);
      if (t.glass) p.glass_sheen(x(), y(), w(), S(56), 0, t.dark);
      p.line(x(), y() + S(56) - 0.5, x() + w(), y() + S(56) - 0.5, t.border);
      for (size_t i = 0; i < top_.size(); ++i) {
        const Button &b = top_[i];
        bool hot = int(i) == hover_button_;
        if (b.toggled) p.fill_rounded(b.x, b.y, b.w, b.h, Sd(10), t.accent.alpha(t.dark ? 0.3 : 0.16));
        else if (hot && b.active) p.fill_rounded(b.x, b.y, b.w, b.h, Sd(10), t.hover);
        Color c = !b.active ? t.text_dim.alpha(0.5) : b.toggled ? t.accent_text : t.text;
        if (b.glyph == icon::HeartFilled) c = Color::hex(0xE0475B);
        if (b.glyph == icon::Play && slideshow_) c = t.accent_text;
        icon::draw(p, b.glyph, b.x + S(9), b.y + S(9), S(22), c);
      }
      if (info) {
        double px = x() + w() - S(300);
        draw_info(p, px, ay, S(300), ah);
        for (size_t i = 0; i < actions_.size(); ++i) {
          const Button &b = actions_[i];
          if (int(i) + 1000 == hover_button_ && b.active) p.fill_rounded(b.x, b.y, b.w, b.h, Sd(9), t.hover);
          icon::draw(p, b.glyph, b.x + S(10), b.y + (b.h - S(20)) / 2, S(20), b.active ? t.text : t.text_dim);
        }
        p.line(px + S(16), actions_.empty() ? 0 : actions_[0].y - S(12), px + S(300) - S(16),
               actions_.empty() ? 0 : actions_[0].y - S(12), t.border);
      }
      // Tooltip.
      if (hover_button_ >= 0 && hover_button_ < int(top_.size())) {
        const Button &b = top_[size_t(hover_button_)];
        text_font(t.font_size - S(1));
        double tw = text_width(b.tip) + S(20), th = S(28);
        double tx = std::max(double(x() + S(6)), std::min(b.x + b.w / 2 - tw / 2, x() + w() - tw - S(6)));
        double ty = b.y + b.h + S(8);
        p.shadow(tx, ty, tw, th, Sd(8), t.shadow, S(6));
        p.fill_rounded(tx, ty, tw, th, Sd(8), t.dark ? t.surface_alt : Color::hex(0x1F2630));
      }
    }
  }
  if (!chrome) return;
  // Text layer.
  int name_x = int(top_.empty() ? x() + S(16) : top_[0].x + top_[0].w + S(12));
  int name_w = (top_.size() > 1 ? int(top_[1].x) : x() + w()) - S(16) - name_x;
  if (ph) {
    text_font(t.font_size + S(1), true);
    draw_text(ph->name, name_x, y() + S(8), name_w, S(22), t.text);
    text_font(t.font_size - S(1));
    std::string sub = format(_("%d of %zu"), index_ + 1, app_->photos().size());
    double sc = current_scale();
    sub += "  ·  " + (zoom_ > 0 ? format("%d%%", int(std::lround(sc * 100)))
                                : format(_("Fit (%d%%)"), int(std::lround(sc * 100))));
    draw_text(sub, name_x, y() + S(29), name_w, S(18), t.text_dim);
  }
  if (ph && failed_ && !full_) {
    text_font(t.font_size);
    draw_text(_("This image cannot be displayed"), int(ax), int(ay + ah / 2 + S(30)), int(aw), S(24), t.text_dim, 1);
  }
  if (info && ph) {
    int px = x() + w() - S(300) + S(20), pw = S(300) - S(40);
    int yy = int(ay) + S(18);
    text_font(t.font_size - S(1), true);
    draw_text(_("DETAILS"), px, yy, pw, S(18), t.text_dim);
    yy += S(26);
    text_font(t.font_size + S(1), true);
    for (const auto &line : wrap(ph->name, pw, 3)) {
      draw_text(line, px, yy, pw, S(22), t.text);
      yy += S(22);
    }
    yy += S(10);
    auto field = [&](const std::string &label, const std::string &value, size_t lines = 1) {
      text_font(t.font_size - S(1));
      draw_text(label, px, yy, pw, S(18), t.text_dim);
      yy += S(19);
      text_font(t.font_size);
      for (const auto &line : wrap(value, pw, lines)) {
        draw_text(line, px, yy, pw, S(20), t.text);
        yy += S(20);
      }
      yy += S(10);
    };
    char date[128];
    struct tm tm {};
    localtime_r(&ph->mtime, &tm);
    if (!strftime(date, sizeof date, "%A, %e %B %Y, %H:%M", &tm)) date[0] = 0;
    field(_("Date"), trim(date));
    if (img_w_ > 0 && img_h_ > 0) {
      double mp = img_w_ * double(img_h_) / 1e6;
      field(_("Dimensions"), format("%d × %d", img_w_, img_h_) +
                                 (mp >= 0.1 ? format(_("  ·  %.1f MP"), mp) : std::string()));
    } else {
      field(_("Dimensions"), failed_ ? _("Unknown") : _("Loading…"));
    }
    field(_("Size"), human_size(ph->size) + "  ·  " + type_name(ph->path));
    field(_("Folder"), ph->dir, 3);
    for (size_t i = 0; i < actions_.size(); ++i) {
      const Button &b = actions_[i];
      text_font(t.font_size);
      draw_text(b.text, int(b.x + S(42)), int(b.y), int(b.w - S(50)), int(b.h), b.active ? t.text : t.text_dim);
    }
  }
  if (hover_button_ >= 0 && hover_button_ < int(top_.size())) {
    const Button &b = top_[size_t(hover_button_)];
    text_font(t.font_size - S(1));
    int tw = text_width(b.tip) + S(20), th = S(28);
    int tx = int(std::max(double(x() + S(6)), std::min(b.x + b.w / 2 - tw / 2, double(x() + w() - tw - S(6)))));
    int ty = int(b.y + b.h + S(8));
    draw_text(b.tip, tx, ty, tw, th, t.dark ? t.text : Color::hex(0xF5F7FA), 1);
  }
}

bool Viewer::keyboard(int key, int state) {
  bool ctrl = state & FL_CTRL;
  const Photo *p = photo();
  double ax, ay, aw, ah;
  image_area(ax, ay, aw, ah);
  switch (key) {
    case FL_Escape:
      if (slideshow_) stop_slideshow();
      else app_->close_viewer();
      return true;
    case FL_Left:
    case FL_Page_Up:
    case FL_BackSpace:
      step(-1);
      return true;
    case FL_Right:
    case FL_Page_Down:
    case ' ':
      step(1);
      return true;
    case FL_Home:
      step(-index_);
      return true;
    case FL_End:
      step(int(app_->photos().size()) - 1 - index_);
      return true;
    case FL_F + 5:
      toggle_slideshow();
      return true;
    case FL_F + 11:
      if (window()->fullscreen_active()) window()->fullscreen_off();
      else window()->fullscreen();
      return true;
    case FL_Delete:
      if (p) app_->trash({p->path});
      return true;
    case FL_Enter:
    case FL_KP_Enter:
      return true;
  }
  if (key == '+' || key == '=' || key == FL_KP + '+') {
    set_zoom(current_scale() * 1.25, ax + aw / 2, ay + ah / 2);
    return true;
  }
  if (key == '-' || key == FL_KP + '-') {
    set_zoom(current_scale() / 1.25, ax + aw / 2, ay + ah / 2);
    return true;
  }
  if (key == '0') {
    set_zoom(0, 0, 0);
    return true;
  }
  if (key == '1') {
    set_zoom(1.0, ax + aw / 2, ay + ah / 2);
    return true;
  }
  if (key == 'i' && !ctrl) {
    info_ = !info_;
    zoom_ = 0;
    pan_x_ = pan_y_ = 0;
    redraw();
    return true;
  }
  if (ctrl && key == 'c' && p) {
    app_->copy_paths({p->path});
    return true;
  }
  if (ctrl && key == 'd' && p) {
    app_->toggle_favorite({p->path});
    return true;
  }
  if (ctrl && key == 'e' && p) {
    app_->edit_in_photo(p->path);
    return true;
  }
  return false;
}

int Viewer::handle(int event) {
  switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS:
      return 1;
    case FL_ENTER:
    case FL_MOVE: {
      layout_buttons();
      int hb = -1;
      int mx = Fl::event_x(), my = Fl::event_y();
      if (!slideshow_) {
        for (size_t i = 0; i < top_.size(); ++i)
          if (mx >= top_[i].x && mx < top_[i].x + top_[i].w && my >= top_[i].y && my < top_[i].y + top_[i].h)
            hb = int(i);
        for (size_t i = 0; i < actions_.size(); ++i)
          if (mx >= actions_[i].x && mx < actions_[i].x + actions_[i].w && my >= actions_[i].y &&
              my < actions_[i].y + actions_[i].h)
            hb = 1000 + int(i);
      }
      double ax, ay, aw, ah;
      image_area(ax, ay, aw, ah);
      int side = 0;
      if (hb < 0 && my >= ay && my < ay + ah && mx >= ax && mx < ax + aw) {
        if (mx < ax + S(84)) side = -1;
        else if (mx > ax + aw - S(84)) side = 1;
      }
      if (hb != hover_button_ || side != hover_side_) {
        hover_button_ = hb;
        hover_side_ = side;
        redraw();
      }
      if (slideshow_) window()->cursor(FL_CURSOR_NONE);
      else window()->cursor(zoom_ > 0 ? FL_CURSOR_MOVE : FL_CURSOR_DEFAULT);
      return 1;
    }
    case FL_LEAVE:
      hover_button_ = -1;
      hover_side_ = 0;
      window()->cursor(FL_CURSOR_DEFAULT);
      redraw();
      return 1;
    case FL_MOUSEWHEEL: {
      double ax, ay, aw, ah;
      image_area(ax, ay, aw, ah);
      int dy = Fl::event_dy();
      if (!dy) return 1;
      if (slideshow_) {
        step(dy > 0 ? 1 : -1);
        return 1;
      }
      double f = dy < 0 ? 1.18 : 1 / 1.18;
      set_zoom(current_scale() * f, Fl::event_x(), Fl::event_y());
      return 1;
    }
    case FL_PUSH: {
      if (Fl::focus() != this) Fl::focus(this);
      int mx = Fl::event_x(), my = Fl::event_y();
      if (slideshow_) {
        step(1);
        return 1;
      }
      layout_buttons();
      for (auto &b : top_)
        if (b.active && mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) {
          auto fn = b.action;  // the button list is rebuilt by the action
          fn();
          redraw();
          return 1;
        }
      for (auto &b : actions_)
        if (b.active && mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) {
          auto fn = b.action;
          fn();
          redraw();
          return 1;
        }
      if (hover_side_ != 0 && zoom_ == 0) {
        step(hover_side_);
        return 1;
      }
      double ax, ay, aw, ah;
      image_area(ax, ay, aw, ah);
      if (my < ay || mx >= ax + aw) return 1;
      if (Fl::event_clicks()) {
        Fl::event_clicks(0);
        if (zoom_ > 0) set_zoom(0, 0, 0);
        else set_zoom(1.0, mx, my);
        return 1;
      }
      panning_ = zoom_ > 0;
      press_x_ = mx;
      press_y_ = my;
      press_pan_x_ = pan_x_;
      press_pan_y_ = pan_y_;
      return 1;
    }
    case FL_DRAG:
      if (panning_) {
        pan_x_ = press_pan_x_ + (Fl::event_x() - press_x_);
        pan_y_ = press_pan_y_ + (Fl::event_y() - press_y_);
        set_zoom(zoom_, Fl::event_x(), Fl::event_y());  // clamps
      }
      return 1;
    case FL_RELEASE:
      panning_ = false;
      return 1;
    case FL_KEYBOARD:
      return keyboard(Fl::event_key(), Fl::event_state()) ? 1 : 0;
  }
  return Fl_Widget::handle(event);
}

}  // namespace gallery
