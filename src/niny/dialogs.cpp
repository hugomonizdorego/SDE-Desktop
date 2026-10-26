// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Niny dialogs: the in-window question sheet and the Font dialog.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
  if (!window()) {
    fprintf(stderr, "niny: BUG: the question sheet is not part of a window\n");
    return cancel_button;
  }
  if (active_) return cancel_button;  // already asking
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
  app_->schedule_qa();
  while (active_ && window()->shown()) Fl::wait();
  hide();
  window()->redraw();
  app_->schedule_qa();
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

// ------------------------------------------------------------- FontDialog

namespace {
const int kSizes[] = {8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24, 28, 32};
const int kSizeCount = int(sizeof kSizes / sizeof kSizes[0]);
const Fl_Font kPreviewFont = Fl_Font(FL_FREE_FONT + 4);  // the editor uses FL_FREE_FONT ... + 2
}  // namespace

FontDialog::FontDialog(App *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  box(FL_NO_BOX);
  search_ = new Field(0, 0, 100, 30);
  search_->when(FL_WHEN_CHANGED);
  search_->callback([](Fl_Widget *, void *p) { static_cast<FontDialog *>(p)->filter(); }, this);
  // The search field keeps the keyboard: the arrows move in the lists.
  search_->on_key = [this](int key, int state) {
    state &= FL_SHIFT | FL_CTRL | FL_ALT | FL_META;
    int &sel = selected(list_);
    int n = count(list_), page = std::max(1, rows_in(list_) - 1);
    switch (key) {
      case FL_Escape: active_ = false; return 1;
      case FL_Enter:
      case FL_KP_Enter:
        chosen_ = !shown_.empty();
        active_ = false;
        return 1;
      case FL_Tab:
        list_ = state & FL_SHIFT ? 0 : 1 - list_;
        redraw();
        return 1;
      case FL_Up: sel = std::max(0, sel - 1); break;
      case FL_Down: sel = std::min(n - 1, sel + 1); break;
      case FL_Page_Up: sel = std::max(0, sel - page); break;
      case FL_Page_Down: sel = std::min(n - 1, sel + page); break;
      default: return 0;
    }
    if (list_ == 0 && sel >= 0 && sel < int(shown_.size())) family_name_ = shown_[size_t(sel)];
    keep_in_view(list_);
    redraw();
    return 1;
  };
  auto pressed = [](Fl_Widget *w, void *p) {
    auto *d = static_cast<FontDialog *>(p);
    if (w == d->default_) {  // Monospace 11, as in a new niny.conf
      d->search_->value("");
      d->family_name_ = Settings().font;
      for (int i = 0; i < kSizeCount; ++i)
        if (kSizes[i] == Settings().font_size) d->size_index_ = i;
      d->filter();
      d->keep_in_view(1);
      d->search_->take_focus();
      return;
    }
    d->chosen_ = w == d->select_ && !d->shown_.empty();
    d->active_ = false;
  };
  auto button = [&](const char *label, bool primary) {
    auto *b = new FlatButton(0, 0, S(100), S(34), label, primary);
    b->copy_label(label);  // named for the UI tests; FlatButton draws its own text
    b->labeltype(FL_NO_LABEL);
    b->callback(pressed, this);
    return b;
  };
  default_ = button(_("Default"), false);
  cancel_ = button(_("Cancel"), false);
  select_ = button(_("Select"), true);
  end();
  hide();
}

int FontDialog::count(int list) const { return list ? kSizeCount : int(shown_.size()); }

int FontDialog::rows_in(int) const { return std::max(1, (lh_ - S(8)) / S(30)); }

void FontDialog::keep_in_view(int list) {
  int sel = list ? size_index_ : family_;
  int rows = rows_in(list);
  int &top = scroll_[list];
  if (sel < top) top = sel;
  if (sel >= top + rows) top = sel - rows + 1;
  top = std::max(0, std::min(top, std::max(0, count(list) - rows)));
}

void FontDialog::filter() {
  std::string q = lower(trim(search_->value()));
  std::vector<std::string> all = font_families();
  if (!original_.empty() && std::find(all.begin(), all.end(), original_) == all.end())
    all.insert(all.begin(), original_);
  shown_.clear();
  for (const std::string &f : all)
    if (q.empty() || lower(f).find(q) != std::string::npos) shown_.push_back(f);
  // The chosen family stays chosen while it matches; else the first match.
  family_ = 0;
  for (size_t i = 0; i < shown_.size(); ++i)
    if (shown_[i] == family_name_) family_ = int(i);
  if (!shown_.empty()) family_name_ = shown_[size_t(family_)];
  scroll_[0] = 0;
  keep_in_view(0);
  select_->activate();
  if (shown_.empty()) select_->deactivate();
  redraw();
}

void FontDialog::layout_card() {
  cw_ = std::min<double>(S(600), w() - S(24));
  ch_ = std::min<double>(S(500), h() - S(24));
  cx_ = x() + (w() - cw_) / 2;
  cy_ = y() + std::max<double>(S(12), (h() - ch_) / 2.4);
  int pad = S(22), gap = S(12), size_w = S(110);
  int top = int(cy_) + S(56);
  lx_[0] = int(cx_) + pad;
  lw_[0] = int(cw_) - 2 * pad - size_w - gap;
  lx_[1] = lx_[0] + lw_[0] + gap;
  lw_[1] = size_w;
  style_input(search_);
  search_->resize(lx_[0] + S(34), top + S(3), lw_[0] - S(42), S(28));
  ly_ = top + S(34) + S(10);
  int by = int(cy_ + ch_) - S(52);
  ph_ = std::max(S(44), std::min(S(72), int(ch_ * 0.16)));
  py_ = by - S(12) - ph_;
  lh_ = std::max(S(38), py_ - S(12) - ly_);
  text_font(look().theme.font_size, true);
  int bx = int(cx_ + cw_) - pad;
  for (sde::FlatButton *b : {select_, cancel_}) {
    int bw = std::max(S(96), text_width(b->label()) + S(32));
    b->resize(bx - bw, by, bw, S(34));
    bx -= bw + S(8);
  }
  default_->resize(lx_[0], by, std::max(S(96), text_width(default_->label()) + S(32)), S(34));
}

bool FontDialog::run(std::string &family, int &size) {
  if (!window() || active_) return false;
  original_ = family_name_ = family;
  size_index_ = 0;
  for (int i = 0; i < kSizeCount; ++i)
    if (std::abs(kSizes[i] - size) < std::abs(kSizes[size_index_] - size)) size_index_ = i;
  list_ = 0;
  resize(0, 0, window()->w(), window()->h());
  layout_card();
  search_->value("");
  filter();
  keep_in_view(1);
  chosen_ = false;
  active_ = true;
  show();
  Fl_Widget *old_focus = Fl::focus();
  search_->take_focus();
  window()->redraw();
  app_->schedule_qa();
  while (active_ && window()->shown()) Fl::wait();
  hide();
  window()->redraw();
  app_->schedule_qa();
  if (old_focus && old_focus->visible_r()) old_focus->take_focus();
  if (!chosen_) return false;
  family = family_name_;
  size = kSizes[size_index_];
  return true;
}

void FontDialog::draw() {
  const Look &L = look();
  int top = int(cy_) + S(56), row = S(30);
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), Color::hex(0x000000, L.dark ? 0.45 : 0.28));
    double r = Sd(L.radius + 6);
    p.shadow(cx_, cy_, cw_, ch_, r, L.shadow, S(20));
    p.fill_rounded(cx_, cy_, cw_, ch_, r, L.card);
    if (L.glass) p.glass_sheen(cx_, cy_, cw_, ch_ * 0.6, r, L.dark);
    p.stroke_rounded(cx_, cy_, cw_, ch_, r, L.card_border);
    // The search field, the two lists and the preview.
    p.fill_rounded(lx_[0], top, lw_[0], S(34), Sd(6), L.field);
    p.stroke_rounded(lx_[0] + 0.5, top + 0.5, lw_[0] - 1, S(34) - 1, Sd(6), list_ == 0 ? L.accent : L.border, 1);
    icon::draw(p, icon::Search, lx_[0] + S(9), top + S(8), S(18), L.dim);
    for (int l = 0; l < 2; ++l) {
      p.fill_rounded(lx_[l], ly_, lw_[l], lh_, Sd(6), L.field);
      p.stroke_rounded(lx_[l] + 0.5, ly_ + 0.5, lw_[l] - 1, lh_ - 1, Sd(6), l == list_ ? L.accent : L.border, 1);
      int sel = l ? size_index_ : family_;
      int y0 = ly_ + S(4) + (sel - scroll_[l]) * row;
      if (sel >= scroll_[l] && sel < scroll_[l] + rows_in(l) && sel < count(l))
        p.fill_rounded(lx_[l] + S(4), y0, lw_[l] - S(8), row - S(2), Sd(5),
                       L.accent.alpha(L.dark ? (l == list_ ? 0.34 : 0.2) : (l == list_ ? 0.18 : 0.1)));
    }
    p.fill_rounded(lx_[0], py_, lx_[1] + lw_[1] - lx_[0], ph_, Sd(6), L.editor);
    p.stroke_rounded(lx_[0] + 0.5, py_ + 0.5, lx_[1] + lw_[1] - lx_[0] - 1, ph_ - 1, Sd(6), L.border, 1);
  }
  text_font(L.theme.font_size + S(3), true);
  draw_text(_("Font"), int(cx_) + S(22), int(cy_) + S(16), int(cw_) - S(44), S(28), L.text);
  text_font(L.theme.font_size - S(1));
  draw_text(_("Size"), lx_[1] + S(4), top, lw_[1], S(34), L.dim);
  for (int l = 0; l < 2; ++l) {
    fl_push_clip(lx_[l] + S(2), ly_ + S(2), lw_[l] - S(4), lh_ - S(4));
    for (int i = scroll_[l]; i < count(l) && i < scroll_[l] + rows_in(l) + 1; ++i) {
      int ry = ly_ + S(4) + (i - scroll_[l]) * row;
      bool sel = i == (l ? size_index_ : family_);
      text_font(L.theme.font_size, sel);
      std::string label = l ? std::to_string(kSizes[i]) : shown_[size_t(i)];
      draw_text(label, lx_[l] + S(14), ry, lw_[l] - S(24), row - S(2), L.text);
    }
    fl_pop_clip();
  }
  if (shown_.empty()) {
    text_font(L.theme.font_size - S(1));
    draw_text(_("No fonts match"), lx_[0] + S(14), ly_ + S(4), lw_[0] - S(24), row, L.dim);
  }
  // The preview, in the chosen font and size.
  static std::string face;
  face = " " + family_name_;
  Fl::set_font(kPreviewFont, face.c_str());
  int px = std::max(8, int(std::lround(kSizes[size_index_] * 4.0 / 3.0 * ui_scale())));
  fl_font(kPreviewFont, px);
  fl_color(L.text.fl());
  fl_push_clip(lx_[0] + S(12), py_ + S(4), lx_[1] + lw_[1] - lx_[0] - S(24), ph_ - S(8));
  fl_draw(_("The quick brown fox jumps over the lazy dog. 0123456789 {}[]()"), lx_[0] + S(14), py_,
          lx_[1] + lw_[1] - lx_[0] - S(28), ph_, FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  fl_pop_clip();
  draw_children();
}

int FontDialog::handle(int event) {
  if (!active_) return 0;
  int mx = Fl::event_x(), my = Fl::event_y();
  auto list_at = [&]() {
    for (int l = 0; l < 2; ++l)
      if (mx >= lx_[l] && mx < lx_[l] + lw_[l] && my >= ly_ && my < ly_ + lh_) return l;
    return -1;
  };
  switch (event) {
    case FL_PUSH: {
      int l = list_at();
      if (l >= 0) {
        int i = scroll_[l] + (my - ly_ - S(4)) / S(30);
        if (i >= 0 && i < count(l)) {
          list_ = l;
          selected(l) = i;
          if (l == 0) family_name_ = shown_[size_t(i)];
          if (Fl::event_clicks() > 0) {  // a double click selects
            chosen_ = true;
            active_ = false;
          }
          redraw();
        }
        search_->take_focus();
        return 1;
      }
      break;
    }
    case FL_MOUSEWHEEL: {
      int l = list_at();
      if (l >= 0) {
        scroll_[l] = std::max(0, std::min(scroll_[l] + Fl::event_dy() * 3, std::max(0, count(l) - rows_in(l))));
        redraw();
        return 1;
      }
      break;
    }
    case FL_KEYBOARD:
    case FL_SHORTCUT:
      if (Fl::event_key() == FL_Escape) {
        active_ = false;
        return 1;
      }
      break;
  }
  int r = Fl_Group::handle(event);
  // Modal: nothing behind the card gets the pointer or the keys.
  if (event == FL_PUSH || event == FL_RELEASE || event == FL_DRAG || event == FL_MOVE || event == FL_MOUSEWHEEL ||
      event == FL_KEYBOARD || event == FL_SHORTCUT)
    return 1;
  return r;
}

}  // namespace niny
