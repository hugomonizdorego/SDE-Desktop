// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "alarm_popup.h"
#include "alarms.h"
#include "controls.h"
#include "glyphs.h"
#include "tray_icons.h"
#include "i18n.h"
#include "icons.h"
#include "menu.h"
#include "paint.h"
#include "preview.h"
#include "process.h"
#include "quick.h"
#include "rust_layout.h"
#include "search.h"
#include "sni.h"
#include "spawn.h"
#include "status_flyout.h"
#include "status_model.h"
#include "touchpad_monitor.h"
#include "tray.h"
#include "util.h"
#include "wm.h"
#include "x11.h"

using namespace sde;

namespace {
long long steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace
Panel *Panel::instance = nullptr;
bool Panel::safe_mode = false;

Panel::Panel() : sde::BufferedWindow(0, 0, 800, 40, "SDE Panel") {
  instance = this;
  end();
  border(0);
  resizable(nullptr);
  load_config();
  place_on_screen();
  menu_.reset(new AppMenu());
  search_.reset(new SearchFlyout());
  quick_.reset(new QuickSettings(QuickSettings::ACTION_CENTER));
  flyout_.reset(new QuickSettings(QuickSettings::FLYOUT));
  status_flyout_.reset(new StatusFlyout());
  alarms_.reset(new AlarmPopup());
  preview_.reset(new TaskPreview());
}

Panel::~Panel() {
  if (input_display_) {
    Fl::remove_fd(ConnectionNumber(input_display_));
    XCloseDisplay(input_display_);
    input_display_ = nullptr;
  }
  for (auto &t : tasks_)
    if (t.icon) cairo_surface_destroy(t.icon);
}

void Panel::monitor(int &x, int &y, int &w, int &h) const {
  x = monitor_x_;
  y = monitor_y_;
  w = monitor_w_;
  h = monitor_h_;
}

void Panel::load_config() {
  cfg_.load();
  theme_ = Theme::from_config(cfg_);
  theme_.apply_to_fltk();
  IconLoader::set_theme(cfg_.get("appearance", "icon_theme", "auto"));
  IconLoader::clear_cache();
  autohide_ = cfg_.get_bool("panel", "autohide", false);
  pinned_.clear();
  std::vector<std::string> seen_exec;
  for (auto &id : split(cfg_.get("panel", "pinned"), ';')) {
    Pinned p;
    if (!find_application(trim(id), p.entry) || !p.entry.visible_in("SDE")) continue;
    // Skip duplicates such as firefox.desktop and firefox-esr.desktop
    // pointing to the same kind of program.
    auto cmd = p.entry.command();
    std::string exec = cmd.empty() ? "" : basename_of(cmd[0]);
    if (std::find(seen_exec.begin(), seen_exec.end(), exec) != seen_exec.end()) continue;
    seen_exec.push_back(exec);
    pinned_.push_back(p);
    if (pinned_.size() >= 12) break;
  }
  if (shown()) {
    Fl_Window::color(theme_.surface.fl());
    XSetWindowBackground(fl_display, fl_xid(this), theme_.surface.fl() >> 8);
    x11::set_opacity(fl_xid(this), theme_.opacity);
  }
}

void Panel::place_on_screen() {
  Fl::screen_xywh(monitor_x_, monitor_y_, monitor_w_, monitor_h_, 0);
  std::string style = cfg_.get("panel", "style", "full");
  int kind = style == "floating" ? 1 : style == "short" ? 2 : 0;
  auto g = sde_panel_geometry(monitor_x_, monitor_y_, monitor_w_, monitor_h_, theme_.panel_height,
                              theme_.panel_top, kind, cfg_.get_int("panel", "width_percent", 96),
                              S(8), S(600), autohide_ && !revealed_);
  edge_reserve_ = g.reserve;
  sde::BufferedWindow::resize(g.x, g.y, g.width, g.height);
  relayout();
  if (shown()) {
    XMoveResizeWindow(fl_display, fl_xid(this), g.x, g.y, unsigned(g.width), unsigned(g.height));
    apply_strut();
    apply_shape();
    if (tray_) tray_->set_icon_size(tray_icon_);
  }
}

void Panel::apply_strut() {
  int reserve = autohide_ ? 0 : edge_reserve_;
  x11::set_edge_strut(fl_xid(this), theme_.panel_top, reserve, x(), x() + w() - 1, monitor_y_,
                      monitor_h_);
}

void Panel::apply_shape() {
  int event, error;
  if (!XShapeQueryExtension(fl_display, &event, &error)) return;
  if (cfg_.get("panel", "style", "full") == "full" || h() <= S(2)) {
    XShapeCombineMask(fl_display, fl_xid(this), ShapeBounding, 0, 0, None, ShapeSet);
    return;
  }
  int radius = std::min(S(16), h() / 2);
  std::vector<XRectangle> rows;
  for (int row = 0; row < h(); ++row) {
    double dy = row < radius          ? radius - row - 0.5
                : row >= h() - radius ? row - (h() - radius) + 0.5
                                      : 0;
    int inset =
        dy > 0 ? int(std::ceil(radius - std::sqrt(std::max(0.0, radius * radius - dy * dy)))) : 0;
    rows.push_back(
        {short(inset), short(row), static_cast<unsigned short>(std::max(1, w() - 2 * inset)), 1});
  }
  XShapeCombineRectangles(fl_display, fl_xid(this), ShapeBounding, 0, 0, rows.data(),
                          int(rows.size()), ShapeSet, YXBanded);
}

void Panel::resize(int x, int y, int w, int h) {
  sde::BufferedWindow::resize(x, y, w, h);
  relayout();
}

void Panel::setup() {
  Window xid = fl_xid(this);
  x11::set_window_type(xid, "_NET_WM_WINDOW_TYPE_DOCK");
  x11::set_atoms(xid, "_NET_WM_STATE",
                 {"_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER",
                  "_NET_WM_STATE_ABOVE"});
  x11::set_cardinals(xid, "_NET_WM_DESKTOP", {long(0xFFFFFFFF)});
  XClassHint hint{const_cast<char *>("sde-panel"), const_cast<char *>("SDE-Panel")};
  XSetClassHint(fl_display, xid, &hint);
  apply_strut();
  apply_shape();
  XSetWindowBackground(fl_display, xid, theme_.surface.fl() >> 8);
  x11::set_opacity(xid, theme_.opacity);
  x11::remap(xid);
  // Keep the requested position after the WM re-manages the window.
  XMoveResizeWindow(fl_display, xid, x(), y(), unsigned(w()), unsigned(h()));

  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask | StructureNotifyMask);
  // Taskbar clients and tray icons are not inside the panel window, so their
  // events must be monitored in addition to FLTK's own windows.
  XWindowAttributes own;
  XGetWindowAttributes(fl_display, xid, &own);
  XSelectInput(fl_display, xid, own.your_event_mask | SubstructureNotifyMask);

  input_display_ = XOpenDisplay(nullptr);
  if (input_display_) {
    int event, error, major = 2, minor = 0;
    if (!XQueryExtension(input_display_, "XInputExtension", &input_opcode_, &event, &error) ||
        XIQueryVersion(input_display_, &major, &minor) != Success) {
      XCloseDisplay(input_display_);
      input_display_ = nullptr;
    } else {
      unsigned char bits[XIMaskLen(XI_RawButtonPress)] = {};
      XISetMask(bits, XI_RawButtonPress);
      XIEventMask mask{XIAllMasterDevices, int(sizeof bits), bits};
      XISelectEvents(input_display_, DefaultRootWindow(input_display_), &mask, 1);
      XFlush(input_display_);
      Fl::add_fd(ConnectionNumber(input_display_), FL_READ, input_event, this);
    }
  }
  // Applets such as nm-applet, blueman-applet or a power manager keep running
  // as agents, but their icons would repeat SDE's own indicators.
  std::vector<std::string> hidden;
  // Alarms and agenda reminders ring even in safe mode.
  Alarms::get().start(
      [this] {
        alarms_->changed();
        quick_->status_changed();
      },
      [this] {
        if (!alarms_->is_open()) open_alarms();
      });
  if (safe_mode) {
    configured_ = true;
    refresh_tasks();
    clock_tick(this);
    return;
  }
  if (cfg_.get_bool("panel", "hide_duplicate_tray", true))
    for (auto &name : split(cfg_.get("panel", "duplicate_tray_items", ""), ';'))
      if (!trim(name).empty()) hidden.push_back(lower(trim(name)));
  tray_.reset(new Tray(xid));
  tray_->set_icon_size(tray_icon_);
  tray_->set_hidden(hidden);
  tray_->set_hide_layout_applets(cfg_.get_bool("panel", "show_keyboard", true));
  tray_->start();
  sni_.reset(new SniHost([this] {
    status_signature_.clear();  // icons can change without a new title
    status_update();
  }));
  sni_->set_hidden(hidden);
  sni_->set_hide_layout_applets(cfg_.get_bool("panel", "show_keyboard", true));
  sni_->start();
  SystemStatus::get().start([this] { status_update(); });
  TouchpadMonitor::get().start([this] { touchpad_activity(); },
                               [this] {
                                 status_signature_.clear();
                                 status_update();
                               });
  TouchpadMonitor::get().want_motion(touchpad_motion_wanted());

  configured_ = true;
  refresh_tasks();
  clock_tick(this);
  if (autohide_) Fl::add_timeout(1.5, hide_timeout, this);
  Fl::add_timeout(5.0, thumbnail_tick, this);
}

// ------------------------------------------------------------- auto-hide

void Panel::set_revealed(bool revealed) {
  if (revealed == revealed_) return;
  revealed_ = revealed;
  place_on_screen();
  redraw();
}

void Panel::hide_timeout(void *data) {
  auto *p = static_cast<Panel *>(data);
  if (!p->autohide_ || p->any_popup_open() || p->dragging_) return;
  int mx, my;
  Window root = RootWindow(fl_display, fl_screen), r, c;
  int wx, wy;
  unsigned mask;
  XQueryPointer(fl_display, root, &r, &c, &mx, &my, &wx, &wy, &mask);
  if (mx >= p->x() && mx < p->x() + p->w() && my >= p->y() && my < p->y() + p->h()) return;
  p->set_revealed(false);
}

// ---------------------------------------------------------------- popups

bool Panel::any_popup_open() const { return open_popup_ && open_popup_->is_open(); }

void Panel::open_popup(PanelPopup *p, int anchor_x, bool align_right) {
  recover_input();
  if (open_popup_ && open_popup_ != p) open_popup_->close();
  // Open for FLTK but not on screen (unmapped by another client): the
  // button opens it again instead of "closing" an invisible popup.
  if (p->is_open() && !p->viewable()) p->close();
  // Open, but the keyboard never reached it (a grab of another client):
  // the button gives it the keyboard instead of closing it.
  if (p->is_open() && !p->has_input() && Fl::grab() == p) {
    p->take_focus();
    XSetInputFocus(fl_display, fl_xid(p), RevertToPointerRoot, CurrentTime);
    return;
  }
  if (p->is_open()) {
    p->close();
    return;
  }
  set_revealed(true);
  p->open_at(anchor_x, align_right);
}

void Panel::popup_opened(PanelPopup *p) {
  open_popup_ = p;
  Fl::remove_timeout(preview_timeout, this);
  preview_pending_ = 0;
  if (preview_ && preview_->showing()) preview_->hide_now();
  if (p == quick_.get() || p == flyout_.get() || p == alarms_.get() ||
      p == status_flyout_.get()) {
    popup_opened_at_ = steady_ms();
    // Presses queued before the popup opened belong to earlier clicks. A
    // popup opened by a command (a client message carries no time) would
    // otherwise keep a stale fl_event_time and let them close it.
    draining_input_ = true;
    input_event(0, this);
    draining_input_ = false;
    popup_opened_time_ = std::max<unsigned long>(fl_event_time, last_press_time_);
    if (p != alarms_.get()) PanelControls::get().set_visible(true);
  }
  relayout();
  redraw();
}

void Panel::popup_closed(PanelPopup *p) {
  if (open_popup_ == p) open_popup_ = nullptr;
  if (p == quick_.get() || p == flyout_.get() || p == status_flyout_.get())
    PanelControls::get().set_visible(false);
  if (p == status_flyout_.get()) TouchpadMonitor::get().want_motion(touchpad_motion_wanted());
  relayout();
  redraw();
  if (autohide_) Fl::add_timeout(0.8, hide_timeout, this);
}

const Fl_Menu_Item *Panel::popup_menu(const Fl_Menu_Item *items, int root_x) {
  // Height of the menu: item heights plus the window border.
  int total = 0;
  for (const Fl_Menu_Item *m = items; m && m->text; m = m->next()) {
    int hh = 0;
    m->measure(&hh, nullptr);
    total += hh + 2;  // item spacing used by FLTK's menu window
  }
  total += S(8);
  int root_y = theme_.panel_top ? y() + h() + 1 : y() - total - 1;
  // Fl_Menu_Item::popup() takes coordinates relative to the window that
  // received the current event.
  int ox = Fl::event_x_root() - Fl::event_x(), oy = Fl::event_y_root() - Fl::event_y();
  return items->popup(root_x - ox, root_y - oy);
}

void Panel::toggle_menu() { open_popup(menu_.get(), x() + S(4), false); }

void Panel::open_search(const std::string &text, bool keep_popup) {
  recover_input();
  if (search_->is_open() && !search_->viewable()) search_->close();
  if (search_->is_open()) {
    if (text.empty()) search_->close();
    else search_->type(text);
    return;
  }
  if (text.empty() && recently_closed("search")) return;
  if (open_popup_ && open_popup_ != search_.get() && !keep_popup) open_popup_->close();
  set_revealed(true);
  // Above the search box (or the Start button when the box is hidden).
  int anchor = x() + (search_w_ ? search_x_ : S(4));
  search_->open_with(text, anchor);
}

bool Panel::search_box_contains(int rx, int ry) const {
  return search_w_ > 0 && rx >= x() + search_x_ && rx < x() + search_x_ + search_w_ &&
         ry >= y() && ry < y() + h();
}

void Panel::search_changed() {
  if (search_w_ > 0) damage_rect(search_x_, 0, search_w_, h());
}
void Panel::toggle_dark_mode() {
  cfg_.set("appearance", "theme", theme_.dark ? "light" : "dark");
  if (cfg_.save()) {
    if (sync_openbox(cfg_)) reconfigure_window_manager();
    x11::notify_config_changed();
  }
}

// ---------------------------------------------------------------- pinning

bool Panel::is_pinned(const std::string &id) const {
  for (auto &p : pinned_)
    if (p.entry.id == id) return true;
  return false;
}

void Panel::save_pins() {
  std::string list;
  for (auto &p : pinned_) list += (list.empty() ? "" : ";") + p.entry.id;
  cfg_.set("panel", "pinned", list);
  cfg_.save();
  x11::notify_config_changed();
}

void Panel::pin(const std::string &id) {
  if (is_pinned(id)) return;
  Pinned p;
  if (!find_application(id, p.entry)) return;
  pinned_.push_back(p);
  save_pins();
  relayout();
  redraw();
}

void Panel::unpin(const std::string &id) {
  pinned_.erase(std::remove_if(pinned_.begin(), pinned_.end(),
                               [&](const Pinned &p) { return p.entry.id == id; }),
                pinned_.end());
  save_pins();
  relayout();
  redraw();
}

void Panel::set_setting(const std::string &section, const std::string &key,
                        const std::string &value) {
  if (value.empty()) cfg_.unset(section, key);
  else cfg_.set(section, key, value);
  cfg_.save();
  x11::notify_config_changed();
}

void Panel::set_settings(const std::vector<Setting> &settings) {
  for (const auto &s : settings) {
    if (s.value.empty())
      cfg_.unset(s.section, s.key);
    else
      cfg_.set(s.section, s.key, s.value);
  }
  cfg_.save();
  x11::notify_config_changed();
}

void Panel::set_do_not_disturb(bool on) {
  cfg_.set("notifications", "do_not_disturb", on ? "true" : "false");
  cfg_.save();
  x11::notify_config_changed();
  quick_->status_changed();
  flyout_->status_changed();
}

bool Panel::find_entry_for(const Task &t, DesktopEntry &out) const {
  std::string cls = lower(t.wm_class), name = lower(t.wm_name);
  if (cls.empty() && name.empty()) return false;
  // Exact desktop file names first ("firefox.desktop", "org.gnome.Nautilus.desktop").
  for (const std::string &n : {name, cls})
    if (!n.empty() && find_application(n + ".desktop", out)) return true;
  for (auto &e : load_applications("SDE")) {
    std::string wm = lower(e.startup_wm_class);
    auto cmd = e.command();
    std::string exec = cmd.empty() ? "" : lower(basename_of(cmd[0]));
    std::string id = lower(e.id);
    if ((!wm.empty() && (wm == cls || wm == name)) || exec == name || exec == cls ||
        ends_with(id, "." + cls + ".desktop")) {
      out = e;
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------ layout

QuickSettings *Panel::tray_host() const {
  if (flyout_ && flyout_->is_open() && flyout_->section() == "tray" &&
      cfg_.get_bool("action_center", "move_status", true))
    return flyout_.get();
  return nullptr;
}
int Panel::status_count() const { return int(status_icons_.size()); }
void Panel::input_event(int, void *d) {
  auto *p = static_cast<Panel *>(d);
  while (p->input_display_ && XPending(p->input_display_)) {
    XEvent event;
    XNextEvent(p->input_display_, &event);
    if (event.type == GenericEvent && event.xcookie.extension == p->input_opcode_ &&
        event.xcookie.evtype == XI_RawButtonPress) {
      unsigned long time = 0;
      int button = 1;
      if (XGetEventData(p->input_display_, &event.xcookie)) {
        auto *raw = static_cast<XIRawEvent *>(event.xcookie.data);
        time = raw->time;
        button = raw->detail;
        XFreeEventData(p->input_display_, &event.xcookie);
      }
      if (time > p->last_press_time_) p->last_press_time_ = time;
      // Scrolling elsewhere (buttons 4-7) leaves popups open.
      if (!p->draining_input_ && (button < 4 || button > 7)) p->outside_pointer_press(time);
    }
  }
}
// Repairs FLTK's input state after another client held the pointer: a grab
// left on a window that is gone, or a button FLTK still thinks is down
// because its release went to the grabbing client.
void Panel::recover_input() {
  if (Fl::grab() && (!Fl::grab()->shown() || !Fl::grab()->visible())) Fl::grab(nullptr);
  if (Fl::pushed()) {
    Window root = DefaultRootWindow(fl_display), r, child;
    int rx, ry, x, y;
    unsigned buttons = 0;
    if (XQueryPointer(fl_display, root, &r, &child, &rx, &ry, &x, &y, &buttons) &&
        !(buttons & (Button1Mask | Button2Mask | Button3Mask)) && Fl::event() != FL_RELEASE &&
        Fl::event() != FL_PUSH && Fl::event() != FL_DRAG)
      Fl::pushed(nullptr);
  }
}
void Panel::outside_pointer_press(unsigned long time) {
  recover_input();
  // The Action Center and the flyouts host native tray windows and do not
  // grab the pointer; a press anywhere else closes them.
  PanelPopup *q = quick_ && quick_->is_open()     ? static_cast<PanelPopup *>(quick_.get())
                  : flyout_ && flyout_->is_open() ? static_cast<PanelPopup *>(flyout_.get())
                  : alarms_ && alarms_->is_open() ? static_cast<PanelPopup *>(alarms_.get())
                  : status_flyout_ && status_flyout_->is_open()
                      ? static_cast<PanelPopup *>(status_flyout_.get())
                      : nullptr;
  if (!q || Fl::grab()) return;
  // XI2 arrives on a separate connection: the press of the click that opened
  // the popup may be dispatched after FLTK's release, even much later on a
  // busy server. Server timestamps tell it apart from a new click.
  if (time && popup_opened_time_ && time <= popup_opened_time_) return;
  if (!time && steady_ms() - popup_opened_at_ < 100) return;
  Window root = DefaultRootWindow(fl_display), r, child;
  int rx, ry, x, y;
  unsigned buttons;
  if (!XQueryPointer(fl_display, root, &r, &child, &rx, &ry, &x, &y, &buttons)) return;
  if (q->contains_root(rx, ry)) return;
  closed_key_ = popup_key(q);
  closed_at_ = steady_ms();
  q->close();
}
bool Panel::recently_closed(const std::string &key) const {
  return key == closed_key_ && steady_ms() - closed_at_ < 300;
}
// Identifies what a popup shows, so that its own button toggles it and any
// other button switches to another popup.
std::string Panel::popup_key(const PanelPopup *p) const {
  if (!p) return "";
  if (p == menu_.get()) return "menu";
  if (p == search_.get()) return "search";
  if (p == quick_.get()) return quick_->calendar_view() ? "calendar" : "center";
  if (p == flyout_.get()) return "flyout:" + flyout_->section();
  if (p == status_flyout_.get()) return "flyout:" + status_flyout_->section();
  if (p == alarms_.get()) return "alarms";
  return "";
}
std::string Panel::target_key(const Target &t) const {
  switch (t.kind) {
    case START:
      return "menu";
    case SEARCH:
      return "search";
    case CLOCK:
      return "alarms";
    case ACTION_CENTER:
      return "center";
    case STATUS:
      if (t.index >= 0 && t.index < int(status_icons_.size())) {
        StatusIcon icon = status_icons_[size_t(t.index)];
        if (icon == CONTROLS_STATUS) return "center";
        std::string section = status_section(icon);
        if (!section.empty()) return "flyout:" + section;
      }
      return "";
    default:
      return "";
  }
}
void Panel::open_action_center(bool calendar) {
  std::string key = calendar ? "calendar" : "center";
  if (quick_->is_open()) {
    // The clock while the Action Center is open from its button (or the
    // other way round) switches the view; the same button closes it.
    bool same = popup_key(quick_.get()) == key;
    quick_->close();
    if (same) return;
  } else if (recently_closed(key)) {
    return;
  }
  quick_->set_calendar_view(calendar);
  open_popup(quick_.get(), x() + w(), true);
}
std::string Panel::status_section(StatusIcon icon) {
  switch (icon) {
    case INPUT_STATUS:
      return "keyboard";
    case BLUETOOTH_STATUS:
      return "bluetooth";
    case BATTERY_STATUS:
      return "power";
    case MEDIA_STATUS:
      return "media";
    case NETWORK_STATUS:
      return "network";
    case SOUND_STATUS:
      return "sound";
    case OVERFLOW_STATUS:
      return "tray";
    case DRIVE_STATUS:
      return "drives";
    case TOUCHPAD_STATUS:
      return "touchpad";
    default:
      return "";
  }
}
bool Panel::status_anchor(const std::string &section, int &center_x) const {
  for (size_t i = 0; i < status_icons_.size(); ++i)
    if (status_section(status_icons_[i]) == section) {
      center_x = x() + status_x_ + int(i) * S(26) + S(13);
      return true;
    }
  return false;
}
void Panel::open_flyout(const std::string &section) {
  // Media controls live in the Action Center (alpha50).
  if (section == "media") {
    open_action_center();
    return;
  }
  if (StatusFlyout::handles(section)) {
    open_status_flyout(section);
    return;
  }
  if (!QuickSettings::valid_section(section)) return;
  std::string key = "flyout:" + section;
  if (flyout_->is_open()) {
    bool same = flyout_->section() == section;
    flyout_->close();
    if (same) return;
  } else if (recently_closed(key)) {
    return;
  }
  if (open_popup_ && open_popup_ != flyout_.get()) open_popup_->close();
  set_revealed(true);
  flyout_->select_section(section);
  int center = 0;
  if (status_anchor(section, center))
    flyout_->open_centered(center);
  else  // The icon is hidden (short panel): next to the Action Center button.
    flyout_->open_at(x() + action_x_ + action_w_, true);
}
void Panel::open_status_flyout(const std::string &section) {
  std::string key = "flyout:" + section;
  if (status_flyout_->is_open()) {
    bool same = status_flyout_->section() == section;
    status_flyout_->close();
    if (same) return;
  } else if (recently_closed(key)) {
    return;
  }
  if (open_popup_ && open_popup_ != status_flyout_.get()) open_popup_->close();
  set_revealed(true);
  status_flyout_->select_section(section);
  int center = 0;
  if (status_anchor(section, center))
    status_flyout_->open_centered(center);
  else
    status_flyout_->open_at(x() + action_x_ + action_w_, true);
}
void Panel::open_alarms(bool add) {
  if (alarms_->is_open()) {
    alarms_->close();
    if (!add) return;
  } else if (!add && recently_closed("alarms")) {
    return;
  }
  if (open_popup_ && open_popup_ != alarms_.get()) open_popup_->close();
  set_revealed(true);
  if (add) alarms_->start_new();
  alarms_->open_centered(x() + clock_x_ + clock_w_ / 2);
}
bool Panel::switch_popup(int root_x, int root_y, PanelPopup *from) {
  if (!shown() || root_x < x() || root_x >= x() + w() || root_y < y() || root_y >= y() + h())
    return false;
  Target t = hit(root_x - x(), root_y - y());
  std::string key = target_key(t);
  // The button that opened this popup only closes it (toggle).
  if (key.empty() || key == popup_key(from)) return false;
  struct Pending {
    std::string key;
  };
  auto *pending = new Pending{key};
  Fl::add_timeout(0, [](void *d) {
    std::unique_ptr<Pending> p(static_cast<Pending *>(d));
    Panel *self = Panel::instance;
    if (p->key == "menu")
      self->toggle_menu();
    else if (p->key == "search")
      self->open_search();
    else if (p->key == "calendar")
      self->open_action_center(true);
    else if (p->key == "alarms")
      self->open_alarms();
    else if (p->key == "center")
      self->open_action_center();
    else if (starts_with(p->key, "flyout:"))
      self->open_flyout(p->key.substr(7));
  }, pending);
  return true;
}

void Panel::status_clicked(int index, int button) {
  if (index < 0 || index >= int(status_icons_.size())) return;
  switch (status_icons_[size_t(index)]) {
    case POWER_STATUS:
      spawn_detached({"sde-logout"});
      break;
    case SOUND_STATUS:
      if (button == FL_MIDDLE_MOUSE)
        SystemStatus::get().toggle_mute();
      else
        open_flyout("sound");
      break;
    case CONTROLS_STATUS:
    case MEDIA_STATUS:
      open_action_center();
      break;
    case INPUT_STATUS:
    case BLUETOOTH_STATUS:
    case BATTERY_STATUS:
    case NETWORK_STATUS:
    case TOUCHPAD_STATUS:
    case OVERFLOW_STATUS:
    case DRIVE_STATUS:
      open_flyout(status_section(status_icons_[size_t(index)]));
      break;
    case DND_STATUS:
      set_do_not_disturb(false);
      break;
  }
}

// Tooltip of a tray status icon: what it is and its current state.
std::string Panel::status_tooltip(StatusIcon icon) const {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  switch (icon) {
    case INPUT_STATUS:
      return std::string(_("Keyboard layout")) + " · " + c.keyboard;
    case BLUETOOTH_STATUS: {
      const auto *a = c.adapter();
      if (!a || !a->powered) return _("Bluetooth is off");
      for (const auto &d : c.bluetooth)
        if (d.connected) return std::string(_("Bluetooth")) + " · " + d.name;
      return _("Bluetooth is on");
    }
    case BATTERY_STATUS:
      return format(s.battery.charging ? _("Battery %d%% · charging") : _("Battery %d%%"),
                    s.battery.percent);
    case POWER_STATUS:
      return _("Shut down or log out");
    case MEDIA_STATUS:
      return _("Media players");
    case NETWORK_STATUS: {
      std::string state = s.network.no_internet() ? std::string(" · ") + _("No internet access")
                          : s.network.portal()    ? std::string(" · ") + _("Sign in to network")
                          : s.network.connectivity == NetworkState::FULL
                              ? std::string(" · ") + _("Internet access")
                              : "";
      if (s.network.kind == "wifi")
        return std::string(_("Wi-Fi")) + " · " + s.network.connection + state;
      if (s.network.kind == "ethernet")
        return std::string(_("Wired network")) + " · " + s.network.connection + state;
      return _("Not connected");
    }
    case SOUND_STATUS:
      return s.audio.muted ? std::string(_("Sound is muted"))
                           : format(_("Volume %d%%"), s.audio.volume);
    case CONTROLS_STATUS:
      return _("Action Center");
    case OVERFLOW_STATUS:
      return _("Show hidden icons");
    case DRIVE_STATUS:
      return c.drives.size() == 1 ? std::string(_("Drive plugged in")) + " · " + c.drives.front().label
                                  : format(_("%d drives plugged in"), int(c.drives.size()));
    case DND_STATUS:
      return _("Do Not Disturb is on · click to turn it off");
    case TOUCHPAD_STATUS:
      return TouchpadMonitor::get().enabled() ? _("Touchpad · on") : _("Touchpad is off");
  }
  return "";
}

std::string Panel::tooltip_for(const Target &t) const {
  if (t.kind == TRAY_ITEM && sni_) {
    auto items = sni_->visible_items();
    if (t.index >= 0 && t.index < int(items.size()))
      return SniHost::display_name(*items[size_t(t.index)]);
  } else if (t.kind == STATUS && t.index >= 0 && t.index < int(status_icons_.size())) {
    return status_tooltip(status_icons_[size_t(t.index)]);
  } else if (t.kind == SEARCH) {
    return _("Search apps, settings and files");
  } else if (t.kind == ACTION_CENTER) {
    return _("Action Center · Super+A");
  } else if (t.kind == CLOCK) {
    return _("Clock and alarms · Super+C opens the calendar");
  } else if (t.kind == SHOW_DESKTOP) {
    return _("Show Desktop · Super+D");
  } else if (t.kind == START) {
    return _("Start · Super");
  } else if (t.kind == TASK && t.index >= 0 && t.index < int(tasks_.size())) {
    // The window preview names the window (alpha51).
    if (previews_enabled()) return "";
    // A narrow task button shows only the icon: its title as tooltip.
    const Task &task = tasks_[size_t(t.index)];
    if (task.w < h() + S(20)) return printable_text(task.title.empty() ? task.wm_class : task.title);
  } else if (t.kind == PINNED && t.index >= 0 && t.index < int(pinned_.size())) {
    return pinned_[size_t(t.index)].entry.name;
  }
  return "";
}
bool Panel::target_rect(const Target &t, int &x, int &w) const {
  switch (t.kind) {
    case START:
      x = 0, w = start_w_;
      return true;
    case SEARCH:
      x = search_x_, w = search_w_;
      return true;
    case PINNED:
      if (t.index < 0 || t.index >= int(pinned_.size())) return false;
      x = pinned_[size_t(t.index)].x, w = pin_w_;
      return true;
    case TASK:
      if (t.index < 0 || t.index >= int(tasks_.size())) return false;
      x = tasks_[size_t(t.index)].x, w = tasks_[size_t(t.index)].w;
      return true;
    case TRAY_ITEM:
      x = sni_x_ + t.index * (tray_icon_ + S(10)), w = tray_icon_ + S(10);
      return true;
    case STATUS:
      x = status_x_ + t.index * S(26), w = S(26);
      return true;
    case CLOCK:
      x = clock_x_, w = clock_w_;
      return true;
    case ACTION_CENTER:
      x = action_x_, w = action_w_;
      return true;
    case SHOW_DESKTOP:
      x = show_desktop_x_, w = show_desktop_w_;
      return true;
    default:
      return false;
  }
}
// The text is stored in tip_ (never a temporary or a string that the next
// hover frees) and Fl_Tooltip is told about each part separately; see
// tooltip_text.h for the alpha16 bug this replaces.
void Panel::update_tooltip() {
  std::string tip = tooltip_for(hover_);
  int tx = 0, tw = w();
  if (tip.empty() || !target_rect(hover_, tx, tw)) {
    if (!tip_.current().empty()) {
      tip_.set("");
      if (Fl_Tooltip::current() == this) Fl_Tooltip::enter_area(this, 0, 0, w(), h(), nullptr);
    }
    return;
  }
  if (tip == tip_.current() && Fl_Tooltip::current() == this) return;
  Fl_Tooltip::enter_area(this, tx, 0, tw, h(), tip_.set(tip));
}
const char *Panel::qa_tooltip() const { return tip_.c_str(); }

// ------------------------------------------------------ LED and touchpad

// IBus or Fcitx manage the keyboard (engines and their layouts) and reset
// the XKB layout when their engine changes, so SDE's layout indicator would
// show a stale "US" next to the input method's own "EN". With an input
// method in the tray, its icon is the only keyboard indicator.
// [panel] keyboard_indicator=sde keeps SDE's indicator anyway.
bool Panel::input_method_indicator() const {
  if (cfg_.get("panel", "keyboard_indicator", "auto") == "sde") return false;
  return (sni_ && sni_->has_input_method()) || (tray_ && tray_->has_input_method());
}

int Panel::status_index(StatusIcon icon) const {
  for (size_t i = 0; i < status_icons_.size(); ++i)
    if (status_icons_[i] == icon) return int(i);
  return -1;
}
void Panel::status_icon_changed(const std::string &section) {
  if (!shown() || (autohide_ && !revealed_)) return;
  for (size_t i = 0; i < status_icons_.size(); ++i)
    if (status_section(status_icons_[i]) == section)
      damage_rect(status_x_ + int(i) * S(26), 0, S(26), h());
}
void Panel::led_update() {
  const auto &n = SystemStatus::get().network;
  bool shown_led = cfg_.get_bool("panel", "network_led", true) && status_index(NETWORK_STATUS) >= 0;
  auto &controls = PanelControls::get();
  // A new connection: ask debion-network-check (proxy, DNS, clock...).
  std::string key = n.connected() ? n.kind + "|" + n.connection : "";
  if (key != led_connection_) {
    led_connection_ = key;
    controls.net_verdict.clear();
    if (!key.empty()) controls.check_network();
  }
  bool checker = controls.network_checker();
  // NetworkManager does not check connectivity: a quick probe of our own,
  // at most once a minute, tells local-only networks apart.
  if (shown_led && !checker && n.connected() && n.connectivity == NetworkState::UNKNOWN &&
      !led_probe_pending_ && steady_ms() - led_probe_at_ > 60000) {
    led_probe_at_ = steady_ms();
    std::string uri = n.check_uri.empty() ? "http://nmcheck.gnome.org/check_network_status.txt"
                                          : n.check_uri;
    std::vector<std::string> probe;
    if (!find_program("curl").empty())
      probe = {"curl", "-s", "-o", "/dev/null", "-m", "4", "-w", "%{http_code}", uri};
    else if (!find_program("wget").empty())
      probe = {"wget", "-q", "-T", "4", "-t", "1", "-O", "/dev/null", uri};
    if (!probe.empty()) {
      led_probe_pending_ = true;
      bool curl = probe[0] == "curl";
      run_async(probe, [this, curl](const std::string &out, int code) {
        led_probe_pending_ = false;
        std::string status = trim(out);
        bool ok = code == 0 && (!curl || (!status.empty() && status[0] == '2'));
        if (ok == !led_probe_failed_) return;
        led_probe_failed_ = !ok;
        status_icon_changed("network");
      });
    }
  } else if (n.connectivity != NetworkState::UNKNOWN || !n.connected()) {
    led_probe_failed_ = false;
  }
  // Steady instead of pulsing on battery saver or with network_led_blink=false.
  bool blink = shown_led && cfg_.get_bool("panel", "network_led_blink", true) &&
               PanelControls::get().power_profile != "power-saver";
  if (blink && !led_running_) {
    led_running_ = true;
    Fl::add_timeout(0.125, led_tick, this);
  } else if (!blink && led_running_) {
    led_running_ = false;
    Fl::remove_timeout(led_tick, this);
    status_icon_changed("network");
  }
}
// A 2 s soft pulse at 8 frames per second; only the icon's rectangle is
// repainted.
void Panel::led_tick(void *data) {
  auto *p = static_cast<Panel *>(data);
  if (!p->led_running_) return;
  p->led_phase_ = double(steady_ms() % 2000) / 2000.0;
  p->status_icon_changed("network");
  Fl::repeat_timeout(0.125, led_tick, data);
}
bool Panel::touchpad_motion_wanted() const {
  if (!TouchpadMonitor::get().present()) return false;
  bool flyout =
      status_flyout_ && status_flyout_->is_open() && status_flyout_->section() == "touchpad";
  std::string mode = cfg_.get("panel", "touchpad_indicator", "auto");
  if (mode == "never") return flyout;
  if (mode == "always") return true;
  int i = status_index(TOUCHPAD_STATUS);
  bool hovered = i >= 0 && hover_.kind == STATUS && hover_.index == i;
  bool recent = steady_ms() - TouchpadMonitor::get().state().click_at < 2000;
  return hovered || flyout || recent;
}
void Panel::touchpad_activity() {
  if (touchpad_tick_pending_) return;
  touchpad_tick_pending_ = true;
  Fl::add_timeout(1.0 / 30, touchpad_tick, this);
}
void Panel::touchpad_tick(void *data) {
  auto *p = static_cast<Panel *>(data);
  p->touchpad_tick_pending_ = false;
  p->status_icon_changed("touchpad");
  if (p->status_flyout_->is_open() && p->status_flyout_->section() == "touchpad")
    p->status_flyout_->redraw();
  auto &tp = TouchpadMonitor::get();
  tp.want_motion(p->touchpad_motion_wanted());
  // Keep animating while the dot fades out and the flash ends.
  if (steady_ms() - tp.last_activity() < 2100) {
    p->touchpad_tick_pending_ = true;
    Fl::add_timeout(1.0 / 20, touchpad_tick, data);
  }
}

// What the panel itself draws from the status services; an update that does
// not change it skips the panel redraw.
std::string Panel::status_signature() const {
  const auto &s = SystemStatus::get();
  const auto &c = PanelControls::get();
  std::string sig = std::to_string(s.network.available) + s.network.kind + s.network.connection +
                    std::to_string(s.network.signal / 25) + std::to_string(s.network.connectivity) +
                    std::to_string(s.network.has_wifi) + std::to_string(s.network.wifi_enabled) +
                    std::to_string(s.audio.available) + std::to_string(s.audio.volume) +
                    std::to_string(s.audio.muted) + std::to_string(s.battery.present) +
                    std::to_string(s.battery.percent) + std::to_string(s.battery.charging) +
                    c.keyboard + std::to_string(c.drives.size()) + std::to_string(c.adapters.size()) + c.net_verdict;
  if (const auto *a = c.adapter()) sig += std::to_string(a->powered);
  for (const auto &d : c.bluetooth) sig += std::to_string(d.connected);
  auto players = c.players();
  sig += std::to_string(players.size()) + (players.empty() ? "" : players.front().status);
  if (sni_)
    for (auto *i : sni_->visible_items()) sig += "|" + i->service + i->path + i->status + i->title;
  if (tray_) sig += "|" + std::to_string(tray_->count());
  sig += "|tp" + std::to_string(TouchpadMonitor::get().present()) +
         std::to_string(TouchpadMonitor::get().enabled());
  return sig;
}
void Panel::status_update() {
  if (status_update_pending_) return;
  status_update_pending_ = true;
  Fl::add_timeout(0.05, status_update_timeout, this);
}
void Panel::status_update_timeout(void *data) {
  auto *p = static_cast<Panel *>(data);
  p->status_update_pending_ = false;
  std::string sig = p->status_signature();
  if (sig != p->status_signature_) {
    p->status_signature_ = sig;
    p->relayout();
    p->redraw();
    // A Title or state that arrives while the pointer rests on an icon.
    if (p->hover_.kind == TRAY_ITEM || p->hover_.kind == STATUS) p->update_tooltip();
  }
  p->quick_->status_changed();
  p->flyout_->status_changed();
  p->status_flyout_->status_changed();
  p->led_update();
}

void Panel::relayout() {
  int H = theme_.panel_height;
  icon_ = H >= S(48) ? S(28) : H >= S(40) ? S(24) : S(20);
  tray_icon_ = H >= S(40) ? S(20) : S(18);
  start_w_ = H + S(46);
  pin_w_ = H;

  // The Show Desktop strip sits at the very end of the panel (an easy
  // target in the screen corner, as in Linux Mint and Windows); the Action
  // Center button comes just before it.
  int right = w();
  show_desktop_w_ = cfg_.get_bool("panel", "show_desktop_button", true) ? S(12) : 0;
  show_desktop_x_ = right - show_desktop_w_;
  right = show_desktop_x_;
  action_w_ = H >= S(40) ? S(40) : S(34);
  action_x_ = right - action_w_ - S(4);
  right = action_x_ - S(2);
  text_font(theme_.font_size, false);
  bool show_date = cfg_.get_bool("panel", "show_date", true);
  bool two_lines = H >= S(40) && show_date;
  std::string sample = cfg_.get_bool("panel", "clock_24h", true) ? "88:88" : "88:88 PM";
  int cw = text_width(sample);
  if (show_date) {
    int dw = text_width(clock_date_.empty() ? "Wed 30 Sep" : clock_date_);
    cw = two_lines ? std::max(cw, dw) : cw + dw + S(10);
  }
  clock_w_ = cw + S(24);
  clock_x_ = right - clock_w_;
  right = clock_x_ - S(4);
  status_icons_.clear();
  const auto &s = SystemStatus::get();
  const auto &controls = PanelControls::get();
  auto add = [&](StatusIcon icon, const char *key, bool available = true) {
    if (available && cfg_.get_bool("panel", key, true)) status_icons_.push_back(icon);
  };
  // Icons appear only while their hardware or source exists: Bluetooth
  // when an adapter is present, drives when one is plugged in, battery on
  // laptops.
  // Application tray icons sit directly on the panel while they are few and
  // fit; more than [panel] tray_overflow_threshold (default 4) of them gather
  // behind the overflow (^) button and its Tray flyout. tray_overflow=always
  // or never overrides the automatic choice.
  int sn = sni_ ? int(sni_->visible_items().size()) : 0, tw = tray_ ? tray_->width() : 0;
  int apps = sn + (tray_ ? tray_->count() : 0);
  std::string overflow_mode = cfg_.get("panel", "tray_overflow", "auto");
  int threshold = std::max(0, cfg_.get_int("panel", "tray_overflow_threshold", 4));
  {
    int others = 0;  // the status icons other than the overflow button
    for (const char *key : {"show_drives", "show_bluetooth", "show_keyboard", "show_network",
                            "show_sound", "show_battery", "show_touchpad"})
      others += cfg_.get_bool("panel", key, true) ? 1 : 0;
    bool fits = right - start_w_ - S(100) >= (others + 1) * S(26) + sn * (tray_icon_ + S(10)) + tw;
    tray_overflow_ = apps > 0 && cfg_.get_bool("panel", "show_tray", true) &&
                     (overflow_mode == "always" ||
                      (overflow_mode != "never" && (apps > threshold || !fits)));
  }
  if (tray_overflow_) status_icons_.push_back(OVERFLOW_STATUS);
  add(DRIVE_STATUS, "show_drives", !controls.drives.empty());
  add(BLUETOOTH_STATUS, "show_bluetooth", !controls.adapters.empty());
  // alpha50: media players are controlled from the Action Center only; the
  // panel shows no media icon while music plays (MEDIA_STATUS is unused).
  // One keyboard indicator: an input method's own tray icon (IBus, Fcitx)
  // replaces SDE's layout indicator; plain XKB applets are hidden instead
  // (SniHost/Tray set_hide_layout_applets).
  add(INPUT_STATUS, "show_keyboard", !input_method_indicator());
  // [panel] touchpad_indicator: auto (shown when a touchpad exists, animated
  // while hovered and briefly after use), always (follows every movement)
  // or never.
  add(TOUCHPAD_STATUS, "show_touchpad",
      cfg_.get("panel", "touchpad_indicator", "auto") != "never" &&
          TouchpadMonitor::get().present());
  add(DND_STATUS, "show_do_not_disturb", cfg_.get_bool("notifications", "do_not_disturb", false));
  add(NETWORK_STATUS, "show_network", s.network.available);
  add(SOUND_STATUS, "show_sound", s.audio.available);
  add(BATTERY_STATUS, "show_battery", s.battery.present);
  add(POWER_STATUS, "show_power", cfg_.get_bool("panel", "show_power", false));
  // CONTROLS_STATUS repeated the Action Center button next to the clock.
  // Behind the overflow button, the StatusNotifier icons leave the panel; the
  // XEmbed icons too, unless [action_center] move_status=false keeps them.
  bool keep_xembed = !tray_overflow_ || !cfg_.get_bool("action_center", "move_status", true);
  if (tray_overflow_) sn = 0;
  if (!keep_xembed) tw = 0;
  compact_status_ = right - start_w_ - S(100) <
                    int(status_icons_.size()) * S(26) + sn * (tray_icon_ + S(10)) + tw;
  if (compact_status_) status_icons_.clear();
  // While the Tray flyout holds the tray icons they leave the panel; the
  // status icons stay in place so the flyout stays above its button.
  bool moved = tray_host() != nullptr;
  status_w_ = int(status_icons_.size()) * S(26);
  status_x_ = right - status_w_;
  right = status_x_;
  sni_w_ = compact_status_ || moved ? 0 : sn * (tray_icon_ + S(10));
  sni_x_ = right - sni_w_;
  right = sni_x_;
  tw = compact_status_ || moved ? 0 : tw;
  tray_x_ = right - tw;
  if (tray_ && !moved) {
    if (compact_status_ || !keep_xembed)
      tray_->park();
    else {
      tray_->set_background(theme_.surface.fl() >> 8);
      tray_->place(tray_x_, H);
    }
  }
  tasks_end_ = tray_x_ - S(8);

  // Search next to the Start button: [panel] search=box, icon or hidden;
  // auto (default) is a box on a full-width panel and an icon on the short
  // and floating ones. A box that leaves too little room becomes an icon.
  {
    std::string mode = cfg_.get("panel", "search", "auto");
    if (mode != "box" && mode != "icon" && mode != "hidden")
      mode = cfg_.get("panel", "style", "full") == "full" ? "box" : "icon";
    search_mode_ = mode == "hidden" ? 0 : mode == "icon" ? 1 : 2;
    int room = tasks_end_ - start_w_;
    search_x_ = start_w_ + S(2);
    search_w_ = 0;
    if (search_mode_ == 2 && room < S(520)) search_mode_ = 1;
    if (search_mode_ == 2)
      search_w_ = std::max(S(170), std::min(S(320), room / 4));
    else if (search_mode_ == 1)
      search_w_ = H;
  }
  int lead = search_w_ ? search_x_ + search_w_ : start_w_;

  // Keep the status area and running windows reachable on short panels.
  int room = std::max(0, tasks_end_ - lead - S(12));
  int task_room = tasks_.empty() ? 0 : std::min(2 * H, room / 2);
  visible_pins_ = std::min(int(pinned_.size()), std::max(0, (room - task_room) / pin_w_));
  int pin_x = lead + S(4);
  for (size_t i = 0; i < pinned_.size(); ++i) {
    pinned_[i].x = int(i) < visible_pins_ ? pin_x : -100000;
    if (int(i) < visible_pins_) pin_x += pin_w_;
  }
  tasks_x_ = pin_x + (visible_pins_ ? S(10) : 0);

  int count = int(tasks_.size());
  if (count) {
    int avail = std::max(0, tasks_end_ - tasks_x_);
    int each = std::min(S(220), avail / count);
    each = std::max(each, std::min(H, avail / std::max(1, count)));
    int tx = tasks_x_;
    for (auto &t : tasks_) {
      t.x = tx;
      t.w = std::max(0, each - S(4));
      tx += each;
    }
  }
}

Panel::Target Panel::hit(int x, int y) const {
  Target t;
  if (y < 0 || y >= h()) return t;
  if (x < start_w_) return {START, 0};
  if (search_w_ && x >= search_x_ && x < search_x_ + search_w_) return {SEARCH, 0};
  for (size_t i = 0; i < pinned_.size(); ++i)
    if (x >= pinned_[i].x && x < pinned_[i].x + pin_w_) return {PINNED, int(i)};
  for (size_t i = 0; i < tasks_.size(); ++i)
    if (x >= tasks_[i].x && x < tasks_[i].x + tasks_[i].w) return {TASK, int(i)};
  if (sni_w_ && x >= sni_x_ && x < sni_x_ + sni_w_) {
    int i = (x - sni_x_) / (tray_icon_ + S(10));
    return {TRAY_ITEM, i};
  }
  if (status_w_ && x >= status_x_ && x < status_x_ + status_w_)
    return {STATUS, std::max(0, (x - status_x_) / S(26))};
  if (x >= clock_x_ && x < clock_x_ + clock_w_) return {CLOCK, 0};
  if (x >= action_x_ && x < action_x_ + action_w_) return {ACTION_CENTER, 0};
  if (show_desktop_w_ && x >= show_desktop_x_ && x < show_desktop_x_ + show_desktop_w_)
    return {SHOW_DESKTOP, 0};
  return t;
}

// ------------------------------------------------------------------ drawing

void Panel::draw() {
  const Theme &t = theme_;
  int W = w(), H = h();
  if (autohide_ && !revealed_) {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.accent.alpha(0.6));
    return;
  }
  int icon = icon_;
  double edge = t.panel_top ? H - 0.5 : 0.5;
  {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface);
    // SDE Aero: a soft glass sheen; the compositor supplies blur and opacity.
    bool glass = t.glass;
    if (glass) {
      p.fill_gradient(0, 0, W, H, 0, t.surface.mix(t.accent, t.dark ? 0.10 : 0.05),
                      t.surface.mix(t.dark ? Color::hex(0x000000) : Color::hex(0xFFFFFF), 0.12));
      p.glass_sheen(0, 0, W, H, 0, t.dark);
    }
    if (cfg_.get("panel", "style", "full") == "full")
      p.line(0, edge, W, edge, t.border);
    else
      p.stroke_rounded(0, 0, W, H, std::min(S(16), H / 2), t.border);

    auto button_bg = [&](int x, int bw, Target target, bool active) {
      bool hov = hover_ == target, down = pressed_ == target;
      Color c = active ? t.accent.alpha(t.dark ? 0.26 : 0.14) : t.hover;
      if (down) c = t.accent.alpha(0.3);
      if (active || hov || down)
        p.fill_rounded(x + S(2), S(4), bw - S(4), H - S(8),
                       (cfg_.get("panel", "style", "full") == "full" ? 0 : S(8)), c);
    };

    // Named launcher and grouped status controls follow the selected panel shape.
    {
      double r = cfg_.get("panel", "style", "full") == "full" ? S(6) : S(10);
      Color base = hover_.kind == START ? t.accent.mix(t.text, 0.12) : t.accent;
      if (glass) {
        p.fill_gradient(S(6), S(5), start_w_ - S(8), H - S(10), r,
                        base.mix(Color::hex(0xFFFFFF), 0.18), base.mix(Color::hex(0x000000), 0.12));
        p.glass_sheen(S(6), S(5), start_w_ - S(8), H - S(10), r, true);
      } else {
        p.fill_rounded(S(6), S(5), start_w_ - S(8), H - S(10), r, base);
      }
    }
    p.image(IconLoader::get({"sde-logo", "start-here", "distributor-logo"}, icon), S(14),
            (H - icon) / 2.0, icon);
    bool searching = search_ && search_->is_open();
    if (search_box_shown()) {
      int bh = std::min(H - S(10), S(34));
      panel_search::draw_field_back(p, t, search_x_ + S(4), (H - bh) / 2, search_w_ - S(8), bh,
                                    searching, hover_.kind == SEARCH,
                                    cfg_.get("panel", "style", "full") == "full" ? S(4) : bh / 2.0);
    } else if (search_w_) {
      button_bg(search_x_, search_w_, {SEARCH, 0}, searching);
      double g = S(20);
      panel_search::glyph(p, search_x_ + (search_w_ - g) / 2.0, (H - g) / 2.0, g, t.text);
    }

    for (size_t i = 0; i < pinned_.size(); ++i) {
      if (int(i) >= visible_pins_) break;
      bool dragged = dragging_ && int(i) == drag_index_;
      if (dragged) continue;
      button_bg(pinned_[i].x, pin_w_, {PINNED, int(i)}, false);
      p.image(IconLoader::get({pinned_[i].entry.icon, "application-x-executable"}, icon),
              pinned_[i].x + (pin_w_ - icon) / 2.0, (H - icon) / 2.0, icon);
    }
    if (visible_pins_) p.line(tasks_x_ - S(6), S(10), tasks_x_ - S(6), H - S(10), t.border);

    for (size_t i = 0; i < tasks_.size(); ++i) {
      Task &task = tasks_[i];
      if (task.w <= 0) continue;
      cairo_save(p.cr());
      cairo_rectangle(p.cr(), task.x, 0, task.w, H);
      cairo_clip(p.cr());
      bool active = task.win == active_ && !task.minimized;
      button_bg(task.x, task.w, {TASK, int(i)}, active);
      if (task.urgent)
        p.fill_rounded(task.x + S(2), S(4), task.w - S(4), H - S(8), S(8),
                       Color::hex(0xE8A33D, 0.35));
      cairo_surface_t *img =
          task.icon ? task.icon : IconLoader::get({"application-x-executable"}, icon);
      int ix = task.w < H + S(20) ? task.x + (task.w - icon) / 2 : task.x + S(10);
      p.image(img, ix, (H - icon) / 2.0, icon, task.minimized ? 0.55 : 1.0);
      // Indicator bar on the screen edge side: wide for the active window.
      double bar = active ? std::max(S(2), std::min(S(28), task.w - S(16))) : S(8);
      double by = t.panel_top ? Sd(2) : H - Sd(5);
      p.fill_rounded(task.x + (task.w - bar) / 2.0, by, bar, Sd(3), Sd(1.5),
                     active ? t.accent : t.text_dim.alpha(0.6));
      cairo_restore(p.cr());
    }

    // Application tray indicators keep their native icons; system controls
    // use a consistent monochrome Cairo set, independent of the GTK theme.
    if (sni_ && sni_w_ > 0) {
      auto items = sni_->visible_items();
      for (size_t i = 0; i < items.size(); ++i) {
        int bx = sni_x_ + int(i) * (tray_icon_ + S(10));
        button_bg(bx, tray_icon_ + S(10), {TRAY_ITEM, int(i)}, false);
        p.image(sni_->icon(*items[i], tray_icon_), bx + S(5), (H - tray_icon_) / 2., tray_icon_);
      }
    }
    const auto &system = SystemStatus::get();
    const auto &controls = PanelControls::get();
    double gs = S(18), gy = (H - gs) / 2.;
    for (size_t i = 0; i < status_icons_.size(); ++i) {
      int bx = status_x_ + int(i) * S(26);
      double gx = bx + S(4);
      std::string section = status_section(status_icons_[i]);
      button_bg(bx, S(26), {STATUS, int(i)},
                (flyout_ && flyout_->is_open() && flyout_->section() == section) ||
                    (status_flyout_ && status_flyout_->is_open() &&
                     status_flyout_->section() == section));
      switch (status_icons_[i]) {
        case INPUT_STATUS:
          tray_icon::layout(p, gx, gy, gs, controls.keyboard, t.text);
          break;
        case BLUETOOTH_STATUS: {
          auto *a = controls.adapter();
          bool connected = false;
          for (const auto &d : controls.bluetooth) connected = connected || d.connected;
          tray_icon::bluetooth(p, gx, gy, gs, a && a->powered, connected, t.text);
          break;
        }
        case BATTERY_STATUS:
          tray_icon::battery(p, gx, gy, gs, system.battery.percent, system.battery.charging,
                             t.text, Color::hex(0xD64545));
          break;
        case POWER_STATUS:
          tray_icon::power(p, gx, gy, gs, t.text);
          break;
        case MEDIA_STATUS: {
          auto players = controls.players();
          tray_icon::media(p, gx, gy, gs,
                           !players.empty() && players.front().status == "Playing", t.text);
          break;
        }
        case NETWORK_STATUS: {
          const auto &n = system.network;
          if (n.kind == "ethernet")
            tray_icon::wired(p, gx, gy, gs, true, t.text);
          else if (n.kind == "wifi")
            tray_icon::wifi(p, gx, gy, gs, std::max(1, (n.signal + 24) / 25), true, t.text);
          else if (n.has_wifi && n.wifi_enabled)
            tray_icon::wifi(p, gx, gy, gs, 0, false, t.text);
          else
            tray_icon::offline(p, gx, gy, gs, t.text);
          if (cfg_.get_bool("panel", "network_led", true)) {
            // The LED: red offline, yellow local network only, green
            // internet; pulsing gently (led_tick) unless steady.
            NetLed led = network_led(n.connected(), n.connectivity, led_probe_failed_,
                                     controls.net_verdict);
            Color c = led == NetLed::GREEN    ? Color::hex(0x22C55E)
                      : led == NetLed::YELLOW ? Color::hex(0xEAB308)
                                              : Color::hex(0xEF4444);
            double pulse = led_running_ ? 0.5 + 0.5 * std::cos(2 * M_PI * led_phase_) : 1.0;
            double r = gs * 0.17, lx = gx + gs - r * 0.6, ly = gy + gs - r * 0.6;
            p.circle(lx, ly, r + Sd(1.2), t.surface);
            p.circle(lx, ly, r * (1.6 + 0.6 * pulse), c.alpha(0.18 * pulse));
            p.circle(lx, ly, r, c.alpha(0.55 + 0.45 * pulse));
          } else if (n.no_internet() || n.portal()) {
            // Connected without internet (or behind a sign-in page): a small
            // warning badge, so the icon never suggests a working connection.
            double r = gs * 0.24, bx2 = gx + gs - r * 0.9, by2 = gy + gs - r * 0.9;
            p.circle(bx2, by2, r + Sd(1), t.surface);
            p.circle(bx2, by2, r, n.portal() ? t.accent : Color::hex(0xE8A33D));
            p.line(bx2, by2 - r * 0.55, bx2, by2 + r * 0.1, Color::hex(0x1A1A1A), Sd(1.6));
            p.circle(bx2, by2 + r * 0.5, Sd(1), Color::hex(0x1A1A1A));
          }
          break;
        }
        case TOUCHPAD_STATUS: {
          auto &tp = TouchpadMonitor::get();
          draw_touchpad(p, gx, gy + gs * 0.14, gs, gs * 0.72, tp.state(), tp.enabled(), true,
                        t.text, t.accent);
          break;
        }
        case SOUND_STATUS:
          tray_icon::speaker(p, gx, gy, gs,
                             system.audio.volume > 66   ? 3
                             : system.audio.volume > 33 ? 2
                             : system.audio.volume > 0  ? 1
                                                        : 0,
                             system.audio.muted || system.audio.volume == 0, t.text);
          break;
        case CONTROLS_STATUS:
          tray_icon::action_center(p, gx, gy, gs, false, t.text, t.accent);
          break;
        case DRIVE_STATUS: {
          bool card = !controls.drives.empty() && controls.drives.front().kind == "card";
          tray_icon::drive(p, gx, gy, gs, card, t.text);
          break;
        }
        case DND_STATUS:
          tray_icon::moon(p, gx, gy, gs, t.text);
          break;
        case OVERFLOW_STATUS:
          tray_icon::overflow(
              p, gx, gy, gs, flyout_ && flyout_->is_open() && flyout_->section() == "tray", t.text);
          break;
      }
    }
    // The Action Center button in the corner: accent tile, own icon.
    bool center_open = quick_ && quick_->is_open();
    button_bg(action_x_, action_w_, {ACTION_CENTER, 0}, center_open && !quick_->calendar_view());
    p.fill_rounded(action_x_ + S(5), S(7), action_w_ - S(10), H - S(14), S(7),
                   quick_ && quick_->is_open() ? t.accent.mix(t.text, 0.15) : t.accent);
    tray_icon::action_center(p, action_x_ + (action_w_ - gs) / 2., gy, gs, false, t.on_accent,
                             t.on_accent);
    button_bg(clock_x_, clock_w_, {CLOCK, 0},
              (center_open && quick_->calendar_view()) || (alarms_ && alarms_->is_open()));
    if (show_desktop_w_) {
      if (hover_ == Target{SHOW_DESKTOP, 0})
        p.fill_rect(show_desktop_x_, 0, show_desktop_w_, H, t.hover);
      p.line(show_desktop_x_ + S(2) + 0.5, S(10), show_desktop_x_ + S(2) + 0.5, H - S(10), t.border);
    }

    // The launcher being dragged follows the pointer.
    if (dragging_ && drag_index_ >= 0 && drag_index_ < int(pinned_.size())) {
      int dx = drag_x_ - pin_w_ / 2;
      p.fill_rounded(dx + S(2), S(4), pin_w_ - S(4), H - S(8), S(8), t.accent.alpha(0.2));
      p.image(IconLoader::get({pinned_[size_t(drag_index_)].entry.icon, "application-x-executable"},
                              icon),
              dx + (pin_w_ - icon) / 2.0, (H - icon) / 2.0, icon);
    }
  }

  // Text on top of the cairo layer.
  text_font(t.font_size, true);
  draw_text("SDE", S(20) + icon, 0, start_w_ - icon - S(28), H, t.on_accent);
  if (search_box_shown()) {
    int bh = std::min(H - S(10), S(34));
    bool searching = search_ && search_->is_open();
    panel_search::draw_field_text(t, search_x_ + S(4), (H - bh) / 2, search_w_ - S(8), bh,
                                  searching ? search_->query() : "",
                                  searching ? search_->caret() : 0,
                                  searching && search_->caret_on(), searching,
                                  searching && search_->all_selected(), _("Type here to search"));
  }
  for (auto &task : tasks_) {
    if (task.w < H + S(20)) continue;
    text_font(t.font_size, task.win == active_ && !task.minimized);
    draw_text(task.title, task.x + S(10) + icon + S(8), 0, task.w - icon - S(28), H,
              task.minimized ? t.text_dim : t.text);
  }
  bool show_date = cfg_.get_bool("panel", "show_date", true);
  if (H >= S(40) && show_date) {
    text_font(t.font_size, true);
    draw_text(clock_time_, clock_x_, H / 2 - S(17) + S(2), clock_w_, S(18), t.text, 1);
    text_font(t.font_size - 1);
    draw_text(clock_date_, clock_x_, H / 2 - S(1), clock_w_, S(18), t.text_dim, 1);
  } else {
    text_font(t.font_size, true);
    std::string str = show_date ? clock_date_ + "  " + clock_time_ : clock_time_;
    draw_text(str, clock_x_, 0, clock_w_, H, t.text, 1);
  }
  if (qa_dump_enabled()) qa_dump_panel();
}

// Panel buttons for UI tests (see qa_dump in popup.h).
void Panel::qa_dump_panel() {
  int H = h();
  auto line = [&](const std::string &name, int bx, int bw) {
    return name + "\t" + std::to_string(bx) + "\t0\t" + std::to_string(bw) + "\t" +
           std::to_string(H) + "\t1\n";
  };
  std::string text = line("start", 0, start_w_) + line("clock", clock_x_, clock_w_) +
                     (search_w_ ? line(search_box_shown() ? "search" : "search-icon", search_x_,
                                       search_w_)
                                : std::string()) +
                     line("action-center", action_x_, action_w_);
  // The tooltip the panel handed to Fl_Tooltip for the hovered part.
  if (!tip_.current().empty()) text += "@tooltip:" + tip_.current() + "\t0\t0\t0\t0\t1\n";
  {
    const auto &n = SystemStatus::get().network;
    NetLed led = network_led(n.connected(), n.connectivity, led_probe_failed_,
                             PanelControls::get().net_verdict);
    if (cfg_.get_bool("panel", "network_led", true) && status_index(NETWORK_STATUS) >= 0)
      text += std::string("@led:") +
              (led == NetLed::GREEN ? "green" : led == NetLed::YELLOW ? "yellow" : "red") +
              (led_running_ ? ":pulse" : ":steady") + "\t0\t0\t0\t0\t1\n";
  }
  for (size_t i = 0; i < status_icons_.size(); ++i) {
    std::string name = status_section(status_icons_[i]);
    if (name.empty()) name = "icon-" + std::to_string(int(status_icons_[i]));
    text += line("status:" + name, status_x_ + int(i) * S(26), S(26));
  }
  // Taskbar buttons (alpha51 window previews).
  for (const auto &t : tasks_)
    if (t.w > 0)
      text += line("task:" + printable_text(t.title.empty() ? t.wm_class : t.title) +
                       (t.minimized ? ":minimized" : ""),
                   t.x, t.w);
  qa_dump(this, text);
}

// ------------------------------------------------------------------ events

int Panel::handle(int event) {
  switch (event) {
    case FL_ENTER:
      if (autohide_) {
        Fl::remove_timeout(hide_timeout, this);
        set_revealed(true);
      }
      [[fallthrough]];
    case FL_MOVE: {
      Target t = hit(Fl::event_x(), Fl::event_y());
      if (!(t == hover_)) {
        hover_ = t;
        update_tooltip();
        TouchpadMonitor::get().want_motion(touchpad_motion_wanted());
        redraw();
      }
      preview_hover(t);
      return 1;
    }
    case FL_LEAVE:
      preview_hover(Target{});
      hover_ = Target{};
      TouchpadMonitor::get().want_motion(touchpad_motion_wanted());
      redraw();
      if (autohide_) Fl::add_timeout(0.8, hide_timeout, this);
      return 1;
    case FL_PUSH:
      // A click on the panel closes the window preview at once.
      Fl::remove_timeout(preview_timeout, this);
      preview_pending_ = 0;
      if (preview_ && preview_->showing()) preview_->hide_now();
      pressed_ = hit(Fl::event_x(), Fl::event_y());
      if (pressed_.kind == PINNED && Fl::event_button() == FL_LEFT_MOUSE) {
        drag_index_ = pressed_.index;
        drag_start_x_ = drag_x_ = Fl::event_x();
        dragging_ = false;
      }
      redraw();
      return 1;
    case FL_DRAG: {
      if (drag_index_ < 0) return 1;
      drag_x_ = Fl::event_x();
      if (!dragging_ && std::abs(drag_x_ - drag_start_x_) > S(6)) dragging_ = true;
      if (dragging_ && !pinned_.empty()) {
        int first = (search_w_ ? search_x_ + search_w_ : start_w_) + S(4);
        int target = std::max(0, std::min(visible_pins_ - 1, (drag_x_ - first) / pin_w_));
        if (target != drag_index_) {
          Pinned moved = pinned_[size_t(drag_index_)];
          pinned_.erase(pinned_.begin() + drag_index_);
          pinned_.insert(pinned_.begin() + target, moved);
          drag_index_ = target;
          relayout();
        }
        redraw();
      }
      return 1;
    }
    case FL_RELEASE: {
      if (dragging_) {
        dragging_ = false;
        drag_index_ = -1;
        pressed_ = Target{};
        save_pins();
        relayout();
        redraw();
        return 1;
      }
      drag_index_ = -1;
      Target t = hit(Fl::event_x(), Fl::event_y());
      Target p = pressed_;
      pressed_ = Target{};
      redraw();
      if (!(t == p)) return 1;
      int button = Fl::event_button();
      if (button == FL_RIGHT_MOUSE &&
          (t.kind == NONE || t.kind == START || t.kind == SEARCH || t.kind == CLOCK || t.kind == SHOW_DESKTOP ||
           t.kind == STATUS || t.kind == ACTION_CENTER)) {
        show_context_menu();
        return 1;
      }
      switch (t.kind) {
        case START:
          toggle_menu();
          break;
        case SEARCH:
          open_search();
          break;
        case PINNED:
          if (button == FL_RIGHT_MOUSE)
            pinned_menu(t.index);
          else
            launch_entry(pinned_[size_t(t.index)].entry);
          break;
        case TASK:
          task_clicked(t.index, button);
          break;
        case TRAY_ITEM:
          tray_item_clicked(t.index, button);
          break;
        case STATUS:
          status_clicked(t.index, button);
          break;
        case ACTION_CENTER:
          open_action_center();
          break;
        case CLOCK:
          // The clock opens the alarms; Super+C (sde-panel --calendar) and
          // the alarm popup's link open the calendar and agenda.
          open_alarms();
          break;
        case SHOW_DESKTOP:
          x11::set_showing_desktop(!x11::showing_desktop());
          break;
        default:
          break;
      }
      return 1;
    }
    case FL_MOUSEWHEEL: {
      Target t = hit(Fl::event_x(), Fl::event_y());
      if (t.kind == STATUS && t.index >= 0 && t.index < int(status_icons_.size()) &&
          status_icons_[size_t(t.index)] == SOUND_STATUS && SystemStatus::get().audio.available) {
        SystemStatus::get().set_volume(SystemStatus::get().audio.volume - Fl::event_dy() * 5);
        redraw();
      } else if (t.kind == TRAY_ITEM && sni_) {
        auto items = sni_->visible_items();
        if (t.index < int(items.size()))
          sni_->scroll(*items[size_t(t.index)], -Fl::event_dy() * 120);
      }
      return 1;
    }
    default:
      return sde::BufferedWindow::handle(event);
  }
}

void Panel::tray_item_clicked(int index, int button) {
  if (!sni_) return;
  auto items = sni_->visible_items();
  if (index < 0 || index >= int(items.size())) return;
  int x = Fl::event_x_root(), y = Fl::event_y_root();
  if (button == FL_LEFT_MOUSE)
    sni_->activate(*items[size_t(index)], x, y);
  else if (button == FL_MIDDLE_MOUSE)
    sni_->secondary_activate(*items[size_t(index)], x, y);
  else
    sni_->context_menu(*items[size_t(index)], x, y);
}

void Panel::pinned_menu(int index) {
  if (index < 0 || index >= int(pinned_.size())) return;
  DesktopEntry e = pinned_[size_t(index)].entry;
  Fl_Menu_Item items[] = {
      {_("Open"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Move Left"), 0, nullptr, nullptr, index == 0 ? FL_MENU_INACTIVE : 0, 0, 0, 0, 0},
      {_("Move Right"), 0, nullptr, nullptr,
       (index + 1 >= int(pinned_.size()) ? FL_MENU_INACTIVE : 0) | FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Unpin from Panel"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  const Fl_Menu_Item *m = popup_menu(items, Fl::event_x_root());
  if (m == &items[0]) {
    launch_entry(e);
  } else if (m == &items[1] || m == &items[2]) {
    int other = m == &items[1] ? index - 1 : index + 1;
    std::swap(pinned_[size_t(index)], pinned_[size_t(other)]);
    save_pins();
    relayout();
    redraw();
  } else if (m == &items[3]) {
    unpin(e.id);
  }
}

// ------------------------------------------------------- window previews

bool Panel::previews_enabled() const { return cfg_.get_bool("panel", "window_previews", true); }

// Hovering a task button shows its preview after [panel] preview_delay_ms;
// another button while a preview shows swaps it at once; anything else
// hides it after 250 ms (unless the pointer moves onto the preview).
void Panel::preview_hover(const Target &t) {
  if (!preview_) return;
  bool task = t.kind == TASK && t.index >= 0 && t.index < int(tasks_.size()) &&
              previews_enabled() && !any_popup_open() && !Fl::pushed();
  if (!task) {
    Fl::remove_timeout(preview_timeout, this);
    preview_pending_ = 0;
    if (preview_->showing()) preview_->hide_soon();
    return;
  }
  Window win = tasks_[size_t(t.index)].win;
  if (preview_->showing()) {
    preview_->cancel_hide();
    if (preview_->hovered() != win) show_preview(t.index);
    return;
  }
  if (preview_pending_ == win) return;
  preview_pending_ = win;
  Fl::remove_timeout(preview_timeout, this);
  int delay = std::max(0, std::min(5000, cfg_.get_int("panel", "preview_delay_ms", 400)));
  Fl::add_timeout(delay / 1000.0, preview_timeout, this);
}

void Panel::preview_timeout(void *data) {
  auto *p = static_cast<Panel *>(data);
  Window win = p->preview_pending_;
  p->preview_pending_ = 0;
  if (!(p->hover_.kind == TASK) || p->any_popup_open()) return;
  for (size_t i = 0; i < p->tasks_.size(); ++i)
    if (p->tasks_[i].win == win && int(i) == p->hover_.index) p->show_preview(int(i));
}

void Panel::show_preview(int index) {
  if (index < 0 || index >= int(tasks_.size())) return;
  const Task &hovered = tasks_[size_t(index)];
  // The windows of the same application side by side, in taskbar order.
  std::vector<TaskPreview::Item> items;
  for (const auto &t : tasks_) {
    bool same = &t == &hovered || (!hovered.wm_class.empty() && t.wm_class == hovered.wm_class);
    if (same) items.push_back({t.win, printable_text(t.title.empty() ? t.wm_class : t.title), t.icon});
  }
  preview_->show_items(items, x() + hovered.x + hovered.w / 2, hovered.win);
}

// The active window's picture, every few seconds while the panel is idle:
// a window minimized with its title bar button still has a recent one.
void Panel::thumbnail_tick(void *data) {
  auto *p = static_cast<Panel *>(data);
  if (p->previews_enabled() && p->active_ && !p->any_popup_open() &&
      !(p->preview_ && p->preview_->showing())) {
    for (const auto &t : p->tasks_)
      if (t.win == p->active_ && !t.minimized) {
        TaskPreview::capture(t.win);
        break;
      }
  }
  Fl::repeat_timeout(5.0, thumbnail_tick, data);
}

void Panel::task_clicked(int index, int button) {
  if (index < 0 || index >= int(tasks_.size())) return;
  Task &t = tasks_[size_t(index)];
  // Keep a picture of a window the panel minimizes (alpha51 previews).
  if (previews_enabled() && !t.minimized) TaskPreview::capture(t.win);
  if (button == FL_MIDDLE_MOUSE) {
    x11::close_window(t.win);
    return;
  }
  if (button == FL_RIGHT_MOUSE) {
    Window win = t.win;
    bool minimized = t.minimized;
    DesktopEntry entry;
    bool has_entry = find_entry_for(t, entry);
    bool pinned = has_entry && is_pinned(entry.id);
    Fl_Menu_Item items[] = {
        {minimized ? _("Restore") : _("Minimize"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
        {_("Maximize / Restore Size"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
        {pinned ? _("Unpin from Panel") : _("Pin to Panel"), 0, nullptr, nullptr,
         (has_entry ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER, 0, 0, 0, 0},
        {_("Close"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
    const Fl_Menu_Item *m = popup_menu(items, Fl::event_x_root());
    if (!m) return;
    if (m == &items[0]) {
      if (minimized)
        x11::activate_window(win);
      else
        x11::minimize_window(win);
    } else if (m == &items[1]) {
      x11::toggle_maximize(win);
    } else if (m == &items[2]) {
      if (pinned)
        unpin(entry.id);
      else
        pin(entry.id);
    } else if (m == &items[3]) {
      x11::close_window(win);
    }
    return;
  }
  if (t.win == active_ && !t.minimized)
    x11::minimize_window(t.win);
  else
    x11::activate_window(t.win);
}

void Panel::show_context_menu() {
  auto radio = [&](int mode) { return FL_MENU_RADIO | (search_mode_ == mode ? FL_MENU_VALUE : 0); };
  Fl_Menu_Item items[] = {{_("Search"), 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER, 0, 0, 0, 0},
                          {_("Show Search Box"), 0, nullptr, nullptr, radio(2), 0, 0, 0, 0},
                          {_("Show Search Icon"), 0, nullptr, nullptr, radio(1), 0, 0, 0, 0},
                          {_("Hidden"), 0, nullptr, nullptr, radio(0), 0, 0, 0, 0},
                          {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                          {_("Panel Settings"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                          {_("Appearance Settings"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                          {_("Run Command..."), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
                          {_("About SDE"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                          {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  const Fl_Menu_Item *m = popup_menu(items, Fl::event_x_root());
  if (m == &items[1])
    set_setting("panel", "search", "box");
  else if (m == &items[2])
    set_setting("panel", "search", "icon");
  else if (m == &items[3])
    set_setting("panel", "search", "hidden");
  else if (m == &items[5])
    spawn_detached({"sde-settings", "--page", "panel"});
  else if (m == &items[6])
    spawn_detached({"sde-settings", "--page", "appearance"});
  else if (m == &items[7])
    spawn_detached({"sde-run"});
  else if (m == &items[8])
    spawn_detached({"sde-settings", "--page", "about"});
}

// ------------------------------------------------------------ X11 / tasks

void Panel::update_task(Task &t, bool icon) {
  t.title = x11::window_title(t.win);
  XClassHint hint;
  if (XGetClassHint(fl_display, t.win, &hint)) {
    t.wm_name = hint.res_name ? hint.res_name : "";
    t.wm_class = hint.res_class ? hint.res_class : "";
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
  }
  if (t.title.empty()) t.title = t.wm_class;
  t.minimized = x11::window_has_state(t.win, "_NET_WM_STATE_HIDDEN");
  t.urgent = x11::window_has_state(t.win, "_NET_WM_STATE_DEMANDS_ATTENTION");
  if (XWMHints *hints = XGetWMHints(fl_display, t.win)) {
    if (hints->flags & XUrgencyHint) t.urgent = true;
    XFree(hints);
  }
  // _NET_WM_ICON is large (often 100+ KiB with every size): fetch and
  // scale it again only when the icon may have changed, not on every title
  // change of a terminal or a browser tab.
  if (!icon && t.icon) return;
  int size = icon_;
  if (t.icon) cairo_surface_destroy(t.icon);
  t.icon = x11::window_icon(t.win, size);
  if (!t.icon) {
    // Fall back to an icon theme lookup by WM_CLASS ("Firefox" -> "firefox").
    cairo_surface_t *themed = IconLoader::get({lower(t.wm_class), lower(t.wm_name)}, size);
    if (themed) t.icon = cairo_surface_reference(themed);
  }
}

void Panel::refresh_tasks() {
  if (!configured_) return;
  Window root = RootWindow(fl_display, fl_screen);
  auto clients = x11::get_windows(root, "_NET_CLIENT_LIST");
  Window was_active = active_;
  active_ = x11::get_window(root, "_NET_ACTIVE_WINDOW");
  // The window that loses the focus is still on screen: keep its picture
  // for the preview (it may be minimized next).
  if (was_active && was_active != active_ && previews_enabled())
    for (const auto &t : tasks_)
      if (t.win == was_active && !t.minimized) TaskPreview::capture(t.win);
  std::vector<Task> next;
  for (Window w : clients) {
    if (!x11::window_is_normal(w) || x11::window_has_state(w, "_NET_WM_STATE_SKIP_TASKBAR"))
      continue;
    auto it = std::find_if(tasks_.begin(), tasks_.end(), [&](const Task &t) { return t.win == w; });
    if (it != tasks_.end()) {
      next.push_back(*it);
      it->icon = nullptr;  // ownership moved
    } else {
      Task t;
      t.win = w;
      XSelectInput(fl_display, w, PropertyChangeMask);
      update_task(t);
      next.push_back(t);
    }
  }
  for (auto &t : tasks_)
    if (t.icon) cairo_surface_destroy(t.icon);
  tasks_ = std::move(next);
  // Closed windows drop their thumbnails; the preview follows the list.
  std::vector<Window> alive;
  for (const auto &t : tasks_) alive.push_back(t.win);
  TaskPreview::retain(alive);
  if (preview_ && preview_->showing()) {
    if (std::find(alive.begin(), alive.end(), preview_->hovered()) == alive.end())
      preview_->hide_now();
  }
  relayout();
  redraw();
}

bool Panel::handle_x(const XEvent &e) {
  bool tray_changed = false;
  if (tray_ && tray_->handle(e, tray_changed)) {
    if (tray_changed) {
      status_signature_.clear();
      status_update();
    }
    return true;
  }
  Window root = RootWindow(fl_display, fl_screen);
  if (e.type == PropertyNotify && e.xproperty.window == root) {
    Atom a = e.xproperty.atom;
    if (a == x11::atom("_NET_CLIENT_LIST") || a == x11::atom("_NET_ACTIVE_WINDOW")) {
      // Activating another window closes the popups, except for the focus
      // change that follows an application quitting just before they opened.
      if (a == x11::atom("_NET_ACTIVE_WINDOW") && !Fl::grab() &&
          steady_ms() - popup_opened_at_ > 500) {
        if (quick_->is_open()) quick_->close();
        if (flyout_->is_open()) flyout_->close();
        if (status_flyout_->is_open()) status_flyout_->close();
        if (alarms_->is_open()) alarms_->close();
      }
      refresh_tasks();
    } else if (a == x11::atom("_SDE_COMMAND")) {
      std::string cmd = x11::read_command();
      if (cmd == "menu")
        toggle_menu();
      else if (cmd == "search")
        open_search();
      else if (starts_with(cmd, "brightness:"))
        SystemStatus::get().step_brightness(atoi(cmd.c_str() + 11));
      else if (starts_with(cmd, "search:"))
        open_search(cmd.substr(7));
      else if (cmd == "quick")
        open_action_center();
      else if (cmd == "quick:overview")
        open_action_center();
      else if (starts_with(cmd, "quick:"))
        open_flyout(cmd.substr(6));
      else if (cmd == "calendar")
        open_action_center(true);
      else if (cmd == "alarms")
        open_alarms();
      else if (cmd == "alarms:new")
        open_alarms(true);
    } else if (a == x11::atom("_SDE_CONFIG_SERIAL")) {
      bool was_autohide = autohide_;
      load_config();
      if (!autohide_) revealed_ = true;
      place_on_screen();
      for (auto &t : tasks_) update_task(t);
      relayout();
      redraw();
      quick_->status_changed();
      flyout_->status_changed();
      status_flyout_->status_changed();
      led_update();
      TouchpadMonitor::get().want_motion(touchpad_motion_wanted());
      Alarms::get().reload();
      alarms_->changed();
      if (autohide_ && !was_autohide) Fl::add_timeout(1.0, hide_timeout, this);
    }
    return true;
  }
  if (e.type == ConfigureNotify && e.xconfigure.window == root) {
    // Screen size changed (xrandr).
    Fl::call_screen_init();
    place_on_screen();
    return true;
  }
  if (e.type == PropertyNotify) {
    for (auto &t : tasks_) {
      if (t.win != e.xproperty.window) continue;
      Atom a = e.xproperty.atom;
      if (a == x11::atom("_NET_WM_NAME") || a == XA_WM_NAME || a == x11::atom("_NET_WM_STATE") ||
          a == x11::atom("_NET_WM_ICON") || a == XA_WM_HINTS ||
          a == x11::atom("_NET_WM_VISIBLE_NAME")) {
        if (a == x11::atom("_NET_WM_STATE") &&
            x11::window_has_state(t.win, "_NET_WM_STATE_SKIP_TASKBAR")) {
          refresh_tasks();
          return true;
        }
        bool was_minimized = t.minimized;
        update_task(t, a == x11::atom("_NET_WM_ICON") || a == XA_WM_HINTS);
        // Minimized: a last picture while the window may still be mapped.
        if (t.minimized && !was_minimized && previews_enabled()) TaskPreview::capture(t.win);
        redraw();
      }
      return true;
    }
  }
  return false;
}

int Panel::x_event_handler(int event) {
  if (!instance || !fl_xevent) return 0;
  return instance->handle_x(*fl_xevent) ? (event == 0 ? 1 : 0) : 0;
}

// ------------------------------------------------------------------ timers

void Panel::clock_tick(void *data) {
  auto *p = static_cast<Panel *>(data);
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char buf[64];
  bool h24 = p->cfg_.get_bool("panel", "clock_24h", true);
  strftime(buf, sizeof buf, h24 ? "%H:%M" : "%I:%M %p", &lt);
  std::string tstr = buf;
  if (!h24 && tstr[0] == '0') tstr.erase(0, 1);
  strftime(buf, sizeof buf, "%a %e %b", &lt);
  std::string dstr = trim(buf);
  if (tstr != p->clock_time_ || dstr != p->clock_date_) {
    bool relayout_needed = dstr.size() != p->clock_date_.size();
    p->clock_time_ = tstr;
    p->clock_date_ = dstr;
    if (relayout_needed) p->relayout();
    p->redraw();
    if (p->quick_ && p->quick_->is_open()) p->quick_->redraw();
    if (p->alarms_ && p->alarms_->is_open()) p->alarms_->redraw();
  }
  // Wake up right after the next full minute.
  Fl::remove_timeout(clock_tick, data);
  Fl::add_timeout(60 - lt.tm_sec + 0.2, clock_tick, data);
}
