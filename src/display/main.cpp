// SPDX-License-Identifier: GPL-3.0-or-later
#include <FL/Fl.H>
#include <FL/x.H>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "integration.h"
#include "paint.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"
#include "x11.h"
namespace sde {
namespace {
Config cfg;
Theme theme;
BufferedWindow* win = nullptr;
Page* page = nullptr;
}  // namespace
Config& settings_config() { return cfg; }
const Theme& settings_theme() { return theme; }
void refresh_page() {
  if (!win) return;
  int scroll = page ? page->yposition() : 0;
  if (page) {
    win->remove(page);
    Fl::delete_widget(page);
  }
  win->begin();
  page = new Page(0, 0, win->w(), win->h());
  build_display_page(page);
  win->end();
  if (scroll) page->scroll_to(0, scroll);
  win->redraw();
}
void apply_change() {
  double old = theme.scale;
  cfg.save();
  sync_gtk_settings(cfg);
  x11::notify_config_changed();
  if (std::fabs(old - Theme::from_config(cfg).scale) > .01)
    Fl::add_timeout(.2, [](void*) {
      execl("/proc/self/exe", "sde-display-settings", static_cast<char*>(nullptr));
    });
}
class DisplayWindow : public BufferedWindow {
 public:
  DisplayWindow(int w, int h) : BufferedWindow(w, h, _("Display Settings")) {}
  void draw() override {
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), theme.background);
    }
    draw_children();
  }
};
}  // namespace sde
int main(int argc, char** argv) {
  using namespace sde;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") {
      printf("Display Settings %s (SDE alpha; DisplayProfileManager MIT port)\n",
             SDE_DISPLAY_VERSION);
      return 0;
    }
    if (a == "--help") {
      puts(
          "Usage: sde-display-settings [--version]\nNative X11 monitor settings and display "
          "profiles for SDE.");
      return 0;
    }
    return 2;
  }
  init_i18n();
  cfg.load();
  fl_open_display();
  theme = Theme::from_config(cfg);
  theme.apply_to_fltk();
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  int x, y, w, h;
  Fl::screen_work_area(x, y, w, h, 0);
  int W = std::min(S(780), w - S(24)), H = std::min(S(760), h - S(48));
  win = new DisplayWindow(W, H);
  win->end();
  win->xclass("sde-display-settings");
  win->position(x + (w - W) / 2, y + (h - H) / 2);
  win->resizable(nullptr);
  refresh_page();
  start_display_watch();
  win->show();
  return Fl::run();
}
