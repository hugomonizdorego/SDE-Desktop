// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The thumbnail grid: month sections, smooth scrolling, selection and
// keyboard navigation.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gallery.h"
#include "i18n.h"
#include "util.h"

using namespace sde;

namespace gallery {

Grid::Grid(GalleryWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {
  layout_items();
}

Grid::~Grid() { Fl::remove_timeout(animate_cb, this); }

void Grid::resize(int x, int y, int w, int h) {
  bool changed = w != this->w() || h != this->h();
  Fl_Widget::resize(x, y, w, h);
  if (changed) relayout();
}

void Grid::layout_items() {
  const auto &photos = app_->photos();
  const auto &groups = app_->groups();
  pad_ = S(24);
  gap_ = S(6);
  header_h_ = S(50);
  double avail = std::max(double(S(60)), w() - 2 * pad_);
  double target = S(app_->zoom());
  cols_ = std::max(1, int((avail + gap_) / (target + gap_)));
  tile_ = (avail - (cols_ - 1) * gap_) / cols_;
  rows_.clear();
  headers_.clear();
  item_row_.assign(photos.size(), 0);
  double y = S(6);
  for (const auto &g : groups) {
    if (!g.label.empty()) {
      headers_.push_back(Header{g.label, g.count, y, rows_.size()});
      y += header_h_;
    } else {
      y += S(12);
    }
    for (size_t i = 0; i < g.count; i += size_t(cols_)) {
      Row r{g.first + i, std::min(size_t(cols_), g.count - i), y};
      for (size_t k = 0; k < r.count; ++k) item_row_[r.first + k] = rows_.size();
      rows_.push_back(r);
      y += tile_ + gap_;
    }
    y += S(14);
  }
  content_h_ = y + S(20);
  if (selected_.size() != photos.size()) {
    selected_.assign(photos.size(), 0);
    if (focus_ >= int(photos.size())) focus_ = -1;
    anchor_ = focus_;
  }
}

double Grid::max_scroll() const { return std::max(0.0, content_h_ - h()); }

void Grid::reset() {
  selected_.clear();
  focus_ = anchor_ = hover_ = -1;
  scroll_ = target_ = 0;
  layout_items();
  redraw();
}

void Grid::relayout() {
  // Keep the first visible photo in place when the tile size changes.
  int anchor = -1;
  double offset = 0;
  for (const auto &r : rows_)
    if (r.y + tile_ > scroll_) {
      anchor = int(r.first);
      offset = r.y - scroll_;
      break;
    }
  layout_items();
  if (anchor >= 0 && anchor < int(item_row_.size())) {
    scroll_ = target_ = rows_[item_row_[size_t(anchor)]].y - offset;
  }
  scroll_ = target_ = std::max(0.0, std::min(max_scroll(), target_));
  redraw();
}

bool Grid::tile_rect(size_t index, double &tx, double &ty, double &ts) const {
  if (index >= item_row_.size() || rows_.empty()) return false;
  const Row &r = rows_[item_row_[index]];
  tx = x() + pad_ + double(index - r.first) * (tile_ + gap_);
  ty = y() + r.y - scroll_;
  ts = tile_;
  return true;
}

int Grid::item_at(int mx, int my) const {
  double cy = my - y() + scroll_;
  auto it = std::upper_bound(rows_.begin(), rows_.end(), cy,
                             [](double v, const Row &r) { return v < r.y; });
  if (it == rows_.begin()) return -1;
  --it;
  if (cy > it->y + tile_) return -1;
  double cx = mx - x() - pad_;
  if (cx < 0) return -1;
  int col = int(cx / (tile_ + gap_));
  if (cx - col * (tile_ + gap_) > tile_ || col >= int(it->count)) return -1;
  return int(it->first) + col;
}

size_t Grid::row_of(size_t index) const { return index < item_row_.size() ? item_row_[index] : 0; }

std::vector<size_t> Grid::selection() const {
  std::vector<size_t> out;
  for (size_t i = 0; i < selected_.size(); ++i)
    if (selected_[i]) out.push_back(i);
  return out;
}

void Grid::clear_selection() {
  std::fill(selected_.begin(), selected_.end(), 0);
  app_->layout();
  window()->redraw();
}

void Grid::handle_select_all() {
  std::fill(selected_.begin(), selected_.end(), 1);
  app_->layout();
  window()->redraw();
}

void Grid::select_index(int i) {
  if (i < 0 || i >= int(selected_.size())) return;
  selected_[size_t(i)] = 1;
  app_->layout();
  redraw();
}

void Grid::set_focus_index(int i) {
  if (i < 0 || i >= int(item_row_.size())) return;
  focus_ = anchor_ = i;
  redraw();
}

void Grid::select_only(int index) {
  std::fill(selected_.begin(), selected_.end(), 0);
  if (index >= 0 && index < int(selected_.size())) selected_[size_t(index)] = 1;
}

void Grid::move_focus(int index, bool extend, bool toggle) {
  int n = int(item_row_.size());
  if (n == 0) return;
  index = std::max(0, std::min(n - 1, index));
  if (extend) {
    if (anchor_ < 0) anchor_ = focus_ >= 0 ? focus_ : index;
    std::fill(selected_.begin(), selected_.end(), 0);
    for (int i = std::min(anchor_, index); i <= std::max(anchor_, index); ++i) selected_[size_t(i)] = 1;
  } else {
    anchor_ = index;
    if (toggle) selected_[size_t(index)] = !selected_[size_t(index)];
  }
  focus_ = index;
  ensure_visible(index);
  app_->layout();
  window()->redraw();
}

void Grid::ensure_visible(int index) {
  if (index < 0 || index >= int(item_row_.size())) return;
  const Row &r = rows_[item_row_[size_t(index)]];
  double top = r.y - S(10), bottom = r.y + tile_ + S(16);
  // Leave room for the pinned month header.
  if (!headers_.empty()) top -= header_h_;
  if (top < target_) target_ = top;
  else if (bottom > target_ + h()) target_ = bottom - h();
  target_ = std::max(0.0, std::min(max_scroll(), target_));
  animate();
}

void Grid::animate() {
  if (!animating_) {
    animating_ = true;
    Fl::add_timeout(1.0 / 60, animate_cb, this);
  }
}

void Grid::animate_cb(void *p) {
  auto *g = static_cast<Grid *>(p);
  double d = g->target_ - g->scroll_;
  if (std::fabs(d) < 0.6) {
    g->scroll_ = g->target_;
    g->animating_ = false;
  } else {
    g->scroll_ += d * 0.28;
    Fl::repeat_timeout(1.0 / 60, animate_cb, p);
  }
  g->redraw();
}

void Grid::request_visible() {
  const auto &photos = app_->photos();
  if (photos.empty() || rows_.empty()) {
    app_->request_thumbnails({});
    return;
  }
  bool large = tile_ > 128;
  std::vector<std::pair<const Photo *, bool>> visible, after, before;
  for (const auto &r : rows_) {
    double top = r.y - scroll_;
    if (top + tile_ < -h() * 0.6 || top > h() * 2.0) continue;
    auto &bucket = top + tile_ < 0 ? before : top > h() ? after : visible;
    for (size_t k = 0; k < r.count; ++k) bucket.push_back({&photos[r.first + k], large});
  }
  std::reverse(before.begin(), before.end());
  visible.insert(visible.end(), after.begin(), after.end());
  visible.insert(visible.end(), before.begin(), before.end());
  app_->request_thumbnails(visible);
}

void Grid::draw() {
  const Theme &t = app_->theme();
  const auto &photos = app_->photos();
  struct Label {
    std::string text;
    int x, y, w, h;
    Color color;
    bool bold;
    int size;
    int align;
  };
  std::vector<Label> labels;
  bool any_selected = std::any_of(selected_.begin(), selected_.end(), [](char c) { return c != 0; });
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), t.background);

    if (photos.empty()) {
      double cx = x() + w() / 2.0, cy = y() + h() * 0.42;
      p.circle(cx, cy, S(46), t.surface_alt);
      icon::Kind glyph = app_->view().kind == ViewKind::Favorites ? icon::Heart
                         : app_->searching()                       ? icon::Search
                                                                   : icon::Picture;
      icon::draw(p, glyph, cx - S(24), cy - S(24), S(48), t.text_dim);
      std::string title, hint;
      if (app_->scanning() && app_->library().empty()) {
        title = _("Looking for photos…");
      } else if (app_->searching()) {
        title = _("No results");
        hint = _("Try a different name, or clear the search.");
      } else if (app_->view().kind == ViewKind::Favorites) {
        title = _("No favorites yet");
        hint = _("Open a photo and choose the heart to keep it here.");
      } else if (app_->view().kind == ViewKind::Screenshots) {
        title = _("No screenshots");
        hint = format(_("Screenshots saved in %s appear here."), app_->screenshots_dir().c_str());
      } else if (app_->view().kind == ViewKind::Recent) {
        title = _("Nothing recent");
        hint = _("Photos from the last 30 days appear here.");
      } else {
        title = _("No photos here yet");
        hint = format(_("Save pictures in %s or add a folder."), app_->pictures().c_str());
      }
      labels.push_back({title, x() + S(20), int(cy + S(60)), w() - S(40), S(30), t.text, true,
                        t.font_size + S(4), 1});
      if (!hint.empty())
        labels.push_back({hint, x() + S(20), int(cy + S(92)), w() - S(40), S(24), t.text_dim, false,
                          t.font_size, 1});
    }

    double radius = std::min(Sd(10), tile_ * 0.07);
    int sticky = -1;
    for (size_t hi = 0; hi < headers_.size(); ++hi)
      if (headers_[hi].y <= scroll_ + 1) sticky = int(hi);
    for (const auto &r : rows_) {
      double ty = y() + r.y - scroll_;
      if (ty + tile_ < y() || ty > y() + h()) continue;
      for (size_t k = 0; k < r.count; ++k) {
        size_t i = r.first + k;
        const Photo &ph = photos[i];
        double tx = x() + pad_ + double(k) * (tile_ + gap_);
        bool sel = i < selected_.size() && selected_[i];
        bool hot = int(i) == hover_;
        double inset = sel ? std::max(Sd(6), tile_ * 0.06) : 0;
        double ix = tx + inset, iy = ty + inset, is = tile_ - 2 * inset;
        bool failed = false;
        cairo_surface_t *s = app_->thumbnail(ph, tile_ > 128, &failed);
        cairo_save(cr);
        p.rounded_rect(ix, iy, is, is, radius);
        cairo_clip(cr);
        if (s) {
          double sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
          double sc = std::max(is / sw, is / sh);
          p.fill_rect(ix, iy, is, is, t.surface_alt);
          cairo_translate(cr, ix + (is - sw * sc) / 2, iy + (is - sh * sc) / 2);
          cairo_scale(cr, sc, sc);
          cairo_set_source_surface(cr, s, 0, 0);
          cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
          cairo_paint(cr);
          cairo_identity_matrix(cr);
        } else {
          p.fill_rect(ix, iy, is, is, t.surface_alt);
          double gs = std::min(is * 0.32, double(S(40)));
          icon::draw(p, failed ? icon::Broken : icon::Picture, ix + (is - gs) / 2, iy + (is - gs) / 2, gs,
                     t.text_dim.alpha(failed ? 0.8 : 0.45));
        }
        if (hot || sel) {
          cairo_pattern_t *g = cairo_pattern_create_linear(0, iy + is * 0.55, 0, iy + is);
          cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, 0);
          cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0.55);
          cairo_rectangle(cr, ix, iy, is, is);
          cairo_set_source(cr, g);
          cairo_fill(cr);
          cairo_pattern_destroy(g);
        }
        cairo_restore(cr);
        if (sel) p.stroke_rounded(tx + Sd(1), ty + Sd(1), tile_ - Sd(2), tile_ - Sd(2), radius + Sd(2),
                                  t.accent, Sd(2.5));
        // Selection check mark.
        double cs = S(24);
        double ccx = ix + S(8) + cs / 2, ccy = iy + S(8) + cs / 2;
        if (sel) {
          p.circle(ccx, ccy, cs / 2 + Sd(1.5), Color::hex(0xFFFFFF));
          p.circle(ccx, ccy, cs / 2, t.accent);
          icon::draw(p, icon::Check, ccx - cs / 2 + Sd(2), ccy - cs / 2 + Sd(2), cs - Sd(4), t.on_accent);
        } else if (hot || any_selected) {
          p.circle(ccx, ccy, cs / 2, Color::hex(0x000000, 0.25));
          cairo_new_sub_path(cr);
          cairo_arc(cr, ccx, ccy, cs / 2 - Sd(1), 0, 2 * M_PI);
          p.set(Color::hex(0xFFFFFF, 0.9));
          cairo_set_line_width(cr, Sd(1.6));
          cairo_stroke(cr);
        }
        if (app_->is_favorite(ph.path)) {
          double hs = std::max(double(S(16)), std::min(double(S(22)), is * 0.14));
          double hx = ix + is - hs - S(8), hy = iy + is - hs - S(8);
          icon::draw(p, icon::HeartFilled, hx + Sd(1), hy + Sd(1), hs, Color::hex(0x000000, 0.35));
          icon::draw(p, icon::HeartFilled, hx, hy, hs, Color::hex(0xFFFFFF));
        }
        if (int(i) == focus_ && Fl::focus() == this && (keyboard_used_ || !sel))
          focus_ring(p, tx, ty, tile_, tile_, radius);
        if ((hot || sel) && is > S(90) && ty > y() + (sticky >= 0 ? header_h_ : 0))
          labels.push_back({ph.name, int(ix + S(10)), int(iy + is - S(30)),
                            int(is - S(20) - (app_->is_favorite(ph.path) ? S(26) : 0)), S(24),
                            Color::hex(0xFFFFFF), false, t.font_size - S(1), 0});
      }
    }
    // Month headers; the current one stays pinned at the top.
    for (size_t hi = 0; hi < headers_.size(); ++hi) {
      const Header &hd = headers_[hi];
      double hy = y() + hd.y - scroll_;
      bool pinned = int(hi) == sticky;
      if (pinned) {
        double next = hi + 1 < headers_.size() ? y() + headers_[hi + 1].y - scroll_ : 1e9;
        hy = std::min(double(y()), next - header_h_);
        p.fill_rect(x(), hy, w(), header_h_, t.background.alpha(0.94));
        if (scroll_ > 1) p.line(x(), hy + header_h_ - 0.5, x() + w(), hy + header_h_ - 0.5, t.border.alpha(0.6));
      }
      if (hy + header_h_ < y() || hy > y() + h()) continue;
      text_font(t.font_size + S(3), true);
      int lw = text_width(hd.label);
      labels.push_back({hd.label, int(x() + pad_), int(hy + S(8)), lw + S(4), int(header_h_ - S(12)), t.text,
                        true, t.font_size + S(3), 0});
      std::string count = format(hd.count == 1 ? _("%zu photo") : _("%zu photos"), hd.count);
      labels.push_back({count, int(x() + pad_ + lw + S(12)), int(hy + S(9)), S(160), int(header_h_ - S(12)),
                        t.text_dim, false, t.font_size - S(1), 0});
    }
    // Overlay scrollbar.
    if (content_h_ > h()) {
      double len = std::max(double(S(40)), h() * h() / content_h_);
      double pos = y() + (h() - len) * (scroll_ / std::max(1.0, max_scroll()));
      double bw = drag_bar_ || bar_hover_ ? S(8) : S(5);
      p.fill_rounded(x() + w() - bw - S(4), pos + S(2), bw, len - S(4), bw / 2,
                     t.text_dim.alpha(drag_bar_ ? 0.7 : bar_hover_ ? 0.5 : 0.32));
    }
    cairo_restore(cr);
  }
  fl_push_clip(x(), y(), w(), h());
  for (const auto &l : labels) {
    text_font(l.size, l.bold);
    if (l.color.r == 1 && l.color.g == 1 && l.color.b == 1) {
      // White captions over photos get a subtle shadow.
      draw_text(l.text, l.x + 1, l.y + 1, l.w, l.h, Color::hex(0x000000, 0.6), l.align);
    }
    draw_text(l.text, l.x, l.y, l.w, l.h, l.color, l.align);
  }
  fl_pop_clip();
  request_visible();
}

void Grid::open_menu(int index) {
  const auto &photos = app_->photos();
  if (index < 0 || index >= int(photos.size())) return;
  std::vector<std::string> paths;
  if (selected_[size_t(index)]) {
    for (size_t i : selection()) paths.push_back(photos[i].path);
  } else {
    paths.push_back(photos[size_t(index)].path);
  }
  bool all_fav = std::all_of(paths.begin(), paths.end(), [&](const std::string &p) { return app_->is_favorite(p); });
  bool single = paths.size() == 1;
  std::string fav = all_fav ? _("Remove from Favorites") : _("Add to Favorites");
  std::string copy = single ? _("Copy Path") : _("Copy Paths");
  std::string trash = _("Move to Trash…");
  int one = single ? 0 : FL_MENU_INACTIVE;
  struct Entry {
    const char *text;
    long id;
    int flags;
  } entries[] = {{_("Open"), 1, one},
                 {fav.c_str(), 2, 0},
                 {_("Open in Photo"), 3, one},
                 {_("Open With Other Application"), 4, one},
                 {_("Set as Wallpaper"), 5, one},
                 {_("Show in Files"), 6, one | FL_MENU_DIVIDER},
                 {copy.c_str(), 7, 0},
                 {trash.c_str(), 8, 0}};
  Fl_Menu_Item items[9];
  memset(items, 0, sizeof items);
  for (size_t i = 0; i < 8; ++i) {
    items[i].text = entries[i].text;
    items[i].user_data_ = reinterpret_cast<void *>(entries[i].id);
    items[i].flags = entries[i].flags;
    items[i].labelsize(app_->theme().font_size);
    items[i].labelcolor(app_->theme().text.fl());
  }
  const Fl_Menu_Item *picked = items->popup(Fl::event_x(), Fl::event_y());
  if (!picked) return;
  switch (reinterpret_cast<long>(picked->user_data())) {
    case 1: app_->open_photo(index); break;
    case 2: app_->toggle_favorite(paths); break;
    case 3: app_->edit_in_photo(paths[0]); break;
    case 4: app_->open_with(paths[0]); break;
    case 5: app_->set_wallpaper(paths[0]); break;
    case 6: app_->show_in_files(paths[0]); break;
    case 7: app_->copy_paths(paths); break;
    case 8: app_->trash(paths); break;
  }
}

bool Grid::keyboard(int key, int state) {
  const auto &photos = app_->photos();
  int n = int(photos.size());
  bool shift = state & FL_SHIFT, ctrl = state & FL_CTRL;
  if (key == '+' || key == '=' || key == FL_KP + '+') {
    app_->set_zoom(app_->zoom() + 24);
    return true;
  }
  if (key == '-' || key == FL_KP + '-') {
    app_->set_zoom(app_->zoom() - 24);
    return true;
  }
  if (n == 0) return false;
  keyboard_used_ = true;
  int f = focus_ < 0 ? 0 : focus_;
  auto vertical = [&](int rows) {
    size_t r = row_of(size_t(f));
    int col = f - int(rows_[r].first);
    long target = long(r) + rows;
    target = std::max(0L, std::min(long(rows_.size()) - 1, target));
    const Row &row = rows_[size_t(target)];
    return int(row.first) + std::min(col, int(row.count) - 1);
  };
  int visible_rows = std::max(1, int(h() / (tile_ + gap_)) - 1);
  switch (key) {
    case FL_Left: move_focus(focus_ < 0 ? 0 : f - 1, shift, false); return true;
    case FL_Right: move_focus(focus_ < 0 ? 0 : f + 1, shift, false); return true;
    case FL_Up: move_focus(focus_ < 0 ? 0 : vertical(-1), shift, false); return true;
    case FL_Down: move_focus(focus_ < 0 ? 0 : vertical(1), shift, false); return true;
    case FL_Page_Up: move_focus(vertical(-visible_rows), shift, false); return true;
    case FL_Page_Down: move_focus(vertical(visible_rows), shift, false); return true;
    case FL_Home: move_focus(0, shift, false); return true;
    case FL_End: move_focus(n - 1, shift, false); return true;
    case ' ':
      if (focus_ < 0) focus_ = 0;
      move_focus(focus_, false, true);
      return true;
    case FL_Enter:
    case FL_KP_Enter:
      if (focus_ >= 0) app_->open_photo(focus_);
      return true;
    case FL_Escape:
      if (std::any_of(selected_.begin(), selected_.end(), [](char c) { return c != 0; })) {
        clear_selection();
        return true;
      }
      return false;
    case FL_Delete: {
      std::vector<std::string> paths;
      for (size_t i : selection()) paths.push_back(photos[i].path);
      if (paths.empty() && focus_ >= 0) paths.push_back(photos[size_t(focus_)].path);
      app_->trash(paths);
      return true;
    }
    case FL_Menu:
      if (focus_ >= 0) open_menu(focus_);
      return true;
  }
  if (ctrl && key == 'a') {
    handle_select_all();
    return true;
  }
  if (ctrl && key == 'c') {
    std::vector<std::string> paths;
    for (size_t i : selection()) paths.push_back(photos[i].path);
    if (paths.empty() && focus_ >= 0) paths.push_back(photos[size_t(focus_)].path);
    app_->copy_paths(paths);
    return true;
  }
  if (ctrl && key == 'd') {
    std::vector<std::string> paths;
    for (size_t i : selection()) paths.push_back(photos[i].path);
    if (paths.empty() && focus_ >= 0) paths.push_back(photos[size_t(focus_)].path);
    app_->toggle_favorite(paths);
    return true;
  }
  return false;
}

int Grid::handle(int event) {
  switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_ENTER:
    case FL_MOVE: {
      int hv = item_at(Fl::event_x(), Fl::event_y());
      int bar = content_h_ > h() && Fl::event_x() >= x() + w() - S(16);
      if (hv != hover_ || bar != bar_hover_) {
        hover_ = hv;
        bar_hover_ = bar;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      bar_hover_ = 0;
      redraw();
      return 1;
    case FL_MOUSEWHEEL: {
      if (Fl::event_state() & FL_CTRL) {
        app_->set_zoom(app_->zoom() - Fl::event_dy() * 16);
        return 1;
      }
      target_ = std::max(0.0, std::min(max_scroll(), target_ + Fl::event_dy() * S(110)));
      animate();
      return 1;
    }
    case FL_PUSH: {
      if (Fl::focus() != this) Fl::focus(this);
      keyboard_used_ = false;
      if (content_h_ > h() && Fl::event_x() >= x() + w() - S(16)) {
        drag_bar_ = true;
        double len = std::max(double(S(40)), h() * h() / content_h_);
        double pos = y() + (h() - len) * (scroll_ / std::max(1.0, max_scroll()));
        drag_offset_ = Fl::event_y() - pos;
        if (drag_offset_ < 0 || drag_offset_ > len) drag_offset_ = len / 2;
        handle(FL_DRAG);
        return 1;
      }
      int i = item_at(Fl::event_x(), Fl::event_y());
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (i >= 0) {
          if (!selected_[size_t(i)]) {
            select_only(-1);
            app_->layout();
          }
          focus_ = i;
          redraw();
          open_menu(i);
        }
        return 1;
      }
      bool ctrl = Fl::event_state() & FL_CTRL, shift = Fl::event_state() & FL_SHIFT;
      if (i < 0) {
        if (!ctrl && !shift && !selection().empty()) clear_selection();
        return 1;
      }
      bool any_selected = !selection().empty();
      // The round check mark toggles selection without opening.
      double tx = 0, ty = 0, ts = 0;
      tile_rect(size_t(i), tx, ty, ts);
      bool on_check = Fl::event_x() < tx + S(40) && Fl::event_y() < ty + S(40);
      if (shift) {
        move_focus(i, true, false);
      } else if (ctrl || on_check || (any_selected && !Fl::event_clicks())) {
        move_focus(i, false, true);
      } else if (Fl::event_clicks()) {
        Fl::event_clicks(0);
        app_->open_photo(i);
      } else {
        focus_ = anchor_ = i;
        redraw();
      }
      return 1;
    }
    case FL_DRAG:
      if (drag_bar_) {
        double len = std::max(double(S(40)), h() * h() / content_h_);
        double f = (Fl::event_y() - drag_offset_ - y()) / std::max(1.0, h() - len);
        scroll_ = target_ = std::max(0.0, std::min(max_scroll(), f * max_scroll()));
        redraw();
      }
      return 1;
    case FL_RELEASE:
      if (drag_bar_) {
        drag_bar_ = false;
        redraw();
      }
      return 1;
    case FL_KEYBOARD:
      return keyboard(Fl::event_key(), Fl::event_state()) ? 1 : 0;
  }
  return Fl_Widget::handle(event);
}

}  // namespace gallery
