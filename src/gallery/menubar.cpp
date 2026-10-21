// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "menubar.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>

#include "buffered_window.h"
#include "i18n.h"
#include "theme.h"
#include "widgets.h"

using namespace sde;

namespace gallery {

namespace {

std::string strip_mnemonic(const char *label) {
  std::string out;
  for (const char *p = label; p && *p; ++p) {
    if (*p == '&' && p[1] != '&') continue;
    out += *p;
  }
  return out;
}

}  // namespace

MenuBar::MenuBar(int x, int y, int w, int h) : Fl_Menu_Bar(x, y, w, h) {
  box(FL_FLAT_BOX);
  down_box(FL_FLAT_BOX);
  apply_theme();
}

void MenuBar::apply_theme() {
  const Theme &t = active_theme();
  color(t.surface.fl());
  selection_color(t.accent.fl());
  textcolor(t.text.fl());
  textsize(t.font_size);
  textfont(FL_HELVETICA);
  redraw();
}

int MenuBar::title_at(int mx) const {
  if (!menu() || !menu()->text) return -1;
  int X = x() + 6, i = 0;
  for (const Fl_Menu_Item *m = menu()->first(); m && m->text; m = m->next(), ++i) {
    int W = m->measure(nullptr, this) + 16;
    if (mx >= X && mx < X + W) return i;
    X += W;
  }
  return -1;
}

void MenuBar::draw() {
  const Theme &t = active_theme();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), t.surface);
    p.line(x(), y() + h() - 0.5, x() + w(), y() + h() - 0.5, t.border.alpha(0.6));
    if (menu() && menu()->text) {
      int X = x() + 6, i = 0;
      for (const Fl_Menu_Item *m = menu()->first(); m && m->text; m = m->next(), ++i) {
        int W = m->measure(nullptr, this) + 16;
        if (i == hover_ && active_r())
          p.fill_rounded(X + 1, y() + S(3), W - 2, h() - S(6), Sd(6), t.hover);
        X += W;
      }
    }
  }
  if (!menu() || !menu()->text) return;
  int X = x() + 6;
  for (const Fl_Menu_Item *m = menu()->first(); m && m->text; m = m->next()) {
    int W = m->measure(nullptr, this) + 16;
    text_font(textsize());
    draw_text(strip_mnemonic(m->label()), X, y(), W, h(), m->active() ? t.text : t.text_dim, 1);
    X += W;
  }
}

int MenuBar::handle(int event) {
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int hv = title_at(Fl::event_x());
      if (hv != hover_) {
        hover_ = hv;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
  }
  int r = Fl_Menu_Bar::handle(event);
  if (event == FL_PUSH) {
    hover_ = -1;
    redraw();
  }
  return r;
}

// ------------------------------------------------------------------ dialogs

namespace {

class InfoWindow : public BufferedWindow {
 public:
  using DrawFn = std::function<void(InfoWindow *)>;
  InfoWindow(int w, int h, const char *title, DrawFn fn) : BufferedWindow(w, h, title), fn_(std::move(fn)) {}
  void draw() override {
    fn_(this);
    draw_children();
  }
  int handle(int event) override {
    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) &&
        (Fl::event_key() == FL_Escape || Fl::event_key() == FL_Enter)) {
      hide();
      return 1;
    }
    return BufferedWindow::handle(event);
  }

 private:
  DrawFn fn_;
};

void run_modal(Fl_Window *win, Fl_Window *parent) {
  if (!parent) parent = Fl::first_window();
  if (parent && parent != win)
    win->position(parent->x() + (parent->w() - win->w()) / 2, parent->y() + (parent->h() - win->h()) / 3);
  win->set_modal();
  win->show();
  while (win->shown()) Fl::wait();
}

}  // namespace

void show_about(const AboutInfo &info) {
  const Theme &t = active_theme();
  int W = S(420), H = S(420) + S(20) * int(info.credits.size());
  std::string title = format(_("About %s"), info.name.c_str());
  Fl_Window *parent = Fl::first_window();
  InfoWindow *win = new InfoWindow(W, H, title.c_str(), [info](InfoWindow *w) {
    const Theme &t = active_theme();
    {
      Painter p(w->w(), w->h());
      p.fill_rect(0, 0, w->w(), w->h(), t.surface);
      if (t.glass) p.glass_sheen(0, 0, w->w(), S(150), 0, t.dark);
      double ls = S(84);
      if (info.logo) info.logo(p, (w->w() - ls) / 2, S(28), ls);
    }
    int y = S(124);
    text_font(t.font_size + S(7), true);
    draw_text(info.name, 0, y, w->w(), S(34), t.text, 1);
    y += S(34);
    text_font(t.font_size - S(1));
    draw_text(format(_("Version %s"), info.version.c_str()) + "  ·  " + info.program, 0, y, w->w(), S(20),
              t.text_dim, 1);
    y += S(32);
    text_font(t.font_size);
    // Simple centred word wrap for the description.
    std::vector<std::string> lines;
    std::string line;
    for (const auto &word : [&] {
           std::vector<std::string> words;
           std::string cur;
           for (char c : info.description) {
             if (c == ' ') {
               if (!cur.empty()) words.push_back(cur);
               cur.clear();
             } else {
               cur += c;
             }
           }
           if (!cur.empty()) words.push_back(cur);
           return words;
         }()) {
      std::string cand = line.empty() ? word : line + " " + word;
      if (text_width(cand) > w->w() - S(64) && !line.empty()) {
        lines.push_back(line);
        line = word;
      } else {
        line = cand;
      }
    }
    if (!line.empty()) lines.push_back(line);
    for (const auto &l : lines) {
      draw_text(l, S(32), y, w->w() - S(64), S(22), t.text, 1);
      y += S(22);
    }
    y += S(16);
    text_font(t.font_size - S(1), true);
    draw_text(_("Written from scratch by the DebION Dev-Team"), 0, y, w->w(), S(20), t.text, 1);
    y += S(20);
    text_font(t.font_size - S(1));
    draw_text("Copyright © 2026 DebION Dev-Team", 0, y, w->w(), S(20), t.text_dim, 1);
    y += S(20);
    draw_text(_("License: GNU GPL version 3 or later"), 0, y, w->w(), S(20), t.text_dim, 1);
    y += S(20);
    for (const auto &c : info.credits) {
      draw_text(c, S(20), y, w->w() - S(40), S(20), t.text_dim, 1);
      y += S(20);
    }
  });
  win->begin();
  auto *close = new FlatButton((W - S(110)) / 2, H - S(52), S(110), S(34), _("Close"), true);
  close->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  win->end();
  win->color(t.surface.fl());
  run_modal(win, parent);
  Fl::delete_widget(win);
}

void show_shortcuts(const std::string &title,
                    const std::vector<std::pair<std::string, std::string>> &rows) {
  const Theme &t = active_theme();
  // Two columns of rows when the list is long.
  size_t half = (rows.size() + 1) / 2;
  bool two = rows.size() > 14;
  if (two) {
    // Do not split a section header from its first row.
    while (half > 0 && half < rows.size() && rows[half - 1].first.empty()) --half;
  }
  size_t per_col = two ? std::max(half, rows.size() - half) : rows.size();
  int W = two ? S(820) : S(440), H = S(84) + S(26) * int(per_col) + S(60);
  Fl_Window *parent = Fl::first_window();
  if (parent) H = std::min(H, std::max(S(300), parent->h()));
  InfoWindow *win = new InfoWindow(W, H, title.c_str(), [rows, two, half, title](InfoWindow *w) {
    const Theme &t = active_theme();
    {
      Painter p(w->w(), w->h());
      p.fill_rect(0, 0, w->w(), w->h(), t.surface);
    }
    text_font(t.font_size + S(4), true);
    draw_text(title, S(24), S(18), w->w() - S(48), S(30), t.text);
    int colw = two ? (w->w() - S(48)) / 2 : w->w() - S(48);
    for (size_t i = 0; i < rows.size(); ++i) {
      bool second = two && i >= half;
      int col_x = S(24) + (second ? colw + S(0) : 0);
      int y = S(64) + S(26) * int(second ? i - half : i);
      if (y + S(26) > w->h() - S(56)) continue;
      const auto &r = rows[i];
      if (r.first.empty()) {
        text_font(t.font_size - S(1), true);
        draw_text(r.second, col_x, y + S(4), colw - S(12), S(22), t.accent_text);
        continue;
      }
      text_font(t.font_size);
      draw_text(r.second, col_x, y, colw - S(170), S(26), t.text);
      text_font(t.font_size - S(1), true);
      draw_text(r.first, col_x + colw - S(170), y, S(158), S(26), t.text_dim, 2);
    }
  });
  win->begin();
  auto *close = new FlatButton(W - S(24) - S(100), H - S(48), S(100), S(34), _("Close"), true);
  close->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  win->end();
  win->color(t.surface.fl());
  run_modal(win, parent);
  Fl::delete_widget(win);
}

}  // namespace gallery
