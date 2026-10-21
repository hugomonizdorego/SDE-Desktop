// SPDX-License-Identifier: GPL-3.0-or-later
// The panel process keeps the clock's alarms and agenda reminders: it loads
// them (agenda.h), schedules the next one with an FLTK timeout, and when one
// is due shows a notification (org.freedesktop.Notifications, sde-notifyd)
// and plays a sound (canberra-gtk-play, paplay or pw-play, else the X bell).
// Alarm notifications offer Snooze and Dismiss; the alarm keeps ringing
// until one of them is chosen (here, in the notification or in the clock's
// alarm popup), for at most a few minutes.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agenda.h"

class Alarms {
 public:
  static Alarms &get();

  // Loads the files and starts scheduling. |changed| runs after any change
  // (edits, rings, snoozes); |unseen| runs when an alarm rings but no
  // notification could be shown, so the panel opens its alarm popup instead.
  void start(std::function<void()> changed, std::function<void()> unseen);
  // Re-reads both files (sde-panel reloads on a configuration change).
  void reload();

  const std::vector<sde::agenda::Alarm> &alarms() const { return alarms_; }
  const std::vector<sde::agenda::Item> &items() const { return items_; }
  // Bumped on every change, for popups that rebuild on change.
  unsigned revision() const { return revision_; }

  // Alarms. |a.id| 0 adds a new one.
  int save_alarm(sde::agenda::Alarm a);
  void remove_alarm(int id);
  void set_enabled(int id, bool on);
  const sde::agenda::Alarm *alarm(int id) const;

  // Agenda items. |item.id| 0 adds a new one; editing resets the reminder.
  int save_item(sde::agenda::Item item);
  void remove_item(int id);
  const sde::agenda::Item *item(int id) const;

  // Ringing alarms and shown reminders.
  struct Ringing {
    int alarm = 0;  // alarm id, or 0 for an agenda reminder
    int item = 0;   // agenda item id
    std::string title, body;
    uint32_t notification = 0;
  };
  const std::vector<Ringing> &ringing() const { return ringing_; }
  void snooze(int alarm_id);
  void dismiss(int alarm_id);
  void dismiss_reminder(int item_id);

  // Epoch seconds of the next alarm (snoozes included), 0 for none.
  long long next_alarm(int *id = nullptr) const;

  static std::string alarms_path();  // ~/.config/sde/alarms.ini
  static std::string agenda_path();  // ~/.local/share/sde/agenda.ini

 private:
  void schedule();
  static void tick(void *);
  void check();
  void ring_alarm(const sde::agenda::Alarm &a, long long when);
  void remind(const sde::agenda::Item &item);
  void notify(Ringing &r, bool alarm);
  void close_notification(uint32_t id);
  void play(bool alarm);
  static void sound_loop(void *);
  void stop_ringing(int alarm_id);
  void save_alarms();
  void save_items();
  void changed();
  bool ringing_alarm() const;
  void listen();

  std::vector<sde::agenda::Alarm> alarms_;
  std::vector<sde::agenda::Item> items_;
  std::vector<Ringing> ringing_;
  std::function<void()> changed_, unseen_;
  long long last_check_ = 0, ring_started_ = 0;
  unsigned revision_ = 1;
  bool listening_ = false, started_ = false, playing_ = false;
};
