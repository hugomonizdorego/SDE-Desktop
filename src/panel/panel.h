// SPDX-License-Identifier: GPL-3.0-or-later
// sde-panel: the bottom panel with application menu, pinned launchers,
// taskbar, system tray, battery and clock.
#pragma once

#include "buffered_window.h"
#include <X11/Xlib.h>
#include <cairo.h>

#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "desktop_entry.h"
#include "theme.h"

class AppMenu;
class Tray;

struct Task {
  Window win = 0;
  std::string title;
  cairo_surface_t *icon = nullptr;  // owned
  bool minimized = false;
  bool urgent = false;
  int x = 0, w = 0;
};

struct Pinned {
  sde::DesktopEntry entry;
  int x = 0;
};

class Panel : public sde::BufferedWindow {
 public:
  Panel();
  ~Panel() override;

  // Called once after show(): sets the dock type and strut, starts the tray
  // and subscribes to root window events.
  void setup();
  void draw() override;
  int handle(int event) override;
  void resize(int x, int y, int w, int h) override;

  void toggle_menu();
  const sde::Theme &theme() const { return theme_; }
  const sde::Config &config() const { return cfg_; }
  void relayout();

  // Raw X event hook (installed with Fl::add_handler).
  static int x_event_handler(int event);
  static Panel *instance;

 private:
  enum Hit { NONE, START, PINNED, TASK, BATTERY, CLOCK, SHOW_DESKTOP };
  struct Target {
    Hit kind = NONE;
    int index = -1;
    bool operator==(const Target &o) const { return kind == o.kind && index == o.index; }
  };

  void load_config();
  void place_on_screen();
  void refresh_tasks();
  void update_task(Task &t);
  void update_battery();
  Target hit(int x, int y) const;
  void task_clicked(int index, int button);
  void show_context_menu();
  bool handle_x(const XEvent &e);
  static void clock_tick(void *);
  static void battery_tick(void *);

  sde::Config cfg_;
  sde::Theme theme_;
  std::vector<Task> tasks_;
  std::vector<Pinned> pinned_;
  Window active_ = 0;
  Target hover_;
  Target pressed_;
  std::unique_ptr<AppMenu> menu_;
  std::unique_ptr<Tray> tray_;
  int monitor_x_ = 0, monitor_y_ = 0, monitor_w_ = 0, monitor_h_ = 0;

  // Layout (window coordinates).
  int start_w_ = 0, tasks_x_ = 0, tasks_end_ = 0, tray_x_ = 0, battery_x_ = 0, battery_w_ = 0,
      clock_x_ = 0, clock_w_ = 0, show_desktop_x_ = 0;

  // Battery state; percent < 0 means "no battery".
  int battery_percent_ = -1;
  bool battery_charging_ = false;
  std::string clock_time_, clock_date_;
  bool configured_ = false;
};
