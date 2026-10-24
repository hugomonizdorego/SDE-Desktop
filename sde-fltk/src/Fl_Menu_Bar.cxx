//
// Menu bar widget for the Fast Light Tool Kit (FLTK).
//
// Copyright 1998-2010 by Bill Spitzak and others.
//
// This library is free software. Distribution and use rights are outlined in
// the file "COPYING" which should have been included with this file.  If this
// file is missing or damaged, see the license at:
//
//     https://www.fltk.org/COPYING.php
//
// Please see the following page on how to report bugs and issues:
//
//     https://www.fltk.org/bugs.php
//

#include <FL/Fl.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/fl_draw.H>
#include <FL/Fl_SDE_Style.H>

// SDE-FLTK: the menu bar under the pointer and the hovered title. FLTK
// clears the pointer when that menu bar is deleted.
static Fl_Widget *sde_hover_bar = 0;
static int sde_hover_item = -1;

static void sde_hover(Fl_Widget *bar, int item) {
  static int watched = 0;
  if (!watched) { Fl::watch_widget_pointer(sde_hover_bar); watched = 1; }
  sde_hover_bar = bar;
  sde_hover_item = item;
}

static int sde_title_at(Fl_Menu_Bar *b, int mx) {
  if (!b->menu() || !b->menu()->text) return -1;
  int X = b->x() + 6, i = 0;
  for (const Fl_Menu_Item *m = b->menu()->first(); m->text; m = m->next(), i++) {
    int W = m->measure(0, b) + 16;
    if (mx >= X && mx < X + W) return i;
    X += W;
  }
  return -1;
}

void Fl_Menu_Bar::draw() {
  if (fl_sde_style() &&
      fl_sde_style()->menubar_draw(this, sde_hover_bar == this ? sde_hover_item : -1))
    return;
  draw_box();
  if (!menu() || !menu()->text) return;
  const Fl_Menu_Item* m;
  int X = x()+6;
  for (m=menu()->first(); m->text; m = m->next()) {
    int W = m->measure(0,this) + 16;
    m->draw(X, y(), W, h(), this);
    X += W;
    if (m->flags & FL_MENU_DIVIDER) {
      int y1 = y() + Fl::box_dy(box());
      int y2 = y1 + h() - Fl::box_dh(box()) - 1;

      // Draw a vertical divider between menus...
      fl_color(FL_DARK3);
      fl_yxline(X - 6, y1, y2);
      fl_color(FL_LIGHT3);
      fl_yxline(X - 5, y1, y2);
    }
  }
}

int Fl_Menu_Bar::handle(int event) {
  const Fl_Menu_Item* v;
  if (menu() && menu()->text) switch (event) {
  case FL_ENTER:
  case FL_MOVE:
  case FL_LEAVE:
    if (fl_sde_style()) {
      int i = event == FL_LEAVE ? -1 : sde_title_at(this, Fl::event_x());
      if (sde_hover_bar != this || sde_hover_item != i) {
        if (sde_hover_bar && sde_hover_bar != this) sde_hover_bar->redraw();
        sde_hover(i < 0 ? 0 : this, i);
        redraw();
      }
    }
    return 1;
  case FL_PUSH:
    if (sde_hover_bar == this) { sde_hover(0, -1); redraw(); }
    v = 0;
  J1:
    v = menu()->pulldown(x(), y(), w(), h(), v, this, 0, 1);
    picked(v);
    return 1;
  case FL_SHORTCUT:
    if (visible_r()) {
      v = menu()->find_shortcut(0, true);
      if (v && v->submenu()) goto J1;
    }
    return test_shortcut() != 0;
  }
  return 0;
}


Fl_Menu_Bar::Fl_Menu_Bar(int X, int Y, int W, int H,const char *l)
: Fl_Menu_(X,Y,W,H,l)
{
}


void Fl_Menu_Bar::play_menu(const Fl_Menu_Item *v) {
  if (v) {
    v = menu()->pulldown(x(), y(), w(), h(), v, this, 0, 1);
    picked(v);
  }
}
