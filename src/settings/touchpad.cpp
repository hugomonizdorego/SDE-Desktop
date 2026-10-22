// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Touchpad (alpha50, sde-settings --page touchpad): libinput
// touchpad options saved in sde.conf (see common/touchpad.h) and applied at
// once through xinput; sde-settings --apply applies them again at login.
#include <FL/Fl_Choice.H>

#include "i18n.h"
#include "settings.h"
#include "touchpad.h"
#include "ui.h"
#include "widgets.h"

namespace sde {

namespace {
std::vector<TouchpadDevice> &pads() {
  static std::vector<TouchpadDevice> found;
  return found;
}
void change(const std::function<void(TouchpadOptions &)> &edit) {
  TouchpadOptions o = load_touchpad_options(settings_config());
  edit(o);
  save_touchpad_options(settings_config(), o);
  apply_change();
  apply_touchpad(settings_config(), pads());
}
}  // namespace

void build_touchpad_page(Page *page) {
  const TouchpadOptions o = load_touchpad_options(settings_config());
  pads() = find_touchpads();
  const auto &found = pads();
  page->heading(_("Touchpad"), _("Tapping, scrolling, clicking and pointer speed"));
  if (found.empty())
    page->note(_("No touchpad was found. These settings are saved and applied when one is "
                 "present at login."));
  else
    page->note(found.front().name);

  page->begin_card(_("General"));
  page->switch_row(_("Touchpad"), o.enabled,
                   [](bool on) { change([on](TouchpadOptions &t) { t.enabled = on; }); },
                   _("Turn the touchpad off when you use a mouse."));
  page->switch_row(_("Disable while typing"), o.disable_typing,
                   [](bool on) { change([on](TouchpadOptions &t) { t.disable_typing = on; }); });
  page->slider_row(_("Pointer speed"), -100, 100, o.speed, [](double v) {
    change([v](TouchpadOptions &t) { t.speed = int(v); });
  }, _("libinput acceleration: slower to the left, faster to the right."));
  page->end_card();

  page->begin_card(_("Tapping and clicking"));
  page->switch_row(_("Tap to click"), o.tap,
                   [](bool on) { change([on](TouchpadOptions &t) { t.tap = on; }); });
  page->switch_row(_("Tap and drag"), o.tap_drag,
                   [](bool on) { change([on](TouchpadOptions &t) { t.tap_drag = on; }); },
                   _("Tap, then touch again and move to drag."));
  page->choice_row(_("Click method"), {_("Button areas"), _("Fingers (two-finger right click)")},
                   o.click == "clickfinger" ? 1 : 0, [](int i) {
                     change([i](TouchpadOptions &t) { t.click = i == 1 ? "clickfinger" : "areas"; });
                   });
  page->end_card();

  page->begin_card(_("Scrolling"));
  page->choice_row(_("Scrolling"), {_("Two fingers"), _("Edge"), _("Off")},
                   o.scroll == "edge" ? 1 : o.scroll == "none" ? 2 : 0, [](int i) {
                     change([i](TouchpadOptions &t) {
                       t.scroll = i == 1 ? "edge" : i == 2 ? "none" : "two-finger";
                     });
                   });
  page->switch_row(_("Natural scrolling"), o.natural,
                   [](bool on) { change([on](TouchpadOptions &t) { t.natural = on; }); },
                   _("Content moves with your fingers."));
  page->end_card();
  page->finish();
}

}  // namespace sde
