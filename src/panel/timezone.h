// SPDX-License-Identifier: GPL-3.0-or-later
// "Sync time zone" for the clock (the Action Center's calendar card and
// Settings > Panel > Date and time): detects the time zone of this network
// with the same free IP geolocation services as the weather
// (https://ipapi.co/json/ "timezone", then http://ip-api.com/json/), sets it
// with timedatectl (systemd-timedated asks polkit; pkexec when that is
// refused), turns on network time and makes systemd-timesyncd synchronize
// again (NTP off and on restarts it).
//
// [clock] auto_timezone=true repeats this at session start and whenever the
// network changes, at most once an hour, without asking for a password:
// when polkit would ask, the card says to press Sync.
//
// Everything runs in the background (curl and timedatectl through
// run_async); the panel never waits.
//
// SDE_TZ_FIXTURE=<Area/City> answers the lookup with that zone, "offline"
// as if there were no internet, "fail" as a service error (UI tests; the
// sandbox blocks the services). The commands still run (tests put a
// timedatectl stub in PATH).
//
// The parsers and the command plan have no FLTK dependency and are unit
// tested in tests/test_timezone.cpp.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tzsync {

// An IANA zone name such as "Asia/Jakarta", "America/Argentina/Salta",
// "Etc/GMT+7" or "UTC": letters, digits and "_+-" in '/'-separated parts,
// no "." or ".." parts.
bool valid_zone(const std::string &zone);
// The "timezone" of ipapi.co/json or ip-api.com/json (?fields=...,timezone);
// false for errors ({"error": true}, {"status": "fail"}) and invalid names.
bool parse_ip_timezone(const std::string &json, std::string &zone);
// The zone of an /etc/localtime symlink target
// ("/usr/share/zoneinfo/Asia/Jakarta", "../usr/share/zoneinfo/UTC"), or "".
std::string zone_from_link(const std::string &target);
// The zone this process uses: TZ (":Zone" or "Zone"), /etc/localtime, then
// /etc/timezone; "" when unknown.
std::string current_zone();
// "UTC+7", "UTC+5:30", "UTC-3", "UTC" for a UTC offset in seconds.
std::string offset_label(long gmtoff);
// "Asia/Jakarta · UTC+7" (the zone of this process at |now|).
std::string current_label();

// One command of the plan. A |required| step that fails stops the plan;
// with |interactive| it is first tried again through pkexec.
struct Step {
  std::vector<std::string> argv;
  bool required = true;
};
// Sets |zone| (unless it is |current|), turns on network time and, with
// systemd-timesyncd, switches NTP off and on so that it synchronizes now.
// Without |interactive| (automatic sync) timedatectl never asks for a
// password (--no-ask-password).
std::vector<Step> sync_plan(const std::string &zone, const std::string &current, bool timesyncd,
                            bool interactive);
// The same command run as root through pkexec (without --no-ask-password).
std::vector<std::string> with_pkexec(const std::vector<std::string> &argv);
// [clock] auto_timezone: due when on and the last automatic sync is at
// least an hour old (or there was none).
bool auto_due(bool enabled, long long last, long long now);
bool timesyncd_installed();

class Sync {
 public:
  enum State { IDLE, DETECTING, APPLYING, DONE, FAILED };
  static Sync &get();
  void set_listener(std::function<void()> changed) { changed_ = std::move(changed); }
  // Detects and sets the zone now (the Sync button). |online|: the network
  // reaches the internet.
  void start(bool online);
  // [clock] auto_timezone: at session start and on network changes.
  void automatic(bool enabled, bool online);
  State state() const { return state_; }
  bool busy() const { return state_ == DETECTING || state_ == APPLYING; }
  // "Synced · Asia/Jakarta", "Detecting the time zone...", the error, or "".
  const std::string &message() const { return message_; }
  const std::string &detected() const { return zone_; }

 private:
  void run(bool interactive);
  void lookup(int step);
  void apply(std::vector<Step> plan, size_t i, bool pk);
  void finish(State s, const std::string &message);
  std::function<void()> changed_;
  State state_ = IDLE;
  bool interactive_ = true;
  std::string message_, zone_;
  long long last_auto_ = 0;
};

}  // namespace tzsync
