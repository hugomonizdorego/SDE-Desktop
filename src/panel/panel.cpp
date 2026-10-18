// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "calendar.h"
#include "controls.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "menu.h"
#include "paint.h"
#include "quick.h"
#include "rust_layout.h"
#include "sni.h"
#include "spawn.h"
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

Panel::Panel() : sde::BufferedWindow(0, 0, 800, 40, "SDE Panel") {
  instance = this;
  end();
  border(0);
  resizable(nullptr);
  load_config();
  place_on_screen();
  menu_.reset(new AppMenu());
  quick_.reset(new QuickSettings());
  calendar_.reset(new Calendar());
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
  tray_.reset(new Tray(xid));
  tray_->set_icon_size(tray_icon_);
  tray_->start();
  sni_.reset(new SniHost([this] {
    relayout();
    redraw();
    quick_->status_changed();
  }));
  sni_->start();
  SystemStatus::get().start([this] {
    relayout();
    redraw();
    quick_->status_changed();
  });

  configured_ = true;
  refresh_tasks();
  clock_tick(this);
  if (autohide_) Fl::add_timeout(1.5, hide_timeout, this);
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
  if (open_popup_ && open_popup_ != p) open_popup_->close();
  if (p->is_open()) {
    p->close();
    return;
  }
  set_revealed(true);
  p->open_at(anchor_x, align_right);
}

void Panel::popup_opened(PanelPopup *p) {
  open_popup_ = p;
  if (p == quick_.get()) PanelControls::get().set_visible(true);
  relayout();
  redraw();
}

void Panel::popup_closed(PanelPopup *p) {
  if (open_popup_ == p) open_popup_ = nullptr;
  if (p == quick_.get()) PanelControls::get().set_visible(false);
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
void Panel::toggle_calendar() { open_popup(calendar_.get(), x() + w() - S(8), true); }
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

void Panel::set_do_not_disturb(bool on) {
  cfg_.set("notifications", "do_not_disturb", on ? "true" : "false");
  cfg_.save();
  x11::notify_config_changed();
  quick_->status_changed();
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

bool Panel::status_in_center() const {
  return quick_ && quick_->is_open() && cfg_.get_bool("action_center", "move_status", true);
}
int Panel::status_count() const { return int(status_icons_.size()); }
void Panel::input_event(int, void *d) {
  auto *p = static_cast<Panel *>(d);
  while (p->input_display_ && XPending(p->input_display_)) {
    XEvent event;
    XNextEvent(p->input_display_, &event);
    if (event.type == GenericEvent && event.xcookie.extension == p->input_opcode_ &&
        event.xcookie.evtype == XI_RawButtonPress)
      p->outside_pointer_press();
  }
}
void Panel::outside_pointer_press() {
  if (!quick_ || !quick_->is_open() || Fl::grab()) return;
  Window root = DefaultRootWindow(fl_display), r, child;
  int rx, ry, x, y;
  unsigned buttons;
  if (!XQueryPointer(fl_display, root, &r, &child, &rx, &ry, &x, &y, &buttons)) return;
  if (rx >= quick_->x() && rx < quick_->x() + quick_->w() && ry >= quick_->y() &&
      ry < quick_->y() + quick_->h())
    return;
  block_quick_until_ = steady_ms() + 300;
  quick_->close();
}
void Panel::open_action_center(const std::string &section) {
  if (!quick_->is_open() && steady_ms() < block_quick_until_) return;
  if (quick_->is_open() && quick_->section() != section) {
    quick_->select_section(section);
    return;
  }
  quick_->select_section(section);
  open_popup(quick_.get(), x() + w(), true);
}
void Panel::status_clicked(int index, int button) {
  if (index < 0 || index >= int(status_icons_.size())) return;
  switch (status_icons_[size_t(index)]) {
    case INPUT_STATUS:
      spawn_detached({"sde-settings", "--page", "input"});
      break;
    case BLUETOOTH_STATUS:
      open_action_center("bluetooth");
      break;
    case BATTERY_STATUS:
      open_action_center("power");
      break;
    case POWER_STATUS:
      spawn_detached({"sde-logout"});
      break;
    case MEDIA_STATUS:
      open_action_center("media");
      break;
    case NETWORK_STATUS:
      open_action_center("network");
      break;
    case SOUND_STATUS:
      if (button == FL_MIDDLE_MOUSE)
        SystemStatus::get().toggle_mute();
      else
        open_action_center("sound");
      break;
    case CONTROLS_STATUS:
      open_action_center();
      break;
    case OVERFLOW_STATUS:
      open_action_center("tray");
      break;
  }
}

void Panel::relayout() {
  int H = theme_.panel_height;
  icon_ = H >= S(48) ? S(28) : H >= S(40) ? S(24) : S(20);
  tray_icon_ = H >= S(40) ? S(20) : S(18);
  start_w_ = H + S(46);
  pin_w_ = H;

  int right = w();
  show_desktop_x_ = right - S(10);
  right = show_desktop_x_ - S(4);
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
  right = clock_x_ - S(2);

  action_w_ = S(34);
  action_x_ = show_desktop_x_ - S(2) - action_w_;
  // Clock is next to the SDE Action Center button, like the reference panel.
  clock_x_ = action_x_ - clock_w_ - S(2);
  right = clock_x_ - S(4);
  status_icons_.clear();
  const auto &s = SystemStatus::get();
  const auto &controls = PanelControls::get();
  auto add = [&](StatusIcon icon, const char *key, bool available = true) {
    if (available && cfg_.get_bool("panel", key, true)) status_icons_.push_back(icon);
  };
  add(OVERFLOW_STATUS, "show_tray");
  add(INPUT_STATUS, "show_keyboard");
  add(BLUETOOTH_STATUS, "show_bluetooth", !controls.adapters.empty());
  add(BATTERY_STATUS, "show_battery", s.battery.present);
  add(POWER_STATUS, "show_power");
  add(MEDIA_STATUS, "show_media");
  add(NETWORK_STATUS, "show_network", s.network.available);
  add(SOUND_STATUS, "show_sound", s.audio.available);
  add(CONTROLS_STATUS, "show_controls");
  int sn = sni_ ? int(sni_->visible_items().size()) : 0, tw = tray_ ? tray_->width() : 0;
  compact_status_ =
      status_in_center() || right - start_w_ - S(100) <
                                int(status_icons_.size()) * S(26) + sn * (tray_icon_ + S(10)) + tw;
  if (compact_status_) status_icons_.clear();
  status_w_ = int(status_icons_.size()) * S(26);
  status_x_ = right - status_w_;
  right = status_x_;
  sni_w_ = compact_status_ ? 0 : sn * (tray_icon_ + S(10));
  sni_x_ = right - sni_w_;
  right = sni_x_;
  tw = compact_status_ ? 0 : tw;
  tray_x_ = right - tw;
  if (tray_ && !status_in_center()) {
    if (compact_status_)
      tray_->hide();
    else {
      tray_->set_background(theme_.surface.fl() >> 8);
      tray_->place(tray_x_, H);
    }
  }
  tasks_end_ = tray_x_ - S(8);

  // Keep the status area and running windows reachable on short panels.
  int room = std::max(0, tasks_end_ - start_w_ - S(12));
  int task_room = tasks_.empty() ? 0 : std::min(2 * H, room / 2);
  visible_pins_ = std::min(int(pinned_.size()), std::max(0, (room - task_room) / pin_w_));
  int pin_x = start_w_ + S(4);
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
  if (x >= show_desktop_x_) return {SHOW_DESKTOP, 0};
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
    p.fill_rounded(S(6), S(5), start_w_ - S(8), H - S(10),
                   cfg_.get("panel", "style", "full") == "full" ? 0 : S(10),
                   hover_.kind == START ? t.accent.mix(t.text, 0.12) : t.accent);
    p.image(IconLoader::get({"sde-logo", "start-here", "distributor-logo"}, icon), S(14),
            (H - icon) / 2.0, icon);

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
    if (sni_ && !compact_status_) {
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
      button_bg(bx, S(26), {STATUS, int(i)}, false);
      switch (status_icons_[i]) {
        case INPUT_STATUS:
          glyph::keyboard(p, gx, gy, gs, t.text);
          break;
        case BLUETOOTH_STATUS: {
          auto *a = controls.adapter();
          glyph::bluetooth(p, gx, gy, gs, t.text, a && a->powered);
          break;
        }
        case BATTERY_STATUS:
          glyph::battery(p, gx, gy, gs, system.battery.percent, system.battery.charging, t.text,
                         system.battery.percent <= 15 ? Color::hex(0xD64545) : t.text);
          break;
        case POWER_STATUS:
          glyph::power(p, gx, gy, gs, t.text);
          break;
        case MEDIA_STATUS: {
          auto players = controls.players();
          glyph::media(p, gx, gy, gs, t.text,
                       !players.empty() && players.front().status == "Playing");
          break;
        }
        case NETWORK_STATUS:
          if (system.network.kind == "ethernet")
            glyph::ethernet(p, gx, gy, gs, t.text);
          else
            glyph::wifi(p, gx, gy, gs, std::max(1, system.network.signal / 25), t.text,
                        system.network.kind.empty());
          break;
        case SOUND_STATUS:
          glyph::speaker(p, gx, gy, gs,
                         system.audio.volume > 66   ? 3
                         : system.audio.volume > 33 ? 2
                         : system.audio.volume > 0  ? 1
                                                    : 0,
                         system.audio.muted, t.text);
          break;
        case CONTROLS_STATUS:
          glyph::sliders(p, gx, gy, gs, t.text);
          break;
        case OVERFLOW_STATUS: {
          auto *cr = p.cr();
          cairo_move_to(cr, gx + gs * .25, gy + gs * .6);
          cairo_line_to(cr, gx + gs * .5, gy + gs * .35);
          cairo_line_to(cr, gx + gs * .75, gy + gs * .6);
          p.set(t.text);
          cairo_set_line_width(cr, Sd(1.5));
          cairo_stroke(cr);
          break;
        }
      }
    }
    button_bg(action_x_, action_w_, {ACTION_CENTER, 0}, quick_ && quick_->is_open());
    p.fill_rounded(action_x_ + S(5), S(7), action_w_ - S(10), H - S(14), S(5), t.accent);
    glyph::sliders(p, action_x_ + (action_w_ - gs) / 2., gy, gs, t.on_accent);
    button_bg(clock_x_, clock_w_, {CLOCK, 0}, quick_ && quick_->is_open());
    if (hover_ == Target{SHOW_DESKTOP, 0})
      p.fill_rect(show_desktop_x_, 0, W - show_desktop_x_, H, t.hover);
    p.line(show_desktop_x_ + 0.5, S(10), show_desktop_x_ + 0.5, H - S(10), t.border);

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
        std::string tip;
        if (t.kind == TRAY_ITEM && sni_) {
          auto items = sni_->visible_items();
          if (t.index < int(items.size())) tip = items[size_t(t.index)]->title;
        } else if (t.kind == STATUS && t.index >= 0 && t.index < int(status_icons_.size())) {
          const char *names[] = {_("Keyboard"), _("Bluetooth"),     _("Battery"),
                                 _("Power"),    _("Media players"), _("Network"),
                                 _("Sound"),    _("Action Center"), _("System tray")};
          tip = names[int(status_icons_[size_t(t.index)])];
          if (status_icons_[size_t(t.index)] == INPUT_STATUS)
            tip += " · " + PanelControls::get().keyboard;
        } else if (t.kind == ACTION_CENTER || t.kind == CLOCK)
          tip = _("Action Center · Super+A");
        copy_tooltip(tip.c_str());
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = Target{};
      redraw();
      if (autohide_) Fl::add_timeout(0.8, hide_timeout, this);
      return 1;
    case FL_PUSH:
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
        int first = start_w_ + S(4);
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
          (t.kind == NONE || t.kind == START || t.kind == CLOCK || t.kind == SHOW_DESKTOP ||
           t.kind == STATUS || t.kind == ACTION_CENTER)) {
        show_context_menu();
        return 1;
      }
      switch (t.kind) {
        case START:
          toggle_menu();
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
          open_action_center();
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

void Panel::task_clicked(int index, int button) {
  if (index < 0 || index >= int(tasks_.size())) return;
  Task &t = tasks_[size_t(index)];
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
  Fl_Menu_Item items[] = {{_("Panel Settings"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                          {_("Appearance Settings"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                          {_("Run Command..."), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
                          {_("About SDE"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                          {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  const Fl_Menu_Item *m = popup_menu(items, Fl::event_x_root());
  if (m == &items[0])
    spawn_detached({"sde-settings", "--page", "panel"});
  else if (m == &items[1])
    spawn_detached({"sde-settings", "--page", "appearance"});
  else if (m == &items[2])
    spawn_detached({"sde-run"});
  else if (m == &items[3])
    spawn_detached({"sde-settings", "--page", "about"});
}

// ------------------------------------------------------------ X11 / tasks

void Panel::update_task(Task &t) {
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
  active_ = x11::get_window(root, "_NET_ACTIVE_WINDOW");
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
  relayout();
  redraw();
}

bool Panel::handle_x(const XEvent &e) {
  bool tray_changed = false;
  if (tray_ && tray_->handle(e, tray_changed)) {
    if (tray_changed) {
      relayout();
      redraw();
      quick_->status_changed();
    }
    return true;
  }
  Window root = RootWindow(fl_display, fl_screen);
  if (e.type == PropertyNotify && e.xproperty.window == root) {
    Atom a = e.xproperty.atom;
    if (a == x11::atom("_NET_CLIENT_LIST") || a == x11::atom("_NET_ACTIVE_WINDOW")) {
      if (a == x11::atom("_NET_ACTIVE_WINDOW") && quick_->is_open() && !Fl::grab()) quick_->close();
      refresh_tasks();
    } else if (a == x11::atom("_SDE_COMMAND")) {
      std::string cmd = x11::read_command();
      if (cmd == "menu")
        toggle_menu();
      else if (cmd == "quick")
        open_action_center();
      else if (starts_with(cmd, "quick:"))
        open_action_center(cmd.substr(6));
      else if (cmd == "calendar")
        open_popup(calendar_.get(), x() + clock_x_ + clock_w_, true);
    } else if (a == x11::atom("_SDE_CONFIG_SERIAL")) {
      bool was_autohide = autohide_;
      load_config();
      if (!autohide_) revealed_ = true;
      place_on_screen();
      for (auto &t : tasks_) update_task(t);
      relayout();
      redraw();
      quick_->status_changed();
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
        update_task(t);
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
  }
  // Wake up right after the next full minute.
  Fl::remove_timeout(clock_tick, data);
  Fl::add_timeout(60 - lt.tm_sec + 0.2, clock_tick, data);
}
