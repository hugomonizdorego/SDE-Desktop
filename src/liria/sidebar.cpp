// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The navigation pane: Home and the Trash, then the groups Pinned (the
// Quick access pins, each with its pin mark), Drives (with an eject button
// for removable drives) and Network (the network page, then the shares
// GVfs mounted). Group headings fold their group (remembered). Folders and
// drives take dropped files; a folder dropped on "Pinned" or between the
// pins is pinned there, and a dragged pin moves. The pane scrolls inside
// an Fl_Scroll (the UI kit's scrolling).
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
#include "trash.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {
namespace liria {

class SidebarCanvas : public Fl_Widget {
 public:
  explicit SidebarCanvas(Sidebar *s) : Fl_Widget(0, 0, 10, 10), s_(s) {}
  int press = -1, press_y = 0, drag_pin = -1;
  bool dragging = false;
  void draw() override;
  int handle(int e) override;
  void damage_row(int i);

 private:
  Sidebar *s_;
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

bool trash_full() {
  static double checked = -1;
  static bool full = false;
  if (now_ms() - checked > 2000) {
    full = !trash_is_empty();
    checked = now_ms();
  }
  return full;
}

}  // namespace

Sidebar::Sidebar(MainWindow *w) : Fl_Scroll(0, 0, 10, 10), w_(w) {
  type(Fl_Scroll::VERTICAL);
  box(FL_NO_BOX);
  canvas = new SidebarCanvas(this);
  end();
}

void Sidebar::rebuild() {
  App &app = App::get();
  const Prefs &prefs = app.prefs;
  rows.clear();
  auto place = [&](const std::string &id, const std::string &label, const std::string &loc,
                   std::vector<std::string> icons) {
    Row r;
    r.kind = Row::Place;
    r.id = id;
    r.label = label;
    r.location = loc;
    r.icons = std::move(icons);
    rows.push_back(r);
    return &rows.back();
  };
  auto heading = [&](const std::string &group, const std::string &label, bool expanded) {
    Row r;
    r.kind = Row::Heading;
    r.group = group;
    r.label = label;
    r.expanded = expanded;
    rows.push_back(r);
  };
  place("home", _("Home"), kHome, {"go-home", "user-home", "folder"});
  place("trash", _("Trash"), kTrash,
        trash_full() ? std::vector<std::string>{"user-trash-full", "user-trash"} : std::vector<std::string>{"user-trash"});
  heading("pinned", _("Pinned"), prefs.side_pinned);
  if (prefs.side_pinned) {
    for (const auto &pin : app.pins.items()) {
      std::string loc = pin_location(pin.uri);
      std::string label = pin.label.empty() ? location_title(loc) : pin.label;
      Row *r = place("pin:" + pin.uri, label, loc, location_icons(loc));
      r->pinned = true;
      if (is_local(loc) && !dir_exists(loc)) {
        // A pinned file: its own icon.
        chooser::Entry e;
        if (chooser::entry_for_path(loc, e) && !e.dir) r->icons = chooser::mime_icon_names(e.mime);
      }
    }
  }
  heading("drives", _("Drives"), prefs.side_drives);
  if (prefs.side_drives) {
    for (const auto &d : app.drives.drives()) {
      Row *r = place("drive:" + d.id, d.name, d.path.empty() ? std::string() : d.path, d.icons);
      r->drive_id = d.id;
      r->ejectable = !d.system && d.mounted && (d.can_eject || (d.removable && d.can_unmount));
    }
  }
  // Network: one place, as in Windows 11 (no heading above it), and the
  // shares that are open below it.
  place("network", _("Network"), kNetwork, {"network-workgroup", "folder-remote", "network-server"});
  for (const auto &d : app.drives.network()) {
    Row *r = place("net:" + d.id, d.name, d.path.empty() ? d.uri : d.path, d.icons);
    r->drive_id = d.id;
    r->ejectable = d.can_unmount;
  }
  // Positions.
  int y = S(6);
  for (auto &r : rows) {
    if (r.kind == Row::Heading) {
      y += S(10);
      r.y = y;
      r.h = S(30);
      y += S(30);
    } else {
      if (r.id == "network") y += S(10);  // as far from the drives as a heading
      r.y = y;
      r.h = S(34);
      y += S(36);
    }
  }
  int sb = Fl::scrollbar_size() ? Fl::scrollbar_size() : S(14);
  canvas->resize(x(), this->y() - yposition(), std::max(10, w() - (y > h() ? sb : 0)), std::max(h(), y + S(8)));
  redraw();
}

void Sidebar::resize(int X, int Y, int W, int H) {
  Fl_Scroll::resize(X, Y, W, H);
  if (!rows.empty()) rebuild();
}

int Sidebar::canvas_y() const { return canvas->y(); }

int Sidebar::row_at(int my) const {
  int cy = my - canvas->y();
  for (size_t i = 0; i < rows.size(); ++i)
    if (cy >= rows[i].y && cy < rows[i].y + rows[i].h) return int(i);
  return -1;
}

void Sidebar::draw() {
  if (!fl_not_clipped(x(), y(), w(), h())) return;
  Fl_Scroll::draw();
}

void SidebarCanvas::damage_row(int i) {
  if (i < 0 || i >= int(s_->rows.size())) return;
  const auto &r = s_->rows[size_t(i)];
  static_cast<MainWindow *>(window())->damage_rect(x(), y() + r.y - S(4), w(), r.h + S(8));
}

void SidebarCanvas::draw() {
  MainWindow *win = s_->win();
  const Look &l = look();
  const Theme &t = active_theme();
  const std::string &cur = win->tab().location;
  fl_push_clip(s_->x(), s_->y(), s_->w(), s_->h());
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(s_->x(), s_->y(), s_->w(), s_->h(), l.toolbar);
    for (size_t i = 0; i < s_->rows.size(); ++i) {
      const auto &r = s_->rows[i];
      int ry = y() + r.y;
      if (!fl_not_clipped(x(), ry, w(), r.h)) continue;
      double rx = x() + S(8), rw = w() - S(16);
      if (r.kind == Sidebar::Row::Heading) {
        if (int(i) == s_->hover) p.fill_rounded(rx, ry, rw, r.h, t.radius_for(r.h), l.hover);
        if (int(i) == s_->drop_row) p.stroke_rounded(rx, ry, rw, r.h, t.radius_for(r.h), l.accent, Sd(1.5));
        liria::glyph(p, r.expanded ? "chevron-up" : "chevron-down", x() + w() - S(34), ry + (r.h - S(12)) / 2.0, S(12),
                     l.dim, 1.8);
        continue;
      }
      bool selected = !r.location.empty() && r.location == cur && win->tab().query.empty();
      if (selected) {
        p.fill_rounded(rx, ry, rw, r.h, t.radius_for(r.h), l.select);
        p.fill_rounded(rx, ry + S(9), Sd(3), r.h - S(18), Sd(1.5), l.accent);
      } else if (int(i) == s_->hover) {
        p.fill_rounded(rx, ry, rw, r.h, t.radius_for(r.h), l.hover);
      }
      if (int(i) == s_->drop_row) p.stroke_rounded(rx, ry, rw, r.h, t.radius_for(r.h), l.accent, Sd(1.5));
      if (Fl::focus() == this && int(i) == s_->focus_row) focus_ring(p, rx, ry, rw, r.h, t.radius_for(r.h));
      draw_icon(p, r.icons, x() + S(22), ry + (r.h - S(18)) / 2.0, S(18));
      if (r.pinned)  // the pin mark of Windows 11's navigation pane, always shown
        liria::glyph(p, "pin", x() + w() - S(36), ry + (r.h - S(14)) / 2.0, S(14),
                     int(i) == s_->hover ? l.dim : l.faint, 1.5);
      if (r.ejectable) {
        bool hot = int(i) == s_->hover_eject;
        if (hot) p.fill_rounded(x() + w() - S(42), ry + S(5), S(26), r.h - S(10), Sd(5), l.press);
        liria::glyph(p, "eject", x() + w() - S(36), ry + (r.h - S(14)) / 2.0, S(14), hot ? l.text : l.dim, 1.6);
      }
    }
    if (s_->drop_gap >= 0 && s_->drop_gap < int(s_->rows.size())) {
      int gy = y() + s_->rows[size_t(s_->drop_gap)].y - S(1);
      p.fill_rounded(x() + S(12), gy - Sd(1), w() - S(24), Sd(3), Sd(1.5), l.accent);
    }
  }
  for (size_t i = 0; i < s_->rows.size(); ++i) {
    const auto &r = s_->rows[i];
    int ry = y() + r.y;
    if (!fl_not_clipped(x(), ry, w(), r.h)) continue;
    if (r.kind == Sidebar::Row::Heading) {
      text_font(t.font_size - 1, false);
      draw_text(r.label, x() + S(22), ry, w() - S(64), r.h, l.dim);
      continue;
    }
    bool selected = !r.location.empty() && r.location == cur && win->tab().query.empty();
    text_font(t.font_size, false);
    int right = r.pinned || r.ejectable ? S(48) : S(16);
    draw_text(r.label, x() + S(50), ry, w() - S(50) - right, r.h, selected ? l.text : l.text);
  }
  fl_pop_clip();
}

int SidebarCanvas::handle(int e) {
  MainWindow *win = s_->win();
  App &app = App::get();
  int mx = Fl::event_x(), my = Fl::event_y();
  auto eject_at = [&](int i) {
    if (i < 0 || !s_->rows[size_t(i)].ejectable) return false;
    return mx >= x() + w() - S(44) && mx < x() + w() - S(14);
  };
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int i = s_->row_at(my);
      int ej = eject_at(i) ? i : -1;
      if (i != s_->hover || ej != s_->hover_eject) {
        damage_row(s_->hover);
        s_->hover = i;
        s_->hover_eject = ej;
        damage_row(i);
        if (i >= 0) {
          static std::string tip;
          const auto &r = s_->rows[size_t(i)];
          if (ej >= 0) tip = _("Eject");
          else if (r.kind == Sidebar::Row::Heading) tip = r.expanded ? _("Fold") : _("Unfold");
          else tip = is_local(r.location) ? display_path(r.location) : r.label;
          Fl_Tooltip::enter_area(this, x(), y() + r.y, w(), r.h, tip.c_str());
        }
      }
      return 1;
    }
    case FL_LEAVE:
      damage_row(s_->hover);
      s_->hover = s_->hover_eject = -1;
      return 1;
    case FL_PUSH: {
      take_focus();
      int i = s_->row_at(my);
      if (i < 0) return 1;
      const auto row = s_->rows[size_t(i)];
      int b = Fl::event_button();
      s_->focus_row = i;
      if (row.kind == Sidebar::Row::Heading) {
        if (b == FL_LEFT_MOUSE) {
          Prefs &pr = app.prefs;
          bool *flag = row.group == "pinned" ? &pr.side_pinned : row.group == "drives" ? &pr.side_drives : &pr.side_network;
          *flag = !*flag;
          pr.save();
          s_->rebuild();
        } else if (b == FL_RIGHT_MOUSE && row.group == "pinned") {
          ContextMenu menu;
          menu.item(_("Reset Quick access to the defaults"), [win] {
            if (confirm(win, _("Reset Quick access?"),
                        _("The pins go back to Desktop, Downloads, Documents, Pictures, Music and Videos."),
                        _("Reset"), false)) {
              App::get().pins.reset();
              App::get().places_changed();
            }
          });
          menu.popup();
        }
        return 1;
      }
      if (b == FL_RIGHT_MOUSE) {
        win->place_menu(row.location, mx, my, row.drive_id);
        return 1;
      }
      if (b == FL_MIDDLE_MOUSE) {
        if (!row.location.empty()) win->new_tab(row.location, false);
        return 1;
      }
      if (b != FL_LEFT_MOUSE) return 1;
      if (eject_at(i)) {
        std::string id = row.drive_id;
        app.drives.eject(id, [win](bool ok, const std::string &error, const std::string &) {
          if (!ok && !error.empty()) win->say(error, true);
          else if (ok) win->say(_("The drive can be removed now."));
        });
        return 1;
      }
      press = i;
      press_y = my;
      dragging = false;
      drag_pin = row.pinned ? i : -1;
      return 1;
    }
    case FL_DRAG:
      if (drag_pin >= 0 && std::abs(my - press_y) > S(6)) {
        dragging = true;
        int i = s_->row_at(my);
        int gap = -1;
        if (i >= 0 && s_->rows[size_t(i)].pinned) {
          const auto &r = s_->rows[size_t(i)];
          gap = my - (y() + r.y) < r.h / 2 ? i : i + 1;
        }
        if (gap != s_->drop_gap) {
          s_->drop_gap = gap;
          s_->redraw();
        }
      }
      return 1;
    case FL_RELEASE: {
      int i = press;
      press = -1;
      if (dragging) {
        dragging = false;
        int gap = s_->drop_gap;
        s_->drop_gap = -1;
        if (gap >= 0 && drag_pin >= 0) {
          // The pins' indices: rows after the "Pinned" heading.
          int first = -1;
          for (size_t r = 0; r < s_->rows.size(); ++r)
            if (s_->rows[r].pinned) {
              first = int(r);
              break;
            }
          int from = drag_pin - first, to = gap - first;
          if (to > from) --to;
          if (first >= 0 && app.pins.move(from, to)) app.places_changed();
        }
        drag_pin = -1;
        s_->redraw();
        return 1;
      }
      drag_pin = -1;
      if (i >= 0 && s_->row_at(my) == i) {
        const auto &row = s_->rows[size_t(i)];
        if (!row.location.empty()) {
          win->go(row.location);
        } else if (!row.drive_id.empty()) {
          // A drive that is not mounted: mount it, then open it.
          std::string id = row.drive_id;
          app.drives.mount(id, mount_questions(win, row.label), [win](bool ok, const std::string &error,
                                                                  const std::string &path) {
            if (ok && !path.empty()) win->go(path);
            else if (!error.empty()) win->say(error, true);
          });
        }
      }
      return 1;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_KEYBOARD: {
      int key = Fl::event_key();
      if (win->shortcut(key)) return 1;
      int n = int(s_->rows.size());
      if (key == FL_Up || key == FL_Down) {
        int i = s_->focus_row;
        for (int k = 0; k < n; ++k) {
          i = key == FL_Down ? std::min(n - 1, i + 1) : std::max(0, i - 1);
          if (s_->rows[size_t(i)].kind == Sidebar::Row::Place) break;
        }
        s_->focus_row = i;
        redraw();
        return 1;
      }
      if ((key == FL_Enter || key == FL_KP_Enter || key == ' ') && s_->focus_row >= 0) {
        const auto &row = s_->rows[size_t(s_->focus_row)];
        if (!row.location.empty()) win->go(row.location);
        return 1;
      }
      return 0;
    }
  }
  return Fl_Widget::handle(e);
}

}  // namespace liria
}  // namespace sde
