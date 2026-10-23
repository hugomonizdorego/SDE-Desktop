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

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
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
      {N_("Lock"), {"system-lock-screen", "changes-prevent"}, {"sde-lock"}},
      {N_("Suspend"), {"system-suspend", "media-playback-pause"}, {"systemctl", "suspend"}},
      {N_("Log Out"), {"system-log-out", "application-exit"}, {"sde-session", "--logout"}},
      {N_("Restart"), {"system-reboot", "view-refresh"}, {"systemctl", "reboot"}},
      {N_("Shut Down"), {"system-shutdown", "application-exit"}, {"systemctl", "poweroff"}},
  };
  return list;
}

AppInfo app_info() {
  AppInfo info;
  info.name = _("Leave SDE");
  info.program = "sde-logout";
  info.version = SDE_VERSION;
  info.icon = "sde-logout";
  info.description = _("Locks the screen, suspends, logs out, restarts or shuts down the computer.");
  info.credits = {toolkit_credit()};
  return info;
}

class LogoutWindow : public sde::BufferedWindow {
 public:
  LogoutWindow(int w, int h) : sde::BufferedWindow(w, h, _("Leave SDE")) {}

  int cell() const { return S(104); }
  int button_at(int x, int y) const {
    int n = int(actions().size());
    int bx0 = (w() - n * cell()) / 2;
    if (y < S(76) || y > S(76) + cell()) return -1;
    int i = (x - bx0) / cell();
    return (x >= bx0 && i >= 0 && i < n) ? i : -1;
  }

  void draw() override {
    int n = int(actions().size()), c = cell();
    int bx0 = (w() - n * c) / 2;
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), g_theme.background);
      for (int i = 0; i < n; ++i) {
        int bx = bx0 + i * c;
        bool hl = i == hover_ || i == focus_;
        p.fill_rounded(bx + S(6), S(76), c - S(12), c, S(14), hl ? g_theme.accent.alpha(0.16) : g_theme.surface);
        if (i == focus_) p.stroke_rounded(bx + S(6), S(76), c - S(12), c, S(14), g_theme.accent, Sd(2));
        p.image(IconLoader::get(actions()[size_t(i)].icons, S(40)), bx + (c - S(40)) / 2.0, S(92), S(40));
      }
    }
    text_font(g_theme.font_size + 4, true);
    draw_text(_("Leave the desktop?"), 0, S(18), w(), S(30), g_theme.text, 1);
    text_font(g_theme.font_size);
    draw_text(_("Save your work before you continue."), 0, S(44), w(), S(22), g_theme.text_dim, 1);
    for (int i = 0; i < n; ++i) {
      text_font(g_theme.font_size, i == focus_);
      draw_text(_(actions()[size_t(i)].label), bx0 + i * c + S(6), S(144), c - S(12), S(24), g_theme.text, 1);
    }
    text_font(g_theme.font_size - 1);
    draw_text(_("Use the arrow keys and Enter, or press Esc to cancel"), 0, h() - S(30), w(), S(22),
              g_theme.text_dim, 1);
  }

  void run_action(int i) {
    if (i < 0) return;
    const Action &a = actions()[size_t(i)];
    if (find_program(a.command[0]).empty()) {
      fl_alert(_("\"%s\" is not available on this system (%s is missing)."), _(a.label),
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
        if (Fl::event_button() == FL_RIGHT_MOUSE) return handle_about_event(e, app_info());
        return 1;
      case FL_RELEASE:
        if (Fl::event_button() != FL_LEFT_MOUSE) return 1;
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
        else if (k == FL_F + 1) return handle_about_event(e, app_info());
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
  init_i18n();
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--version")) return print_version(app_info());
    // sde-logout --action NAME runs one action without the dialog (the
    // Action Center's power buttons).
    if (!strcmp(argv[i], "--action") && i + 1 < argc) {
      static const char *names[] = {"lock", "suspend", "logout", "reboot", "poweroff"};
      for (size_t a = 0; a < actions().size() && a < 5; ++a)
        if (!strcmp(argv[i + 1], names[a])) {
          const auto &cmd = actions()[a].command;
          if (find_program(cmd[0]).empty()) {
            fprintf(stderr, "sde-logout: %s is missing\n", cmd[0].c_str());
            return 1;
          }
          return spawn_detached(cmd) ? 0 : 1;
        }
      fprintf(stderr, "sde-logout: unknown action %s\n", argv[i + 1]);
      return 2;
    }
    printf("Usage: sde-logout [--action lock|suspend|logout|reboot|poweroff] [--version]\n");
    return strcmp(argv[i], "--help") ? 2 : 0;
  }
  fl_open_display();
  Config cfg;
  cfg.load();
  g_theme = Theme::from_config(cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));

  auto *win = new LogoutWindow(5 * S(104) + S(48), S(236));
  win->end();
  win->xclass("sde-logout");
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, 0);
  win->position(sx + (sw - win->w()) / 2, sy + (sh - win->h()) / 2);
  win->show();
  return Fl::run();
}
