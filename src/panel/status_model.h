// SPDX-License-Identifier: GPL-3.0-or-later
// Display-free rules of the panel's status area (alpha50), unit tested in
// tests/test_panel_status.cpp: tray item names, the network LED, keyboard
// indicator de-duplication, removable drives from UDisks2, touchpad
// properties and (0.2 Alpha 2) the battery indicator: its phases, panel
// text, time estimates and animation.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "tooltip_text.h"

// ----------------------------------------------------------------- tray names

// Name of a StatusNotifier item: its Title, else the ToolTip title, Id or bus
// name (":1.42" and "org.kde.StatusNotifierItem-<pid>-<n>" only as a last
// resort). Printable and never empty.
std::string tray_item_name(const std::string &title, const std::string &tooltip,
                           const std::string &id, const std::string &service);

// ---------------------------------------------------------------- network LED

// The dot on the panel's network icon: red without an active connection,
// green with internet access, yellow when the connection only reaches the
// local network (NetworkManager Connectivity NONE/PORTAL/LIMITED while a
// connection is active). UNKNOWN with a connection counts as green unless a
// quick probe of our own failed.
// |verdict| is debion-network-check's (--quick --json): "" when it is not
// installed or has not answered yet; anything but "ok"/"unknown" (no-dns,
// proxy-required, clock, tls-intercepted, ...) means the browser will not
// reach the internet although NetworkManager may say FULL.
enum class NetLed { RED, YELLOW, GREEN };
NetLed network_led(bool connected, int connectivity, bool probe_failed,
                   const std::string &verdict = "");
// The "verdict" of debion-network-check --quick --json output ("" if none).
std::string parse_network_verdict(const std::string &json);
// A sentence for the Network flyout ("" for ok/unknown/none). The panel's
// own verdict "checking" (the first check of a connection has not answered
// yet) is yellow too: NetworkManager reports FULL as soon as its own check
// passes, before the browser can resolve a single name.
std::string network_verdict_text(const std::string &verdict);
// A boolean member of a flat JSON object (|fallback| when absent).
bool parse_json_bool(const std::string &json, const std::string &key, bool fallback = false);
// Whether debion-network-check's repair is likely to help: DNS fails, or
// --quick reported "repairable" (an unmanaged device, ifupdown DHCP, a
// resolv.conf nobody writes) for a network without route or internet.
bool network_verdict_fixable(const std::string &verdict, bool repairable);
// The outcome of a --fix/--boot-repair run for the flyout: "ok" when DNS
// works afterwards, the line that says what still fails, "done" for a
// successful run without a DNS line, else the last line.
std::string network_repair_summary(const std::string &output, int status);

// ------------------------------------------------------- keyboard indicators

// What an application tray icon (SNI Id/Title/bus name, XEmbed WM_CLASS) is
// when it shows the keyboard: an input method framework (IBus, Fcitx, ...)
// or a plain XKB layout applet (gxkb, xxkb, ...).
enum class KeyboardApplet { NONE, INPUT_METHOD, LAYOUT };
KeyboardApplet keyboard_applet(const std::string &name);

// --------------------------------------------------------------------- drives

// One block device as UDisks2 describes it (org.freedesktop.UDisks2.Block,
// .Filesystem, .Partition) and one drive (.Drive).
struct UdisksBlock {
  std::string object, device, drive, label, fstype, usage, hint_name, crypto_backing;
  long long size = 0;
  bool hint_ignore = false, hint_system = false, hint_auto = false;
  bool filesystem = false, partition = false, partition_table = false;
  std::vector<std::string> mount_points{};
};
struct UdisksDrive {
  std::string object, vendor, model, connection_bus, media;
  long long size = 0;
  int rotation_rate = -1;
  bool removable = false, media_removable = false, ejectable = false, can_power_off = false;
};
struct DrivePartition {
  std::string path, label, mountpoint, fstype;
  std::string object;  // UDisks2 block object (empty from lsblk)
  long long size = 0;
  bool automount = false;  // UDisks2 HintAuto
};
// A USB stick, memory card or external disk (not the system disk).
struct RemovableDrive {
  std::string name, path, label, model, kind, icon;  // kind: usb, card, hdd, ssd
  std::string object;  // UDisks2 drive object (empty from lsblk)
  long long size = 0;
  bool can_power_off = false, ejectable = false;
  std::vector<DrivePartition> parts;
  std::string mountpoint() const;
  bool mounted() const { return !mountpoint().empty(); }
};
// The removable drives among UDisks2's objects, the way GNOME and Cinnamon
// pick them: a drive that is removable, has removable media or sits on USB,
// SDIO or FireWire, with at least one filesystem that is not a system or
// ignored one. Unmounted filesystems count too (they can be mounted).
std::vector<RemovableDrive> udisks_removable_drives(const std::vector<UdisksBlock> &blocks,
                                                    const std::vector<UdisksDrive> &drives);
// Parses `lsblk -P -b -o NAME,PATH,LABEL,SIZE,TYPE,TRAN,RM,HOTPLUG,ROTA,
// MOUNTPOINT,FSTYPE,MODEL,VENDOR,PKNAME` (the fallback without UDisks2).
std::vector<RemovableDrive> parse_removable_drives(const std::string &text);
std::string human_size(long long bytes);
// The processes that keep files open below |mountpoint|, from `fuser -vm`
// or `lsof -Fc` output: program names without duplicates.
std::vector<std::string> parse_busy_programs(const std::string &text);

// ------------------------------------------------------- battery (alpha51)

// "3 hours 57 minutes", "1 hour", "45 minutes"; "" for an unknown time.
std::string battery_duration_text(int minutes);
// The second line of the battery flyout: "Using battery power - 3 hours 57
// minutes remaining", "Charging - 1 hour 10 minutes until full", "Fully
// charged", "Charging" or "Using battery power".
std::string battery_status_line(bool charging, bool full, int minutes);

// ------------------------------------------- battery indicator (0.2 Alpha 2)

// What the panel's battery icon shows (docs/PANEL.md): on battery (normal,
// low, critical), charging, fully charged on AC, or plugged in without
// charging (a charge limit; UPower "pending-charge").
enum class BatteryPhase { DISCHARGING, LOW, CRITICAL, CHARGING, FULL, PLUGGED };
// From UPower's DisplayDevice: |state| is Device.State (1 charging,
// 2 discharging, 3 empty, 4 fully charged, 5 pending charge, 6 pending
// discharge, 0 unknown: then |on_battery|, UPower's OnBattery, decides) and
// |warning| Device.WarningLevel (3 low, 4 critical, 5 action). On battery
// the level is low at |low| percent or less (or with WarningLevel low) and
// critical at |critical| percent or less (or WarningLevel critical/action).
BatteryPhase battery_phase(int state, double percent, int warning, bool on_battery, int low = 20,
                           int critical = 10);
// The UPower state of a kernel power_supply "status" ("Charging",
// "Discharging", "Full", "Not charging", "Unknown"); |ac_online|: a mains
// adapter reports online=1.
int battery_state_from_sysfs(const std::string &status, double percent, bool ac_online);
// "discharging", "low", "critical", "charging", "full", "plugged" (UI tests).
const char *battery_phase_name(BatteryPhase phase);
bool battery_on_battery(BatteryPhase phase);  // discharging, low or critical
// The level thresholds: 20 % and 10 %, or the [power] battery_low and
// battery_critical warnings of sde-power when they are higher.
int battery_low_level(int power_low);
int battery_critical_level(int power_critical);

// [panel] battery_display: what follows the icon on the panel.
enum class BatteryDisplay { ICON, PERCENT, LABEL, LABEL_PERCENT };
BatteryDisplay parse_battery_display(const std::string &name);  // unknown: ICON
const char *battery_display_name(BatteryDisplay display);  // icon, percent, label, label-percent
// The state as a short label: "On battery", "Low battery", "Critically
// low", "Charging", "Full", "Plugged in".
std::string battery_label(BatteryPhase phase);
// "3 h 57 min", "45 min", "1 h"; "" for an unknown time.
std::string battery_duration_short(int minutes);
// The estimate for a label: "3 h 57 min left", "1 h 10 min to full",
// "Calculating…" while it is not known yet; "" when there is none.
std::string battery_time_label(BatteryPhase phase, int minutes, bool calculating);
// The text after the panel's battery icon ("" for ICON): "45%", "Charging",
// "On battery · 45%". With |time| ([panel] battery_time) a label shows the
// estimate instead of the state when there is one: "3 h 57 min left · 45%".
std::string battery_panel_text(BatteryDisplay display, BatteryPhase phase, int percent, int minutes,
                               bool calculating, bool time);
// The flyout's state line and the menu's header for a phase:
// "Using battery power - 3 hours 57 minutes remaining", "Low battery - 25
// minutes remaining", "Charging - Calculating…", "Plugged in, not charging".
std::string battery_phase_line(BatteryPhase phase, int minutes, bool calculating);
// The icon's tooltip: "Battery 45% · 3 hours 57 minutes remaining",
// "Battery 52% · charging · 1 hour 10 minutes until full", "Battery 52% ·
// charging · calculating…", "Battery 100% · fully charged".
std::string battery_tooltip(BatteryPhase phase, int percent, int minutes, bool calculating);
// [panel] battery_animation: auto (default) animates unless the Effects
// page turned effects off ([effects] enabled=false), [appearance]
// low_resource or reduced_motion is set, or [aero] animations=false.
bool battery_motion(const std::string &mode, const std::string &effects, bool low_resource,
                    bool reduced_motion, bool animations);
// The animation of |phase| (with motion): the charging sweep (period 2 s),
// the low pulse (2 s) and the critical pulse (1.2 s); none otherwise.
bool battery_animated(BatteryPhase phase);
double battery_pulse_period(BatteryPhase phase);

// One frame of the battery icon: the level shown (it eases to a new
// percentage), the charging sweep (0-1 through its period, -1 none) and
// the strength of the low/critical colours (|pulse| 0-1, 1 steady).
struct BatteryFrame {
  double level = 0, sweep = -1, pulse = 1;
};
// The icon's animation over time |t| (seconds): set() with every new state,
// frame() for each repaint; running() says whether more frames are needed,
// so that the panel stops its timer when nothing moves. Without |motion|
// (reduced motion) the level jumps and nothing pulses.
class BatteryAnimation {
 public:
  static constexpr double kEase = 0.8;  // seconds from one level to the next
  void set(int percent, BatteryPhase phase, bool motion, double t);
  BatteryFrame frame(double t) const;
  bool running(double t) const;
  BatteryPhase phase() const { return phase_; }

 private:
  bool started_ = false, motion_ = true;
  BatteryPhase phase_ = BatteryPhase::DISCHARGING;
  double from_ = 0, to_ = 0, since_ = 0, start_ = 0;
};

// One reading of the battery for the time estimate. Energies and the rate
// may be in any unit (Wh and W from UPower, µWh and µW or µAh and µA from
// sysfs) as long as they match; 0 means unknown.
struct BatteryReading {
  double t = 0;  // seconds on a monotonic clock
  BatteryPhase phase = BatteryPhase::DISCHARGING;
  double percent = 0;
  double energy = 0, energy_full = 0, rate = 0;
  long long time_to_empty = 0, time_to_full = 0;  // UPower's estimates, seconds
};
// The time until empty (on battery) or full (charging): UPower's own
// TimeToEmpty/TimeToFull when it has them; otherwise the energy left (or
// still missing) over the rate. The rate is smoothed (exponentially, time
// constant kRateTau), so that a burst of load moves the estimate gently
// instead of making it jump; without a reported rate it comes from how fast
// the energy, or else the percentage, changes. Switching between charging
// and discharging starts over: "Calculating…" until there is a rate, for
// at most kCalculatingFor seconds.
class BatteryEstimator {
 public:
  static constexpr double kRateTau = 150, kCalculatingFor = 600, kMaxHours = 48;
  void update(const BatteryReading &r);
  void reset();
  int minutes() const { return minutes_; }  // -1: unknown
  bool calculating() const { return calculating_; }
  double rate() const { return rate_; }  // smoothed, in the reading's unit per hour

 private:
  bool started_ = false, calculating_ = false;
  BatteryPhase phase_ = BatteryPhase::DISCHARGING;
  double since_ = 0, last_t_ = 0, rate_ = 0;
  // Energy (or percentage) samples for a rate the battery does not report.
  double mark_t_ = -1, mark_energy_ = 0;
  int minutes_ = -1;
};
// The battery's name from sysfs: "<manufacturer> <model_name>", either part
// when the other is missing, "" when both are.
std::string battery_model_name(const std::string &manufacturer, const std::string &model);
// power-profiles-daemon profiles in the applet's order: Power Saver,
// Balanced, Performance, then any other profile as reported.
std::vector<std::string> ordered_power_profiles(const std::vector<std::string> &profiles);

// ------------------------------------------------ taskbar previews (alpha51)

struct PreviewSize {
  int w = 0, h = 0;
};
// A window of |win_w| x |win_h| scaled to fit |max_w| x |max_h| keeping its
// aspect ratio (never enlarged, at least 1x1).
PreviewSize preview_fit(int win_w, int win_h, int max_w, int max_h);
// Width of each of |count| thumbnails side by side with |gap| between them
// so that the row fits |available| (at most |preferred|, at least |minimum|).
int preview_column_width(int count, int preferred, int minimum, int gap, int available);

