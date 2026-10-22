// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny dialogs: the in-window question sheet.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <algorithm>
#include <cmath>
#include <set>

#include "i18n.h"
#include "niny.h"
#include "process.h"
#include "util.h"

using namespace sde;

namespace niny {

// ------------------------------------------------------------------ Sheet

Sheet::Sheet(App *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  box(FL_NO_BOX);
  end();
  hide();
}

void Sheet::layout_card() {
  cw_ = std::min<double>(S(460), w() - S(40));
  text_font(look().theme.font_size);
  int tw = 0, th = 0;
  fl_measure(message_.c_str(), tw = int(cw_ - S(48)), th, 0);
  ch_ = S(64) + th + S(72);
  cx_ = x() + (w() - cw_) / 2;
  cy_ = y() + std::max<double>(S(60), (h() - ch_) / 2.6);
  int bx = int(cx_ + cw_ - S(20));
  for (int i = int(buttons_.size()) - 1; i >= 0; --i) {
    text_font(look().theme.font_size, true);
    int bw = std::max<int>(S(90), text_width(buttons_[size_t(i)]->label() ? buttons_[size_t(i)]->label() : "") + S(32));
    bw = std::max(bw, S(96));
    buttons_[size_t(i)]->resize(bx - bw, int(cy_ + ch_ - S(52)), bw, S(34));
    bx -= bw + S(8);
  }
}

int Sheet::ask(const std::string &title, const std::string &message, const std::vector<std::string> &buttons,
               int default_button, int cancel_button) {
  if (!window() || active_) return cancel_button;  // not in a window, or already asking
  for (auto *b : buttons_) {
    remove(b);
    Fl::delete_widget(b);
  }
  buttons_.clear();
  title_ = title;
  message_ = message;
  default_ = default_button;
  cancel_ = cancel_button;
  begin();
  static std::vector<std::string> labels;
  labels = buttons;
  for (size_t i = 0; i < buttons.size(); ++i) {
    auto *b = new FlatButton(0, 0, S(100), S(34), buttons[i], int(i) == default_button);
    b->copy_label(buttons[i].c_str());
    b->labeltype(FL_NO_LABEL);
    b->callback([](Fl_Widget *w, void *p) {
      auto *self = static_cast<Sheet *>(p);
      for (size_t k = 0; k < self->buttons_.size(); ++k)
        if (self->buttons_[k] == w) self->result_ = int(k);
      self->active_ = false;
    }, this);
    buttons_.push_back(b);
  }
  end();
  resize(0, 0, window()->w(), window()->h());
  layout_card();
  result_ = cancel_button;
  active_ = true;
  show();
  Fl_Widget *old_focus = Fl::focus();
  if (default_button >= 0 && default_button < int(buttons_.size())) buttons_[size_t(default_button)]->take_focus();
  window()->redraw();
  while (active_ && window()->shown()) Fl::wait();
  hide();
  window()->redraw();
  if (old_focus && old_focus->visible_r()) old_focus->take_focus();
  return result_;
}

void Sheet::draw() {
  const Look &L = look();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), Color::hex(0x000000, L.dark ? 0.45 : 0.28));
    double r = Sd(L.radius + 6);
    p.shadow(cx_, cy_, cw_, ch_, r, L.shadow, S(20));
    p.fill_rounded(cx_, cy_, cw_, ch_, r, L.card);
    if (L.glass) p.glass_sheen(cx_, cy_, cw_, ch_ * 0.6, r, L.dark);
    p.stroke_rounded(cx_, cy_, cw_, ch_, r, L.card_border);
  }
  text_font(L.theme.font_size + S(3), true);
  draw_text(title_, int(cx_ + S(24)), int(cy_ + S(18)), int(cw_ - S(48)), S(28), L.text);
  text_font(L.theme.font_size);
  fl_color(L.dim.fl());
  fl_draw(message_.c_str(), int(cx_ + S(24)), int(cy_ + S(54)), int(cw_ - S(48)), int(ch_ - S(120)),
          FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
  draw_children();
}

int Sheet::handle(int event) {
  if (!active_) return 0;
  if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
    if (Fl::event_key() == FL_Escape) {
      result_ = cancel_;
      active_ = false;
      return 1;
    }
    if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
      for (size_t k = 0; k < buttons_.size(); ++k)
        if (Fl::focus() == buttons_[k]) result_ = int(k);
      if (Fl::focus() != nullptr && std::find(buttons_.begin(), buttons_.end(), Fl::focus()) == buttons_.end())
        result_ = default_;
      active_ = false;
      return 1;
    }
    if (Fl::event_key() == FL_Left || Fl::event_key() == FL_Right || Fl::event_key() == FL_Tab) {
      int cur = 0;
      for (size_t k = 0; k < buttons_.size(); ++k)
        if (Fl::focus() == buttons_[k]) cur = int(k);
      int n = int(buttons_.size());
      bool back = Fl::event_key() == FL_Left || (Fl::event_state() & FL_SHIFT);
      if (n) buttons_[size_t((cur + (back ? n - 1 : 1)) % n)]->take_focus();
      redraw();
      return 1;
    }
  }
  int r = Fl_Group::handle(event);
  if (event == FL_PUSH || event == FL_RELEASE || event == FL_DRAG || event == FL_MOVE || event == FL_MOUSEWHEEL ||
      event == FL_KEYBOARD || event == FL_SHORTCUT)
    return 1;
  return r;
}

}  // namespace niny
