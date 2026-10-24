// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Settings > Power (sde-power, sde.conf [power]; the panel's battery icon,
// [panel] battery_display and battery_time) and Settings > Lock Screen
// (sde-lock, [lock]). The daemons re-read sde.conf when Settings saves it.
#include <FL/Fl.H>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "i18n.h"
#include "power.h"
#include "settings.h"
#include "spawn.h"
#include "ui.h"
#include "util.h"

namespace sde {

namespace {

struct Delay {
  int seconds;
  const char *label;
};
const std::vector<Delay> &delays() {
  static const std::vector<Delay> list = {
      {0, N_("Never")},          {30, N_("30 seconds")},    {60, N_("1 minute")},
      {120, N_("2 minutes")},    {300, N_("5 minutes")},    {600, N_("10 minutes")},
      {900, N_("15 minutes")},   {1800, N_("30 minutes")},  {3600, N_("1 hour")},
  };
  return list;
}

std::vector<std::string> delay_labels() {
  std::vector<std::string> out;
  for (auto &d : delays()) out.push_back(_(d.label));
  return out;
}

int delay_index(int seconds) {
  if (seconds <= 0) return 0;
  int best = 1;
  for (size_t i = 1; i < delays().size(); ++i)
    if (std::abs(delays()[i].seconds - seconds) < std::abs(delays()[size_t(best)].seconds - seconds))
      best = int(i);
  return best;
}

// A delay choice bound to |section|.|key| (keys are static strings).
void delay_row(Page *page, const std::string &label, const char *section, const char *key, int fallback,
               const std::string &hint = "") {
  int cur = settings_config().get_int(section, key, fallback);
  page->choice_row(label, delay_labels(), delay_index(cur),
                   [section, key](int v) {
                     if (v < 0) return;
                     settings_config().set(section, key, std::to_string(delays()[size_t(v)].seconds));
                     apply_change();
                   },
                   hint);
}

struct ActionChoice {
  const char *id;
  const char *label;
};

void action_row(Page *page, const std::string &label, const char *key, const char *fallback,
                const std::vector<ActionChoice> &choices, const std::string &hint = "") {
  std::string cur = settings_config().get("power", key, fallback);
  std::vector<std::string> labels;
  int index = 0;
  for (size_t i = 0; i < choices.size(); ++i) {
    labels.push_back(_(choices[i].label));
    if (cur == choices[i].id) index = int(i);
  }
  auto *kept = keep(new std::vector<ActionChoice>(choices));
  page->choice_row(label, labels, index,
                   [key, kept](int v) {
                     if (v < 0 || v >= int(kept->size())) return;
                     settings_config().set("power", key, (*kept)[size_t(v)].id);
                     apply_change();
                   },
                   hint);
}

power::Profiles g_profiles;
bool g_profiles_loaded = false;

}  // namespace

void build_power_page(Page *page) {
  Config &cfg = settings_config();
  page->heading(_("Power"), _("Battery, screen dimming, sleep, lid and power button"));

  int brightness = power::brightness_percent();
  if (brightness >= 0) {
    page->begin_card(_("Screen"));
    page->slider_row(_("Brightness"), 1, 100, brightness,
                     [](double v) { power::set_brightness_percent(int(v)); },
                     _("The brightness keys change it too and show it on the screen."));
    page->switch_row(_("Show brightness changes on the screen"), cfg.get_bool("power", "show_osd", true),
                     [](bool v) {
                       settings_config().set("power", "show_osd", v ? "true" : "false");
                       apply_change();
                     });
    page->end_card();
  }

  if (!g_profiles_loaded) {
    g_profiles = power::power_profiles();
    g_profiles_loaded = true;
  }
  if (g_profiles.available && !g_profiles.names.empty()) {
    page->begin_card(_("Power mode"));
    std::vector<std::string> labels;
    int cur = 0;
    for (size_t i = 0; i < g_profiles.names.size(); ++i) {
      labels.push_back(power::profile_label(g_profiles.names[i]));
      if (g_profiles.names[i] == g_profiles.active) cur = int(i);
    }
    page->choice_row(_("Power mode"), labels, cur, [](int v) {
      if (v < 0 || v >= int(g_profiles.names.size())) return;
      power::set_power_profile(g_profiles.names[size_t(v)]);
      g_profiles = power::power_profiles();
    });
    std::vector<std::string> with_keep{_("Do not change")};
    with_keep.insert(with_keep.end(), labels.begin(), labels.end());
    for (const char *key : {"profile_battery", "profile_ac"}) {
      std::string want = cfg.get("power", key);
      int idx = 0;
      for (size_t i = 0; i < g_profiles.names.size(); ++i)
        if (g_profiles.names[i] == want) idx = int(i) + 1;
      page->choice_row(std::string(key) == "profile_battery" ? _("On battery, switch to")
                                                            : _("When plugged in, switch to"),
                       with_keep, idx, [key](int v) {
                         settings_config().set("power", key,
                                               v <= 0 ? "" : g_profiles.names[size_t(v - 1)]);
                         apply_change();
                       });
    }
    page->end_card();
  }

  page->begin_card(_("On battery"));
  delay_row(page, _("Dim the screen after"), "power", "idle_dim_battery", 120);
  delay_row(page, _("Turn the screen off after"), "power", "idle_blank_battery", 300);
  delay_row(page, _("Suspend after"), "power", "idle_suspend_battery", 900);
  page->end_card();
  page->begin_card(_("When plugged in"));
  delay_row(page, _("Dim the screen after"), "power", "idle_dim_ac", 300);
  delay_row(page, _("Turn the screen off after"), "power", "idle_blank_ac", 600);
  delay_row(page, _("Suspend after"), "power", "idle_suspend_ac", 0);
  page->note(_("Videos and presentations that ask the session to stay awake pause these timers."));
  page->end_card();

  const std::vector<ActionChoice> lid = {
      {"suspend", N_("Suspend")},     {"hibernate", N_("Hibernate")}, {"lock", N_("Lock the screen")},
      {"blank", N_("Turn the screen off")}, {"nothing", N_("Do nothing")},
      {"logind", N_("Let the system decide")}};
  page->begin_card(_("Lid and power button"));
  action_row(page, _("Closing the lid on battery"), "lid_battery", "suspend", lid);
  action_row(page, _("Closing the lid when plugged in"), "lid_ac", "suspend", lid,
             _("Nothing happens while an external monitor is connected."));
  action_row(page, _("Power button"), "power_button", "interactive",
             {{"interactive", N_("Ask what to do")}, {"suspend", N_("Suspend")},
              {"hibernate", N_("Hibernate")}, {"poweroff", N_("Shut down")},
              {"nothing", N_("Do nothing")}, {"logind", N_("Let the system decide")}});
  page->end_card();

  // "--page battery" (the battery icon's menu) opens the page here.
  page->anchor("battery");
  page->begin_card(_("Battery"));
  // 0.2 Alpha 2: what follows the battery icon on the panel (its right-click
  // menu has the same choices).
  std::string shown = lower(trim(cfg.get("panel", "battery_display", "icon")));
  page->choice_row(_("On the panel"),
                   {_("Icon only"), _("Icon and percentage"), _("Icon and label"),
                    _("Icon, label and percentage")},
                   shown == "percent" ? 1 : shown == "label" ? 2 : shown == "label-percent" ? 3 : 0,
                   [](int v) {
                     static const char *values[] = {"icon", "percent", "label", "label-percent"};
                     settings_config().set("panel", "battery_display", values[v < 0 || v > 3 ? 0 : v]);
                     apply_change();
                   },
                   _("The label tells the state: Charging, On battery, Full, Low battery."));
  page->switch_row(_("Show the time remaining in the label"),
                   cfg.get_bool("panel", "battery_time", false),
                   [](bool v) {
                     settings_config().set("panel", "battery_time", v ? "true" : "false");
                     apply_change();
                   },
                   _("\"3 h 57 min left\" or \"1 h 10 min to full\" instead of the state."));
  page->switch_row(_("Warn when the battery is low"), cfg.get_bool("power", "notify_battery", true),
                   [](bool v) {
                     settings_config().set("power", "notify_battery", v ? "true" : "false");
                     apply_change();
                   });
  page->slider_row(_("Low battery level"), 5, 30, cfg.get_int("power", "battery_low", 10), [](double v) {
    settings_config().set("power", "battery_low", std::to_string(int(v)));
    apply_change();
  });
  page->slider_row(_("Critical battery level"), 2, 15, cfg.get_int("power", "battery_critical", 5),
                   [](double v) {
                     settings_config().set("power", "battery_critical", std::to_string(int(v)));
                     apply_change();
                   });
  action_row(page, _("At the critical level"), "critical_action", "hibernate",
             {{"hibernate", N_("Hibernate")}, {"suspend", N_("Suspend")},
              {"poweroff", N_("Shut down")}, {"nothing", N_("Do nothing")}},
             _("Hibernate shuts down instead when the computer cannot hibernate."));
  page->end_card();
  page->note(_("SDE Power starts with the session (sde-power --daemon)."));
  page->finish();
}

void build_lock_page(Page *page) {
  Config &cfg = settings_config();
  page->heading(_("Lock Screen"), _("When the screen locks and how it looks"));
  page->begin_card(_("Automatic lock"));
  delay_row(page, _("Lock after inactivity"), "lock", "idle_lock", 300,
            _("Not while a video or presentation keeps the session awake."));
  page->switch_row(_("Lock before suspend"), cfg.get_bool("lock", "lock_on_suspend", true), [](bool v) {
    settings_config().set("lock", "lock_on_suspend", v ? "true" : "false");
    apply_change();
  });
  delay_row(page, _("Turn the screen off on the lock screen after"), "lock", "blank_after", 30);
  page->end_card();
  page->begin_card(_("Lock screen"));
  page->switch_row(_("Offer \"Switch User\""), cfg.get_bool("lock", "show_switch_user", true),
                   [](bool v) {
                     settings_config().set("lock", "show_switch_user", v ? "true" : "false");
                     apply_change();
                   },
                   _("Shown with LightDM, which keeps both sessions running."));
  page->note(_("The lock screen shows your wallpaper blurred, the clock, your picture and your "
               "name (Users & Avatar). Lock it now with Ctrl+Alt+L."));
  page->row(_("Lock the screen now"), S(112), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Lock"), true);
    b->callback([](Fl_Widget *, void *) { spawn_detached({"sde-lock", "--lock"}); });
    return b;
  });
  page->end_card();
  page->finish();
}

}  // namespace sde
