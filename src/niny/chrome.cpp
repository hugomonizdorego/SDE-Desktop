// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny chrome: tab strip in the header, status pills, notice bar and the
// start page shown when no document is open.
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "i18n.h"
#include "niny.h"
#include "textops.h"
#include "util.h"

using namespace sde;

namespace niny {

// ------------------------------------------------------------------ Field

int Field::handle(int event) {
  if (event == FL_KEYBOARD && on_key) {
    int r = on_key(Fl::event_key(), Fl::event_state());
    if (r) return r;
  }
  if (event == FL_FOCUS || event == FL_UNFOCUS) {
    if (parent()) parent()->redraw();
  }
  return Fl_Input::handle(event);
}

// ----------------------------------------------------------------- TabBar

TabBar::TabBar(App *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

std::vector<TabBar::Tab> TabBar::layout() const {
  const auto &docs = app_->docs();
  std::vector<Tab> tabs(docs.size());
  if (docs.empty()) return tabs;
  text_font(look().theme.font_size);
  double avail = w() - S(40);
  std::vector<double> want(docs.size());
  double total = 0;
  for (size_t i = 0; i < docs.size(); ++i) {
    want[i] = std::max<double>(S(118), std::min<double>(S(230), text_width(docs[i]->display_name()) + S(76)));
    total += want[i] + S(4);
  }
  if (total > avail) {
    double each = std::max<double>(S(96), avail / docs.size() - S(4));
    for (auto &wv : want) wv = std::min(wv, each);
  }
  double x0 = x() - scroll_;
  for (size_t i = 0; i < docs.size(); ++i) {
    tabs[i].x = x0;
    tabs[i].w = want[i];
    x0 += want[i] + S(4);
  }
  return tabs;
}

double TabBar::plus_x() const {
  auto t = layout();
  double px = t.empty() ? x() : t.back().x + t.back().w + S(6);
  return std::min<double>(px, x() + w() - S(32));
}

int TabBar::tab_at(int mx) const {
  auto t = layout();
  for (size_t i = 0; i < t.size(); ++i)
    if (mx >= t[i].x && mx < t[i].x + t[i].w && mx < x() + w() - S(36)) return int(i);
  return -1;
}

bool TabBar::on_close(int index, int mx, int my) const {
  auto t = layout();
  if (index < 0 || index >= int(t.size())) return false;
  double cx = t[size_t(index)].x + t[size_t(index)].w - S(26);
  double cy = y() + (h() - S(20)) / 2.0 + S(2);
  return mx >= cx && mx < cx + S(20) && my >= cy && my < cy + S(20);
}

void TabBar::ensure_visible(int index) {
  auto t = layout();
  if (index < 0 || index >= int(t.size())) return;
  double left = t[size_t(index)].x, right = left + t[size_t(index)].w;
  double limit = x() + w() - S(40);
  if (left < x()) scroll_ -= x() - left;
  else if (right > limit) scroll_ += right - limit;
  double content = 0;
  for (auto &tab : t) content += tab.w + S(4);
  scroll_ = std::max(0.0, std::min(scroll_, std::max(0.0, content - (w() - S(40)))));
  redraw();
}

void TabBar::draw() {
  const Look &L = look();
  const auto &docs = app_->docs();
  auto tabs = layout();
  int active = app_->active_index();
  double ty = y() + S(6), th = h() - S(6);
  double r = Sd(L.radius);
  {
    Painter p(window()->w(), window()->h());
    cairo_save(p.cr());
    cairo_rectangle(p.cr(), x(), y(), w() - S(34), h());
    cairo_clip(p.cr());
    for (size_t i = 0; i < tabs.size(); ++i) {
      double tx = tabs[i].x, tw = tabs[i].w;
      bool act = int(i) == active, hot = int(i) == hover_;
      if (act) {
        if (L.liquid) {
          p.shadow(tx, ty + S(2), tw, th - S(8), th / 2, L.shadow.alpha(0.12), 4);
          p.fill_rounded(tx, ty + S(2), tw, th - S(8), (th - S(8)) / 2, L.tab_active.alpha(0.96));
          p.stroke_rounded(tx, ty + S(2), tw, th - S(8), (th - S(8)) / 2, L.border.alpha(0.7));
        } else {
          // A card with rounded top corners that merges into the editor.
          cairo_t *cr = p.cr();
          cairo_new_sub_path(cr);
          cairo_move_to(cr, tx - r, ty + th);
          cairo_arc_negative(cr, tx - r, ty + th - r, r, M_PI / 2, 0);
          cairo_line_to(cr, tx, ty + r);
          cairo_arc(cr, tx + r, ty + r, r, M_PI, 3 * M_PI / 2);
          cairo_line_to(cr, tx + tw - r, ty);
          cairo_arc(cr, tx + tw - r, ty + r, r, -M_PI / 2, 0);
          cairo_line_to(cr, tx + tw, ty + th - r);
          cairo_arc_negative(cr, tx + tw + r, ty + th - r, r, M_PI, M_PI / 2);
          cairo_close_path(cr);
          p.set(L.tab_active);
          cairo_fill_preserve(cr);
          p.set(L.border.alpha(0.8));
          cairo_set_line_width(cr, 1);
          cairo_stroke(cr);
          p.fill_rect(tx - r + 1, ty + th - 1, tw + 2 * r - 2, 2, L.tab_active);
          p.fill_rounded(tx + S(12), ty + S(1), tw - S(24), Sd(2.5), Sd(1.2), L.accent);
        }
      } else if (hot) {
        double inset = L.liquid ? 0 : S(3);
        p.fill_rounded(tx + inset, ty + S(2), tw - 2 * inset, th - S(8), L.liquid ? (th - S(8)) / 2 : r,
                       L.tab_hover);
      } else if (int(i) + 1 != active && i + 1 < tabs.size() && !L.liquid) {
        p.line(tx + tw + S(2) - 0.5, ty + S(9), tx + tw + S(2) - 0.5, ty + th - S(13), L.border, 1);
      }
      double bs = S(18);
      draw_badge(p, docs[i]->lang, tx + S(12), ty + (th - S(6) - bs) / 2, bs);
      // Close button or modified dot.
      double cs = S(20), ccx = tx + tw - S(26), ccy = y() + (h() - cs) / 2 + S(2);
      bool show_close = act || hot;
      if (hot && hover_close_) p.fill_rounded(ccx, ccy, cs, cs, cs / 2, L.hover.mix(L.text, 0.08));
      if (docs[i]->modified() && !(hot && hover_close_)) {
        icon::draw(p, icon::Dot, ccx + S(2), ccy + S(2), cs - S(4), act ? L.accent : L.dim);
      } else if (show_close) {
        icon::draw(p, icon::Close, ccx + S(3), ccy + S(3), cs - S(6), L.dim);
      }
    }
    cairo_restore(p.cr());
    // New tab button.
    double px = plus_x(), ps = S(28), py = y() + (h() - ps) / 2 + S(2);
    if (hover_plus_) p.fill_rounded(px, py, ps, ps, L.liquid ? ps / 2 : Sd(7), L.tab_hover);
    icon::draw(p, icon::Plus, px + S(5), py + S(5), ps - S(10), L.dim);
  }
  // Labels.
  fl_push_clip(x(), y(), w() - S(34), h());
  for (size_t i = 0; i < tabs.size(); ++i) {
    double tx = tabs[i].x, tw = tabs[i].w;
    bool act = int(i) == active;
    double bs = S(18);
    draw_badge_text(docs[i]->lang, tx + S(12), ty + (th - S(6) - bs) / 2, bs);
    text_font(L.theme.font_size, act);
    draw_text(docs[i]->display_name(), int(tx + S(38)), int(ty), int(tw - S(70)), int(th - S(6)),
              act ? L.tab_text : L.tab_text_dim);
  }
  fl_pop_clip();
}

int TabBar::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int h = tab_at(mx);
      bool hc = h >= 0 && on_close(h, mx, my);
      double px = plus_x();
      bool hp = mx >= px && mx < px + S(28);
      if (h != hover_ || hc != hover_close_ || hp != hover_plus_) {
        hover_ = h;
        hover_close_ = hc;
        hover_plus_ = hp;
        redraw();
        if (h >= 0) {
          auto t = layout();
          const Document *d = app_->docs()[size_t(h)].get();
          static std::string tip;
          tip = d->path.empty() ? d->display_name() : home_relative(d->path);
          if (d->modified()) tip += "  •  " + std::string(_("modified"));
          Fl_Tooltip::enter_area(this, int(t[size_t(h)].x - x()), 0, int(t[size_t(h)].w), this->h(), tip.c_str());
        }
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      hover_close_ = hover_plus_ = false;
      redraw();
      return 1;
    case FL_PUSH: {
      int t = tab_at(mx);
      press_ = t;
      press_x_ = mx;
      dragging_ = false;
      drag_from_ = t;
      if (Fl::event_button() == FL_MIDDLE_MOUSE) {
        if (t >= 0) app_->close_document(t);
        return 1;
      }
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (t >= 0) app_->activate(t);
        app_->popup_menu(mx, my);
        return 1;
      }
      if (t >= 0 && !on_close(t, mx, my)) app_->activate(t);
      if (t < 0 && Fl::event_clicks() > 0) app_->new_document();
      return 1;
    }
    case FL_DRAG: {
      if (drag_from_ < 0) return 1;
      if (std::abs(mx - press_x_) > S(8)) dragging_ = true;
      if (dragging_) {
        int t = tab_at(mx);
        if (t >= 0 && t != drag_from_) {
          app_->move_document(drag_from_, t);
          drag_from_ = t;
        }
      }
      return 1;
    }
    case FL_RELEASE: {
      int t = tab_at(mx);
      if (!dragging_ && Fl::event_button() == FL_LEFT_MOUSE) {
        if (t >= 0 && t == press_ && on_close(t, mx, my)) app_->close_document(t);
        double px = plus_x();
        if (t < 0 && mx >= px && mx < px + S(28)) app_->new_document();
      }
      dragging_ = false;
      drag_from_ = -1;
      return 1;
    }
    case FL_MOUSEWHEEL: {
      int d = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
      int n = int(app_->docs().size());
      if (n > 1) app_->activate((app_->active_index() + (d > 0 ? 1 : n - 1)) % n);
      return 1;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
      return 0;
  }
  return Fl_Widget::handle(event);
}

// -------------------------------------------------------------- StatusBar

StatusBar::StatusBar(App *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

std::vector<StatusBar::Pill> StatusBar::pills() const {
  std::vector<Pill> out;
  Document *d = app_->active();
  if (!d) return out;
  EditorView *ed = app_->editor();
  int pos = ed->insert_position();
  int line = d->line_of(pos);
  int ls = d->buf->line_start(pos);
  char *lt = d->buf->text_range(ls, pos);
  int col = display_column(lt, pos - ls, d->buf->tab_distance()) + 1;
  free(lt);
  int a = 0, b = 0;
  std::string where = format(_("Ln %d, Col %d"), line, col);
  if (d->buf->selection_position(&a, &b) && b > a) {
    int lines = d->buf->count_lines(a, b);
    where += lines ? format(_("  (%d lines)"), lines + 1) : format(_("  (%d selected)"), b - a);
  }
  out.push_back({where, 1});
  out.push_back({d->use_tabs ? format(_("Tabs: %d"), d->buf->tab_distance()) : format(_("Spaces: %d"), d->indent_width),
                 2});
  out.push_back({encoding_name(d->encoding), 3});
  out.push_back({eol_name(d->eol), 4});
  out.push_back({language_name(d->lang), 5});
  if (app_->settings().zoom != 0) {
    int base = std::max(7, int(std::lround(app_->settings().font_size * 4.0 / 3.0 * ui_scale())));
    out.push_back({format("%d%%", int(std::lround(100.0 * (base + app_->settings().zoom) / base))), 6});
  }
  text_font(look().theme.font_size - S(1));
  double xr = x() + w() - S(8);
  for (int i = int(out.size()) - 1; i >= 0; --i) {
    out[size_t(i)].w = text_width(out[size_t(i)].text) + S(18);
    xr -= out[size_t(i)].w;
    out[size_t(i)].x = xr;
    xr -= S(2);
  }
  return out;
}

void StatusBar::draw() {
  const Look &L = look();
  auto ps = pills();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), L.status);
    p.line(x(), y() + 0.5, x() + w(), y() + 0.5, L.header_line, 1);
    for (size_t i = 0; i < ps.size(); ++i) {
      if (int(i) == hover_)
        p.fill_rounded(ps[i].x, y() + S(3), ps[i].w, h() - S(6), L.liquid ? (h() - S(6)) / 2.0 : Sd(6), L.hover);
    }
    Document *d = app_->active();
    if (d && d->readonly && app_->toast_text().empty())
      icon::draw(p, icon::Warning, x() + S(10), y() + (h() - S(14)) / 2, S(14), L.dim);
  }
  text_font(L.theme.font_size - S(1));
  for (size_t i = 0; i < ps.size(); ++i)
    draw_text(ps[i].text, int(ps[i].x), y(), int(ps[i].w), h(), int(i) == hover_ ? L.text : L.dim, 1);
  double left_end = ps.empty() ? x() + w() - S(8) : ps.front().x - S(12);
  std::string msg = app_->toast_text();
  Document *d = app_->active();
  int lx = x() + S(12);
  if (msg.empty() && d) {
    msg = d->path.empty() ? _("Not saved yet") : home_relative(d->path);
    if (d->readonly) {
      msg = std::string(_("Read-only")) + "  ·  " + msg;
      lx += S(20);
    }
  }
  draw_text(msg, lx, y(), int(left_end - lx), h(), app_->toast_text().empty() ? L.dim : L.accent_text);
}

int StatusBar::handle(int event) {
  int mx = Fl::event_x();
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      auto ps = pills();
      int h = -1;
      for (size_t i = 0; i < ps.size(); ++i)
        if (mx >= ps[i].x && mx < ps[i].x + ps[i].w) h = int(i);
      if (h != hover_) {
        hover_ = h;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      auto ps = pills();
      for (auto &p : ps)
        if (mx >= p.x && mx < p.x + p.w) {
          app_->popup_status(p.id, int(p.x), y());
          return 1;
        }
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}

// ---------------------------------------------------------------- InfoBar

InfoBar::InfoBar(App *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  primary_ = new FlatButton(0, 0, S(100), S(28), "", true);
  secondary_ = new FlatButton(0, 0, S(100), S(28), "");
  primary_->callback([](Fl_Widget *, void *p) {
    auto *self = static_cast<InfoBar *>(p);
    self->app_->handle_notice(self->doc_, 0);
  }, this);
  secondary_->callback([](Fl_Widget *, void *p) {
    auto *self = static_cast<InfoBar *>(p);
    self->app_->handle_notice(self->doc_, 1);
  }, this);
  end();
  box(FL_NO_BOX);
}

void InfoBar::show_for(Document *doc) {
  doc_ = doc;
  if (!doc || doc->notice == Document::Notice::Clear) {
    hide();
    return;
  }
  warning_ = true;
  switch (doc->notice) {
    case Document::Notice::Changed:
      text_ = doc->modified() ? _("The file changed on disk and here. Reload it and lose your changes?")
                              : _("The file changed on disk.");
      primary_->text(_("Reload"));
      secondary_->text(_("Keep Mine"));
      secondary_->show();
      break;
    case Document::Notice::Deleted:
      text_ = _("The file was deleted or moved. Save to create it again.");
      primary_->text(_("Save"));
      secondary_->text(_("Dismiss"));
      secondary_->show();
      break;
    case Document::Notice::Recovered:
      warning_ = false;
      text_ = doc->notice_text.empty() ? _("Unsaved changes were recovered.") : doc->notice_text;
      primary_->text(_("Save"));
      secondary_->text(_("Dismiss"));
      secondary_->show();
      break;
    default:
      text_ = doc->notice_text;
      primary_->text(_("Dismiss"));
      secondary_->hide();
      break;
  }
  show();
  layout_buttons();
  redraw();
}

void InfoBar::resize(int X, int Y, int W, int H) {
  Fl_Widget::resize(X, Y, W, H);
  layout_buttons();
}

void InfoBar::layout_buttons() {
  text_font(look().theme.font_size);
  int bh = S(28), by = y() + (h() - bh) / 2;
  int w1 = std::max<int>(S(84), text_width(_("Keep Mine")) + S(28));
  int xr = x() + w() - S(10);
  if (secondary_->visible()) {
    secondary_->resize(xr - w1, by, w1, bh);
    xr -= w1 + S(8);
  }
  primary_->resize(xr - w1, by, w1, bh);
}

void InfoBar::draw() {
  const Look &L = look();
  {
    Painter p(window()->w(), window()->h());
    Color tint = warning_ ? Color::hex(0xE0A11B) : L.accent;
    p.fill_rect(x(), y(), w(), h(), L.editor.mix(tint, L.dark ? 0.16 : 0.12));
    p.line(x(), y() + h() - 0.5, x() + w(), y() + h() - 0.5, tint.alpha(0.45), 1);
    icon::draw(p, warning_ ? icon::Warning : icon::Info, x() + S(14), y() + (h() - S(18)) / 2, S(18),
               warning_ ? (L.dark ? Color::hex(0xF2C14E) : Color::hex(0x9A6700)) : L.accent_text);
  }
  text_font(L.theme.font_size);
  int right = secondary_->visible() ? secondary_->x() : primary_->x();
  right = std::min(right, primary_->x());
  draw_text(text_, x() + S(42), y(), right - x() - S(52), h(), L.text);
  draw_children();
}

// -------------------------------------------------------------- StartPage

StartPage::StartPage(App *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

std::vector<StartPage::Hit> StartPage::hits() const {
  std::vector<Hit> out;
  double cw = std::min<double>(S(520), w() - S(48));
  double cx = x() + (w() - cw) / 2;
  double cy = y() + std::max<double>(S(40), (h() - S(520)) / 2);
  double ty = cy + S(150);
  double bw = (cw - S(16)) / 2;
  out.push_back({cx, ty, bw, Sd(64), 0});
  out.push_back({cx + bw + S(16), ty, bw, Sd(64), 1});
  double ry = ty + S(110);
  const auto &rec = app_->recent();
  for (size_t i = 0; i < rec.size() && i < 7 && ry + S(36) < y() + h(); ++i, ry += S(38))
    out.push_back({cx, ry, cw, Sd(36), int(100 + i)});
  return out;
}

void StartPage::draw() {
  const Look &L = look();
  auto hs = hits();
  double cw = std::min<double>(S(520), w() - S(48));
  double cx = x() + (w() - cw) / 2;
  double cy = y() + std::max<double>(S(40), (h() - S(520)) / 2);
  const auto &rec = app_->recent();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), L.editor);
    // Soft accent glow behind the mark.
    cairo_pattern_t *g = cairo_pattern_create_radial(cx + S(36), cy + S(36), 0, cx + S(36), cy + S(36), S(160));
    cairo_pattern_add_color_stop_rgba(g, 0, L.accent.r, L.accent.g, L.accent.b, L.dark ? 0.16 : 0.10);
    cairo_pattern_add_color_stop_rgba(g, 1, L.accent.r, L.accent.g, L.accent.b, 0);
    cairo_set_source(p.cr(), g);
    cairo_rectangle(p.cr(), x(), y(), w(), h());
    cairo_fill(p.cr());
    cairo_pattern_destroy(g);
    icon::logo(p, cx, cy, S(72));
    for (auto &h : hs) {
      bool hot = h.id == hover_;
      if (h.id < 100) {
        p.fill_rounded(h.x, h.y, h.w, h.h, Sd(L.radius + 4), hot ? L.hover : L.card);
        p.stroke_rounded(h.x, h.y, h.w, h.h, Sd(L.radius + 4), hot ? L.accent.alpha(0.6) : L.border);
        p.fill_rounded(h.x + S(14), h.y + S(14), S(36), S(36), Sd(10), L.accent.alpha(L.dark ? 0.25 : 0.12));
        icon::draw(p, h.id == 0 ? icon::File : icon::Open, h.x + S(21), h.y + S(21), S(22), L.accent_text);
      } else {
        if (hot) p.fill_rounded(h.x, h.y, h.w, h.h, Sd(8), L.hover);
        icon::draw(p, icon::File, h.x + S(10), h.y + S(9), S(18), L.dim);
      }
    }
    if (!rec.empty() && hs.size() > 2) p.line(cx, hs[2].y - S(12), cx + cw, hs[2].y - S(12), L.border, 1);
  }
  text_font(L.theme.font_size + S(14), true);
  draw_text("Niny", int(cx + S(92)), int(cy + S(6)), int(cw - S(92)), S(40), L.text);
  text_font(L.theme.font_size + S(1));
  draw_text(_("A fast, quiet text editor for SDE"), int(cx + S(92)), int(cy + S(44)), int(cw - S(92)), S(24),
            L.dim);
  for (auto &h : hs) {
    if (h.id < 100) {
      text_font(L.theme.font_size + S(1), true);
      draw_text(h.id == 0 ? _("New Document") : _("Open File…"), int(h.x + S(62)), int(h.y + S(10)),
                int(h.w - S(70)), S(24), L.text);
      text_font(L.theme.font_size - S(1));
      draw_text(h.id == 0 ? "Ctrl+N" : "Ctrl+O", int(h.x + S(62)), int(h.y + S(32)), int(h.w - S(70)), S(20), L.dim);
    } else {
      const std::string &path = rec[size_t(h.id - 100)];
      text_font(L.theme.font_size);
      int nw = std::min<int>(text_width(basename_of(path)), int(h.w * 0.5));
      draw_text(basename_of(path), int(h.x + S(36)), int(h.y), nw + S(4), int(h.h), L.text);
      text_font(L.theme.font_size - S(1));
      draw_text(home_relative(path.substr(0, path.rfind('/'))), int(h.x + S(48) + nw), int(h.y),
                int(h.w - nw - S(56)), int(h.h), L.dim);
    }
  }
  if (!rec.empty() && hs.size() > 2) {
    text_font(L.theme.font_size - S(1), true);
    draw_text(_("RECENT"), int(cx), int(hs[2].y - S(40)), int(cw), S(24), L.dim);
  }
  text_font(L.theme.font_size - S(1));
  draw_text(_("Ctrl+Shift+P shows every command  ·  Drop files here to open them"), x(), y() + h() - S(40), w(),
            S(24), L.dim.alpha(0.8), 1);
}

int StartPage::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  auto find_hit = [&]() {
    for (auto &h : hits())
      if (mx >= h.x && mx < h.x + h.w && my >= h.y && my < h.y + h.h) return h.id;
    return -1;
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int h = find_hit();
      if (h != hover_) {
        hover_ = h;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH:
      return 1;
    case FL_RELEASE: {
      int h = find_hit();
      if (h == 0) app_->new_document();
      else if (h == 1) app_->open_dialog();
      else if (h >= 100 && h - 100 < int(app_->recent().size())) {
        std::string p = app_->recent()[size_t(h - 100)];
        app_->open_file(p);
      }
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}

}  // namespace niny
