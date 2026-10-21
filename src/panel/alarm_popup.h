// SPDX-License-Identifier: GPL-3.0-or-later
// The popup of the panel clock: the time and date, ringing alarms (Snooze,
// Dismiss), the list of alarms with on/off switches, and the alarm editor
// (time, repeat days, name, snooze time). A link opens the Action Center's
// calendar and agenda. The panel process rings the alarms (alarms.h).
#pragma once

#include <string>

#include "agenda.h"
#include "popup.h"

class AlarmPopup : public PanelPopup {
 public:
  AlarmPopup();
  // Alarms, the theme or the configuration changed.
  void changed();
  void draw() override;
  int handle(int event) override;
  // Opens the editor for a new alarm (from the Action Center tile).
  void start_new() { pending_new_ = true; }

  // Used by the editor widgets.
  void edit(int id);  // 0: a new alarm
  void step_hour(int delta);
  void step_minute(int delta);
  void toggle_day(int bit);
  const sde::agenda::Alarm &draft() const { return draft_; }
  void set_label(const std::string &s) { draft_.label = s; }
  void set_snooze(int minutes) { draft_.snooze = minutes; }
  void save();
  void cancel();
  void remove();

 protected:
  void prepare() override;
  void closed() override;
  bool native_children() const override { return true; }

 private:
  void rebuild();
  std::string key() const;
  static void rebuild_timeout(void *);
  class Fl_Scroll *body_ = nullptr;
  int content_height_ = 0, header_ = 0;
  bool editing_ = false, pending_new_ = false, rebuild_pending_ = false;
  sde::agenda::Alarm draft_;
  std::string built_;
  std::string message_;
};
