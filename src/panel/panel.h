// SPDX-License-Identifier: GPL-3.0-or-later
// sde-panel: application menu, pinned launchers, taskbar, system tray,
// status flyouts (sound, network, Bluetooth, power, ...), clock and the
// Action Center with its calendar.
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
#include "tooltip_text.h"

class AlarmPopup;
class AppMenu;
class PanelPopup;
class QuickSettings;
class SearchFlyout;
class SniHost;
class StatusFlyout;
class Tray;

struct Task {
  Window win = 0;
  std::string title;
  std::string wm_class;             // res_class
  std::string wm_name;              // res_name
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

  // The Action Center; with |calendar| it opens with the calendar in view
  // (the clock, sde-panel --calendar). Clicking the same button closes it.
  void open_action_center(bool calendar = false);
  // The flyout of one status icon (sound, network, bluetooth, power, media,
  // drives, keyboard, tray), anchored above that icon; toggles.
  void open_flyout(const std::string &section);
  // Network, Drives and Touchpad (status_flyout.h); toggles like open_flyout.
  void open_status_flyout(const std::string &section);
  // The clock's popup: alarms (sde-panel --alarms); with |add| it opens the
  // editor for a new alarm. Toggles.
  void open_alarms(bool add = false);
  bool native_popup_input() const { return input_display_ != nullptr; }
  // The status icon of |section| needs a repaint (network LED, touchpad).
  void status_icon_changed(const std::string &section);
  // The popup that currently holds the panel's XEmbed tray icons (the Tray
  // flyout with [action_center] move_status), or nullptr.
  QuickSettings *tray_host() const;
  Tray *legacy_tray() const { return tray_.get(); }
  SniHost *status_notifiers() const { return sni_.get(); }
  void toggle_menu();
  // The search flyout above the panel's search box, with |text| typed. Called
  // without text it toggles.
  // keep_popup: the caller (the Start menu handing over typed text) closes
  // itself once the search has the keyboard.
  void open_search(const std::string &text = "", bool keep_popup = false);
  // [panel] search=box: the panel shows the box (and the query while the
  // flyout is open); icon or hidden: the flyout shows its own field.
  bool search_box_shown() const { return search_mode_ == 2 && search_w_ > 0; }
  bool search_box_contains(int root_x, int root_y) const;
  // The query or caret changed: repaint the search box.
  void search_changed();
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
  // Saves one key in the user's sde.conf and announces the change.
  void set_setting(const std::string &section, const std::string &key, const std::string &value);
  struct Setting {
    std::string section, key, value;
  };
  // Several keys, saved and announced once.
  void set_settings(const std::vector<Setting> &settings);

  // Shows a menu next to the panel (above it, or below a top panel), with its
  // left edge at root x |root_x|. Returns the picked item or nullptr.
  const struct Fl_Menu_Item *popup_menu(const struct Fl_Menu_Item *items, int root_x);

  // Bookkeeping for the pop-up windows.
  void popup_opened(PanelPopup *p);
  void popup_closed(PanelPopup *p);

  // Raw X event hook (installed with Fl::add_handler).
  static int x_event_handler(int event);
  static Panel *instance;
  // sde-panel --safe: no tray, StatusNotifier, D-Bus status or device
  // polling; started by sde-session after the panel keeps crashing.
  static bool safe_mode;

 private:
  enum Hit { NONE, START, SEARCH, PINNED, TASK, TRAY_ITEM, STATUS, CLOCK, ACTION_CENTER, SHOW_DESKTOP };
  enum StatusIcon {
    INPUT_STATUS,
    BLUETOOTH_STATUS,
    BATTERY_STATUS,
    POWER_STATUS,
    MEDIA_STATUS,
    NETWORK_STATUS,
    SOUND_STATUS,
    CONTROLS_STATUS,
    OVERFLOW_STATUS,
    DRIVE_STATUS,     // a USB drive or memory card is plugged in
    DND_STATUS,       // Do Not Disturb is on
    TOUCHPAD_STATUS   // a touchpad exists ([panel] touchpad_indicator)
  };
  struct Target {
    Hit kind = NONE;
    int index = -1;
    bool operator==(const Target &o) const { return kind == o.kind && index == o.index; }
  };

  void load_config();
  std::string status_tooltip(StatusIcon icon) const;
  // Tooltips of the panel's parts (see tooltip_text.h): the text for the
  // hovered target, handed to Fl_Tooltip for that target's rectangle.
  std::string tooltip_for(const Target &t) const;
  bool target_rect(const Target &t, int &x, int &w) const;
  void update_tooltip();
  TooltipText tip_;
 public:
  const char *qa_tooltip() const;
 private:
  // An input method (IBus, Fcitx) shows its own keyboard indicator in the
  // tray: SDE's layout indicator then stays hidden (see relayout()).
  bool input_method_indicator() const;
  // Network LED: a small dot on the network icon, pulsing gently.
  static void led_tick(void *);
  void led_update();
  int status_index(StatusIcon icon) const;
  bool led_running_ = false;
  std::string led_connection_;  // kind|connection the verdict is for
  bool tz_pending_ = false;     // automatic time zone sync for this connection
  double led_phase_ = 0;
  bool led_probe_failed_ = false, led_probe_pending_ = false;
  long long led_probe_at_ = 0;
  // The touchpad indicator: animates while hovered or just after use.
  static void touchpad_tick(void *);
  void touchpad_activity();
  bool touchpad_motion_wanted() const;
  bool touchpad_tick_pending_ = false;
  static std::string status_section(StatusIcon icon);
  // Root x of the center of the visible status icon for |section|.
  bool status_anchor(const std::string &section, int &center_x) const;
  std::string popup_key(const PanelPopup *p) const;
  std::string target_key(const Target &t) const;
  bool recently_closed(const std::string &key) const;
  void qa_dump_panel();
  void place_on_screen();
  void apply_strut();
  void apply_shape();
  void refresh_tasks();
  void update_task(Task &t, bool icon = true);
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
  void status_clicked(int index, int button);
 public:
  // A click outside an open popup that lands on another panel button (the
  // clock while the Action Center is open, ...) closes the popup and opens
  // that button's popup at once. Returns true when it did.
  bool switch_popup(int root_x, int root_y, PanelPopup *from);
  // The time zone was set ("Sync time zone"): the clock shows it at once.
  void time_zone_changed() { clock_tick(this); }
 private:
  static void input_event(int, void *);
  void outside_pointer_press(unsigned long time);
  void recover_input();

  // Auto-hide.
  void set_revealed(bool revealed);
  static void hide_timeout(void *data);

  static void clock_tick(void *);
  // SystemStatus, PanelControls and the tray report many small changes;
  // they are folded into one panel and popup update per event loop turn.
  void status_update();
  static void status_update_timeout(void *);
  std::string status_signature() const;
  std::string status_signature_;
  bool status_update_pending_ = false;
  // The tray's application icons do not fit or exceed the threshold, so they
  // gather behind the overflow (^) button.
  bool tray_overflow_ = false;

  sde::Config cfg_;
  sde::Theme theme_;
  std::vector<Task> tasks_;
  std::vector<Pinned> pinned_;
  Window active_ = 0;
  Target hover_;
  Target pressed_;
  std::unique_ptr<AppMenu> menu_;
  std::unique_ptr<SearchFlyout> search_;
  std::unique_ptr<QuickSettings> quick_;
  std::unique_ptr<QuickSettings> flyout_;
  // Network, Drives and Touchpad flyouts (status_flyout.h).
  std::unique_ptr<StatusFlyout> status_flyout_;
  std::unique_ptr<AlarmPopup> alarms_;
  std::unique_ptr<Tray> tray_;
  std::unique_ptr<SniHost> sni_;
  PanelPopup *open_popup_ = nullptr;
  int monitor_x_ = 0, monitor_y_ = 0, monitor_w_ = 0, monitor_h_ = 0;

  // Layout (window coordinates).
  int start_w_ = 0, pin_w_ = 0, tasks_x_ = 0, tasks_end_ = 0, tray_x_ = 0, sni_x_ = 0, sni_w_ = 0,
      status_x_ = 0, status_w_ = 0, clock_x_ = 0, clock_w_ = 0, show_desktop_x_ = 0,
      show_desktop_w_ = 0;
  int icon_ = 24, tray_icon_ = 22;
  int search_x_ = 0, search_w_ = 0, search_mode_ = 0;  // mode: 0 hidden, 1 icon, 2 box
  int action_x_ = 0, action_w_ = 0;
  std::vector<StatusIcon> status_icons_;
  bool compact_status_ = false;
  Display *input_display_ = nullptr;
  int input_opcode_ = 0;
  // The popup closed by the press that started the current click, so that
  // the release on its own button does not reopen it.
  std::string closed_key_;
  long long closed_at_ = 0, popup_opened_at_ = 0;
  unsigned long popup_opened_time_ = 0;  // X server time of the opening event
  unsigned long last_press_time_ = 0;    // newest raw press seen on input_display_
  bool draining_input_ = false;
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
