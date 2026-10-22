// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny find & replace: a floating card over the top right of the editor.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>

#include "i18n.h"
#include "niny.h"

using namespace sde;

namespace niny {

namespace {

constexpr int kRow = 44;  // height of one bar row (unscaled)

// Small square button showing an icon or a short label; optionally a toggle
// bound to a flag.
class Chip : public DrawnButton {
 public:
  Chip(icon::Kind kind, const char *label, bool *flag)
      : DrawnButton(0, 0, S(28), S(28), nullptr, nullptr, Sd(6)), kind_(kind), label_(label), flag_(flag) {}
  void draw() override {
    const Look &L = look();
    bool hot = Fl::belowmouse() == this, on = flag_ && *flag_;
    {
      Painter p(window()->w(), window()->h());
      double r = Sd(6);
      if (on) {
        p.fill_rounded(x(), y(), w(), h(), r, L.accent.alpha(L.dark ? 0.32 : 0.16));
        p.stroke_rounded(x(), y(), w(), h(), r, L.accent.alpha(0.65));
      } else if (hot) {
        p.fill_rounded(x(), y(), w(), h(), r, L.hover);
      }
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
      if (!label_) icon::draw(p, kind_, x() + (w() - S(16)) / 2.0, y() + (h() - S(16)) / 2.0, S(16),
                              active_r() ? L.dim : L.dim.alpha(0.4));
    }
    if (label_) {
      fl_font(FL_HELVETICA_BOLD, look().theme.font_size - S(1));
      fl_color((on ? L.accent_text : L.dim).fl());
      fl_draw(label_, x(), y(), w(), h(), FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
      if (std::string(label_) == "W") {  // whole word: underline
        int tw = int(fl_width("W"));
        fl_line(x() + (w() - tw) / 2, y() + h() - S(7), x() + (w() + tw) / 2, y() + h() - S(7));
      }
    }
  }

 private:
  icon::Kind kind_;
  const char *label_;
  bool *flag_;
};

}  // namespace

DrawnButton *FindBar::chip(const char *label, bool *flag) {
  auto *b = new Chip(icon::Dot, label, flag);
  b->callback([](Fl_Widget *w, void *p) {
    auto *self = static_cast<FindBar *>(p);
    (void)w;
    self->redraw();
    self->app_->find_changed();
  }, this);
  return b;
}

DrawnButton *FindBar::icon_button(icon::Kind kind, std::function<void()> fn, const char *tip) {
  auto *b = new Chip(kind, nullptr, nullptr);
  b->tooltip(tip);
  actions_.push_back(std::make_unique<std::function<void()>>(std::move(fn)));
  auto *holder = actions_.back().get();  // lives as long as the find bar
  b->callback([](Fl_Widget *, void *f) { (*static_cast<std::function<void()> *>(f))(); }, holder);
  return b;
}

FindBar::FindBar(App *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  box(FL_NO_BOX);
  toggle_ = icon_button(icon::ChevronRight, [this] {
    replace_mode_ = !replace_mode_;
    app_->layout();
    if (replace_mode_) replace_->take_focus();
    app_->redraw();
  }, _("Toggle Replace (Ctrl+H)"));
  find_ = new Field(0, 0, 100, 30);
  find_->when(FL_WHEN_CHANGED);
  find_->callback([](Fl_Widget *, void *p) { static_cast<FindBar *>(p)->app_->find_changed(); }, this);
  find_->on_key = [this](int key, int state) {
    if (key == FL_Enter || key == FL_KP_Enter) {
      app_->find_next(!(state & FL_SHIFT));
      return 1;
    }
    if (key == FL_Escape) {
      app_->hide_find();
      return 1;
    }
    if (key == FL_Down) {
      app_->find_next(true);
      return 1;
    }
    if (key == FL_Up) {
      app_->find_next(false);
      return 1;
    }
    return 0;
  };
  case_ = chip("Aa", &opts_.case_sensitive);
  case_->tooltip(_("Match Case (Alt+C)"));
  word_ = chip("W", &opts_.whole_word);
  word_->tooltip(_("Whole Words (Alt+W)"));
  regex_ = chip(".*", &opts_.regex);
  regex_->tooltip(_("Regular Expression (Alt+R)"));
  prev_ = icon_button(icon::ChevronUp, [this] { app_->find_next(false); }, _("Previous Match (Shift+F3)"));
  next_ = icon_button(icon::ChevronDown, [this] { app_->find_next(true); }, _("Next Match (F3)"));
  close_ = icon_button(icon::Close, [this] { app_->hide_find(); }, _("Close (Esc)"));
  replace_ = new Field(0, 0, 100, 30);
  replace_->on_key = [this](int key, int state) {
    if (key == FL_Enter || key == FL_KP_Enter) {
      if (state & FL_CTRL) app_->replace_all();
      else app_->replace_one();
      return 1;
    }
    if (key == FL_Escape) {
      app_->hide_find();
      return 1;
    }
    return 0;
  };
  rep_one_ = new FlatButton(0, 0, S(88), S(28), _("Replace"));
  rep_one_->callback([](Fl_Widget *, void *p) { static_cast<FindBar *>(p)->app_->replace_one(); }, this);
  rep_all_ = new FlatButton(0, 0, S(64), S(28), _("All"), true);
  rep_all_->tooltip(_("Replace All (Ctrl+Enter)"));
  rep_all_->callback([](Fl_Widget *, void *p) { static_cast<FindBar *>(p)->app_->replace_all(); }, this);
  end();
  style_input(find_);
  style_input(replace_);
  hide();
}

int FindBar::wanted_height() const { return S(replace_mode_ ? kRow * 2 - 4 : kRow) + S(8); }

void FindBar::relayout() {
  style_input(find_);
  style_input(replace_);
  int pad = S(10), bh = S(28);
  int row1 = y() + S(8) + (S(kRow) - bh) / 2 - S(2);
  int xr = x() + w() - pad;
  close_->resize(xr - bh, row1, bh, bh);
  xr -= bh + S(2);
  next_->resize(xr - bh, row1, bh, bh);
  xr -= bh + S(2);
  prev_->resize(xr - bh, row1, bh, bh);
  xr -= bh + S(84);  // count text
  regex_->resize(xr - bh, row1, bh, bh);
  xr -= bh + S(2);
  word_->resize(xr - bh, row1, bh, bh);
  xr -= bh + S(2);
  case_->resize(xr - bh, row1, bh, bh);
  xr -= bh + S(8);
  toggle_->resize(x() + pad, row1, bh, bh);
  int fx = x() + pad + bh + S(14);
  find_->resize(fx, row1 + S(2), std::max(S(60), xr - fx - S(6)), bh - S(4));
  if (replace_mode_) {
    int row2 = row1 + S(kRow - 4);
    int bx = x() + w() - pad;
    rep_all_->resize(bx - S(64), row2, S(64), bh);
    bx -= S(64) + S(6);
    rep_one_->resize(bx - S(88), row2, S(88), bh);
    bx -= S(88) + S(14);
    replace_->resize(fx, row2 + S(2), std::max(S(60), bx - fx), bh - S(4));
    replace_->show();
    rep_one_->show();
    rep_all_->show();
  } else {
    replace_->hide();
    rep_one_->hide();
    rep_all_->hide();
  }
}

void FindBar::open(bool replace, const std::string &seed) {
  replace_mode_ = replace;
  if (!seed.empty() && seed.find('\n') == std::string::npos) find_->value(seed.c_str());
  show();
  app_->layout();
  find_->take_focus();
  find_->position(find_->size(), 0);
  redraw();
}

void FindBar::close() { hide(); }

void FindBar::set_status(const std::string &s, bool error) {
  status_ = s;
  error_ = error;
  redraw();
}

void FindBar::draw() {
  const Look &L = look();
  {
    Painter p(window()->w(), window()->h());
    double r = Sd(L.radius + 2);
    double cy = y() + S(4), ch = h() - S(8);
    p.shadow(x() + S(4), cy, w() - S(8), ch, r, L.shadow, S(10));
    p.fill_rounded(x() + S(4), cy, w() - S(8), ch, r, L.card);
    if (L.glass) p.glass_sheen(x() + S(4), cy, w() - S(8), ch, r, L.dark);
    p.stroke_rounded(x() + S(4), cy, w() - S(8), ch, r, L.card_border);
    auto field_bg = [&](Fl_Input *in, bool err) {
      double fr = L.liquid ? (in->h() + S(4)) / 2.0 : Sd(7);
      p.fill_rounded(in->x() - S(8), in->y() - S(2), in->w() + S(16), in->h() + S(4), fr, L.field);
      if (Fl::focus() == in || err)
        p.stroke_rounded(in->x() - S(8), in->y() - S(2), in->w() + S(16), in->h() + S(4), fr,
                         err ? Color::hex(0xD1453B) : L.accent, Sd(1.5));
    };
    field_bg(find_, error_);
    if (replace_mode_) field_bg(replace_, false);
    // The replace toggle is a chevron that points down when open.
  }
  draw_children();
  if (toggle_->visible()) {
    Painter p(window()->w(), window()->h());
    p.fill_rect(toggle_->x() + 1, toggle_->y() + 1, toggle_->w() - 2, toggle_->h() - 2,
                Fl::belowmouse() == toggle_ ? look().hover : look().card);
    icon::draw(p, replace_mode_ ? icon::ChevronDown : icon::ChevronRight, toggle_->x() + S(6), toggle_->y() + S(6),
               S(16), look().dim);
  }
  text_font(L.theme.font_size - S(1));
  std::string s = status_;
  if (find_->size() == 0) s = "";
  draw_text(s, regex_->x() + regex_->w() + S(6), prev_->y(), S(80), prev_->h(),
            error_ ? Color::hex(L.dark ? 0xFF8A80 : 0xC4262E) : L.dim, 1);
  if (find_->size() == 0 && Fl::focus() != find_) {
    draw_text(_("Find"), find_->x(), find_->y(), find_->w(), find_->h(), L.dim.alpha(0.7));
  }
  if (replace_mode_ && replace_->size() == 0 && Fl::focus() != replace_)
    draw_text(_("Replace with"), replace_->x(), replace_->y(), replace_->w(), replace_->h(), L.dim.alpha(0.7));
}

}  // namespace niny
