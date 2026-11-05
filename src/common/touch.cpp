// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch.h"

#include <FL/Fl.H>
#include <FL/Fl_Touch.H>
#include <FL/platform.H>
#include <X11/extensions/XInput2.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include "config.h"
#include "process.h"
#include "util.h"

namespace sde {

namespace {
int scroll_axis(int &pos, int max, int delta) {
  if (max < 0) max = 0;
  if (!delta) return 1;
  int old = pos;
  pos = std::max(0, std::min(max, pos + delta));
  return (delta > 0 && old < max) || (delta < 0 && old > 0);
}
}  // namespace

int touch_scroll_y(int &pos, int max) { return scroll_axis(pos, max, Fl::event_dy()); }
int touch_scroll_x(int &pos, int max) { return scroll_axis(pos, max, Fl::event_dx()); }

int touch_scroll_y(double &pos, double max) {
  int delta = Fl::event_dy();
  if (max < 0) max = 0;
  if (!delta) return 1;
  double old = pos;
  pos = std::max(0.0, std::min(max, pos + delta));
  return (delta > 0 && old < max) || (delta < 0 && old > 0);
}

int touch_scroll_rows(int &first, int max_first, int row_h) {
  if (max_first < 0) max_first = 0;
  int delta = Fl::event_dy();
  int can = !delta || (delta > 0 && first < max_first) || (delta < 0 && first > 0);
  int rows = Fl_Touch::scroll_units_y(std::max(1, row_h));
  first = std::max(0, std::min(max_first, first + rows));
  return can;
}

bool touch_event() { return Fl_Touch::event_is_touch(); }

TouchOptions load_touch_options(const Config &cfg) {
  TouchOptions o;
  o.gestures = cfg.get_bool("touchscreen", "gestures", true);
  o.show_touches = cfg.get_bool("touchscreen", "show_touches", false);
  o.output = trim(cfg.get("touchscreen", "output", "auto"));
  if (o.output.empty()) o.output = "auto";
  o.keyboard = cfg.get("touchscreen", "keyboard", "auto") == "off" ? "off" : "auto";
  o.long_press_ms = std::max(200, std::min(2000, cfg.get_int("touchscreen", "long_press", 500)));
  return o;
}

void save_touch_options(Config &cfg, const TouchOptions &o) {
  auto b = [](bool v) { return std::string(v ? "true" : "false"); };
  cfg.set("touchscreen", "gestures", b(o.gestures));
  cfg.set("touchscreen", "show_touches", b(o.show_touches));
  cfg.set("touchscreen", "output", o.output.empty() ? "auto" : o.output);
  cfg.set("touchscreen", "keyboard", o.keyboard == "off" ? "off" : "auto");
  cfg.set("touchscreen", "long_press", std::to_string(o.long_press_ms));
}

namespace {
bool g_keyboard_on = true;

// A tap gave the focus to a text field: show the on-screen keyboard.
// Onboard is asked over D-Bus (it starts hidden and stays running); it is
// started when it is not running yet.
void show_keyboard(Fl_Widget *) {
  if (!g_keyboard_on || onscreen_keyboard().empty()) return;
  run_async({"dbus-send", "--session", "--type=method_call", "--dest=org.onboard.Onboard",
             "/org/onboard/Onboard/Keyboard", "org.onboard.Onboard.Keyboard.Show"},
            [](const std::string &, int status) {
              if (status != 0) run_async({"onboard"}, nullptr);
            });
}
}  // namespace

void apply_touch_to_toolkit(const TouchOptions &o, double scale) {
  Fl_Touch::scale(scale);
  Fl_Touch::long_press_delay(o.long_press_ms / 1000.0);
  g_keyboard_on = o.keyboard != "off";
  Fl_Touch::keyboard_hook(show_keyboard);
  // SDE_TOUCH=0 in the environment wins (Fl_Touch reads it at start).
  const char *env = getenv("SDE_TOUCH");
  if (!(env && *env)) Fl_Touch::enable(o.gestures);
}

bool builtin_output(const std::string &name) {
  std::string n = lower(name);
  return starts_with(n, "edp") || starts_with(n, "lvds") || starts_with(n, "dsi");
}

std::string touch_output(const TouchOptions &o, const std::vector<std::string> &connected) {
  if (o.output == "all") return "";
  if (o.output != "auto") {
    // A display that is not connected: the whole desktop, until it is back.
    return std::find(connected.begin(), connected.end(), o.output) != connected.end() ? o.output : "";
  }
  // With one display there is nothing to choose; with more, a laptop's touch
  // screen is its built-in panel.
  if (connected.size() < 2) return "";
  for (const auto &n : connected)
    if (builtin_output(n)) return n;
  return "";
}

std::vector<TouchScreen> find_touch_screens(Display *dpy) {
  std::vector<TouchScreen> out;
  if (!dpy) {
    fl_open_display();
    dpy = fl_display;
  }
  int opcode, event, error;
  if (!dpy || !XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error)) return out;
  // Do not ask the server for a version lower than SDE-FLTK did (2.2).
  int major = 2, minor = 2;
  if (XIQueryVersion(dpy, &major, &minor) != Success || (major == 2 && minor < 2)) return out;
  int count = 0;
  XIDeviceInfo *devices = XIQueryDevice(dpy, XIAllDevices, &count);
  for (int i = 0; devices && i < count; ++i) {
    const XIDeviceInfo &d = devices[i];
    if (d.use != XISlavePointer && d.use != XIFloatingSlave) continue;
    for (int c = 0; c < d.num_classes; ++c) {
      if (d.classes[c]->type != XITouchClass) continue;
      auto *t = reinterpret_cast<XITouchClassInfo *>(d.classes[c]);
      if (t->mode != XIDirectTouch) continue;  // touchpads are dependent devices
      TouchScreen s;
      s.id = d.deviceid;
      s.name = d.name ? d.name : "";
      s.touches = t->num_touches;
      out.push_back(s);
      break;
    }
  }
  if (devices) XIFreeDeviceInfo(devices);
  return out;
}

std::vector<std::string> parse_connected_outputs(const std::string &text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == ' ' || line[0] == '\t') continue;
    std::istringstream words(line);
    std::string name, state;
    words >> name >> state;
    if (state == "connected") out.push_back(name);
  }
  return out;
}

int apply_touch_mapping(const Config &cfg, Display *dpy) {
  std::vector<TouchScreen> screens = find_touch_screens(dpy);
  if (screens.empty() || find_program("xinput").empty()) return int(screens.size());
  std::vector<std::string> connected;
  if (!find_program("xrandr").empty())
    connected = parse_connected_outputs(run_sync({"xrandr", "--query"}, nullptr, 3000));
  std::string output = touch_output(load_touch_options(cfg), connected);
  for (const auto &s : screens) {
    std::string id = std::to_string(s.id);
    if (!output.empty()) {
      run_sync({"xinput", "map-to-output", id, output}, nullptr, 3000);
    } else {
      // The whole desktop: the identity matrix.
      run_sync({"xinput", "set-prop", id, "Coordinate Transformation Matrix", "1", "0", "0", "0", "1",
                "0", "0", "0", "1"},
               nullptr, 3000);
    }
  }
  return int(screens.size());
}

std::string onscreen_keyboard() {
  return find_program("onboard").empty() ? std::string() : std::string("onboard");
}

}  // namespace sde
