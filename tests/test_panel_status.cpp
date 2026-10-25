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
  // Login (alpha52): NetworkManager says FULL before DNS works; no green
  // until the first verdict for the connection is in; a check that failed
  // ("unknown") falls back to NetworkManager.
  CHECK(network_led(true, 4, false, "checking") == NetLed::YELLOW);
  CHECK(network_led(false, 4, false, "checking") == NetLed::RED);
  CHECK(network_led(true, 4, false, "no-dns") == NetLed::YELLOW);
  CHECK(network_led(true, 4, false, "unknown") == NetLed::GREEN);
  CHECK(network_verdict_text("checking") == "Checking internet access...");
  {
    std::string j = "{\"verdict\":\"no-dns\",\"clock_skew\":null,\"repairable\":true,\"auto_repair\": false}";
    CHECK(parse_json_bool(j, "repairable"));
    CHECK(!parse_json_bool(j, "auto_repair", true));
    CHECK(parse_json_bool("{\"version\":2}", "auto_repair", true));
    CHECK(!parse_json_bool("{\"version\":2}", "repairable"));
  }
  CHECK(network_verdict_fixable("no-dns", false));
  CHECK(network_verdict_fixable("no-route", true) && !network_verdict_fixable("no-route", false));
  CHECK(!network_verdict_fixable("proxy-required", true) && !network_verdict_fixable("ok", true));
  CHECK(network_repair_summary("Fixing\nDNS works now: deb.debian.org resolves to 1.2.3.4\n", 0) == "ok");
  CHECK(network_repair_summary("  connected enp0s3\n  DNS works now: x\n", 0) == "ok");
  CHECK(network_repair_summary("DNS still does not work: the network gives no DNS server\nNext: x\n", 4) ==
        "DNS still does not work: the network gives no DNS server");
  CHECK(network_repair_summary("nothing to repair\n", 0) == "done");
  CHECK(network_repair_summary("", 126).empty());
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
  // alpha51: the battery flyout's text, power profile order and the
  // taskbar preview geometry.
  {
    CHECK(battery_duration_text(237) == "3 hours 57 minutes");
    CHECK(battery_duration_text(60) == "1 hour");
    CHECK(battery_duration_text(1) == "1 minute");
    CHECK(battery_duration_text(-1).empty() && battery_duration_text(0).empty());
    CHECK(battery_status_line(false, false, 237) ==
          "Using battery power - 3 hours 57 minutes remaining");
    CHECK(battery_status_line(true, false, 70) == "Charging - 1 hour 10 minutes until full");
    CHECK(battery_status_line(true, true, 70) == "Fully charged");
    CHECK(battery_status_line(true, false, -1) == "Charging");
    CHECK(battery_status_line(false, false, -1) == "Using battery power");
    CHECK(battery_model_name("LGES", "AP23A8L") == "LGES AP23A8L");
    CHECK(battery_model_name(" ", "AP23A8L\n") == "AP23A8L");
    CHECK(battery_model_name("SMP", "SMP 5B10W") == "SMP 5B10W");
    CHECK(battery_model_name("", "").empty());
    auto order = ordered_power_profiles({"performance", "balanced", "custom", "power-saver"});
    CHECK((order == std::vector<std::string>{"power-saver", "balanced", "performance", "custom"}));
    PreviewSize s = preview_fit(1920, 1080, 220, 140);
    CHECK(s.w == 220 && s.h == 124);
    s = preview_fit(600, 1200, 220, 140);
    CHECK(s.w == 70 && s.h == 140);
    s = preview_fit(100, 50, 220, 140);  // never enlarged
    CHECK(s.w == 100 && s.h == 50);
    s = preview_fit(0, 50, 220, 140);
    CHECK(s.w == 0 && s.h == 0);
    CHECK(preview_column_width(1, 220, 120, 6, 1000) == 220);
    CHECK(preview_column_width(5, 220, 120, 6, 800) == 155);
    CHECK(preview_column_width(20, 220, 120, 6, 800) == 120);
  }
  // 0.2 Alpha 2: the battery indicator's phases, panel text, estimates and
  // animation.
  {
    using P = BatteryPhase;
    // UPower Device.State: 1 charging, 2 discharging, 3 empty, 4 full,
    // 5 pending charge, 6 pending discharge, 0 unknown (OnBattery decides).
    CHECK(battery_phase(1, 52, 1, false) == P::CHARGING);
    CHECK(battery_phase(1, 5, 4, false) == P::CHARGING);  // charging is never "low"
    CHECK(battery_phase(2, 45, 1, true) == P::DISCHARGING);
    CHECK(battery_phase(2, 20, 1, true) == P::LOW && battery_phase(2, 21, 1, true) == P::DISCHARGING);
    CHECK(battery_phase(2, 10, 1, true) == P::CRITICAL && battery_phase(2, 11, 1, true) == P::LOW);
    CHECK(battery_phase(2, 35, 3, true) == P::LOW);       // WarningLevel low
    CHECK(battery_phase(2, 35, 4, true) == P::CRITICAL);  // critical
    CHECK(battery_phase(2, 35, 5, true) == P::CRITICAL);  // action
    CHECK(battery_phase(3, 0, 1, true) == P::CRITICAL);
    CHECK(battery_phase(6, 60, 1, true) == P::DISCHARGING);
    CHECK(battery_phase(4, 100, 1, false) == P::FULL && battery_phase(5, 80, 1, false) == P::PLUGGED);
    CHECK(battery_phase(0, 80, 0, false) == P::PLUGGED && battery_phase(0, 100, 0, false) == P::FULL);
    CHECK(battery_phase(0, 80, 0, true) == P::DISCHARGING);
    // [power] battery_low/battery_critical only raise the 20/10 % levels.
    CHECK(battery_low_level(10) == 20 && battery_low_level(30) == 30 && battery_low_level(90) == 50);
    CHECK(battery_critical_level(5) == 10 && battery_critical_level(15) == 15);
    CHECK(battery_phase(2, 28, 1, true, battery_low_level(30)) == P::LOW);
    // The kernel's power_supply status.
    CHECK(battery_state_from_sysfs("Charging\n", 40, true) == 1);
    CHECK(battery_state_from_sysfs("Discharging", 40, false) == 2);
    CHECK(battery_state_from_sysfs("Full", 100, true) == 4);
    CHECK(battery_state_from_sysfs("Not charging", 80, true) == 5);
    CHECK(battery_state_from_sysfs("Not charging", 97, true) == 4);
    CHECK(battery_state_from_sysfs("Unknown", 60, true) == 5);
    CHECK(battery_state_from_sysfs("Unknown", 60, false) == 2);
    for (P p : {P::DISCHARGING, P::LOW, P::CRITICAL, P::CHARGING, P::FULL, P::PLUGGED})
      CHECK(std::string(battery_phase_name(p)).size() > 2);
    CHECK(std::string(battery_phase_name(P::CHARGING)) == "charging");
    // Display modes and the text after the icon.
    using D = BatteryDisplay;
    for (D d : {D::ICON, D::PERCENT, D::LABEL, D::LABEL_PERCENT})
      CHECK(parse_battery_display(battery_display_name(d)) == d);
    CHECK(parse_battery_display("") == D::ICON && parse_battery_display("bogus") == D::ICON);
    CHECK(parse_battery_display(" Percentage ") == D::PERCENT && parse_battery_display("both") == D::LABEL_PERCENT);
    CHECK(battery_panel_text(D::ICON, P::DISCHARGING, 45, 237, false, true).empty());
    CHECK(battery_panel_text(D::PERCENT, P::DISCHARGING, 45, 237, false, true) == "45%");
    CHECK(battery_panel_text(D::LABEL, P::DISCHARGING, 45, 237, false, false) == "On battery");
    CHECK(battery_panel_text(D::LABEL_PERCENT, P::CHARGING, 52, 70, false, false) == "Charging · 52%");
    CHECK(battery_panel_text(D::LABEL_PERCENT, P::FULL, 100, -1, false, false) == "Full · 100%");
    CHECK(battery_panel_text(D::LABEL, P::PLUGGED, 80, -1, false, true) == "Plugged in");
    CHECK(battery_panel_text(D::LABEL, P::LOW, 15, 30, false, false) == "Low battery");
    CHECK(battery_panel_text(D::LABEL, P::CRITICAL, 7, 9, false, false) == "Critically low");
    // ... and with the time remaining in the label.
    CHECK(battery_panel_text(D::LABEL, P::DISCHARGING, 45, 237, false, true) == "3 h 57 min left");
    CHECK(battery_panel_text(D::LABEL_PERCENT, P::CHARGING, 52, 70, false, true) == "1 h 10 min to full · 52%");
    CHECK(battery_panel_text(D::LABEL, P::CHARGING, 52, -1, true, true) == "Calculating…");
    CHECK(battery_panel_text(D::LABEL, P::CHARGING, 52, -1, false, true) == "Charging");
    CHECK(battery_panel_text(D::LABEL_PERCENT, P::FULL, 100, -1, false, true) == "Full · 100%");
    CHECK(battery_duration_short(237) == "3 h 57 min" && battery_duration_short(60) == "1 h");
    CHECK(battery_duration_short(45) == "45 min" && battery_duration_short(-1).empty());
    // The flyout line and the tooltip.
    CHECK(battery_phase_line(P::DISCHARGING, 237, false) ==
          "Using battery power - 3 hours 57 minutes remaining");
    CHECK(battery_phase_line(P::CHARGING, 70, false) == "Charging - 1 hour 10 minutes until full");
    CHECK(battery_phase_line(P::CHARGING, -1, true) == "Charging - Calculating…");
    CHECK(battery_phase_line(P::DISCHARGING, -1, true) == "Using battery power - Calculating…");
    CHECK(battery_phase_line(P::DISCHARGING, -1, false) == "Using battery power");
    CHECK(battery_phase_line(P::LOW, 25, false) == "Low battery - 25 minutes remaining");
    CHECK(battery_phase_line(P::CRITICAL, -1, false) == "Critically low - plug in the charger");
    CHECK(battery_phase_line(P::FULL, -1, false) == "Fully charged");
    CHECK(battery_phase_line(P::PLUGGED, -1, false) == "Plugged in, not charging");
    CHECK(battery_tooltip(P::DISCHARGING, 45, 237, false) == "Battery 45% · 3 hours 57 minutes remaining");
    CHECK(battery_tooltip(P::CHARGING, 52, 70, false) == "Battery 52% · charging · 1 hour 10 minutes until full");
    CHECK(battery_tooltip(P::CHARGING, 52, -1, true) == "Battery 52% · charging · calculating…");
    CHECK(battery_tooltip(P::LOW, 15, 30, false) == "Battery 15% · low · 30 minutes remaining");
    CHECK(battery_tooltip(P::FULL, 100, -1, false) == "Battery 100% · fully charged");
    // Reduced motion: the Effects page, low resource mode, no animations.
    CHECK(battery_motion("auto", "auto", false, false, true));
    CHECK(!battery_motion("auto", "false", false, false, true));
    CHECK(!battery_motion("auto", "auto", true, false, true));
    CHECK(!battery_motion("auto", "true", false, true, true));
    CHECK(!battery_motion("", "auto", false, false, false));
    CHECK(battery_motion("true", "false", true, true, false) && !battery_motion("false", "true", false, false, true));
    CHECK(battery_animated(P::CHARGING) && battery_animated(P::LOW) && battery_animated(P::CRITICAL));
    CHECK(!battery_animated(P::DISCHARGING) && !battery_animated(P::FULL) && !battery_animated(P::PLUGGED));
    CHECK(battery_pulse_period(P::CRITICAL) < battery_pulse_period(P::LOW));

    // Estimates. UPower's own times win.
    BatteryEstimator est;
    BatteryReading r;
    r.phase = P::DISCHARGING;
    r.percent = 45, r.energy = 39.55, r.energy_full = 88, r.rate = 10, r.time_to_empty = 14220;
    est.update(r);
    CHECK(est.minutes() == 237 && !est.calculating());
    // Without them: the energy over the rate (39.55 Wh at 10 W).
    r.time_to_empty = 0;
    est.update(r);
    CHECK(est.minutes() == 237);
    // A short burst of load (30 s at 30 W) moves the estimate gently...
    double t = 0;
    for (int i = 0; i < 5; ++i) {
      r.t = t += 30;
      est.update(r);
    }
    CHECK(est.minutes() == 237);
    r.t = t += 30, r.rate = 30;
    est.update(r);
    int burst = est.minutes();
    CHECK(burst < 237 && burst > 237 * 0.7);
    // ...and comes back when the load is gone.
    r.rate = 10;
    for (int i = 0; i < 30; ++i) {
      r.t = t += 30;
      est.update(r);
    }
    CHECK(std::abs(est.minutes() - 237) <= 2);
    // A lasting change is followed within minutes (79 min at 30 W).
    r.rate = 30;
    for (int i = 0; i < 30; ++i) {
      r.t = t += 30;
      est.update(r);
    }
    CHECK(std::abs(est.minutes() - 79) <= 2);
    // Plugged in: charging starts over, "Calculating…" until a rate comes.
    r.phase = P::CHARGING, r.rate = 0, r.t = t += 30;
    est.update(r);
    CHECK(est.minutes() == -1 && est.calculating());
    // The rate arrives: (88 - 39.55) Wh at 24 W, 121 minutes; UPower's
    // TimeToFull would win again.
    r.rate = 24, r.t = t += 30;
    est.update(r);
    CHECK(est.minutes() == 121 && !est.calculating());
    r.time_to_full = 4200;
    est.update(r);
    CHECK(est.minutes() == 70);
    r.time_to_full = 0;
    // Fully charged and plugged in: no estimate, nothing to calculate.
    r.phase = P::FULL, r.t = t += 30;
    est.update(r);
    CHECK(est.minutes() == -1 && !est.calculating());
    r.phase = P::PLUGGED;
    est.update(r);
    CHECK(est.minutes() == -1 && !est.calculating());
    // No rate reported: from the energy that moved over at least a minute
    // (0.5 Wh in 3 minutes is 10 W; 39 Wh left: 234 minutes).
    BatteryEstimator slow;
    BatteryReading s;
    s.phase = P::DISCHARGING, s.energy = 39.5, s.energy_full = 88, s.t = 0;
    slow.update(s);
    CHECK(slow.minutes() == -1 && slow.calculating());
    s.energy = 39.25, s.t = 90;
    slow.update(s);
    s.energy = 39.0, s.t = 180;
    slow.update(s);
    CHECK(std::abs(slow.minutes() - 234) <= 1);
    // Only the percentage (no energies): 1 % in 6 minutes, 45 % left.
    BatteryEstimator pct;
    BatteryReading q;
    q.phase = P::DISCHARGING, q.percent = 46, q.t = 0;
    pct.update(q);
    q.percent = 45, q.t = 360;
    pct.update(q);
    CHECK(pct.minutes() == 270);
    // Nothing usable for ten minutes: no more "Calculating…".
    BatteryEstimator none;
    BatteryReading n;
    n.phase = P::CHARGING, n.percent = 50, n.t = 0;
    none.update(n);
    CHECK(none.calculating());
    n.t = 601;
    none.update(n);
    CHECK(!none.calculating() && none.minutes() == -1);
    // More than two days is not an estimate.
    BatteryEstimator idle;
    BatteryReading z;
    z.phase = P::DISCHARGING, z.energy = 88, z.energy_full = 88, z.rate = 0.5;
    idle.update(z);
    CHECK(idle.minutes() == -1);

    // Animation: the level eases to a new percentage, charging sweeps,
    // low and critical pulse; reduced motion keeps everything still.
    BatteryAnimation anim;
    anim.set(45, P::DISCHARGING, true, 10);
    CHECK(anim.frame(10).level == 45 && anim.frame(10).sweep < 0 && anim.frame(10).pulse == 1);
    CHECK(!anim.running(10));
    anim.set(60, P::DISCHARGING, true, 20);
    double mid = anim.frame(20.3).level;
    CHECK(mid > 45 && mid < 60 && anim.running(20.3));
    CHECK(anim.frame(21).level == 60 && !anim.running(21));
    anim.set(60, P::CHARGING, true, 30);
    CHECK(anim.running(30) && anim.running(500));
    CHECK(anim.frame(30.5).sweep > 0.2 && anim.frame(30.5).sweep < 0.3);
    CHECK(anim.frame(32.5).sweep > 0.2 && anim.frame(32.5).sweep < 0.3);  // a 2 s period
    anim.set(15, P::LOW, true, 40);
    double lo = 1, hi = 0;
    for (double u = 41; u < 43; u += 0.05) {
      lo = std::min(lo, anim.frame(u).pulse);
      hi = std::max(hi, anim.frame(u).pulse);
    }
    CHECK(lo < 0.5 && hi > 0.95 && anim.frame(41).sweep < 0);
    anim.set(15, P::LOW, false, 50);
    CHECK(!anim.running(50) && anim.frame(50).pulse == 1);
    anim.set(30, P::CHARGING, false, 60);
    CHECK(anim.frame(60).level == 30 && anim.frame(60).sweep < 0 && !anim.running(60));
  }
  if (failures) return 1;
  printf("Panel status rules (tooltips, LED, keyboard, drives, touchpad, battery, previews) passed\n");
  return 0;
}
