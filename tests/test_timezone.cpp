// SPDX-License-Identifier: GPL-3.0-or-later
// Clock "Sync time zone": IP geolocation answers, zone names, the
// /etc/localtime link, UTC offsets and the timedatectl command plan.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/panel/timezone.h"
#include "util.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

std::string fixture(const std::string &name) {
  std::string text;
  if (!sde::read_file(std::string(SDE_TEST_SOURCE_DIR) + "/tests/fixtures/weather/" + name, text))
    std::fprintf(stderr, "missing fixture %s\n", name.c_str()), ++failures;
  return text;
}
using Argv = std::vector<std::string>;
}  // namespace

int main() {
  using namespace tzsync;
  // ------------------------------------------------------ zone names
  for (const char *z : {"Asia/Jakarta", "America/Argentina/Salta", "Etc/GMT+7", "UTC",
                        "America/Port-au-Prince", "Europe/Isle_of_Man"})
    CHECK(valid_zone(z));
  for (const char *z : {"", "/Asia/Jakarta", "Asia/", "Asia//Jakarta", "../etc/passwd",
                        "Asia/../../etc", "Asia/Jakarta;reboot", "Asia/Ja karta", "-rf", "7Up",
                        "Europe/Lisbon\n"})
    CHECK(!valid_zone(z));
  CHECK(!valid_zone(std::string(65, 'A')));

  // ------------------------------------------------- service answers
  std::string zone = "unchanged";
  CHECK(parse_ip_timezone(fixture("ipapi-co.json"), zone) && zone == "Asia/Jakarta");
  zone = "unchanged";
  CHECK(parse_ip_timezone(R"({"status":"success","timezone":"Europe/Berlin"})", zone) &&
        zone == "Europe/Berlin");
  zone = "unchanged";
  CHECK(!parse_ip_timezone(fixture("ip-api-com-fail.json"), zone) && zone == "unchanged");
  CHECK(!parse_ip_timezone(fixture("ipapi-co-ratelimited.json"), zone));
  CHECK(!parse_ip_timezone(fixture("ip-api-com.json"), zone));  // asked without the field
  CHECK(!parse_ip_timezone(R"({"timezone":"../../etc/shadow"})", zone));
  CHECK(!parse_ip_timezone(R"({"timezone":42})", zone));
  CHECK(!parse_ip_timezone("<html>blocked</html>", zone));
  CHECK(!parse_ip_timezone("", zone) && zone == "unchanged");

  // ------------------------------------------------ /etc/localtime
  CHECK(zone_from_link("/usr/share/zoneinfo/Asia/Jakarta") == "Asia/Jakarta");
  CHECK(zone_from_link("../usr/share/zoneinfo/America/Argentina/Salta") ==
        "America/Argentina/Salta");
  CHECK(zone_from_link("/usr/share/zoneinfo/posix/Europe/Lisbon") == "Europe/Lisbon");
  CHECK(zone_from_link("/usr/share/zoneinfo/UTC") == "UTC");
  CHECK(zone_from_link("/etc/sde-localtime") == "");
  CHECK(zone_from_link("/usr/share/zoneinfo/") == "");

  // ------------------------------------------------------- offsets
  CHECK(offset_label(7 * 3600) == "UTC+7");
  CHECK(offset_label(5 * 3600 + 1800) == "UTC+5:30");
  CHECK(offset_label(5 * 3600 + 45 * 60) == "UTC+5:45");
  CHECK(offset_label(-3 * 3600) == "UTC-3");
  CHECK(offset_label(-(9 * 3600 + 1800)) == "UTC-9:30");
  CHECK(offset_label(0) == "UTC");

  // ------------------------------------------------------ the plan
  auto plan = sync_plan("Asia/Jakarta", "Europe/Lisbon", true, true);
  CHECK(plan.size() == 3);
  if (plan.size() == 3) {
    CHECK(plan[0].argv == (Argv{"timedatectl", "set-timezone", "Asia/Jakarta"}) && plan[0].required);
    CHECK(plan[1].argv == (Argv{"timedatectl", "set-ntp", "false"}) && !plan[1].required);
    CHECK(plan[2].argv == (Argv{"timedatectl", "set-ntp", "true"}) && plan[2].required);
  }
  // Already right: only network time.
  plan = sync_plan("Asia/Jakarta", "Asia/Jakarta", false, true);
  CHECK(plan.size() == 1 && plan[0].argv == (Argv{"timedatectl", "set-ntp", "true"}));
  // Automatic sync never asks for a password.
  plan = sync_plan("Asia/Jakarta", "UTC", false, false);
  CHECK(plan.size() == 2);
  if (plan.size() == 2) {
    CHECK(plan[0].argv == (Argv{"timedatectl", "--no-ask-password", "set-timezone", "Asia/Jakarta"}));
    CHECK(plan[1].argv == (Argv{"timedatectl", "--no-ask-password", "set-ntp", "true"}));
    CHECK(with_pkexec(plan[0].argv) == (Argv{"pkexec", "timedatectl", "set-timezone", "Asia/Jakarta"}));
  }
  // An invalid zone is never passed on.
  plan = sync_plan("Asia/Jakarta;reboot", "UTC", false, true);
  CHECK(plan.size() == 1 && plan[0].argv[1] == "set-ntp");

  // ------------------------------------------- automatic schedule
  CHECK(!auto_due(false, 0, 10000));
  CHECK(auto_due(true, 0, 10000));
  CHECK(!auto_due(true, 10000, 10000 + 3599));
  CHECK(auto_due(true, 10000, 10000 + 3600));
  CHECK(auto_due(true, 10000, 5000));  // the clock went back

  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  else std::printf("timezone: all checks passed\n");
  return failures ? 1 : 0;
}
