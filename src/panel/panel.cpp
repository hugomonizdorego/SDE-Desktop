// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <X11/Xutil.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "icons.h"
#include "menu.h"
#include "paint.h"
#include "spawn.h"
#include "tray.h"
#include "util.h"
#include "x11.h"

using namespace sde;

Panel *Panel::instance = nullptr;

Panel::Panel() : sde::BufferedWindow(0, 0, 800, 40, "SDE Panel") {
  instance = this;
  end();
  border(0);
  load_config();
  place_on_screen();
  menu_.reset(new AppMenu());
}

Panel::~Panel() {
  for (auto &t : tasks_)
    if (t.icon) cairo_surface_destroy(t.icon);
}

void Panel::load_config() {
  cfg_.load();
  theme_ = Theme::from_config(cfg_);
  theme_.apply_to_fltk();
  IconLoader::set_theme(cfg_.get("appearance", "icon_theme", "auto"));
  IconLoader::clear_cache();
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
    if (pinned_.size() >= 6) break;
  }
  if (shown()) {
    Fl_Window::color(theme_.surface.fl());
    XSetWindowBackground(fl_display, fl_xid(this), theme_.surface.fl() >> 8);
  }
}

void Panel::place_on_screen() {
  Fl::screen_xywh(monitor_x_, monitor_y_, monitor_w_, monitor_h_, 0);
  int h = theme_.panel_height;
  sde::BufferedWindow::resize(monitor_x_, monitor_y_ + monitor_h_ - h, monitor_w_, h);
  if (shown()) {
    x11::set_bottom_strut(fl_xid(this), h, monitor_x_, monitor_x_ + monitor_w_ - 1,
                          monitor_y_ + monitor_h_);
    if (tray_) tray_->set_icon_size(h <= 32 ? 18 : 22);
  }
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
  x11::set_bottom_strut(xid, h(), x(), x() + w() - 1, monitor_y_ + monitor_h_);
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

  tray_.reset(new Tray(xid));
  tray_->set_icon_size(h() <= 32 ? 18 : 22);
  tray_->start();

  configured_ = true;
  refresh_tasks();
  clock_tick(this);
  battery_tick(this);
}

// ------------------------------------------------------------------ layout

void Panel::relayout() {
  int H = h();
  start_w_ = H + 8;
  int x = start_w_ + 4;
  for (auto &p : pinned_) {
    p.x = x;
    x += H;
  }
  if (!pinned_.empty()) x += 10;
  tasks_x_ = x;

  int right = w();
  show_desktop_x_ = right - 10;
  right = show_desktop_x_ - 4;
  text_font(theme_.font_size, false);
  bool two_lines = H >= 40 && cfg_.get_bool("panel", "show_date", true);
  std::string sample = cfg_.get_bool("panel", "clock_24h", true) ? "88:88" : "88:88 PM";
  int cw = text_width(sample);
  if (cfg_.get_bool("panel", "show_date", true)) {
    int dw = text_width("Wed 30 Sep");
    cw = two_lines ? std::max(cw, dw) : cw + dw + 10;
  }
  clock_w_ = cw + 24;
  clock_x_ = right - clock_w_;
  right = clock_x_ - 2;
  battery_w_ = (battery_percent_ >= 0 && cfg_.get_bool("panel", "show_battery", true))
                   ? text_width("100%") + 40
                   : 0;
  battery_x_ = right - battery_w_;
  right = battery_x_;
  int tw = tray_ ? tray_->width() : 0;
  tray_x_ = right - tw;
  if (tray_) tray_->place(tray_x_, H);
  tasks_end_ = tray_x_ - 8;

  int n = int(tasks_.size());
  if (n) {
    int avail = std::max(0, tasks_end_ - tasks_x_);
    int each = std::min(220, avail / n);
    each = std::max(each, std::min(H, avail / std::max(1, n)));
    int tx = tasks_x_;
    for (auto &t : tasks_) {
      t.x = tx;
      t.w = each - 4;
      tx += each;
    }
  }
}

Panel::Target Panel::hit(int x, int y) const {
  Target t;
  if (y < 0 || y >= h()) return t;
  if (x < start_w_) return {START, 0};
  for (size_t i = 0; i < pinned_.size(); ++i)
    if (x >= pinned_[i].x && x < pinned_[i].x + h()) return {PINNED, int(i)};
  for (size_t i = 0; i < tasks_.size(); ++i)
    if (x >= tasks_[i].x && x < tasks_[i].x + tasks_[i].w) return {TASK, int(i)};
  if (battery_w_ && x >= battery_x_ && x < battery_x_ + battery_w_) return {BATTERY, 0};
  if (x >= clock_x_ && x < clock_x_ + clock_w_) return {CLOCK, 0};
  if (x >= show_desktop_x_) return {SHOW_DESKTOP, 0};
  return t;
}

// ------------------------------------------------------------------ drawing

void Panel::draw() {
  const Theme &t = theme_;
  int W = w(), H = h();
  int icon = H >= 40 ? 24 : 20;
  if (H >= 48) icon = 28;
  {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface);
    p.line(0, 0.5, W, 0.5, t.border);

    auto button_bg = [&](int x, int bw, Target target, bool active) {
      bool hov = hover_ == target, down = pressed_ == target;
      Color c = active ? t.accent.alpha(t.dark ? 0.26 : 0.14) : t.hover;
      if (down) c = t.accent.alpha(0.3);
      if (active || hov || down) p.fill_rounded(x + 2, 4, bw - 4, H - 8, 8, c);
    };

    // Start button with the SDE logo.
    button_bg(4, start_w_ - 4, {START, 0}, menu_ && menu_->shown());
    p.image(IconLoader::get({"sde-logo", "start-here", "distributor-logo"}, icon),
            4 + (start_w_ - 4 - icon) / 2.0, (H - icon) / 2.0, icon);

    for (size_t i = 0; i < pinned_.size(); ++i) {
      button_bg(pinned_[i].x, H, {PINNED, int(i)}, false);
      p.image(IconLoader::get({pinned_[i].entry.icon}, icon), pinned_[i].x + (H - icon) / 2.0,
              (H - icon) / 2.0, icon);
    }
    if (!pinned_.empty()) p.line(tasks_x_ - 6, 10, tasks_x_ - 6, H - 10, t.border);

    for (size_t i = 0; i < tasks_.size(); ++i) {
      Task &task = tasks_[i];
      bool active = task.win == active_ && !task.minimized;
      button_bg(task.x, task.w, {TASK, int(i)}, active);
      if (task.urgent) p.fill_rounded(task.x + 2, 4, task.w - 4, H - 8, 8, Color::hex(0xE8A33D, 0.35));
      cairo_surface_t *img = task.icon ? task.icon : IconLoader::get({"application-x-executable"}, icon);
      int ix = task.w < H + 20 ? task.x + (task.w - icon) / 2 : task.x + 10;
      p.image(img, ix, (H - icon) / 2.0, icon, task.minimized ? 0.55 : 1.0);
      // Indicator bar: wide for the active window, short for the others.
      double bar = active ? std::min(28, task.w - 16) : 8;
      p.fill_rounded(task.x + (task.w - bar) / 2.0, H - 5, bar, 3, 1.5,
                     active ? t.accent : t.text_dim.alpha(0.6));
    }

    if (battery_w_) {
      button_bg(battery_x_, battery_w_, {BATTERY, 0}, false);
      double bx = battery_x_ + 10, by = H / 2.0 - 6;
      p.stroke_rounded(bx, by, 22, 12, 3, t.text, 1.4);
      p.fill_rect(bx + 22, by + 3.5, 2, 5, t.text);
      Color level = battery_percent_ <= 15 && !battery_charging_ ? Color::hex(0xD64545) : t.text;
      if (battery_charging_) level = Color::hex(0x2E9E5B);
      p.fill_rounded(bx + 2.5, by + 2.5, 17 * battery_percent_ / 100.0, 7, 1.5, level);
    }
    button_bg(clock_x_, clock_w_, {CLOCK, 0}, false);
    if (hover_ == Target{SHOW_DESKTOP, 0}) p.fill_rect(show_desktop_x_, 0, W - show_desktop_x_, H, t.hover);
    p.line(show_desktop_x_ + 0.5, 10, show_desktop_x_ + 0.5, H - 10, t.border);
  }

  // Text on top of the cairo layer.
  for (auto &task : tasks_) {
    if (task.w < H + 20) continue;
    text_font(t.font_size, task.win == active_ && !task.minimized);
    draw_text(task.title, task.x + 10 + icon + 8, 0, task.w - icon - 28, H,
              task.minimized ? t.text_dim : t.text);
  }
  if (battery_w_) {
    text_font(t.font_size - 1);
    draw_text(std::to_string(battery_percent_) + "%", battery_x_ + 38, 0, battery_w_ - 40, H, t.text);
  }
  bool show_date = cfg_.get_bool("panel", "show_date", true);
  if (H >= 40 && show_date) {
    text_font(t.font_size, true);
    draw_text(clock_time_, clock_x_, H / 2 - 17 + 2, clock_w_, 18, t.text, 1);
    text_font(t.font_size - 1);
    draw_text(clock_date_, clock_x_, H / 2 - 1, clock_w_, 18, t.text_dim, 1);
  } else {
    text_font(t.font_size, true);
    std::string s = show_date ? clock_date_ + "  " + clock_time_ : clock_time_;
    draw_text(s, clock_x_, 0, clock_w_, H, t.text, 1);
  }
}

// ------------------------------------------------------------------ events

int Panel::handle(int event) {
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      Target t = hit(Fl::event_x(), Fl::event_y());
      if (!(t == hover_)) {
        hover_ = t;
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = Target{};
      redraw();
      return 1;
    case FL_PUSH:
      pressed_ = hit(Fl::event_x(), Fl::event_y());
      redraw();
      return 1;
    case FL_DRAG:
      return 1;
    case FL_RELEASE: {
      Target t = hit(Fl::event_x(), Fl::event_y());
      Target p = pressed_;
      pressed_ = Target{};
      redraw();
      if (!(t == p)) return 1;
      int button = Fl::event_button();
      if (button == FL_RIGHT_MOUSE && t.kind != TASK) {
        show_context_menu();
        return 1;
      }
      switch (t.kind) {
        case START: toggle_menu(); break;
        case PINNED: launch_entry(pinned_[size_t(t.index)].entry); break;
        case TASK: task_clicked(t.index, button); break;
        case CLOCK: break;
        case BATTERY: spawn_detached({"sde-settings", "--page", "about"}); break;
        case SHOW_DESKTOP: x11::set_showing_desktop(!x11::showing_desktop()); break;
        default: break;
      }
      return 1;
    }
    case FL_MOUSEWHEEL:
      return 1;
    default:
      return sde::BufferedWindow::handle(event);
  }
}

void Panel::toggle_menu() {
  if (menu_->shown()) {
    menu_->close();
  } else {
    menu_->popup(theme_, x() + 4, y(), monitor_x_, monitor_w_);
  }
  redraw();
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
    Fl_Menu_Item items[] = {
        {t.minimized ? "Restore" : "Minimize", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
        {"Maximize / Restore size", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
        {"Close", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
    const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y() - 90);
    if (!m) return;
    if (m == &items[0]) {
      if (t.minimized) x11::activate_window(win);
      else x11::minimize_window(win);
    } else if (m == &items[1]) {
      x11::toggle_maximize(win);
    } else if (m == &items[2]) {
      x11::close_window(win);
    }
    return;
  }
  if (t.win == active_ && !t.minimized) x11::minimize_window(t.win);
  else x11::activate_window(t.win);
}

void Panel::show_context_menu() {
  Fl_Menu_Item items[] = {
      {"Panel Settings", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Desktop Settings", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {"Run Command...", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {"About SDE", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y() - 110);
  if (m == &items[0]) spawn_detached({"sde-settings", "--page", "panel"});
  else if (m == &items[1]) spawn_detached({"sde-settings", "--page", "appearance"});
  else if (m == &items[2]) spawn_detached({"sde-run"});
  else if (m == &items[3]) spawn_detached({"sde-settings", "--page", "about"});
}

// ------------------------------------------------------------ X11 / tasks

void Panel::update_task(Task &t) {
  t.title = x11::window_title(t.win);
  if (t.title.empty()) t.title = x11::window_class(t.win);
  t.minimized = x11::window_has_state(t.win, "_NET_WM_STATE_HIDDEN");
  t.urgent = x11::window_has_state(t.win, "_NET_WM_STATE_DEMANDS_ATTENTION");
  if (XWMHints *hints = XGetWMHints(fl_display, t.win)) {
    if (hints->flags & XUrgencyHint) t.urgent = true;
    XFree(hints);
  }
  int size = h() >= 48 ? 28 : h() >= 40 ? 24 : 20;
  if (t.icon) cairo_surface_destroy(t.icon);
  t.icon = x11::window_icon(t.win, size);
  if (!t.icon) {
    // Fall back to an icon theme lookup by WM_CLASS ("Firefox" -> "firefox").
    cairo_surface_t *themed = IconLoader::get({lower(x11::window_class(t.win))}, size);
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
    }
    return true;
  }
  Window root = RootWindow(fl_display, fl_screen);
  if (e.type == PropertyNotify && e.xproperty.window == root) {
    Atom a = e.xproperty.atom;
    if (a == x11::atom("_NET_CLIENT_LIST") || a == x11::atom("_NET_ACTIVE_WINDOW")) {
      refresh_tasks();
    } else if (a == x11::atom("_SDE_COMMAND")) {
      std::string cmd = x11::read_command();
      if (cmd == "menu") toggle_menu();
    } else if (a == x11::atom("_SDE_CONFIG_SERIAL")) {
      load_config();
      place_on_screen();
      for (auto &t : tasks_) update_task(t);
      relayout();
      redraw();
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
          a == x11::atom("_NET_WM_ICON") || a == XA_WM_HINTS || a == x11::atom("_NET_WM_VISIBLE_NAME")) {
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
  strftime(buf, sizeof buf, p->cfg_.get_bool("panel", "clock_24h", true) ? "%H:%M" : "%I:%M %p", &lt);
  std::string tstr = buf;
  if (!p->cfg_.get_bool("panel", "clock_24h", true) && tstr[0] == '0') tstr.erase(0, 1);
  strftime(buf, sizeof buf, "%a %d %b", &lt);
  if (tstr != p->clock_time_ || buf != p->clock_date_) {
    p->clock_time_ = tstr;
    p->clock_date_ = buf;
    p->redraw();
  }
  // Wake up right after the next full minute.
  Fl::repeat_timeout(60 - lt.tm_sec + 0.2, clock_tick, data);
}

void Panel::update_battery() {
  int percent = -1;
  bool charging = false;
  for (auto &dev : list_dir("/sys/class/power_supply")) {
    std::string base = "/sys/class/power_supply/" + dev, type, cap, status;
    if (!read_file(base + "/type", type) || trim(type) != "Battery") continue;
    std::string scope;
    if (read_file(base + "/scope", scope) && trim(scope) == "Device") continue;  // mice etc.
    if (!read_file(base + "/capacity", cap)) continue;
    percent = std::max(0, std::min(100, atoi(cap.c_str())));
    read_file(base + "/status", status);
    charging = trim(status) == "Charging" || trim(status) == "Full";
    break;
  }
  bool layout = (percent >= 0) != (battery_percent_ >= 0);
  if (percent != battery_percent_ || charging != battery_charging_) {
    battery_percent_ = percent;
    battery_charging_ = charging;
    if (layout) relayout();
    redraw();
  }
}

void Panel::battery_tick(void *data) {
  static_cast<Panel *>(data)->update_battery();
  Fl::repeat_timeout(30.0, battery_tick, data);
}
