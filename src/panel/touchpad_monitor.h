// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's touchpad indicator (alpha50).
//
// Touchpads are found with XInput 2 (slave pointers with libinput's
// "libinput Tapping Enabled" property) on a separate X connection, which
// also receives the raw events of the touchpad: button presses (clicks and
// taps) always, motion and scrolling only while the indicator animates
// ([panel] touchpad_indicator=always, while hovered or its flyout is open,
// and for 2 s after a click), so an idle panel does not wake up for every
// pointer movement. libinput reports relative motion, so the dot on the
// mini touchpad follows the finger's movement, not an absolute position.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "paint.h"
#include "touchpad.h"

class TouchpadMonitor {
 public:
  static TouchpadMonitor &get();
  // |activity|: something to animate; |changed|: devices or state changed.
  void start(std::function<void()> activity, std::function<void()> changed);
  bool present() const { return id_ != 0; }
  bool enabled() const { return enabled_; }
  const std::string &name() const { return name_; }
  int device_id() const { return id_; }

  // Indicator state, in [0, 1] touchpad coordinates.
  struct State {
    double x = 0.5, y = 0.5;
    long long moved_at = 0, click_at = 0, scroll_at = 0;
    int click = 0;          // 1 left, 2 middle (both halves), 3 right
    int scroll_dx = 0, scroll_dy = 0;  // last scroll direction
  };
  const State &state() const { return state_; }
  long long last_activity() const;
  // Motion and scroll events are wanted (hovered, flyout open, "always").
  void want_motion(bool on);

  // Changes and saves an option, then applies it to the touchpad.
  void set_enabled(bool on);
  void apply(const sde::TouchpadOptions &o);

 private:
  static void input(int, void *);
  void detect();
  void select();
  void handle_raw(int evtype, void *raw);
  struct _XDisplay *display_ = nullptr;
  int opcode_ = 0, id_ = 0;
  std::string name_;
  bool enabled_ = true, motion_ = false, motion_selected_ = false;
  int scroll_h_ = -1, scroll_v_ = -1;  // valuator numbers
  std::vector<std::string> props_;
  State state_;
  std::function<void()> activity_, changed_;
};

// A mini touchpad in the box (x, y, w, h): its outline and button line; with
// |live|, the finger's dot (for 2 s after it moved), the clicked half
// flashing and scroll arrows. Disabled: struck through.
void draw_touchpad(sde::Painter &p, double x, double y, double w, double h,
                   const TouchpadMonitor::State &s, bool enabled, bool live, const sde::Color &ink,
                   const sde::Color &accent);
