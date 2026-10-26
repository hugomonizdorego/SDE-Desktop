// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Niny chrome: the tab strip, the menu bar with the formatting buttons
// under it, the status bar and the notice bar.
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

namespace {
const int kTabGap = 2;
}

std::vector<TabBar::Tab> TabBar::layout() const {
  const auto &docs = app_->docs();
  std::vector<Tab> tabs(docs.size());
  if (docs.empty()) return tabs;
  text_font(look().theme.font_size);
  double avail = w() - S(48);
  std::vector<double> want(docs.size());
  double total = 0;
  for (size_t i = 0; i < docs.size(); ++i) {
    std::string name = docs[i]->display_name();
    want[i] = std::max<double>(S(150), std::min<double>(S(250), text_width(name) + S(70)));
    total += want[i] + S(kTabGap);
  }
  if (total > avail) {
    double each = std::max<double>(S(96), avail / docs.size() - S(kTabGap));
    for (auto &wv : want) wv = std::min(wv, each);
  }
  double x0 = x() - scroll_;
  for (size_t i = 0; i < docs.size(); ++i) {
    tabs[i].x = x0;
    tabs[i].w = want[i];
    x0 += want[i] + S(kTabGap);
  }
  return tabs;
}

double TabBar::clip_right() const { return x() + w() - S(40); }

double TabBar::plus_x(const std::vector<Tab> &t) const {
  double px = t.empty() ? x() : t.back().x + t.back().w + S(6);
  return std::min<double>(px, clip_right());
}

void TabBar::close_geometry(const Tab &t, double &cx, double &cy, double &cs) const {
  cs = S(26);
  cx = t.x + t.w - S(34);
  cy = y() + S(6) + (h() - S(6) - cs) / 2;
}

int TabBar::tab_at(int mx) const {
  auto t = layout();
  for (size_t i = 0; i < t.size(); ++i)
    if (mx >= t[i].x && mx < t[i].x + t[i].w && mx < clip_right() - S(2)) return int(i);
  return -1;
}

bool TabBar::on_close(const std::vector<Tab> &t, int index, int mx, int my) const {
  if (index < 0 || index >= int(t.size())) return false;
  double cx, cy, cs;
  close_geometry(t[size_t(index)], cx, cy, cs);
  return mx >= cx && mx < cx + cs && my >= cy && my < cy + cs && mx < clip_right() - S(2);
}

bool TabBar::on_plus(const std::vector<Tab> &t, int mx, int my) const {
  double px = plus_x(t);
  return my >= y() && my < y() + h() && mx >= px && mx < px + S(32) && tab_at(mx) < 0;
}

TabBar::Box TabBar::tab_box(int index) const {
  auto t = layout();
  Box b;
  if (index < 0 || index >= int(t.size())) return b;
  const Tab &tab = t[size_t(index)];
  b.x = int(std::lround(tab.x));
  b.y = y() + S(6);
  b.w = int(std::lround(tab.w));
  b.h = h() - S(6);
  b.shown = tab.x >= x() - 0.5 && tab.x + tab.w <= clip_right() + 0.5;
  return b;
}

TabBar::Box TabBar::close_box(int index) const {
  auto t = layout();
  Box b;
  if (index < 0 || index >= int(t.size())) return b;
  double cx, cy, cs;
  close_geometry(t[size_t(index)], cx, cy, cs);
  b.x = int(std::ceil(cx));
  b.y = int(std::ceil(cy));
  b.w = b.h = int(cs) - 1;
  b.shown = cx >= x() && cx + cs <= clip_right() - S(2);
  return b;
}

TabBar::Box TabBar::plus_box() const {
  auto t = layout();
  Box b;
  double px = plus_x(t), ps = S(32);
  b.x = int(std::ceil(px));
  b.y = int(std::ceil(y() + S(6) + (h() - S(6) - ps) / 2));
  b.w = b.h = int(ps) - 1;
  b.shown = true;
  return b;
}

void TabBar::ensure_visible(int index) {
  auto t = layout();
  if (index < 0 || index >= int(t.size())) return;
  double left = t[size_t(index)].x, right = left + t[size_t(index)].w;
  double limit = x() + w() - S(48);
  if (left < x()) scroll_ -= x() - left;
  else if (right > limit) scroll_ += right - limit;
  double content = 0;
  for (auto &tab : t) content += tab.w + S(kTabGap);
  scroll_ = std::max(0.0, std::min(scroll_, std::max(0.0, content - (w() - S(48)))));
  redraw();
  app_->schedule_qa();
}

void TabBar::draw() {
  const Look &L = look();
  const auto &docs = app_->docs();
  auto tabs = layout();
  int active = app_->active_index();
  bool closable = app_->can_close_tab();
  double ty = y() + S(6), th = h() - S(6);
  double r = Sd(8);
  Color hover_bg = L.dark ? Color::hex(0xFFFFFF, 0.06) : Color::hex(0x000000, 0.05);
  // The active tab flows into the menu bar, or into the text without it.
  Color card = app_->menubar_shown() ? L.bar : L.editor;
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w() - S(40), h());
    cairo_clip(cr);
    for (size_t i = 0; i < tabs.size(); ++i) {
      double tx = tabs[i].x, tw = tabs[i].w;
      bool act = int(i) == active, hot = int(i) == hover_;
      if (act) {
        // A card with rounded top corners that flows into the bar below.
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
        p.set(card);
        cairo_fill_preserve(cr);
        if (L.dark || L.glass) {
          p.set(L.border.alpha(L.dark ? 0.35 : 0.5));
          cairo_set_line_width(cr, 1);
          cairo_stroke(cr);
          p.fill_rect(tx - r + 1, ty + th - 1, tw + 2 * r - 2, 2, card);
        } else {
          cairo_new_path(cr);
        }
      } else if (hot) {
        p.fill_rounded(tx + S(2), ty + S(1), tw - S(4), th - S(7), Sd(6), hover_bg);
      } else if (int(i) + 1 != active && i + 1 < tabs.size()) {
        p.line(tx + tw + S(kTabGap) / 2.0 - 0.5, ty + S(9), tx + tw + S(kTabGap) / 2.0 - 0.5, ty + th - S(11),
               L.border.alpha(0.9), 1);
      }
      // Close button, or the dot of a modified document.
      double cx, cy, cs;
      close_geometry(tabs[i], cx, cy, cs);
      bool modified = docs[i]->modified();
      bool on_x = hot && hover_close_;  // only while the × can close its tab
      if (on_x) p.fill_rounded(cx, cy, cs, cs, Sd(5), hover_bg.alpha(L.dark ? 0.1 : 0.08));
      if (modified && !on_x && !(act && hot && closable)) {
        p.circle(cx + cs / 2, cy + cs / 2, Sd(4), L.text.mix(L.dim, 0.35));
      } else if (!closable) {
        // The only tab: its × is inactive.
        icon::draw(p, icon::Close, cx + S(6), cy + S(6), cs - S(12), L.dim.alpha(0.45));
      } else if (act || hot) {
        icon::draw(p, icon::Close, cx + S(6), cy + S(6), cs - S(12), L.text.mix(L.dim, 0.3));
      }
    }
    cairo_restore(cr);
    // New tab button.
    double px = plus_x(tabs), ps = S(32), py = ty + (th - ps) / 2;
    if (hover_plus_) p.fill_rounded(px, py, ps, ps, Sd(6), hover_bg);
    icon::draw(p, icon::Plus, px + S(8), py + S(8), ps - S(16), L.text.mix(L.dim, 0.3));
  }
  // Titles.
  fl_push_clip(x(), y(), w() - S(40), h());
  for (size_t i = 0; i < tabs.size(); ++i) {
    double tx = tabs[i].x, tw = tabs[i].w;
    bool act = int(i) == active;
    text_font(L.theme.font_size, act);
    draw_text(docs[i]->display_name(), int(tx + S(14)), int(ty), int(tw - S(54)), int(th),
              act ? L.tab_text : L.tab_text.mix(L.dim, 0.45));
  }
  fl_pop_clip();
}

void TabBar::track(int mx, int my) {
  auto t = layout();
  bool inside = mx >= x() && mx < x() + w() && my >= y() && my < y() + h();
  int i = inside ? tab_at(mx) : -1;
  bool on_x = i >= 0 && on_close(t, i, mx, my);
  bool hp = inside && i < 0 && on_plus(t, mx, my);
  bool hc = on_x && app_->can_close_tab();  // the only tab's × has no hover
  if (i != hover_ || hc != hover_close_ || hp != hover_plus_) {
    hover_ = i;
    hover_close_ = hc;
    hover_plus_ = hp;
    redraw();
  }
  int zone = i >= 0 ? 2 * i + (on_x ? 1 : 0) : hp ? -2 : -1;
  if (zone == tip_zone_) return;
  tip_zone_ = zone;
  // Fl_Tooltip compares the text pointers: alternate two buffers.
  static std::string tips[2];
  static int which = 0;
  which ^= 1;
  if (on_x) {
    double cx, cy, cs;
    close_geometry(t[size_t(i)], cx, cy, cs);
    Fl_Tooltip::enter_area(this, int(cx) - x(), int(cy) - y(), int(cs), int(cs),
                           app_->can_close_tab() ? _("Close Tab (Ctrl+W)")
                                                 : _("Only one tab is open, so it cannot be closed"));
  } else if (i >= 0) {
    const Document *d = app_->docs()[size_t(i)].get();
    std::string &tip = tips[which];
    tip = d->path.empty() ? d->display_name() : home_relative(d->path);
    if (d->modified()) tip += "  •  " + std::string(_("modified"));
    Fl_Tooltip::enter_area(this, int(t[size_t(i)].x - x()), 0, int(t[size_t(i)].w), h(), tip.c_str());
  } else if (hp) {
    Fl_Tooltip::enter_area(this, int(plus_x(t) - x()), 0, S(32), h(), _("New tab (Ctrl+N)"));
  } else {
    Fl_Tooltip::enter_area(this, 0, 0, 0, 0, nullptr);
  }
}

int TabBar::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  // Where the pointer is now: after a tab closed (perhaps after a question)
  // another tab, or nothing, is under it.
  auto retrack = [this] {
    int rx = 0, ry = 0;
    Fl::get_mouse(rx, ry);
    if (window()) track(rx - window()->x_root(), ry - window()->y_root());
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE:
      track(mx, my);
      return 1;
    case FL_LEAVE:
      hover_ = -1;
      hover_close_ = hover_plus_ = false;
      tip_zone_ = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      auto t = layout();
      int i = tab_at(mx), button = Fl::event_button();
      bool on_x = i >= 0 && on_close(t, i, mx, my);
      bool plus = i < 0 && on_plus(t, mx, my);
      bool empty = i < 0 && !plus;
      // A double click on the empty strip opens a tab when both clicks were
      // there. The click after one whose × closed the last tab used to land
      // on the empty strip and count as a double click (alpha 3).
      bool double_empty = empty && last_press_empty_ && Fl::event_clicks() > 0;
      last_press_empty_ = empty && button == FL_LEFT_MOUSE;
      press_ = i;
      press_x_ = mx;
      press_close_ = on_x && button == FL_LEFT_MOUSE;
      press_plus_ = plus && button == FL_LEFT_MOUSE;
      dragging_ = false;
      drag_from_ = button == FL_LEFT_MOUSE ? i : -1;
      if (button == FL_MIDDLE_MOUSE) {
        if (i >= 0) {
          Fl::event_is_click(0);
          app_->close_document(i);  // not the only tab
          retrack();
        }
        return 1;
      }
      if (button == FL_RIGHT_MOUSE) {
        if (i >= 0) {
          app_->activate(i);
          app_->tab_menu(i, Fl::event_x_root(), Fl::event_y_root());
        }
        return 1;
      }
      if (i >= 0 && !on_x) app_->activate(i);
      if (double_empty) {
        Fl::event_is_click(0);
        last_press_empty_ = false;
        app_->new_document();
      }
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
      bool dragged = dragging_, was_close = press_close_, was_plus = press_plus_;
      dragging_ = press_close_ = press_plus_ = false;
      drag_from_ = -1;
      if (dragged || Fl::event_button() != FL_LEFT_MOUSE) return 1;
      auto t = layout();
      int i = tab_at(mx);
      if (was_close && i == press_ && on_close(t, i, mx, my)) {
        // The × closes its tab and nothing else happens: whatever slides
        // under the pointer (a tab, the empty strip, the + button) is left
        // alone. The only tab's × is inactive.
        if (app_->can_close_tab()) {
          Fl::event_is_click(0);
          app_->close_document(i);
        }
        retrack();
        return 1;
      }
      if (was_plus && on_plus(t, mx, my)) {
        Fl::event_is_click(0);
        app_->new_document();
        retrack();
      }
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

// ---------------------------------------------------------------- Toolbar

namespace {

struct MenuTitle {
  int id;
  const char *label;  // "&" marks the underlined letter
  const char *qa;
};
// The menu bar, in the order of Pluma, Mousepad and Kate.
const MenuTitle kMenus[] = {
    {Toolbar::File, N_("&File"), "file"},
    {Toolbar::Edit, N_("&Edit"), "edit"},
    {Toolbar::View, N_("&View"), "view"},
    {Toolbar::Search, N_("&Search"), "search"},
    {Toolbar::Document, N_("&Document"), "document"},
    {Toolbar::Tools, N_("&Tools"), "tools"},
    {Toolbar::Help, N_("&Help"), "help"},
};
const int kMenuCount = int(sizeof kMenus / sizeof kMenus[0]);
// Alt shortcuts of the editor that a title must not take (Word Wrap, tabs).
const char kAltKeys[] = "z123456789";

// The titles with their underlined letters (the language does not change
// while Niny runs).
const std::vector<Mnemonic> &menu_titles() {
  static const std::vector<Mnemonic> titles = [] {
    std::vector<std::string> labels;
    for (const MenuTitle &m : kMenus) labels.push_back(_(m.label));
    return menu_mnemonics(labels, kAltKeys);
  }();
  return titles;
}

int menu_index(int id) {
  for (int i = 0; i < kMenuCount; ++i)
    if (kMenus[i].id == id) return i;
  return -1;
}

}  // namespace

Toolbar::Toolbar(App *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

std::string Toolbar::title(int id) {
  int i = menu_index(id);
  return i < 0 ? std::string() : menu_titles()[size_t(i)].text;
}

int Toolbar::mnemonic(int id) {
  int i = menu_index(id);
  return i < 0 ? 0 : menu_titles()[size_t(i)].key;
}

const char *Toolbar::qa_name(int id) {
  int i = menu_index(id);
  if (i >= 0) return kMenus[i].qa;
  return id == Heading ? "heading" : id == List ? "list" : "toolbar";
}

std::vector<Toolbar::Item> Toolbar::items() const {
  std::vector<Item> out;
  const Look &L = look();
  text_font(L.theme.font_size);
  const auto &titles = menu_titles();
  // Narrow windows and long translations: the titles move closer; the
  // formatting group gives way first, then the gear (Edit > Preferences).
  double room = w() - S(6) - S(8) - S(38) - S(16), need = 0;
  for (const Mnemonic &t : titles) need += text_width(t.text) + S(26) + S(2);
  double pad = need <= room ? S(26) : S(14);
  double bx = x() + S(6);
  for (int i = 0; i < kMenuCount; ++i) {
    Item it;
    it.id = kMenus[i].id;
    it.label = titles[size_t(i)].text;
    it.mnemonic = titles[size_t(i)].index;
    it.x = bx;
    it.w = text_width(it.label) + pad;
    bx += it.w + S(2);
    out.push_back(it);
  }
  double left_end = bx + S(16);
  double xr = x() + w() - S(8);
  auto right = [&](int id, int icon, double width, const char *tip, const std::string &label = "",
                   bool dropdown = false, bool on = false) {
    Item it;
    it.id = id;
    it.icon = icon;
    it.label = label;
    it.dropdown = dropdown;
    it.on = on;
    it.tip = tip;
    it.w = width;
    xr -= width;
    it.x = xr;
    xr -= S(2);
    out.push_back(it);
  };
  if (xr - S(38) >= bx)
    right(Settings, icon::Gear, S(38), _("Preferences (Ctrl+,)"), "", false, app_->preferences_visible());
  niny::Document *d = app_->active();  // Toolbar::Document is the menu
  if (d && app_->is_markdown(d) && !app_->settings_visible()) {
    xr -= S(10);
    right(Preview, icon::Eye, S(38), _("Formatted view (Ctrl+Shift+M)"), "", false, d->preview);
    xr -= S(10);
    right(Clear, icon::ClearFormat, S(38), _("Clear formatting"));
    right(Link, icon::Link, S(38), _("Link (Ctrl+K)"));
    right(Italic, icon::Italic, S(38), _("Italic (Ctrl+I)"));
    right(Bold, icon::Bold, S(38), _("Bold (Ctrl+B)"));
    right(List, icon::ListBullet, S(56), _("Lists"), "", true);
    int h = d->preview ? 0 : app_->current_heading();
    std::string hl = h ? "H" + std::to_string(h) : std::string(_("Text"));
    right(Heading, -1, std::max<double>(S(64), text_width(hl) + S(40)), _("Heading"), hl, true);
    // Hide the formatting group when it would run into the menus; keep the
    // formatted view toggle when it fits.
    if (xr < left_end) {
      std::vector<Item> keep;
      for (auto &it : out)
        if (it.id < Heading || it.id == Settings || (it.id == Preview && it.x >= left_end)) keep.push_back(it);
      out = keep;
    }
    for (auto &it : out)
      if (it.id >= Heading && it.id <= Clear) it.enabled = !d->preview;
  }
  return out;
}

void Toolbar::draw() {
  const Look &L = look();
  auto its = items();
  Color hover_bg = L.dark ? Color::hex(0xFFFFFF, 0.07) : Color::hex(0x000000, 0.05);
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), L.bar);
    p.line(x(), y() + h() - 0.5, x() + w(), y() + h() - 0.5, L.bar_line, 1);
    double by = y() + S(4), bh = h() - S(8);
    for (size_t i = 0; i < its.size(); ++i) {
      const Item &it = its[i];
      bool hot = (int(i) == hover_ || it.id == open_ || it.id == focus_) && it.enabled;
      if (it.on) p.fill_rounded(it.x, by, it.w, bh, Sd(5), L.accent.alpha(L.dark ? 0.24 : 0.13));
      else if (hot) p.fill_rounded(it.x, by, it.w, bh, Sd(5), hover_bg);
      // Alt pressed alone: the title that has the keyboard.
      if (it.id == focus_ && open_ < 0) focus_ring(p, it.x, by, it.w, bh, Sd(5));
      Color c = !it.enabled ? L.dim.alpha(0.45) : it.on ? L.accent_text : L.text.mix(L.dim, 0.15);
      double is = S(18);
      if (it.icon >= 0) {
        double ix = it.dropdown ? it.x + S(8) : it.x + (it.w - is) / 2;
        icon::draw(p, icon::Kind(it.icon), ix, by + (bh - is) / 2, is, c);
      }
      if (it.dropdown) icon::draw(p, icon::ChevronDown, it.x + it.w - S(22), by + (bh - S(14)) / 2, S(14), c);
    }
    // Divider between the formatting group and the view toggle / gear.
    for (auto &it : its)
      if (it.id == Preview || (it.id == Settings && its.size() > size_t(kMenuCount) + 1)) {
        double dx = it.x - S(6) + 0.5;
        p.line(dx, y() + S(11), dx, y() + h() - S(11), L.border, 1);
      }
  }
  for (auto &it : its) {
    if (it.label.empty()) continue;
    text_font(L.theme.font_size, false);
    Color c = !it.enabled ? L.dim.alpha(0.45) : L.text;
    if (!is_menu(it.id)) {
      draw_text(it.label, int(it.x + S(10)), y(), int(it.w - S(30)), h(), c, 1);
      continue;
    }
    draw_text(it.label, int(it.x), y(), int(it.w), h(), c, 1);
    // The letter that Alt opens the menu with, underlined as FLTK does.
    if (it.mnemonic >= 0 && it.mnemonic < int(it.label.size())) {
      int tw = text_width(it.label);
      int ux = int(it.x) + (int(it.w) - tw) / 2 + text_width(it.label.substr(0, size_t(it.mnemonic)));
      int uw = text_width(it.label.substr(size_t(it.mnemonic), 1));
      int base = y() + (h() + fl_height()) / 2 - fl_descent();
      fl_color(c.fl());
      fl_rectf(ux, base + std::max(1, S(1.5)), uw, std::max(1, S(1)));
    }
  }
}

int Toolbar::title_at_root(int rx, int ry) const {
  if (!visible_r() || !window()) return 0;
  int mx = rx - window()->x_root(), my = ry - window()->y_root();
  if (my < y() || my >= y() + h()) return 0;
  for (auto &it : items())
    if (is_menu(it.id) && mx >= it.x && mx < it.x + it.w) return it.id;
  return 0;
}

int Toolbar::neighbour(int id, int dir) const {
  int i = menu_index(id);
  if (i < 0) return File;
  return kMenus[(i + dir + kMenuCount) % kMenuCount].id;
}

int Toolbar::menu_for_key(int key) const {
  if (key >= 'A' && key <= 'Z') key += 32;
  const auto &titles = menu_titles();
  for (int i = 0; i < kMenuCount; ++i)
    if (titles[size_t(i)].key && titles[size_t(i)].key == key) return kMenus[i].id;
  return 0;
}

void Toolbar::focus_titles(bool on) {
  int f = on ? int(File) : 0;
  if (f == focus_) return;
  focus_ = f;
  redraw();
  if (!on) app_->peek_menubar(false);
  app_->schedule_qa();
}

int Toolbar::title_key(int key, int state) {
  if (!focus_ || (state & (FL_CTRL | FL_META))) return 0;
  switch (key) {
    case FL_Left:
    case FL_Right:
      focus_ = neighbour(focus_, key == FL_Left ? -1 : 1);
      redraw();
      app_->schedule_qa();
      return 1;
    case FL_Home:
    case FL_End:
      focus_ = key == FL_Home ? int(File) : int(Help);
      redraw();
      app_->schedule_qa();
      return 1;
    case FL_Down:
    case FL_Up:
    case FL_Enter:
    case FL_KP_Enter:
    case ' ': {
      int id = focus_;
      focus_ = 0;
      open(id, true);
      return 1;
    }
    case FL_Escape:
    case FL_F + 10:
      focus_titles(false);
      return 1;
  }
  // The underlined letters work without Alt while the titles are focused.
  if (int id = menu_for_key(key)) {
    focus_ = 0;
    open(id, true);
    return 1;
  }
  return 0;
}

void Toolbar::open(int id, bool keyboard) {
  // The formatting buttons act at once.
  for (auto &it : items()) {
    if (it.id != id || !it.enabled) continue;
    switch (id) {
      case Bold: app_->md_wrap("**"); return;
      case Italic: app_->md_wrap("*"); return;
      case Link: app_->md_link(); return;
      case Clear: app_->md_clear(); return;
      case Preview: app_->toggle_preview(); return;
      case Settings: app_->show_settings_page(!app_->preferences_visible()); return;
      default: break;
    }
  }
  focus_ = 0;
  // A menu of the menu bar can hand over to another title (Left, Right,
  // the pointer, Alt and a letter): open that one next.
  while (id) {
    auto its = items();
    auto it = std::find_if(its.begin(), its.end(), [id](const Item &i) { return i.id == id && i.enabled; });
    if (it == its.end()) break;
    open_ = id;
    redraw();
    MenuSpec spec = app_->menu_spec(id);
    spec.keyboard = keyboard;
    spec.qa_name = qa_name(id);
    if (is_menu(id)) {
      auto link = std::make_shared<MenuBarLink>();
      link->current = id;
      link->title_at = [this](int rx, int ry) { return title_at_root(rx, ry); };
      link->title_for_key = [this](int key) { return menu_for_key(key); };
      link->neighbour = [this, id](int dir) { return neighbour(id, dir); };
      spec.bar = link;
    }
    app_->schedule_qa();
    int rx = window()->x_root() + int(it->x), ry = window()->y_root() + y() + h() - S(2);
    popup_menu(spec, rx, ry, h());
    open_ = -1;
    hover_ = -1;
    redraw();
    id = spec.bar ? spec.bar->next : 0;
    keyboard = spec.bar && spec.bar->next_keyboard;
  }
  app_->peek_menubar(false);
  app_->schedule_qa();
}

int Toolbar::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  auto find = [&]() {
    auto its = items();
    for (size_t i = 0; i < its.size(); ++i)
      if (mx >= its[i].x && mx < its[i].x + its[i].w && my >= y() && my < y() + h()) return int(i);
    return -1;
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int h = find();
      if (h != hover_) {
        hover_ = h;
        redraw();
        if (h >= 0) {
          auto its = items();
          if (its[size_t(h)].tip)
            Fl_Tooltip::enter_area(this, int(its[size_t(h)].x - x()), 0, int(its[size_t(h)].w), this->h(),
                                   its[size_t(h)].tip);
        }
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      int h = find();
      focus_ = 0;
      if (h >= 0) {
        int id = items()[size_t(h)].id;
        if (is_menu(id) || id == Heading || id == List) open(id);
      }
      return 1;
    }
    case FL_RELEASE: {
      int h = find();
      if (h >= 0) {
        int id = items()[size_t(h)].id;
        if (!(is_menu(id) || id == Heading || id == List)) open(id);
      }
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}

// -------------------------------------------------------------- StatusBar

StatusBar::StatusBar(App *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

std::vector<StatusBar::Segment> StatusBar::segments() const {
  std::vector<Segment> left, right;
  Document *d = app_->active();
  if (!d) return left;
  EditorView *ed = app_->editor();
  int pos = ed->insert_position();
  int line = d->line_of(pos);
  int ls = d->buf->line_start(pos);
  char *lt = d->buf->text_range(ls, pos);
  int col = int(utf8_length(lt, size_t(pos - ls))) + 1;
  free(lt);
  left.push_back({format(_("Ln %d, Col %d"), line, col), Position});
  long sel = 0;
  int a = 0, b = 0;
  if (d->buf->selection_position(&a, &b) && b > a) {
    char *t = d->buf->text_range(a, b);
    sel = utf8_length(t, size_t(b - a));
    free(t);
  }
  left.push_back({character_status(sel, d->char_count()), Characters, 0, 0, false});
  std::string type = is_plain(d->lang) ? _("Plain text") : language_name(d->lang);
  left.push_back({type, FileType});
  if (d->readonly) left.push_back({_("Read-only"), ReadOnly, 0, 0, false});
  const Settings &s = app_->settings();
  int base = std::max(7, int(std::lround(s.font_size * 4.0 / 3.0 * ui_scale())));
  right.push_back({format("%d%%", int(std::lround(100.0 * (base + s.zoom) / base))), Zoom});
  std::string eol = d->eol == Eol::CRLF ? _("Windows (CRLF)") : d->eol == Eol::CR ? _("Macintosh (CR)") : _("Unix (LF)");
  if (d->mixed_eol) eol += " *";
  right.push_back({eol, LineEnding});
  right.push_back({encoding_name(d->encoding), Encoding});

  const Look &L = look();
  text_font(L.theme.font_size - S(1));
  double xl = x() + S(8);
  const int min_left[] = {S(120), S(170), S(110), S(90)};
  for (size_t i = 0; i < left.size(); ++i) {
    left[i].w = std::max<double>(text_width(left[i].text) + S(28), min_left[std::min<size_t>(i, 3)]);
    left[i].x = xl;
    xl += left[i].w;
  }
  const int min_right[] = {S(64), S(130), S(96)};
  double xr = x() + w() - S(10);
  for (int i = int(right.size()) - 1; i >= 0; --i) {
    right[size_t(i)].w = std::max<double>(text_width(right[size_t(i)].text) + S(28), min_right[i]);
    xr -= right[size_t(i)].w;
    right[size_t(i)].x = xr;
  }
  // Narrow windows: drop the character count, then the file type.
  while (left.size() > 1 && left.back().x + left.back().w > right.front().x - S(8)) {
    if (left.size() >= 2 && left[1].id == Characters) left.erase(left.begin() + 1);
    else left.pop_back();
    double xx = x() + S(8);
    for (auto &sg : left) {
      sg.x = xx;
      xx += sg.w;
    }
  }
  std::vector<Segment> out = left;
  out.insert(out.end(), right.begin(), right.end());
  return out;
}

void StatusBar::draw() {
  const Look &L = look();
  auto sg = segments();
  Color hover_bg = L.dark ? Color::hex(0xFFFFFF, 0.07) : Color::hex(0x000000, 0.05);
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), L.status);
    p.line(x(), y() + 0.5, x() + w(), y() + 0.5, L.bar_line, 1);
    for (size_t i = 0; i < sg.size(); ++i) {
      if (int(i) == hover_ && sg[i].clickable)
        p.fill_rounded(sg[i].x + S(2), y() + S(4), sg[i].w - S(4), h() - S(8), Sd(4), hover_bg);
      // Separators between segments of a group.
      bool first_right = sg[i].id == Zoom;
      if (i > 0 || first_right)
        p.line(sg[i].x + 0.5, y() + S(8), sg[i].x + 0.5, y() + h() - S(8), L.border.alpha(0.9), 1);
    }
  }
  text_font(L.theme.font_size - S(1));
  for (size_t i = 0; i < sg.size(); ++i)
    draw_text(sg[i].text, int(sg[i].x + S(14)), y(), int(sg[i].w - S(20)), h(),
              int(i) == hover_ && sg[i].clickable ? L.text : L.text.mix(L.dim, 0.4));
  // Messages ("Saved", "Search wrapped") between the two groups.
  const std::string &msg = app_->toast_text();
  if (!msg.empty() && !sg.empty()) {
    double l = x(), r = x() + w();
    for (auto &s : sg) {
      if (s.id == Zoom) r = s.x;
      else if (s.id < Zoom || s.id == ReadOnly) l = std::max(l, s.x + s.w);
    }
    draw_text(msg, int(l + S(16)), y(), int(r - l - S(28)), h(), L.accent_text, 1);
  }
}

int StatusBar::handle(int event) {
  int mx = Fl::event_x();
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      auto sg = segments();
      int h = -1;
      for (size_t i = 0; i < sg.size(); ++i)
        if (mx >= sg[i].x && mx < sg[i].x + sg[i].w) h = int(i);
      if (h != hover_) {
        hover_ = h;
        redraw();
        if (h >= 0 && sg[size_t(h)].id == Characters) {
          Document *d = app_->active();
          static std::string tip;
          if (d) {
            const std::string &t = d->text();
            TextCounts c = count_text(t.data(), t.size());
            tip = format(_("%s words, %s lines"), group_thousands(c.words).c_str(), group_thousands(c.lines).c_str());
            Fl_Tooltip::enter_area(this, int(sg[size_t(h)].x - x()), 0, int(sg[size_t(h)].w), this->h(),
                                   tip.c_str());
          }
        }
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      auto sg = segments();
      for (auto &s : sg)
        if (mx >= s.x && mx < s.x + s.w && s.clickable) {
          app_->popup_status(s.id, window()->x_root() + int(s.x), window()->y_root() + y());
          hover_ = -1;
          redraw();
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

}  // namespace niny
