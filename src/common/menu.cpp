// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The SDE look of FLTK's popups and stock widgets (an Fl_SDE_Style, see
// FL/Fl_SDE_Style.H in SDE-FLTK) and the ContextMenu builder.
#include "menu.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser_.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Light_Button.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <utility>

#include "config.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "theme.h"
#include "x11.h"

namespace sde {

namespace {

// ------------------------------------------------------------------ theme

// The theme of the program, or the configured one for programs that never
// call Theme::apply_to_fltk() (e.g. the panel draws everything itself).
Theme g_config_theme;
bool g_config_loaded = false;

void reload_config_theme() {
  Config cfg;
  cfg.load();
  double scale = ui_scale();
  g_config_theme = Theme::from_config(cfg);
  set_ui_scale(scale);  // reading the theme must not change the program's scale
  g_config_loaded = true;
}

const Theme &ui_theme() {
  const Theme &a = active_theme();
  if (!a.font.empty()) return a;
  if (!g_config_loaded) reload_config_theme();
  return g_config_theme;
}

Color from_fl(Fl_Color c) {
  uchar r, g, b;
  Fl::get_color(c, r, g, b);
  return Color{r / 255.0, g / 255.0, b / 255.0, 1.0};
}

int cur_w() { return Fl_Window::current() ? Fl_Window::current()->w() : 1; }
int cur_h() { return Fl_Window::current() ? Fl_Window::current()->h() : 1; }

std::string strip_mnemonic(const char *label) {
  std::string out;
  for (const char *p = label; p && *p; ++p) {
    if (*p == '&' && p[1] != '&') continue;
    if (*p == '&') ++p;
    out += *p;
  }
  return out;
}

// ------------------------------------------------------------------ icons

std::map<const Fl_Menu_Item *, std::string> &icons_by_item() {
  static std::map<const Fl_Menu_Item *, std::string> m;
  return m;
}
std::map<std::string, std::string> &icons_by_label() {
  static std::map<std::string, std::string> m;
  return m;
}

void line(Painter &p, double x1, double y1, double x2, double y2) {
  cairo_move_to(p.cr(), x1, y1);
  cairo_line_to(p.cr(), x2, y2);
}

void stroke(Painter &p, const Color &c, double lw) {
  p.set(c);
  cairo_set_line_width(p.cr(), lw);
  cairo_set_line_cap(p.cr(), CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(p.cr(), CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(p.cr());
}

void rect_path(Painter &p, double x, double y, double w, double h, double r) {
  p.rounded_rect(x, y, w, h, r);
}

// Line icons in the style of the SDE glyphs. Returns false for unknown names.
bool draw_line_icon(Painter &p, const std::string &name, double x, double y, double s,
                    const Color &c) {
  cairo_t *cr = p.cr();
  double lw = std::max(1.2, s * 0.085);
  auto X = [&](double f) { return x + s * f; };
  auto Y = [&](double f) { return y + s * f; };
  if (name == "cut" || name == "edit-cut") {
    cairo_new_sub_path(cr);
    cairo_arc(cr, X(0.28), Y(0.74), s * 0.13, 0, 2 * M_PI);
    cairo_new_sub_path(cr);
    cairo_arc(cr, X(0.72), Y(0.74), s * 0.13, 0, 2 * M_PI);
    line(p, X(0.36), Y(0.63), X(0.7), Y(0.12));
    line(p, X(0.64), Y(0.63), X(0.3), Y(0.12));
  } else if (name == "copy" || name == "edit-copy") {
    rect_path(p, X(0.32), Y(0.3), s * 0.5, s * 0.58, s * 0.08);
    cairo_move_to(cr, X(0.22), Y(0.7));
    cairo_line_to(cr, X(0.18), Y(0.7));
    cairo_line_to(cr, X(0.18), Y(0.12));
    cairo_line_to(cr, X(0.6), Y(0.12));
    cairo_line_to(cr, X(0.6), Y(0.2));
  } else if (name == "paste" || name == "edit-paste") {
    rect_path(p, X(0.2), Y(0.18), s * 0.6, s * 0.7, s * 0.08);
    rect_path(p, X(0.36), Y(0.1), s * 0.28, s * 0.16, s * 0.05);
    line(p, X(0.34), Y(0.5), X(0.66), Y(0.5));
    line(p, X(0.34), Y(0.66), X(0.56), Y(0.66));
  } else if (name == "delete" || name == "trash" || name == "edit-delete" ||
             name == "user-trash") {
    line(p, X(0.16), Y(0.24), X(0.84), Y(0.24));
    cairo_move_to(cr, X(0.38), Y(0.24));
    cairo_line_to(cr, X(0.4), Y(0.12));
    cairo_line_to(cr, X(0.6), Y(0.12));
    cairo_line_to(cr, X(0.62), Y(0.24));
    cairo_move_to(cr, X(0.24), Y(0.24));
    cairo_line_to(cr, X(0.29), Y(0.88));
    cairo_line_to(cr, X(0.71), Y(0.88));
    cairo_line_to(cr, X(0.76), Y(0.24));
    line(p, X(0.43), Y(0.4), X(0.44), Y(0.72));
    line(p, X(0.57), Y(0.4), X(0.56), Y(0.72));
  } else if (name == "select-all" || name == "edit-select-all") {
    cairo_save(cr);
    double d[] = {s * 0.1, s * 0.09};
    cairo_set_dash(cr, d, 2, 0);
    rect_path(p, X(0.12), Y(0.12), s * 0.76, s * 0.76, s * 0.06);
    stroke(p, c, lw);
    cairo_restore(cr);
    rect_path(p, X(0.32), Y(0.32), s * 0.36, s * 0.36, s * 0.04);
    p.set(c);
    cairo_fill(cr);
    return true;
  } else if (name == "undo" || name == "edit-undo" || name == "redo" || name == "edit-redo" ||
             name == "refresh" || name == "view-refresh") {
    bool redo = name.find("redo") != std::string::npos;
    if (name.find("refresh") != std::string::npos) {
      cairo_new_sub_path(cr);
      cairo_arc(cr, X(0.5), Y(0.52), s * 0.32, -M_PI * 0.35, M_PI * 1.45);
      cairo_move_to(cr, X(0.62), Y(0.12));
      cairo_line_to(cr, X(0.78), Y(0.24));
      cairo_line_to(cr, X(0.62), Y(0.38));
    } else {
      cairo_save(cr);
      if (redo) {
        cairo_translate(cr, x + s, y);
        cairo_scale(cr, -1, 1);
        cairo_translate(cr, -x, -y);
      }
      cairo_move_to(cr, X(0.34), Y(0.18));
      cairo_line_to(cr, X(0.18), Y(0.34));
      cairo_line_to(cr, X(0.34), Y(0.5));
      cairo_move_to(cr, X(0.18), Y(0.34));
      cairo_line_to(cr, X(0.6), Y(0.34));
      cairo_curve_to(cr, X(0.9), Y(0.34), X(0.9), Y(0.84), X(0.6), Y(0.84));
      cairo_line_to(cr, X(0.36), Y(0.84));
      cairo_restore(cr);
    }
  } else if (name == "rename" || name == "edit-rename") {
    rect_path(p, X(0.1), Y(0.3), s * 0.62, s * 0.4, s * 0.06);
    line(p, X(0.8), Y(0.16), X(0.8), Y(0.84));
    line(p, X(0.72), Y(0.16), X(0.88), Y(0.16));
    line(p, X(0.72), Y(0.84), X(0.88), Y(0.84));
  } else if (name == "share" || name == "send-to") {
    cairo_new_sub_path(cr);
    cairo_arc(cr, X(0.74), Y(0.22), s * 0.1, 0, 2 * M_PI);
    cairo_new_sub_path(cr);
    cairo_arc(cr, X(0.26), Y(0.5), s * 0.1, 0, 2 * M_PI);
    cairo_new_sub_path(cr);
    cairo_arc(cr, X(0.74), Y(0.78), s * 0.1, 0, 2 * M_PI);
    line(p, X(0.35), Y(0.45), X(0.65), Y(0.27));
    line(p, X(0.35), Y(0.55), X(0.65), Y(0.73));
  } else if (name == "open" || name == "folder" || name == "document-open" ||
             name == "folder-open") {
    cairo_move_to(cr, X(0.12), Y(0.8));
    cairo_line_to(cr, X(0.12), Y(0.2));
    cairo_line_to(cr, X(0.38), Y(0.2));
    cairo_line_to(cr, X(0.46), Y(0.3));
    cairo_line_to(cr, X(0.84), Y(0.3));
    cairo_line_to(cr, X(0.84), Y(0.8));
    cairo_close_path(cr);
  } else if (name == "new" || name == "document-new" || name == "save" ||
             name == "document-save" || name == "properties" || name == "document-properties") {
    cairo_move_to(cr, X(0.22), Y(0.1));
    cairo_line_to(cr, X(0.58), Y(0.1));
    cairo_line_to(cr, X(0.78), Y(0.3));
    cairo_line_to(cr, X(0.78), Y(0.9));
    cairo_line_to(cr, X(0.22), Y(0.9));
    cairo_close_path(cr);
    if (name.find("save") != std::string::npos) {
      line(p, X(0.5), Y(0.36), X(0.5), Y(0.72));
      cairo_move_to(cr, X(0.36), Y(0.58));
      cairo_line_to(cr, X(0.5), Y(0.72));
      cairo_line_to(cr, X(0.64), Y(0.58));
    } else if (name.find("propert") != std::string::npos) {
      line(p, X(0.34), Y(0.46), X(0.66), Y(0.46));
      line(p, X(0.34), Y(0.6), X(0.66), Y(0.6));
      line(p, X(0.34), Y(0.74), X(0.54), Y(0.74));
    } else {
      line(p, X(0.5), Y(0.4), X(0.5), Y(0.72));
      line(p, X(0.34), Y(0.56), X(0.66), Y(0.56));
    }
  } else if (name == "print" || name == "document-print") {
    rect_path(p, X(0.12), Y(0.36), s * 0.76, s * 0.34, s * 0.06);
    cairo_move_to(cr, X(0.28), Y(0.36));
    cairo_line_to(cr, X(0.28), Y(0.12));
    cairo_line_to(cr, X(0.72), Y(0.12));
    cairo_line_to(cr, X(0.72), Y(0.36));
    rect_path(p, X(0.28), Y(0.58), s * 0.44, s * 0.3, s * 0.03);
  } else if (name == "find" || name == "edit-find" || name == "search") {
    cairo_new_sub_path(cr);
    cairo_arc(cr, X(0.42), Y(0.42), s * 0.26, 0, 2 * M_PI);
    line(p, X(0.62), Y(0.62), X(0.86), Y(0.86));
  } else if (name == "settings" || name == "preferences-system") {
    glyph::gear(p, x, y, s, c);
    return true;
  } else if (name == "info" || name == "help-about" || name == "dialog-information") {
    cairo_new_sub_path(cr);
    cairo_arc(cr, X(0.5), Y(0.5), s * 0.38, 0, 2 * M_PI);
    line(p, X(0.5), Y(0.46), X(0.5), Y(0.7));
    stroke(p, c, lw);
    p.circle(X(0.5), Y(0.32), s * 0.055, c);
    return true;
  } else if (name == "image" || name == "image-x-generic") {
    rect_path(p, X(0.12), Y(0.18), s * 0.76, s * 0.64, s * 0.08);
    cairo_move_to(cr, X(0.2), Y(0.74));
    cairo_line_to(cr, X(0.42), Y(0.5));
    cairo_line_to(cr, X(0.56), Y(0.64));
    cairo_line_to(cr, X(0.66), Y(0.54));
    cairo_line_to(cr, X(0.8), Y(0.7));
  } else if (name == "link" || name == "insert-link") {
    cairo_save(cr);
    cairo_translate(cr, X(0.5), Y(0.5));
    cairo_rotate(cr, -M_PI / 4);
    p.rounded_rect(-s * 0.42, -s * 0.13, s * 0.46, s * 0.26, s * 0.13);
    p.rounded_rect(-s * 0.04, -s * 0.13, s * 0.46, s * 0.26, s * 0.13);
    cairo_restore(cr);
  } else if (name == "close" || name == "window-close") {
    line(p, X(0.24), Y(0.24), X(0.76), Y(0.76));
    line(p, X(0.76), Y(0.24), X(0.24), Y(0.76));
  } else {
    return false;
  }
  stroke(p, c, lw);
  return true;
}

void draw_icon(Painter &p, const std::string &name, double x, double y, double s,
               const Color &c, double alpha) {
  if (name.empty()) return;
  if (draw_line_icon(p, name, x, y, s, c.alpha(c.a * alpha))) return;
  if (cairo_surface_t *img = IconLoader::get({name}, int(std::lround(s))))
    p.image(img, x, y, s, alpha);
}

bool item_has_icon(const Fl_Menu_Item *m) { return !menu_icon(m).empty(); }

// ------------------------------------------------------------------ boxes

enum {
  BOX_BUTTON = FL_FREE_BOXTYPE + 40,
  BOX_BUTTON_DOWN,
  BOX_FIELD,
  BOX_CIRCLE,
};

void button_box(int x, int y, int w, int h, Fl_Color c, bool down) {
  const Theme &t = ui_theme();
  Painter p(cur_w(), cur_h());
  Color fill = from_fl(c);
  if (down) fill = fill.mix(t.text, 0.12);
  double r = std::min(Sd(6), h / 2.0);
  p.fill_rounded(x + 0.5, y + 0.5, w - 1, h - 1, r, fill);
  p.stroke_rounded(x, y, w, h, r, fill.luminance() < t.border.luminance() - 0.1 ? fill : t.border);
}
void button_up(int x, int y, int w, int h, Fl_Color c) { button_box(x, y, w, h, c, false); }
void button_down(int x, int y, int w, int h, Fl_Color c) { button_box(x, y, w, h, c, true); }
void field_box(int x, int y, int w, int h, Fl_Color c) {
  const Theme &t = ui_theme();
  Painter p(cur_w(), cur_h());
  double r = std::min(Sd(5), h / 2.0);
  p.fill_rounded(x + 0.5, y + 0.5, w - 1, h - 1, r, from_fl(c));
  p.stroke_rounded(x, y, w, h, r, t.border);
}
void circle_box(int x, int y, int w, int h, Fl_Color c) {
  Painter p(cur_w(), cur_h());
  double d = std::min(w, h) - Sd(4);
  p.circle(x + w / 2.0, y + h / 2.0, d / 2, from_fl(c));
}

void register_boxes() {
  static bool done = false;
  if (done) return;
  done = true;
  int b = std::max(2, S(3));
  Fl::set_boxtype(Fl_Boxtype(BOX_BUTTON), button_up, 1, 1, 2, 2);
  Fl::set_boxtype(Fl_Boxtype(BOX_BUTTON_DOWN), button_down, 1, 1, 2, 2);
  Fl::set_boxtype(Fl_Boxtype(BOX_FIELD), field_box, b, b, 2 * b, 2 * b);
  Fl::set_boxtype(Fl_Boxtype(BOX_CIRCLE), circle_box, 0, 0, 0, 0);
}

// Restyles a stock window (fl_ask.H dialogs, Fl_File_Chooser).
void restyle(Fl_Widget *w, const Theme &t) {
  if (auto *win = dynamic_cast<Fl_Window *>(w)) {
    win->color(t.surface.fl());
  } else if (auto *in = dynamic_cast<Fl_Input_ *>(w)) {
    in->box(Fl_Boxtype(BOX_FIELD));
    in->color(t.surface_alt.fl());
    in->textcolor(t.text.fl());
    in->selection_color(t.accent.fl());
    in->cursor_color(t.accent.fl());
  } else if (auto *br = dynamic_cast<Fl_Browser_ *>(w)) {
    br->box(Fl_Boxtype(BOX_FIELD));
    br->color(t.surface_alt.fl());
    br->textcolor(t.text.fl());
    br->selection_color(t.accent.fl());
  } else if (dynamic_cast<Fl_Check_Button *>(w) || dynamic_cast<Fl_Round_Button *>(w) ||
             dynamic_cast<Fl_Light_Button *>(w)) {
    w->selection_color(t.accent.fl());
    w->labelcolor(t.text.fl());
  } else if (dynamic_cast<Fl_Return_Button *>(w)) {
    auto *rb = static_cast<Fl_Button *>(w);
    rb->box(Fl_Boxtype(BOX_BUTTON));
    rb->down_box(Fl_Boxtype(BOX_BUTTON_DOWN));
    w->color(t.accent.fl());
    w->selection_color(t.accent.mix(t.text, 0.15).fl());
    w->labelcolor(t.on_accent.fl());
  } else if (auto *b = dynamic_cast<Fl_Button *>(w)) {
    b->box(Fl_Boxtype(BOX_BUTTON));
    b->down_box(Fl_Boxtype(BOX_BUTTON_DOWN));
    b->color(t.surface_alt.fl());
    b->selection_color(t.hover.fl());
    b->labelcolor(t.text.fl());
  } else if (auto *mb = dynamic_cast<Fl_Menu_Button *>(w)) {
    mb->box(Fl_Boxtype(BOX_BUTTON));
    mb->color(t.surface_alt.fl());
    mb->labelcolor(t.text.fl());
    mb->textcolor(t.text.fl());
  } else if (auto *ch = dynamic_cast<Fl_Choice *>(w)) {
    ch->color(t.surface_alt.fl());
    ch->textcolor(t.text.fl());
  } else if (auto *box = dynamic_cast<Fl_Box *>(w)) {
    // fl_ask's icon: a thin box with a single bold character.
    const char *l = box->label();
    if (box->box() == FL_THIN_UP_BOX && l && strlen(l) == 1) {
      box->box(Fl_Boxtype(BOX_CIRCLE));
      box->color(t.accent.fl());
      box->labelcolor(t.on_accent.fl());
      box->labelfont(FL_HELVETICA_BOLD);
    } else {
      box->labelcolor(t.text.fl());
    }
  }
  if (auto *g = w->as_group())
    for (int i = 0; i < g->children(); ++i) restyle(g->child(i), t);
}

// ------------------------------------------------------------------ style

class SdeStyle : public Fl_SDE_Style {
 public:
  Fl_Color last_tip_color = 0, last_tip_text = 0;

  void refresh() {
    if (active_theme().font.empty()) reload_config_theme();
    const Theme &t = ui_theme();
    register_boxes();
    // Tooltip colors, unless the program chose its own.
    Fl_Color tip_bg = t.surface.fl(), tip_fg = t.text.fl();
    if (Fl_Tooltip::color() == last_tip_color ||
        Fl_Tooltip::color() == fl_color_cube(FL_NUM_RED - 1, FL_NUM_GREEN - 1, FL_NUM_BLUE - 2))
      Fl_Tooltip::color(tip_bg);
    if (Fl_Tooltip::textcolor() == last_tip_text || Fl_Tooltip::textcolor() == FL_BLACK)
      Fl_Tooltip::textcolor(tip_fg);
    last_tip_color = tip_bg;
    last_tip_text = tip_fg;
    Fl_Tooltip::size(std::max(10, t.font_size - S(1)));
  }

  void menu_begin() override { refresh(); }

  void menu_window_shown(Fl_Window *win) override {
    if (!fl_display || !win->shown()) return;
    Window xid = fl_xid(win);
    int w = win->w(), h = win->h();
    if (w < 4 || h < 4) return;
    int ev, er;
    if (XShapeQueryExtension(fl_display, &ev, &er)) {
      // Rounded corners; compositors draw the shadow along this shape.
      int r = std::min(S(8), std::min(w, h) / 2);
      Pixmap mask = XCreatePixmap(fl_display, xid, w, h, 1);
      GC gc = XCreateGC(fl_display, mask, 0, nullptr);
      XSetForeground(fl_display, gc, 0);
      XFillRectangle(fl_display, mask, gc, 0, 0, w, h);
      XSetForeground(fl_display, gc, 1);
      XFillRectangle(fl_display, mask, gc, r, 0, w - 2 * r, h);
      XFillRectangle(fl_display, mask, gc, 0, r, w, h - 2 * r);
      XFillArc(fl_display, mask, gc, 0, 0, 2 * r, 2 * r, 0, 360 * 64);
      XFillArc(fl_display, mask, gc, w - 2 * r - 1, 0, 2 * r, 2 * r, 0, 360 * 64);
      XFillArc(fl_display, mask, gc, 0, h - 2 * r - 1, 2 * r, 2 * r, 0, 360 * 64);
      XFillArc(fl_display, mask, gc, w - 2 * r - 1, h - 2 * r - 1, 2 * r, 2 * r, 0, 360 * 64);
      XShapeCombineMask(fl_display, xid, ShapeBounding, 0, 0, mask, ShapeSet);
      XFreeGC(fl_display, gc);
      XFreePixmap(fl_display, mask);
    }
    const Theme &t = ui_theme();
    if (t.glass && t.popup_opacity < 0.999) x11::set_opacity(xid, t.popup_opacity);
  }

  int menu_padding() override { return S(5); }
  int menu_item_height(const Fl_Menu_Item *, const Fl_Menu_ *, int label_h) override {
    return std::max(label_h + S(12), S(30));
  }
  int menu_divider_height() override { return S(9); }
  int menu_leading_width(const Fl_Menu_Item *first, const Fl_Menu_ *) override {
    for (const Fl_Menu_Item *m = first; m && m->text; m = m->next())
      if ((m->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)) || item_has_icon(m)) return S(36);
    return S(14);
  }
  int menu_trailing_width() override { return S(26); }
  int menu_shortcut_gap() override { return S(32); }
  int menu_quick_size() override { return S(40); }

  void menu_draw_background(int w, int h) override {
    const Theme &t = ui_theme();
    Painter p(w, h);
    p.fill_rect(0, 0, w, h, t.surface);
    if (t.glass) p.glass_sheen(0, 0, w, std::min(h, S(80)), 0, t.dark);
    p.stroke_rounded(0, 0, w, h, S(8), t.dark ? t.border.mix(t.text, 0.08) : t.border);
  }

  void menu_draw_item(const Fl_Menu_Item *m, const Fl_Menu_ *b, int x, int y, int w, int h,
                      int selected, int lead, int sw) override {
    const Theme &t = ui_theme();
    bool active = m->active();
    bool sel = selected && active;
    Color fg = sel ? t.on_accent : active ? t.text : t.text_dim.alpha(0.7);
    Color dim = sel ? t.on_accent.alpha(0.85) : t.text_dim;
    int trail = menu_trailing_width();
    {
      Painter p(cur_w(), cur_h());
      if (sel) p.fill_rounded(x, y + 1, w, h - 2, Sd(6), t.accent);
      double is = Sd(16), ix = x + (lead - is) / 2.0, iy = y + (h - is) / 2.0;
      if ((m->flags & FL_MENU_TOGGLE) && m->value()) {
        glyph::check(p, ix, iy, is, sel ? t.on_accent : t.accent_text);
      } else if ((m->flags & FL_MENU_RADIO) && m->value()) {
        p.circle(ix + is / 2, iy + is / 2, Sd(3.5), sel ? t.on_accent : t.accent_text);
      } else {
        std::string icon = menu_icon(m);
        if (!icon.empty()) draw_icon(p, icon, ix, iy, is, fg, active ? 1.0 : 0.5);
      }
      if (m->submenu()) {
        double cs = Sd(12);
        glyph::chevron(p, x + w - trail + (trail - cs) / 2.0, y + (h - cs) / 2.0, cs, true, dim);
      }
    }
    int lx = x + lead, lw = w - lead - trail - (sw ? sw + menu_shortcut_gap() : 0);
    fl_sde_draw_menu_label(m, b, lx, y, lw, h, fg.fl(), Fl::event_state(FL_ALT) ? 1 : 0);
    if (m->shortcut_ && !m->submenu()) {
      fl_sde_menu_font(m, b);
      fl_color(dim.fl());
      fl_draw(fl_shortcut_label(m->shortcut_), x + w - trail - sw, y, sw, h, FL_ALIGN_RIGHT);
    }
  }

  void menu_draw_divider(int x, int y, int w, int h) override {
    const Theme &t = ui_theme();
    Painter p(cur_w(), cur_h());
    double yy = std::floor(y + h / 2.0) + 0.5;
    p.line(x + Sd(6), yy, x + w - Sd(6), yy, t.border, 1.0);
  }

  void menu_draw_quick(const Fl_Menu_Item *m, const Fl_Menu_ *, int x, int y, int w, int h,
                       int selected) override {
    const Theme &t = ui_theme();
    bool active = m->active();
    bool sel = selected && active;
    Painter p(cur_w(), cur_h());
    double in = Sd(3);
    if (sel) p.fill_rounded(x + in, y + in, w - 2 * in, h - 2 * in, Sd(6), t.accent);
    double is = Sd(18);
    std::string icon = menu_icon(m);
    Color c = sel ? t.on_accent : t.text;
    if (icon.empty()) {
      // No icon: the first letter of the label.
      p.flush();
      std::string l = strip_mnemonic(m->label());
      text_font(t.font_size, true);
      draw_text(l.substr(0, 1), x, y, w, h, active ? c : t.text_dim.alpha(0.6), 1);
      return;
    }
    draw_icon(p, icon, x + (w - is) / 2.0, y + (h - is) / 2.0, is, c, active ? 1.0 : 0.38);
  }

  void menu_draw_title(const Fl_Menu_Item *m, const Fl_Menu_ *b, int w, int h,
                       int bar) override {
    const Theme &t = ui_theme();
    if (bar) {
      fl_rectf(0, 0, w, h, b ? b->color() : t.background.fl());
      Color bg = from_fl(b ? b->color() : t.background.fl());
      {
        Painter p(w, h);
        Color pill = bg.mix(t.accent, t.dark ? 0.28 : 0.16);
        p.fill_rounded(1, S(3), w - 2, h - S(6), Sd(6), pill);
        double uw = std::min(w - S(16), S(18));
        p.fill_rounded((w - uw) / 2.0, h - S(3) - Sd(3), uw, Sd(2.5), Sd(1.25), t.accent);
      }
      fl_sde_draw_menu_label(m, b, 8, 0, w - 8, h, (b ? b->textcolor() : t.text.fl()),
                             Fl::event_state(FL_ALT) ? 1 : 0);
      return;
    }
    {
      Painter p(w, h);
      p.fill_rect(0, 0, w, h, t.surface);
      p.stroke_rounded(0, 0, w, h, S(6), t.border);
    }
    fl_font(FL_HELVETICA_BOLD, b ? b->textsize() : FL_NORMAL_SIZE);
    fl_color(t.text_dim.fl());
    fl_draw(strip_mnemonic(m->label()).c_str(), S(12), 0, w - S(24), h, FL_ALIGN_LEFT);
  }

  int menubar_draw(Fl_Menu_Bar *b, int hover) override {
    const Theme &t = ui_theme();
    fl_rectf(b->x(), b->y(), b->w(), b->h(), b->color());
    if (!b->menu() || !b->menu()->text) return 1;
    {
      Painter p(cur_w(), cur_h());
      Color bg = from_fl(b->color());
      p.line(b->x(), b->y() + b->h() - 0.5, b->x() + b->w(), b->y() + b->h() - 0.5,
             t.border.alpha(0.6));
      int X = b->x() + 6, i = 0;
      for (const Fl_Menu_Item *m = b->menu()->first(); m->text; m = m->next(), ++i) {
        int W = m->measure(nullptr, b) + 16;
        if (i == hover && b->active_r() && m->active())
          p.fill_rounded(X + 1, b->y() + S(3), W - 2, b->h() - S(6), Sd(6),
                         bg.mix(t.text, t.dark ? 0.12 : 0.07));
        X += W;
      }
    }
    int X = b->x() + 6;
    for (const Fl_Menu_Item *m = b->menu()->first(); m->text; m = m->next()) {
      int W = m->measure(nullptr, b) + 16;
      Fl_Color c = b->textcolor();
      if (!m->active() || !b->active_r()) c = t.text_dim.fl();
      fl_sde_draw_menu_label(m, b, X + 8, b->y(), W - 8, b->h(), c,
                             Fl::event_state(FL_ALT) ? 1 : 0);
      X += W;
    }
    return 1;
  }

  int choice_draw(Fl_Choice *c) override {
    const Theme &t = ui_theme();
    bool hover = Fl::belowmouse() == c && c->active_r();
    bool focus = Fl::focus() == c;
    Color bg = from_fl(c->color());
    if (c->color() == FL_BACKGROUND_COLOR || c->color() == FL_GRAY) bg = t.surface_alt;
    {
      Painter p(cur_w(), cur_h());
      double r = std::min(Sd(6), c->h() / 2.0);
      p.fill_rounded(c->x() + 0.5, c->y() + 0.5, c->w() - 1, c->h() - 1, r,
                     hover ? bg.mix(t.text, 0.05) : bg);
      p.stroke_rounded(c->x(), c->y(), c->w(), c->h(), r,
                       focus ? t.accent : hover ? t.text_dim.alpha(0.6) : t.border,
                       focus ? Sd(1.5) : 1.0);
      // Down chevron.
      double cs = Sd(9), cx = c->x() + c->w() - S(12) - cs / 2, cy = c->y() + c->h() / 2.0;
      cairo_t *cr = p.cr();
      cairo_move_to(cr, cx - cs / 2, cy - cs / 4);
      cairo_line_to(cr, cx, cy + cs / 4);
      cairo_line_to(cr, cx + cs / 2, cy - cs / 4);
      stroke(p, c->active_r() ? t.text_dim : t.text_dim.alpha(0.5), Sd(1.6));
    }
    if (const Fl_Menu_Item *m = c->mvalue()) {
      int xx = c->x() + S(10), ww = c->w() - S(10) - S(28);
      fl_push_clip(c->x() + 1, c->y() + 1, c->w() - 2, c->h() - 2);
      Fl_Color fg = c->textcolor();
      if (!c->active_r() || !m->active()) fg = t.text_dim.fl();
      fl_sde_draw_menu_label(m, c, xx, c->y(), ww, c->h(), fg, 0);
      fl_pop_clip();
    }
    return 1;
  }

  int scrollbar_draw(Fl_Scrollbar *s, int X, int Y, int W, int H, int tx, int ty, int tw,
                     int th, int hover) override {
    const Theme &t = ui_theme();
    Fl_Color bgc = s->parent() ? s->parent()->color() : t.surface.fl();
    if (s->parent() && s->parent()->box() == FL_NO_BOX) bgc = t.surface.fl();
    fl_rectf(X, Y, W, H, bgc);
    if (s->slider_size() >= 1.0 || !s->active_r()) return 1;
    Painter p(cur_w(), cur_h());
    bool horiz = (s->type() & FL_HOR_SLIDER) != 0;
    double across = horiz ? H : W;
    double thick = hover ? std::min(across - Sd(4), Sd(8)) : std::min(across - Sd(6), Sd(5));
    Color thumb = t.text_dim.alpha(hover ? 0.75 : 0.5);
    if (hover) {
      Color track = t.text.alpha(t.dark ? 0.08 : 0.06);
      if (horiz)
        p.fill_rounded(X + Sd(2), Y + (H - thick) / 2, W - Sd(4), thick, thick / 2, track);
      else
        p.fill_rounded(X + (W - thick) / 2, Y + Sd(2), thick, H - Sd(4), thick / 2, track);
    }
    if (horiz)
      p.fill_rounded(tx + Sd(1), Y + (H - thick) / 2, tw - Sd(2), thick, thick / 2, thumb);
    else
      p.fill_rounded(X + (W - thick) / 2, ty + Sd(1), thick, th - Sd(2), thick / 2, thumb);
    return 1;
  }

  int tooltip_margin(int vertical) override { return vertical ? S(5) : S(9); }

  int tooltip_draw(int w, int h, const char *text) override {
    const Theme &t = ui_theme();
    Color bg = from_fl(Fl_Tooltip::color());
    {
      Painter p(w, h);
      p.fill_rect(0, 0, w, h, bg);
      p.stroke_rounded(0, 0, w, h, 0, t.border);
    }
    fl_color(Fl_Tooltip::textcolor());
    fl_font(Fl_Tooltip::font(), Fl_Tooltip::size());
    int mx = tooltip_margin(0), my = tooltip_margin(1);
    fl_draw(text, mx, my, w - 2 * mx, h - 2 * my, Fl_Align(FL_ALIGN_LEFT | FL_ALIGN_WRAP), 0, 1);
    return 1;
  }

  void dialog(Fl_Window *win) override {
    refresh();
    restyle(win, ui_theme());
  }
};

SdeStyle &style() {
  static SdeStyle s;
  return s;
}

// ------------------------------------------------------- text field menus

bool g_input_menus = true;
int (*g_prev_dispatch)(int, Fl_Window *) = nullptr;

Fl_Widget *widget_at(Fl_Group *g, int x, int y) {
  for (int i = g->children() - 1; i >= 0; --i) {
    Fl_Widget *c = g->child(i);
    if (!c->visible() || c->as_window()) continue;
    if (x < c->x() || y < c->y() || x >= c->x() + c->w() || y >= c->y() + c->h()) continue;
    if (Fl_Group *cg = c->as_group())
      if (Fl_Widget *in = widget_at(cg, x, y)) return in;
    return c;
  }
  return nullptr;
}

int dispatch(int event, Fl_Window *w) {
  if (g_input_menus && event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && w &&
      !Fl::grab() && !w->menu_window()) {
    if (auto *in = dynamic_cast<Fl_Input_ *>(widget_at(w, Fl::event_x(), Fl::event_y()))) {
      if (in->active_r() && in->visible_r()) {
        if (Fl::focus() != in) in->take_focus();
        show_input_context_menu(in);
        return 1;
      }
    }
  }
  return g_prev_dispatch ? g_prev_dispatch(event, w) : Fl::handle_(event, w);
}

void action_trampoline(Fl_Widget *, void *data) {
  if (auto *fn = static_cast<ContextMenu::Action *>(data))
    if (*fn) (*fn)();
}

}  // namespace

// ------------------------------------------------------------- public API

void install_ui_style() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  fl_sde_style(&style());
  Fl_Event_Dispatch prev = Fl::event_dispatch();
  if (prev != dispatch) g_prev_dispatch = prev;
  Fl::event_dispatch(dispatch);
}

void set_input_context_menus(bool enabled) { g_input_menus = enabled; }

void set_menu_icon(const Fl_Menu_Item *item, const std::string &icon) {
  if (icon.empty()) icons_by_item().erase(item);
  else icons_by_item()[item] = icon;
}

void set_menu_icon_for_label(const std::string &label, const std::string &icon) {
  icons_by_label()[strip_mnemonic(label.c_str())] = icon;
}

std::string menu_icon(const Fl_Menu_Item *item) {
  if (!item) return "";
  auto it = icons_by_item().find(item);
  if (it != icons_by_item().end()) return it->second;
  if (!icons_by_label().empty() && item->text) {
    auto jt = icons_by_label().find(strip_mnemonic(item->text));
    if (jt != icons_by_label().end()) return jt->second;
  }
  return "";
}

// ------------------------------------------------------------ ContextMenu

struct ContextMenu::Entry {
  enum Kind { Item, Quick, Check, Radio, Submenu } kind = Item;
  std::string label, icon, text;  // text: label with '&' escaped
  Action action;
  int shortcut = 0;
  bool enabled = true, on = false, divider = false;
  std::vector<Entry> children;
};

ContextMenu::ContextMenu() = default;
ContextMenu::ContextMenu(ContextMenu &&) noexcept = default;
ContextMenu &ContextMenu::operator=(ContextMenu &&) noexcept = default;
ContextMenu::~ContextMenu() {
  for (auto &m : built_) icons_by_item().erase(&m);
}

ContextMenu &ContextMenu::quick(const std::string &label, const std::string &icon, Action action,
                                bool enabled) {
  Entry e;
  e.kind = Entry::Quick;
  e.label = label;
  e.icon = icon;
  e.action = std::move(action);
  e.enabled = enabled;
  // Quick actions always come first.
  auto pos = std::find_if(entries_.begin(), entries_.end(),
                          [](const Entry &x) { return x.kind != Entry::Quick; });
  entries_.insert(pos, std::move(e));
  return *this;
}

ContextMenu &ContextMenu::edit_actions(const EditActions &a) {
  if (a.cut) quick(_("Cut"), "cut", a.cut, a.can_cut);
  if (a.copy) quick(_("Copy"), "copy", a.copy, a.can_copy);
  if (a.paste) quick(_("Paste"), "paste", a.paste, a.can_paste);
  if (a.del) quick(_("Delete"), "delete", a.del, a.can_delete);
  if (a.select_all) {
    MenuItemOptions o;
    o.icon = "select-all";
    o.shortcut = FL_CTRL + 'a';
    o.enabled = a.can_select_all;
    item(_("Select All"), a.select_all, o);
  }
  return *this;
}

ContextMenu &ContextMenu::item(const std::string &label, Action action,
                               const MenuItemOptions &opts) {
  Entry e;
  e.label = label;
  e.icon = opts.icon;
  e.action = std::move(action);
  e.shortcut = opts.shortcut;
  e.enabled = opts.enabled;
  e.divider = opts.divider;
  entries_.push_back(std::move(e));
  return *this;
}

ContextMenu &ContextMenu::check(const std::string &label, bool checked, Action action,
                                bool enabled) {
  Entry e;
  e.kind = Entry::Check;
  e.label = label;
  e.on = checked;
  e.action = std::move(action);
  e.enabled = enabled;
  entries_.push_back(std::move(e));
  return *this;
}

ContextMenu &ContextMenu::radio(const std::string &label, bool selected, Action action,
                                bool enabled) {
  Entry e;
  e.kind = Entry::Radio;
  e.label = label;
  e.on = selected;
  e.action = std::move(action);
  e.enabled = enabled;
  entries_.push_back(std::move(e));
  return *this;
}

ContextMenu &ContextMenu::submenu(const std::string &label, ContextMenu &&sub,
                                  const std::string &icon) {
  Entry e;
  e.kind = Entry::Submenu;
  e.label = label;
  e.icon = icon;
  e.children = std::move(sub.entries_);
  entries_.push_back(std::move(e));
  return *this;
}

ContextMenu &ContextMenu::separator() {
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->kind == Entry::Quick) break;
    it->divider = true;
    break;
  }
  return *this;
}

bool ContextMenu::empty() const { return entries_.empty(); }

namespace {
// Labels go to Fl_Menu_Item as C strings; '&', '/' and '\\' are literal.
std::string escape_label(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '&') out += "&&";
    else out += c;
  }
  return out;
}
}  // namespace

void ContextMenu::flatten(std::vector<Fl_Menu_Item> &out) {
  // Stored labels live in |entries_|; they are not copied by FLTK.
  std::function<void(std::vector<Entry> &)> add = [&](std::vector<Entry> &list) {
    for (auto &e : list) {
      Fl_Menu_Item m{};
      e.text = escape_label(e.label);
      m.text = e.text.c_str();
      m.shortcut_ = e.shortcut;
      int flags = 0;
      if (!e.enabled) flags |= FL_MENU_INACTIVE;
      if (e.divider) flags |= FL_MENU_DIVIDER;
      if (e.kind == Entry::Quick) flags |= FL_MENU_SDE_QUICK;
      if (e.kind == Entry::Check) flags |= FL_MENU_TOGGLE | (e.on ? FL_MENU_VALUE : 0);
      if (e.kind == Entry::Radio) flags |= FL_MENU_RADIO | (e.on ? FL_MENU_VALUE : 0);
      if (e.kind == Entry::Submenu) flags |= FL_SUBMENU;
      m.flags = flags;
      if (e.kind != Entry::Submenu && e.action) {
        actions_.push_back(std::make_unique<Action>(e.action));
        m.callback_ = action_trampoline;
        m.user_data_ = actions_.back().get();
      }
      out.push_back(m);
      if (e.kind == Entry::Submenu) {
        add(e.children);
        out.push_back(Fl_Menu_Item{});
      }
    }
  };
  add(entries_);
  out.push_back(Fl_Menu_Item{});
}

void ContextMenu::build() {
  for (auto &m : built_) icons_by_item().erase(&m);
  built_.clear();
  actions_.clear();
  built_.reserve(64);
  flatten(built_);
  // Register icons now that the array has its final addresses.
  size_t i = 0;
  std::function<void(std::vector<Entry> &)> walk = [&](std::vector<Entry> &list) {
    for (auto &e : list) {
      if (!e.icon.empty()) icons_by_item()[&built_[i]] = e.icon;
      ++i;
      if (e.kind == Entry::Submenu) {
        walk(e.children);
        ++i;  // terminator
      }
    }
  };
  walk(entries_);
}

const Fl_Menu_Item *ContextMenu::items() {
  build();
  return built_.data();
}

bool ContextMenu::run(const Fl_Menu_Item *picked) {
  if (!picked || picked->submenu()) return false;
  if (picked->callback_) picked->callback_(nullptr, picked->user_data_);
  return true;
}

bool ContextMenu::popup() { return popup(Fl::event_x(), Fl::event_y()); }

bool ContextMenu::popup(int x, int y) {
  install_ui_style();
  if (entries_.empty()) return false;
  const Fl_Menu_Item *m = items()->popup(x, y);
  return run(m);
}

bool ContextMenu::popup_below(Fl_Widget *anchor) {
  install_ui_style();
  if (entries_.empty() || !anchor) return false;
  const Fl_Menu_Item *m =
      items()->pulldown(anchor->x(), anchor->y(), 0, anchor->h());
  return run(m);
}

void show_input_context_menu(Fl_Input_ *in) {
  if (!in) return;
  bool ro = in->readonly();
  bool sel = in->position() != in->mark();
  bool empty = in->size() == 0;
  Fl_Widget_Tracker alive(in);
  ContextMenu menu;
  EditActions a;
  a.cut = [in, &alive] { if (alive.exists()) { in->copy(1); in->cut(); in->do_callback(); } };
  a.copy = [in, &alive] { if (alive.exists()) in->copy(1); };
  a.paste = [in, &alive] { if (alive.exists()) Fl::paste(*in, 1); };
  a.del = [in, &alive] { if (alive.exists()) { in->cut(); in->do_callback(); } };
  a.select_all = [in, &alive] { if (alive.exists()) in->position(in->size(), 0); };
  a.can_cut = !ro && sel && in->input_type() != FL_SECRET_INPUT;
  a.can_copy = sel && in->input_type() != FL_SECRET_INPUT;
  a.can_paste = !ro && Fl::clipboard_contains(Fl::clipboard_plain_text);
  a.can_delete = !ro && sel;
  a.can_select_all = !empty;
  MenuItemOptions undo;
  undo.icon = "undo";
  undo.shortcut = FL_CTRL + 'z';
  undo.enabled = !ro;
  menu.item(_("Undo"), [in, &alive] { if (alive.exists()) in->undo(); }, undo);
  menu.edit_actions(a);
  menu.popup();
}

// Programs link this file through paint.cpp; install the style at startup.
namespace {
struct AutoInstall {
  AutoInstall() { install_ui_style(); }
} g_auto_install;
}  // namespace

}  // namespace sde
