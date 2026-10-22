// SPDX-License-Identifier: GPL-3.0-or-later
// alpha50 panel status rules: tooltip storage and names, network LED,
// keyboard indicator de-duplication, UDisks2 drives, busy programs and
// touchpad properties.
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/common/touchpad.h"
#include "../src/panel/status_model.h"
#include "../src/panel/tooltip_text.h"

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

int main() {
  // Regression (alpha16 "^G" / garbage tooltips): Fl_Tooltip keeps the
  // pointer it was given. The text handed out must stay valid after the
  // next set() and a new text must never reuse the previous pointer.
  {
    TooltipText tip;
    const char *a = tip.set("Volume 40%");
    const char *b = tip.set("Keyboard layout · US");
    CHECK(a != b);
    CHECK(!strcmp(a, "Volume 40%"));  // still readable while Fl_Tooltip may show it
    CHECK(!strcmp(b, "Keyboard layout · US"));
    const char *c = tip.set("Keyboard layout · US");
    CHECK(c != b && !strcmp(c, b));
    CHECK(tip.current() == "Keyboard layout · US");
  }
  // Names of tray items: printable, with fallbacks.
  CHECK(printable_text("\aCelluloid\n") == "Celluloid");
  CHECK(printable_text("a\xff" "b") == "a\xEF\xBF\xBD" "b");
  CHECK(printable_text("Caf\xC3\xA9") == "Caf\xC3\xA9");
  CHECK(tray_item_name("", "", "nm-applet", ":1.4") == "nm-applet");
  CHECK(tray_item_name("\a", "Telegram", "", "") == "Telegram");
  CHECK(tray_item_name("", "", "", "org.kde.StatusNotifierItem-1234-1") == "Application");
  CHECK(tray_item_name("", "", "", "org.example.Notes") == "Notes");
  // Network LED.
  CHECK(network_led(false, 4, false) == NetLed::RED);
  CHECK(network_led(true, 4, false) == NetLed::GREEN);
  CHECK(network_led(true, 3, false) == NetLed::YELLOW);
  CHECK(network_led(true, 2, false) == NetLed::YELLOW);
  CHECK(network_led(true, 1, false) == NetLed::YELLOW);
  CHECK(network_led(true, 0, false) == NetLed::GREEN);
  CHECK(network_led(true, 0, true) == NetLed::YELLOW);
  // debion-network-check: FULL alone is not enough when it says otherwise.
  CHECK(network_led(true, 4, false, "proxy-required") == NetLed::YELLOW);
  CHECK(network_led(true, 4, false, "ok") == NetLed::GREEN);
  CHECK(network_led(true, 0, true, "ok") == NetLed::GREEN);
  CHECK(network_led(false, 4, false, "ok") == NetLed::RED);
  CHECK(parse_network_verdict("{\"version\":2,\"verdict\":\"no-dns\",\"nm\":\"\"}") == "no-dns");
  CHECK(parse_network_verdict("garbage").empty());
  CHECK(!network_verdict_text("proxy-required").empty() && network_verdict_text("ok").empty());
  // Keyboard applets.
  CHECK(keyboard_applet("ibus-ui-gtk3") == KeyboardApplet::INPUT_METHOD);
  CHECK(keyboard_applet("Fcitx") == KeyboardApplet::INPUT_METHOD);
  CHECK(keyboard_applet("org.fcitx.Fcitx5") == KeyboardApplet::INPUT_METHOD);
  CHECK(keyboard_applet("gxkb") == KeyboardApplet::LAYOUT);
  CHECK(keyboard_applet("xxkb") == KeyboardApplet::LAYOUT);
  CHECK(keyboard_applet("nm-applet") == KeyboardApplet::NONE);
  // UDisks2: a USB stick with an unmounted partition, a card in a reader,
  // the internal disk (left out) and a USB disk with a swap only (left out).
  {
    std::vector<UdisksDrive> drives(4);
    drives[0].object = "/drives/Kingston";
    drives[0].vendor = "Kingston";
    drives[0].model = "DataTraveler";
    drives[0].connection_bus = "usb";
    drives[0].removable = true;
    drives[0].can_power_off = true;
    drives[0].size = 16000000000;
    drives[1].object = "/drives/Reader";
    drives[1].model = "SD/MMC";
    drives[1].media_removable = true;
    drives[1].ejectable = true;
    drives[1].media = "flash_sd";
    drives[2].object = "/drives/Samsung_SSD";
    drives[2].connection_bus = "";
    drives[3].object = "/drives/Swap";
    drives[3].connection_bus = "usb";
    std::vector<UdisksBlock> blocks(7);
    blocks[0] = {"/block/sdb", "/dev/sdb", "/drives/Kingston", "", "", "", "", "", 16000000000};
    blocks[0].partition_table = true;
    blocks[1] = {"/block/sdb1", "/dev/sdb1", "/drives/Kingston", "DATA", "vfat", "filesystem", "", "", 15900000000};
    blocks[1].filesystem = blocks[1].partition = blocks[1].hint_auto = true;
    blocks[2] = {"/block/mmcblk0", "/dev/mmcblk0", "/drives/Reader", "", "", "", "", "", 32000000000};
    blocks[3] = {"/block/mmcblk0p1", "/dev/mmcblk0p1", "/drives/Reader", "CAMERA", "exfat", "filesystem", "", "", 32000000000};
    blocks[3].filesystem = blocks[3].partition = true;
    blocks[3].mount_points = {"/media/user/CAMERA"};
    blocks[4] = {"/block/nvme0n1p2", "/dev/nvme0n1p2", "/drives/Samsung_SSD", "", "ext4", "filesystem", "", "", 500000000000};
    blocks[4].filesystem = blocks[4].partition = blocks[4].hint_system = true;
    blocks[4].mount_points = {"/"};
    blocks[5] = {"/block/sdc1", "/dev/sdc1", "/drives/Swap", "", "swap", "other", "", "", 1000000};
    blocks[5].partition = true;
    blocks[6] = {"/block/sdc", "/dev/sdc", "/drives/Swap", "", "", "", "", "", 1000000};
    auto found = udisks_removable_drives(blocks, drives);
    CHECK(found.size() == 2);
    if (found.size() == 2) {
      CHECK(found[0].name == "mmcblk0" && found[0].kind == "card" && found[0].label == "CAMERA");
      CHECK(found[0].mounted() && found[0].ejectable && !found[0].can_power_off);
      CHECK(found[1].name == "sdb" && found[1].kind == "usb" && found[1].label == "DATA");
      CHECK(!found[1].mounted() && found[1].can_power_off && found[1].parts.size() == 1);
      CHECK(found[1].parts[0].object == "/block/sdb1" && found[1].model == "Kingston DataTraveler");
    }
  }
  // lsblk fallback: util-linux 2.37+ MOUNTPOINTS column, unmounted stick.
  {
    auto d = parse_removable_drives(
        "NAME=\"sdb\" PATH=\"/dev/sdb\" LABEL=\"\" SIZE=\"16008609792\" TYPE=\"disk\" TRAN=\"usb\" "
        "RM=\"1\" HOTPLUG=\"1\" ROTA=\"0\" MOUNTPOINTS=\"\" FSTYPE=\"\" MODEL=\"Flash\" VENDOR=\"\" "
        "PKNAME=\"\"\n"
        "NAME=\"sdb1\" PATH=\"/dev/sdb1\" LABEL=\"STICK\" SIZE=\"16000000000\" TYPE=\"part\" "
        "TRAN=\"\" RM=\"1\" HOTPLUG=\"1\" ROTA=\"0\" MOUNTPOINTS=\"\" FSTYPE=\"vfat\" MODEL=\"\" "
        "VENDOR=\"\" PKNAME=\"sdb\"\n");
    CHECK(d.size() == 1 && d[0].label == "STICK" && !d[0].mounted() && d[0].can_power_off);
  }
  // Programs that keep a drive busy.
  {
    auto fuser = parse_busy_programs(
        "                     USER        PID ACCESS COMMAND\n"
        "/media/u/DATA:       root     kernel mount /media/u/DATA\n"
        "                     u          1234 F.... soffice.bin\n"
        "                     u          1240 ..c.. bash\n"
        "                     u          1250 F.... soffice.bin\n");
    CHECK(fuser.size() == 2 && fuser[0] == "soffice.bin" && fuser[1] == "bash");
    auto lsof = parse_busy_programs("p1234\ncvlc\np99\ncbash\n");
    CHECK(lsof.size() == 2 && lsof[0] == "vlc");
  }
  // Touchpad properties: only what the device has.
  {
    sde::TouchpadOptions o;
    o.tap = false;
    o.scroll = "edge";
    o.click = "clickfinger";
    o.speed = 50;
    auto props = sde::touchpad_props(
        o, sde::parse_prop_names("Device 'SynPS/2 Synaptics TouchPad':\n"
                                 "\tDevice Enabled (180):\t1\n"
                                 "\tlibinput Tapping Enabled (312):\t1\n"
                                 "\tlibinput Scroll Method Enabled (320):\t1, 0, 0\n"
                                 "\tlibinput Click Method Enabled (325):\t1, 0\n"
                                 "\tlibinput Accel Speed (330):\t0.000000\n"));
    CHECK(props.size() == 5);
    bool tap = false, scroll = false, click = false, speed = false;
    for (auto &p : props) {
      if (p.first == "libinput Tapping Enabled") tap = p.second == std::vector<std::string>{"0"};
      if (p.first == "libinput Scroll Method Enabled")
        scroll = p.second == std::vector<std::string>{"0", "1", "0"};
      if (p.first == "libinput Click Method Enabled")
        click = p.second == std::vector<std::string>{"0", "1"};
      if (p.first == "libinput Accel Speed") speed = p.second == std::vector<std::string>{"0.50"};
    }
    CHECK(tap && scroll && click && speed);
  }
  if (failures) return 1;
  printf("Panel status rules (tooltips, LED, keyboard, drives, touchpad) passed\n");
  return 0;
}
