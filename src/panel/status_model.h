// SPDX-License-Identifier: GPL-3.0-or-later
// Display-free rules of the panel's status area (alpha50), unit tested in
// tests/test_panel_status.cpp: tray item names, the network LED, keyboard
// indicator de-duplication, removable drives from UDisks2 and touchpad
// properties.
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
// A sentence for the Network flyout ("" for ok/unknown/none).
std::string network_verdict_text(const std::string &verdict);

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
  std::vector<std::string> mount_points;
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

