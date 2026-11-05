// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
// Control Center > Touch Screen (0.2.0 Alpha 4, sde-settings --page touch,
// docs/TOUCH.md): the gestures of SDE applications, the display a touch
// screen drives, showing touches and the on-screen keyboard. The keys are
// [touchscreen] in sde.conf (src/common/touch.h); the display is applied at
// once with xinput, and again at login and when a monitor comes or goes.
#include <FL/Fl.H>

#include <string>
#include <vector>

#include "i18n.h"
#include "process.h"
#include "settings.h"
#include "spawn.h"
#include "touch.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {
std::vector<std::string> &connected() {
  static std::vector<std::string> outputs;
  return outputs;
}

void change(const std::function<void(TouchOptions &)> &edit) {
  TouchOptions o = load_touch_options(settings_config());
  edit(o);
  save_touch_options(settings_config(), o);
  apply_change();
}

// Shows or hides the touch points at once (sde-touch-overlay reads the
// setting itself and ends when it is turned off).
void start_overlay() {
  if (!find_program("sde-touch-overlay").empty()) spawn_detached({"sde-touch-overlay"});
}
}  // namespace

void build_touch_page(Page *page) {
  const TouchOptions o = load_touch_options(settings_config());
  std::vector<TouchScreen> screens = find_touch_screens();
  if (!find_program("xrandr").empty())
    connected() = parse_connected_outputs(run_sync({"xrandr", "--query"}, nullptr, 3000));
  page->heading(_("Touch Screen"), _("Gestures, display and on-screen keyboard"));
  if (screens.empty()) {
    page->note(_("No touch screen was found. These settings are saved and used when one is "
                 "connected."));
  } else {
    std::string names;
    for (const auto &s : screens) names += (names.empty() ? "" : ", ") + s.name;
    page->note(names);
  }

  page->begin_card(_("Gestures"));
  page->switch_row(_("Touch gestures"), o.gestures,
                   [](bool on) { change([on](TouchOptions &t) { t.gestures = on; }); },
                   _("In SDE applications a finger scrolls, a long press opens the context menu "
                     "and two fingers zoom. Off: a touch is a mouse click, as in older "
                     "applications."));
  int delay = o.long_press_ms <= 380 ? 0 : o.long_press_ms >= 700 ? 2 : 1;
  page->choice_row(_("Long press"), {_("Short"), _("Normal"), _("Long")}, delay, [](int i) {
    change([i](TouchOptions &t) { t.long_press_ms = i == 0 ? 350 : i == 2 ? 800 : 500; });
  }, _("How long to hold a finger still for the context menu."));
  page->switch_row(_("Show touches"), o.show_touches,
                   [](bool on) {
                     change([on](TouchOptions &t) { t.show_touches = on; });
                     if (on) start_overlay();
                   },
                   _("A circle follows each finger, for presentations and screen recordings."));
  page->end_card();

  // The display: Automatic, the whole desktop, or one of the connected ones.
  page->begin_card(_("Display"));
  std::vector<std::string> options = {_("Automatic (the built-in screen)"), _("The whole desktop")};
  int chosen = o.output == "all" ? 1 : 0;
  for (const auto &name : connected()) {
    options.push_back(name);
    if (name == o.output) chosen = int(options.size()) - 1;
  }
  page->choice_row(_("Touch screen drives"), options, chosen, [](int i) {
    change([i](TouchOptions &t) {
      t.output = i == 0 ? "auto" : i == 1 ? "all" : connected()[size_t(i - 2)];
    });
    apply_touch_mapping(settings_config());
  }, _("With several monitors, touches land on this one. Automatic: a laptop's own screen."));
  page->end_card();

  page->begin_card(_("On-screen keyboard"));
  if (onscreen_keyboard().empty()) {
    page->note(_("Install Onboard (the onboard package) to type without a keyboard."));
  } else {
    page->switch_row(_("Show the keyboard for text fields"), o.keyboard != "off",
                     [](bool on) { change([on](TouchOptions &t) { t.keyboard = on ? "auto" : "off"; }); },
                     _("Onboard appears when a finger taps a text field in an SDE application."));
    page->row(_("Onboard"), S(112), S(34), [](int x, int y, int w, int h) {
      auto *b = new FlatButton(x, y, w, h, _("Open"));
      b->callback([](Fl_Widget *, void *) { spawn_detached({"onboard"}); });
      return b;
    });
  }
  page->end_card();

  page->note(_("Firefox, GTK and Qt applications take the touches themselves; SDE starts them "
               "with what they need for it."));
  page->finish();
}

}  // namespace sde
