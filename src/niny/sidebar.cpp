// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny side panel: the open documents, and a browser of the current folder.
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <dirent.h>
#include <sys/stat.h>

#include <strings.h>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "niny.h"
#include "util.h"

using namespace sde;

namespace niny {

Sidebar::Sidebar(App *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

int Sidebar::rows_top() const { return y() + S(54) + (mode_ == 1 ? S(36) : 0); }
int Sidebar::row_h() const { return mode_ == 0 ? S(44) : S(30); }
int Sidebar::row_count() const { return mode_ == 0 ? int(app_->docs().size()) : int(entries_.size()); }

int Sidebar::row_at(int my) const {
  if (my < rows_top()) return -1;
  int r = int((my - rows_top() + scroll_) / row_h());
  return r >= 0 && r < row_count() ? r : -1;
}

void Sidebar::set_mode(int m) {
  mode_ = m;
  scroll_ = 0;
  if (m == 1 && entries_.empty()) refresh();
  redraw();
}

void Sidebar::set_folder(const std::string &dir) {
  if (dir.empty() || dir == folder_) return;
  folder_ = dir;
  scroll_ = 0;
  refresh();
}

void Sidebar::refresh() {
  entries_.clear();
  if (folder_.empty()) folder_ = home_dir();
  DIR *d = opendir(folder_.c_str());
  if (d) {
    while (dirent *e = readdir(d)) {
      std::string n = e->d_name;
      if (n.empty() || n[0] == '.') continue;
      Entry en;
      en.name = n;
      en.path = folder_ == "/" ? "/" + n : folder_ + "/" + n;
      struct stat st;
      en.dir = stat(en.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      entries_.push_back(en);
    }
    closedir(d);
  }
  std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
    if (a.dir != b.dir) return a.dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  redraw();
}

void Sidebar::draw() {
  const Look &L = look();
  const auto &docs = app_->docs();
  int rh = row_h(), top = rows_top();
  double seg_x = x() + S(10), seg_y = y() + S(12), seg_w = w() - S(20), seg_h = S(32);
  int first = int(scroll_ / rh);
  {
    Painter p(window()->w(), window()->h());
    cairo_save(p.cr());
    cairo_rectangle(p.cr(), x(), y(), w(), h());
    cairo_clip(p.cr());
    if (L.glass) p.fill_gradient(x(), y(), w(), h(), 0, L.sidebar, L.sidebar.mix(L.accent, L.dark ? 0.05 : 0.06));
    else p.fill_rect(x(), y(), w(), h(), L.sidebar);
    p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), L.border, 1);
    // Segmented control.
    double sr = L.liquid ? seg_h / 2 : Sd(8);
    p.fill_rounded(seg_x, seg_y, seg_w, seg_h, sr, L.field);
    double half = seg_w / 2;
    p.shadow(seg_x + S(3) + mode_ * half, seg_y + S(3), half - S(6), seg_h - S(6), sr, L.shadow.alpha(0.08), 3);
    p.fill_rounded(seg_x + S(3) + mode_ * half, seg_y + S(3), half - S(6), seg_h - S(6), sr - S(2), L.card);
    if (hover_seg_ >= 0 && hover_seg_ != mode_)
      p.fill_rounded(seg_x + S(3) + hover_seg_ * half, seg_y + S(3), half - S(6), seg_h - S(6), sr - S(2), L.hover);
    icon::draw(p, icon::Documents, seg_x + S(12), seg_y + (seg_h - S(16)) / 2, S(16), mode_ == 0 ? L.accent_text : L.dim);
    icon::draw(p, icon::Folder, seg_x + half + S(12), seg_y + (seg_h - S(16)) / 2, S(16), mode_ == 1 ? L.accent_text : L.dim);
    if (mode_ == 1) {
      double hy = y() + S(52);
      if (hover_up_) p.fill_rounded(x() + S(8), hy, S(28), S(28), Sd(7), L.hover);
      icon::draw(p, icon::Up, x() + S(13), hy + S(5), S(18), folder_ == "/" ? L.dim.alpha(0.4) : L.dim);
    }
    cairo_rectangle(p.cr(), x(), top, w(), h() - (top - y()));
    cairo_clip(p.cr());
    for (int i = first; i < row_count(); ++i) {
      double ry = top + i * rh - scroll_;
      if (ry > y() + h()) break;
      double rx = x() + S(8), rw = w() - S(16);
      bool hot = i == hover_;
      if (mode_ == 0) {
        bool act = i == app_->active_index();
        if (act) p.fill_rounded(rx, ry + S(2), rw, rh - S(4), Sd(L.radius), L.sidebar_active);
        else if (hot) p.fill_rounded(rx, ry + S(2), rw, rh - S(4), Sd(L.radius), L.hover);
        draw_badge(p, docs[size_t(i)]->lang, rx + S(8), ry + (rh - S(24)) / 2, S(24));
        double cs = S(20), ccx = rx + rw - S(26), ccy = ry + (rh - cs) / 2;
        if (hot && hover_close_) p.fill_rounded(ccx, ccy, cs, cs, cs / 2, L.hover.mix(L.text, 0.08));
        if (hot) icon::draw(p, icon::Close, ccx + S(3), ccy + S(3), cs - S(6), L.dim);
        else if (docs[size_t(i)]->modified()) icon::draw(p, icon::Dot, ccx + S(2), ccy + S(2), cs - S(4), L.accent);
      } else {
        if (hot) p.fill_rounded(rx, ry + S(1), rw, rh - S(2), Sd(L.radius - 2), L.hover);
        const Entry &e = entries_[size_t(i)];
        bool open = !e.dir && app_->find_document(e.path) >= 0;
        icon::draw(p, e.dir ? icon::Folder : icon::File, rx + S(8), ry + (rh - S(16)) / 2, S(16),
                   e.dir ? L.accent_text : open ? L.text : L.dim);
      }
    }
    cairo_restore(p.cr());
  }
  // Text.
  text_font(L.theme.font_size - S(1), true);
  draw_text(_("Open"), int(seg_x + S(32)), int(seg_y), int(seg_w / 2 - S(36)), int(seg_h), mode_ == 0 ? L.text : L.dim);
  draw_text(_("Folder"), int(seg_x + seg_w / 2 + S(32)), int(seg_y), int(seg_w / 2 - S(36)), int(seg_h),
            mode_ == 1 ? L.text : L.dim);
  if (mode_ == 1) {
    text_font(L.theme.font_size - S(1), true);
    std::string name = folder_ == "/" ? "/" : basename_of(folder_);
    if (folder_ == home_dir()) name = _("Home");
    draw_text(name, x() + S(42), y() + S(52), w() - S(52), S(28), L.text);
  }
  fl_push_clip(x(), top, w(), h() - (top - y()));
  for (int i = first; i < row_count(); ++i) {
    int ry = int(top + i * rh - scroll_);
    if (ry > y() + h()) break;
    int rx = x() + S(8), rw = w() - S(16);
    if (mode_ == 0) {
      const Document *d = docs[size_t(i)].get();
      draw_badge_text(d->lang, rx + S(8), ry + (rh - S(24)) / 2.0, S(24));
      bool act = i == app_->active_index();
      text_font(L.theme.font_size, act);
      draw_text(d->display_name(), rx + S(40), ry + S(4), rw - S(72), S(20), L.text);
      text_font(L.theme.font_size - S(2));
      std::string sub = d->path.empty() ? std::string(_("Not saved")) : home_relative(d->folder());
      draw_text(sub, rx + S(40), ry + S(22), rw - S(72), S(18), L.dim);
    } else {
      const Entry &e = entries_[size_t(i)];
      bool open = !e.dir && app_->find_document(e.path) >= 0;
      text_font(L.theme.font_size - S(1), open);
      draw_text(e.name, rx + S(32), ry, rw - S(40), rh, L.text);
    }
  }
  if (mode_ == 0 && docs.empty()) {
    text_font(L.theme.font_size - S(1));
    draw_text(_("No open documents"), x() + S(16), top + S(8), w() - S(32), S(24), L.dim);
  }
  if (mode_ == 1 && entries_.empty()) {
    text_font(L.theme.font_size - S(1));
    draw_text(_("This folder is empty."), x() + S(16), top + S(8), w() - S(32), S(24), L.dim);
  }
  fl_pop_clip();
}

void Sidebar::activate_row(int row, bool middle) {
  if (mode_ == 0) {
    if (middle) app_->close_document(row);
    else app_->activate(row);
    return;
  }
  const Entry e = entries_[size_t(row)];
  if (e.dir) set_folder(e.path);
  else app_->open_file(e.path);
}

int Sidebar::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  double seg_x = x() + S(10), seg_y = y() + S(12), seg_w = w() - S(20), seg_h = S(32);
  bool in_seg = mx >= seg_x && mx < seg_x + seg_w && my >= seg_y && my < seg_y + seg_h;
  bool edge = mx >= x() + w() - S(5);
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      window()->cursor(edge ? FL_CURSOR_WE : FL_CURSOR_DEFAULT);
      int h = edge ? -1 : row_at(my);
      int hs = in_seg ? int((mx - seg_x) / (seg_w / 2)) : -1;
      bool up = mode_ == 1 && mx >= x() + S(8) && mx < x() + S(36) && my >= y() + S(52) && my < y() + S(80);
      bool hc = mode_ == 0 && h >= 0 && mx >= x() + w() - S(42) && mx < x() + w() - S(14);
      if (h != hover_ || hs != hover_seg_ || up != hover_up_ || hc != hover_close_) {
        hover_ = h;
        hover_seg_ = hs;
        hover_up_ = up;
        hover_close_ = hc;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      window()->cursor(FL_CURSOR_DEFAULT);
      hover_ = hover_seg_ = -1;
      hover_up_ = hover_close_ = false;
      redraw();
      return 1;
    case FL_PUSH:
      if (edge) {
        resizing_ = true;
        return 1;
      }
      if (in_seg) {
        set_mode(int((mx - seg_x) / (seg_w / 2)) ? 1 : 0);
        return 1;
      }
      if (hover_up_ && folder_ != "/") {
        size_t s = folder_.rfind('/');
        set_folder(s == 0 || s == std::string::npos ? "/" : folder_.substr(0, s));
        return 1;
      }
      {
        int r = row_at(my);
        if (r >= 0) {
          if (mode_ == 0 && hover_close_) app_->close_document(r);
          else activate_row(r, Fl::event_button() == FL_MIDDLE_MOUSE);
        }
      }
      return 1;
    case FL_DRAG:
      if (resizing_) {
        int nw = std::max(S(150), std::min(mx - x(), window()->w() / 2));
        app_->settings().sidebar_width = int(std::lround(nw / ui_scale()));
        app_->layout();
        window()->redraw();
      }
      return 1;
    case FL_RELEASE:
      if (resizing_) {
        resizing_ = false;
        app_->apply_settings(true);
      }
      return 1;
    case FL_MOUSEWHEEL: {
      double content = row_count() * double(row_h());
      double view = h() - (rows_top() - y());
      scroll_ = std::max(0.0, std::min(scroll_ + Fl::event_dy() * row_h() * 3.0, std::max(0.0, content - view)));
      redraw();
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}

}  // namespace niny
