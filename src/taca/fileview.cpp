// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The archive contents: a list with sortable columns, or a grid of file
// cards. Multi-selection with Ctrl and Shift, keyboard navigation and
// dragging the selection out to a file manager.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "i18n.h"
#include "taca.h"
#include "util.h"

using namespace sde;

namespace taca {

namespace {

std::string short_date(time_t t) {
  if (t <= 0) return "—";
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

}  // namespace

FileView::FileView(TacaWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

int FileView::header_h() const { return grid_ ? 0 : S(34); }
int FileView::row_h() const { return S(36); }
int FileView::tile_w() const { return S(118); }
int FileView::tile_h() const { return S(124); }
int FileView::columns() const { return std::max(1, (w() - S(24)) / (tile_w() + S(8))); }

int FileView::content_h() const {
  int n = int(items_.size());
  if (grid_) return S(12) + ((n + columns() - 1) / columns()) * (tile_h() + S(8)) + S(12);
  return n * row_h() + S(8);
}

void FileView::clamp_scroll() {
  int view = h() - header_h();
  scroll_ = std::max(0, std::min(scroll_, std::max(0, content_h() - view)));
}

void FileView::resize(int x, int y, int w, int h) {
  Fl_Widget::resize(x, y, w, h);
  clamp_scroll();
}

void FileView::set_items(std::vector<int> items, bool keep_selection) {
  std::set<int> keep;
  if (keep_selection)
    for (int n : selection()) keep.insert(n);
  int focus_node = focus_item();
  items_ = std::move(items);
  selected_.assign(items_.size(), 0);
  focus_ = -1;
  for (size_t i = 0; i < items_.size(); ++i) {
    if (keep.count(items_[i])) selected_[i] = 1;
    if (items_[i] == focus_node && keep_selection) focus_ = int(i);
  }
  if (!keep_selection) scroll_ = 0;
  anchor_ = focus_;
  hover_ = -1;
  clamp_scroll();
  redraw();
}

std::vector<int> FileView::selection() const {
  std::vector<int> out;
  for (size_t i = 0; i < items_.size(); ++i)
    if (selected_[i]) out.push_back(items_[i]);
  return out;
}

void FileView::select_all() {
  std::fill(selected_.begin(), selected_.end(), 1);
  app_->selection_changed();
  redraw();
}

void FileView::clear_selection() {
  std::fill(selected_.begin(), selected_.end(), 0);
  app_->selection_changed();
  redraw();
}

void FileView::set_grid(bool g) {
  grid_ = g;
  scroll_ = 0;
  clamp_scroll();
  if (focus_ >= 0) ensure_visible(focus_);
  redraw();
}

void FileView::item_rect(int i, int &ix, int &iy, int &iw, int &ih) const {
  if (grid_) {
    int cols = columns();
    int gap = S(8);
    int total = cols * tile_w() + (cols - 1) * gap;
    int left = x() + std::max(S(12), (w() - S(12) - total) / 2);
    ix = left + (i % cols) * (tile_w() + gap);
    iy = y() + S(12) + (i / cols) * (tile_h() + gap) - scroll_;
    iw = tile_w();
    ih = tile_h();
  } else {
    ix = x() + S(8);
    iy = y() + header_h() + i * row_h() - scroll_;
    iw = w() - S(22);
    ih = row_h();
  }
}

int FileView::item_at(int mx, int my) const {
  if (my < y() + header_h() || my >= y() + h() || mx >= x() + w() - S(10)) return -1;
  if (grid_) {
    for (int i = 0; i < int(items_.size()); ++i) {
      int ix, iy, iw, ih;
      item_rect(i, ix, iy, iw, ih);
      if (mx >= ix && mx < ix + iw && my >= iy && my < iy + ih) return i;
    }
    return -1;
  }
  int i = (my - y() - header_h() + scroll_) / row_h();
  return i >= 0 && i < int(items_.size()) ? i : -1;
}

void FileView::ensure_visible(int i) {
  if (i < 0) return;
  int ix, iy, iw, ih;
  item_rect(i, ix, iy, iw, ih);
  int top = y() + header_h(), bottom = y() + h();
  if (iy < top) scroll_ -= top - iy + S(4);
  else if (iy + ih > bottom) scroll_ += iy + ih - bottom + S(4);
  clamp_scroll();
}

void FileView::column_x(int &name, int &size, int &packed, int &type, int &modified, int &end) const {
  end = x() + w() - S(22);
  int left = x() + S(8);
  int width = end - left;
  bool wide = width > S(720), mid = width > S(520);
  modified = wide ? end - S(150) : end;
  type = mid ? modified - S(130) : modified;
  packed = type - S(96);
  size = packed - S(96);
  name = left;
}

int FileView::header_column_at(int mx) const {
  int nx, sx, px, tx, mx2, end;
  column_x(nx, sx, px, tx, mx2, end);
  if (mx < sx) return 0;
  if (mx < px) return 1;
  if (mx < tx) return 2;
  if (mx < mx2) return 3;
  return 4;
}

void FileView::draw_list(Painter *p, bool text_pass) {
  const Theme &t = app_->theme();
  const Tree &tree = app_->tree();
  int nx, sx, px, tx, mx, end;
  column_x(nx, sx, px, tx, mx, end);
  int hh = header_h();
  int first = std::max(0, scroll_ / row_h()), last = std::min(int(items_.size()), first + h() / row_h() + 2);
  fl_push_clip(x(), y() + hh, w(), h() - hh);
  for (int i = first; i < last; ++i) {
    int ix, iy, iw, ih;
    item_rect(i, ix, iy, iw, ih);
    const Node &n = tree.node(items_[size_t(i)]);
    int icon = S(22);
    if (!text_pass) {
      bool sel = selected_[size_t(i)];
      if (sel) p->fill_rounded(ix, iy + S(2), iw, ih - S(4), S(8), t.accent.alpha(t.dark ? 0.34 : 0.18));
      else if (i == hover_) p->fill_rounded(ix, iy + S(2), iw, ih - S(4), S(8), t.hover);
      if (i == focus_ && Fl::focus() == this)
        p->stroke_rounded(ix + 0.5, iy + S(2) + 0.5, iw - 1, ih - S(4) - 1, S(8), t.accent, Sd(1.5));
      icon::file(*p, n.name, nx + S(8), iy + (ih - icon) / 2.0, icon, n.dir, t.accent, t.dark);
      if (n.encrypted) icon::draw(*p, icon::Lock, nx + S(8) + icon - S(8), iy + ih / 2.0 + S(1), S(11), t.accent_text);
      if (n.unsafe) icon::draw(*p, icon::Warning, nx + S(8) + icon - S(9), iy + ih / 2.0, S(12), Color::hex(0xD9822B));
    } else {
      Color fg = n.unsafe ? t.text_dim : t.text;
      text_font(t.font_size);
      int name_w = sx - nx - S(48);
      std::string shown = n.name;
      if (app_->searching() && !n.path.empty() && n.path != n.name) {
        draw_text(n.name, nx + S(40), iy + S(3), name_w, ih / 2, fg);
        text_font(t.font_size - S(2));
        draw_text(n.path.substr(0, n.path.size() - n.name.size()), nx + S(40), iy + ih / 2 - S(2), name_w,
                  ih / 2, t.text_dim);
        text_font(t.font_size);
      } else {
        draw_text(shown, nx + S(40), iy, name_w, ih, fg);
      }
      Color dim = t.text_dim;
      std::string size = n.dir ? (n.files == 1 ? std::string(_("1 item")) : format(_("%zu items"), n.files))
                               : human_size(n.size);
      draw_text(size, sx, iy, px - sx - S(10), ih, dim, 2);
      std::string packed = n.packed >= 0 ? human_size(n.packed) : "—";
      draw_text(packed, px, iy, tx - px - S(10), ih, dim, 2);
      if (mx > tx) draw_text(file_kind(n.name, n.dir), tx + S(14), iy, mx - tx - S(18), ih, dim);
      if (end > mx) draw_text(short_date(n.mtime), mx + S(4), iy, end - mx - S(4), ih, dim);
    }
  }
  fl_pop_clip();
  // Column header.
  struct Col {
    const char *label;
    int x, w, align;
    SortKey key;
  } cols[] = {{_("Name"), nx + S(40), sx - nx - S(48), 0, SortKey::Name},
              {_("Size"), sx, px - sx - S(10), 2, SortKey::Size},
              {_("Packed"), px, tx - px - S(10), 2, SortKey::Packed},
              {_("Type"), tx + S(14), mx - tx - S(18), 0, SortKey::Type},
              {_("Modified"), mx + S(4), end - mx - S(4), 0, SortKey::Modified}};
  text_font(t.font_size - S(1), true);
  if (!text_pass) {
    p->fill_rect(x(), y(), w(), hh, t.background);
    p->line(x() + S(8), y() + hh - 0.5, x() + w() - S(14), y() + hh - 0.5, t.border.alpha(0.8));
    // Sort direction: a small triangle next to the active column's label.
    for (const auto &c : cols) {
      if (c.key != app_->sort_key() || c.w <= S(10)) continue;
      double d = Sd(4), ay = y() + hh / 2.0;
      double ax = c.align == 2 ? c.x + c.w - 2 * d : c.x + text_width(c.label) + S(6);
      cairo_t *cr = p->cr();
      double dir = app_->sort_descending() ? 1 : -1;
      cairo_move_to(cr, ax, ay - dir * d / 2);
      cairo_line_to(cr, ax + 2 * d, ay - dir * d / 2);
      cairo_line_to(cr, ax + d, ay + dir * d / 2 + dir);
      cairo_close_path(cr);
      p->set(t.accent_text);
      cairo_fill(cr);
    }
  } else {
    for (const auto &c : cols) {
      if (c.w <= S(10)) continue;
      bool active = app_->sort_key() == c.key;
      int cw = c.w - (active && c.align == 2 ? S(14) : 0);
      draw_text(c.label, c.x, y(), cw, hh, active ? t.accent_text : t.text_dim, c.align);
    }
  }
}

void FileView::draw_grid(Painter *p, bool text_pass) {
  const Theme &t = app_->theme();
  const Tree &tree = app_->tree();
  fl_push_clip(x(), y(), w(), h());
  for (int i = 0; i < int(items_.size()); ++i) {
    int ix, iy, iw, ih;
    item_rect(i, ix, iy, iw, ih);
    if (iy + ih < y() || iy > y() + h()) continue;
    const Node &n = tree.node(items_[size_t(i)]);
    int icon = S(58);
    double icx = ix + (iw - icon) / 2.0, icy = iy + S(10);
    if (!text_pass) {
      bool sel = selected_[size_t(i)];
      if (sel) p->fill_rounded(ix, iy, iw, ih, S(12), t.accent.alpha(t.dark ? 0.34 : 0.18));
      else if (i == hover_) p->fill_rounded(ix, iy, iw, ih, S(12), t.hover);
      if (i == focus_ && Fl::focus() == this) p->stroke_rounded(ix + 0.5, iy + 0.5, iw - 1, ih - 1, S(12), t.accent, Sd(1.5));
      icon::file(*p, n.name, icx, icy, icon, n.dir, t.accent, t.dark);
      if (n.encrypted) {
        p->circle(icx + icon - S(6), icy + icon - S(6), S(10), t.surface);
        icon::draw(*p, icon::Lock, icx + icon - S(14), icy + icon - S(14), S(16), t.accent_text);
      }
    } else {
      int lx, ly, lw, lh;
      icon::file_label_box(n.name, icx, icy, icon, lx, ly, lw, lh);
      if (!n.dir && lw > 0) {
        text_font(std::max(S(8), lh - S(5)), true);
        std::string e = extension_label(n.name);
        if (e.size() > 4) e = e.substr(0, 4);
        draw_text(e, lx, ly, lw, lh, Color::hex(0xFFFFFF), 1);
      }
      text_font(t.font_size - S(1));
      // Two lines for long names.
      std::string name = n.name;
      int tw = iw - S(12);
      std::string l1 = name, l2;
      if (text_width(name) > tw) {
        size_t cut = name.size();
        while (cut > 1 && text_width(name.substr(0, cut)) > tw) --cut;
        while (cut > 1 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        l1 = name.substr(0, cut);
        l2 = name.substr(cut);
      }
      int ty = int(icy + icon + S(6));
      draw_text(l1, ix + S(6), ty, tw, S(18), t.text, 1);
      if (!l2.empty()) draw_text(l2, ix + S(6), ty + S(16), tw, S(18), t.text, 1);
      else {
        text_font(t.font_size - S(2));
        std::string sub = n.dir ? format(_("%zu items"), n.files) : human_size(n.size);
        draw_text(sub, ix + S(6), ty + S(16), tw, S(18), t.text_dim, 1);
      }
    }
  }
  fl_pop_clip();
}

void FileView::draw() {
  const Theme &t = app_->theme();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), t.background);
    if (grid_) draw_grid(&p, false);
    else draw_list(&p, false);
    // Scroll bar.
    int view = h() - header_h(), total = content_h();
    if (total > view) {
      double bar_h = std::max(double(S(36)), view * double(view) / total);
      double by = y() + header_h() + (view - bar_h) * scroll_ / double(total - view);
      bool hot = dragging_bar_ || (Fl::belowmouse() == this && Fl::event_x() > x() + w() - S(14));
      double bw = hot ? S(8) : S(5);
      p.fill_rounded(x() + w() - bw - S(3), by + S(2), bw, bar_h - S(4), bw / 2, t.text_dim.alpha(hot ? 0.55 : 0.35));
    }
  }
  if (grid_) draw_grid(nullptr, true);
  else draw_list(nullptr, true);
  if (items_.empty()) {
    text_font(t.font_size + S(1));
    draw_text(app_->searching() ? _("Nothing matches your search") : _("This folder is empty"), x(),
              y() + h() / 3, w(), S(30), t.text_dim, 1);
  }
}

void FileView::click(int i, int state) {
  if (i < 0) {
    if (!(state & (FL_CTRL | FL_SHIFT))) clear_selection();
    return;
  }
  if (state & FL_SHIFT && anchor_ >= 0) {
    if (!(state & FL_CTRL)) std::fill(selected_.begin(), selected_.end(), 0);
    for (int k = std::min(anchor_, i); k <= std::max(anchor_, i); ++k) selected_[size_t(k)] = 1;
  } else if (state & FL_CTRL) {
    selected_[size_t(i)] = !selected_[size_t(i)];
    anchor_ = i;
  } else {
    std::fill(selected_.begin(), selected_.end(), 0);
    selected_[size_t(i)] = 1;
    anchor_ = i;
  }
  focus_ = i;
  app_->selection_changed();
  redraw();
}

bool FileView::keyboard(int key, int state) {
  int n = int(items_.size());
  bool ctrl = state & FL_CTRL, shift = state & FL_SHIFT;
  if (ctrl && key == 'a') {
    select_all();
    return true;
  }
  if (key == FL_Enter || key == FL_KP_Enter) {
    if (focus_ >= 0) app_->enter(items_[size_t(focus_)]);
    return true;
  }
  if (key == FL_BackSpace || (key == FL_Up && (state & FL_ALT))) {
    app_->go_up();
    return true;
  }
  if (key == FL_Menu) {
    if (focus_ >= 0) app_->context_menu(items_[size_t(focus_)]);
    return true;
  }
  if (key == ' ' && focus_ >= 0) {
    click(focus_, FL_CTRL);
    return true;
  }
  int step = 0;
  int cols = grid_ ? columns() : 1;
  int page = std::max(1, (h() - header_h()) / (grid_ ? tile_h() : row_h())) * cols;
  switch (key) {
    case FL_Down: step = cols; break;
    case FL_Up: step = -cols; break;
    case FL_Right: step = grid_ ? 1 : 0; break;
    case FL_Left: step = grid_ ? -1 : 0; break;
    case FL_Page_Down: step = page; break;
    case FL_Page_Up: step = -page; break;
    case FL_Home: step = -n; break;
    case FL_End: step = n; break;
    default: return false;
  }
  if (n == 0) return true;
  int target = focus_ < 0 ? 0 : std::max(0, std::min(n - 1, focus_ + step));
  if (shift) {
    if (anchor_ < 0) anchor_ = std::max(0, focus_);
    std::fill(selected_.begin(), selected_.end(), 0);
    for (int k = std::min(anchor_, target); k <= std::max(anchor_, target); ++k) selected_[size_t(k)] = 1;
    focus_ = target;
    app_->selection_changed();
  } else if (ctrl) {
    focus_ = target;
  } else {
    click(target, 0);
  }
  ensure_visible(focus_);
  redraw();
  return true;
}

int FileView::handle(int event) {
  switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_ENTER:
    case FL_MOVE: {
      int hv = item_at(Fl::event_x(), Fl::event_y());
      if (hv != hover_) {
        hover_ = hv;
        redraw();
      }
      if (Fl::event_x() > x() + w() - S(14)) redraw();
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_MOUSEWHEEL:
      scroll_ += Fl::event_dy() * S(60);
      clamp_scroll();
      redraw();
      return 1;
    case FL_PUSH: {
      if (Fl::visible_focus()) Fl::focus(this);
      int mx = Fl::event_x(), my = Fl::event_y();
      int view = h() - header_h(), total = content_h();
      if (mx > x() + w() - S(14) && total > view) {
        dragging_bar_ = true;
        double bar_h = std::max(double(S(36)), view * double(view) / total);
        double by = y() + header_h() + (view - bar_h) * scroll_ / double(total - view);
        bar_offset_ = (my >= by && my <= by + bar_h) ? int(my - by) : int(bar_h / 2);
        return handle(FL_DRAG);
      }
      if (!grid_ && my < y() + header_h()) {
        static const SortKey keys[] = {SortKey::Name, SortKey::Size, SortKey::Packed, SortKey::Type, SortKey::Modified};
        app_->set_sort(keys[header_column_at(mx)]);
        return 1;
      }
      int i = item_at(mx, my);
      press_x_ = mx;
      press_y_ = my;
      press_item_ = i;
      drag_started_ = false;
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (i >= 0 && !selected_[size_t(i)]) click(i, 0);
        app_->context_menu(i >= 0 ? items_[size_t(i)] : -1);
        return 1;
      }
      if (Fl::event_clicks() && i >= 0) {
        Fl::event_clicks(0);
        app_->enter(items_[size_t(i)]);
        return 1;
      }
      // Pressing on a selected item keeps the selection for a drag.
      if (i >= 0 && selected_[size_t(i)] && !(Fl::event_state() & (FL_CTRL | FL_SHIFT))) {
        focus_ = i;
        redraw();
        return 1;
      }
      click(i, Fl::event_state());
      return 1;
    }
    case FL_DRAG: {
      if (dragging_bar_) {
        int view = h() - header_h(), total = content_h();
        double bar_h = std::max(double(S(36)), view * double(view) / total);
        double f = (Fl::event_y() - bar_offset_ - y() - header_h()) / std::max(1.0, view - bar_h);
        scroll_ = int(f * (total - view));
        clamp_scroll();
        redraw();
        return 1;
      }
      if (press_item_ >= 0 && !drag_started_ &&
          std::abs(Fl::event_x() - press_x_) + std::abs(Fl::event_y() - press_y_) > S(10)) {
        drag_started_ = true;
        if (!selected_[size_t(press_item_)]) click(press_item_, 0);
        app_->start_drag();
      }
      return 1;
    }
    case FL_RELEASE:
      if (dragging_bar_) {
        dragging_bar_ = false;
        redraw();
        return 1;
      }
      if (!drag_started_ && press_item_ >= 0 && selected_[size_t(press_item_)] &&
          !(Fl::event_state() & (FL_CTRL | FL_SHIFT)) && Fl::event_button() == FL_LEFT_MOUSE)
        click(press_item_, 0);
      press_item_ = -1;
      return 1;
    case FL_KEYBOARD:
      return keyboard(Fl::event_key(), Fl::event_state()) ? 1 : 0;
  }
  return Fl_Widget::handle(event);
}

}  // namespace taca
