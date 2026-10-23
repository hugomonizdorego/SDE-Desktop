// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// sde-power: the SDE power manager.
//
//  * battery: UPower DisplayDevice; low and critical notifications
//    (org.freedesktop.Notifications) and the critical action
//  * lid and power button: systemd-logind inhibitors (handle-lid-switch,
//    handle-power-key) when SDE handles them, UPower LidIsClosed
//  * idle: dim the screen, turn it off (DPMS) and suspend after the
//    [power] timeouts, separately on battery and AC; screen saver
//    inhibitors (org.freedesktop.ScreenSaver, held by sde-lock) and logind
//    "idle" inhibitors stop it
//  * brightness keys (XF86MonBrightnessUp/Down) with an on-screen display
//  * power profiles (power-profiles-daemon) per power source
//
// Locking before suspend is done by sde-lock (logind PrepareForSleep).
// D-Bus: org.debion.SDE.Power at /org/debion/SDE/Power on the session bus.
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/XF86keysym.h>
#include <X11/XKBlib.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "crash.h"
#include "dbus.h"
#include "i18n.h"
#include "osd.h"
#include "policy.h"
#include "power.h"
#include "session.h"
#include "spawn.h"
#include "theme.h"
#include "util.h"
#include "x11.h"

using namespace sde;
using namespace sde::power;

namespace {

const char *kName = "org.debion.SDE.Power";
const char *kPath = "/org/debion/SDE/Power";
const char *kUPower = "org.freedesktop.UPower";
const char *kUPowerPath = "/org/freedesktop/UPower";
const char *kDisplayDevice = "/org/freedesktop/UPower/devices/DisplayDevice";

struct State {
  Config cfg;
  Settings set;
  dbus::Bus *session = nullptr;
  dbus::Bus *system = nullptr;

  // UPower
  bool upower = false;
  bool on_battery = false;
  bool battery_present = false;
  double percent = -1;
  int battery_state = 0;  // 2 discharging
  long time_to_empty = 0;
  bool lid_closed = false;
  bool have_profile_source = false;
  bool last_on_battery = false;
  BatteryLevel level = BatteryLevel::Normal;
  uint32_t battery_note = 0;
  bool critical_pending = false;

  // logind
  int inhibit_fd = -1;
  std::string inhibited_what;

  // idle
  IdleStage stage = IdleStage::Active;
  int dim_saved = -1, dim_set = -1;
  bool idle_inhibited = false;
  bool suspended_for_idle = false;
  long last_idle = 0;

  bool grabbed_power_key = false;
} g;

// ------------------------------------------------------------ idle

const IdleTimeouts &timeouts() { return g.on_battery ? g.set.battery : g.set.ac; }

void restore_brightness() {
  if (g.dim_saved < 0) return;
  // Leave the brightness alone when the user changed it while dimmed.
  int now = brightness_percent();
  if (now >= 0 && std::abs(now - g.dim_set) <= 2) set_brightness_percent(g.dim_saved);
  g.dim_saved = g.dim_set = -1;
}

void enter_stage(IdleStage stage) {
  if (stage == g.stage) return;
  IdleStage old = g.stage;
  g.stage = stage;
  if (stage == IdleStage::Active) {
    restore_brightness();
    g.suspended_for_idle = false;
    return;
  }
  if (stage >= IdleStage::Dim && old < IdleStage::Dim && g.dim_saved < 0) {
    int now = brightness_percent();
    if (now > 0) {
      int target = std::max(1, now * g.set.dim_level / 100);
      if (target < now && set_brightness_percent(target)) {
        g.dim_saved = now;
        g.dim_set = brightness_percent();
      }
    }
  }
  if (stage >= IdleStage::Blank && old < IdleStage::Blank) session::monitors_off();
  if (stage == IdleStage::Suspend && !g.suspended_for_idle) {
    g.suspended_for_idle = true;
    session::logind_action("Suspend");
  }
}

void query_inhibitors() {
  // sde-lock tracks org.freedesktop.ScreenSaver.Inhibit callers (video
  // players, presentations).
  if (g.session) {
    DBusMessage *m = dbus::method("org.debion.SDE.ScreenLock", "/org/debion/SDE/ScreenLock",
                                  "org.debion.SDE.ScreenLock", "IsIdleInhibited");
    g.session->call_async(m, [](DBusMessage *reply) {
      auto a = dbus::read_args(reply);
      bool lock_inhibit = !a.empty() && a[0].boolean();
      if (!g.system) {
        g.idle_inhibited = lock_inhibit;
        return;
      }
      // systemd-inhibit --what=idle
      DBusMessage *p = dbus::method("org.freedesktop.login1", "/org/freedesktop/login1",
                                    "org.freedesktop.DBus.Properties", "Get");
      dbus::Writer(p).str("org.freedesktop.login1.Manager").str("BlockInhibited");
      g.system->call_async(p, [lock_inhibit](DBusMessage *r) {
        auto b = dbus::read_args(r);
        std::string what = b.empty() ? "" : b[0].str();
        bool idle = false;
        for (auto &w : split(what, ':')) idle |= w == "idle";
        g.idle_inhibited = lock_inhibit || idle;
      }, 1000);
    }, 1000);
  }
}

void idle_tick(void *) {
  long idle = session::idle_ms();
  double next = 5.0;
  if (idle >= 0) {
    const IdleTimeouts &t = timeouts();
    // Ask about inhibitors shortly before a stage would start.
    long first = -1;
    for (int s : {t.dim, t.blank, t.suspend})
      if (s > 0 && (first < 0 || s < first)) first = s;
    if (first > 0 && idle + 6000 >= first * 1000L) query_inhibitors();
    IdleStage want = g.idle_inhibited ? IdleStage::Active : idle_stage(idle, t);
    if (idle < g.last_idle) want = std::min(want, idle_stage(idle, t));
    enter_stage(want);
    g.last_idle = idle;
    long until = next_stage_in(idle, t);
    if (g.stage != IdleStage::Active)
      next = 0.25;  // notice input quickly to undim
    else if (until >= 0)
      next = std::max(0.25, std::min(5.0, until / 1000.0 + 0.05));
  }
  Fl::repeat_timeout(next, idle_tick);
}

// ------------------------------------------------------------ actions

void run_action(Action a, const char *why) {
  switch (a) {
    case Action::Suspend: session::logind_action("Suspend"); break;
    case Action::Hibernate:
      if (session::logind_can("Hibernate") == "yes")
        session::logind_action("Hibernate");
      else
        session::logind_action("PowerOff");
      break;
    case Action::PowerOff: session::logind_action("PowerOff"); break;
    case Action::Lock: spawn_detached({"sde-lock", "--lock"}); break;
    case Action::Blank: session::monitors_off(); break;
    case Action::Interactive: spawn_detached({"sde-logout"}); break;
    case Action::Nothing:
    case Action::Logind: break;
  }
  if (a != Action::Nothing && a != Action::Logind)
    fprintf(stderr, "sde-power: %s: %s\n", why, action_name(a));
}

// ------------------------------------------------------------ battery

std::string time_text(long seconds) {
  if (seconds <= 0) return "";
  long minutes = (seconds + 59) / 60;
  if (minutes < 60) return format(_("About %ld minutes remaining."), minutes);
  return format(_("About %ld h %02ld min remaining."), minutes / 60, minutes % 60);
}

void critical_cb(void *) {
  g.critical_pending = false;
  if (g.level == BatteryLevel::Critical && g.on_battery)
    run_action(g.set.critical_action, "critical battery");
}

void evaluate_battery() {
  bool discharging = g.battery_present && g.on_battery;
  BatteryLevel level = battery_level(g.percent, discharging, g.level, g.set.battery_low,
                                     g.set.battery_critical);
  BatteryLevel old = g.level;
  g.level = level;
  if (level == old) return;
  if (level == BatteryLevel::Normal) {
    Fl::remove_timeout(critical_cb);
    g.critical_pending = false;
    return;
  }
  int pct = int(std::lround(g.percent));
  if (level == BatteryLevel::Low && old == BatteryLevel::Normal && g.set.notify) {
    session::notify(_("SDE Power"), "battery-caution", format(_("Battery low: %d%%"), pct),
                    time_text(g.time_to_empty) + " " + _("Connect the charger soon."), 1,
                    g.battery_note, [](uint32_t id) { g.battery_note = id; });
  }
  if (level == BatteryLevel::Critical) {
    std::string action;
    switch (g.set.critical_action) {
      case Action::Suspend: action = _("The computer will suspend in 30 seconds."); break;
      case Action::Hibernate: action = _("The computer will hibernate in 30 seconds."); break;
      case Action::PowerOff: action = _("The computer will shut down in 30 seconds."); break;
      default: action = _("Save your work and connect the charger."); break;
    }
    // A critical warning is always shown, even with notify_battery=false.
    session::notify(_("SDE Power"), "battery-empty", format(_("Battery critically low: %d%%"), pct),
                    action, 2, g.battery_note, [](uint32_t id) { g.battery_note = id; });
    if (!g.critical_pending && g.set.critical_action != Action::Nothing) {
      g.critical_pending = true;
      Fl::add_timeout(30.0, critical_cb);
    }
  }
}

void apply_profile() {
  const std::string &want = g.on_battery ? g.set.profile_battery : g.set.profile_ac;
  if (want.empty()) return;
  Profiles p = power_profiles();
  if (p.available && p.active != want &&
      std::find(p.names.begin(), p.names.end(), want) != p.names.end())
    set_power_profile(want);
}

void lid_changed(bool closed);

void read_upower() {
  if (!g.system) return;
  dbus::get_all_properties(g.system, kUPower, kUPowerPath, kUPower, [](const dbus::Value &v) {
    if (!v.valid()) return;
    g.upower = true;
    bool was_battery = g.on_battery;
    if (auto *b = v.get("OnBattery")) g.on_battery = b->boolean();
    if (auto *l = v.get("LidIsClosed")) {
      bool closed = l->boolean();
      if (closed != g.lid_closed) lid_changed(closed);
    }
    if (!g.have_profile_source || was_battery != g.on_battery) {
      g.have_profile_source = true;
      apply_profile();
    }
    evaluate_battery();
  });
  dbus::get_all_properties(g.system, kUPower, kDisplayDevice, "org.freedesktop.UPower.Device",
                           [](const dbus::Value &v) {
                             if (!v.valid()) return;
                             if (auto *p = v.get("IsPresent")) g.battery_present = p->boolean();
                             if (auto *p = v.get("Percentage")) g.percent = p->inner().d;
                             if (auto *s = v.get("State")) g.battery_state = int(s->num());
                             if (auto *t = v.get("TimeToEmpty")) g.time_to_empty = long(t->num());
                             evaluate_battery();
                           });
}

// ------------------------------------------------------------ lid and keys

void lid_changed(bool closed) {
  g.lid_closed = closed;
  if (!closed) {
    session::reset_idle();
    return;
  }
  if (!g.system) return;
  // logind knows whether an external monitor (or a dock) is connected.
  DBusMessage *m = dbus::method("org.freedesktop.login1", "/org/freedesktop/login1",
                                "org.freedesktop.DBus.Properties", "Get");
  dbus::Writer(m).str("org.freedesktop.login1.Manager").str("Docked");
  g.system->call_async(m, [](DBusMessage *reply) {
    auto a = dbus::read_args(reply);
    bool docked = !a.empty() && a[0].boolean();
    if (!g.lid_closed) return;
    run_action(lid_action(g.set, g.on_battery, docked), "lid closed");
  }, 1000);
}

// Takes the logind inhibitors for what SDE handles itself.
void update_inhibitor() {
  std::vector<std::string> what;
  if (g.set.lid_battery != Action::Logind || g.set.lid_ac != Action::Logind)
    what.push_back("handle-lid-switch");
  if (g.set.power_button != Action::Logind) what.push_back("handle-power-key");
  std::string joined;
  for (auto &w : what) joined += (joined.empty() ? "" : ":") + w;
  if (joined == g.inhibited_what && (g.inhibit_fd >= 0 || joined.empty())) return;
  if (g.inhibit_fd >= 0) close(g.inhibit_fd);
  g.inhibit_fd = -1;
  g.inhibited_what = joined;
  if (!joined.empty()) {
    g.inhibit_fd = session::logind_inhibit(joined, "SDE Power",
                                           "SDE handles the lid and the power button", "block");
    if (g.inhibit_fd < 0)
      fprintf(stderr, "sde-power: cannot take the logind inhibitor %s\n", joined.c_str());
    else
      fcntl(g.inhibit_fd, F_SETFD, FD_CLOEXEC);
  }
}

bool g_grab_failed = false;
int grab_error(Display *, XErrorEvent *e) {
  if (e->error_code == BadAccess) g_grab_failed = true;
  return 0;
}

void grab_key(KeySym sym, bool grab) {
  KeyCode code = XKeysymToKeycode(fl_display, sym);
  if (!code) return;
  Window root = RootWindow(fl_display, fl_screen);
  if (grab)
    XGrabKey(fl_display, code, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
  else
    XUngrabKey(fl_display, code, AnyModifier, root);
}

void update_key_grabs() {
  XSync(fl_display, False);
  auto old = XSetErrorHandler(grab_error);
  g_grab_failed = false;
  grab_key(XF86XK_MonBrightnessUp, true);
  grab_key(XF86XK_MonBrightnessDown, true);
  // The power key reaches X only while logind is inhibited.
  bool power = g.set.power_button != Action::Logind && g.inhibit_fd >= 0;
  if (power != g.grabbed_power_key) {
    grab_key(XF86XK_PowerOff, power);
    g.grabbed_power_key = power;
  }
  XSync(fl_display, False);
  XSetErrorHandler(old);
  if (g_grab_failed)
    fprintf(stderr, "sde-power: another program already handles some brightness/power keys\n");
}

int step_and_show(int steps) {
  int pct = step_brightness(steps, g.set.brightness_step);
  if (pct >= 0) {
    // The user chose a new level: do not "restore" an older one later.
    g.dim_saved = g.dim_set = -1;
    if (g.set.osd) show_osd(OsdKind::Brightness, pct);
  }
  return pct;
}

double monotonic() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + ts.tv_nsec / 1e9;
}

double g_last_power_key = 0;

void reload_config() {
  g.cfg.load();
  g.set = Settings::from_config(g.cfg);
  osd_reload_theme();
  update_inhibitor();
  update_key_grabs();
  evaluate_battery();
}

int x_events(int) {
  if (!fl_xevent) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == KeyPress) {
    KeySym sym = XkbKeycodeToKeysym(fl_display, KeyCode(e.xkey.keycode), 0, 0);
    if (sym == XF86XK_MonBrightnessUp) {
      step_and_show(1);
      return 1;
    }
    if (sym == XF86XK_MonBrightnessDown) {
      step_and_show(-1);
      return 1;
    }
    if (sym == XF86XK_PowerOff && g.grabbed_power_key) {
      // Ignore key repeat and bounces.
      double now = monotonic();
      if (now - g_last_power_key > 1.0) run_action(g.set.power_button, "power button");
      g_last_power_key = now;
      return 1;
    }
  }
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL"))
    reload_config();
  return 0;
}

// ------------------------------------------------------------ D-Bus

const char *kIntrospect =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node><interface name=\"org.debion.SDE.Power\">"
    "<method name=\"GetBrightness\"><arg type=\"i\" direction=\"out\"/></method>"
    "<method name=\"SetBrightness\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"out\"/></method>"
    "<method name=\"StepBrightness\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"out\"/></method>"
    "<method name=\"GetStatus\"><arg type=\"a{sv}\" direction=\"out\"/></method>"
    "<method name=\"Reload\"/>"
    "</interface></node>";

void add_entry(dbus::Writer &dict, const char *key, const std::function<void(dbus::Writer &)> &value) {
  dbus::Writer e(dict.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
  e.str(key);
  value(e);
}

DBusHandlerResult on_message(DBusMessage *msg) {
  if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *member = dbus_message_get_member(msg);
  if (!member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  std::string m = member;
  DBusMessage *r = dbus_message_new_method_return(msg);
  if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
    dbus::Writer(r).str(kIntrospect);
  } else if (m == "GetBrightness") {
    dbus::Writer(r).i32(brightness_percent());
  } else if (m == "SetBrightness" || m == "StepBrightness") {
    auto a = dbus::read_args(msg);
    int v = a.empty() ? 0 : int(a[0].num());
    int pct;
    if (m == "StepBrightness") {
      pct = step_and_show(std::max(-20, std::min(20, v)));
    } else {
      pct = set_brightness_percent(std::max(1, std::min(100, v))) ? brightness_percent() : -1;
      if (pct >= 0) {
        g.dim_saved = g.dim_set = -1;
        if (g.set.osd) show_osd(OsdKind::Brightness, pct);
      }
    }
    dbus::Writer(r).i32(pct);
  } else if (m == "GetStatus") {
    dbus::Writer w(r);
    dbus::Writer dict(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
    add_entry(dict, "on-battery", [](dbus::Writer &e) { e.variant_bool(g.on_battery); });
    add_entry(dict, "battery-present", [](dbus::Writer &e) { e.variant_bool(g.battery_present); });
    add_entry(dict, "battery-percent",
              [](dbus::Writer &e) { e.variant_i32(int(std::lround(g.percent))); });
    add_entry(dict, "battery-level", [](dbus::Writer &e) { e.variant_i32(int(g.level)); });
    add_entry(dict, "lid-closed", [](dbus::Writer &e) { e.variant_bool(g.lid_closed); });
    add_entry(dict, "idle-stage", [](dbus::Writer &e) { e.variant_i32(int(g.stage)); });
    add_entry(dict, "idle-inhibited", [](dbus::Writer &e) { e.variant_bool(g.idle_inhibited); });
    add_entry(dict, "logind-inhibited", [](dbus::Writer &e) { e.variant_str(g.inhibited_what); });
  } else if (m == "Reload") {
    reload_config();
  } else {
    dbus_message_unref(r);
    g.session->send(dbus::Bus::error(msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown method " + m));
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  g.session->send(r);
  return DBUS_HANDLER_RESULT_HANDLED;
}

void upower_changed_cb(void *) { read_upower(); }

int run_daemon() {
  fl_open_display();
  g.cfg.load();
  g.set = Settings::from_config(g.cfg);
  g.session = dbus::Bus::session();
  if (!g.session) {
    fprintf(stderr, "sde-power: no session bus\n");
    return 1;
  }
  if (!g.session->request_name(kName)) {
    fprintf(stderr, "sde-power: already running\n");
    return 0;
  }
  g.session->export_object(kPath, on_message);
  g.system = dbus::Bus::system();
  session::disable_x_timers();
  if (g.system) {
    g.system->add_signal_handler(
        "type='signal',sender='org.freedesktop.UPower',interface='org.freedesktop.DBus."
        "Properties',member='PropertiesChanged'",
        nullptr, [](DBusMessage *) {
          // Signals come in bursts: read the properties once.
          Fl::remove_timeout(upower_changed_cb);
          Fl::add_timeout(0.2, upower_changed_cb);
        });
    read_upower();
  }
  update_inhibitor();
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  update_key_grabs();
  Fl::add_handler(x_events);
  Fl::add_timeout(1.0, idle_tick);
  for (;;) Fl::wait(1e20);
}

void usage(FILE *out) {
  fprintf(out, "%s",
          _("Usage: sde-power [--daemon]\n"
            "       sde-power --brightness [get | up | down | PERCENT]\n"
            "       sde-power --profile [get | list | NAME]\n"
            "       sde-power --status | --osd [PERCENT] | --version\n\n"
            "The SDE power manager: battery warnings, lid and power button, idle\n"
            "dimming, screen blanking and suspend, brightness keys. Settings are\n"
            "in sde.conf [power] (Settings > Power).\n"));
}

// Asks the running daemon first, so that it shows the OSD.
int brightness_command(const std::string &arg) {
  if (arg == "get" || arg.empty()) {
    int v = brightness_percent();
    if (v < 0) {
      fprintf(stderr, "sde-power: %s\n", _("no backlight found"));
      return 1;
    }
    printf("%d\n", v);
    return 0;
  }
  bool step = arg == "up" || arg == "down";
  int value = step ? (arg == "up" ? 1 : -1) : atoi(arg.c_str());
  if (!step && (value < 1 || value > 100)) {
    usage(stderr);
    return 2;
  }
  if (dbus::Bus *bus = dbus::Bus::session()) {
    DBusMessage *m = dbus::method(kName, kPath, kName, step ? "StepBrightness" : "SetBrightness");
    dbus::Writer(m).i32(value);
    if (DBusMessage *reply = bus->call(m, 3000)) {
      auto a = dbus::read_args(reply);
      dbus_message_unref(reply);
      int v = a.empty() ? -1 : int(a[0].num());
      if (v >= 0) printf("%d\n", v);
      return v >= 0 ? 0 : 1;
    }
  }
  Config cfg;
  cfg.load();
  int v = step ? step_brightness(value, Settings::from_config(cfg).brightness_step)
               : (set_brightness_percent(value) ? brightness_percent() : -1);
  if (v < 0) {
    fprintf(stderr, "sde-power: %s\n", _("cannot change the brightness"));
    return 1;
  }
  printf("%d\n", v);
  return 0;
}

int profile_command(const std::string &arg) {
  Profiles p = power_profiles();
  if (!p.available) {
    fprintf(stderr, "sde-power: %s\n", _("power-profiles-daemon is not running"));
    return 1;
  }
  if (arg.empty() || arg == "get") {
    printf("%s\n", p.active.c_str());
    return 0;
  }
  if (arg == "list") {
    for (auto &n : p.names) printf("%s%s\n", n == p.active ? "* " : "  ", n.c_str());
    return 0;
  }
  return set_power_profile(arg) ? 0 : 1;
}

int status_command() {
  dbus::Bus *bus = dbus::Bus::session();
  DBusMessage *reply = bus ? bus->call(dbus::method(kName, kPath, kName, "GetStatus"), 2000) : nullptr;
  if (!reply) {
    fprintf(stderr, "sde-power: %s\n", _("the power manager is not running"));
    return 1;
  }
  auto a = dbus::read_args(reply);
  dbus_message_unref(reply);
  if (!a.empty())
    for (auto &e : a[0].items)
      if (e.items.size() == 2) {
        const dbus::Value &v = e.items[1].inner();
        printf("%s=%s\n", e.items[0].s.c_str(),
               v.is_string() ? v.s.c_str() : std::to_string(v.num()).c_str());
      }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  install_crash_handler("sde-power");
  std::string cmd = argc > 1 ? argv[1] : "--daemon";
  std::string arg = argc > 2 ? argv[2] : "";
  if (cmd == "--version") return print_version("sde-power", SDE_APP_VERSION);
  if (cmd == "--help" || cmd == "-h") {
    usage(stdout);
    return 0;
  }
  if (cmd == "--daemon") return run_daemon();
  if (cmd == "--brightness") return brightness_command(arg);
  if (cmd == "--profile") return profile_command(arg);
  if (cmd == "--status") return status_command();
  if (cmd == "--osd") {
    // Shows the brightness OSD once (to check the theme).
    fl_open_display();
    int pct = arg.empty() ? brightness_percent() : atoi(arg.c_str());
    show_osd(OsdKind::Brightness, std::max(0, std::min(100, pct)), "", 3.0);
    Fl::add_timeout(3.2, [](void *) { exit(0); });
    for (;;) Fl::wait(1e20);
  }
  usage(stderr);
  return 2;
}
