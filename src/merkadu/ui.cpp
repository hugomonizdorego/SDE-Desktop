// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "buffered_window.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "image_cache.h"
#include "store.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"
#include "wm.h"

namespace sde {
namespace merkadu {

// ------------------------------------------------------------- painting

Color source_color(Source s) {
  switch (s) {
    case Source::Flatpak: return Color::hex(0x3876B9);
    case Source::Snap: return Color::hex(0xC8742F);
    default: return Color::hex(0xC9564E);
  }
}

Color readable(const Color &c, const Color &bg) {
  // Darken (light themes) or lighten (dark themes) until the contrast is 4.5:1.
  bool dark_bg = bg.luminance() < 0.4;
  Color out = c;
  for (int i = 0; i < 10 && out.contrast(bg) < 4.5; ++i)
    out = out.mix(dark_bg ? Color::hex(0xFFFFFF) : Color::hex(0x000000), 0.15);
  return out;
}

int badge_width(const std::string &text) {
  text_font(std::max(S(9), active_theme().font_size - S(2)), true);
  return text_width(text) + S(14);
}

void paint_badge(Painter &p, double x, double y, double w, double h, const Color &c) {
  const Theme &t = active_theme();
  p.fill_rounded(x, y, w, h, h / 2, c.alpha(t.dark ? 0.26 : 0.14));
}

void draw_badge_text(const std::string &text, int x, int y, int w, int h, const Color &c) {
  const Theme &t = active_theme();
  text_font(std::max(S(9), t.font_size - S(2)), true);
  draw_text(text, x, y, w, h, readable(c, t.surface), 1);
}

namespace {

std::vector<Source> badge_sources(const App &a) {
  std::vector<Source> out;
  for (auto &o : a.origins)
    if (std::find(out.begin(), out.end(), o.source) == out.end()) out.push_back(o.source);
  return out;
}

}  // namespace

int paint_source_badges(Painter &p, const App &a, int x, int y, int h, int max_w) {
  int cx = x;
  for (Source s : badge_sources(a)) {
    int w = badge_width(source_label(s));
    if (cx + w - x > max_w) break;
    paint_badge(p, cx, y, w, h, source_color(s));
    cx += w + S(6);
  }
  return cx;
}

void draw_source_badges(const App &a, int x, int y, int h, int max_w) {
  int cx = x;
  for (Source s : badge_sources(a)) {
    std::string label = source_label(s);
    int w = badge_width(label);
    if (cx + w - x > max_w) break;
    draw_badge_text(label, cx, y, w, h, source_color(s));
    cx += w + S(6);
  }
}

void paint_progress(Painter &p, double x, double y, double w, double h, int percent, double phase) {
  const Theme &t = active_theme();
  p.fill_rounded(x, y, w, h, h / 2, t.border.alpha(0.6));
  if (percent >= 0) {
    double fw = std::max(h, w * std::min(100, percent) / 100.0);
    p.fill_rounded(x, y, fw, h, h / 2, t.accent);
  } else {
    // Unknown progress: a segment sliding along.
    double seg = w * 0.3, pos = std::fmod(phase, 1.0) * (w + seg) - seg;
    double sx = std::max(x, x + pos), ex = std::min(x + w, x + pos + seg);
    if (ex > sx) p.fill_rounded(sx, y, ex - sx, h, h / 2, t.accent);
  }
}

void paint_card(Painter &p, double x, double y, double w, double h, bool hot, bool focus) {
  const Theme &t = active_theme();
  double r = std::min(double(t.radius), Sd(12));
  p.fill_rounded(x, y, w, h, r, hot ? t.hover : t.surface);
  p.stroke_rounded(x + 0.5, y + 0.5, w - 1, h - 1, r, hot ? t.accent : t.border);
  if (focus) focus_ring(p, x, y, w, h, r);
}

void paint_symbol(Painter &p, Symbol s, double x, double y, double size, const Color &c) {
  cairo_t *cr = p.cr();
  double u = size / 20.0;
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, u, u);
  p.set(c);
  cairo_set_line_width(cr, 1.6);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  auto rr = [&](double rx, double ry, double rw, double rh, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, rx + rw - r, ry + r, r, -M_PI / 2, 0);
    cairo_arc(cr, rx + rw - r, ry + rh - r, r, 0, M_PI / 2);
    cairo_arc(cr, rx + r, ry + rh - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, rx + r, ry + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
  };
  switch (s) {
    case Symbol::Explore:  // a compass
      cairo_arc(cr, 10, 10, 7.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 13.2, 6.8);
      cairo_line_to(cr, 11.2, 11.2);
      cairo_line_to(cr, 6.8, 13.2);
      cairo_line_to(cr, 8.8, 8.8);
      cairo_close_path(cr);
      cairo_fill(cr);
      break;
    case Symbol::Installed:  // a box with a check
      rr(3, 5, 14, 12, 2);
      cairo_stroke(cr);
      cairo_move_to(cr, 3, 8.5);
      cairo_line_to(cr, 17, 8.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 7.5, 12.6);
      cairo_line_to(cr, 9.4, 14.4);
      cairo_line_to(cr, 12.8, 10.8);
      cairo_stroke(cr);
      break;
    case Symbol::Updates:
    case Symbol::Refresh:  // circular arrows
      cairo_arc(cr, 10, 10, 6.5, -M_PI * 0.15, M_PI * 0.75);
      cairo_stroke(cr);
      cairo_arc(cr, 10, 10, 6.5, M_PI * 0.85, M_PI * 1.75);
      cairo_stroke(cr);
      cairo_move_to(cr, 16.5, 4.2);
      cairo_line_to(cr, 15.9, 8.1);
      cairo_line_to(cr, 12.2, 7.0);
      cairo_stroke(cr);
      cairo_move_to(cr, 3.5, 15.8);
      cairo_line_to(cr, 4.1, 11.9);
      cairo_line_to(cr, 7.8, 13.0);
      cairo_stroke(cr);
      break;
    case Symbol::History:
    case Symbol::Clock:
      cairo_arc(cr, 10, 10, 7.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 10, 5.5);
      cairo_line_to(cr, 10, 10);
      cairo_line_to(cr, 13, 12);
      cairo_stroke(cr);
      break;
    case Symbol::Progress:
    case Symbol::Download:  // an arrow into a tray
      cairo_move_to(cr, 10, 3);
      cairo_line_to(cr, 10, 12.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 6, 8.8);
      cairo_line_to(cr, 10, 12.8);
      cairo_line_to(cr, 14, 8.8);
      cairo_stroke(cr);
      cairo_move_to(cr, 3.5, 13);
      cairo_line_to(cr, 3.5, 16.5);
      cairo_line_to(cr, 16.5, 16.5);
      cairo_line_to(cr, 16.5, 13);
      cairo_stroke(cr);
      break;
    case Symbol::Sources:  // stacked layers
      cairo_move_to(cr, 10, 3);
      cairo_line_to(cr, 17.5, 7);
      cairo_line_to(cr, 10, 11);
      cairo_line_to(cr, 2.5, 7);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 2.5, 10.5);
      cairo_line_to(cr, 10, 14.5);
      cairo_line_to(cr, 17.5, 10.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 2.5, 14);
      cairo_line_to(cr, 10, 18);
      cairo_line_to(cr, 17.5, 14);
      cairo_stroke(cr);
      break;
    case Symbol::Search:
      cairo_arc(cr, 8.5, 8.5, 5.5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 12.6, 12.6);
      cairo_line_to(cr, 17, 17);
      cairo_stroke(cr);
      break;
    case Symbol::Back:
      cairo_move_to(cr, 12.5, 4);
      cairo_line_to(cr, 6.5, 10);
      cairo_line_to(cr, 12.5, 16);
      cairo_stroke(cr);
      break;
    case Symbol::Check:
      cairo_move_to(cr, 4.5, 10.5);
      cairo_line_to(cr, 8.5, 14.5);
      cairo_line_to(cr, 15.5, 6);
      cairo_stroke(cr);
      break;
    case Symbol::Plus:
    case Symbol::Minus:
      cairo_move_to(cr, 4.5, 10);
      cairo_line_to(cr, 15.5, 10);
      cairo_stroke(cr);
      if (s == Symbol::Plus) {
        cairo_move_to(cr, 10, 4.5);
        cairo_line_to(cr, 10, 15.5);
        cairo_stroke(cr);
      }
      break;
    case Symbol::Warning:
      cairo_move_to(cr, 10, 3);
      cairo_line_to(cr, 18, 17);
      cairo_line_to(cr, 2, 17);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 10, 8);
      cairo_line_to(cr, 10, 12);
      cairo_stroke(cr);
      cairo_arc(cr, 10, 14.6, 0.9, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    case Symbol::Trash:
      cairo_move_to(cr, 3.5, 5.5);
      cairo_line_to(cr, 16.5, 5.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 8, 5.5);
      cairo_line_to(cr, 8, 3.5);
      cairo_line_to(cr, 12, 3.5);
      cairo_line_to(cr, 12, 5.5);
      cairo_stroke(cr);
      cairo_move_to(cr, 5, 5.5);
      cairo_line_to(cr, 6, 17);
      cairo_line_to(cr, 14, 17);
      cairo_line_to(cr, 15, 5.5);
      cairo_stroke(cr);
      break;
    case Symbol::Shield:
      cairo_move_to(cr, 10, 2.5);
      cairo_line_to(cr, 16.5, 5);
      cairo_curve_to(cr, 16.5, 11, 14, 15.5, 10, 17.5);
      cairo_curve_to(cr, 6, 15.5, 3.5, 11, 3.5, 5);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Symbol::Folder:
      cairo_move_to(cr, 2.5, 6);
      cairo_line_to(cr, 2.5, 15.5);
      cairo_line_to(cr, 17.5, 15.5);
      cairo_line_to(cr, 17.5, 7);
      cairo_line_to(cr, 9.5, 7);
      cairo_line_to(cr, 8, 4.5);
      cairo_line_to(cr, 2.5, 4.5);
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Symbol::Star:
      for (int i = 0; i < 10; ++i) {
        double a = -M_PI / 2 + i * M_PI / 5, r = i % 2 ? 3.4 : 7.6;
        if (i == 0)
          cairo_move_to(cr, 10 + r * std::cos(a), 10.6 + r * std::sin(a));
        else
          cairo_line_to(cr, 10 + r * std::cos(a), 10.6 + r * std::sin(a));
      }
      cairo_close_path(cr);
      cairo_stroke(cr);
      break;
    case Symbol::Close:
      cairo_move_to(cr, 5, 5);
      cairo_line_to(cr, 15, 15);
      cairo_move_to(cr, 15, 5);
      cairo_line_to(cr, 5, 15);
      cairo_stroke(cr);
      break;
  }
  cairo_restore(cr);
}

cairo_surface_t *app_icon(const App &a, int size) {
  std::string fallback = a.kind == "font" ? "font-x-generic" : "application-x-executable";
  return images::icon(a.icon, size, fallback);
}

// --------------------------------------------------------------- button

Button::Button(int x, int y, int w, int h, const std::string &text, Kind kind)
    : Fl_Button(x, y, w, h), text_(text), kind_(kind) {
  box(FL_NO_BOX);
  down_box(FL_NO_BOX);
  copy_tooltip(text.c_str());
  callback([](Fl_Widget *w, void *) {
    auto *b = static_cast<Button *>(w);
    if (b->on_click) b->on_click();
  });
}

void Button::text(const std::string &t) {
  text_ = t;
  copy_tooltip(t.c_str());
  redraw();
}

int Button::fit(const std::string &text) {
  text_font(active_theme().font_size, true);
  return std::max(S(84), text_width(text) + S(32));
}

void Button::draw() {
  const Theme &t = active_theme();
  bool hot = Fl::belowmouse() == this && active_r(), down = value() != 0;
  bool strong = kind_ == Primary || kind_ == Danger;
  Color accent = kind_ == Danger ? t.error : t.accent;
  {
    Painter p(window()->w(), window()->h());
    double r = t.radius_for(h());
    Color bg = strong ? accent : kind_ == Ghost ? t.surface.alpha(0) : t.surface_alt;
    if (hot) bg = strong ? accent.mix(t.text, 0.12) : t.hover;
    if (down) bg = bg.mix(t.text, 0.15);
    if (!active_r()) bg = t.surface_alt.mix(t.surface, 0.5);
    if (bg.a > 0) p.fill_rounded(x(), y(), w(), h(), r, bg);
    if (!strong && kind_ != Ghost) p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.border);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  text_font(t.font_size, strong);
  Color fg = strong ? (kind_ == Danger ? Color::hex(0xFFFFFF) : t.on_accent) : t.text;
  if (kind_ == Ghost) fg = t.accent_text;
  if (!active_r()) fg = t.text_dim;
  draw_text(text_, x() + S(10), y(), w() - S(20), h(), fg, 1);
}

int Button::handle(int e) {
  if (e == FL_ENTER || e == FL_LEAVE || e == FL_FOCUS || e == FL_UNFOCUS) {
    redraw();
    return 1;
  }
  if (e == FL_KEYBOARD && Fl::focus() == this &&
      (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter || Fl::event_key() == ' ')) {
    do_callback();
    return 1;
  }
  return Fl_Button::handle(e);
}

// ----------------------------------------------------------------- chip

Chip::Chip(int x, int y, int w, int h, const std::string &text, bool on)
    : Fl_Widget(x, y, w, h), dot(Color::hex(0, 0)), text_(text), on_(on) {
  copy_tooltip(text.c_str());
}

int Chip::fit(const std::string &text) {
  text_font(active_theme().font_size, false);
  return text_width(text) + S(30);
}

void Chip::draw() {
  const Theme &t = active_theme();
  bool hot = Fl::belowmouse() == this;
  {
    Painter p(window()->w(), window()->h());
    double r = h() / 2.0;
    Color bg = on_ ? t.accent.alpha(t.dark ? 0.30 : 0.16) : hot ? t.hover : t.surface;
    p.fill_rounded(x(), y(), w(), h(), r, bg);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, on_ ? t.accent : t.border);
    if (dot.a > 0) p.circle(x() + S(14), y() + h() / 2.0, Sd(4), dot);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  text_font(t.font_size, on_);
  int dx = dot.a > 0 ? S(10) : 0;
  draw_text(text_, x() + S(10) + dx, y(), w() - S(20) - dx, h(), on_ ? t.accent_text : t.text, 1);
}

int Chip::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      return 1;
    case FL_RELEASE:
      if (Fl::event_inside(this) && on_click) on_click();
      return 1;
    case FL_KEYBOARD:
      if ((Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) && on_click) {
        on_click();
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// -------------------------------------------------------------- AppTile

AppTile::AppTile(int x, int y, int w, int h, const std::string &app_id) : Fl_Widget(x, y, w, h), id_(app_id) {
  if (const App *a = Store::get().app(app_id)) copy_tooltip(a->name.c_str());
}

void AppTile::draw() {
  const Theme &t = active_theme();
  const App *a = Store::get().app(id_);
  if (!a) return;
  int is = S(48), pad = S(14);
  int tx = x() + pad + is + S(14), tw = w() - (tx - x()) - pad;
  int badge_h = S(20), by = y() + h() - pad - badge_h + S(2);
  {
    Painter p(window()->w(), window()->h());
    paint_card(p, x(), y(), w(), h(), hot_, Fl::focus() == this);
    p.image(app_icon(*a, is), x() + pad, y() + pad, is);
    paint_source_badges(p, *a, tx, by, badge_h, tw - S(24));
    if (a->installed()) {
      double cs = S(18);
      p.circle(x() + w() - S(14) - cs / 2, y() + S(14) + cs / 2, cs / 2, t.success.alpha(0.9));
      paint_symbol(p, Symbol::Check, x() + w() - S(14) - cs + S(2), y() + S(14) + S(2), cs - S(4),
                   Color::hex(0xFFFFFF));
    }
  }
  int name_w = tw - (a->installed() ? S(24) : 0);
  text_font(t.font_size + 1, true);
  draw_text(a->name, tx, y() + S(10), name_w, S(22), t.text);
  text_font(std::max(S(9), t.font_size - S(1)));
  fl_color(t.text_dim.fl());
  int sh = by - (y() + S(34)) - S(2);
  fl_push_clip(tx, y() + S(34), tw, std::max(0, sh));
  fl_draw(a->summary.c_str(), tx, y() + S(34), tw, sh,
          Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  fl_pop_clip();
  draw_source_badges(*a, tx, by, badge_h, tw - S(24));
}

int AppTile::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
      hot_ = e == FL_ENTER;
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      return 1;
    case FL_RELEASE:
      if (Fl::event_inside(this) && on_open) on_open(id_);
      return 1;
    case FL_KEYBOARD:
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == ' ' || Fl::event_key() == FL_KP_Enter) && on_open) {
        on_open(id_);
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// --------------------------------------------------------------- AppRow

AppRow::AppRow(int x, int y, int w, int h, const std::string &app_id, Mode mode)
    : Fl_Widget(x, y, w, h), id_(app_id), mode_(mode) {
  if (const App *a = Store::get().app(app_id)) copy_tooltip(a->name.c_str());
}

std::string AppRow::action_text() const {
  const App *a = Store::get().app(id_);
  if (!a) return "";
  if (Store::get().active_job(id_)) return "";
  switch (mode_) {
    case Installed: {
      const Origin *o = a->installed_origin();
      if (o && o->source == Source::Debian && Store::get().settings().protected_packages.count(o->id))
        return _("Part of SDE");
      return _("Remove");
    }
    case Updates:
      return _("Update");
    default:
      if (a->installed()) return _("Installed");
      return _("Install");
  }
}

void AppRow::action_box(int *bx, int *by, int *bw, int *bh) const {
  std::string text = action_text();
  *bw = text.empty() ? S(150) : Button::fit(text);
  *bh = S(32);
  *bx = x() + w() - *bw - S(14);
  *by = y() + (h() - *bh) / 2;
}

void AppRow::draw() {
  const Theme &t = active_theme();
  const App *a = Store::get().app(id_);
  if (!a) return;
  const Job *job = Store::get().active_job(id_);
  std::string action = action_text();
  bool can_act = !action.empty() && action != _("Installed") && action != _("Part of SDE");
  bool primary = mode_ != Installed && can_act;
  int bx, by, bw, bh;
  action_box(&bx, &by, &bw, &bh);
  int is = S(40), ix = x() + S(14), tx = ix + is + S(14), tw = bx - tx - S(14);
  text_font(t.font_size + 1, true);
  int name_w = std::min(tw, text_width(a->name) + S(4));
  {
    Painter p(window()->w(), window()->h());
    paint_card(p, x(), y(), w(), h(), hot_, Fl::focus() == this);
    p.image(app_icon(*a, is), ix, y() + (h() - is) / 2.0, is);
    paint_source_badges(p, *a, tx + name_w + S(8), y() + h() / 2 - S(20), S(18), tw - name_w - S(8));
    if (job) {
      paint_progress(p, bx, by + bh / 2.0 + S(4), bw, S(6), job->percent, 0);
    } else if (!action.empty()) {
      Color accent = mode_ == Installed ? t.error : t.accent;
      double r = t.radius_for(bh);
      if (!can_act) {
        // A label, not a button.
      } else if (primary) {
        p.fill_rounded(bx, by, bw, bh, r, hot_button_ ? accent.mix(t.text, 0.12) : accent);
      } else {
        p.fill_rounded(bx, by, bw, bh, r, hot_button_ ? t.hover : t.surface_alt);
        p.stroke_rounded(bx + 0.5, by + 0.5, bw - 1, bh - 1, r, accent.alpha(0.7));
      }
    }
  }
  text_font(t.font_size + 1, true);
  draw_text(a->name, tx, y() + h() / 2 - S(22), name_w, S(22), t.text);
  draw_source_badges(*a, tx + name_w + S(8), y() + h() / 2 - S(20), S(18), tw - name_w - S(8));
  text_font(std::max(S(9), t.font_size - S(1)));
  std::string line2 = a->summary;
  if (mode_ == Installed) {
    if (const Origin *o = a->installed_origin()) {
      line2 = origin_line(*o);
      if (o->installed_size > 0) line2 += "  \xC2\xB7  " + format_size(o->installed_size);
    }
  } else if (mode_ == Updates) {
    for (auto &o : a->origins)
      if (o.installed && o.update) {
        line2 = origin_line(o);
        if (!o.installed_version.empty() && !o.version.empty())
          line2 += "  \xC2\xB7  " + o.installed_version + " \xE2\x86\x92 " + o.version;
      }
  }
  draw_text(line2, tx, y() + h() / 2 + S(1), tw, S(20), t.text_dim);
  if (job) {
    text_font(std::max(S(9), t.font_size - S(1)));
    std::string st = job->state == Job::Queued ? std::string(_("Waiting")) : job->status;
    if (job->percent >= 0 && job->state == Job::Running) st += format(" %d%%", job->percent);
    draw_text(st, bx, by - S(4), bw, S(20), t.text_dim, 1);
  } else if (!action.empty()) {
    text_font(t.font_size, primary);
    Color fg = primary ? t.on_accent : can_act ? (mode_ == Installed ? readable(t.error, t.surface) : t.text)
                                               : t.text_dim;
    draw_text(action, bx + S(8), by, bw - S(16), bh, fg, can_act ? 1 : 2);
  }
}

int AppRow::handle(int e) {
  int bx, by, bw, bh;
  action_box(&bx, &by, &bw, &bh);
  auto in_button = [&] {
    int ex = Fl::event_x(), ey = Fl::event_y();
    return ex >= bx && ex < bx + bw && ey >= by && ey < by + bh;
  };
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
      hot_ = e == FL_ENTER;
      hot_button_ = hot_ && in_button();
      redraw();
      return 1;
    case FL_MOVE: {
      bool b = in_button();
      if (b != hot_button_) {
        hot_button_ = b;
        redraw();
      }
      return 1;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      return 1;
    case FL_RELEASE: {
      if (!Fl::event_inside(this)) return 1;
      std::string action = action_text();
      bool can_act = !action.empty() && action != _("Installed") && action != _("Part of SDE");
      if (in_button() && can_act && on_action)
        on_action(id_);
      else if (on_open)
        on_open(id_);
      return 1;
    }
    case FL_KEYBOARD:
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) && on_open) {
        on_open(id_);
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// --------------------------------------------------------------- Banner

Banner::Banner(int x, int y, int w, int h, const std::string &app_id) : Fl_Widget(x, y, w, h), id_(app_id) {
  if (const App *a = Store::get().app(app_id)) copy_tooltip(a->name.c_str());
}

void Banner::draw() {
  const Theme &t = active_theme();
  const App *a = Store::get().app(id_);
  if (!a) return;
  double r = t.radius_large;
  int pad = S(28), is = std::min(S(96), h() - 2 * pad);
  bool shot = !a->screenshots.empty() && w() > S(620);
  int shot_w = shot ? std::min(int(w() * 0.42), int((h() - S(36)) * 16 / 9.0)) : 0;
  int tx = x() + pad + is + S(24), tw = w() - (tx - x()) - pad - (shot ? shot_w + S(24) : 0);
  {
    Painter p(window()->w(), window()->h());
    Color base = t.surface;
    p.fill_gradient(x(), y(), w(), h(), r, t.accent.alpha(t.dark ? 0.34 : 0.20).mix(base, 0.25),
                    base.alpha(t.dark ? 0.9 : 1.0), true);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, hot_ ? t.accent : t.border);
    p.image(app_icon(*a, is), x() + pad, y() + (h() - is) / 2.0, is);
    if (shot) {
      int sh = int(shot_w * 9 / 16.0), sx = x() + w() - pad - shot_w, sy = y() + (h() - sh) / 2;
      cairo_surface_t *img = images::picture(a->screenshots[0].thumbnail, shot_w, sh);
      p.shadow(sx, sy, shot_w, sh, S(8), t.shadow.alpha(std::min(0.3, t.shadow.a)), S(6));
      p.fill_rounded(sx, sy, shot_w, sh, S(8), t.surface_alt);
      if (img) {
        double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
        cairo_save(p.cr());
        p.rounded_rect(sx, sy, shot_w, sh, S(8));
        cairo_clip(p.cr());
        p.surface(img, sx + (shot_w - iw) / 2, sy + (sh - ih) / 2);
        cairo_restore(p.cr());
      }
      p.stroke_rounded(sx + 0.5, sy + 0.5, shot_w - 1, sh - 1, S(8), t.border);
    }
    paint_source_badges(p, *a, tx, y() + h() / 2 + S(34), S(22), tw);
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
  }
  text_font(std::max(S(9), t.font_size - S(1)), true);
  draw_text(_("FEATURED"), tx, y() + h() / 2 - S(58), tw, S(20), t.accent_text);
  text_font(t.font_size + S(10), true);
  draw_text(a->name, tx, y() + h() / 2 - S(38), tw, S(38), t.text);
  text_font(t.font_size + 1);
  fl_color(t.text_dim.fl());
  fl_draw(a->summary.c_str(), tx, y() + h() / 2 + S(2), tw, S(30),
          Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  draw_source_badges(*a, tx, y() + h() / 2 + S(34), S(22), tw);
}

int Banner::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
      hot_ = e == FL_ENTER;
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      return 1;
    case FL_RELEASE:
      if (Fl::event_inside(this) && on_open) on_open(id_);
      return 1;
    case FL_KEYBOARD:
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == ' ') && on_open) {
        on_open(id_);
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// --------------------------------------------------------- CategoryTile

CategoryTile::CategoryTile(int x, int y, int w, int h, const std::string &title, const std::string &icon, int count,
                           bool compact)
    : Fl_Widget(x, y, w, h), title_(title), icon_(icon), count_(count), compact_(compact) {
  copy_tooltip(title.c_str());
}

void CategoryTile::draw() {
  const Theme &t = active_theme();
  int box = compact_ ? S(32) : S(40), bx = x() + S(12), by = y() + (h() - box) / 2;
  {
    Painter p(window()->w(), window()->h());
    paint_card(p, x(), y(), w(), h(), hot_, Fl::focus() == this);
    p.fill_rounded(bx, by, box, box, Sd(10), t.accent.alpha(0.12));
    int is = compact_ ? S(22) : S(26);
    p.image(IconLoader::get({icon_, "applications-other", "folder"}, is), bx + (box - is) / 2.0,
            by + (box - is) / 2.0, is);
  }
  int tx = bx + box + S(12), tw = w() - (tx - x()) - S(10);
  text_font(t.font_size, true);
  draw_text(title_, tx, y() + h() / 2 - S(20), tw, S(20), t.text);
  text_font(std::max(S(9), t.font_size - S(1)));
  draw_text(count_ >= 0 ? apps_count(size_t(count_)) : std::string(), tx, y() + h() / 2 + S(1), tw, S(18),
            t.text_dim);
}

int CategoryTile::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_LEAVE:
      hot_ = e == FL_ENTER;
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      return 1;
    case FL_RELEASE:
      if (Fl::event_inside(this) && on_open) on_open();
      return 1;
    case FL_KEYBOARD:
      if ((Fl::event_key() == FL_Enter || Fl::event_key() == ' ') && on_open) {
        on_open();
        return 1;
      }
      return 0;
    default:
      return Fl_Widget::handle(e);
  }
}

// ------------------------------------------------------ ScreenshotStrip

ScreenshotStrip::ScreenshotStrip(int x, int y, int w, int h, const std::vector<Screenshot> &shots)
    : Fl_Widget(x, y, w, h), shots_(shots) {
  tooltip(_("Screenshots"));
}

int ScreenshotStrip::preferred_height(int w) {
  // One large screenshot and the next ones beside it on wide pages.
  return std::min(S(300), std::max(S(140), int(w * 0.62 * 9 / 16)));
}

int ScreenshotStrip::shot_at(int ex) const {
  int sh = h(), sw = sh * 16 / 9, gap = S(12);
  for (size_t i = 0; i < shots_.size(); ++i) {
    int sx = x() + int(i) * (sw + gap);
    if (sx + sw > x() + w()) break;
    if (ex >= sx && ex < sx + sw) return int(i);
  }
  return -1;
}

void ScreenshotStrip::draw() {
  const Theme &t = active_theme();
  int sh = h(), sw = sh * 16 / 9, gap = S(12);
  Painter p(window()->w(), window()->h());
  for (size_t i = 0; i < shots_.size(); ++i) {
    int sx = x() + int(i) * (sw + gap);
    if (sx + sw > x() + w()) break;
    double r = S(10);
    p.fill_rounded(sx, y(), sw, sh, r, t.surface_alt);
    cairo_surface_t *img = images::picture(shots_[i].thumbnail, sw, sh);
    if (img) {
      double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
      cairo_save(p.cr());
      p.rounded_rect(sx, y(), sw, sh, r);
      cairo_clip(p.cr());
      p.surface(img, sx + (sw - iw) / 2, y() + (sh - ih) / 2);
      cairo_restore(p.cr());
    } else {
      // Loading or missing: a picture symbol.
      double s = S(28);
      paint_symbol(p, Symbol::Folder, sx + (sw - s) / 2, y() + (sh - s) / 2, s, t.text_dim.alpha(0.5));
    }
    p.stroke_rounded(sx + 0.5, y() + 0.5, sw - 1, sh - 1, r, int(i) == hover_ ? t.accent : t.border);
  }
}

int ScreenshotStrip::handle(int e) {
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int h2 = shot_at(Fl::event_x());
      if (h2 != hover_) {
        hover_ = h2;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH:
      return Fl::event_button() == FL_LEFT_MOUSE ? 1 : 0;
    case FL_RELEASE: {
      int i = shot_at(Fl::event_x());
      if (i >= 0 && on_open) on_open(i);
      return 1;
    }
    default:
      return Fl_Widget::handle(e);
  }
}

// ---------------------------------------------------------- SearchField

namespace {

// The rounded field behind the search input, with its magnifier; a click
// on it (outside the text) types into the input.
class SearchFrame : public Fl_Widget {
 public:
  SearchFrame(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  Fl_Widget *input = nullptr;
  void draw() override {
    const Theme &t = active_theme();
    bool focused = input && Fl::focus() == input;
    Painter p(window()->w(), window()->h());
    double r = h() / 2.0;
    p.fill_rounded(x(), y(), w(), h(), r, t.surface);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, focused ? t.accent : t.border,
                     focused ? Sd(2) : Sd(1));
    paint_symbol(p, Symbol::Search, x() + S(13), y() + (h() - S(16)) / 2.0, S(16), t.text_dim);
  }
  int handle(int e) override {
    if (e == FL_PUSH && input) {
      input->take_focus();
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

}  // namespace

SearchField::SearchField(int x, int y, int w, int h, const std::string &placeholder)
    : Fl_Input(x + S(36), y + S(4), w - S(50), h - S(8)), placeholder_(placeholder) {
  // The frame is created first so that it is drawn below the input.
  restyle();
  when(FL_WHEN_CHANGED);
  tooltip(_("Search"));
}

SearchField *make_search_field(int x, int y, int w, int h, const std::string &placeholder) {
  auto *frame = new SearchFrame(x, y, w, h);
  auto *input = new SearchField(x, y, w, h, placeholder);
  frame->input = input;
  input->frame = frame;
  return input;
}

void SearchField::restyle() {
  const Theme &t = active_theme();
  box(FL_FLAT_BOX);
  color(t.surface.fl());
  textcolor(t.text.fl());
  cursor_color(t.accent.fl());
  selection_color(t.accent.fl());
  textsize(t.font_size + 1);
}

void SearchField::draw() {
  Fl_Input::draw();
  if (size() == 0 && Fl::focus() != this) {
    const Theme &t = active_theme();
    text_font(t.font_size + 1);
    draw_text(placeholder_, x() + S(3), y(), w() - S(6), h(), t.text_dim);
  }
}

int SearchField::handle(int e) {
  if ((e == FL_FOCUS || e == FL_UNFOCUS) && frame) frame->redraw();
  if (e == FL_KEYBOARD) {
    int k = Fl::event_key();
    if ((k == FL_Enter || k == FL_KP_Enter) && on_enter) {
      on_enter();
      return 1;
    }
    if (k == FL_Escape) {
      if (size() > 0) {
        value("");
        do_callback();
      } else if (on_escape) {
        on_escape();
      }
      return 1;
    }
  }
  return Fl_Input::handle(e);
}

// ------------------------------------------------------------ TextBlock

TextBlock::TextBlock(int x, int y, int w, int h, const std::string &text, bool dim, int size_delta)
    : Fl_Widget(x, y, w, h), text_(text), dim_(dim), delta_(size_delta) {}

int TextBlock::measure(const std::string &text, int w, int size_delta) {
  text_font(active_theme().font_size + size_delta);
  int ww = w, hh = 0;
  fl_measure(text.c_str(), ww, hh, 0);
  return hh;
}

void TextBlock::draw() {
  const Theme &t = active_theme();
  text_font(t.font_size + delta_);
  fl_color((dim_ ? t.text_dim : t.text).fl());
  fl_draw(text_.c_str(), x(), y(), w(), h(), Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE),
          nullptr, 0);
}

// ------------------------------------------------------ screenshot view

namespace {

class ShotWindow : public BufferedWindow {
 public:
  ShotWindow(int w, int h, const std::vector<Screenshot> &shots, int index, const std::string &title)
      : BufferedWindow(w, h), shots_(shots), index_(index), title_(title) {
    update_label();
  }
  void update_label() {
    label_ = shots_.size() > 1 ? format(_("%s \xE2\x80\x94 screenshot %d of %d"), title_.c_str(), index_ + 1,
                                        int(shots_.size()))
                               : format(_("%s \xE2\x80\x94 screenshot"), title_.c_str());
    label(label_.c_str());
  }
  void draw() override {
    const Theme &t = active_theme();
    Painter p(w(), h());
    p.fill_rect(0, 0, w(), h(), t.background);
    const Screenshot &s = shots_[size_t(index_)];
    cairo_surface_t *img = images::picture(s.image, w() - S(24), h() - S(24));
    if (!img) img = images::picture(s.thumbnail, w() - S(24), h() - S(24));
    if (img) {
      double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
      p.surface(img, (w() - iw) / 2, (h() - ih) / 2);
    } else {
      double sz = S(48);
      paint_symbol(p, Symbol::Folder, (w() - sz) / 2, (h() - sz) / 2, sz, t.text_dim);
    }
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key(), n = int(shots_.size());
      if (k == FL_Escape) {
        hide();
        return 1;
      }
      if ((k == FL_Right || k == ' ' || k == FL_Page_Down) && n > 1) index_ = (index_ + 1) % n;
      else if ((k == FL_Left || k == FL_BackSpace || k == FL_Page_Up) && n > 1) index_ = (index_ + n - 1) % n;
      else return BufferedWindow::handle(e);
      update_label();
      redraw();
      return 1;
    }
    if (e == FL_PUSH) {
      int n = int(shots_.size());
      if (n > 1) {
        index_ = (index_ + (Fl::event_x() < w() / 3 ? n - 1 : 1)) % n;
        update_label();
        redraw();
      }
      return 1;
    }
    return BufferedWindow::handle(e);
  }

 private:
  std::vector<Screenshot> shots_;
  int index_;
  std::string title_, label_;
};

}  // namespace

void show_screenshot(const std::vector<Screenshot> &shots, int index, const std::string &title) {
  if (shots.empty()) return;
  int x = 0, y = 0, w = S(1100), h = int(S(1100) * 9 / 16.0) + S(24);
  fit_to_work_area(x, y, w, h);
  auto *win = new ShotWindow(w, h, shots, std::max(0, std::min(index, int(shots.size()) - 1)), title);
  win->xclass("merkadu");
  win->set_non_modal();
  win->position(x, y);
  win->callback([](Fl_Widget *w, void *) {
    w->hide();
    Fl::delete_widget(w);
  });
  win->show();
}

}  // namespace merkadu
}  // namespace sde
