// SPDX-License-Identifier: GPL-3.0-or-later
#include "alarms.h"

#include <FL/Fl.H>
#include <FL/x.H>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>

#include "dbus.h"
#include "i18n.h"
#include "panel.h"
#include "process.h"
#include "util.h"

using namespace sde;
using agenda::Alarm;
using agenda::Item;

namespace {
const char *kNotifyName = "org.freedesktop.Notifications";
const char *kNotifyPath = "/org/freedesktop/Notifications";
// A missed alarm (the computer was asleep or off) still rings this late.
const long long kAlarmGrace = 15 * 60, kReminderGrace = 6 * 3600;
// The alarm sound repeats for this long when nobody answers.
const long long kRingFor = 3 * 60;

long long now_s() { return (long long)time(nullptr); }

std::string clock_text(int hour, int minute) {
  char buf[16];
  snprintf(buf, sizeof buf, "%02d:%02d", hour, minute);
  return buf;
}

std::string parent_dir(const std::string &path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

// One basic value of type |type| in a variant (dbus::Writer has no byte).
void append_variant(DBusMessageIter *parent, int type, const char *sig, const void *value) {
  DBusMessageIter v;
  dbus_message_iter_open_container(parent, DBUS_TYPE_VARIANT, sig, &v);
  dbus_message_iter_append_basic(&v, type, value);
  dbus_message_iter_close_container(parent, &v);
}
}  // namespace

Alarms &Alarms::get() {
  static Alarms a;
  return a;
}

std::string Alarms::alarms_path() { return config_home() + "/sde/alarms.ini"; }
std::string Alarms::agenda_path() { return data_home() + "/sde/agenda.ini"; }

void Alarms::start(std::function<void()> changed, std::function<void()> unseen) {
  changed_ = std::move(changed);
  unseen_ = std::move(unseen);
  last_check_ = now_s();
  started_ = true;
  reload();
}

void Alarms::reload() {
  std::string text;
  alarms_ = read_file(alarms_path(), text) ? agenda::parse_alarms(text) : std::vector<Alarm>{};
  text.clear();
  items_ = read_file(agenda_path(), text) ? agenda::parse_items(text) : std::vector<Item>{};
  ++revision_;
  if (started_) {
    check();
    if (changed_) changed_();
  }
}

void Alarms::changed() {
  ++revision_;
  schedule();
  if (changed_) changed_();
}

void Alarms::save_alarms() {
  make_dirs(parent_dir(alarms_path()));
  write_file_atomic(alarms_path(), agenda::format_alarms(alarms_));
}

void Alarms::save_items() {
  make_dirs(parent_dir(agenda_path()));
  write_file_atomic(agenda_path(), agenda::format_items(items_));
}

const Alarm *Alarms::alarm(int id) const {
  for (const auto &a : alarms_)
    if (a.id == id) return &a;
  return nullptr;
}

const Item *Alarms::item(int id) const {
  for (const auto &i : items_)
    if (i.id == id) return &i;
  return nullptr;
}

int Alarms::save_alarm(Alarm a) {
  a.snoozed_until = 0;
  a.enabled = true;
  bool found = false;
  for (auto &old : alarms_)
    if (a.id && old.id == a.id) {
      old = a;
      found = true;
    }
  if (!found) {
    a.id = agenda::next_id(alarms_);
    alarms_.push_back(a);
  }
  save_alarms();
  changed();
  return a.id;
}

void Alarms::remove_alarm(int id) {
  stop_ringing(id);
  alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
                               [id](const Alarm &a) { return a.id == id; }),
                alarms_.end());
  save_alarms();
  changed();
}

void Alarms::set_enabled(int id, bool on) {
  for (auto &a : alarms_)
    if (a.id == id) {
      a.enabled = on;
      a.snoozed_until = 0;
    }
  if (!on) stop_ringing(id);
  save_alarms();
  changed();
}

int Alarms::save_item(Item item) {
  item.reminded = false;
  // A reminder whose time has already passed when it is set does not fire.
  long long at = agenda::reminder_time(item);
  if (at && at <= now_s() - 60) item.reminded = true;
  bool found = false;
  for (auto &old : items_)
    if (item.id && old.id == item.id) {
      old = item;
      found = true;
    }
  if (!found) {
    item.id = agenda::next_id(items_);
    items_.push_back(item);
  }
  save_items();
  changed();
  return item.id;
}

void Alarms::remove_item(int id) {
  dismiss_reminder(id);
  items_.erase(std::remove_if(items_.begin(), items_.end(),
                              [id](const Item &i) { return i.id == id; }),
               items_.end());
  save_items();
  changed();
}

long long Alarms::next_alarm(int *id) const {
  long long best = 0, now = now_s();
  for (const auto &a : alarms_) {
    long long t = agenda::next_due(a, now);
    if (t && (!best || t < best)) {
      best = t;
      if (id) *id = a.id;
    }
  }
  return best;
}

// ------------------------------------------------------------ scheduling

void Alarms::schedule() {
  Fl::remove_timeout(tick, this);
  long long now = now_s(), next = 0;
  for (const auto &a : alarms_) {
    long long t = agenda::next_due(a, now);
    if (t && (!next || t < next)) next = t;
  }
  for (const auto &i : items_) {
    long long t = i.reminded ? 0 : agenda::reminder_time(i);
    if (t > now && (!next || t < next)) next = t;
  }
  // Wake at least every 30 s, so that clock changes and suspend are noticed.
  double delay = 30;
  if (next) delay = std::max(0.2, std::min(30.0, double(next - now) + 0.2));
  Fl::add_timeout(delay, tick, this);
}

void Alarms::tick(void *data) { static_cast<Alarms *>(data)->check(); }

void Alarms::check() {
  long long now = now_s();
  bool alarms_dirty = false, items_dirty = false;
  // Copies: ringing may run callbacks.
  for (auto &a : alarms_) {
    long long due = agenda::next_due(a, last_check_);
    if (!due || due > now) continue;
    bool snoozed = a.snoozed_until > 0 && a.snoozed_until <= now;
    if (snoozed) a.snoozed_until = 0;
    if (!snoozed && a.days == 0) a.enabled = false;  // rings once
    alarms_dirty = true;
    if (now - due <= kAlarmGrace) ring_alarm(a, due);
  }
  for (auto &i : items_) {
    if (i.reminded) continue;
    long long at = agenda::reminder_time(i);
    if (!at || at > now) continue;
    i.reminded = true;
    items_dirty = true;
    if (now - at <= kReminderGrace) remind(i);
  }
  last_check_ = now;
  if (alarms_dirty) save_alarms();
  if (items_dirty) save_items();
  if (alarms_dirty || items_dirty)
    changed();
  else
    schedule();
}

// ---------------------------------------------------------- notifications

void Alarms::ring_alarm(const Alarm &a, long long) {
  stop_ringing(a.id);
  Ringing r;
  r.alarm = a.id;
  r.title = a.label.empty() ? std::string(_("Alarm")) : a.label;
  r.body = clock_text(a.hour, a.minute) + " · " +
           format(_("Snooze for %d minutes or dismiss the alarm."), a.snooze);
  ringing_.push_back(r);
  ring_started_ = now_s();
  notify(ringing_.back(), true);
  play(true);
}

void Alarms::remind(const Item &item) {
  Ringing r;
  r.item = item.id;
  r.title = item.title;
  std::string when = item.timed() ? clock_text(item.hour, item.minute) : _("All day");
  char date[64];
  struct tm t {};
  t.tm_year = item.year - 1900;
  t.tm_mon = item.month - 1;
  t.tm_mday = item.day;
  t.tm_hour = 12;
  t.tm_isdst = -1;
  mktime(&t);
  strftime(date, sizeof date, "%a %e %b", &t);
  r.body = trim(date) + " · " + when + (item.note.empty() ? "" : "\n" + item.note);
  ringing_.push_back(r);
  notify(ringing_.back(), false);
  const auto &cfg = Panel::instance->config();
  if (!cfg.get_bool("notifications", "do_not_disturb", false)) play(false);
}

void Alarms::listen() {
  if (listening_) return;
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return;
  listening_ = true;
  bus->add_signal_handler(
      "type='signal',interface='org.freedesktop.Notifications'",
      [](DBusMessage *m) {
        return bool(dbus_message_is_signal(m, kNotifyName, "ActionInvoked") ||
                    dbus_message_is_signal(m, kNotifyName, "NotificationClosed"));
      },
      [this](DBusMessage *m) {
        auto args = dbus::read_args(m);
        if (args.size() < 2) return;
        uint32_t id = uint32_t(args[0].num());
        bool action = dbus_message_is_signal(m, kNotifyName, "ActionInvoked");
        std::string key = action ? args[1].str() : "";
        int reason = action ? 0 : int(args[1].num());
        for (const auto &r : std::vector<Ringing>(ringing_)) {
          if (!id || r.notification != id) continue;
          if (key == "snooze" && r.alarm)
            snooze(r.alarm);
          else if (key == "open")
            Fl::add_timeout(0, [](void *) {
              if (Panel::instance) Panel::instance->open_action_center(true);
            });
          // Closed by the user (reason 2) or an action: the alarm stops.
          if (key == "dismiss" || key == "default" || key == "open" || reason == 2) {
            if (r.alarm)
              dismiss(r.alarm);
            else
              dismiss_reminder(r.item);
          }
          break;
        }
      });
}

void Alarms::notify(Ringing &r, bool alarm) {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus || !bus->name_has_owner(kNotifyName)) {
    if (alarm && unseen_) unseen_();
    return;
  }
  listen();
  DBusMessage *m = dbus::method(kNotifyName, kNotifyPath, kNotifyName, "Notify");
  {
    dbus::Writer w(m);
    w.str(_("Clock")).u32(0).str(alarm ? "alarm-clock" : "appointment-soon").str(r.title).str(r.body);
    if (alarm)
      w.strings({"snooze", _("Snooze"), "dismiss", _("Dismiss")});
    else
      w.strings({"open", _("Open calendar"), "dismiss", _("Dismiss")});
    {
      dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}");
      {
        dbus::Writer e(hints.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("urgency");
        unsigned char urgency = alarm ? 2 : 1;
        append_variant(e.iter(), DBUS_TYPE_BYTE, "y", &urgency);
      }
      {
        dbus::Writer e(hints.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("category").variant_str(alarm ? "x-sde.alarm" : "x-sde.reminder");
      }
      {
        // The panel plays the sound itself.
        dbus::Writer e(hints.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("suppress-sound").variant_bool(true);
      }
      if (alarm) {
        dbus::Writer e(hints.iter(), DBUS_TYPE_DICT_ENTRY, nullptr);
        e.str("resident").variant_bool(true);
      }
    }
    w.i32(alarm ? 0 : -1);
  }
  int alarm_id = r.alarm, item_id = r.item;
  bus->call_async(m, [this, alarm, alarm_id, item_id](DBusMessage *reply) {
    auto args = dbus::read_args(reply);
    uint32_t id = args.empty() ? 0 : uint32_t(args[0].num());
    bool found = false;
    for (auto &x : ringing_)
      if (x.alarm == alarm_id && x.item == item_id && !x.notification) {
        x.notification = id;
        found = true;
        break;
      }
    // The ring was answered before the reply arrived: close it again.
    if (!found && id) close_notification(id);
    if (!id && alarm && unseen_) unseen_();
  });
}

void Alarms::close_notification(uint32_t id) {
  if (!id) return;
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return;
  DBusMessage *m = dbus::method(kNotifyName, kNotifyPath, kNotifyName, "CloseNotification");
  dbus::Writer(m).u32(id);
  bus->call_async(m, nullptr);
}

bool Alarms::ringing_alarm() const {
  for (const auto &r : ringing_)
    if (r.alarm) return true;
  return false;
}

void Alarms::stop_ringing(int alarm_id) {
  for (auto it = ringing_.begin(); it != ringing_.end();) {
    if (alarm_id && it->alarm == alarm_id) {
      close_notification(it->notification);
      it = ringing_.erase(it);
    } else {
      ++it;
    }
  }
  if (!ringing_alarm()) Fl::remove_timeout(sound_loop, this);
}

void Alarms::snooze(int alarm_id) {
  for (auto &a : alarms_)
    if (a.id == alarm_id) a.snoozed_until = now_s() + (long long)a.snooze * 60;
  stop_ringing(alarm_id);
  save_alarms();
  changed();
}

void Alarms::dismiss(int alarm_id) {
  stop_ringing(alarm_id);
  changed();
}

void Alarms::dismiss_reminder(int item_id) {
  for (auto it = ringing_.begin(); it != ringing_.end();) {
    if (!it->alarm && it->item == item_id) {
      close_notification(it->notification);
      it = ringing_.erase(it);
    } else {
      ++it;
    }
  }
  changed();
}

// ------------------------------------------------------------------ sound

namespace {
struct Chain {
  std::vector<std::vector<std::string>> commands;
  size_t next = 0;
  std::function<void()> done;
};
void run_chain(std::shared_ptr<Chain> c) {
  if (c->next >= c->commands.size()) {
    // No player worked: the X bell is the last resort.
    if (fl_display) {
      XBell(fl_display, 0);
      XFlush(fl_display);
    }
    c->done();
    return;
  }
  auto cmd = c->commands[c->next++];
  bool started = run_async(cmd, [c](const std::string &, int status) {
    if (status == 0)
      c->done();
    else
      run_chain(c);
  });
  if (!started) run_chain(c);
}
}  // namespace

void Alarms::play(bool alarm) {
  if (playing_) return;
  const std::string dir = "/usr/share/sounds/freedesktop/stereo/";
  std::string event = alarm ? "alarm-clock-elapsed" : "message-new-instant", file;
  std::vector<std::string> names = alarm ? std::vector<std::string>{"alarm-clock-elapsed", "bell", "complete"}
                                         : std::vector<std::string>{"message-new-instant", "message", "bell"};
  for (const auto &n : names)
    if (file_exists(dir + n + ".oga")) {
      file = dir + n + ".oga";
      break;
    }
  auto chain = std::make_shared<Chain>();
  if (!find_program("canberra-gtk-play").empty()) {
    chain->commands.push_back({"canberra-gtk-play", "-i", event, "-d", _("Clock")});
    if (!file.empty()) chain->commands.push_back({"canberra-gtk-play", "-f", file});
  }
  if (!file.empty())
    for (const char *player : {"paplay", "pw-play"})
      if (!find_program(player).empty()) chain->commands.push_back({player, file});
  playing_ = true;
  chain->done = [this, alarm] {
    playing_ = false;
    // The alarm repeats until it is answered, for a few minutes.
    if (alarm && ringing_alarm() && now_s() - ring_started_ < kRingFor) {
      Fl::remove_timeout(sound_loop, this);
      Fl::add_timeout(1.5, sound_loop, this);
    }
  };
  run_chain(chain);
}

void Alarms::sound_loop(void *data) {
  auto *self = static_cast<Alarms *>(data);
  if (self->ringing_alarm()) self->play(true);
}
