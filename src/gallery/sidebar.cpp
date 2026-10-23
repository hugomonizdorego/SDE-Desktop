// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Sidebar with the library views and folders, and the segmented sort control.
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>

#include <FL/Fl_Menu_Item.H>

#include <algorithm>
#include <cstring>

#include "gallery.h"
#include "i18n.h"
#include "util.h"

using namespace sde;

namespace gallery {

// Small version of the application icon (see sde-gallery.svg).
static void draw_logo(Painter &p, double x, double y, double s) {
  cairo_t *cr = p.cr();
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + s, y + s);
  cairo_pattern_add_color_stop_rgb(g, 0, 0x2B / 255.0, 0xB4 / 255.0, 0xD3 / 255.0);
  cairo_pattern_add_color_stop_rgb(g, 0.55, 0x17 / 255.0, 0x7E / 255.0, 0x9C / 255.0);
  cairo_pattern_add_color_stop_rgb(g, 1, 0x0E / 255.0, 0x56 / 255.0, 0x6C / 255.0);
  p.rounded_rect(x, y, s, s, s * 0.26);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  // Back card.
  cairo_save(cr);
  cairo_translate(cr, x + s * 0.5, y + s * 0.5);
  cairo_rotate(cr, -0.21);
  p.fill_rounded(-s * 0.31, -s * 0.27, s * 0.56, s * 0.46, s * 0.06, Color::hex(0xFFFFFF, 0.45));
  cairo_restore(cr);
  // Front card with a landscape.
  double fx = x + s * 0.22, fy = y + s * 0.30, fw = s * 0.60, fh = s * 0.48;
  p.fill_rounded(fx, fy, fw, fh, s * 0.06, Color::hex(0xFFFFFF));
  cairo_save(cr);
  p.rounded_rect(fx + s * 0.04, fy + s * 0.04, fw - s * 0.08, fh - s * 0.08, s * 0.03);
  cairo_clip(cr);
  p.fill_rect(fx, fy, fw, fh, Color::hex(0xCFEFF6));
  p.set(Color::hex(0x177E9C));
  cairo_move_to(cr, fx, fy + fh);
  cairo_line_to(cr, fx + fw * 0.36, fy + fh * 0.42);
  cairo_line_to(cr, fx + fw * 0.58, fy + fh * 0.70);
  cairo_line_to(cr, fx + fw * 0.72, fy + fh * 0.56);
  cairo_line_to(cr, fx + fw, fy + fh * 0.9);
  cairo_line_to(cr, fx + fw, fy + fh);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_restore(cr);
  p.circle(fx + fw * 0.74, fy + fh * 0.32, s * 0.06, Color::hex(0xF2B84B));
}

Sidebar::Sidebar(GalleryWindow *app, int x, int y, int w, int h)
    : Fl_Widget(x, y, w, h), app_(app) {}

std::vector<Sidebar::Row> Sidebar::rows() const {
  std::vector<Row> out;
  int y = this->y() + S(76), rh = S(38), hh = S(36);
  auto add = [&](Row r, int height) {
    r.y = y;
    r.h = height;
    y += height + (r.header ? 0 : S(2));
    out.push_back(std::move(r));
  };
  Row r;
  r = Row{_("All Photos"), icon::Photos, app_->library().size(), View{ViewKind::All, ""}};
  add(r, rh);
  r = Row{_("Favorites"), icon::Heart, app_->count_for(View{ViewKind::Favorites, ""}),
          View{ViewKind::Favorites, ""}};
  add(r, rh);
  r = Row{_("Recent"), icon::Clock, app_->count_for(View{ViewKind::Recent, ""}), View{ViewKind::Recent, ""}};
  add(r, rh);
  r = Row{_("Screenshots"), icon::Screenshot, app_->count_for(View{ViewKind::Screenshots, ""}),
          View{ViewKind::Screenshots, ""}};
  add(r, rh);
  Row h;
  h.label = _("Folders");
  h.header = true;
  add(h, hh);
  View pictures{ViewKind::Folder, app_->pictures()};
  r = Row{_("Pictures"), icon::Folder, app_->count_for(pictures), pictures};
  add(r, rh);
  for (const auto &f : app_->user_folders()) {
    View v{ViewKind::Folder, f};
    r = Row{basename_of(f), icon::Folder, app_->count_for(v), v};
    r.removable = true;
    add(r, rh);
  }
  r = Row{_("Add Folder…"), icon::Plus, 0, View{}};
  r.add = true;
  add(r, rh);
  const View &cur = app_->view();
  if (cur.kind == ViewKind::Browse) {
    Row b{basename_of(cur.folder).empty() ? cur.folder : basename_of(cur.folder), icon::Folder,
          app_->count_for(cur), cur};
    add(b, rh);
  }
  Row ah;
  ah.label = _("Albums");
  ah.header = true;
  add(ah, hh);
  for (const auto &a : app_->user_albums()) {
    View v{ViewKind::Album, a.file};
    r = Row{a.name, icon::Album, a.count, v};
    r.album = true;
    r.removable = true;  // the hover button deletes the album (not the photos)
    add(r, rh);
  }
  r = Row{_("New Album…"), icon::Plus, 0, View{}};
  r.add = true;
  r.new_album = true;
  add(r, rh);
  if (!app_->devices().empty()) {
    Row d;
    d.label = _("Devices");
    d.header = true;
    add(d, hh);
    for (const auto &m : app_->devices()) {
      View v{ViewKind::Device, m.mount_point};
      r = Row{m.label(), icon::Drive, app_->count_for(v), v};
      r.device = true;
      r.removable = true;  // the hover button ejects
      add(r, rh);
    }
  }
  return out;
}

int Sidebar::row_at(int my) const {
  auto rs = rows();
  for (size_t i = 0; i < rs.size(); ++i)
    if (!rs[i].header && my >= rs[i].y && my < rs[i].y + rs[i].h) return int(i);
  return -1;
}

void Sidebar::draw() {
  const Theme &t = app_->theme();
  bool small = compact();
  auto rs = rows();
  std::vector<std::pair<int, std::string>> labels;  // drawn after cairo
  {
    Painter p(window()->w(), window()->h());
    cairo_save(p.cr());
    cairo_rectangle(p.cr(), x(), y(), w(), h());
    cairo_clip(p.cr());
    if (t.glass) {
      p.fill_gradient(x(), y(), w(), h(), 0, t.surface_alt, t.surface_alt.mix(t.accent, t.dark ? 0.05 : 0.07));
    } else {
      p.fill_rect(x(), y(), w(), h(), t.surface_alt);
    }
    p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), t.border.alpha(0.8));
    double ls = S(32);
    draw_logo(p, small ? x() + (w() - ls) / 2 : x() + S(20), y() + S(18), ls);
    for (size_t i = 0; i < rs.size(); ++i) {
      const Row &r = rs[i];
      double rx = x() + S(10), rw = w() - S(20);
      if (r.header) {
        if (small) p.line(x() + S(16), r.y + r.h / 2.0, x() + w() - S(16), r.y + r.h / 2.0, t.border);
        continue;
      }
      bool active = !r.add && app_->view() == r.view;
      bool hot = int(i) == hover_;
      if (active) p.fill_rounded(rx, r.y, rw, r.h, Sd(9), t.accent.alpha(t.dark ? 0.28 : 0.16));
      else if (hot) p.fill_rounded(rx, r.y, rw, r.h, Sd(9), t.hover);
      if (Fl::focus() == this && int(i) == focus_) focus_ring(p, rx, r.y, rw, r.h, Sd(9));
      double is = S(20);
      double ix = small ? x() + (w() - is) / 2 : rx + S(10);
      Color ic = active ? t.accent_text : r.add ? t.text_dim : t.text;
      icon::Kind glyph = r.glyph;
      if (glyph == icon::Heart && active) glyph = icon::HeartFilled;
      icon::draw(p, glyph, ix, r.y + (r.h - is) / 2, is, ic);
      if (hot && r.removable && !small) {
        double bx = rx + rw - S(30), by = r.y + (r.h - S(24)) / 2.0;
        if (hover_remove_) p.fill_rounded(bx, by, S(24), S(24), Sd(6), t.border);
        icon::draw(p, r.device ? icon::Eject : icon::Close, bx + S(4), by + S(4), S(16), t.text_dim);
      }
    }
    cairo_restore(p.cr());
  }
  if (!small) {
    text_font(t.font_size + S(3), true);
    draw_text(_("Gallery"), x() + S(62), y() + S(18), w() - S(76), S(32), t.text);
  }
  for (size_t i = 0; i < rs.size(); ++i) {
    const Row &r = rs[i];
    if (small) continue;
    if (r.header) {
      text_font(t.font_size - S(1), true);
      draw_text(r.label, x() + S(22), r.y + S(8), w() - S(44), r.h - S(8), t.text_dim);
      continue;
    }
    bool active = !r.add && app_->view() == r.view;
    int tx = x() + S(50), tw = w() - S(50) - S(20);
    bool hot = int(i) == hover_;
    std::string count = r.add || (hot && r.removable) || (r.device && r.count == 0) ? "" : std::to_string(r.count);
    text_font(t.font_size - S(1));
    int cw = count.empty() ? 0 : text_width(count) + S(10);
    if (!count.empty())
      draw_text(count, x() + w() - S(20) - cw, r.y, cw - S(4), r.h, active ? t.accent_text : t.text_dim, 2);
    text_font(t.font_size, active);
    draw_text(r.label, tx, r.y, tw - cw - (hot && r.removable ? S(30) : 0), r.h,
              active ? t.accent_text : r.add ? t.text_dim : t.text);
  }
}

int Sidebar::handle(int event) {
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int r = row_at(Fl::event_y());
      bool rm = false;
      if (r >= 0) {
        auto rs = rows();
        rm = rs[size_t(r)].removable && Fl::event_x() >= x() + w() - S(10) - S(32);
      }
      if (r != hover_ || rm != hover_remove_) {
        hover_ = r;
        hover_remove_ = rm;
        redraw();
      }
      if (compact() && r != tip_row_) {
        // Icon-only sidebar: name the row in a tooltip.
        tip_row_ = r;
        if (r >= 0) {
          auto rs = rows();
          tip_ = rs[size_t(r)].label;
          Fl_Tooltip::enter_area(this, x(), rs[size_t(r)].y, w(), rs[size_t(r)].h, tip_.c_str());
        } else {
          Fl_Tooltip::exit(this);
        }
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      tip_row_ = -1;
      hover_remove_ = false;
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH: {
      int r = row_at(Fl::event_y());
      if (r < 0) return 1;
      focus_ = r;
      auto rs = rows();
      const Row &row = rs[size_t(r)];
      if (Fl::event_button() == FL_RIGHT_MOUSE && row.album) {
        ContextMenu m;
        View v = row.view;
        GalleryWindow *app = app_;
        m.item(_("Open"), [app, v] { app->set_view(v); }, {"open"});
        m.item(_("Slideshow"), [app, v] {
          app->set_view(v);
          app->slideshow({});
        });
        m.separator();
        m.item(_("Rename…"), [app, v] { app->rename_album(v.folder); }, {"rename"});
        m.item(_("Delete Album…"), [app, v] { app->delete_album(v.folder); }, {"delete"});
        m.popup();
        redraw();
        return 1;
      }
      if (Fl::event_button() == FL_RIGHT_MOUSE && row.device) {
        Fl_Menu_Item items[4];
        memset(items, 0, sizeof items);
        items[0].text = _("Open");
        items[1].text = _("Import Photos…");
        items[2].text = _("Eject");
        for (int k = 0; k < 3; ++k) {
          items[k].labelsize(app_->theme().font_size);
          items[k].labelcolor(app_->theme().text.fl());
        }
        const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y());
        int pick = m ? int(m - items) : -1;
        if (pick == 0) app_->set_view(row.view);
        else if (pick == 1) {
          app_->set_view(row.view);
          for (const auto &d : app_->devices())
            if (d.mount_point == row.view.folder) app_->import_from(d);
        } else if (pick == 2) {
          app_->eject(row.view.folder);
        }
        redraw();
        return 1;
      }
      activate(row);
      redraw();
      return 1;
    }
    case FL_KEYBOARD: {
      auto rs = rows();
      int k = Fl::event_key();
      if (k == FL_Up || k == FL_Down) {
        int step = k == FL_Up ? -1 : 1, i = focus_;
        do {
          i += step;
        } while (i >= 0 && i < int(rs.size()) && rs[size_t(i)].header);
        if (i >= 0 && i < int(rs.size())) focus_ = i;
        redraw();
        return 1;
      }
      if ((k == FL_Enter || k == FL_KP_Enter || k == ' ') && focus_ >= 0 && focus_ < int(rs.size())) {
        if (rs[size_t(focus_)].new_album) app_->new_album();
        else if (rs[size_t(focus_)].add) app_->add_folder();
        else app_->set_view(rs[size_t(focus_)].view);
        return 1;
      }
      if (k == FL_Delete && focus_ >= 0 && focus_ < int(rs.size()) && rs[size_t(focus_)].removable) {
        if (rs[size_t(focus_)].device) app_->eject(rs[size_t(focus_)].view.folder);
        else if (rs[size_t(focus_)].album) app_->delete_album(rs[size_t(focus_)].view.folder);
        else app_->remove_folder(rs[size_t(focus_)].view.folder);
        return 1;
      }
      return 0;
    }
  }
  return Fl_Widget::handle(event);
}

void Sidebar::activate(const Row &row) {
  if (row.new_album) {
    app_->new_album();
  } else if (row.add) {
    app_->add_folder();
  } else if (row.removable && hover_remove_ && !compact()) {
    if (row.device) app_->eject(row.view.folder);
    else if (row.album) app_->delete_album(row.view.folder);
    else app_->remove_folder(row.view.folder);
    hover_ = -1;
  } else {
    app_->set_view(row.view);
  }
}

// ------------------------------------------------------------ segmented

Segmented::Segmented(int x, int y, int w, int h, std::vector<std::string> labels)
    : Fl_Widget(x, y, w, h), labels_(std::move(labels)) {}

void Segmented::draw() {
  const Theme &t = active_theme();
  int n = std::max<int>(1, int(labels_.size()));
  double sw = double(w() - S(6)) / n;
  {
    Painter p(window()->w(), window()->h());
    p.fill_rounded(x(), y(), w(), h(), h() / 2.0, t.surface_alt);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, h() / 2.0, t.border);
    for (int i = 0; i < n; ++i) {
      double sx = x() + S(3) + i * sw, sy = y() + S(3), sh = h() - S(6);
      if (i == value_) {
        p.shadow(sx, sy, sw, sh, sh / 2, t.shadow.alpha(t.shadow.a * 0.5), S(2));
        p.fill_rounded(sx, sy, sw, sh, sh / 2, t.accent);
      } else if (i == hover_) {
        p.fill_rounded(sx, sy, sw, sh, sh / 2, t.hover);
      }
    }
    if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), h() / 2.0);
  }
  for (int i = 0; i < n && i < int(labels_.size()); ++i) {
    text_font(t.font_size - S(1), i == value_);
    draw_text(labels_[size_t(i)], int(x() + S(3) + i * sw), y(), int(sw), h(),
              i == value_ ? t.on_accent : t.text, 1);
  }
}

int Segmented::handle(int event) {
  int n = std::max<int>(1, int(labels_.size()));
  auto seg = [&]() {
    int i = int((Fl::event_x() - x() - S(3)) * n / std::max(1, w() - S(6)));
    return std::max(0, std::min(n - 1, i));
  };
  switch (event) {
    case FL_ENTER:
    case FL_MOVE:
      if (seg() != hover_) {
        hover_ = seg();
        redraw();
      }
      return 1;
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
      redraw();
      return 1;
    case FL_PUSH:
      if (seg() != value_) {
        value_ = seg();
        do_callback();
      }
      redraw();
      return 1;
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      if (k == FL_Left || k == FL_Right) {
        int v = std::max(0, std::min(n - 1, value_ + (k == FL_Left ? -1 : 1)));
        if (v != value_) {
          value_ = v;
          do_callback();
        }
        redraw();
        return 1;
      }
      return 0;
    }
  }
  return Fl_Widget::handle(event);
}

}  // namespace gallery
