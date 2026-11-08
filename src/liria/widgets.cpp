// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The look, the small helpers and the bars of Liria's window: the tab strip
// with the caption buttons, the navigation buttons, the address bar with
// its suggestions, the search box and the command bar.
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>
#include <fcntl.h>
#include <glib.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "location.h"
#include "menu.h"
#include "ui.h"
#include "util.h"

namespace sde {
namespace liria {

// ------------------------------------------------------------------ look

namespace {
Look g_look;
bool g_look_ready = false;
}  // namespace

void update_look() {
  const Theme &t = active_theme();
  Look l;
  l.liquid = t.liquid;
  if (t.liquid) {
    // The chrome in the surface color (the glass shader turns exactly that
    // color into glass), the files on a card just beyond it, which the
    // shader keeps opaque.
    l.chrome = l.toolbar = t.surface;
    l.card = t.dark ? t.surface.mix(Color::hex(0x000000), 0.22) : t.surface.mix(Color::hex(0xFFFFFF), 1.0);
  } else {
    l.chrome = t.surface_alt;
    l.toolbar = t.background;
    l.card = t.surface;
  }
  l.card_border = t.border.mix(l.toolbar, 0.35);
  l.text = t.text;
  l.dim = t.text_dim;
  l.faint = t.text_dim.alpha(0.55);
  l.hover = t.hover;
  l.press = t.hover.mix(t.text, 0.08);
  l.accent = t.accent;
  l.accent_text = t.accent_text;
  l.select = t.accent.alpha(t.dark ? 0.30 : 0.16);
  l.select_line = t.accent.alpha(t.dark ? 0.65 : 0.45);
  g_look = l;
  g_look_ready = true;
}

const Look &look() {
  if (!g_look_ready) update_look();
  return g_look;
}

// ------------------------------------------------------------------ helpers

double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string thousands(long long n) {
  gchar *s = g_strdup_printf("%'lld", n);
  std::string out = s ? s : std::to_string(n);
  g_free(s);
  return out;
}

std::string display_path(const std::string &path) {
  if (!is_local(path)) return path;
  gchar *s = g_filename_display_name(home_relative(path).c_str());
  std::string out = s ? s : path;
  g_free(s);
  return out;
}

void draw_icon(Painter &p, const std::vector<std::string> &names, double x, double y, double size, double alpha) {
  cairo_surface_t *icon = IconLoader::get(names, int(std::lround(size)));
  if (icon) p.image(icon, x, y, size, alpha);
}

bool hovering(Fl_Widget *w) { return Fl::belowmouse() == w; }

unsigned fl_mods() {
  int st = Fl::event_state();
  unsigned m = 0;
  if (st & FL_SHIFT) m |= desk::MOD_SHIFT;
  if (st & FL_CTRL) m |= desk::MOD_CTRL;
  if (st & FL_ALT) m |= desk::MOD_ALT;
  return m;
}

Waker::Waker(std::function<void()> fn) : fn_(std::move(fn)) {
  if (pipe2(fds_, O_NONBLOCK | O_CLOEXEC) == 0) Fl::add_fd(fds_[0], FL_READ, ready, this);
  else fds_[0] = fds_[1] = -1;
}

Waker::~Waker() {
  if (fds_[0] >= 0) {
    Fl::remove_fd(fds_[0]);
    close(fds_[0]);
    close(fds_[1]);
  }
}

void Waker::wake() {
  if (fds_[1] < 0) return;
  char c = 1;
  ssize_t r = write(fds_[1], &c, 1);
  (void)r;
}

void Waker::ready(int fd, void *data) {
  char buf[256];
  while (read(fd, buf, sizeof buf) > 0) {
  }
  static_cast<Waker *>(data)->fn_();
}

// ------------------------------------------------------------------ NavButton

NavButton::NavButton(const char *g, const char *tip) : Fl_Widget(0, 0, 10, 10), glyph(g) {
  box(FL_NO_BOX);
  tooltip(tip);
  clear_visible_focus();
}

void NavButton::draw() {
  if (!fl_not_clipped(x(), y(), w(), h())) return;
  const Look &l = look();
  Painter p(window()->w(), window()->h());
  double r = active_theme().radius_for(h());
  if (active_r() && (down || hot)) p.fill_rounded(x(), y(), w(), h(), r, down ? l.press : l.hover);
  Color c = active_r() ? l.text : l.faint;
  double gs = std::round(std::min(w(), h()) * 0.52);
  liria::glyph(p, glyph, x() + std::round((w() - gs) / 2), y() + std::round((h() - gs) / 2), gs, c, 1.7);
}

int NavButton::handle(int e) {
  switch (e) {
    case FL_ENTER:
      hot = true;
      redraw();
      return 1;
    case FL_LEAVE:
      hot = false;
      down = false;
      redraw();
      return 1;
    case FL_PUSH:
      if (!active_r() || Fl::event_button() != FL_LEFT_MOUSE) return 1;
      down = true;
      redraw();
      return 1;
    case FL_DRAG: {
      bool in = Fl::event_inside(this);
      if (in != down) {
        down = in;
        redraw();
      }
      return 1;
    }
    case FL_RELEASE:
      if (down) {
        down = false;
        redraw();
        if (Fl::event_inside(this)) do_callback();
      }
      return 1;
  }
  return Fl_Widget::handle(e);
}

// ------------------------------------------------------------------ TabBar

TabBar::TabBar(MainWindow *w) : Fl_Widget(0, 0, 10, 10), w_(w) { box(FL_NO_BOX); }

void TabBar::relayout() {
  int n = w_->tab_count();
  button_w = S(46);
  int left = x() + S(6);
  pane_x = left;
  left += S(40) + S(4);
  caption_x = w_->csd() ? x() + w() - 3 * button_w : x() + w();
  int avail = caption_x - left - S(40) - S(48);  // room for "+" and a place to grab the window
  int tw = n > 0 ? std::min(S(240), std::max(S(96), avail / std::max(1, n))) : S(240);
  slots.assign(size_t(n), Slot());
  int tx = left;
  for (int i = 0; i < n; ++i) {
    slots[size_t(i)].x = tx;
    slots[size_t(i)].w = tw;
    slots[size_t(i)].close_x = tx + tw - S(30);
    tx += tw + S(2);
  }
  plus_x = tx + S(2);
}

int TabBar::tab_at(int mx) const {
  for (size_t i = 0; i < slots.size(); ++i)
    if (mx >= slots[i].x && mx < slots[i].x + slots[i].w) return int(i);
  return -1;
}

void TabBar::draw() {
  if (!fl_not_clipped(x(), y(), w(), h())) return;
  relayout();
  const Look &l = look();
  const Theme &t = active_theme();
  int ty = y() + S(6), th = h() - S(6);
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), l.chrome);
    // The pane toggle.
    if (hover_button == 0) p.fill_rounded(pane_x, ty + S(3), S(40), th - S(9), t.radius_for(th - S(9)), l.hover);
    liria::glyph(p, "sidebar", pane_x + (S(40) - S(18)) / 2.0, ty + (th - S(6) - S(18)) / 2.0 + S(1), S(18), l.dim);
    for (int i = 0; i < int(slots.size()); ++i) {
      const Slot &s = slots[size_t(i)];
      bool cur = i == w_->current_tab();
      int sx = s.x;
      if (drag_from == i) sx = std::max(slots[0].x, std::min(plus_x - s.w, s.x + drag_x));
      double r = std::max(Sd(6), double(t.radius));
      if (cur) {
        // The active tab joins the toolbar below it.
        cairo_t *cr = p.cr();
        p.set(l.toolbar);
        cairo_new_path(cr);
        cairo_move_to(cr, sx - r, ty + th);
        cairo_arc_negative(cr, sx - r, ty + th - r, r, M_PI / 2, 0);
        cairo_line_to(cr, sx, ty + r);
        cairo_arc(cr, sx + r, ty + r, r, M_PI, 3 * M_PI / 2);
        cairo_line_to(cr, sx + s.w - r, ty);
        cairo_arc(cr, sx + s.w - r, ty + r, r, -M_PI / 2, 0);
        cairo_line_to(cr, sx + s.w, ty + th - r);
        cairo_arc_negative(cr, sx + s.w + r, ty + th - r, r, M_PI, M_PI / 2);
        cairo_close_path(cr);
        cairo_fill(cr);
      } else if (i == hover && drag_from < 0) {
        p.fill_rounded(sx + S(2), ty + S(3), s.w - S(4), th - S(9), t.radius_for(th - S(9)), l.hover);
      } else if (i + 1 < int(slots.size()) && i + 1 != w_->current_tab() && i + 1 != hover) {
        p.line(sx + s.w + S(1) - 0.5, ty + S(10), sx + s.w + S(1) - 0.5, ty + th - S(14), t.border);
      }
      double is = S(16);
      draw_icon(p, location_icons(w_->tab_at(i).location), sx + S(12), ty + (th - is) / 2.0 - (cur ? 0 : S(1)), is);
      if (cur || i == hover) {
        int cx = s.close_x + (sx - s.x);
        if (i == hover && hover_close == i) p.fill_rounded(cx, ty + (th - S(24)) / 2.0, S(24), S(24), Sd(5), l.press);
        liria::glyph(p, "close", cx + S(5), ty + (th - S(14)) / 2.0, S(14), l.dim, 1.8);
      }
    }
    // "+": a new tab.
    if (hover_button == 1)
      p.fill_rounded(plus_x, ty + S(4), S(32), th - S(11), t.radius_for(th - S(11)), l.hover);
    liria::glyph(p, "plus", plus_x + S(8), ty + (th - S(6) - S(16)) / 2.0 + S(1), S(16), l.text);
    if (w_->csd()) {
      const char *names[3] = {"minimize", w_->maximized() ? "restore" : "maximize", "window-close"};
      for (int b = 0; b < 3; ++b) {
        int bx = caption_x + b * button_w;
        bool hot = hover_button == 2 + b;
        if (hot) p.fill_rect(bx, y(), button_w, h() - S(4), b == 2 ? t.caption_close : l.hover);
        Color g = hot && b == 2 ? Color::hex(0xFFFFFF) : t.caption_glyph;
        liria::glyph(p, names[b], bx + (button_w - S(16)) / 2.0, y() + (h() - S(4) - S(16)) / 2.0, S(16), g, 1.3);
      }
    }
  }
  for (int i = 0; i < int(slots.size()); ++i) {
    const Slot &s = slots[size_t(i)];
    bool cur = i == w_->current_tab();
    int sx = s.x;
    if (drag_from == i) sx = std::max(slots[0].x, std::min(plus_x - s.w, s.x + drag_x));
    text_font(t.font_size, false);
    int tx = sx + S(36), tw = s.w - S(36) - S(34);
    draw_text(w_->tab_at(i).title(), tx, ty, tw, th - (cur ? 0 : S(2)), cur ? l.text : l.dim);
  }
}

int TabBar::handle(int e) {
  int mx = Fl::event_x(), my = Fl::event_y();
  auto button_at = [&]() {
    if (mx >= pane_x && mx < pane_x + S(40) && my > y() + S(6)) return 0;
    if (mx >= plus_x && mx < plus_x + S(32) && my > y() + S(6)) return 1;
    if (w_->csd() && mx >= caption_x) return 2 + std::min(2, (mx - caption_x) / std::max(1, button_w));
    return -1;
  };
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      relayout();
      int hb = button_at();
      int ht = hb < 0 ? tab_at(mx) : -1;
      int hc = ht >= 0 && mx >= slots[size_t(ht)].close_x && mx < slots[size_t(ht)].close_x + S(24) ? ht : -1;
      if (hb != hover_button || ht != hover || hc != hover_close) {
        hover_button = hb;
        hover = ht;
        hover_close = hc;
        redraw();
        static std::string tip;
        if (ht >= 0) {
          const Tab &t = w_->tab_at(ht);
          tip = is_local(t.location) ? display_path(t.location) : t.title();
          Fl_Tooltip::enter_area(this, slots[size_t(ht)].x, y(), slots[size_t(ht)].w, h(), tip.c_str());
        } else if (hb >= 0) {
          static const char *tips[5] = {N_("Navigation pane"), N_("New tab (Ctrl+T)"), N_("Minimize"),
                                        N_("Maximize"), N_("Close")};
          tip = _(tips[hb]);
          if (hb == 3 && w_->maximized()) tip = _("Restore down");
          int bx = hb == 0 ? pane_x : hb == 1 ? plus_x : caption_x + (hb - 2) * button_w;
          Fl_Tooltip::enter_area(this, bx, y(), hb <= 1 ? S(36) : button_w, h(), tip.c_str());
        }
      }
      return 1;
    }
    case FL_LEAVE:
      hover = hover_close = hover_button = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      relayout();
      int b = Fl::event_button();
      int hb = button_at();
      int ti = hb < 0 ? tab_at(mx) : -1;
      if (b == FL_MIDDLE_MOUSE) {
        if (ti >= 0) w_->close_tab(ti);
        else if (hb < 0) w_->new_tab(kHome);
        return 1;
      }
      if (b == FL_RIGHT_MOUSE) {
        ContextMenu menu;
        int at = ti;
        menu.item(_("New tab"), [this] { w_->new_tab(kHome); }, {"tab-new", FL_CTRL + 't'});
        if (at >= 0) {
          menu.item(_("Duplicate tab"), [this, at] { w_->duplicate_tab(at); });
          menu.separator();
          menu.item(_("Close tab"), [this, at] { w_->close_tab(at); }, {"close", FL_CTRL + 'w'});
          menu.item(_("Close other tabs"), [this, at] {
            for (int i = w_->tab_count() - 1; i >= 0; --i)
              if (i != at) w_->close_tab(i);
          }, {"", 0, w_->tab_count() > 1});
          menu.item(_("Close tabs to the right"), [this, at] {
            for (int i = w_->tab_count() - 1; i > at; --i) w_->close_tab(i);
          }, {"", 0, at + 1 < w_->tab_count()});
        }
        menu.popup();
        return 1;
      }
      if (b != FL_LEFT_MOUSE) return 1;
      if (hb >= 0) {
        press = 100 + hb;
        return 1;
      }
      if (ti >= 0) {
        if (hover_close == ti) {
          press = 200 + ti;
          return 1;
        }
        w_->select_tab(ti);
        press = ti;
        drag_from = -1;
        drag_x = mx;
        return 1;
      }
      // The empty part of the title bar: moves the window; a double click
      // maximizes it.
      if (Fl::event_clicks()) {
        Fl::event_clicks(0);
        w_->toggle_maximize();
        return 1;
      }
      press = -2;
      drag_x = mx;
      return 1;
    }
    case FL_DRAG:
      if (press == -2 && std::abs(mx - drag_x) + std::abs(my - Fl::event_y()) >= 0) {
        if (w_->csd()) w_->start_move(FL_LEFT_MOUSE);
        press = -1;
        return 1;
      }
      if (press >= 0 && press < 100) {
        if (drag_from < 0 && std::abs(mx - drag_x) > S(8) && w_->tab_count() > 1) {
          drag_from = press;
          drag_x = 0;
        }
        if (drag_from >= 0) {
          drag_x = mx - (slots[size_t(drag_from)].x + slots[size_t(drag_from)].w / 2);
          redraw();
        }
      }
      return 1;
    case FL_RELEASE: {
      int was = press;
      press = -1;
      if (drag_from >= 0) {
        int from = drag_from;
        drag_from = -1;
        int to = tab_at(mx);
        if (to < 0) to = mx < slots.front().x ? 0 : w_->tab_count() - 1;
        if (to != from) w_->move_tab(from, to);
        redraw();
        return 1;
      }
      if (was >= 200) {
        int ti = was - 200;
        if (tab_at(mx) == ti) w_->close_tab(ti);
        return 1;
      }
      if (was >= 100 && button_at() == was - 100) {
        switch (was - 100) {
          case 0: w_->toggle_sidebar(); break;
          case 1: w_->new_tab(kHome); break;
          case 2: w_->minimize(); break;
          case 3: w_->toggle_maximize(); break;
          case 4: w_->do_callback(); break;
        }
      }
      return 1;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
      return 1;
  }
  return Fl_Widget::handle(e);
}

// ------------------------------------------------------------- AddressBar

namespace {

class AddressEntry : public Fl_Input {
 public:
  AddressEntry(AddressBar *bar) : Fl_Input(0, 0, 10, 10), bar_(bar) {
    box(FL_NO_BOX);
    when(FL_WHEN_CHANGED);
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int key = Fl::event_key();
      auto &sug = bar_->suggestions;
      switch (key) {
        case FL_Escape:
          if (bar_->list && bar_->list->visible() && !sug.empty()) {
            sug.clear();
            bar_->list->place();
          } else {
            bar_->stop_edit();
          }
          return 1;
        case FL_Down:
        case FL_Up:
          if (!sug.empty()) {
            int n = int(sug.size());
            bar_->suggestion = bar_->suggestion < 0 ? (key == FL_Down ? 0 : n - 1)
                                                    : (bar_->suggestion + (key == FL_Down ? 1 : -1) + n) % n;
            if (bar_->list) bar_->list->redraw();
            return 1;
          }
          break;
        case FL_Tab:
          if (!sug.empty()) {
            std::string pick = sug[size_t(std::max(0, bar_->suggestion))].text;
            value(pick.c_str());
            insert_position(size());
            bar_->update_suggestions();
            return 1;
          }
          break;
        case FL_Enter:
        case FL_KP_Enter: {
          std::string typed = value();
          if (bar_->suggestion >= 0 && bar_->suggestion < int(sug.size())) typed = sug[size_t(bar_->suggestion)].text;
          bar_->go_typed(typed);
          return 1;
        }
      }
    }
    if (e == FL_UNFOCUS) {
      int r = Fl_Input::handle(e);
      // Leaving the field (a click elsewhere) shows the breadcrumbs again.
      Fl::add_timeout(0.0, [](void *d) {
        auto *b = static_cast<AddressBar *>(d);
        if (b->editing && Fl::focus() != b->entry) b->stop_edit();
      }, bar_);
      return r;
    }
    return Fl_Input::handle(e);
  }

 private:
  AddressBar *bar_;
};

}  // namespace

AddressBar::AddressBar(MainWindow *w) : Fl_Group(0, 0, 10, 10), w_(w) {
  entry = new AddressEntry(this);
  entry->hide();
  entry->callback([](Fl_Widget *, void *d) { static_cast<AddressBar *>(d)->update_suggestions(); }, this);
  end();
  clip_children(1);
}

void AddressBar::go_typed(const std::string &text) {
  std::string t = text;
  stop_edit();
  w_->go_typed(t);
}

void AddressBar::rebuild() {
  crumbs.clear();
  hidden.clear();
  const Tab &tab = w_->tab();
  const std::string &loc = tab.location;
  App &app = App::get();
  if (is_special(loc)) {
    Crumb c;
    c.location = loc;
    c.label = location_title(loc);
    c.icons = location_icons(loc);
    c.current = true;
    crumbs.push_back(c);
  } else {
    std::vector<std::string> anchors = app.drives.mount_points();
    std::vector<std::string> chain = crumb_chain(loc, anchors);
    bool first = true;
    for (const auto &p : chain) {
      Crumb c;
      c.location = p;
      if (first) {
        c.icons = location_icons(p);
        if (is_local(p)) {
          const Drive *d = app.drives.drive_of(p);
          if (d && d->path == p && p != "/") {
            c.label = d->name;
            c.icons = d->icons;
          }
        }
      }
      if (c.label.empty()) c.label = location_title(p);
      c.current = p == loc;
      crumbs.push_back(c);
      first = false;
    }
  }
  if (!tab.query.empty()) {
    Crumb c;
    c.label = format(_("Search results in %s"), location_title(loc).c_str());
    c.current = true;  // no location: a click edits the address
    if (!crumbs.empty()) crumbs.back().current = false;
    crumbs.push_back(c);
  }
  layout_crumbs();
  redraw();
}

void AddressBar::layout_crumbs() {
  const Theme &t = active_theme();
  int sep = S(20), pad = S(7);
  int avail = w() - S(12);
  for (auto &c : crumbs) {
    text_font(t.font_size);
    c.w = std::min(S(220), text_width(c.label)) + 2 * pad + (c.icons.empty() ? 0 : S(22));
  }
  int total = 0;
  for (auto &c : crumbs) total += c.w + sep;
  hidden.clear();
  while (crumbs.size() > 1 && total > avail) {
    hidden.push_back(crumbs.front());
    total -= crumbs.front().w + sep;
    crumbs.erase(crumbs.begin());
    if (hidden.size() == 1) total += S(30) + sep;
  }
  if (!hidden.empty()) {
    Crumb more;
    more.label = "\xE2\x80\xA6";
    more.overflow = true;
    more.w = S(30);
    crumbs.insert(crumbs.begin(), more);
  }
  int cx = x() + S(4);
  for (auto &c : crumbs) {
    c.x = cx;
    c.sep_x = cx + c.w;
    cx += c.w + sep;
  }
  if (!crumbs.empty() && crumbs.back().x + crumbs.back().w > x() + w() - S(4))
    crumbs.back().w = std::max(S(40), x() + w() - S(4) - crumbs.back().x);
}

int AddressBar::crumb_at(int mx) const {
  for (size_t i = 0; i < crumbs.size(); ++i)
    if (mx >= crumbs[i].x && mx < crumbs[i].x + crumbs[i].w) return int(i);
  return -1;
}

int AddressBar::sep_at(int mx) const {
  for (size_t i = 0; i < crumbs.size(); ++i) {
    const Crumb &c = crumbs[i];
    bool chevron = i + 1 < crumbs.size() || (is_local(c.location) && !c.overflow);
    if (chevron && mx >= c.sep_x && mx < c.sep_x + S(20)) return int(i);
  }
  return -1;
}

void AddressBar::resize(int X, int Y, int W, int H) {
  Fl_Group::resize(X, Y, W, H);
  entry->resize(X + S(10), Y + 1, W - S(20), H - 2);
  layout_crumbs();
}

void AddressBar::start_edit(const std::string &text) {
  editing = true;
  error.clear();
  const Tab &tab = w_->tab();
  std::string value = text;
  if (value.empty()) {
    if (is_local(tab.location)) value = tab.location;
    else if (is_remote(tab.location)) value = tab.location;
  }
  entry->value(value.c_str());
  entry->show();
  entry->take_focus();
  entry->insert_position(entry->size(), 0);
  suggestions.clear();
  suggestion = -1;
  if (list) list->place();
  redraw();
  w_->schedule_qa();
}

void AddressBar::stop_edit() {
  if (!editing) return;
  editing = false;
  entry->hide();
  suggestions.clear();
  suggestion = -1;
  if (list) list->place();
  if (Fl::focus() == entry) w_->fileview()->canvas_focus();
  redraw();
  w_->schedule_qa();
}

void AddressBar::update_suggestions() {
  suggestions.clear();
  suggestion = -1;
  error.clear();
  std::string typed = entry->value();
  if (!typed.empty()) {
    App &app = App::get();
    for (const auto &s : app.servers.matching(typed)) {
      Suggestion sg;
      sg.text = s;
      sg.label = s;
      sg.detail = _("Recent server");
      sg.glyph = "server";
      suggestions.push_back(sg);
      if (suggestions.size() >= 4) break;
    }
    Address a = parse_address(typed);
    if (a.kind == Address::Host) {
      for (const auto &scheme : host_schemes()) {
        if (scheme == "smb") continue;
        Suggestion sg;
        sg.text = address_with_scheme(a, scheme);
        sg.label = sg.text;
        sg.detail = scheme_label(scheme);
        sg.glyph = "network";
        suggestions.push_back(sg);
      }
      Suggestion smb;
      smb.text = a.uri;
      smb.label = a.uri;
      smb.detail = _("Windows shares of this computer");
      smb.glyph = "network";
      suggestions.insert(suggestions.begin(), smb);
    } else if (a.kind == Address::Error) {
      error = a.message;
    } else if (a.kind == Address::Path) {
      std::string cwd = is_local(w_->tab().location) ? w_->tab().location : home_dir();
      for (const auto &c : chooser::complete_location(typed, cwd, true, App::get().prefs.show_hidden, 8)) {
        Suggestion sg;
        sg.text = c;
        std::string name = c;
        if (!name.empty() && name.back() == '/') name.pop_back();
        sg.label = basename_of(name);
        sg.detail = display_path(chooser::expand_location(c, cwd));
        sg.glyph = "folder";
        suggestions.push_back(sg);
      }
    }
  }
  if (list) list->place();
  redraw();
}

void AddressBar::draw() {
  if (!fl_not_clipped(x(), y(), w(), h())) return;
  const Look &l = look();
  const Theme &t = active_theme();
  {
    Painter p(window()->w(), window()->h());
    double r = t.radius_for(h());
    p.fill_rounded(x(), y(), w(), h(), r, l.card);
    if (editing) p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.accent, Sd(1.6));
    else p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, hovering(this) ? t.border.mix(t.text, 0.2) : t.border);
    if (!editing) {
      for (size_t i = 0; i < crumbs.size(); ++i) {
        const Crumb &c = crumbs[i];
        int cy = y() + S(4), ch = h() - S(8);
        if (int(i) == hover) p.fill_rounded(c.x, cy, c.w, ch, t.radius_for(ch), l.hover);
        if (!c.icons.empty()) draw_icon(p, c.icons, c.x + S(7), y() + (h() - S(16)) / 2.0, S(16));
        bool chevron = i + 1 < crumbs.size() || (is_local(c.location) && !c.overflow);
        if (chevron) {
          if (int(i) == hover_sep) p.fill_rounded(c.sep_x + S(1), cy, S(18), ch, t.radius_for(ch), l.hover);
          liria::glyph(p, "chevron-right", c.sep_x + S(3), y() + (h() - S(14)) / 2.0, S(14), l.dim, 1.7);
        }
      }
    }
  }
  if (editing) {
    Fl_Input *in = entry;
    in->color(l.card.fl());
    in->textcolor(t.text.fl());
    in->selection_color(t.accent.fl());
    in->cursor_color(t.accent.fl());
    in->textfont(FL_HELVETICA);
    in->textsize(t.font_size);
    draw_child(*in);
    return;
  }
  for (size_t i = 0; i < crumbs.size(); ++i) {
    const Crumb &c = crumbs[i];
    text_font(t.font_size, false);
    int tx = c.x + S(7) + (c.icons.empty() ? 0 : S(22));
    draw_text(c.label, tx, y(), c.w - (tx - c.x) - S(6), h(), l.text, c.overflow ? 1 : 0);
  }
}

int AddressBar::handle(int e) {
  if (editing) {
    if (e == FL_PUSH && !Fl::event_inside(entry)) {
      stop_edit();
      return 1;
    }
    return Fl_Group::handle(e);
  }
  int mx = Fl::event_x();
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int hv = crumb_at(mx), hs = sep_at(mx);
      if (hv != hover || hs != hover_sep) {
        hover = hv;
        hover_sep = hs;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover = hover_sep = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      int b = Fl::event_button();
      if (b == FL_RIGHT_MOUSE) {
        ContextMenu menu;
        std::string loc = w_->tab().location;
        menu.item(_("Copy address"), [loc] {
          std::string text = is_local(loc) ? loc : loc;
          Fl::copy(text.c_str(), int(text.size()), 1);
          Fl::copy(text.c_str(), int(text.size()), 0);
        }, {"copy"});
        menu.item(_("Edit address"), [this] { start_edit(); }, {"rename", FL_CTRL + 'l'});
        menu.popup();
        return 1;
      }
      if (b != FL_LEFT_MOUSE) return 1;
      int s = sep_at(mx);
      if (s >= 0) {
        // The folders inside that crumb's folder.
        const std::string dir = crumbs[size_t(s)].location;
        std::vector<std::pair<std::string, std::string>> subs;
        if (is_local(dir)) {
          for (const auto &name : list_dir(dir)) {
            if (name.empty() || name[0] == '.') continue;
            std::string p = dir == "/" ? "/" + name : dir + "/" + name;
            if (dir_exists(p)) subs.push_back({location_title(p), p});
          }
        }
        std::sort(subs.begin(), subs.end());
        ContextMenu menu;
        int n = 0;
        for (const auto &sp : subs) {
          if (++n > 60) break;
          std::string p = sp.second;
          menu.item(sp.first, [this, p] { w_->go(p); }, {"folder"});
        }
        if (menu.empty()) menu.item(_("No folders"), [] {}, {"", 0, false});
        menu.popup(crumbs[size_t(s)].sep_x, y() + h() + S(2));
        return 1;
      }
      int k = crumb_at(mx);
      if (k < 0) {
        start_edit();
        return 1;
      }
      if (crumbs[size_t(k)].overflow) {
        ContextMenu menu;
        for (auto it = hidden.rbegin(); it != hidden.rend(); ++it) {
          std::string p = it->location;
          menu.item(it->label, [this, p] { w_->go(p); }, {"folder"});
        }
        menu.popup(crumbs[size_t(k)].x, y() + h() + S(2));
        return 1;
      }
      const std::string &loc = crumbs[size_t(k)].location;
      if (loc.empty() || (crumbs[size_t(k)].current && w_->tab().query.empty())) start_edit();
      else w_->go(loc);
      return 1;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
      return 1;
  }
  return Fl_Group::handle(e);
}

// ------------------------------------------------------- SuggestionList

SuggestionList::SuggestionList(MainWindow *w) : Fl_Widget(0, 0, 10, 10), w_(w) { hide(); }

int SuggestionList::row_h() const { return S(36); }

int SuggestionList::rows() const {
  AddressBar *bar = w_->addressbar();
  int n = int(bar->suggestions.size()) + (bar->error.empty() ? 0 : 1);
  return std::min(n, 8);
}

void SuggestionList::place() {
  AddressBar *bar = w_->addressbar();
  int n = rows();
  if (!bar->editing || n == 0) {
    if (visible()) {
      hide();
      w_->redraw();
    }
    return;
  }
  resize(bar->x(), bar->y() + bar->h() + S(4), std::max(bar->w(), S(360)), n * row_h() + S(8));
  show();
  w_->redraw();
}

void SuggestionList::draw() {
  AddressBar *bar = w_->addressbar();
  const Theme &t = active_theme();
  const Look &l = look();
  {
    Painter p(window()->w(), window()->h());
    p.shadow(x(), y(), w(), h(), t.radius, t.shadow, S(10));
    p.fill_rounded(x(), y(), w(), h(), t.radius, t.menu_bg);
    p.stroke_rounded(x(), y(), w(), h(), t.radius, t.menu_border);
    int row = 0;
    if (!bar->error.empty()) {
      liria::glyph(p, "warning", x() + S(14), y() + S(4) + (row_h() - S(18)) / 2.0, S(18), t.warning);
      ++row;
    }
    for (size_t i = 0; i < bar->suggestions.size() && row < rows(); ++i, ++row) {
      int ry = y() + S(4) + row * row_h();
      bool cur = int(i) == bar->suggestion, hot = int(i) == hover;
      if (cur || hot)
        p.fill_rounded(x() + S(4), ry, w() - S(8), row_h(), t.radius_for(row_h()), cur ? t.menu_hover : l.hover);
      liria::glyph(p, bar->suggestions[i].glyph, x() + S(14), ry + (row_h() - S(18)) / 2.0, S(18),
                   cur ? t.menu_hover_text : l.dim);
    }
  }
  int row = 0;
  if (!bar->error.empty()) {
    text_font(t.font_size - 1);
    draw_text(bar->error, x() + S(42), y() + S(4), w() - S(52), row_h(), t.text);
    ++row;
  }
  for (size_t i = 0; i < bar->suggestions.size() && row < rows(); ++i, ++row) {
    int ry = y() + S(4) + row * row_h();
    bool cur = int(i) == bar->suggestion;
    const auto &sg = bar->suggestions[i];
    text_font(t.font_size);
    int lw = std::min(text_width(sg.label), (w() - S(60)) * 6 / 10);
    draw_text(sg.label, x() + S(42), ry, lw + S(2), row_h(), cur ? t.menu_hover_text : t.menu_text);
    text_font(t.font_size - 1);
    draw_text(sg.detail, x() + S(42) + lw + S(14), ry, w() - S(60) - lw - S(14), row_h(),
              cur ? t.menu_hover_text : t.text_dim);
  }
}

int SuggestionList::handle(int e) {
  AddressBar *bar = w_->addressbar();
  int k = (Fl::event_y() - y() - S(4)) / row_h() - (bar->error.empty() ? 0 : 1);
  switch (e) {
    case FL_ENTER:
    case FL_MOVE:
      if (k != hover) {
        hover = k;
        redraw();
      }
      return 1;
    case FL_LEAVE:
      hover = -1;
      redraw();
      return 1;
    case FL_PUSH:
      if (k >= 0 && k < int(bar->suggestions.size())) bar->go_typed(bar->suggestions[size_t(k)].text);
      return 1;
  }
  return Fl_Widget::handle(e);
}

// ------------------------------------------------------------- SearchBox

SearchBox::SearchBox(MainWindow *w) : Fl_Input(0, 0, 10, 10), w_(w) {
  box(FL_NO_BOX);
  when(FL_WHEN_CHANGED);
  callback([](Fl_Widget *wd, void *d) {
    static_cast<MainWindow *>(d)->set_query(static_cast<SearchBox *>(wd)->value());
  }, w);
}

void SearchBox::draw() {
  if (!fl_not_clipped(x(), y(), w(), h())) return;
  const Look &l = look();
  const Theme &t = active_theme();
  bool focused = Fl::focus() == this;
  {
    Painter p(window()->w(), window()->h());
    double r = t.radius_for(h());
    p.fill_rounded(x(), y(), w(), h(), r, l.card);
    if (focused) p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.accent, Sd(1.6));
    else p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, hovering(this) ? t.border.mix(t.text, 0.2) : t.border);
    if (size() > 0) liria::glyph(p, "close", x() + w() - S(28), y() + (h() - S(14)) / 2.0, S(14), l.dim, 1.8);
    else liria::glyph(p, "search", x() + w() - S(28), y() + (h() - S(16)) / 2.0, S(16), l.dim);
  }
  color(l.card.fl());
  textcolor(t.text.fl());
  selection_color(t.accent.fl());
  cursor_color(t.accent.fl());
  textfont(FL_HELVETICA);
  textsize(t.font_size);
  int dx = S(12), dy = S(3);
  drawtext(x() + dx, y() + dy, w() - dx - S(36), h() - 2 * dy);
  if (size() == 0 && !focused) {
    text_font(t.font_size);
    draw_text(placeholder, x() + dx + S(1), y(), w() - dx - S(40), h(), t.text_dim);
  }
}

int SearchBox::handle(int e) {
  if (e == FL_PUSH && Fl::event_x() >= x() + w() - S(34) && size() > 0) {
    value("");
    do_callback();
    take_focus();
    return 1;
  }
  if (e == FL_KEYBOARD) {
    int key = Fl::event_key();
    if (key == FL_Escape) {
      if (size() > 0) {
        value("");
        do_callback();
      } else {
        w_->fileview()->canvas_focus();
      }
      return 1;
    }
    if (key == FL_Enter || key == FL_KP_Enter) {
      w_->start_deep_search();
      return 1;
    }
    if (key == FL_Down) {
      w_->fileview()->canvas_focus();
      return 1;
    }
    if (w_->shortcut(key)) return 1;
  }
  if (e == FL_FOCUS || e == FL_UNFOCUS || e == FL_ENTER || e == FL_LEAVE) redraw();
  // Keep FLTK's text positions inside the rounded field.
  int r;
  {
    int ox = x(), ow = w();
    resize(ox + S(12), y(), ow - S(48), h());
    r = Fl_Input::handle(e);
    resize(ox, y(), ow, h());
  }
  return r;
}

// ------------------------------------------------------------- CommandBar

CommandBar::CommandBar(MainWindow *w) : Fl_Widget(0, 0, 10, 10), w_(w) { box(FL_NO_BOX); }

const CommandBar::Button *CommandBar::find(const std::string &id) const {
  for (const auto &b : buttons)
    if (b.id == id) return &b;
  return nullptr;
}

void CommandBar::rebuild() {
  const Theme &t = active_theme();
  Tab &tab = w_->tab();
  std::vector<Item> sel = w_->selected_items();
  bool folder = tab.is_folder();
  bool trash = tab.location == kTrash;
  bool writable = w_->can_write_here();
  bool any = !sel.empty();
  bool home = tab.location == kHome;
  buttons.clear();
  auto add = [&](const char *id, const char *g, const std::string &label, const std::string &tip, bool enabled,
                 bool dropdown = false, bool sep = false) {
    Button b;
    b.id = id;
    b.glyph = g;
    b.label = label;
    b.tip = tip;
    b.enabled = enabled;
    b.dropdown = dropdown;
    b.separator_before = sep;
    buttons.push_back(b);
  };
  add("new", "new", _("New"), _("Create a folder or a document (Ctrl+Shift+N)"), folder && writable && !trash, true);
  add("cut", "cut", "", _("Cut (Ctrl+X)"), any && !trash && !home, false, true);
  add("copy", "copy", "", _("Copy (Ctrl+C)"), any && !home);
  add("paste", "paste", "", _("Paste (Ctrl+V)"), folder && writable && !trash && w_->can_paste());
  add("rename", "rename", "", _("Rename (F2)"), sel.size() == 1 && !trash && !home);
  add("share", "share", "", _("Share"), any && !home, false);
  add("delete", "delete", "", trash ? _("Delete permanently (Delete)") : _("Delete (Delete)"), any && !home);
  add("sort", "sort", _("Sort"), _("Sort the items"), folder || trash, true, true);
  add("view", "view", _("View"), _("Layout and options of the view"), true, true);
  // Contextual commands: only when they apply.
  bool sep = true;
  auto ctx = [&](const char *id, const char *g, const std::string &label, const std::string &tip) {
    add(id, g, label, tip, true, false, sep);
    buttons.back().accent = true;
    sep = false;
  };
  auto all_of = [&](auto pred) {
    return any && std::all_of(sel.begin(), sel.end(), pred);
  };
  static const std::set<std::string> archives = {
      "application/zip", "application/x-tar", "application/x-compressed-tar", "application/x-bzip-compressed-tar",
      "application/x-xz-compressed-tar", "application/x-7z-compressed", "application/vnd.rar", "application/x-rar",
      "application/gzip", "application/x-zstd-compressed-tar", "application/x-lzma-compressed-tar",
      "application/x-bzip2-compressed-tar", "application/x-zip-compressed", "application/x-cpio"};
  if (trash) {
    if (any) ctx("restore", "restore-item", _("Restore"), _("Put the selected items back where they were"));
    ctx("empty-trash", "empty-trash", _("Empty Trash"), _("Delete everything in the Trash for good"));
  } else if (all_of([&](const Item &i) { return !i.dir && archives.count(i.mime); })) {
    ctx("extract", "extract", _("Extract all"), _("Extract the archive into this folder with Taca"));
  } else if (sel.size() == 1 && (sel[0].mime == "application/vnd.debian.binary-package" ||
                                  sel[0].mime == "application/x-deb")) {
    ctx("install", "install", _("Install"), _("See what the package contains and changes, then install it (Debrun)"));
  } else if (sel.size() == 1 &&
             (sel[0].mime == "application/vnd.appimage" || sel[0].mime == "application/x-iso9660-appimage")) {
    ctx("run-appimage", "appimage", _("Run"), _("Verify the AppImage, then run it"));
  } else if (tab.location == kNetwork) {
    ctx("connect", "connect", _("Connect to server"), _("Open a share or a server on the network"));
  } else if (folder && is_local(tab.location)) {
    const Drive *d = App::get().drives.drive_of(tab.location);
    if (d && d->path == tab.location && (d->can_eject || d->can_unmount) && !d->system)
      ctx("eject", "eject", d->can_eject ? _("Eject") : _("Unmount"), _("Remove the drive safely"));
  }
  add("more", "more", "", _("See more"), true, false, true);
  // Layout: labels where there is room, icon only otherwise.
  auto width_of = [&](const Button &b, bool labels) {
    int bw = S(40);
    if (labels && !b.label.empty()) {
      text_font(t.font_size);
      bw = S(36) + text_width(b.label) + S(12);
    }
    if (b.dropdown) bw += S(16);
    return bw;
  };
  for (int pass = 0; pass < 2; ++pass) {
    bool labels = pass == 0;
    int cx = x() + S(8);
    for (auto &b : buttons) {
      if (b.separator_before) cx += S(13);
      b.x = cx;
      b.w = width_of(b, labels);
      cx += b.w + S(2);
    }
    if (cx <= x() + w() - S(8) || pass == 1) break;
    for (auto &b : buttons)
      if (b.id != "new" && !b.accent) b.label.clear();
  }
  redraw();
}

int CommandBar::button_at(int mx, int my) const {
  if (my < y() + S(4) || my >= y() + h() - S(4)) return -1;
  for (size_t i = 0; i < buttons.size(); ++i)
    if (mx >= buttons[i].x && mx < buttons[i].x + buttons[i].w) return int(i);
  return -1;
}

void CommandBar::draw() {
  if (!fl_not_clipped(x(), y(), w(), h())) return;
  const Look &l = look();
  const Theme &t = active_theme();
  int by = y() + S(5), bh = h() - S(10);
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), l.toolbar);
    p.line(x(), y() + h() - 0.5, x() + w(), y() + h() - 0.5, l.card_border);
    for (size_t i = 0; i < buttons.size(); ++i) {
      const Button &b = buttons[i];
      if (b.separator_before) p.line(b.x - S(7) + 0.5, by + S(8), b.x - S(7) + 0.5, by + bh - S(8), t.border);
      if (b.enabled && (int(i) == hover || int(i) == press))
        p.fill_rounded(b.x, by, b.w, bh, t.radius_for(bh), int(i) == press ? l.press : l.hover);
      Color c = !b.enabled ? l.faint : b.accent ? l.accent_text : l.text;
      double gs = S(18);
      liria::glyph(p, b.glyph, b.x + S(11), by + (bh - gs) / 2.0, gs, c, 1.6);
      if (b.dropdown) liria::glyph(p, "chevron-down", b.x + b.w - S(20), by + (bh - S(12)) / 2.0, S(12), c, 1.8);
    }
  }
  for (const auto &b : buttons) {
    if (b.label.empty()) continue;
    text_font(t.font_size);
    draw_text(b.label, b.x + S(36), by, b.w - S(36) - (b.dropdown ? S(20) : S(8)), bh,
              b.enabled ? (b.accent ? l.accent_text : l.text) : l.faint);
  }
}

int CommandBar::handle(int e) {
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int k = button_at(mx, my);
      if (k != hover) {
        hover = k;
        redraw();
        if (k >= 0) {
          static std::string tip;
          tip = buttons[size_t(k)].tip;
          Fl_Tooltip::enter_area(this, buttons[size_t(k)].x, y(), buttons[size_t(k)].w, h(), tip.c_str());
        }
      }
      return 1;
    }
    case FL_LEAVE:
      hover = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      int k = button_at(mx, my);
      if (k < 0 || !buttons[size_t(k)].enabled || Fl::event_button() != FL_LEFT_MOUSE) return 1;
      press = k;
      redraw();
      const Button b = buttons[size_t(k)];
      if (b.dropdown || b.id == "more" || b.id == "share") {
        // Menus open on the press, as menus do.
        int ax = b.x, ay = y() + h() - S(2);
        if (b.id == "new") w_->new_menu(this);
        else if (b.id == "sort") w_->sort_menu(ax, ay);
        else if (b.id == "view") w_->view_menu(ax, ay);
        else if (b.id == "share") w_->share_menu(this, ax, ay);
        else w_->more_menu(ax, ay);
        press = -1;
        redraw();
      }
      return 1;
    }
    case FL_RELEASE: {
      int k = press;
      press = -1;
      redraw();
      if (k >= 0 && button_at(mx, my) == k) w_->command(buttons[size_t(k)].id);
      return 1;
    }
  }
  return Fl_Widget::handle(e);
}

}  // namespace liria
}  // namespace sde
