// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Home page, as in the File Explorer of Windows 11:
//
//   Quick access       the user's pins as cards (each with its pin mark);
//                      nothing is added here by Liria. Pins move by
//                      dragging, a dropped folder is pinned, files dropped
//                      on a folder card go into that folder.
//   Drives             a card for every drive with its usage bar.
//   Network locations  the shares GVfs mounted and the recent servers; when
//                      there are none, a quiet note with "Turn off".
//   Recent files       the recently used files of every program
//                      (recently-used.xbel), newest first; one or all can be
//                      forgotten and the section can be turned off.
//
// Sections fold with their headings (remembered in liria.conf). The page
// scrolls inside an Fl_Scroll.
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "location.h"
#include "menu.h"
#include "openwith.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {
namespace liria {

class HomeCanvas : public Fl_Widget {
 public:
  explicit HomeCanvas(HomeView *h) : Fl_Widget(0, 0, 10, 10), h_(h) {}
  int press = -1, press_x = 0, press_y = 0;
  bool dragging = false;
  void draw() override;
  int handle(int e) override;
  void damage_card(int i);
  void activate(int i, bool new_tab);
  void menu(int i, int x, int y);

 private:
  HomeView *h_;
};

namespace {

std::string pin_location(const std::string &uri) {
  std::string loc = normalize_location(uri);
  if (is_remote(loc)) {
    std::string local = local_path_of_uri(loc);
    if (!local.empty()) return local;
  }
  return loc;
}

std::string usage_text(long long free_b, long long total) {
  if (total <= 0) return "";
  return format(_("%s free of %s"), chooser::size_text(free_b).c_str(), chooser::size_text(total).c_str());
}

}  // namespace

HomeView::HomeView(MainWindow *w) : Fl_Scroll(0, 0, 10, 10), w_(w) {
  type(Fl_Scroll::VERTICAL);
  box(FL_NO_BOX);
  canvas = new HomeCanvas(this);
  end();
}

void HomeView::rebuild() {
  App &app = App::get();
  const Prefs &pr = app.prefs;
  cards.clear();
  auto header = [&](const std::string &section, const std::string &label) {
    Card c;
    c.kind = Card::Header;
    c.section = section;
    c.label = label;
    cards.push_back(c);
  };
  header("quick", _("Quick access"));
  if (pr.home_quick) {
    int index = 0;
    for (const auto &pin : app.pins.items()) {
      Card c;
      c.kind = Card::Pin;
      c.section = "quick";
      c.id = pin.uri;
      c.location = pin_location(pin.uri);
      c.label = pin.label.empty() ? location_title(c.location) : pin.label;
      c.icons = location_icons(c.location);
      c.index = index++;
      if (is_local(c.location) && !dir_exists(c.location)) {
        chooser::Entry e;
        if (chooser::entry_for_path(c.location, e)) {
          c.icons = chooser::mime_icon_names(e.mime);
          c.mime = e.mime;
          c.path = c.location;
        }
      }
      cards.push_back(c);
    }
    if (app.pins.items().empty()) {
      Card c;
      c.kind = Card::Empty;
      c.section = "quick";
      c.label = _("Pin folders here with “Pin to Quick access” in their menu, or drag them here.");
      cards.push_back(c);
    }
  }
  header("drives", _("Drives"));
  if (pr.home_drives) {
    for (const auto &d : app.drives.drives()) {
      Card c;
      c.kind = Card::Drive;
      c.section = "drives";
      c.id = d.id;
      c.label = d.name;
      c.location = d.path;
      c.icons = d.icons;
      c.total = d.total;
      c.free = d.free;
      c.sub = d.mounted ? usage_text(d.free, d.total) : std::string(_("Not mounted: click to open"));
      cards.push_back(c);
    }
  }
  if (pr.network_shown) {
    header("network", _("Network locations"));
    if (pr.home_network) {
      std::set<std::string> shown;
      for (const auto &d : app.drives.network()) {
        Card c;
        c.kind = Card::Network;
        c.section = "network";
        c.id = d.id;
        c.label = d.name;
        c.location = d.path.empty() ? d.uri : d.path;
        c.icons = d.icons;
        c.sub = d.uri;
        cards.push_back(c);
        shown.insert(server_root(d.uri));
      }
      for (const auto &s : app.servers.items()) {
        if (shown.count(s)) continue;
        Card c;
        c.kind = Card::Server;
        c.section = "network";
        c.id = s;
        c.location = s;
        c.label = uri_basename(s);
        c.sub = s;
        c.icons = {"folder-remote", "network-server"};
        cards.push_back(c);
      }
      if (app.drives.network().empty() && app.servers.items().empty()) {
        Card c;
        c.kind = Card::Empty;
        c.section = "network";
        c.label = _("There are no network locations. If you don't use network locations, you can turn this section off.");
        cards.push_back(c);
        Card b;
        b.kind = Card::Button;
        b.section = "network";
        b.id = "turn-off-network";
        b.label = _("Turn off");
        cards.push_back(b);
      }
    }
  }
  if (pr.recent_shown) {
    header("recent", _("Recent files"));
    if (pr.home_recent) {
      recent = chooser::read_recent(20);
      for (const auto &r : recent) {
        Card c;
        c.kind = Card::Recent;
        c.section = "recent";
        c.id = r.path;
        c.path = r.path;
        c.location = r.path;
        c.label = location_title(r.path);
        c.sub = display_path(chooser::parent_dir(r.path));
        c.mime = r.mime.empty() ? chooser::guess_mime(r.path, false) : r.mime;
        c.icons = chooser::mime_icon_names(c.mime);
        c.time = r.time;
        cards.push_back(c);
      }
      if (recent.empty()) {
        Card c;
        c.kind = Card::Empty;
        c.section = "recent";
        c.label = _("Files you open appear here.");
        cards.push_back(c);
      }
    }
  }
  relayout();
}

void HomeView::relayout() {
  int sb = Fl::scrollbar_size() ? Fl::scrollbar_size() : S(14);
  int pad = S(20);
  int width = std::max(S(200), w() - sb - 2 * pad);
  int y = S(12);
  int cx = 0;
  int row_h = 0;
  std::string section;
  auto newline = [&]() {
    if (cx > 0) {
      y += row_h + S(8);
      cx = 0;
      row_h = 0;
    }
  };
  for (auto &c : cards) {
    switch (c.kind) {
      case Card::Header:
        newline();
        if (c.section != "quick") y += S(10);
        c.x = pad;
        c.y = y;
        c.w = width;
        c.h = S(32);
        y += S(38);
        break;
      case Card::Pin:
      case Card::Drive:
      case Card::Network:
      case Card::Server: {
        int cw = c.kind == Card::Pin ? S(136) : S(250), ch = c.kind == Card::Pin ? S(80) : S(72);
        if (cx > 0 && cx + cw > width) newline();
        c.x = pad + cx;
        c.y = y;
        c.w = cw;
        c.h = ch;
        cx += cw + S(8);
        row_h = std::max(row_h, ch);
        break;
      }
      case Card::Empty:
        newline();
        c.x = pad;
        c.y = y;
        c.w = width;
        c.h = S(48);
        y += S(56);
        break;
      case Card::Button: {
        // Inside the note above it, on its right.
        Card *note = nullptr;
        for (auto &o : cards)
          if (o.kind == Card::Empty && o.section == c.section) note = &o;
        const Theme &t = active_theme();
        text_font(t.font_size);
        c.w = text_width(c.label) + S(28);
        c.h = S(30);
        if (note) {
          c.x = note->x + note->w - c.w - S(10);
          c.y = note->y + (note->h - c.h) / 2;
        }
        break;
      }
      case Card::Recent:
        newline();
        c.x = pad;
        c.y = y;
        c.w = width;
        c.h = S(40);
        y += S(42);
        break;
    }
  }
  newline();
  content_h = y + S(20);
  canvas->resize(x(), this->y() - yposition(), std::max(10, w() - sb), std::max(h(), content_h));
  redraw();
}

void HomeView::resize(int X, int Y, int W, int H) {
  bool sized = W != w();
  Fl_Scroll::resize(X, Y, W, H);
  if (sized || canvas->w() < 20) relayout();
}

int HomeView::canvas_x() const { return canvas->x(); }
int HomeView::canvas_y() const { return canvas->y(); }
void HomeView::focus_canvas() { canvas->take_focus(); }

int HomeView::card_at(int mx, int my) const {
  int cx = mx - canvas->x(), cy = my - canvas->y();
  // Buttons first: they lie on their notes.
  for (int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i < cards.size(); ++i) {
      const Card &c = cards[i];
      if ((pass == 0) != (c.kind == Card::Button)) continue;
      if (cx >= c.x && cx < c.x + c.w && cy >= c.y && cy < c.y + c.h) return int(i);
    }
  return -1;
}

void HomeView::draw() {
  if (!fl_not_clipped(x(), y(), w(), h())) return;
  Fl_Scroll::draw();
}

void HomeCanvas::damage_card(int i) {
  if (i < 0 || i >= int(h_->cards.size())) return;
  const auto &c = h_->cards[size_t(i)];
  static_cast<MainWindow *>(window())->damage_rect(x() + c.x - S(4), y() + c.y - S(4), c.w + S(8), c.h + S(8));
}

void HomeCanvas::draw() {
  const Look &l = look();
  const Theme &t = active_theme();
  App &app = App::get();
  fl_push_clip(h_->x(), h_->y(), h_->w(), h_->h());
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(h_->x(), h_->y(), h_->w(), h_->h(), l.card);
    for (size_t i = 0; i < h_->cards.size(); ++i) {
      const auto &c = h_->cards[i];
      int cx = x() + c.x, cy = y() + c.y;
      if (!fl_not_clipped(cx, cy, c.w, c.h)) continue;
      bool hot = int(i) == h_->hover, drop = int(i) == h_->drop_card;
      double r = t.radius_small + S(2);
      switch (c.kind) {
        case HomeView::Card::Header: {
          bool open = c.section == "quick"     ? app.prefs.home_quick
                      : c.section == "drives"  ? app.prefs.home_drives
                      : c.section == "network" ? app.prefs.home_network
                                               : app.prefs.home_recent;
          if (hot) p.fill_rounded(cx - S(6), cy, S(30), c.h, t.radius_for(c.h), l.hover);
          liria::glyph(p, open ? "chevron-up" : "chevron-down", cx + S(2), cy + (c.h - S(14)) / 2.0, S(14), l.dim, 1.8);
          if (drop) p.stroke_rounded(cx - S(6), cy, c.w + S(6), c.h, t.radius_for(c.h), l.accent, Sd(1.5));
          if (c.section == "recent" || c.section == "network")
            liria::glyph(p, "more", cx + c.w - S(26), cy + (c.h - S(16)) / 2.0, S(16), l.dim);
          break;
        }
        case HomeView::Card::Pin: {
          Color fill = hot ? l.hover : t.dark ? l.card.mix(Color::hex(0xFFFFFF), 0.04) : l.card.mix(t.text, 0.025);
          p.fill_rounded(cx, cy, c.w, c.h, r, fill);
          p.stroke_rounded(cx + 0.5, cy + 0.5, c.w - 1, c.h - 1, r, drop ? l.accent : l.card_border, drop ? Sd(2) : 1.0);
          if (Fl::focus() == this && int(i) == h_->focus) focus_ring(p, cx, cy, c.w, c.h, r);
          draw_icon(p, c.icons, cx + (c.w - S(36)) / 2.0, cy + S(10), S(36));
          liria::glyph(p, "pin", cx + c.w - S(20), cy + S(6), S(13), l.dim, 1.5);
          break;
        }
        case HomeView::Card::Drive:
        case HomeView::Card::Network:
        case HomeView::Card::Server: {
          Color fill = hot ? l.hover : t.dark ? l.card.mix(Color::hex(0xFFFFFF), 0.04) : l.card.mix(t.text, 0.025);
          p.fill_rounded(cx, cy, c.w, c.h, r, fill);
          p.stroke_rounded(cx + 0.5, cy + 0.5, c.w - 1, c.h - 1, r, drop ? l.accent : l.card_border, drop ? Sd(2) : 1.0);
          if (Fl::focus() == this && int(i) == h_->focus) focus_ring(p, cx, cy, c.w, c.h, r);
          draw_icon(p, c.icons, cx + S(12), cy + (c.h - S(40)) / 2.0, S(40), c.kind == HomeView::Card::Server ? 0.7 : 1.0);
          if (c.kind == HomeView::Card::Drive && c.total > 0) {
            double bx = cx + S(64), bw = c.w - S(64) - S(14), by = cy + S(36);
            double used = 1.0 - double(c.free) / double(c.total);
            used = std::max(0.0, std::min(1.0, used));
            p.fill_rounded(bx, by, bw, Sd(5), Sd(2.5), t.border.mix(l.card, 0.2));
            Color bar = used > 0.9 ? t.error : l.accent;
            if (used > 0) p.fill_rounded(bx, by, std::max(Sd(5), bw * used), Sd(5), Sd(2.5), bar);
          }
          break;
        }
        case HomeView::Card::Empty:
          p.fill_rounded(cx, cy, c.w, c.h, r, t.dark ? l.card.mix(Color::hex(0xFFFFFF), 0.03) : l.card.mix(t.text, 0.02));
          p.stroke_rounded(cx + 0.5, cy + 0.5, c.w - 1, c.h - 1, r, l.card_border);
          liria::glyph(p, "info", cx + S(14), cy + (c.h - S(18)) / 2.0, S(18), l.accent_text);
          break;
        case HomeView::Card::Button:
          p.fill_rounded(cx, cy, c.w, c.h, t.radius_for(c.h), hot ? l.hover : l.card);
          p.stroke_rounded(cx + 0.5, cy + 0.5, c.w - 1, c.h - 1, t.radius_for(c.h), t.border);
          break;
        case HomeView::Card::Recent:
          if (hot) p.fill_rounded(cx, cy, c.w, c.h, t.radius_for(c.h), l.hover);
          if (Fl::focus() == this && int(i) == h_->focus) focus_ring(p, cx, cy, c.w, c.h, t.radius_for(c.h));
          draw_icon(p, c.icons, cx + S(12), cy + (c.h - S(24)) / 2.0, S(24));
          break;
      }
    }
  }
  time_t now = time(nullptr);
  for (size_t i = 0; i < h_->cards.size(); ++i) {
    const auto &c = h_->cards[i];
    int cx = x() + c.x, cy = y() + c.y;
    if (!fl_not_clipped(cx, cy, c.w, c.h)) continue;
    switch (c.kind) {
      case HomeView::Card::Header:
        text_font(t.font_size, true);
        draw_text(c.label, cx + S(24), cy, c.w - S(60), c.h, l.text);
        break;
      case HomeView::Card::Pin:
        text_font(t.font_size);
        draw_text(c.label, cx + S(8), cy + S(50), c.w - S(16), S(24), l.text, 1);
        break;
      case HomeView::Card::Drive:
      case HomeView::Card::Network:
      case HomeView::Card::Server:
        text_font(t.font_size, false);
        draw_text(c.label, cx + S(64), cy + S(8), c.w - S(76), S(24), l.text);
        text_font(t.font_size - 1);
        draw_text(c.sub, cx + S(64), cy + (c.kind == HomeView::Card::Drive && c.total > 0 ? S(44) : S(34)),
                  c.w - S(76), S(20), l.dim);
        break;
      case HomeView::Card::Empty: {
        int reserve = 0;
        for (const auto &o : h_->cards)
          if (o.kind == HomeView::Card::Button && o.section == c.section) reserve = o.w + S(20);
        text_font(t.font_size);
        draw_text(c.label, cx + S(44), cy, c.w - S(56) - reserve, c.h, l.text);
        break;
      }
      case HomeView::Card::Button:
        text_font(t.font_size);
        draw_text(c.label, cx, cy, c.w, c.h, l.text, 1);
        break;
      case HomeView::Card::Recent: {
        int dw = S(170);
        text_font(t.font_size);
        int nw = std::min(text_width(c.label) + S(4), (c.w - S(60) - dw) / 2);
        draw_text(c.label, cx + S(48), cy, nw, c.h, l.text);
        text_font(t.font_size - 1);
        draw_text(c.sub, cx + S(48) + nw + S(16), cy, c.w - S(48) - nw - S(16) - dw - S(10), c.h, l.dim);
        draw_text(chooser::time_text(c.time, now), cx + c.w - dw - S(10), cy, dw, c.h, l.dim, 2);
        break;
      }
    }
  }
  fl_pop_clip();
}

void HomeCanvas::activate(int i, bool new_tab) {
  if (i < 0 || i >= int(h_->cards.size())) return;
  MainWindow *w = h_->win();
  App &app = App::get();
  HomeView::Card c = h_->cards[size_t(i)];
  switch (c.kind) {
    case HomeView::Card::Header: {
      Prefs &pr = app.prefs;
      bool *flag = c.section == "quick" ? &pr.home_quick : c.section == "drives" ? &pr.home_drives
                   : c.section == "network" ? &pr.home_network : &pr.home_recent;
      *flag = !*flag;
      pr.save();
      for (auto *win : app.windows)
        if (win->homeview()) win->homeview()->rebuild();
      break;
    }
    case HomeView::Card::Pin:
      if (!c.path.empty()) {
        Item it;
        if (chooser::entry_for_path(c.path, it)) w->open_item(it, false);
      } else if (is_remote(c.location)) {
        w->open_network_location(c.location, new_tab);
      } else if (new_tab) {
        w->new_tab(c.location, false);
      } else {
        w->go(c.location);
      }
      break;
    case HomeView::Card::Drive:
      if (!c.location.empty()) {
        if (new_tab) w->new_tab(c.location, false);
        else w->go(c.location);
      } else {
        std::string id = c.id;
        app.drives.mount(id, mount_questions(w, c.label),
                         [w](bool ok, const std::string &error, const std::string &path) {
                           if (ok && !path.empty()) w->go(path);
                           else if (!error.empty()) w->say(error, true);
                         });
      }
      break;
    case HomeView::Card::Network:
      if (is_local(c.location)) {
        if (new_tab) w->new_tab(c.location, false);
        else w->go(c.location);
      } else {
        w->open_network_location(c.location, new_tab);
      }
      break;
    case HomeView::Card::Server:
      w->open_network_location(c.location, new_tab);
      break;
    case HomeView::Card::Recent: {
      Item it;
      if (chooser::entry_for_path(c.path, it)) w->open_item(it, false);
      else w->say(format(_("“%s” cannot be found."), c.label.c_str()), true);
      break;
    }
    case HomeView::Card::Button:
      if (c.id == "turn-off-network") {
        app.prefs.network_shown = false;
        app.prefs.save();
        for (auto *win : app.windows)
          if (win->homeview()) win->homeview()->rebuild();
        w->say(_("Network locations are hidden. Turn them on again in View > Show."));
      }
      break;
    case HomeView::Card::Empty:
      break;
  }
}

void HomeCanvas::menu(int i, int mx, int my) {
  MainWindow *w = h_->win();
  App &app = App::get();
  if (i < 0) return;
  HomeView::Card c = h_->cards[size_t(i)];
  ContextMenu m;
  switch (c.kind) {
    case HomeView::Card::Header:
      if (c.section == "recent") {
        m.item(_("Clear recent files"), [w] {
          if (confirm(w, _("Clear recent files?"),
                      _("The list of recently used files of every program is cleared. The files themselves stay."),
                      _("Clear"), false)) {
            clear_recent();
            App::get().places_changed();
          }
        }, {"delete"});
        m.item(_("Turn off Recent files"), [] {
          App::get().prefs.recent_shown = false;
          App::get().prefs.save();
          App::get().places_changed();
        });
      } else if (c.section == "network") {
        m.item(_("Connect to server…"), [w] { w->connect_to_server(); }, {"connect"});
        if (!app.servers.items().empty())
          m.item(_("Forget recent servers"), [] {
            App::get().servers.clear();
            App::get().places_changed();
          });
        m.item(_("Turn off Network locations"), [] {
          App::get().prefs.network_shown = false;
          App::get().prefs.save();
          App::get().places_changed();
        });
      } else if (c.section == "quick") {
        m.item(_("Reset Quick access to the defaults"), [w] {
          if (confirm(w, _("Reset Quick access?"),
                      _("The pins go back to Desktop, Downloads, Documents, Pictures, Music and Videos."), _("Reset"),
                      false)) {
            App::get().pins.reset();
            App::get().places_changed();
          }
        });
      }
      break;
    case HomeView::Card::Pin: {
      int index = c.index;
      std::string uri = c.id;
      m.item(_("Open"), [this, i] { activate(i, false); }, {"open"});
      if (c.path.empty()) m.item(_("Open in new tab"), [this, i] { activate(i, true); }, {"tab-new"});
      m.separator();
      m.item(_("Unpin from Quick access"), [uri] {
        App::get().pins.remove(uri);
        App::get().places_changed();
      }, {"pin"});
      m.item(_("Move left"), [index] {
        if (App::get().pins.move(index, index - 1)) App::get().places_changed();
      }, {"", 0, index > 0});
      m.item(_("Move right"), [index] {
        if (App::get().pins.move(index, index + 1)) App::get().places_changed();
      }, {"", 0, index + 1 < int(App::get().pins.items().size())});
      if (is_local(c.location)) {
        m.separator();
        std::string loc = c.location;
        m.item(_("Properties"), [w, loc] {
          Item it;
          if (chooser::entry_for_path(loc, it)) show_properties(w, {it});
        }, {"properties"});
      }
      break;
    }
    case HomeView::Card::Drive:
    case HomeView::Card::Network:
      w->place_menu(c.location, mx, my, c.id);
      return;
    case HomeView::Card::Server: {
      std::string uri = c.location;
      m.item(_("Open"), [this, i] { activate(i, false); }, {"open"});
      m.item(_("Open in new tab"), [this, i] { activate(i, true); }, {"tab-new"});
      m.item(_("Pin to Quick access"), [uri] {
        App::get().pins.add(uri);
        App::get().places_changed();
      }, {"pin"});
      m.separator();
      m.item(_("Forget this server"), [uri] {
        App::get().servers.forget(uri);
        App::get().places_changed();
      }, {"delete"});
      break;
    }
    case HomeView::Card::Recent: {
      std::string path = c.path;
      m.item(_("Open"), [this, i] { activate(i, false); }, {"open"});
      m.item(_("Open file location"), [w, path] { w->show_item(path); }, {"folder"});
      m.separator();
      m.item(_("Remove from list"), [path] {
        forget_recent(path);
        App::get().places_changed();
      }, {"delete"});
      m.item(_("Clear recent files"), [w] {
        if (confirm(w, _("Clear recent files?"),
                    _("The list of recently used files of every program is cleared. The files themselves stay."),
                    _("Clear"), false)) {
          clear_recent();
          App::get().places_changed();
        }
      });
      break;
    }
    default:
      return;
  }
  if (!m.empty()) m.popup(mx, my);
}

int HomeCanvas::handle(int e) {
  MainWindow *w = h_->win();
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int i = h_->card_at(mx, my);
      if (i >= 0 && h_->cards[size_t(i)].kind == HomeView::Card::Empty) i = -1;
      if (i >= 0 && h_->cards[size_t(i)].kind == HomeView::Card::Header &&
          mx - x() > h_->cards[size_t(i)].x + S(24) + S(200) && h_->cards[size_t(i)].section != "recent" &&
          h_->cards[size_t(i)].section != "network")
        i = -1;
      if (i != h_->hover) {
        damage_card(h_->hover);
        h_->hover = i;
        damage_card(i);
        if (i >= 0) {
          static std::string tip;
          const auto &c = h_->cards[size_t(i)];
          tip = c.kind == HomeView::Card::Pin || c.kind == HomeView::Card::Recent
                    ? (is_local(c.location) ? display_path(c.location) : c.location)
                    : c.kind == HomeView::Card::Drive ? c.label + (c.sub.empty() ? "" : "\n" + c.sub) : std::string();
          if (!tip.empty()) Fl_Tooltip::enter_area(this, x() + c.x, y() + c.y, c.w, c.h, tip.c_str());
        }
      }
      return 1;
    }
    case FL_LEAVE:
      damage_card(h_->hover);
      h_->hover = -1;
      return 1;
    case FL_PUSH: {
      take_focus();
      int i = h_->card_at(mx, my);
      int b = Fl::event_button();
      if (b == FL_BACK_MOUSE || b == FL_FORWARD_MOUSE) return 0;
      if (i < 0) {
        if (b == FL_RIGHT_MOUSE) w->background_menu(mx, my);
        return 1;
      }
      h_->focus = i;
      const auto &c = h_->cards[size_t(i)];
      if (b == FL_RIGHT_MOUSE) {
        menu(i, mx, my);
        return 1;
      }
      if (b == FL_MIDDLE_MOUSE) {
        activate(i, true);
        return 1;
      }
      if (c.kind == HomeView::Card::Header && mx - x() >= c.x + c.w - S(34)) {
        menu(i, mx, my);
        return 1;
      }
      press = i;
      press_x = mx;
      press_y = my;
      dragging = false;
      return 1;
    }
    case FL_DRAG:
      if (press >= 0 && !dragging && std::abs(mx - press_x) + std::abs(my - press_y) > S(8)) {
        const auto &c = h_->cards[size_t(press)];
        if (c.kind == HomeView::Card::Pin && is_local(c.location)) {
          dragging = true;
          w->begin_drag({c.location}, 1, fl_event_time);
        } else if (c.kind == HomeView::Card::Recent) {
          dragging = true;
          w->begin_drag({c.path}, 1, fl_event_time);
        }
      }
      if (dragging) w->drag_motion(Fl::event_x_root(), Fl::event_y_root(), fl_mods(), fl_event_time);
      return 1;
    case FL_RELEASE: {
      int i = press;
      press = -1;
      if (dragging) {
        dragging = false;
        w->drag_drop(fl_event_time);
        return 1;
      }
      if (i >= 0 && h_->card_at(mx, my) == i) activate(i, false);
      return 1;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      if (w->shortcut(key)) return 1;
      int n = int(h_->cards.size());
      if (key == FL_Right || key == FL_Down || key == FL_Left || key == FL_Up || key == FL_Tab) {
        bool fwd = key == FL_Right || key == FL_Down || (key == FL_Tab && !(Fl::event_state() & FL_SHIFT));
        int i = h_->focus;
        for (int k = 0; k < n; ++k) {
          i = fwd ? (i + 1) % n : (i - 1 + n) % n;
          auto kind = h_->cards[size_t(i)].kind;
          if (kind != HomeView::Card::Empty) break;
        }
        damage_card(h_->focus);
        h_->focus = i;
        damage_card(i);
        return 1;
      }
      if ((key == FL_Enter || key == FL_KP_Enter || key == ' ') && h_->focus >= 0) {
        activate(h_->focus, Fl::event_state() & FL_CTRL);
        return 1;
      }
      if (key == FL_Menu && h_->focus >= 0) {
        const auto &c = h_->cards[size_t(h_->focus)];
        menu(h_->focus, x() + c.x + c.w / 2, y() + c.y + c.h / 2);
        return 1;
      }
      if (key == FL_Delete && h_->focus >= 0 && h_->cards[size_t(h_->focus)].kind == HomeView::Card::Pin) {
        App::get().pins.remove(h_->cards[size_t(h_->focus)].id);
        App::get().places_changed();
        return 1;
      }
      return 0;
    }
  }
  return Fl_Widget::handle(e);
}

}  // namespace liria
}  // namespace sde
