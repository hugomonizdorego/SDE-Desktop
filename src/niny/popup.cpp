// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Themed popup menus drawn with cairo: rounded hover rows, icons, check
// marks, shortcuts, cascading submenus, an optional quick action row and
// keyboard navigation. Each level is an override-redirect window; the first
// one holds the pointer grab and routes every event by root position.
#include "popup.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <algorithm>
#include <cmath>

#include "niny.h"
#include "util.h"

using namespace sde;

namespace niny {

namespace {

class Controller;

class Level : public BufferedWindow {
 public:
  Level(Controller *c, const std::vector<MenuItem> *items, const std::vector<QuickAction> *quick, int min_w)
      : BufferedWindow(0, 0, 100, 100), ctl_(c), items_(items), quick_(quick) {
    set_override();
    border(0);
    end();
    measure(min_w);
  }
  int handle(int event) override;
  void draw() override;

  struct Row {
    int y = 0, h = 0;
  };
  // Index of the item under window point (wx, wy), -1 for none; quick
  // buttons are returned as -2 - index.
  int hit(int wx, int wy) const {
    if (quick_ && !quick_->empty() && wy < quick_h_) {
      int n = int(quick_->size());
      int bw = (w() - 2 * S(6)) / n;
      int i = (wx - S(6)) / std::max(1, bw);
      if (wx >= S(6) && i >= 0 && i < n) return -2 - i;
      return -1;
    }
    int y = wy + scroll_;
    for (size_t i = 0; i < rows_.size(); ++i)
      if (y >= rows_[i].y && y < rows_[i].y + rows_[i].h && !(*items_)[i].separator) return int(i);
    return -1;
  }
  int row_top(int i) const { return rows_[size_t(i)].y - scroll_; }
  void ensure_visible(int i) {
    if (i < 0 || content_h_ <= h()) return;
    int top = rows_[size_t(i)].y, bottom = top + rows_[size_t(i)].h;
    if (top - scroll_ < quick_h_ + S(4)) scroll_ = top - quick_h_ - S(4);
    if (bottom - scroll_ > h() - S(4)) scroll_ = bottom - h() + S(4);
    scroll_ = std::max(0, std::min(scroll_, content_h_ - h()));
  }
  void scroll_by(int d) {
    if (content_h_ <= h()) return;
    scroll_ = std::max(0, std::min(scroll_ + d, content_h_ - h()));
    redraw();
  }
  void clamp_height(int max_h) {
    if (content_h_ > max_h) size(w(), max_h);
  }

  Controller *ctl_;
  const std::vector<MenuItem> *items_;
  const std::vector<QuickAction> *quick_;
  std::vector<Row> rows_;
  int hover_ = -1;
  int open_sub_ = -1;  // item whose submenu is open
  int quick_h_ = 0, content_h_ = 0, scroll_ = 0;
  int icon_col_ = 0, label_w_ = 0, short_w_ = 0;
  bool chevrons_ = false;

 private:
  void measure(int min_w) {
    const Look &L = look();
    bool icons = false;
    for (auto &it : *items_)
      if (it.icon >= 0 || it.checked || it.radio) icons = true;
    for (auto &it : *items_)
      if (!it.sub.empty()) chevrons_ = true;
    icon_col_ = icons ? S(34) : S(10);
    text_font(L.theme.font_size);
    label_w_ = 0;
    short_w_ = 0;
    for (auto &it : *items_) {
      if (it.separator) continue;
      label_w_ = std::max(label_w_, text_width(it.label));
      if (!it.hint.empty()) {
        text_font(L.theme.font_size - S(2));
        label_w_ = std::max(label_w_, text_width(it.hint));
        text_font(L.theme.font_size);
      }
      if (!it.shortcut.empty()) short_w_ = std::max(short_w_, text_width(it.shortcut));
    }
    int W = S(6) + icon_col_ + label_w_ + (short_w_ ? S(40) + short_w_ : S(24)) + (chevrons_ ? S(24) : 0) + S(14);
    W = std::max(W, S(min_w));
    W = std::max(W, S(180));
    int y = S(5);
    if (quick_ && !quick_->empty()) {
      int need = S(12);
      for (auto &q : *quick_) need += std::max(S(58), text_width(q.label) + S(18));
      W = std::max(W, need);
      quick_h_ = S(66);
      y = quick_h_ + S(5);
    }
    rows_.clear();
    for (auto &it : *items_) {
      Row r;
      r.y = y;
      r.h = it.separator ? S(9) : it.hint.empty() ? S(34) : S(48);
      y += r.h;
      rows_.push_back(r);
    }
    content_h_ = y + S(5);
    size(W, content_h_);
  }
};

class Controller {
 public:
  explicit Controller(const MenuSpec &spec) : spec_(spec) {}
  ~Controller() {
    for (Level *l : levels_) {
      l->hide();
      Fl::delete_widget(l);
    }
  }

  void open(int x, int y, int anchor_h) {
    auto *l = new Level(this, &spec_.items, &spec_.quick, spec_.min_width);
    int sx, sy, sw, sh;
    Fl::screen_work_area(sx, sy, sw, sh, x, y);
    l->clamp_height(sh - S(16));
    int X = x, Y = y;
    if (anchor_h < 0) {
      Y = y - l->h() - S(2);
      if (Y < sy) Y = y;
    } else if (Y + l->h() > sy + sh) {
      if (anchor_h > 0 && y - anchor_h - l->h() >= sy) Y = y - anchor_h - l->h();
      else if (anchor_h == 0 && y - l->h() >= sy) Y = y - l->h();
      else Y = sy + sh - l->h();
    }
    if (X + l->w() > sx + sw) X = anchor_h > 0 ? sx + sw - l->w() : std::max(sx, x - l->w());
    X = std::max(sx, X);
    Y = std::max(sy, Y);
    l->position(X, Y);
    levels_.push_back(l);
    l->show();
    open_time_ = now_seconds();
    Fl::grab(l);
  }

  void open_sub(size_t level, int index) {
    close_after(level);
    Level *p = levels_[level];
    const MenuItem &it = (*p->items_)[size_t(index)];
    if (it.sub.empty() || !it.enabled) return;
    auto *l = new Level(this, &it.sub, nullptr, 0);
    int sx, sy, sw, sh;
    Fl::screen_work_area(sx, sy, sw, sh, p->x(), p->y());
    l->clamp_height(sh - S(16));
    int X = p->x() + p->w() - S(4);
    if (X + l->w() > sx + sw) X = p->x() - l->w() + S(4);
    int Y = p->y() + p->row_top(index) - S(5);
    if (Y + l->h() > sy + sh) Y = sy + sh - l->h();
    l->position(std::max(sx, X), std::max(sy, Y));
    p->open_sub_ = index;
    levels_.push_back(l);
    l->show();
    p->redraw();
  }

  void close_after(size_t level) {
    while (levels_.size() > level + 1) {
      Level *l = levels_.back();
      levels_.pop_back();
      l->hide();
      Fl::delete_widget(l);
    }
    levels_[level]->open_sub_ = -1;
    levels_[level]->redraw();
  }

  int level_at(int rx, int ry) const {
    for (int i = int(levels_.size()) - 1; i >= 0; --i) {
      Level *l = levels_[size_t(i)];
      if (rx >= l->x() && rx < l->x() + l->w() && ry >= l->y() && ry < l->y() + l->h()) return i;
    }
    return -1;
  }

  void choose(std::function<void()> fn) {
    chosen_ = std::move(fn);
    done_ = true;
  }

  void activate(size_t level, int index) {
    Level *l = levels_[level];
    if (index <= -2) {
      const QuickAction &q = (*l->quick_)[size_t(-2 - index)];
      if (q.enabled) choose(q.run);
      return;
    }
    if (index < 0) return;
    const MenuItem &it = (*l->items_)[size_t(index)];
    if (!it.enabled || it.separator) return;
    if (!it.sub.empty()) {
      open_sub(level, index);
      Level *s = levels_.back();
      s->hover_ = next_enabled(s, -1, 1);
      s->redraw();
      return;
    }
    choose(it.run ? it.run : [] {});
  }

  static int next_enabled(Level *l, int from, int dir) {
    int n = int(l->items_->size());
    int i = from;
    for (int k = 0; k < n; ++k) {
      i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
      const MenuItem &it = (*l->items_)[size_t(i)];
      if (!it.separator && it.enabled) return i;
    }
    return -1;
  }

  int handle(int event) {
    int rx = Fl::event_x_root(), ry = Fl::event_y_root();
    int li = level_at(rx, ry);
    switch (event) {
      case FL_ENTER:
      case FL_MOVE:
      case FL_DRAG: {
        if (li < 0) return 1;
        Level *l = levels_[size_t(li)];
        int h = l->hit(rx - l->x(), ry - l->y());
        if (h != l->hover_) {
          l->hover_ = h;
          l->redraw();
        }
        if (h >= 0) {
          const MenuItem &it = (*l->items_)[size_t(h)];
          if (!it.sub.empty() && it.enabled) {
            if (l->open_sub_ != h) open_sub(size_t(li), h);
          } else if (size_t(li) + 1 < levels_.size()) {
            close_after(size_t(li));
          }
        }
        return 1;
      }
      case FL_PUSH:
        if (li < 0) {
          done_ = true;
          return 1;
        }
        pressed_ = true;
        return 1;
      case FL_RELEASE: {
        if (li < 0) return 1;
        if (!pressed_ && now_seconds() - open_time_ < 0.35) return 1;
        pressed_ = false;
        Level *l = levels_[size_t(li)];
        int h = l->hit(rx - l->x(), ry - l->y());
        if (h >= 0 && !(*l->items_)[size_t(h)].sub.empty()) return 1;
        activate(size_t(li), h);
        return 1;
      }
      case FL_MOUSEWHEEL:
        if (li >= 0) levels_[size_t(li)]->scroll_by(Fl::event_dy() * S(34));
        return 1;
      case FL_KEYBOARD:
      case FL_SHORTCUT: {
        Level *l = levels_.back();
        int key = Fl::event_key();
        if (key == FL_Escape) {
          if (levels_.size() > 1) close_after(levels_.size() - 2);
          else done_ = true;
          return 1;
        }
        if (key == FL_Down || key == FL_Up || (key == FL_Tab)) {
          int dir = key == FL_Up || (key == FL_Tab && (Fl::event_state() & FL_SHIFT)) ? -1 : 1;
          l->hover_ = next_enabled(l, l->hover_ < 0 ? -1 : l->hover_, dir);
          l->ensure_visible(l->hover_);
          l->redraw();
          return 1;
        }
        if (key == FL_Right) {
          if (l->hover_ >= 0 && !(*l->items_)[size_t(l->hover_)].sub.empty())
            activate(levels_.size() - 1, l->hover_);
          return 1;
        }
        if (key == FL_Left) {
          if (levels_.size() > 1) close_after(levels_.size() - 2);
          return 1;
        }
        if (key == FL_Enter || key == FL_KP_Enter || key == ' ') {
          activate(levels_.size() - 1, l->hover_);
          return 1;
        }
        // First letter of a label.
        const char *t = Fl::event_text();
        if (t && t[0] && Fl::event_length() == 1) {
          char c = char(tolower(static_cast<unsigned char>(t[0])));
          int n = int(l->items_->size());
          for (int k = 1; k <= n; ++k) {
            int i = (std::max(l->hover_, -1) + k + n) % n;
            const MenuItem &it = (*l->items_)[size_t(i)];
            if (!it.separator && it.enabled && !it.label.empty() &&
                tolower(static_cast<unsigned char>(it.label[0])) == c) {
              l->hover_ = i;
              l->ensure_visible(i);
              l->redraw();
              break;
            }
          }
        }
        return 1;
      }
    }
    return 1;
  }

  bool run() {
    while (!done_ && !levels_.empty() && levels_[0]->shown()) Fl::wait();
    if (Fl::grab() == levels_[0]) Fl::grab(nullptr);
    for (Level *l : levels_) l->hide();
    Fl::flush();
    if (chosen_) {
      chosen_();
      return true;
    }
    return false;
  }

 private:
  const MenuSpec &spec_;
  std::vector<Level *> levels_;
  bool done_ = false, pressed_ = false;
  double open_time_ = 0;
  std::function<void()> chosen_;
};

int Level::handle(int event) {
  switch (event) {
    case FL_ENTER:
    case FL_MOVE:
    case FL_DRAG:
    case FL_PUSH:
    case FL_RELEASE:
    case FL_MOUSEWHEEL:
    case FL_KEYBOARD:
    case FL_SHORTCUT:
      return ctl_->handle(event);
    case FL_LEAVE:
      if (hover_ >= 0 && open_sub_ != hover_) {
        hover_ = -1;
        redraw();
      }
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      return 1;
  }
  return BufferedWindow::handle(event);
}

void Level::draw() {
  const Look &L = look();
  Color bg = L.dark ? L.card.mix(Color::hex(0xFFFFFF), 0.03) : L.card;
  Color hover = L.dark ? Color::hex(0xFFFFFF, 0.08) : Color::hex(0x000000, 0.055);
  int pad = S(5);
  {
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), bg);
    p.stroke_rounded(0.5, 0.5, w() - 1, h() - 1, Sd(2), L.border.mix(bg, L.dark ? 0.1 : 0.0), 1);
    cairo_t *cr = p.cr();
    // Quick actions.
    if (quick_ && !quick_->empty()) {
      int n = int(quick_->size());
      double bw = (w() - 2 * S(6)) / double(n);
      for (int i = 0; i < n; ++i) {
        const QuickAction &q = (*quick_)[size_t(i)];
        double bx = S(6) + i * bw;
        if (hover_ == -2 - i && q.enabled) p.fill_rounded(bx + S(2), S(5), bw - S(4), quick_h_ - S(10), Sd(5), hover);
        Color ic = q.enabled ? L.text : L.dim.alpha(0.45);
        if (q.icon >= 0) icon::draw(p, icon::Kind(q.icon), bx + (bw - S(20)) / 2, S(12), S(20), ic);
        if (i > 0) p.line(bx + 0.5, S(14), bx + 0.5, quick_h_ - S(14), L.border.alpha(0.6), 1);
      }
      p.line(0, quick_h_ + 0.5, w(), quick_h_ + 0.5, L.border.alpha(0.8), 1);
    }
    cairo_save(cr);
    cairo_rectangle(cr, 0, quick_h_ + 1, w(), h() - quick_h_ - 1);
    cairo_clip(cr);
    for (size_t i = 0; i < rows_.size(); ++i) {
      const MenuItem &it = (*items_)[i];
      double ry = rows_[i].y - scroll_, rh = rows_[i].h;
      if (ry + rh < 0 || ry > h()) continue;
      if (it.separator) {
        p.line(0, ry + rh / 2 + 0.5, w(), ry + rh / 2 + 0.5, L.border.alpha(0.8), 1);
        continue;
      }
      bool hot = (int(i) == hover_ || int(i) == open_sub_) && it.enabled;
      if (hot) {
        p.fill_rounded(pad, ry + S(2), w() - 2 * pad, rh - S(4), Sd(5), hover);
        if (int(i) == hover_) p.fill_rounded(pad, ry + rh / 2 - S(8), Sd(3), S(16), Sd(1.5), L.accent);
      }
      Color tc = it.enabled ? L.text : L.dim.alpha(0.55);
      double iy = ry + (std::min<double>(rh, S(34)) - S(18)) / 2;
      if (it.icon >= 0) {
        icon::draw(p, icon::Kind(it.icon), pad + S(9), iy, S(18), tc);
      } else if (it.checked && it.radio) {
        p.circle(pad + S(18), iy + S(9), Sd(3.5), it.enabled ? L.accent_text : tc);
      } else if (it.checked) {
        icon::draw(p, icon::Check, pad + S(9), iy, S(18), it.enabled ? L.accent_text : tc);
      }
      if (it.icon >= 0 && it.checked) icon::draw(p, icon::Check, w() - pad - S(28), iy + S(2), S(14), L.accent_text);
      if (!it.sub.empty()) icon::draw(p, icon::ChevronRight, w() - pad - S(22), iy + S(2), S(14), tc);
    }
    cairo_restore(cr);
  }
  // Labels.
  if (quick_ && !quick_->empty()) {
    int n = int(quick_->size());
    double bw = (w() - 2 * S(6)) / double(n);
    text_font(L.theme.font_size - S(1));
    for (int i = 0; i < n; ++i) {
      const QuickAction &q = (*quick_)[size_t(i)];
      draw_text(q.label, int(S(6) + i * bw), S(36), int(bw), S(22), q.enabled ? L.text : L.dim.alpha(0.5), 1);
    }
  }
  fl_push_clip(0, quick_h_ + 1, w(), h() - quick_h_ - 1);
  for (size_t i = 0; i < rows_.size(); ++i) {
    const MenuItem &it = (*items_)[i];
    if (it.separator) continue;
    int ry = rows_[i].y - scroll_, rh = rows_[i].h;
    if (ry + rh < 0 || ry > h()) continue;
    Color tc = it.enabled ? L.text : L.dim.alpha(0.6);
    int lx = pad + icon_col_;
    int lh = std::min(rh, S(34));
    text_font(L.theme.font_size);
    int right = w() - pad - (chevrons_ ? S(28) : S(10)) - (short_w_ ? short_w_ + S(16) : 0);
    draw_text(it.label, lx, ry, right - lx, lh, tc);
    if (!it.shortcut.empty()) {
      text_font(L.theme.font_size - S(1));
      draw_text(it.shortcut, w() - pad - (chevrons_ ? S(28) : S(12)) - short_w_, ry, short_w_, lh,
                L.dim.alpha(it.enabled ? 1.0 : 0.6), 2);
    }
    if (!it.hint.empty()) {
      text_font(L.theme.font_size - S(2));
      draw_text(it.hint, lx, ry + S(24), w() - lx - pad - S(8), S(18), L.dim.alpha(0.85));
    }
  }
  fl_pop_clip();
}

}  // namespace

bool popup_menu(const MenuSpec &spec, int x, int y, int anchor_h) {
  if (spec.items.empty() && spec.quick.empty()) return false;
  Fl_Widget *focus = Fl::focus();
  bool ran;
  {
    Controller c(spec);
    c.open(x, y, anchor_h);
    ran = c.run();
  }
  if (focus && !ran && focus->visible_r()) focus->take_focus();
  return ran;
}

}  // namespace niny
