// SPDX-License-Identifier: GPL-3.0-or-later
// Settings > Keyboard & Mouse: layouts, key repeat, pointer speed, scrolling
// and touchpad options (setxkbmap, xset, xinput/libinput).
#include <algorithm>

#include "i18n.h"
#include "process.h"
#include "settings.h"
#include "system.h"
#include "ui.h"
#include "util.h"

namespace sde {

namespace {

std::vector<std::pair<std::string, std::string>> g_layouts;

std::string current_layout() {
  std::string configured = settings_config().get("input", "keyboard_layout");
  if (!configured.empty()) return configured;
  std::string q = run_sync({"setxkbmap", "-query"});
  size_t p = q.find("layout:");
  if (p == std::string::npos) return "us";
  std::string v = trim(q.substr(p + 7, q.find('\n', p) - p - 7));
  return v.substr(0, v.find(','));
}

void save_and_apply_keyboard() {
  apply_change();
  apply_keyboard(settings_config());
}

void save_and_apply_pointers() {
  apply_change();
  apply_pointers(settings_config());
}

}  // namespace

void build_input_page(Page *page) {
  Config &cfg = settings_config();
  if (g_layouts.empty()) g_layouts = keyboard_layouts();
  page->heading(_("Keyboard & Mouse"), _("Layouts, key repeat, pointer and touchpad"));

  page->begin_card(_("Keyboard"));
  if (g_layouts.empty()) {
    page->note(_("Keyboard layouts are not available (the xkb-data package is missing)."));
  } else {
    std::vector<std::string> names;
    std::string cur = current_layout(), second = cfg.get("input", "keyboard_layout2");
    int cur_i = 0, second_i = 0;
    for (size_t i = 0; i < g_layouts.size(); ++i) {
      names.push_back(g_layouts[i].second);
      if (g_layouts[i].first == cur) cur_i = int(i);
      if (g_layouts[i].first == second) second_i = int(i) + 1;
    }
    page->choice_row(_("Layout"), names, cur_i, [](int v) {
      if (v < 0) return;
      settings_config().set("input", "keyboard_layout", g_layouts[size_t(v)].first);
      save_and_apply_keyboard();
    });
    std::vector<std::string> with_none{_("None")};
    with_none.insert(with_none.end(), names.begin(), names.end());
    page->choice_row(_("Second layout"), with_none, second_i, [](int v) {
      settings_config().set("input", "keyboard_layout2", v <= 0 ? "" : g_layouts[size_t(v - 1)].first);
      save_and_apply_keyboard();
    }, _("Switch between the two layouts with Alt+Shift."));
  }
  page->slider_row(_("Repeat delay"), 150, 1000, cfg.get_int("input", "repeat_delay", 500), [](double v) {
    settings_config().set("input", "repeat_delay", std::to_string(int(v)));
    save_and_apply_keyboard();
  }, _("How long a key is held before it repeats (150-1000 ms)."));
  page->slider_row(_("Repeat speed"), 5, 60, cfg.get_int("input", "repeat_rate", 30), [](double v) {
    settings_config().set("input", "repeat_rate", std::to_string(int(v)));
    save_and_apply_keyboard();
  }, _("Characters per second while a key is held."));
  page->end_card();

  auto pointers = query_pointers();
  bool has_touchpad = std::any_of(pointers.begin(), pointers.end(), [](const Pointer &p) { return p.touchpad; });

  page->begin_card(_("Mouse"));
  page->slider_row(_("Pointer speed"), -100, 100, cfg.get_int("input", "mouse_speed", 0), [](double v) {
    settings_config().set("input", "mouse_speed", std::to_string(int(v)));
    save_and_apply_pointers();
  });
  page->switch_row(_("Natural scrolling"), cfg.get_bool("input", "mouse_natural_scroll", false), [](bool on) {
    settings_config().set("input", "mouse_natural_scroll", on ? "true" : "false");
    save_and_apply_pointers();
  }, _("Content moves in the direction of the wheel."));
  if (pointers.empty())
    page->note(_("No adjustable pointing device was found. Your choices are saved and applied when "
                 "one is connected at login."));
  page->end_card();

  page->begin_card(_("Touchpad"));
  if (!has_touchpad) page->note(_("No touchpad was found. These settings apply when one is present at login."));
  page->switch_row(_("Tap to click"), cfg.get_bool("input", "touchpad_tap", true), [](bool on) {
    settings_config().set("input", "touchpad_tap", on ? "true" : "false");
    save_and_apply_pointers();
  });
  page->switch_row(_("Natural scrolling"), cfg.get_bool("input", "touchpad_natural_scroll", true), [](bool on) {
    settings_config().set("input", "touchpad_natural_scroll", on ? "true" : "false");
    save_and_apply_pointers();
  });
  page->switch_row(_("Disable while typing"), cfg.get_bool("input", "touchpad_disable_while_typing", true),
                   [](bool on) {
                     settings_config().set("input", "touchpad_disable_while_typing", on ? "true" : "false");
                     save_and_apply_pointers();
                   });
  page->slider_row(_("Pointer speed"), -100, 100, cfg.get_int("input", "touchpad_speed", 0), [](double v) {
    settings_config().set("input", "touchpad_speed", std::to_string(int(v)));
    save_and_apply_pointers();
  });
  page->end_card();
  page->finish();
}

}  // namespace sde
