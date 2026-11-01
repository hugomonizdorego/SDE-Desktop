// SPDX-License-Identifier: GPL-3.0-or-later
// The Action Center's session buttons (0.2.0 Alpha 3.1): Sleep, Log Out,
// Restart and Shut Down, a row of round icon buttons that is always shown
// in the footer, next to the user, Control Center and Lock Screen
// (docs/PANEL.md, "Session buttons").
//
// The rules have no display and are unit tested (tests/test_session_actions.cpp).
// The glyphs are drawn with cairo on the 24-unit grid of the panel's tray
// icons (one stroke weight, round caps), so they stay sharp at every scale
// and take the theme's colours.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "paint.h"

namespace session_actions {

enum class Kind { SLEEP, LOGOUT, RESTART, SHUTDOWN };

struct Action {
  Kind kind;
  const char *id;       // sde-logout --action <id>: suspend, logout, reboot, poweroff
  const char *name;     // N_("Sleep"), ...: the tooltip and the accessible name
  const char *confirm;  // N_("Click again to shut down"); nullptr: acts at once
  const char *logind;   // the org.freedesktop.login1.Manager method that tells
                        // whether it is possible ("CanSuspend"); nullptr: always
  bool destructive() const { return confirm != nullptr; }
};
// Sleep, Log Out, Restart, Shut Down, in this order.
const std::vector<Action> &all();
const Action &of(Kind kind);

// Whether an action is offered for logind's answer to its Can* method:
// "yes" and "challenge" (polkit asks for a password first) are, "no"
// (polkit refuses) and "na" (the system cannot, e.g. no suspend support)
// are not. "" is no answer (logind not running, no system bus): the button
// stays, as before 0.2.0 Alpha 3.1.
bool available(const std::string &answer);

// The protection against accidental clicks (docs/PANEL.md): the first
// press of Log Out, Restart or Shut Down arms that button for kArmSeconds
// (it turns red, a ring runs down and the footer says "Click again to shut
// down"); a second press of the same button in that time runs it. A press
// within kSettleSeconds of arming (a double click, a touchpad tap counted
// twice) is ignored. Sleep runs at once and disarms.
constexpr double kArmSeconds = 4.0;
constexpr double kSettleSeconds = 0.4;
enum class Press { RUN, ARM, IGNORE };
struct Arming {
  int armed = -1;  // index into all(); -1: none
  double since = 0;
  bool active(double now) const { return armed >= 0 && now - since < kArmSeconds; }
  // The share of the time that is left: 1 when just armed, 0 at the end.
  double left(double now) const;
  Press press(int index, double now);
  void reset() { armed = -1; }
};

// The glyph of |kind| in a |size| square at |x|, |y|.
void draw(sde::Painter &p, Kind kind, double x, double y, double size, const sde::Color &c);
void draw(cairo_t *cr, Kind kind, double x, double y, double size, const sde::Color &c);

// What systemd-logind says the system can do. Asked asynchronously on the
// system bus when the panel starts and whenever the Action Center opens.
class Capabilities {
 public:
  static Capabilities &get();
  // Asks again; |changed| runs (from the event loop) when an answer changed.
  void refresh(std::function<void()> changed);
  const std::string &answer(Kind kind) const { return answers_[int(kind)]; }
  bool shown(Kind kind) const { return available(answer(kind)); }
  // The answers in one string (the Action Center's rebuild signature, QA).
  std::string signature() const;

 private:
  std::string answers_[4];
};

}  // namespace session_actions
