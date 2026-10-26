// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "forms.h"

#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "buffered_window.h"
#include "i18n.h"
#include "icons.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

using namespace sde;

namespace gallery {

namespace {

std::vector<std::string> wrap_words(const std::string &text, int width) {
  std::vector<std::string> lines;
  for (const auto &para : split(text, '\n', true)) {
    std::string line;
    for (const auto &word : split(para, ' ')) {
      std::string next = line.empty() ? word : line + " " + word;
      if (!line.empty() && text_width(next) > width) {
        lines.push_back(line);
        line = word;
      } else {
        line = next;
      }
    }
    lines.push_back(line);
  }
  return lines;
}

void style_input(Fl_Input *in, const Theme &t) {
  in->box(FL_FLAT_BOX);
  in->color(t.surface_alt.fl());
  in->textcolor(t.text.fl());
  in->selection_color(t.accent.fl());
  in->cursor_color(t.accent.fl());
  in->textsize(t.font_size);
  in->textfont(FL_HELVETICA);
}

// The work area of the screen the program's window is on.
void work_area(int &x, int &y, int &w, int &h) {
  Fl_Window *parent = Fl::first_window();
  int n = parent ? Fl::screen_num(parent->x() + parent->w() / 2, parent->y() + parent->h() / 2) : 0;
  Fl::screen_work_area(x, y, w, h, n);
}

void run_window(Fl_Window *win) {
  Fl_Window *parent = Fl::first_window();
  if (parent && parent != win) {
    int x = parent->x() + (parent->w() - win->w()) / 2, y = parent->y() + std::max(0, (parent->h() - win->h()) / 3);
    // Whole on the screen, title bar included.
    int sx, sy, sw, sh;
    work_area(sx, sy, sw, sh);
    x = std::max(sx, std::min(x, sx + sw - win->w()));
    y = std::max(sy + S(28), std::min(y, sy + sh - win->h()));
    win->position(x, y);
  }
  win->set_modal();
  win->show();
  while (win->shown()) Fl::wait();
}

}  // namespace

struct Form::Impl {
  struct Row {
    enum Kind { Choice, Slider, Text, Check, Note } kind = Note;
    std::string label;
    std::vector<std::string> options;
    int value = 0;
    std::function<std::string(double)> show;
    bool on = false, enabled = true;
    sde::Slider *slider = nullptr;
    Fl_Input *input = nullptr;
    std::string text, hint;
    double y = 0, h = 0;
    struct Chip {
      double x, y, w, h;
    };
    std::vector<Chip> chips;
  };

  class Win : public BufferedWindow {
   public:
    Win(Impl *impl, int w, int h) : BufferedWindow(w, h), impl_(impl) {}
    void draw() override { impl_->draw(); }
    // The controls of the rows are clipped to the part that scrolls; the
    // buttons are always whole.
    void paint_children(int clip_h) {
      for (int i = 0; i < children(); ++i) {
        Fl_Widget &c = *child(i);
        if (!c.visible()) continue;
        bool row = c.y() < clip_h;
        if (row) fl_push_clip(0, 0, w(), clip_h);
        draw_child(c);
        if (row) fl_pop_clip();
      }
    }
    int handle(int event) override { return impl_->handle(event) ? 1 : BufferedWindow::handle(event); }
    Impl *impl_;
  };

  Form *form = nullptr;
  std::string title, message;
  std::vector<Row> rows;
  std::string preview_label;
  std::function<std::vector<std::string>()> preview_fn;
  int preview_rows = 0;
  double preview_y = 0;
  std::function<void(Form &)> changed;
  Win *win = nullptr;
  int result = 0;
  int hover_row = -1, hover_chip = -1;
  // On a small screen the rows scroll above the buttons.
  double scroll = 0;
  int content_h = 0;

  int width() const { return S(520); }
  // Where the buttons begin (the rows are drawn above).
  int rows_bottom() const { return win ? win->h() - S(64) : 1 << 20; }
  bool scrolls() const { return win && content_h > win->h(); }

  // Places the rows; returns the height of the whole dialog.
  int layout() {
    const Theme &t = active_theme();
    int x = S(24), w = width() - S(48);
    double y = S(20) + S(30) - scroll;
    text_font(t.font_size);
    if (!message.empty()) y += S(20) * double(wrap_words(message, w).size()) + S(6);
    y += S(10);
    for (auto &r : rows) {
      r.y = y;
      switch (r.kind) {
        case Row::Choice: {
          r.chips.clear();
          double cx = x, cy = y + S(24);
          text_font(t.font_size - S(1));
          for (const auto &o : r.options) {
            double cw = text_width(o) + S(28);
            if (cx + cw > x + w && cx > x) {
              cx = x;
              cy += S(38);
            }
            r.chips.push_back({cx, cy, cw, double(S(32))});
            cx += cw + S(8);
          }
          y = cy + S(32) + S(14);
          break;
        }
        case Row::Slider:
          if (r.slider) r.slider->resize(x, int(y + S(24)), w, S(26));
          y += S(24) + S(26) + S(14);
          break;
        case Row::Text:
          if (r.input) r.input->resize(x, int(y + S(24)), w, S(32));
          y += S(24) + S(32) + S(14);
          break;
        case Row::Check:
          y += S(32) + S(12);
          break;
        case Row::Note:
          text_font(t.font_size - S(1));
          y += S(18) * double(wrap_words(r.label, w).size()) + S(10);
          break;
      }
      r.h = y - r.y;
    }
    if (preview_fn) {
      preview_y = y;
      y += S(24) + preview_rows * S(20) + S(16) + S(10);
    }
    return int(y + scroll + S(60));
  }

  void scroll_by(double dy) {
    if (!win) return;
    double most = std::max(0, content_h - win->h());
    double s = std::max(0.0, std::min(most, scroll + dy));
    if (s == scroll) return;
    scroll = s;
    layout();
    win->redraw();
  }

  void draw() {
    const Theme &t = active_theme();
    layout();
    int x = S(24), w = width() - S(48);
    int clip_h = scrolls() ? rows_bottom() : win->h();
    {
      Painter p(win->w(), win->h());
      p.fill_rect(0, 0, win->w(), win->h(), t.surface);
      cairo_save(p.cr());
      cairo_rectangle(p.cr(), 0, 0, win->w(), clip_h);
      cairo_clip(p.cr());
      for (size_t i = 0; i < rows.size(); ++i) {
        const Row &r = rows[i];
        if (r.kind == Row::Choice) {
          for (size_t k = 0; k < r.chips.size(); ++k) {
            const auto &c = r.chips[k];
            bool on = int(k) == r.value, hot = int(i) == hover_row && int(k) == hover_chip && r.enabled;
            Color bg = on ? t.accent.alpha(t.dark ? 0.32 : 0.16) : hot ? t.hover : t.surface_alt;
            p.fill_rounded(c.x, c.y, c.w, c.h, Sd(9), bg);
            if (on) p.stroke_rounded(c.x + 0.5, c.y + 0.5, c.w - 1, c.h - 1, Sd(9), t.accent, Sd(1.2));
          }
        } else if (r.kind == Row::Check) {
          bool hot = int(i) == hover_row && r.enabled;
          p.fill_rounded(x, r.y, w, S(32), Sd(9), r.on ? t.accent.alpha(t.dark ? 0.32 : 0.16) : hot ? t.hover : t.surface_alt);
          double bx = x + S(10), by = r.y + S(8);
          p.stroke_rounded(bx, by, S(16), S(16), Sd(4), r.on ? t.accent : t.text_dim, Sd(1.5));
          if (r.on) p.fill_rounded(bx + S(3), by + S(3), S(10), S(10), Sd(2), t.accent);
        }
      }
      if (preview_fn) {
        double py = preview_y + S(24);
        p.fill_rounded(x, py, w, preview_rows * S(20) + S(16), Sd(9), t.surface_alt);
      }
      cairo_restore(p.cr());
      if (scrolls()) {
        // A line above the buttons and a scroll bar show there is more.
        p.line(0, clip_h - 0.5, win->w(), clip_h - 0.5, t.border.alpha(0.7));
        double track = clip_h - S(16), thumb = std::max<double>(S(24), track * clip_h / content_h);
        double most = std::max(1, content_h - win->h());
        double ty = S(8) + (track - thumb) * scroll / most;
        p.fill_rounded(win->w() - S(9), ty, S(4), thumb, Sd(2), t.text_dim.alpha(0.45));
      }
    }
    fl_push_clip(0, 0, win->w(), clip_h);
    text_font(t.font_size + S(2), true);
    draw_text(title, x, int(S(16) - scroll), w, S(30), t.text);
    double y = S(50) - scroll;
    text_font(t.font_size);
    if (!message.empty())
      for (const auto &line : wrap_words(message, w)) {
        draw_text(line, x, int(y), w, S(20), t.text_dim);
        y += S(20);
      }
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row &r = rows[i];
      Color fg = r.enabled ? t.text : t.text_dim.alpha(0.6);
      switch (r.kind) {
        case Row::Choice:
          text_font(t.font_size - S(1), true);
          draw_text(r.label, x, int(r.y), w, S(20), r.enabled ? t.accent_text : fg);
          for (size_t k = 0; k < r.chips.size(); ++k) {
            const auto &c = r.chips[k];
            bool on = int(k) == r.value;
            text_font(t.font_size - S(1), on);
            draw_text(r.options[k], int(c.x), int(c.y), int(c.w), int(c.h), !r.enabled ? fg : on ? t.accent_text : t.text,
                      1);
          }
          break;
        case Row::Slider:
          text_font(t.font_size - S(1), true);
          draw_text(r.label, x, int(r.y), w, S(20), r.enabled ? t.accent_text : fg);
          if (r.slider) {
            text_font(t.font_size - S(1));
            double v = r.slider->value();
            draw_text(r.show ? r.show(v) : format("%g", v), x, int(r.y), w, S(20), t.text_dim, 2);
          }
          break;
        case Row::Text:
          text_font(t.font_size - S(1), true);
          draw_text(r.label, x, int(r.y), w, S(20), r.enabled ? t.accent_text : fg);
          break;
        case Row::Check:
          text_font(t.font_size - S(1), r.on);
          draw_text(r.label, x + S(34), int(r.y), w - S(40), S(32), !r.enabled ? fg : r.on ? t.accent_text : t.text);
          break;
        case Row::Note: {
          text_font(t.font_size - S(1));
          double ly = r.y;
          for (const auto &line : wrap_words(r.label, w)) {
            draw_text(line, x, int(ly), w, S(18), t.text_dim);
            ly += S(18);
          }
          break;
        }
      }
    }
    if (preview_fn) {
      text_font(t.font_size - S(1), true);
      draw_text(preview_label, x, int(preview_y), w, S(20), t.accent_text);
      text_font(t.font_size - S(1));
      auto lines = preview_fn();
      double ly = preview_y + S(24) + S(8);
      for (int i = 0; i < preview_rows && i < int(lines.size()); ++i) {
        draw_text(lines[size_t(i)], x + S(12), int(ly), w - S(24), S(20), t.text);
        ly += S(20);
      }
    }
    fl_pop_clip();
    win->paint_children(clip_h);
    // Hints over empty text fields (drawn after them).
    fl_push_clip(0, 0, win->w(), clip_h);
    for (auto &r : rows)
      if (r.input && r.input->size() == 0 && Fl::focus() != r.input && !r.hint.empty()) {
        text_font(t.font_size);
        draw_text(r.hint, r.input->x() + S(4), r.input->y(), r.input->w() - S(8), r.input->h(), t.text_dim);
      }
    fl_pop_clip();
  }

  void notify() {
    if (changed) changed(*form);
    for (auto &r : rows) {
      if (r.slider) r.enabled ? r.slider->activate() : r.slider->deactivate();
      if (r.input) r.input->readonly(!r.enabled);
    }
    if (win) win->redraw();
  }

  bool handle(int event) {
    int mx = Fl::event_x(), my = Fl::event_y();
    switch (event) {
      case FL_KEYBOARD:
      case FL_SHORTCUT:
        if (Fl::event_key() == FL_Escape) {
          result = 0;
          win->hide();
          return true;
        }
        if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
          result = -2;  // the default button
          win->hide();
          return true;
        }
        return false;
      case FL_MOUSEWHEEL:
        if (!scrolls()) return false;
        scroll_by(Fl::event_dy() * S(48));
        return true;
      case FL_MOVE:
      case FL_ENTER: {
        int hr = -1, hc = -1;
        if (my >= rows_bottom()) mx = my = -1000000;  // over the buttons
        for (size_t i = 0; i < rows.size(); ++i) {
          const Row &r = rows[i];
          if (r.kind == Row::Choice) {
            for (size_t k = 0; k < r.chips.size(); ++k)
              if (mx >= r.chips[k].x && mx < r.chips[k].x + r.chips[k].w && my >= r.chips[k].y &&
                  my < r.chips[k].y + r.chips[k].h) {
                hr = int(i);
                hc = int(k);
              }
          } else if (r.kind == Row::Check && my >= r.y && my < r.y + S(32) && mx >= S(24) && mx < width() - S(24)) {
            hr = int(i);
          }
        }
        if (hr != hover_row || hc != hover_chip) {
          hover_row = hr;
          hover_chip = hc;
          win->redraw();
        }
        return false;
      }
      case FL_PUSH:
        if (hover_row >= 0 && rows[size_t(hover_row)].enabled) {
          Row &r = rows[size_t(hover_row)];
          if (r.kind == Row::Choice && hover_chip >= 0) r.value = hover_chip;
          else if (r.kind == Row::Check) r.on = !r.on;
          notify();
          return true;
        }
        return false;
    }
    return false;
  }
};

Form::Form(const std::string &title, const std::string &message) : impl_(new Impl) {
  impl_->form = this;
  impl_->title = title;
  impl_->message = message;
}

Form::~Form() {
  if (impl_->win) Fl::delete_widget(impl_->win);
}

int Form::choice(const std::string &label, const std::vector<std::string> &options, int value) {
  Impl::Row r;
  r.kind = Impl::Row::Choice;
  r.label = label;
  r.options = options;
  r.value = std::max(0, std::min(int(options.size()) - 1, value));
  impl_->rows.push_back(r);
  return int(impl_->rows.size()) - 1;
}

int Form::slider(const std::string &label, double lo, double hi, double step, double value,
                 std::function<std::string(double)> show) {
  Impl::Row r;
  r.kind = Impl::Row::Slider;
  r.label = label;
  r.show = std::move(show);
  impl_->rows.push_back(r);
  int i = int(impl_->rows.size()) - 1;
  // The widget is made with the window; keep the settings until then.
  impl_->rows.back().options = {format("%.17g", lo), format("%.17g", hi), format("%.17g", step), format("%.17g", value)};
  return i;
}

int Form::text(const std::string &label, const std::string &value, const std::string &hint) {
  Impl::Row r;
  r.kind = Impl::Row::Text;
  r.label = label;
  r.text = value;
  r.hint = hint;
  impl_->rows.push_back(r);
  return int(impl_->rows.size()) - 1;
}

int Form::check(const std::string &label, bool value) {
  Impl::Row r;
  r.kind = Impl::Row::Check;
  r.label = label;
  r.on = value;
  impl_->rows.push_back(r);
  return int(impl_->rows.size()) - 1;
}

void Form::note(const std::string &text) {
  Impl::Row r;
  r.kind = Impl::Row::Note;
  r.label = text;
  impl_->rows.push_back(r);
}

void Form::preview(const std::string &label, std::function<std::vector<std::string>()> lines, int rows) {
  impl_->preview_label = label;
  impl_->preview_fn = std::move(lines);
  impl_->preview_rows = rows;
}

void Form::on_change(std::function<void(Form &)> fn) { impl_->changed = std::move(fn); }

void Form::set_enabled(int row, bool on) {
  if (row >= 0 && row < int(impl_->rows.size())) impl_->rows[size_t(row)].enabled = on;
}

int Form::run(const std::vector<std::string> &buttons) {
  const Theme &t = active_theme();
  int W = impl_->width();
  impl_->scroll = 0;
  impl_->content_h = impl_->layout();
  // Taller than the screen: the rows scroll above the buttons.
  int sx, sy, sw, sh;
  work_area(sx, sy, sw, sh);
  int H = std::max(S(240), std::min(impl_->content_h, sh - S(40)));
  auto *win = new Impl::Win(impl_.get(), W, H);
  impl_->win = win;
  win->copy_label(impl_->title.c_str());
  win->begin();
  for (auto &r : impl_->rows) {
    if (r.kind == Impl::Row::Slider) {
      r.slider = new sde::Slider(0, 0, 10, S(26));
      double lo = atof(r.options[0].c_str()), hi = atof(r.options[1].c_str());
      r.slider->bounds(lo, hi);
      r.slider->step(atof(r.options[2].c_str()));
      r.slider->value(atof(r.options[3].c_str()));
      r.slider->callback([](Fl_Widget *w, void *) {
        auto *win = static_cast<Impl::Win *>(w->window());
        win->impl_->notify();
      });
    } else if (r.kind == Impl::Row::Text) {
      r.input = new Fl_Input(0, 0, 10, S(32));
      style_input(r.input, t);
      r.input->value(r.text.c_str());
      r.input->when(FL_WHEN_CHANGED);
      r.input->callback([](Fl_Widget *w, void *) {
        auto *win = static_cast<Impl::Win *>(w->window());
        win->impl_->notify();
      });
    }
  }
  int bw = S(132), x = W - S(24) - bw * int(buttons.size()) - S(8) * int(buttons.size() - 1);
  for (size_t i = 0; i < buttons.size(); ++i) {
    auto *b = new FlatButton(x, H - S(52), bw, S(34), buttons[i], i + 1 == buttons.size());
    b->callback([](Fl_Widget *w, void *data) {
      auto *win = static_cast<Impl::Win *>(w->window());
      win->impl_->result = int(reinterpret_cast<long>(data));
      win->hide();
    }, reinterpret_cast<void *>(long(i)));
    x += bw + S(8);
  }
  win->end();
  win->color(t.surface.fl());
  impl_->layout();
  impl_->notify();
  for (auto &r : impl_->rows)
    if (r.input) {
      Fl::focus(r.input);
      r.input->insert_position(r.input->size(), 0);
      break;
    }
  run_window(win);
  int result = impl_->result == -2 ? int(buttons.size()) - 1 : impl_->result;
  // Keep the values after the window is gone.
  for (auto &r : impl_->rows) {
    if (r.slider) r.options[3] = format("%.17g", r.slider->value());
    if (r.input) r.text = r.input->value();
  }
  return result;
}

int Form::choice_value(int row) const {
  return row >= 0 && row < int(impl_->rows.size()) ? impl_->rows[size_t(row)].value : 0;
}

double Form::slider_value(int row) const {
  if (row < 0 || row >= int(impl_->rows.size())) return 0;
  const auto &r = impl_->rows[size_t(row)];
  return r.slider && r.slider->window() && r.slider->window()->shown() ? r.slider->value() : atof(r.options[3].c_str());
}

std::string Form::text_value(int row) const {
  if (row < 0 || row >= int(impl_->rows.size())) return "";
  const auto &r = impl_->rows[size_t(row)];
  return r.input && r.input->window() && r.input->window()->shown() ? std::string(r.input->value()) : r.text;
}

bool Form::check_value(int row) const {
  return row >= 0 && row < int(impl_->rows.size()) && impl_->rows[size_t(row)].on;
}

// ---------------------------------------------------------------- fields

namespace {

class FieldsWindow : public BufferedWindow {
 public:
  FieldsWindow(int w, int h, const std::string &title, const std::string &message,
               const std::vector<std::pair<std::string, std::string>> &rows)
      : BufferedWindow(w, h), title_(title), message_(message), rows_(rows) {
    copy_label(title.c_str());
    begin();
    auto *close = new FlatButton(w - S(24) - S(120), h - S(52), S(120), S(34), _("Close"), true);
    close->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
    end();
  }

  void draw() override {
    const Theme &t = active_theme();
    int lx = S(24), lw = w() - S(48), name_w = lw * 2 / 5;
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      p.line(lx, top() - 0.5, lx + lw, top() - 0.5, t.border.alpha(0.6));
      p.line(lx, bottom() + 0.5, lx + lw, bottom() + 0.5, t.border.alpha(0.6));
      if (content_h() > bottom() - top()) {
        double track = bottom() - top() - S(8), thumb = std::max<double>(S(24), track * (bottom() - top()) / content_h());
        double most = std::max(1.0, content_h() - (bottom() - top()));
        p.fill_rounded(w() - S(12), top() + S(4) + (track - thumb) * scroll_ / most, S(4), thumb, Sd(2),
                       t.text_dim.alpha(0.45));
      }
    }
    text_font(t.font_size + S(2), true);
    draw_text(title_, lx, S(16), lw, S(30), t.text);
    text_font(t.font_size);
    draw_text(message_, lx, S(48), lw, S(22), t.text_dim);
    fl_push_clip(0, top(), w(), bottom() - top());
    double y = top() + S(8) - scroll_;
    for (const auto &r : rows_) {
      if (r.second.empty()) {
        y += S(6);
        text_font(t.font_size - S(1), true);
        draw_text(upper(r.first), lx, int(y), lw, S(22), t.accent_text);
        y += S(26);
        continue;
      }
      text_font(t.font_size - S(1));
      draw_text(r.first, lx, int(y), name_w - S(8), S(22), t.text_dim);
      text_font(t.font_size);
      draw_text(r.second, lx + name_w, int(y), lw - name_w, S(22), t.text);
      y += S(24);
    }
    fl_pop_clip();
    draw_children();
  }

  int handle(int event) override {
    if (event == FL_MOUSEWHEEL) {
      double most = std::max(0.0, content_h() - (bottom() - top()));
      scroll_ = std::max(0.0, std::min(most, scroll_ + Fl::event_dy() * S(48)));
      redraw();
      return 1;
    }
    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) &&
        (Fl::event_key() == FL_Escape || Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
      hide();
      return 1;
    }
    return BufferedWindow::handle(event);
  }

 private:
  static std::string upper(std::string s) {
    for (auto &c : s)
      if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
  }
  int top() const { return S(82); }
  int bottom() const { return h() - S(68); }
  double content_h() const {
    double hgt = S(16);
    for (const auto &r : rows_) hgt += r.second.empty() ? S(32) : S(24);
    return hgt;
  }
  std::string title_, message_;
  std::vector<std::pair<std::string, std::string>> rows_;
  double scroll_ = 0;
};

}  // namespace

void show_fields(const std::string &title, const std::string &message,
                 const std::vector<std::pair<std::string, std::string>> &rows) {
  const Theme &t = active_theme();
  double content = S(16);
  for (const auto &r : rows) content += r.second.empty() ? S(32) : S(24);
  int sx, sy, sw, sh;
  work_area(sx, sy, sw, sh);
  int W = S(560), H = std::max(S(260), std::min(int(content) + S(150), sh - S(60)));
  auto *win = new FieldsWindow(W, H, title, message, rows);
  win->color(t.surface.fl());
  run_window(win);
  Fl::delete_widget(win);
}

// ------------------------------------------------------- choose application

namespace {

class AppChooser : public BufferedWindow {
 public:
  AppChooser(int w, int h, const std::string &title, const std::string &message,
             const std::vector<DesktopEntry> &apps, bool with_default)
      : BufferedWindow(w, h), title_(title), message_(message), apps_(apps), with_default_(with_default) {
    copy_label(title.c_str());
    begin();
    auto *cancel = new FlatButton(w - S(24) - 2 * S(120) - S(8), h - S(52), S(120), S(34), _("Cancel"));
    cancel->callback([](Fl_Widget *b, void *) {
      static_cast<AppChooser *>(b->window())->chosen = -1;
      b->window()->hide();
    });
    open_ = new FlatButton(w - S(24) - S(120), h - S(52), S(120), S(34), _("Open"), true);
    open_->callback([](Fl_Widget *b, void *) {
      auto *self = static_cast<AppChooser *>(b->window());
      self->chosen = self->sel_;
      b->window()->hide();
    });
    end();
    if (!apps_.empty()) sel_ = 0;
  }

  int chosen = -1;
  bool make_default = false;

  void draw() override {
    const Theme &t = active_theme();
    int lx = S(24), lw = w() - S(48);
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      cairo_save(p.cr());
      cairo_rectangle(p.cr(), 0, top(), w(), bottom() - top());
      cairo_clip(p.cr());
      for (size_t i = 0; i < apps_.size(); ++i) {
        double y = row_y(int(i));
        if (y + S(48) < top() || y > bottom()) continue;
        bool sel = int(i) == sel_, hot = int(i) == hover_;
        if (sel) p.fill_rounded(lx - S(6), y, lw + S(12), S(48), Sd(9), t.accent.alpha(t.dark ? 0.3 : 0.15));
        else if (hot) p.fill_rounded(lx - S(6), y, lw + S(12), S(48), Sd(9), t.hover);
        std::string icon = IconLoader::app_icon(apps_[i], S(32));
        if (cairo_surface_t *img = IconLoader::get({icon.empty() ? "application-x-executable" : icon,
                                                    "application-x-executable"},
                                                   S(32)))
          p.image(img, lx + S(4), y + S(8), S(32));
      }
      cairo_restore(p.cr());
      p.line(lx, top() - 0.5, lx + lw, top() - 0.5, t.border.alpha(0.6));
      p.line(lx, bottom() + 0.5, lx + lw, bottom() + 0.5, t.border.alpha(0.6));
      if (with_default_) {
        double by = bottom() + S(12);
        p.fill_rounded(lx, by, lw, S(32), Sd(9), make_default ? t.accent.alpha(t.dark ? 0.32 : 0.16) : t.surface_alt);
        p.stroke_rounded(lx + S(10), by + S(8), S(16), S(16), Sd(4), make_default ? t.accent : t.text_dim, Sd(1.5));
        if (make_default) p.fill_rounded(lx + S(13), by + S(11), S(10), S(10), Sd(2), t.accent);
      }
    }
    text_font(t.font_size + S(2), true);
    draw_text(title_, lx, S(16), lw, S(30), t.text);
    text_font(t.font_size);
    draw_text(message_, lx, S(48), lw, S(22), t.text_dim);
    fl_push_clip(0, top(), w(), bottom() - top());
    for (size_t i = 0; i < apps_.size(); ++i) {
      double y = row_y(int(i));
      if (y + S(48) < top() || y > bottom()) continue;
      text_font(t.font_size, true);
      draw_text(apps_[i].name, lx + S(48), int(y + S(4)), lw - S(56), S(22), t.text);
      text_font(t.font_size - S(1));
      std::string sub = !apps_[i].comment.empty() ? apps_[i].comment : apps_[i].generic_name;
      draw_text(sub, lx + S(48), int(y + S(24)), lw - S(56), S(20), t.text_dim);
    }
    fl_pop_clip();
    if (with_default_) {
      text_font(t.font_size - S(1), make_default);
      draw_text(_("Use it to edit photos from now on (Ctrl+E)"), lx + S(34), bottom() + S(12), lw - S(40), S(32),
                make_default ? t.accent_text : t.text);
    }
    draw_children();
  }

  int handle(int event) override {
    int mx = Fl::event_x(), my = Fl::event_y();
    switch (event) {
      case FL_KEYBOARD:
      case FL_SHORTCUT: {
        int key = Fl::event_key();
        if (key == FL_Escape) {
          chosen = -1;
          hide();
          return 1;
        }
        if (key == FL_Enter || key == FL_KP_Enter) {
          chosen = sel_;
          hide();
          return 1;
        }
        if (key == FL_Down || key == FL_Up) {
          int n = int(apps_.size());
          if (n) sel_ = std::max(0, std::min(n - 1, sel_ + (key == FL_Down ? 1 : -1)));
          // Keep it in view.
          double y = row_y(sel_);
          if (y < top()) scroll_ -= top() - y;
          else if (y + S(48) > bottom()) scroll_ += y + S(48) - bottom();
          redraw();
          return 1;
        }
        break;
      }
      case FL_MOVE:
      case FL_ENTER: {
        int r = row_at(mx, my);
        if (r != hover_) {
          hover_ = r;
          redraw();
        }
        break;
      }
      case FL_PUSH: {
        int r = row_at(mx, my);
        if (r >= 0) {
          sel_ = r;
          if (Fl::event_clicks()) {
            chosen = r;
            hide();
          }
          redraw();
          return 1;
        }
        if (with_default_ && my >= bottom() + S(12) && my < bottom() + S(44) && mx >= S(24) && mx < w() - S(24)) {
          make_default = !make_default;
          redraw();
          return 1;
        }
        break;
      }
      case FL_MOUSEWHEEL: {
        double room = bottom() - top(), content = apps_.size() * S(52.0);
        scroll_ = std::max(0.0, std::min(std::max(0.0, content - room), scroll_ + Fl::event_dy() * S(52)));
        redraw();
        return 1;
      }
    }
    return BufferedWindow::handle(event);
  }

 private:
  int top() const { return S(82); }
  int bottom() const { return h() - S(68) - (with_default_ ? S(48) : 0); }
  double row_y(int i) const { return top() + S(6) + i * S(52) - scroll_; }
  int row_at(int mx, int my) const {
    if (my < top() || my >= bottom() || mx < S(16) || mx > w() - S(16)) return -1;
    for (size_t i = 0; i < apps_.size(); ++i)
      if (my >= row_y(int(i)) && my < row_y(int(i)) + S(48)) return int(i);
    return -1;
  }
  std::string title_, message_;
  const std::vector<DesktopEntry> &apps_;
  bool with_default_;
  int sel_ = -1, hover_ = -1;
  double scroll_ = 0;
  FlatButton *open_ = nullptr;
};

}  // namespace

int choose_application(const std::string &title, const std::string &message, const std::vector<DesktopEntry> &apps,
                       bool *make_default) {
  const Theme &t = active_theme();
  int W = S(480), H = S(160) + S(52) * int(std::min<size_t>(apps.size(), 7)) + (make_default ? S(48) : 0);
  if (Fl_Window *parent = Fl::first_window()) H = std::min(H, std::max(S(320), parent->h() - S(40)));
  auto *win = new AppChooser(W, H, title, message, apps, make_default != nullptr);
  win->color(t.surface.fl());
  run_window(win);
  int r = win->chosen;
  if (make_default) *make_default = win->make_default;
  Fl::delete_widget(win);
  return r;
}

}  // namespace gallery
