// SPDX-License-Identifier: GPL-3.0-or-later
// Touch screens (0.2.0 Alpha 4, docs/TOUCH.md).
//
// SDE-FLTK turns touches into clicks, scrolls, context menus and zooms
// (sde-fltk/FL/Fl_Touch.H). This is SDE's side of it:
//
//  * helpers for widgets that scroll themselves: FL_SCROLL_GESTURE moves a
//    scroll position by the finger, within its limits, and says whether the
//    content could move that way (what the handler returns);
//  * the [touchscreen] settings of sde.conf: gestures on or off, the display
//    a touch screen drives, show touches, the on-screen keyboard, the long
//    press delay; apply_touch_to_toolkit() hands them to SDE-FLTK in every
//    program (Theme::apply_to_fltk() calls it);
//  * touch screens (XInput 2 devices with direct touch) and mapping them to
//    a display (xinput map-to-output): with several monitors a laptop's
//    touch screen drives its built-in panel, not the whole desktop.
//
// Keys: [touchscreen] gestures (true), output (auto, all or an output name
// such as eDP-1), show_touches (false), keyboard (auto: the on-screen
// keyboard when a text field is tapped, if one is installed; off),
// long_press (milliseconds, 500).
#pragma once

#include <string>
#include <vector>

typedef struct _XDisplay Display;

namespace sde {

class Config;

// FL_SCROLL_GESTURE (in handle()): moves |pos| (0 to |max|) by the event's
// Fl::event_dy() (or event_dx()). Returns 1 when the content could move that
// way (also when it reached the limit with this event), 0 when it is at its
// end or does not scroll: return that from handle().
int touch_scroll_y(int &pos, int max);
int touch_scroll_x(int &pos, int max);
int touch_scroll_y(double &pos, double max);
// For lists that scroll by rows: |first| (0 to |max_first|) moves by whole
// rows of |row_h| pixels; the pixels in between are kept for the next event.
int touch_scroll_rows(int &first, int max_first, int row_h);
// True while the event being handled comes from a touch screen.
bool touch_event();

struct TouchOptions {
  bool gestures = true;
  bool show_touches = false;
  std::string output = "auto";    // auto, all, or an output name
  std::string keyboard = "auto";  // auto or off
  int long_press_ms = 500;
};
TouchOptions load_touch_options(const Config &cfg);
void save_touch_options(Config &cfg, const TouchOptions &o);
// SDE-FLTK in this program: gestures on or off, the long press, the scale
// of the distances, the on-screen keyboard hook.
void apply_touch_to_toolkit(const TouchOptions &o, double scale);

// The built-in panel of a laptop (eDP, LVDS, DSI).
bool builtin_output(const std::string &name);
// The output a touch screen should drive with |o| when |connected| outputs
// are connected (in that order): an output name, or "" for the whole
// desktop (no mapping).
std::string touch_output(const TouchOptions &o, const std::vector<std::string> &connected);

struct TouchScreen {
  int id = 0;
  std::string name;
  int touches = 0;  // the most fingers it reports
};
// Touch screens (direct touch devices) on |dpy| (fl_display if null).
std::vector<TouchScreen> find_touch_screens(Display *dpy = nullptr);
// Connected outputs, from xrandr --query output.
std::vector<std::string> parse_connected_outputs(const std::string &xrandr_query);
// Maps every touch screen to the output touch_output() chooses (xinput
// map-to-output), or back to the whole desktop. Blocks for a moment: at
// login, when a monitor comes or goes, and from Control Center. Returns
// the number of touch screens.
int apply_touch_mapping(const Config &cfg, Display *dpy = nullptr);

// An on-screen keyboard that SDE can show: "onboard" when installed, else "".
std::string onscreen_keyboard();

// Panel and Start menu targets of at least 40 px at 100 % for fingers:
// [touchscreen] large_targets = auto (when a touch screen is connected,
// checked once per program), true or false; SDE_TOUCH_TARGETS=1 or 0 in the
// environment decides for tests and screenshots.
bool large_touch_targets(const Config &cfg);

}  // namespace sde
