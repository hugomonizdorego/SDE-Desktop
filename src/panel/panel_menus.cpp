// SPDX-License-Identifier: GPL-3.0-or-later
// alpha52: the right-click menu of every panel item.
//
// Start button, search, pinned launchers, taskbar buttons, each status icon
// (network, sound, Bluetooth, battery, keyboard, drives, touchpad, ...), the
// clock, the notifications button and the Action Center button have menus
// of their own; the empty panel keeps the generic panel menu. Tray items
// show their application's own menu (sni.cpp). With SDE_PANEL_QA_DIR set,
// every menu is described in <dir>/context-menu.txt for the UI tests.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_ask.H>
#include <FL/x.H>
#include <X11/Xatom.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "../common/menu.h"
#include "controls.h"
#include "i18n.h"
#include "notify_center.h"
#include "panel.h"
#include "popup.h"
#include "process.h"
#include "quick.h"
#include "spawn.h"
#include "status_model.h"
#include "timezone.h"
#include "touchpad_monitor.h"
#include "util.h"
#include "x11.h"

using namespace sde;

namespace {
std::function<void()> run(std::vector<std::string> cmd) {
  return [cmd] { spawn_detached(cmd); };
}
std::function<void()> settings(const std::string &page) {
  return run({"sde-settings", "--page", page});
}

int menu_seq = 0;

// <qa>/context-menu.txt: "@menu:<id>" then one "item:<label>" line per item
// ("sub:<submenu>/<label>" inside submenus), its active flag as the sixth
// field and ":on" for checked toggles and radios.
void qa_menu(const std::string &id, const Fl_Menu_Item *items) {
  const char *dir = getenv("SDE_PANEL_QA_DIR");
  if (!dir || !*dir) return;
  std::string text = "@menu:" + id + "\t0\t0\t0\t0\t1\n@seq:" + std::to_string(++menu_seq) +
                     "\t0\t0\t0\t0\t1\n";
  std::function<void(const Fl_Menu_Item *, const std::string &)> walk =
      [&](const Fl_Menu_Item *m, const std::string &prefix) {
        for (; m && m->text; m = m->next()) {
          std::string label = m->text;
          label.erase(std::remove(label.begin(), label.end(), '\t'), label.end());
          bool on = (m->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)) && (m->flags & FL_MENU_VALUE);
          text += (prefix.empty() ? "item:" : "sub:" + prefix) + label + (on ? ":on" : "") + "\t0\t0\t0\t0\t" +
                  std::to_string(m->active() ? 1 : 0) + "\n";
          // 0.2: "icon:<label>=<icon>" for items with their own icon.
          std::string icon = sde::menu_icon(m);
          if (!icon.empty()) text += "icon:" + label + "=" + icon + "\t0\t0\t0\t0\t1\n";
          if (m->submenu()) walk(m + 1, prefix + label + "/");
        }
      };
  walk(items, "");
  std::string path = std::string(dir) + "/context-menu.txt", tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}

void send_desktop(Window win, long desktop) {
  XEvent ev{};
  ev.xclient.type = ClientMessage;
  ev.xclient.window = win;
  ev.xclient.message_type = x11::atom("_NET_WM_DESKTOP");
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = desktop;
  ev.xclient.data.l[1] = 2;  // a pager
  Window root = RootWindow(fl_display, fl_screen);
  XSendEvent(fl_display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  XFlush(fl_display);
}

std::vector<std::string> keyboard_layouts(const Config &cfg) {
  std::vector<std::string> layouts;
  for (const char *key : {"keyboard_layout", "keyboard_layout2"}) {
    std::string v = trim(cfg.get("input", key));
    if (!v.empty() && std::find(layouts.begin(), layouts.end(), v) == layouts.end())
      layouts.push_back(v);
  }
  return layouts;
}

std::string upper(std::string s) {
  for (char &c : s) c = char(toupper(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

bool Panel::run_menu(const std::string &id, ContextMenu &menu, int root_x) {
  if (menu.empty()) return false;
  const Fl_Menu_Item *items = menu.items();
  qa_menu(id, items);
  const Fl_Menu_Item *m = popup_menu(items, root_x);
  if (!m || m->submenu()) return false;
  if (m->callback()) m->do_callback(this, m->user_data());
  return true;
}

const Fl_Menu_Item *Panel::popup_named(const std::string &id, const Fl_Menu_Item *items,
                                       int root_x) {
  qa_menu(id, items);
  return popup_menu(items, root_x);
}

void Panel::context_menu_for(const Target &t) {
  switch (t.kind) {
    case START:
      start_context_menu();
      break;
    case SEARCH:
      search_context_menu();
      break;
    case PINNED:
      pinned_menu(t.index);
      break;
    case TASK:
      task_menu(t.index);
      break;
    case STATUS:
      status_menu(t.index);
      break;
    case CLOCK:
      clock_menu();
      break;
    case NOTIFY:
      notifications_menu(false);
      break;
    case ACTION_CENTER:
      notifications_menu(true);
      break;
    default:
      show_context_menu();
      break;
  }
}

// Lock, Log Out and the power actions (Start button, power icon).
void Panel::power_items(ContextMenu &m) {
  m.item(_("Lock"), run({"sde-logout", "--action", "lock"}), {"system-lock-screen"});
  m.item(_("Log Out..."), run({"sde-logout"}), {"system-log-out"});
  ContextMenu power;
  power.item(_("Sleep"), run({"sde-logout", "--action", "suspend"}), {"system-suspend"});
  power.item(_("Restart"), run({"sde-logout", "--action", "reboot"}), {"system-reboot"});
  power.item(_("Shut Down"), run({"sde-logout", "--action", "poweroff"}), {"system-shutdown"});
  m.submenu(_("Power"), std::move(power), "system-shutdown");
}

void Panel::start_context_menu() {
  ContextMenu m;
  m.item(_("Settings"), run({"sde-settings"}), {"settings"});
  m.item(_("System Monitor"), run({"sde-system-monitor"}), {"utilities-system-monitor"});
  m.item(_("Run Command..."), run({"sde-run"}), {"system-run"});
  m.item(_("Software Updater"), run({"sde-update"}), {"system-software-update"});
  m.separator();
  power_items(m);
  m.separator();
  m.item(_("Panel Settings..."), settings("panel"), {"preferences-system-windows"});
  run_menu("start", m, Fl::event_x_root());
}

// [panel] search=box|icon|hidden; Super still opens the Start menu, which
// searches as soon as one types.
void Panel::search_context_menu() {
  std::string mode = search_mode_ == 2 ? "box" : search_mode_ == 1 ? "icon" : "hidden";
  ContextMenu m;
  m.radio(_("Show Search Box"), mode == "box", [this] { set_setting("panel", "search", "box"); });
  m.radio(_("Show Search Icon Only"), mode == "icon",
          [this] { set_setting("panel", "search", "icon"); });
  m.radio(_("Hidden"), mode == "hidden", [this] { set_setting("panel", "search", "hidden"); });
  m.separator();
  m.item(_("Search Settings..."), settings("panel"), {"settings"});
  run_menu("search", m, Fl::event_x_root());
}

void Panel::show_context_menu() {
  std::string mode = search_mode_ == 2 ? "box" : search_mode_ == 1 ? "icon" : "hidden";
  bool locked = cfg_.get_bool("panel", "locked", false);
  ContextMenu m;
  m.item(_("Show Desktop"), [] { x11::set_showing_desktop(!x11::showing_desktop()); },
         {"user-desktop"});
  m.separator();
  // The way back for a hidden search.
  ContextMenu search;
  search.radio(_("Show Search Box"), mode == "box", [this] { set_setting("panel", "search", "box"); });
  search.radio(_("Show Search Icon Only"), mode == "icon",
               [this] { set_setting("panel", "search", "icon"); });
  search.radio(_("Hidden"), mode == "hidden", [this] { set_setting("panel", "search", "hidden"); });
  m.submenu(_("Search"), std::move(search), "find");
  m.check(_("Lock the Panel"), locked,
          [this, locked] { set_setting("panel", "locked", locked ? "false" : "true"); });
  m.separator();
  m.item(_("System Monitor"), run({"sde-system-monitor"}), {"utilities-system-monitor"});
  m.item(_("Panel Settings..."), settings("panel"), {"preferences-system-windows"});
  m.separator();
  m.item(_("About SDE"), settings("about"), {"info"});
  run_menu("panel", m, Fl::event_x_root());
}

void Panel::pinned_menu(int index) {
  if (index < 0 || index >= int(pinned_.size())) return;
  DesktopEntry e = pinned_[size_t(index)].entry;
  bool locked = cfg_.get_bool("panel", "locked", false);
  auto move = [this, index](int other) {
    return [this, index, other] {
      if (index >= int(pinned_.size()) || other < 0 || other >= int(pinned_.size())) return;
      std::swap(pinned_[size_t(index)], pinned_[size_t(other)]);
      save_pins();
      relayout();
      redraw();
    };
  };
  ContextMenu m;
  m.item(_("Open"), [e] { launch_entry(e); }, {"open", 0, true, true});
  m.item(_("Move Left"), move(index - 1), {"", 0, index > 0 && !locked});
  m.item(_("Move Right"), move(index + 1), {"", 0, index + 1 < int(pinned_.size()) && !locked, true});
  m.item(_("Unpin from Panel"), [this, id = e.id] { unpin(id); }, {"", 0, true});
  m.item(_("Properties"), [e] {
    std::string text = e.name;
    if (!e.comment.empty()) text += "\n" + e.comment;
    text += std::string("\n\n") + _("Command") + ": " + e.exec + "\n" + _("File") + ": " + e.path;
    fl_message_title(e.name.c_str());
    fl_message("%s", text.c_str());
  }, {"properties"});
  run_menu("pinned:" + e.id, m, Fl::event_x_root());
}

void Panel::task_menu(int index) {
  if (index < 0 || index >= int(tasks_.size())) return;
  const Task &t = tasks_[size_t(index)];
  Window win = t.win;
  bool minimized = t.minimized;
  bool maximized = x11::window_has_state(win, "_NET_WM_STATE_MAXIMIZED_VERT") &&
                   x11::window_has_state(win, "_NET_WM_STATE_MAXIMIZED_HORZ");
  DesktopEntry entry;
  bool has_entry = find_entry_for(t, entry);
  bool pinned = has_entry && is_pinned(entry.id);
  // The other windows of the same application.
  std::vector<std::pair<Window, std::string>> windows;
  for (const auto &o : tasks_)
    if (o.win == win || (!t.wm_class.empty() && o.wm_class == t.wm_class))
      windows.push_back({o.win, printable_text(o.title.empty() ? o.wm_class : o.title)});
  ContextMenu m;
  if (windows.size() > 1) {
    for (const auto &w : windows)
      m.radio(w.second, w.first == win, [id = w.first] { x11::activate_window(id); });
    m.separator();
  }
  if (has_entry) {
    m.item(_("New Window"), [entry] { launch_entry(entry); }, {"new"});
    if (pinned)
      m.item(_("Unpin from Panel"), [this, id = entry.id] { unpin(id); }, {"", 0, true, true});
    else
      m.item(_("Pin to Panel"), [this, id = entry.id] { pin(id); }, {"", 0, true, true});
  }
  m.item(minimized ? _("Restore") : _("Minimize"), [win, minimized] {
    if (minimized)
      x11::activate_window(win);
    else
      x11::minimize_window(win);
  });
  m.item(maximized ? _("Restore Size") : _("Maximize"), [win] { x11::toggle_maximize(win); });
  Window root = RootWindow(fl_display, fl_screen);
  auto count = x11::get_cardinals(root, "_NET_NUMBER_OF_DESKTOPS");
  if (!count.empty() && count[0] > 1) {
    auto names = split(x11::get_utf8(root, "_NET_DESKTOP_NAMES"), 0, true);
    auto current = x11::get_cardinals(win, "_NET_WM_DESKTOP");
    ContextMenu ws;
    for (unsigned long i = 0; i < count[0] && i < 32; ++i) {
      std::string name = i < names.size() && !trim(names[i]).empty()
                             ? names[i]
                             : format(_("Workspace %d"), int(i + 1));
      ws.radio(name, !current.empty() && current[0] == i, [win, i] { send_desktop(win, long(i)); });
    }
    m.submenu(_("Move to Workspace"), std::move(ws));
  }
  m.separator();
  m.item(_("Close Window"), [win] { x11::close_window(win); }, {"close"});
  if (windows.size() > 1) {
    std::vector<Window> all;
    for (const auto &w : windows) all.push_back(w.first);
    m.item(_("Close All Windows"), [all] {
      for (Window w : all) x11::close_window(w);
    });
  }
  run_menu("task:" + (t.wm_class.empty() ? std::string("window") : t.wm_class), m,
           Fl::event_x_root());
}

void Panel::status_menu(int index) {
  if (index < 0 || index >= int(status_icons_.size())) return;
  StatusIcon icon = status_icons_[size_t(index)];
  auto &s = SystemStatus::get();
  auto &c = PanelControls::get();
  std::string section = status_section(icon);
  ContextMenu m;
  switch (icon) {
    case NETWORK_STATUS: {
      if (s.network.has_wifi)
        m.check(_("Wi-Fi"), s.network.wifi_enabled,
                [on = s.network.wifi_enabled] { SystemStatus::get().set_wifi_enabled(!on); });
      m.check(_("Airplane Mode"), c.airplane,
              [on = c.airplane] { PanelControls::get().wireless_off(!on); });
      m.separator();
      m.item(_("Troubleshoot"), [] { SystemStatus::get().troubleshoot_network(); }, {"refresh"});
      m.item(_("Network Settings..."), settings("network"), {"settings"});
      break;
    }
    case SOUND_STATUS: {
      m.check(_("Mute"), s.audio.muted, [] { SystemStatus::get().toggle_mute(); });
      m.item(_("Open Mixer"), [this] {
        if (!find_program("pavucontrol").empty())
          spawn_detached({"pavucontrol"});
        else
          open_flyout("sound");
      });
      m.separator();
      m.item(_("Sound Settings..."), settings("sound"), {"settings"});
      break;
    }
    case BLUETOOTH_STATUS: {
      const auto *a = c.adapter();
      bool on = a && a->powered;
      m.check(_("Bluetooth"), on, [on] { PanelControls::get().bluetooth_power(!on); }, a != nullptr);
      m.item(_("Show Devices"), [this] { open_flyout("bluetooth"); });
      m.separator();
      m.item(_("Bluetooth Settings..."), settings("bluetooth"), {"settings"});
      break;
    }
    case BATTERY_STATUS: {
      m.item(format(s.battery.charging ? _("Battery %d%% · charging") : _("Battery %d%%"),
                    s.battery.percent),
             nullptr, {"", 0, false, true});
      c.refresh_power_profiles();
      struct Profile {
        const char *id, *label;
      };
      static const Profile profiles[] = {{"performance", N_("Performance")},
                                         {"balanced", N_("Balanced")},
                                         {"power-saver", N_("Power Saver")}};
      for (const auto &p : profiles) {
        if (!c.power_profiles.empty() &&
            std::find(c.power_profiles.begin(), c.power_profiles.end(), p.id) == c.power_profiles.end())
          continue;
        m.radio(_(p.label), c.power_profile == p.id,
                [id = std::string(p.id)] { PanelControls::get().set_power_profile(id); },
                !c.power_profiles.empty());
      }
      m.separator();
      m.item(_("Power Settings..."), settings("power"), {"settings"});
      break;
    }
    case POWER_STATUS:
      power_items(m);
      break;
    case INPUT_STATUS: {
      auto layouts = keyboard_layouts(cfg_);
      for (const auto &l : layouts) {
        std::string up = upper(l);
        m.radio(up, up == c.keyboard, [this, layouts, l, up] {
          // The chosen layout first; Alt+Shift still switches to the other.
          std::string other;
          for (const auto &o : layouts)
            if (o != l) other = o;
          run_async({"setxkbmap", "-layout", other.empty() ? l : l + "," + other, "-option", "",
                     "-option", "grp:alt_shift_toggle"},
                    nullptr);
          PanelControls::get().keyboard = up;
          set_settings({{"input", "keyboard_layout", l}, {"input", "keyboard_layout2", other}});
        });
      }
      if (!layouts.empty()) m.separator();
      m.item(_("Keyboard Settings..."), settings("input"), {"settings"});
      break;
    }
    case DRIVE_STATUS: {
      for (const auto &d : c.drives)
        m.item(format(_("Open %s"), d.label.empty() ? d.name.c_str() : d.label.c_str()),
               [name = d.name] { PanelControls::get().drive_open(name); }, {"folder"});
      m.separator();
      std::vector<std::string> names;
      for (const auto &d : c.drives) names.push_back(d.name);
      m.item(_("Eject All"), [names] {
        for (const auto &n : names) PanelControls::get().drive_eject(n, true);
      }, {"", 0, !names.empty()});
      m.item(_("Open Liria"), run({"liria"}), {"folder"});
      break;
    }
    case TOUCHPAD_STATUS: {
      bool on = TouchpadMonitor::get().enabled();
      m.check(_("Touchpad"), on, [on] { TouchpadMonitor::get().set_enabled(!on); });
      m.separator();
      m.item(_("Touchpad Settings..."), settings("touchpad"), {"settings"});
      break;
    }
    case DND_STATUS:
      m.item(_("Turn Off Do Not Disturb"), [this] { set_do_not_disturb(false); });
      m.item(_("Notification Settings..."), settings("notifications"), {"settings"});
      break;
    case OVERFLOW_STATUS: {
      std::string mode = cfg_.get("panel", "tray_overflow", "auto");
      m.radio(_("Gather Tray Icons Automatically"), mode != "always" && mode != "never",
              [this] { set_setting("panel", "tray_overflow", "auto"); });
      m.radio(_("Always Gather Tray Icons"), mode == "always",
              [this] { set_setting("panel", "tray_overflow", "always"); });
      m.radio(_("Keep Tray Icons on the Panel"), mode == "never",
              [this] { set_setting("panel", "tray_overflow", "never"); });
      m.separator();
      m.item(_("Panel Settings..."), settings("panel"), {"settings"});
      break;
    }
    case MEDIA_STATUS:
    case CONTROLS_STATUS:
      m.item(_("Open Action Center"), [this] { open_action_center(); });
      m.item(_("Action Center Settings..."), settings("action-center"), {"settings"});
      break;
  }
  if (section.empty()) section = "icon-" + std::to_string(int(icon));
  run_menu("status:" + section, m, Fl::event_x_root());
}

void Panel::clock_menu() {
  bool seconds = cfg_.get_bool("panel", "clock_seconds", false);
  bool h24 = cfg_.get_bool("panel", "clock_24h", true);
  bool date = cfg_.get_bool("panel", "show_date", true);
  std::string style = cfg_.get("action_center", "clock_style", "digital");
  ContextMenu m;
  // Settings > Clock & Weather (alpha52).
  m.item(_("Adjust Date and Time..."), settings("clock"), {"settings"});
  m.item(_("Sync Time Zone"), [] {
    const auto &n = SystemStatus::get().network;
    tzsync::Sync::get().start(!n.available || (n.connected() && !n.no_internet()));
  }, {"refresh", 0, !tzsync::Sync::get().busy()});
  m.separator();
  ContextMenu styles;
  auto set_style = [this](const char *v) {
    return [this, v] { set_setting("action_center", "clock_style", v); };
  };
  styles.radio(_("Digital"), style != "led" && style != "analog" && style != "blink",
               set_style("digital"));
  styles.radio(_("Blink"), style == "blink", set_style("blink"));
  styles.radio(_("LED"), style == "led", set_style("led"));
  styles.radio(_("Analog"), style == "analog", set_style("analog"));
  m.submenu(_("Clock Style"), std::move(styles));
  m.check(_("Show Seconds"), seconds,
          [this, seconds] { set_setting("panel", "clock_seconds", seconds ? "false" : "true"); });
  m.check(_("24-Hour Clock"), h24,
          [this, h24] { set_setting("panel", "clock_24h", h24 ? "false" : "true"); });
  m.check(_("Show the Date"), date,
          [this, date] { set_setting("panel", "show_date", date ? "false" : "true"); });
  m.separator();
  m.item(_("Alarms"), [this] { open_alarms(); });
  m.item(_("Calendar"), [this] { open_action_center(true); });
  run_menu("clock", m, Fl::event_x_root());
}

void Panel::notifications_items(ContextMenu &m) {
  bool dnd = cfg_.get_bool("notifications", "do_not_disturb", false);
  int total = NotifyCenter::get().total();
  m.check(_("Do Not Disturb"), dnd, [this, dnd] { set_do_not_disturb(!dnd); });
  m.item(_("Clear All Notifications"), [] { NotifyCenter::get().clear(); }, {"", 0, total > 0});
  m.item(_("Notification Settings..."), settings("notifications"), {"settings"});
}

void Panel::notifications_menu(bool action_center) {
  ContextMenu m;
  if (action_center) m.item(_("Open Action Center"), [this] { open_action_center(); }, {"", 0, true, true});
  notifications_items(m);
  if (action_center) {
    m.separator();
    m.item(_("Action Center Settings..."), settings("action-center"), {"settings"});
  }
  run_menu(action_center ? "action-center" : "notifications", m, Fl::event_x_root());
}
