// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <FL/Fl_Box.H>

#include <memory>

#include "paint.h"
#include "theme.h"

namespace sde {

// ------------------------------------------------------------------ Label

Label::Label(int x, int y, int w, int h, const std::string &text, Style style, bool wrap)
    : Fl_Widget(x, y, w, h), text_(text), style_(style), wrap_(wrap) {}

static void label_font(Label::Style style) {
  const Theme &t = active_theme();
  switch (style) {
    case Label::BOLD: text_font(t.font_size + 1, true); break;
    case Label::TITLE: text_font(t.font_size + 8, true); break;
    case Label::SUBTITLE: text_font(t.font_size); break;
    case Label::DIM: text_font(t.font_size - 1); break;
    default: text_font(t.font_size + 1); break;
  }
}

int Label::measure(const std::string &text, int w, Style style) {
  label_font(style);
  int ww = w, hh = 0;
  fl_measure(text.c_str(), ww, hh, 0);
  return hh;
}

void Label::draw() {
  const Theme &t = active_theme();
  label_font(style_);
  Color c = (style_ == DIM || style_ == SUBTITLE) ? t.text_dim : t.text;
  if (!active_r()) c = t.text_dim;
  if (wrap_) {
    fl_color(c.fl());
    fl_draw(text_.c_str(), x(), y(), w(), h(),
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  } else {
    draw_text(text_, x(), y(), w(), h(), c);
  }
}

void Label::set(const std::string &t) {
  text_ = t;
  redraw();
}

void Card::draw() {
  const Theme &t = active_theme();
  Painter p(window()->w(), window()->h());
  p.fill_rounded(x(), y(), w(), h(), Sd(12), t.surface);
  p.stroke_rounded(x(), y(), w(), h(), Sd(12), t.border);
}

// ------------------------------------------------------------------- Page

Page::Page(int x, int y, int w, int h) : Fl_Scroll(x, y, w, h) {
  const Theme &t = active_theme();
  type(Fl_Scroll::VERTICAL);
  box(FL_FLAT_BOX);
  color(t.background.fl());
  scrollbar_size(S(12));
  scrollbar.color(t.background.fl());
  scrollbar.selection_color(t.border.fl());
  scrollbar.box(FL_FLAT_BOX);
  scrollbar.slider(FL_FLAT_BOX);
  y_ = y + S(24);
  max_width(S(860));
}

void Page::max_width(int w) {
  // The usual margins, and more on both sides of content that is wider
  // than |w|: a maximized window on a large screen keeps lines readable.
  max_w_ = w;
  int inner = this->w() - scrollbar_size() - 2 * S(28);
  pad_ = S(28) + std::max(0, (inner - max_w_) / 2);
}

void Page::heading(const std::string &title, const std::string &subtitle) {
  int w = width();
  new Label(left(), y_, w, S(36), title, Label::TITLE);
  y_ += S(38);
  if (!subtitle.empty()) {
    new Label(left(), y_, w, S(22), subtitle, Label::SUBTITLE);
    y_ += S(24);
  }
  y_ += S(14);
}

void Page::begin_card(const std::string &title) {
  card_ = new Card(left(), y_, width(), S(40));
  card_y_ = y_;
  y_ += S(10);
  first_row_ = true;
  if (!title.empty()) {
    new Label(left() + S(20), y_, width() - S(40), S(30), title, Label::BOLD);
    y_ += S(32);
  }
}

void Page::end_card() {
  if (!card_) return;
  y_ += S(10);
  card_->size(width(), y_ - card_y_);
  card_ = nullptr;
  y_ += S(16);
}

void Page::note(const std::string &text) {
  int x = card_ ? left() + S(20) : left();
  int w = card_ ? width() - S(40) : width();
  int h = Label::measure(text, w, Label::DIM) + S(6);
  new Label(x, y_, w, h, text, Label::DIM, true);
  y_ += h + S(6);
}

void Page::control_box(int control_w, int *x, int *w) const {
  int inner_x = card_ ? left() + S(20) : left();
  int inner_w = card_ ? width() - S(40) : width();
  bool stacked = inner_w - control_w - S(16) < S(160);
  *x = stacked ? inner_x : inner_x + inner_w - control_w;
  *w = stacked ? inner_w : control_w;
}

Fl_Widget *Page::row(const std::string &label, int control_w, int control_h,
                     const std::function<Fl_Widget *(int, int, int, int)> &make, const std::string &hint) {
  int inner_x = card_ ? left() + S(20) : left();
  int inner_w = card_ ? width() - S(40) : width();
  int label_w = inner_w - control_w - S(16);
  // Narrow windows: put the control under its label.
  bool stacked = label_w < S(160);
  int row_h = std::max(S(40), control_h + S(8));
  int hint_h = hint.empty() ? 0 : Label::measure(hint, stacked ? inner_w : label_w, Label::DIM);
  if (stacked) {
    new Label(inner_x, y_, inner_w, S(26), label);
    y_ += S(28);
    if (!hint.empty()) {
      new Label(inner_x, y_, inner_w, hint_h, hint, Label::DIM, true);
      y_ += hint_h + S(4);
    }
    Fl_Widget *w = make(inner_x, y_, std::min(inner_w, std::max(control_w, inner_w)), control_h);
    y_ += control_h + S(10);
    return w;
  }
  int text_h = S(24) + (hint_h ? hint_h + S(2) : 0);
  int h = std::max(row_h, text_h + S(8));
  int ty = y_ + (h - text_h) / 2;
  new Label(inner_x, ty, label_w, S(24), label);
  if (!hint.empty()) new Label(inner_x, ty + S(24), label_w, hint_h, hint, Label::DIM, true);
  Fl_Widget *w = make(inner_x + inner_w - control_w, y_ + (h - control_h) / 2, control_w, control_h);
  y_ += h;
  return w;
}

Switch *Page::switch_row(const std::string &label, bool value, std::function<void(bool)> changed,
                         const std::string &hint) {
  auto *fn = keep(new std::function<void(bool)>(std::move(changed)));
  Switch *sw = nullptr;
  row(label, S(48), S(28), [&](int x, int y, int w, int h) {
    sw = new Switch(x, y, w, h);
    sw->value(value);
    sw->tooltip(nullptr);
    sw->callback([](Fl_Widget *wdg, void *d) {
      (*static_cast<std::function<void(bool)> *>(d))(static_cast<Switch *>(wdg)->value() != 0);
    }, fn);
    return sw;
  }, hint);
  return sw;
}

void style_choice(Fl_Choice *c) {
  const Theme &t = active_theme();
  c->box(FL_BORDER_BOX);
  c->down_box(FL_BORDER_BOX);
  c->color(t.surface_alt.fl());
  c->textcolor(t.text.fl());
  c->textsize(t.font_size + 1);
  c->labelsize(t.font_size + 1);
  c->selection_color(t.accent.fl());
}

std::string menu_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '/' || c == '&' || c == '_' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

Fl_Choice *Page::choice_row(const std::string &label, const std::vector<std::string> &options, int value,
                            std::function<void(int)> changed, const std::string &hint) {
  auto *fn = keep(new std::function<void(int)>(std::move(changed)));
  Fl_Choice *ch = nullptr;
  row(label, S(240), S(32), [&](int x, int y, int w, int h) {
    ch = new Fl_Choice(x, y, w, h);
    style_choice(ch);
    for (auto &o : options) ch->add(menu_escape(o).c_str());
    if (value >= 0 && value < int(options.size())) ch->value(value);
    ch->callback([](Fl_Widget *wdg, void *d) {
      (*static_cast<std::function<void(int)> *>(d))(static_cast<Fl_Choice *>(wdg)->value());
    }, fn);
    return ch;
  }, hint);
  return ch;
}

Slider *Page::slider_row(const std::string &label, double min, double max, double value,
                         std::function<void(double)> changed, const std::string &hint) {
  auto *fn = keep(new std::function<void(double)>(std::move(changed)));
  Slider *sl = nullptr;
  row(label, S(240), S(32), [&](int x, int y, int w, int h) {
    sl = new Slider(x, y, w, h);
    sl->bounds(min, max);
    sl->value(value);
    // Apply once the user lets go, not on every pixel of a drag.
    sl->when(FL_WHEN_RELEASE);
    sl->callback([](Fl_Widget *wdg, void *d) {
      (*static_cast<std::function<void(double)> *>(d))(static_cast<Slider *>(wdg)->value());
    }, fn);
    return sl;
  }, hint);
  return sl;
}

void Page::finish() {
  y_ += S(24);
  // Spacer so that the scroll range includes the bottom padding.
  auto *spacer = new Fl_Box(left(), y_ - S(4), 1, S(4));
  spacer->box(FL_NO_BOX);
  end();
}

}  // namespace sde
