// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Plugins window shared by Photo and Gallery: built-in plugins and
// action plugins with on/off switches.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "buffered_window.h"
#include "i18n.h"
#include "icons.h"
#include "line_icons.h"
#include "menubar.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

using namespace sde;

namespace gallery {

namespace {

// Greedy word wrap; the last line allowed takes the rest, shortened.
std::vector<std::string> wrap_text(const std::string &text, int width, size_t max_lines) {
  std::vector<std::string> words = split(text, ' '), lines;
  std::string line;
  for (size_t i = 0; i < words.size(); ++i) {
    std::string next = line.empty() ? words[i] : line + " " + words[i];
    if (!line.empty() && text_width(next) > width) {
      if (lines.size() + 1 == max_lines) {
        std::string rest = line;
        for (size_t k = i; k < words.size(); ++k) rest += " " + words[k];
        lines.push_back(ellipsize(rest, width));
        return lines;
      }
      lines.push_back(ellipsize(line, width));
      line = words[i];
    } else {
      line = next;
    }
  }
  if (!line.empty()) lines.push_back(ellipsize(line, width));
  return lines;
}

class PluginsWindow : public BufferedWindow {
 public:
  PluginsWindow(int w, int h, const std::string &title, const std::string &intro, std::vector<PluginRow> &rows,
                const std::string &folder)
      : BufferedWindow(w, h), title_(title), intro_(intro), rows_(rows), folder_(folder) {
    copy_label(title.c_str());
    begin();
    folder_btn_ = new FlatButton(S(24), h - S(52), S(200), S(34), _("Open Plugin Folder"));
    folder_btn_->callback([](Fl_Widget *b, void *) {
      auto *self = static_cast<PluginsWindow *>(b->window());
      make_dirs(self->folder_);
      if (!find_program("liria").empty() && spawn_detached({"liria", "--no-desktop", self->folder_})) return;
      open_uri(self->folder_);
    });
    auto *close = new FlatButton(w - S(24) - S(110), h - S(52), S(110), S(34), _("Close"), true);
    close->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
    end();
    text_font(active_theme().font_size);
    folder_btn_->size(text_width(_("Open Plugin Folder")) + S(36), S(34));
  }

  bool changed = false;

  void draw() override {
    const Theme &t = active_theme();
    layout_rows();
    int lx = S(24), lw = w() - S(48);
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      cairo_t *cr = p.cr();
      cairo_save(cr);
      cairo_rectangle(cr, 0, list_top(), w(), list_bottom() - list_top());
      cairo_clip(cr);
      for (size_t i = 0; i < rows_.size(); ++i) {
        const Item &it = items_[i];
        if (it.y + it.h < list_top() || it.y > list_bottom()) continue;
        const PluginRow &r = rows_[i];
        bool hot = int(i) == hover_, focus = int(i) == focus_ && Fl::focus() == this;
        if (hot) p.fill_rounded(lx - S(8), it.y, lw + S(16), it.h, Sd(10), t.hover);
        if (focus) focus_ring(p, lx - S(8), it.y, lw + S(16), it.h, Sd(10));
        double is = S(28), iy = it.y + S(12);
        Color ic = r.usable ? t.accent_text : t.text_dim;
        if (r.glyph >= 0) {
          p.fill_rounded(lx, iy - S(4), is + S(8), is + S(8), Sd(9), t.accent.alpha(t.dark ? 0.22 : 0.12));
          icon::draw(p, icon::Kind(r.glyph), lx + S(6), iy + S(2), is - S(4), ic);
        } else {
          std::string name = r.icon.empty() ? "application-x-executable" : r.icon;
          if (cairo_surface_t *img = IconLoader::get({name, "applications-other", "application-x-executable"}, int(is)))
            p.image(img, lx + S(4), iy, is, r.usable ? 1.0 : 0.5);
        }
        // The switch.
        double sw = S(42), sh = S(24), sx = lx + lw - sw, sy = it.y + S(14);
        Color track = !r.usable ? t.border.alpha(0.5) : r.on ? t.accent : t.border;
        p.fill_rounded(sx, sy, sw, sh, sh / 2, track);
        double knob = sh - S(6), kx = r.on ? sx + sw - knob - S(3) : sx + S(3);
        p.circle(kx + knob / 2, sy + sh / 2, knob / 2, Color::hex(0xFFFFFF, r.usable ? 1.0 : 0.7));
        if (r.settings && r.on) {
          double bx = sx - S(96), by = sy - S(4);
          p.fill_rounded(bx, by, S(84), S(32), Sd(8), int(i) == hover_settings_ ? t.hover.mix(t.border, 0.3) : t.surface_alt);
        }
      }
      // Section titles.
      cairo_restore(cr);
      double room = list_bottom() - list_top();
      if (content_h_ > room + 1) {
        double track = room - S(8);
        double bar = std::max(double(S(24)), track * room / content_h_);
        double by = list_top() + S(4) + (track - bar) * scroll_ / std::max(1.0, content_h_ - room);
        p.fill_rounded(w() - S(9), by, S(4), bar, S(2), t.text_dim.alpha(0.45));
      }
      p.line(S(24), list_top() - 0.5, w() - S(24), list_top() - 0.5, t.border.alpha(0.6));
      p.line(S(24), list_bottom() + 0.5, w() - S(24), list_bottom() + 0.5, t.border.alpha(0.6));
    }
    text_font(t.font_size + S(4), true);
    draw_text(title_, S(24), S(16), w() - S(48), S(32), t.text);
    text_font(t.font_size);
    int iy = S(52);
    for (const auto &line : wrap_text(intro_, w() - S(48), 2)) {
      draw_text(line, S(24), iy, w() - S(48), S(20), t.text_dim);
      iy += S(20);
    }
    fl_push_clip(0, list_top(), w(), list_bottom() - list_top());
    for (const auto &s : sections_) {
      if (s.y + S(24) < list_top() || s.y > list_bottom()) continue;
      text_font(t.font_size - S(1), true);
      draw_text(s.text, lx, int(s.y), lw, S(22), t.accent_text);
      if (!s.note.empty()) {
        text_font(t.font_size - S(1));
        int ny = int(s.y + S(26));
        for (const auto &line : wrap_text(s.note, lw, 3)) {
          draw_text(line, lx, ny, lw, S(20), t.text_dim);
          ny += S(20);
        }
      }
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
      const Item &it = items_[i];
      if (it.y + it.h < list_top() || it.y > list_bottom()) continue;
      const PluginRow &r = rows_[i];
      int tx = lx + S(48), tw = lw - S(48) - S(56) - (r.settings && r.on ? S(96) : 0);
      text_font(t.font_size, true);
      draw_text(r.name, tx, int(it.y + S(8)), tw, S(22), r.usable ? t.text : t.text_dim);
      text_font(t.font_size - S(1));
      int ly = int(it.y + S(30));
      for (const auto &line : wrap_text(r.description, tw, 2)) {
        draw_text(line, tx, ly, tw, S(18), t.text_dim);
        ly += S(18);
      }
      if (!r.detail.empty()) {
        draw_text(r.detail, tx, ly, tw, S(18), r.usable ? t.text_dim.alpha(0.8) : t.error);
      }
      if (r.settings && r.on) {
        double sx = lx + lw - S(42) - S(96);
        draw_text(_("Settings…"), int(sx), int(it.y + S(10)), S(84), S(32), t.text, 1);
      }
    }
    fl_pop_clip();
    draw_children();
  }

  int handle(int event) override {
    int mx = Fl::event_x(), my = Fl::event_y();
    switch (event) {
      case FL_KEYBOARD:
      case FL_SHORTCUT: {
        int key = Fl::event_key();
        if (key == FL_Escape || key == FL_Enter || key == FL_KP_Enter) {
          hide();
          return 1;
        }
        if (key == FL_Down || key == FL_Up) {
          int n = int(rows_.size());
          if (n) focus_ = focus_ < 0 ? 0 : std::max(0, std::min(n - 1, focus_ + (key == FL_Down ? 1 : -1)));
          ensure_visible(focus_);
          Fl::focus(this);
          redraw();
          return 1;
        }
        if (key == ' ' && focus_ >= 0 && Fl::focus() == this) {
          toggle(focus_);
          return 1;
        }
        break;
      }
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_MOVE:
      case FL_ENTER: {
        int r = row_at(mx, my), s = settings_at(mx, my);
        if (r != hover_ || s != hover_settings_) {
          hover_ = r;
          hover_settings_ = s;
          redraw();
        }
        break;
      }
      case FL_LEAVE:
        hover_ = hover_settings_ = -1;
        redraw();
        break;
      case FL_PUSH: {
        int s = settings_at(mx, my);
        if (s >= 0) {
          auto fn = rows_[size_t(s)].settings;
          fn();
          redraw();
          return 1;
        }
        int r = row_at(mx, my);
        if (r >= 0) {
          focus_ = r;
          Fl::focus(this);
          toggle(r);
          return 1;
        }
        break;
      }
      case FL_MOUSEWHEEL: {
        double room = list_bottom() - list_top();
        scroll_ = std::max(0.0, std::min(std::max(0.0, content_h_ - room), scroll_ + Fl::event_dy() * S(48)));
        redraw();
        return 1;
      }
    }
    return BufferedWindow::handle(event);
  }

 private:
  struct Item {
    double y = 0, h = 0;
  };
  struct Section {
    std::string text, note;
    double y = 0;
  };
  int list_top() const { return S(100); }
  int list_bottom() const { return h() - S(68); }

  void layout_rows() {
    items_.assign(rows_.size(), Item());
    sections_.clear();
    double y = list_top() + S(10) - scroll_;
    bool actions = false, any_action = false;
    for (const auto &r : rows_) any_action |= r.action;
    sections_.push_back({_("BUILT IN"), "", y});
    y += S(30);
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (rows_[i].action && !actions) {
        actions = true;
        sections_.push_back({_("INSTALLED"), "", y + S(8)});
        y += S(38);
      }
      double h = S(64) + (rows_[i].detail.empty() ? 0 : S(18));
      items_[i] = {y, h};
      y += h + S(4);
    }
    if (!any_action) {
      sections_.push_back({_("INSTALLED"),
                           format(_("No action plugins are installed. A .plugin file in %s adds a command that "
                                    "runs a program on the photos."),
                                  folder_.c_str()),
                           y + S(8)});
      y += S(38) + S(64);
    }
    content_h_ = y + scroll_ - list_top();
  }

  int row_at(int mx, int my) const {
    if (my < list_top() || my >= list_bottom() || mx < S(16) || mx > w() - S(16)) return -1;
    for (size_t i = 0; i < items_.size(); ++i)
      if (my >= items_[i].y && my < items_[i].y + items_[i].h) return int(i);
    return -1;
  }

  int settings_at(int mx, int my) const {
    int r = row_at(mx, my);
    if (r < 0 || !rows_[size_t(r)].settings || !rows_[size_t(r)].on) return -1;
    double sx = S(24) + (w() - S(48)) - S(42) - S(96), sy = items_[size_t(r)].y + S(10);
    return mx >= sx && mx < sx + S(84) && my >= sy && my < sy + S(32) ? r : -1;
  }

  void toggle(int i) {
    PluginRow &r = rows_[size_t(i)];
    if (!r.usable && !r.on) return;
    r.on = !r.on;
    changed = true;
    redraw();
  }

  void ensure_visible(int i) {
    if (i < 0) return;
    layout_rows();
    const Item &it = items_[size_t(i)];
    double room = list_bottom() - list_top();
    if (it.y < list_top()) scroll_ -= list_top() - it.y + S(8);
    else if (it.y + it.h > list_bottom()) scroll_ += it.y + it.h - list_bottom() + S(8);
    scroll_ = std::max(0.0, std::min(std::max(0.0, content_h_ - room), scroll_));
  }

  std::string title_, intro_;
  std::vector<PluginRow> &rows_;
  std::string folder_;
  std::vector<Item> items_;
  std::vector<Section> sections_;
  double scroll_ = 0, content_h_ = 0;
  int hover_ = -1, hover_settings_ = -1, focus_ = -1;
  FlatButton *folder_btn_ = nullptr;
};

}  // namespace

bool show_plugins_dialog(const std::string &title, const std::string &intro, std::vector<PluginRow> &rows,
                         const std::string &folder) {
  const Theme &t = active_theme();
  Fl_Window *parent = Fl::first_window();
  int W = S(600), H = S(680);
  if (parent) {
    W = std::min(W, std::max(S(420), parent->w() - S(40)));
    H = std::min(H, std::max(S(360), parent->h() - S(40)));
  }
  auto *win = new PluginsWindow(W, H, title, intro, rows, folder);
  win->color(t.surface.fl());
  if (parent && parent != win)
    win->position(parent->x() + (parent->w() - W) / 2, parent->y() + (parent->h() - H) / 3);
  win->set_modal();
  win->show();
  Fl::focus(win);
  while (win->shown()) Fl::wait();
  bool changed = win->changed;
  Fl::delete_widget(win);
  return changed;
}

}  // namespace gallery
