// SPDX-License-Identifier: GPL-3.0-or-later
// sde-panel: application menu, pinned launchers, taskbar, system tray,
// quick settings (network, sound, brightness, battery) and clock/calendar.
// The panel sits at the bottom or top of the primary monitor and can hide
// itself automatically.
#pragma once

#include <X11/Xlib.h>
#include <cairo.h>

#include <memory>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "config.h"
#include "desktop_entry.h"
#include "theme.h"

class AppMenu;
class Calendar;
class PanelPopup;
class QuickSettings;
class SniHost;
class Tray;

struct Task {
  Window win = 0;
  std::string title;
  std::string wm_class;  // res_class
  std::string wm_name;   // res_name
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
  void toggle_calendar();
  void toggle_dark_mode();
  const sde::Theme &theme() const { return theme_; }
  const sde::Config &config() const { return cfg_; }
  void monitor(int &x, int &y, int &w, int &h) const;
  void relayout();

  // Pinned applications (desktop file IDs), saved in sde.conf.
  bool is_pinned(const std::string &id) const;
  void pin(const std::string &id);
  void unpin(const std::string &id);
  void set_do_not_disturb(bool on);

  // Shows a menu next to the panel (above it, or below a top panel), with its
  // left edge at root x |root_x|. Returns the picked item or nullptr.
  const struct Fl_Menu_Item *popup_menu(const struct Fl_Menu_Item *items, int root_x);

  // Bookkeeping for the pop-up windows.
  void popup_opened(PanelPopup *p);
  void popup_closed(PanelPopup *p);

  // Raw X event hook (installed with Fl::add_handler).
  static int x_event_handler(int event);
  static Panel *instance;

 private:
  enum Hit { NONE, START, PINNED, TASK, TRAY_ITEM, STATUS, CLOCK, SHOW_DESKTOP };
  struct Target {
    Hit kind = NONE;
    int index = -1;
    bool operator==(const Target &o) const { return kind == o.kind && index == o.index; }
  };

  void load_config();
  void place_on_screen();
  void apply_strut();
  void apply_shape();
  void refresh_tasks();
  void update_task(Task &t);
  Target hit(int x, int y) const;
  void task_clicked(int index, int button);
  void pinned_menu(int index);
  void tray_item_clicked(int index, int button);
  void show_context_menu();
  bool handle_x(const XEvent &e);
  void save_pins();
  bool find_entry_for(const Task &t, sde::DesktopEntry &out) const;
  void open_popup(PanelPopup *p, int anchor_x, bool align_right);
  bool any_popup_open() const;
  int status_count() const;

  // Auto-hide.
  void set_revealed(bool revealed);
  static void hide_timeout(void *data);

  static void clock_tick(void *);

  sde::Config cfg_;
  sde::Theme theme_;
  std::vector<Task> tasks_;
  std::vector<Pinned> pinned_;
  Window active_ = 0;
  Target hover_;
  Target pressed_;
  std::unique_ptr<AppMenu> menu_;
  std::unique_ptr<QuickSettings> quick_;
  std::unique_ptr<Calendar> calendar_;
  std::unique_ptr<Tray> tray_;
  std::unique_ptr<SniHost> sni_;
  PanelPopup *open_popup_ = nullptr;
  int monitor_x_ = 0, monitor_y_ = 0, monitor_w_ = 0, monitor_h_ = 0;

  // Layout (window coordinates).
  int start_w_ = 0, pin_w_ = 0, tasks_x_ = 0, tasks_end_ = 0, tray_x_ = 0, sni_x_ = 0, sni_w_ = 0,
      status_x_ = 0, status_w_ = 0, clock_x_ = 0, clock_w_ = 0, show_desktop_x_ = 0;
  int icon_ = 24, tray_icon_ = 22;
  int edge_reserve_ = 0, visible_pins_ = 0;

  // Dragging a pinned launcher to reorder it.
  int drag_index_ = -1;
  int drag_start_x_ = 0, drag_x_ = 0;
  bool dragging_ = false;

  bool autohide_ = false;
  bool revealed_ = true;

  std::string clock_time_, clock_date_;
  bool configured_ = false;
};
