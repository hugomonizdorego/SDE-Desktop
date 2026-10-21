// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel_widgets.h"

#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include "paint.h"
#include "panel.h"
#include "theme.h"

using namespace sde;

TextField::TextField(int x, int y, int w, int h, const std::string &name,
                     const std::string &placeholder, std::function<void(const std::string &)> changed)
    : Fl_Input(x, y, w, h), placeholder_(placeholder), changed_(std::move(changed)) {
  copy_tooltip(name.c_str());
  box(FL_FLAT_BOX);
  when(FL_WHEN_CHANGED);
  callback([](Fl_Widget *w, void *) {
    auto *f = static_cast<TextField *>(w);
    if (f->changed_) f->changed_(f->value());
  });
}

void TextField::draw() {
  const Theme &t = Panel::instance->theme();
  bool focus = Fl::focus() == this;
  {
    Painter p(window()->w(), window()->h());
    p.fill_rounded(x(), y(), w(), h(), S(8), t.surface);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(8), focus ? t.accent : t.border,
                     focus ? Sd(2) : 1.0);
  }
  textfont(FL_HELVETICA);
  textsize(t.font_size);
  textcolor(t.text.fl());
  selection_color(t.accent.mix(t.surface, 0.6).fl());
  cursor_color(t.text.fl());
  int pad = S(8);
  if (!size() && !focus && !placeholder_.empty()) {
    text_font(t.font_size);
    draw_text(placeholder_, x() + pad, y(), w() - 2 * pad, h(), t.text_dim);
    return;
  }
  fl_push_clip(x() + pad, y() + S(2), w() - 2 * pad, h() - S(4));
  drawtext(x() + pad, y() + S(2), w() - 2 * pad, h() - S(4));
  fl_pop_clip();
}

int TextField::handle(int event) {
  int r = Fl_Input::handle(event);
  if (event == FL_FOCUS || event == FL_UNFOCUS) redraw();
  return r;
}

std::string focused_name(Fl_Window *win) {
  Fl_Widget *f = Fl::focus();
  if (!f || f->window() != win || !f->tooltip()) return "";
  return f->tooltip();
}

namespace {
Fl_Widget *find_named(Fl_Group *g, const std::string &name) {
  for (int i = 0; i < g->children(); ++i) {
    Fl_Widget *c = g->child(i);
    if (c->tooltip() && name == c->tooltip()) return c;
    if (auto *sub = c->as_group())
      if (Fl_Widget *w = find_named(sub, name)) return w;
  }
  return nullptr;
}
}  // namespace

void restore_focus(Fl_Window *win, const std::string &name) {
  if (name.empty()) return;
  Fl_Widget *w = find_named(win, name);
  if (!w || !w->visible_r()) return;
  w->take_focus();
  if (auto *in = dynamic_cast<Fl_Input *>(w)) in->position(in->size());
}
