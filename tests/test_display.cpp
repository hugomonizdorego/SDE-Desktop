// SPDX-License-Identifier: GPL-3.0-or-later
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../src/display/backend.h"
#include "util.h"
using namespace sde;
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)
Output monitor(const std::string& name, const std::string& id, int x, bool primary) {
  Output o;
  o.name = name;
  o.identity = id;
  o.connected = o.enabled = true;
  o.primary = primary;
  o.x = x;
  o.width = 1920;
  o.height = 1080;
  o.current_mode = o.preferred_mode = "1920x1080";
  o.rate = 59.94;
  o.available = {
      {"1920x1080", 1920, 1080, 59.94}, {"1920x1080", 1920, 1080, 60}, {"1280x720", 1280, 720, 60}};
  o.modes = {"1920x1080", "1280x720"};
  o.rotations = {"normal", "left", "right", "inverted"};
  return o;
}
int main() {
  auto a = monitor("eDP-1", "panel-id", 0, true), b = monitor("HDMI-1", "external-id", 1920, false);
  std::vector<Output> hardware = {a, b}, wanted = hardware;
  CHECK(validate_display_layout(wanted, hardware).empty());
  wanted[1].rotation = "left";
  CHECK(output_width(wanted[1]) == 1080 && output_height(wanted[1]) == 1920);
  CHECK(validate_display_layout(wanted, hardware).empty());
  auto args = xrandr_args(wanted);
  CHECK(std::find(args.begin(), args.end(), "left") != args.end());
  wanted[1].identity = "replacement";
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[1].rate = 144;
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[1].current_mode = "3840x2160";
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[0].primary = false;
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[1].primary = true;
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[0].enabled = wanted[1].enabled = false;
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[1].x = -1;
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[1].x = 32760;
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[1].name = wanted[0].name;
  CHECK(!validate_display_layout(wanted, hardware).empty());
  wanted = hardware;
  wanted[1].rotation = "normal; touch /tmp/bad";
  CHECK(!validate_display_layout(wanted, hardware).empty());
  auto unplugged = hardware;
  unplugged[1].connected = false;
  CHECK(!validate_display_layout(hardware, unplugged).empty());
  std::vector<Output> imported;
  CHECK(
      import_legacy_display_layout("--output eDP-1 --mode 1920x1080 --pos 0x0 --primary --output "
                                   "HDMI-1 --mode 1280x720 --pos 1920x0",
                                   hardware, imported));
  CHECK(imported[1].current_mode == "1280x720");
  CHECK(!import_legacy_display_layout("--output eDP-1 --auto", hardware, imported));
  CHECK(!import_legacy_display_layout("--output eDP-1 --mode 9000x9000 --output HDMI-1 --off",
                                      hardware, imported));
  CHECK(!import_legacy_display_layout("--output eDP-1 --mode 1920x1080 ; touch /tmp/bad", hardware,
                                      imported));
  std::vector<unsigned char> edid(128, 0);
  for (size_t i = 1; i < 7; ++i) edid[i] = 255;
  edid[57] = 0xfc;
  std::string model = "SDE Monitor\n";
  std::copy(model.begin(), model.end(), edid.begin() + 59);
  unsigned sum = 0;
  for (size_t i = 0; i < 127; ++i) sum += edid[i];
  edid[127] = static_cast<unsigned char>(256 - sum % 256);
  CHECK(edid_name(edid) == "SDE Monitor");
  edid[120] = 42;
  CHECK(edid_name(edid).empty());
  CHECK(edid_name({}).empty());
  CHECK(unique_profile_name("Desk", {"desk", "Desk (1)"}) == "Desk (2)");
  CHECK(unique_profile_name("Dock", {"Desk"}) == "Dock");
  char dir[] = "/tmp/sde-display-tests-XXXXXX";
  CHECK(mkdtemp(dir));
  std::string old = env("XDG_CONFIG_HOME");
  setenv("XDG_CONFIG_HOME", dir, 1);
  CHECK(save_display_profile({"Desk / 125%", 125, hardware}));
  CHECK(display_profiles() == std::vector<std::string>{"Desk / 125%"});
  DisplayProfile p;
  CHECK(load_display_profile("Desk / 125%", p, hardware));
  CHECK(p.outputs.size() == 2 && p.scale == 125 && std::fabs(p.outputs[1].rate - 59.94) < .001);
  auto redocked = hardware;
  redocked[1].name = "DP-2";
  CHECK(load_display_profile("Desk / 125%", p, redocked));
  CHECK(p.outputs[1].name == "DP-2");
  auto replacement = hardware;
  replacement[1].identity = "replacement";
  CHECK(!load_display_profile("Desk / 125%", p, replacement));
  CHECK(!load_display_profile("Desk / 125%", p, unplugged));
  CHECK(!save_display_profile({"Invalid", 300, hardware}));
  wanted = hardware;
  wanted[1].rate = 144;
  CHECK(save_display_profile({"Invalid mode", 100, wanted}));
  CHECK(!load_display_profile("Invalid mode", p, hardware));
  CHECK(delete_display_profile("Invalid mode"));
  CHECK(delete_display_profile("Desk / 125%"));
  CHECK(display_profiles().empty());
  if (old.empty())
    unsetenv("XDG_CONFIG_HOME");
  else
    setenv("XDG_CONFIG_HOME", old.c_str(), 1);
  puts(failures ? "Display checks FAILED"
                : "EDID, multi-monitor validation, rotation, profiles, legacy migration and "
                  "redocking passed.");
  return failures ? 1 : 0;
}
