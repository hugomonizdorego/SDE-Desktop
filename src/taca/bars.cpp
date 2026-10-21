// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// The menu bar and the toolbar of large labelled buttons.
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstring>

#include "i18n.h"
#include "taca.h"
#include "util.h"

using namespace sde;

namespace taca {

// --------------------------------------------------------------- menu bar

namespace {

std::string strip_mnemonic(const char *label) {
  std::string out;
  for (const char *p = label; p && *p; ++p) {
    if (*p == '&' && p[1] != '&') continue;
    out += *p;
  }
  return out;
}

}  // namespace

MenuBar::MenuBar(int x, int y, int w, int h) : Fl_Menu_Bar(x, y, w, h) {
  box(FL_FLAT_BOX);
  down_box(FL_FLAT_BOX);
  apply_theme();
}

void MenuBar::apply_theme() {
  const Theme &t = active_theme();
  color(t.surface.fl());
  selection_color(t.accent.fl());
  textcolor(t.text.fl());
  textsize(t.font_size);
  textfont(FL_HELVETICA);
  redraw();
}

int MenuBar::title_at(int mx) const {
  if (!menu() || !menu()->text) return -1;
  int X = x() + 6, i = 0;
  for (const Fl_Menu_Item *m = menu()->first(); m && m->text; m = m->next(), ++i) {
    int W = m->measure(nullptr, this) + 16;
    if (mx >= X && mx < X + W) return i;
    X += W;
  }
  return -1;
}

void MenuBar::draw() {
  const Theme &t = active_theme();
  {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), w(), h(), t.surface);
    if (menu() && menu()->text) {
      int X = x() + 6, i = 0;
      for (const Fl_Menu_Item *m = menu()->first(); m && m->text; m = m->next(), ++i) {
        int W = m->measure(nullptr, this) + 16;
        if (i == hover_ && active_r()) p.fill_rounded(X + 1, y() + S(3), W - 2, h() - S(6), Sd(6), t.hover);
        X += W;
      }
    }
  }
  if (!menu() || !menu()->text) return;
  int X = x() + 6;
  for (const Fl_Menu_Item *m = menu()->first(); m && m->text; m = m->next()) {
    int W = m->measure(nullptr, this) + 16;
    text_font(textsize());
    std::string label = strip_mnemonic(m->label());
    draw_text(label, X, y(), W, h(), m->active() ? t.text : t.text_dim, 1);
    // Underline the mnemonic letter like classic menu bars.
    const char *amp = strchr(m->label(), '&');
    if (amp && amp[1]) {
      std::string before(m->label(), size_t(amp - m->label()));
      int tx = X + (W - text_width(label)) / 2 + text_width(before);
      int lw = text_width(std::string(1, amp[1]));
      fl_color(t.text_dim.fl());
      fl_line(tx, y() + h() / 2 + textsize() / 2, tx + lw - 1, y() + h() / 2 + textsize() / 2);
    }
    X += W;
  }
}

int MenuBar::handle(int event) {
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int hv = title_at(Fl::event_x());
      if (hv != hover_) {
        hover_ = hv;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
  }
  int r = Fl_Menu_Bar::handle(event);
  if (event == FL_PUSH) {
    hover_ = -1;
    redraw();
  }
  return r;
}

// ---------------------------------------------------------------- toolbar

namespace {

const char *tool_label(int t) {
  switch (t) {
    case icon::TAdd: return _("Add");
    case icon::TExtract: return _("Extract To");
    case icon::TTest: return _("Test");
    case icon::TView: return _("View");
    case icon::TDelete: return _("Delete");
    case icon::TFind: return _("Find");
    case icon::TWizard: return _("Wizard");
    case icon::TInfo: return _("Info");
    case icon::TRepair: return _("Repair");
  }
  return "";
}

const char *tool_tip(int t) {
  switch (t) {
    case icon::TAdd: return _("Add files to an archive (Alt+A)");
    case icon::TExtract: return _("Extract to a folder (Alt+E)");
    case icon::TTest: return _("Test the archive for errors (Alt+T)");
    case icon::TView: return _("View the selected file (Alt+V)");
    case icon::TDelete: return _("Delete the selected items (Del)");
    case icon::TFind: return _("Find files by name (F3)");
    case icon::TWizard: return _("Step by step: create, extract or convert (Alt+Z)");
    case icon::TInfo: return _("Show information (Alt+I)");
    case icon::TRepair: return _("Repair a damaged archive (Alt+R)");
  }
  return "";
}

}  // namespace

Toolbar::Toolbar(TacaWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

int Toolbar::wanted_height(int wh) { return wh < S(560) ? S(56) : S(70); }

bool Toolbar::compact() const { return h() < S(64); }

std::vector<Toolbar::Button> Toolbar::layout() const {
  const Theme &t = app_->theme();
  text_font(t.font_size - (compact() ? S(1) : 0));
  int isz = compact() ? S(24) : S(32);
  std::vector<Button> out;
  std::vector<int> widths;
  int total = 0;
  for (int i = 0; i < icon::ToolCount; ++i) {
    int bw = std::max(isz + S(26), text_width(tool_label(i)) + S(18));
    widths.push_back(bw);
    total += bw;
  }
  int sep = S(14), avail = w() - S(12);
  bool labels = total + sep <= avail;
  int X = x() + S(6);
  for (int i = 0; i < icon::ToolCount; ++i) {
    if (i == icon::TRepair) X += sep;
    int bw = labels ? widths[size_t(i)] : std::max(isz + S(10), std::min(widths[size_t(i)], (avail - sep) / icon::ToolCount));
    out.push_back({i, X, bw});
    X += bw;
  }
  return out;
}

int Toolbar::at(int mx, int my) const {
  if (my < y() || my >= y() + h()) return -1;
  for (const auto &b : layout())
    if (mx >= b.x && mx < b.x + b.w) return b.tool;
  return -1;
}

void Toolbar::draw() {
  const Theme &t = app_->theme();
  auto buttons = layout();
  bool small = compact();
  int isz = small ? S(24) : S(32);
  {
    Painter p(window()->w(), window()->h());
    p.fill_gradient(x(), y(), w(), h(), 0, t.surface, t.surface.mix(t.surface_alt, 0.6));
    p.line(x(), y() + h() - 0.5, x() + w(), y() + h() - 0.5, t.border.alpha(0.8));
    for (const auto &b : buttons) {
      bool on = !is_enabled || is_enabled(b.tool);
      int bx = b.x + S(2), by = y() + S(4), bw = b.w - S(4), bh = h() - S(8);
      if (on && b.tool == down_) p.fill_rounded(bx, by, bw, bh, S(8), t.accent.alpha(t.dark ? 0.35 : 0.2));
      else if (on && b.tool == hover_) {
        p.fill_rounded(bx, by, bw, bh, S(8), t.hover);
        p.stroke_rounded(bx + 0.5, by + 0.5, bw - 1, bh - 1, S(8), t.border.alpha(0.8));
      }
      if (b.tool == icon::TRepair) {
        double sx = b.x - S(7) + 0.5;
        p.line(sx, y() + S(10), sx, y() + h() - S(10), t.border);
      }
      icon::tool(p, icon::Tool(b.tool), b.x + (b.w - isz) / 2.0, y() + (small ? S(5) : S(6)), isz, t.dark, on);
    }
  }
  text_font(t.font_size - (small ? S(1) : 0));
  for (const auto &b : buttons) {
    bool on = !is_enabled || is_enabled(b.tool);
    if (text_width(tool_label(b.tool)) > b.w - S(4)) continue;  // icons only when narrow
    draw_text(tool_label(b.tool), b.x, y() + isz + (small ? S(6) : S(8)), b.w, small ? S(18) : S(22),
              on ? t.text : t.text_dim.alpha(0.55), 1);
  }
}

int Toolbar::handle(int event) {
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int hv = at(Fl::event_x(), Fl::event_y());
      if (hv != hover_) {
        hover_ = hv;
        redraw();
        if (hv >= 0) {
          for (const auto &b : layout())
            if (b.tool == hv) Fl_Tooltip::enter_area(this, b.x, y(), b.w, h(), tool_tip(hv));
        } else {
          Fl_Tooltip::exit(this);
        }
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      Fl_Tooltip::exit(this);
      redraw();
      return 1;
    case FL_PUSH: {
      int b = at(Fl::event_x(), Fl::event_y());
      if (b >= 0 && (!is_enabled || is_enabled(b)) && Fl::event_button() == FL_LEFT_MOUSE) {
        down_ = b;
        redraw();
      }
      return 1;
    }
    case FL_DRAG: {
      int b = at(Fl::event_x(), Fl::event_y());
      if (down_ >= 0 && b != hover_) {
        hover_ = b;
        redraw();
      }
      return 1;
    }
    case FL_RELEASE: {
      int b = at(Fl::event_x(), Fl::event_y());
      int was = down_;
      down_ = -1;
      redraw();
      Fl_Tooltip::exit(this);
      if (was >= 0 && b == was && on_click) on_click(b);
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}

}  // namespace taca
