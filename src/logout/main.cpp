// SPDX-License-Identifier: GPL-3.0-or-later
// sde-logout: Log out / Restart / Shut down / Suspend dialog.
// Power actions use systemd-logind through systemctl, which applies the
// system's polkit policy (no root privileges are requested by SDE).
#include <FL/Fl.H>
#include "buffered_window.h"
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "icons.h"
#include "paint.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"

using namespace sde;

namespace {

Theme g_theme;

struct Action {
  const char *label;
  std::vector<std::string> icons;
  std::vector<std::string> command;
};

const std::vector<Action> &actions() {
  static const std::vector<Action> list = {
      {"Lock", {"system-lock-screen", "changes-prevent"}, {"xdg-screensaver", "lock"}},
      {"Suspend", {"system-suspend", "media-playback-pause"}, {"systemctl", "suspend"}},
      {"Log Out", {"system-log-out", "application-exit"}, {"sde-session", "--logout"}},
      {"Restart", {"system-reboot", "view-refresh"}, {"systemctl", "reboot"}},
      {"Shut Down", {"system-shutdown", "application-exit"}, {"systemctl", "poweroff"}},
  };
  return list;
}

class LogoutWindow : public sde::BufferedWindow {
 public:
  LogoutWindow(int w, int h) : sde::BufferedWindow(w, h, "Leave SDE") {}

  int button_at(int x, int y) const {
    int n = int(actions().size());
    int bx0 = (w() - n * 104) / 2;
    if (y < 76 || y > 76 + 104) return -1;
    int i = (x - bx0) / 104;
    return (x >= bx0 && i >= 0 && i < n) ? i : -1;
  }

  void draw() override {
    int n = int(actions().size());
    int bx0 = (w() - n * 104) / 2;
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), g_theme.background);
      for (int i = 0; i < n; ++i) {
        int bx = bx0 + i * 104;
        bool hl = i == hover_ || i == focus_;
        p.fill_rounded(bx + 6, 76, 92, 104, 14, hl ? g_theme.accent.alpha(0.16) : g_theme.surface);
        if (i == focus_) p.stroke_rounded(bx + 6, 76, 92, 104, 14, g_theme.accent, 2);
        p.image(IconLoader::get(actions()[size_t(i)].icons, 40), bx + 32, 92, 40);
      }
    }
    text_font(g_theme.font_size + 4, true);
    draw_text("Leave the desktop?", 0, 18, w(), 30, g_theme.text, 1);
    text_font(g_theme.font_size);
    draw_text("Save your work before you continue.", 0, 44, w(), 22, g_theme.text_dim, 1);
    for (int i = 0; i < n; ++i) {
      text_font(g_theme.font_size, i == focus_);
      draw_text(actions()[size_t(i)].label, bx0 + i * 104 + 6, 144, 92, 24, g_theme.text, 1);
    }
    text_font(g_theme.font_size - 1);
    draw_text("Press Esc to cancel", 0, h() - 30, w(), 22, g_theme.text_dim, 1);
  }

  void run_action(int i) {
    if (i < 0) return;
    const Action &a = actions()[size_t(i)];
    if (find_program(a.command[0]).empty()) {
      fl_alert("\"%s\" is not available on this system (%s is missing).", a.label,
               a.command[0].c_str());
      return;
    }
    hide();
    spawn_detached(a.command);
  }

  int handle(int e) override {
    switch (e) {
      case FL_MOVE:
      case FL_ENTER: {
        int i = button_at(Fl::event_x(), Fl::event_y());
        if (i != hover_) {
          hover_ = i;
          redraw();
        }
        return 1;
      }
      case FL_PUSH:
        return 1;
      case FL_RELEASE:
        run_action(button_at(Fl::event_x(), Fl::event_y()));
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key(), n = int(actions().size());
        if (k == FL_Left) focus_ = (focus_ + n - 1) % n;
        else if (k == FL_Right || k == FL_Tab) focus_ = (focus_ + 1) % n;
        else if (k == FL_Enter || k == FL_KP_Enter || k == ' ') run_action(focus_);
        else if (k == FL_Escape) hide();
        else return 0;
        redraw();
        return 1;
      }
      default:
        return sde::BufferedWindow::handle(e);
    }
  }

 private:
  int hover_ = -1;
  int focus_ = 2;  // "Log Out"
};

}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) {
      printf("sde-logout %s (%s)\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    }
    printf("Usage: sde-logout [--version]\n");
    return strcmp(argv[i], "--help") ? 2 : 0;
  }
  fl_open_display();
  Config cfg;
  cfg.load();
  g_theme = Theme::from_config(cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));

  auto *win = new LogoutWindow(5 * 104 + 48, 236);
  win->end();
  win->xclass("sde-logout");
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, 0);
  win->position(sx + (sw - win->w()) / 2, sy + (sh - win->h()) / 2);
  win->show();
  return Fl::run();
}
