// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The details list (or an icon grid): folders and files on disk, the
// members of an archive or find results, with a ".." row to go up.
// Multi-selection with Ctrl and Shift, keyboard navigation, sortable
// columns and dragging the selection out to a file manager.
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
  if (t <= 0) return "";
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

}  // namespace

FileView::FileView(TacaWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

int FileView::header_h() const { return grid_ ? 0 : S(26); }
int FileView::row_h() const { return S(26); }
int FileView::tile_w() const { return S(112); }
int FileView::tile_h() const { return S(112); }
int FileView::columns() const { return std::max(1, (w() - S(24)) / (tile_w() + S(6))); }

int FileView::content_h() const {
  int n = int(items_.size());
  if (grid_) return S(10) + ((n + columns() - 1) / columns()) * (tile_h() + S(6)) + S(10);
  return n * row_h() + S(4);
}

void FileView::clamp_scroll() {
  int view = h() - header_h();
  scroll_ = std::max(0, std::min(scroll_, std::max(0, content_h() - view)));
}

void FileView::resize(int x, int y, int w, int h) {
  Fl_Widget::resize(x, y, w, h);
  clamp_scroll();
}

void FileView::set_items(std::vector<int> items, bool parent_row, bool keep_selection) {
  std::set<int> keep;
  if (keep_selection)
    for (int n : selection()) keep.insert(n);
  int focus_node = focus_item();
  items_.clear();
  if (parent_row) items_.push_back(kParentItem);
  items_.insert(items_.end(), items.begin(), items.end());
  selected_.assign(items_.size(), 0);
  focus_ = -1;
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i] != kParentItem && keep.count(items_[i])) selected_[i] = 1;
    if (items_[i] == focus_node && keep_selection) focus_ = int(i);
  }
  if (!keep_selection) {
    scroll_ = 0;
    focus_ = items_.empty() ? -1 : 0;
  }
  anchor_ = focus_;
  hover_ = -1;
  clamp_scroll();
  redraw();
}

std::vector<int> FileView::selection() const {
  std::vector<int> out;
  for (size_t i = 0; i < items_.size(); ++i)
    if (selected_[i] && items_[i] != kParentItem) out.push_back(items_[i]);
  return out;
}

void FileView::select_all() {
  for (size_t i = 0; i < items_.size(); ++i) selected_[i] = items_[i] != kParentItem;
  app_->selection_changed();
  redraw();
}

void FileView::invert_selection() {
  for (size_t i = 0; i < items_.size(); ++i) selected_[i] = items_[i] != kParentItem && !selected_[i];
  app_->selection_changed();
  redraw();
}

void FileView::clear_selection() {
  std::fill(selected_.begin(), selected_.end(), 0);
  app_->selection_changed();
  redraw();
}

void FileView::select_node(int node) {
  for (size_t i = 0; i < items_.size(); ++i)
    if (items_[i] == node) {
      click(int(i), 0);
      ensure_visible(int(i));
      return;
    }
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
    int cols = columns(), gap = S(6);
    int total = cols * tile_w() + (cols - 1) * gap;
    int left = x() + std::max(S(10), (w() - S(12) - total) / 2);
    ix = left + (i % cols) * (tile_w() + gap);
    iy = y() + S(10) + (i / cols) * (tile_h() + gap) - scroll_;
    iw = tile_w();
    ih = tile_h();
  } else {
    ix = x();
    iy = y() + header_h() + i * row_h() - scroll_;
    iw = w() - S(12);
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
  if (iy < top) scroll_ -= top - iy;
  else if (iy + ih > bottom) scroll_ += iy + ih - bottom;
  clamp_scroll();
}

// Columns from the right edge; the name takes what is left. Narrow
// windows drop the less important columns first.
std::vector<FileView::Column> FileView::columns_layout() const {
  bool archive = app_->has_archive() && !app_->results(), results = app_->results();
  struct Want {
    std::string label;
    int w, align;
    SortKey key;
    int priority;  // higher is dropped first
  };
  std::vector<Want> want;
  if (results) {
    want = {{_("Folder"), S(250), 0, SortKey::Type, 2},
            {_("Size"), S(96), 2, SortKey::Size, 1},
            {_("Modified"), S(132), 0, SortKey::Modified, 3}};
  } else {
    want.push_back({_("Size"), S(96), 2, SortKey::Size, 1});
    if (archive) {
      want.push_back({_("Packed"), S(92), 2, SortKey::Packed, 4});
      want.push_back({_("Ratio"), S(58), 2, SortKey::Ratio, 5});
    }
    want.push_back({_("Type"), S(150), 0, SortKey::Type, 3});
    want.push_back({_("Modified"), S(132), 0, SortKey::Modified, 2});
  }
  int avail = w() - S(12), min_name = S(170);
  for (;;) {
    int sum = 0;
    for (const auto &c : want) sum += c.w;
    if (sum + min_name <= avail || want.empty()) break;
    auto worst = std::max_element(want.begin(), want.end(),
                                  [](const Want &a, const Want &b) { return a.priority < b.priority; });
    want.erase(worst);
  }
  int sum = 0;
  for (const auto &c : want) sum += c.w;
  std::vector<Column> out;
  out.push_back({_("Name"), x(), avail - sum, 0, SortKey::Name});
  int X = x() + avail - sum;
  for (const auto &c : want) {
    out.push_back({c.label, X, c.w, c.align, c.key});
    X += c.w;
  }
  return out;
}

void FileView::draw_icon(Painter &p, int item, double ix, double iy, double size) {
  const Theme &t = app_->theme();
  if (item == kParentItem) {
    icon::parent_folder(p, ix, iy, size, t.accent, t.dark);
    return;
  }
  const Node &n = app_->tree().node(item);
  if (!n.dir && app_->node_is_archive(item)) icon::archive(p, ix, iy, size, t.accent, t.dark);
  else icon::file(p, n.name, ix, iy, size, n.dir, t.accent, t.dark);
}

void FileView::draw_list(Painter *p, bool text_pass) {
  const Theme &t = app_->theme();
  const Tree &tree = app_->tree();
  auto cols = columns_layout();
  int hh = header_h();
  bool archive = app_->has_archive() && !app_->results();
  int first = std::max(0, scroll_ / row_h()), last = std::min(int(items_.size()), first + h() / row_h() + 2);
  fl_push_clip(x(), y() + hh, w(), h() - hh);
  for (int i = first; i < last; ++i) {
    int ix, iy, iw, ih;
    item_rect(i, ix, iy, iw, ih);
    int item = items_[size_t(i)];
    int isz = S(18);
    bool sel = selected_[size_t(i)];
    if (!text_pass) {
      if (i % 2 == 1) p->fill_rect(ix, iy, iw, ih, t.text.alpha(t.dark ? 0.025 : 0.022));
      if (sel) p->fill_rect(ix, iy, iw, ih, t.accent.alpha(Fl::focus() == this ? (t.dark ? 0.45 : 0.28) : 0.16));
      else if (i == hover_) p->fill_rect(ix, iy, iw, ih, t.hover.alpha(0.8));
      if (i == focus_ && Fl::focus() == this)
        p->stroke_rounded(ix + 1.5, iy + 0.5, iw - 3, ih - 1, S(3), t.accent, Sd(1.2));
      draw_icon(*p, item, ix + S(8), iy + (ih - isz) / 2.0, isz);
      if (item != kParentItem) {
        const Node &n = tree.node(item);
        if (n.encrypted) icon::draw(*p, icon::Lock, ix + S(8) + isz - S(7), iy + ih / 2.0, S(10), t.accent_text);
        if (n.unsafe) icon::draw(*p, icon::Warning, ix + S(8) + isz - S(8), iy + ih / 2.0, S(11), Color::hex(0xD9822B));
      }
    } else {
      text_font(t.font_size);
      Color fg = t.text, dim = t.text_dim;
      if (item == kParentItem) {
        draw_text("..", cols[0].x + S(34), iy, cols[0].w - S(40), ih, fg);
        for (size_t c = 1; c < cols.size(); ++c)
          if (cols[c].key == SortKey::Type && !app_->results())
            draw_text(_("Folder"), cols[c].x + S(8), iy, cols[c].w - S(12), ih, dim);
        continue;
      }
      const Node &n = tree.node(item);
      if (n.unsafe) fg = t.text_dim;
      draw_text(n.name, cols[0].x + S(34), iy, cols[0].w - S(40), ih, fg);
      for (size_t c = 1; c < cols.size(); ++c) {
        const Column &col = cols[c];
        std::string s;
        switch (col.key) {
          case SortKey::Size:
            if (!n.dir) s = n.size >= 0 ? group_digits(n.size) : "?";
            else if (archive) s = group_digits(n.size);
            break;
          case SortKey::Packed:
            s = n.packed >= 0 ? group_digits(n.packed) : "";
            break;
          case SortKey::Ratio: {
            double r = packed_ratio(n);
            s = r >= 0 ? format("%d%%", int(std::lround(r * 100))) : "";
            break;
          }
          case SortKey::Type:
            s = app_->results() ? app_->hit_folder(item) : app_->type_of(item);
            break;
          case SortKey::Modified: s = short_date(n.mtime); break;
          default: break;
        }
        int pad = S(8);
        draw_text(s, col.x + pad, iy, col.w - 2 * pad, ih, dim, col.align);
      }
    }
  }
  fl_pop_clip();
  // Column header.
  text_font(t.font_size - S(1));
  if (!text_pass) {
    p->fill_gradient(x(), y(), w(), hh, 0, t.surface, t.surface_alt);
    p->line(x(), y() + hh - 0.5, x() + w(), y() + hh - 0.5, t.border);
    for (size_t c = 0; c < cols.size(); ++c) {
      const Column &col = cols[c];
      if (c + 1 < cols.size() || col.x + col.w < x() + w() - S(14))
        p->line(col.x + col.w - 0.5, y() + S(5), col.x + col.w - 0.5, y() + hh - S(5), t.border);
      if (col.key != app_->sort_key()) continue;
      // Sort direction: a small triangle next to the label.
      double d = Sd(3.5), ay = y() + hh / 2.0;
      double ax = col.align == 2 ? col.x + S(8) : col.x + S(c == 0 ? 34 : 8) + text_width(col.label) + S(6);
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
    for (size_t c = 0; c < cols.size(); ++c) {
      const Column &col = cols[c];
      bool active = app_->sort_key() == col.key;
      int lx = col.x + S(c == 0 ? 34 : 8);
      draw_text(col.label, lx, y(), col.w - (lx - col.x) - S(8), hh, active ? t.text : t.text_dim, col.align);
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
    int item = items_[size_t(i)];
    int isz = S(52);
    double icx = ix + (iw - isz) / 2.0, icy = iy + S(10);
    if (!text_pass) {
      bool sel = selected_[size_t(i)];
      if (sel) p->fill_rounded(ix, iy, iw, ih, S(10), t.accent.alpha(t.dark ? 0.34 : 0.2));
      else if (i == hover_) p->fill_rounded(ix, iy, iw, ih, S(10), t.hover);
      if (i == focus_ && Fl::focus() == this) p->stroke_rounded(ix + 0.5, iy + 0.5, iw - 1, ih - 1, S(10), t.accent, Sd(1.5));
      draw_icon(*p, item, icx, icy, isz);
    } else {
      std::string name = item == kParentItem ? ".." : tree.node(item).name;
      if (item != kParentItem && !tree.node(item).dir && !app_->node_is_archive(item)) {
        int lx, ly, lw, lh;
        icon::file_label_box(name, icx, icy, isz, lx, ly, lw, lh);
        if (lw > 0) {
          text_font(std::max(S(8), lh - S(5)), true);
          std::string e = extension_label(name);
          if (e.size() > 4) e = e.substr(0, 4);
          draw_text(e, lx, ly, lw, lh, Color::hex(0xFFFFFF), 1);
        }
      }
      text_font(t.font_size - S(1));
      int tw = iw - S(10);
      std::string l1 = name, l2;
      if (text_width(name) > tw) {
        size_t cut = name.size();
        while (cut > 1 && text_width(name.substr(0, cut)) > tw) --cut;
        while (cut > 1 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        l1 = name.substr(0, cut);
        l2 = name.substr(cut);
      }
      int ty = int(icy + isz + S(4));
      draw_text(l1, ix + S(5), ty, tw, S(18), t.text, 1);
      if (!l2.empty()) draw_text(l2, ix + S(5), ty + S(16), tw, S(18), t.text, 1);
    }
  }
  fl_pop_clip();
}

void FileView::draw() {
  const Theme &t = app_->theme();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), t.surface);
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
  bool only_parent = items_.size() == 1 && items_[0] == kParentItem;
  if (items_.empty() || only_parent) {
    text_font(t.font_size);
    draw_text(app_->results() ? _("No files match the pattern") : _("This folder is empty"), x(),
              y() + header_h() + row_h() * 2, w(), S(30), t.text_dim, 1);
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
  if (state & FL_ALT) return false;  // Alt+letter: menu shortcuts
  if (ctrl && key == 'a') {
    select_all();
    return true;
  }
  if (key == FL_Enter || key == FL_KP_Enter) {
    if (focus_ >= 0) app_->enter(items_[size_t(focus_)]);
    return true;
  }
  if (key == FL_BackSpace) {
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
    default: {
      // Type-ahead: jump to the next name starting with the letter.
      const char *txt = Fl::event_text();
      if (!ctrl && txt && txt[0] && static_cast<unsigned char>(txt[0]) > ' ' && n > 0) {
        char c = char(tolower(static_cast<unsigned char>(txt[0])));
        for (int k = 1; k <= n; ++k) {
          int i = (std::max(0, focus_) + k) % n;
          if (items_[size_t(i)] == kParentItem) continue;
          const std::string &name = app_->tree().node(items_[size_t(i)]).name;
          if (!name.empty() && tolower(static_cast<unsigned char>(name[0])) == c) {
            click(i, 0);
            ensure_visible(i);
            return true;
          }
        }
        return true;
      }
      return false;
    }
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
      scroll_ += Fl::event_dy() * row_h() * 3;
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
        for (const auto &c : columns_layout())
          if (mx >= c.x && mx < c.x + c.w) app_->set_sort(c.key);
        return 1;
      }
      int i = item_at(mx, my);
      press_x_ = mx;
      press_y_ = my;
      press_item_ = i;
      drag_started_ = false;
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (i >= 0 && !selected_[size_t(i)]) click(i, 0);
        app_->context_menu(i >= 0 ? items_[size_t(i)] : -2);
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
      if (press_item_ >= 0 && items_[size_t(press_item_)] != kParentItem && !drag_started_ &&
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
