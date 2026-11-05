// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Control Center > Workspaces (0.2.0 Alpha 4, sde-settings --page
// workspaces; docs/WINDOWS.md): how many workspaces SDE has (1 to 5), their
// names, whether Ctrl+Win+Left/Right go round, and whether the taskbar and
// Alt+Tab show the windows of every workspace. A change reaches SDE WM at
// once (EWMH) and its rc.xml for the next login (sync_openbox).
#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Window.H>
#include <FL/x.H>

#include <string>
#include <vector>

#include "config.h"
#include "i18n.h"
#include "paint.h"
#include "settings.h"
#include "shortcuts.h"
#include "spawn.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "workspaces.h"

namespace sde {

namespace {

workspaces::Settings current() { return workspaces::Settings::from_config(settings_config()); }

void save(const workspaces::Settings &s) {
  s.save(settings_config());
  apply_change();
  if (fl_display) workspaces::apply_live(fl_display, s);
}

// The workspaces as small screens with their names; the one in view is
// marked with the accent. A click shows that workspace.
class WorkspaceStrip : public Fl_Widget {
 public:
  WorkspaceStrip(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}

  int tile_w() const { return std::min(S(150), (w() - S(12) * (workspaces::kMax - 1)) / workspaces::kMax); }

  void draw() override {
    const Theme &t = settings_theme();
    auto s = current();
    auto live = workspaces::read_live(fl_display);
    int n = workspaces::clamp_count(s.count), tw = tile_w(), gap = S(12), th = h() - S(26);
    int x0 = x() + (w() - (n * tw + (n - 1) * gap)) / 2;
    {
      Painter p(window()->w(), window()->h());
      for (int i = 0; i < n; ++i) {
        int tx = x0 + i * (tw + gap);
        bool on = i == live.current;
        p.fill_rounded(tx, y(), tw, th, t.radius_small, on ? t.accent.alpha(0.18) : t.surface_alt);
        p.stroke_rounded(tx + 0.5, y() + 0.5, tw - 1, th - 1, t.radius_small, on ? t.accent : t.border,
                         on ? Sd(2) : Sd(1));
        // Two windows on the small screen.
        p.fill_rounded(tx + tw * 0.14, y() + th * 0.2, tw * 0.42, th * 0.46, S(3), t.surface);
        p.fill_rounded(tx + tw * 0.46, y() + th * 0.36, tw * 0.4, th * 0.44, S(3), t.surface);
        p.fill_rounded(tx + tw * 0.46, y() + th * 0.36, tw * 0.4, th * 0.08, S(2),
                       on ? t.accent : t.text_dim.alpha(0.5));
      }
    }
    for (int i = 0; i < n; ++i) {
      bool on = i == live.current;
      text_font(t.font_size - 1, on);
      draw_text(s.name(i), x0 + i * (tw + gap), y() + th + S(4), tw, S(22), on ? t.accent_text : t.text, 1);
    }
  }

  int handle(int e) override {
    if (e == FL_PUSH) return 1;
    if (e == FL_RELEASE && Fl::event_inside(this)) {
      auto s = current();
      int n = workspaces::clamp_count(s.count), tw = tile_w(), gap = S(12);
      int x0 = x() + (w() - (n * tw + (n - 1) * gap)) / 2;
      int i = (Fl::event_x() - x0) / (tw + gap);
      if (Fl::event_x() >= x0 && i >= 0 && i < n) {
        workspaces::switch_to(fl_display, i);
        Fl::add_timeout(0.2, [](void *w) { static_cast<Fl_Widget *>(w)->redraw(); }, this);
      }
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

// The name of workspace |index|, saved when Enter is pressed or the field
// is left.
Fl_Widget *name_field(int index, int x, int y, int w, int h) {
  const Theme &t = settings_theme();
  auto *in = new Fl_Input(x, y, w, h);
  // A soft rounded field (install_soft_boxes()), as on Users & Avatar.
  in->box(FL_DOWN_BOX);
  in->color(t.surface_alt.fl());
  in->textcolor(t.text.fl());
  in->cursor_color(t.accent.fl());
  in->selection_color(t.accent.fl());
  in->textsize(t.font_size + 1);
  in->value(current().name(index).c_str());
  in->maximum_size(40);
  in->when(FL_WHEN_ENTER_KEY_ALWAYS | FL_WHEN_RELEASE);
  in->callback([](Fl_Widget *w, void *data) {
    int i = int(reinterpret_cast<long>(data));
    auto s = current();
    std::string v = trim(static_cast<Fl_Input *>(w)->value());
    if (v == s.name(i)) return;
    s.names[size_t(i)] = v;
    save(s);
    if (v.empty()) static_cast<Fl_Input *>(w)->value(s.name(i).c_str());
    refresh_if_shown("workspaces");
  }, reinterpret_cast<void *>(long(index)));
  return in;
}

}  // namespace

// The tile and sidebar symbol: two screens side by side.
void draw_workspaces_glyph(Painter &p, double x, double y, double s, const Color &c) {
  double lw = std::max(1.2, s * 0.08);
  p.stroke_rounded(x + s * 0.04, y + s * 0.2, s * 0.56, s * 0.42, s * 0.08, c, lw);
  p.stroke_rounded(x + s * 0.4, y + s * 0.38, s * 0.56, s * 0.42, s * 0.08, c, lw);
}

void build_workspaces_page(Page *page) {
  auto s = current();
  page->heading(_("Workspaces"), _("Desktops of their own for your windows"));
  new WorkspaceStrip(page->left(), page->cursor(), page->width(), S(110));
  page->advance(S(110) + S(14));

  page->begin_card(_("Workspaces"));
  std::vector<std::string> counts;
  for (int i = workspaces::kMin; i <= workspaces::kMax; ++i) counts.push_back(std::to_string(i));
  page->choice_row(_("Number of workspaces"), counts, workspaces::clamp_count(s.count) - workspaces::kMin,
                   [](int v) {
                     auto s = current();
                     s.count = workspaces::clamp_count(v + workspaces::kMin);
                     save(s);
                     Fl::add_timeout(0, [](void *) { refresh_page(); });
                   },
                   _("Windows on a workspace that goes away move to the last one."));
  for (int i = 0; i < workspaces::clamp_count(s.count); ++i)
    page->row(format(_("Name of workspace %d"), i + 1), S(240), S(32),
              [i](int x, int y, int w, int h) { return name_field(i, x, y, w, h); });
  page->switch_row(_("Go round from the last workspace to the first"), s.wrap, [](bool on) {
    auto s = current();
    s.wrap = on;
    save(s);
  }, _("Ctrl+Win+Left and Ctrl+Win+Right"));
  page->end_card();

  page->begin_card(_("Windows on other workspaces"));
  page->switch_row(_("Show them on the taskbar"), s.taskbar_all, [](bool on) {
    auto s = current();
    s.taskbar_all = on;
    save(s);
  }, _("Off: the taskbar shows the windows of the workspace in view."));
  page->switch_row(_("Show them with Alt+Tab"), s.alt_tab_all, [](bool on) {
    auto s = current();
    s.alt_tab_all = on;
    save(s);
  });
  page->end_card();

  page->begin_card(_("Keyboard"));
  page->note(_("Win+Tab shows Task View: every workspace and its windows. Ctrl+Win+Left and Ctrl+Win+Right "
               "go to the workspace next to this one, Ctrl+Win+Shift+Left and Right take the active "
               "window with you. Ctrl+Win+D adds a workspace, Ctrl+Win+F4 closes the one in view; its "
               "windows move to the workspace next to it."));
  page->row(_("Task View"), S(132), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Open"), true);
    b->callback([](Fl_Widget *, void *) { spawn_detached({"sde-taskview"}); });
    return b;
  });
  page->row(_("All keyboard shortcuts"), S(132), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Open"), false);
    b->callback([](Fl_Widget *, void *) { shortcuts::open_sheet(); });
    return b;
  }, _("The shortcut sheet (PDF), ready to print."));
  page->end_card();
  page->finish();
}

}  // namespace sde
