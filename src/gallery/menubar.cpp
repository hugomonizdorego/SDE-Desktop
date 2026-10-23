// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "menubar.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>

#include "about.h"
#include "buffered_window.h"
#include "i18n.h"
#include "theme.h"
#include "util.h"
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
  // The common SDE About window (src/common/about.h).
  sde::AppInfo app;
  app.name = info.name;
  app.program = info.program;
  app.version = info.version;
  app.icon = info.icon.empty() ? info.program : info.icon;
  app.icon_fallbacks = info.icon_fallbacks;
  app.description = info.description;
  app.logo = info.logo;
  app.credits = info.credits;
  sde::show_about(app);
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

int ask(const std::string &title, const std::string &message, const std::vector<std::string> &buttons,
        int *quality) {
  const Theme &t = active_theme();
  int W = S(460);
  // Wrap the message (paths break at slashes).
  text_font(t.font_size);
  std::vector<std::string> lines;
  for (const auto &para : split(message, '\n', true)) {
    std::string line;
    size_t i = 0;
    while (i < para.size()) {
      size_t j = para.find_first_of(" /", i);
      std::string word = para.substr(i, j == std::string::npos ? std::string::npos : j - i + 1);
      i = j == std::string::npos ? para.size() : j + 1;
      if (!line.empty() && text_width(line + word) > W - S(48)) {
        lines.push_back(line);
        line.clear();
      }
      line += word;
    }
    lines.push_back(line);
  }
  int H = S(126) + S(22) * int(lines.size()) + (quality ? S(64) : 0);
  struct Win : BufferedWindow {
    std::vector<std::string> lines;
    sde::Slider *slider = nullptr;
    int result = 0;
    Win(int w, int h, const char *l) : BufferedWindow(w, h, l) {}
    void draw() override {
      const Theme &t = active_theme();
      {
        Painter p(w(), h());
        p.fill_rect(0, 0, w(), h(), t.surface);
      }
      text_font(t.font_size + S(1), true);
      int y = S(22);
      draw_text(label() ? label() : "", S(24), y, w() - S(48), S(24), t.text);
      y += S(34);
      text_font(t.font_size);
      for (const auto &line : lines) {
        draw_text(line, S(24), y, w() - S(48), S(22), t.text_dim);
        y += S(22);
      }
      if (slider) {
        text_font(t.font_size - S(1));
        draw_text(_("Quality"), S(24), slider->y() - S(22), S(200), S(20), t.text);
        draw_text(format("%d", int(slider->value())), w() - S(124), slider->y() - S(22), S(100), S(20), t.text_dim, 2);
      }
      draw_children();
    }
    int handle(int e) override {
      if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
        result = 0;
        hide();
        return 1;
      }
      return BufferedWindow::handle(e);
    }
  };
  Win *win = new Win(W, H, title.c_str());
  win->lines = lines;
  win->begin();
  if (quality) {
    win->slider = new sde::Slider(S(24), H - S(110), W - S(48), S(26));
    win->slider->bounds(30, 100);
    win->slider->step(1);
    win->slider->value(*quality);
    win->slider->callback([](Fl_Widget *w, void *) { w->window()->redraw(); });
  }
  int bw = S(124), x = W - S(24) - bw * int(buttons.size()) - S(8) * int(buttons.size() - 1);
  for (size_t i = 0; i < buttons.size(); ++i) {
    auto *b = new FlatButton(x, H - S(52), bw, S(34), buttons[i], i + 1 == buttons.size());
    b->callback([](Fl_Widget *w, void *data) {
      auto *win = static_cast<Win *>(w->window());
      win->result = int(reinterpret_cast<long>(data));
      win->hide();
    }, reinterpret_cast<void *>(long(i)));
    x += bw + S(8);
  }
  win->end();
  win->color(t.surface.fl());
  if (Fl_Window *parent = Fl::first_window())
    win->position(parent->x() + (parent->w() - W) / 2, parent->y() + (parent->h() - H) / 3);
  win->set_modal();
  win->show();
  while (win->shown()) Fl::wait();
  int r = win->result;
  if (quality && win->slider) *quality = int(win->slider->value());
  Fl::delete_widget(win);
  return r;
}


}  // namespace gallery
