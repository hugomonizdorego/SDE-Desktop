// SPDX-License-Identifier: GPL-3.0-or-later
#include "timezone.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

#include "build_config.h"
#include "i18n.h"
#include "process.h"
#include "util.h"
#include "weather.h"

using namespace sde;

namespace tzsync {

bool valid_zone(const std::string &zone) {
  if (zone.empty() || zone.size() > 64 || zone.front() == '/' || zone.back() == '/') return false;
  for (const auto &part : split(zone, '/', true)) {
    if (part.empty() || part == "." || part == "..") return false;
    for (char c : part)
      if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '+')) return false;
    if (!isalpha((unsigned char)part[0])) return false;
  }
  return true;
}

bool parse_ip_timezone(const std::string &text, std::string &zone) {
  weather::Json j;
  if (!weather::parse_json(text, j) || j.type != weather::Json::OBJECT) return false;
  if (const weather::Json *e = j.get("error"))
    if (e->type == weather::Json::BOOL && e->b) return false;
  if (j.get("status") && j.text("status") != "success") return false;
  std::string z = trim(j.text("timezone"));
  if (!valid_zone(z)) return false;
  zone = z;
  return true;
}

std::string zone_from_link(const std::string &target) {
  const std::string key = "zoneinfo/";
  size_t at = target.rfind(key);
  if (at == std::string::npos) return "";
  std::string z = target.substr(at + key.size());
  // zoneinfo/posix/Europe/Lisbon and zoneinfo/right/... are the same zones.
  for (const char *prefix : {"posix/", "right/"})
    if (starts_with(z, prefix)) z = z.substr(std::string(prefix).size());
  return valid_zone(z) ? z : "";
}

std::string current_zone() {
  std::string tz = env("TZ");
  if (!tz.empty()) {
    if (tz[0] == ':') tz = tz.substr(1);
    if (valid_zone(tz)) return tz;
    std::string z = zone_from_link(tz);
    if (!z.empty()) return z;
  }
  char buf[512];
  ssize_t n = readlink("/etc/localtime", buf, sizeof buf - 1);
  if (n > 0) {
    std::string z = zone_from_link(std::string(buf, size_t(n)));
    if (!z.empty()) return z;
  }
  std::string text;
  if (read_file("/etc/timezone", text) && valid_zone(trim(text))) return trim(text);
  return "";
}

std::string offset_label(long gmtoff) {
  if (gmtoff == 0) return "UTC";
  long a = gmtoff < 0 ? -gmtoff : gmtoff;
  long h = a / 3600, m = (a % 3600) / 60;
  std::string s = std::string("UTC") + (gmtoff < 0 ? "-" : "+") + std::to_string(h);
  if (m) s += format(":%02ld", m);
  return s;
}

std::string current_label() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  std::string zone = current_zone();
  if (zone.empty()) zone = lt.tm_zone ? lt.tm_zone : "";
  std::string off = offset_label(lt.tm_gmtoff);
  if (zone.empty() || zone == off) return off;
  return zone + " \xc2\xb7 " + off;
}

std::vector<Step> sync_plan(const std::string &zone, const std::string &current, bool timesyncd,
                            bool interactive) {
  std::vector<Step> plan;
  auto td = [interactive](std::vector<std::string> args) {
    std::vector<std::string> argv{"timedatectl"};
    if (!interactive) argv.push_back("--no-ask-password");
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
  };
  if (valid_zone(zone) && zone != current) plan.push_back({td({"set-timezone", zone}), true});
  // Off and on restarts systemd-timesyncd, which then synchronizes at once.
  if (timesyncd) plan.push_back({td({"set-ntp", "false"}), false});
  plan.push_back({td({"set-ntp", "true"}), true});
  return plan;
}

std::vector<std::string> with_pkexec(const std::vector<std::string> &argv) {
  std::vector<std::string> out{"pkexec"};
  for (const auto &a : argv)
    if (a != "--no-ask-password") out.push_back(a);
  return out;
}

bool auto_due(bool enabled, long long last, long long now) {
  return enabled && (last == 0 || now - last >= 3600 || now < last);
}

bool timesyncd_installed() {
  return is_executable("/lib/systemd/systemd-timesyncd") ||
         is_executable("/usr/lib/systemd/systemd-timesyncd");
}

// ----------------------------------------------------------------- Sync

namespace {
const char *const kUrls[2] = {
    "https://ipapi.co/json/",
    "http://ip-api.com/json/?fields=status,message,timezone",
};
std::vector<std::string> curl(const std::string &url) {
  return {"curl", "-fsSL", "--max-time", "10", "--connect-timeout", "6",
          "-A", std::string("SDE/") + SDE_VERSION + " (time zone)", url};
}
bool network_failure(int status) {
  return status == 5 || status == 6 || status == 7 || status == 28 || status == 35 ||
         status == 56 || status < 0;
}
const std::string &fixture() {
  static const std::string name = env("SDE_TZ_FIXTURE");
  return name;
}
}  // namespace

Sync &Sync::get() {
  static Sync s;
  return s;
}

void Sync::start(bool online) {
  if (busy()) return;
  if (!online && fixture().empty()) {
    finish(FAILED, _("No internet: the time zone cannot be detected."));
    return;
  }
  run(true);
}

void Sync::automatic(bool enabled, bool online) {
  long long now = (long long)time(nullptr);
  if (busy() || !online || !auto_due(enabled, last_auto_, now)) return;
  last_auto_ = now;
  run(false);
}

void Sync::run(bool interactive) {
  interactive_ = interactive;
  if (fixture().empty() && find_program("curl").empty()) {
    finish(FAILED, _("curl is not installed: the time zone cannot be detected."));
    return;
  }
  state_ = DETECTING;
  message_ = _("Detecting the time zone...");
  if (changed_) changed_();
  lookup(0);
}

void Sync::lookup(int step) {
  std::vector<std::string> argv;
  const std::string &fx = fixture();
  if (fx == "offline") argv = {"sh", "-c", "exit 6"};
  else if (fx == "fail") argv = {"sh", "-c", "exit 22"};
  else if (!fx.empty()) argv = {"printf", "%s", "{\"timezone\":" + weather::json_escape(fx) + "}"};
  else argv = curl(kUrls[step]);
  bool started = run_async(argv, [this, step](const std::string &out, int st) {
    std::string zone;
    if (st == 0 && parse_ip_timezone(out, zone)) {
      zone_ = zone;
      state_ = APPLYING;
      message_ = format(_("Setting the time zone %s..."), zone.c_str());
      if (changed_) changed_();
      apply(sync_plan(zone, current_zone(), timesyncd_installed(), interactive_), 0, false);
      return;
    }
    if (step == 0 && fixture().empty() && !network_failure(st)) {
      lookup(1);
      return;
    }
    finish(FAILED, network_failure(st) ? _("No internet: the time zone cannot be detected.")
                                       : _("The time zone could not be detected (no answer from "
                                           "the location service)."));
  });
  if (!started) finish(FAILED, _("The time zone could not be detected."));
}

void Sync::apply(std::vector<Step> plan, size_t i, bool pk) {
  if (i >= plan.size()) {
    // The clock of this process (and the panel's) follows the new zone.
    if (!env("TZ").empty() || !fixture().empty()) setenv("TZ", (":" + zone_).c_str(), 1);
    tzset();
    finish(DONE, format(_("Synced \xc2\xb7 %s"), zone_.c_str()));
    return;
  }
  std::vector<std::string> argv = pk ? with_pkexec(plan[i].argv) : plan[i].argv;
  bool started = run_async(argv, [this, plan, i, pk](const std::string &, int st) {
    if (st == 0 || !plan[i].required) {
      apply(plan, i + 1, false);
      return;
    }
    // systemd-timedated refused (no polkit agent, or not allowed): ask
    // through pkexec once.
    if (!pk && interactive_ && !find_program("pkexec").empty()) {
      apply(plan, i, true);
      return;
    }
    bool zone_step = plan[i].argv.size() > 1 &&
                     std::find(plan[i].argv.begin(), plan[i].argv.end(), "set-timezone") !=
                         plan[i].argv.end();
    if (pk && (st == 126 || st == 127)) finish(FAILED, _("Authentication was cancelled."));
    else if (!interactive_) finish(FAILED, _("Setting the time zone needs an administrator: press Sync."));
    else
      finish(FAILED, zone_step ? format(_("The time zone %s could not be set."), zone_.c_str())
                               : std::string(_("Network time could not be turned on.")));
  });
  if (!started) finish(FAILED, _("timedatectl is not installed (systemd)."));
}

void Sync::finish(State s, const std::string &message) {
  state_ = s;
  message_ = message;
  if (changed_) changed_();
}

}  // namespace tzsync
