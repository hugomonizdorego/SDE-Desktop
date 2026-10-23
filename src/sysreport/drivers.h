// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// SDE Driver Manager model (sde-drivers): finds hardware that needs
// non-free firmware or drivers on Debian and the packages that provide them.
//
//   * graphics cards (PCI vendor IDs, nvidia-detect when it is installed)
//   * "firmware: failed to load ..." messages of the kernel log, mapped to
//     Debian's firmware-* packages
//   * CPU microcode (intel-microcode, amd64-microcode)
//   * isenkram-lookup suggestions when isenkram-cli is installed
//   * whether the APT sources enable the non-free-firmware component, and a
//     safe rewrite of a sources file that adds it
//
// Nothing here touches FLTK, so the parsers are unit tested with fixtures.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace sde {

struct Gpu;

namespace drivers {

// One recommended package.
struct Recommendation {
  std::string package;    // "firmware-iwlwifi"
  std::string device;     // "Intel Wi-Fi (iwlwifi-ty-a0-gf-a0-72.ucode)"
  std::string reason;     // why it is recommended
  std::string component;  // Debian archive area: "main", "contrib", "non-free", "non-free-firmware"
  bool installed = false;
  // The preferred choice for its device. False for alternatives such as
  // firmware for nouveau next to nvidia-driver, or a DKMS driver that was
  // installed by hand (since 0.1.0~alpha51).
  bool recommended = true;
};

// File names from "firmware: failed to load X (-2)", "Direct firmware load
// for X failed with error -2" and "... failed to load firmware X" lines,
// without duplicates, in the order they appear.
std::vector<std::string> parse_firmware_failures(const std::string &log);

// Debian package that ships a firmware file, e.g. "iwlwifi-cc-a0-50.ucode"
// -> "firmware-iwlwifi", "rtl_nic/rtl8168h-2.fw" -> "firmware-realtek".
// Unknown files map to "firmware-misc-nonfree".
std::string firmware_package(const std::string &file);
// Human readable device family of a firmware file ("Intel Wi-Fi", ...).
std::string firmware_device(const std::string &file);

// Recommendations for graphics cards from their PCI vendor IDs and bound
// drivers. |nvidia_detect| is the output of nvidia-detect ("" when it is
// not installed).
std::vector<Recommendation> gpu_recommendations(const std::vector<Gpu> &gpus,
                                                const std::string &nvidia_detect = "");
// The package nvidia-detect recommends ("nvidia-driver",
// "nvidia-tesla-470-driver", ...) or "".
std::string parse_nvidia_detect(const std::string &output);

// "intel-microcode" / "amd64-microcode" for the vendor_id of /proc/cpuinfo,
// "" for other CPUs (and in virtual machines, where |virtual_machine|).
std::string microcode_package(const std::string &cpuinfo, bool virtual_machine = false);

// Package names printed by "isenkram-lookup" (one per line).
std::vector<std::string> parse_isenkram(const std::string &output);

// "dpkg-query -W -f='${Package} ${Status}\n' a b c" output -> installed
// packages ("install ok installed").
std::vector<std::string> parse_dpkg_installed(const std::string &output);

// APT sources. |text| is a one-line style sources.list or a deb822 .sources
// file (|deb822|). Only Debian archive entries count (deb.debian.org,
// security.debian.org, ftp.*.debian.org, debian mirrors with /debian).
bool sources_have_component(const std::string &text, bool deb822, const std::string &component);
// |text| with |components| appended to every Debian entry that lacks them.
// Comments, other repositories, options and line order are kept. Returns
// |text| unchanged when nothing had to change.
std::string enable_components(const std::string &text, bool deb822,
                              const std::vector<std::string> &components);

// Merges recommendations by package (first device wins, reasons joined).
std::vector<Recommendation> merge(const std::vector<Recommendation> &list);

// ---- Devices and driver choices (Driver Manager window, 0.1.0~alpha51)
//
// The window shows one row per device with its driver packages as radio
// choices plus "Do not use the device", like Linux Mint's Driver Manager.
// Recommendations are grouped by device here; the selection model computes
// what to install and remove. Display-free, unit tested.

enum class DeviceKind { Graphics, Wifi, Bluetooth, Network, Audio, Processor, Other };

// A PCI device of /sys/bus/pci/devices (or $SDE_PCI_SYSFS).
struct PciDevice {
  std::string slot;       // "0000:02:00.0"
  std::string vendor_id;  // "10ec" (lower-case hex, no 0x)
  std::string device_id;  // "b852"
  std::string class_id;   // "028000"
  std::string vendor;     // "Realtek Semiconductor Co., Ltd."
  std::string model;      // "RTL8852BE PCIe 802.11ax Wireless Network Controller"
  std::string driver;     // bound kernel driver, "" when none
};
// Network, Bluetooth and audio controllers below |sysfs| (default
// $SDE_PCI_SYSFS or /sys/bus/pci/devices), named from pci.ids.
std::vector<PciDevice> detect_pci_devices(const std::string &sysfs = "");
// PCI class -> kind: 0280 Wi-Fi, 02xx network, 0d11 Bluetooth, 0403 audio,
// 03xx graphics.
DeviceKind pci_kind(const std::string &class_id);
// The kind of device a recommendation is for (from its package and device text).
DeviceKind device_kind(const Recommendation &r);

// Hardware DKMS packages: "rtw89-dkms" -> "Realtek Wi-Fi". "" for packages
// that are not drivers for a device (virtualbox-dkms, zfs-dkms ...) and for
// nvidia's own DKMS modules (part of nvidia-driver).
std::string dkms_device(const std::string &package);
// Firmware and microcode packages: kept when the user switches to another
// driver of the same device, since that driver usually needs them too.
bool is_firmware_package(const std::string &package);
// Free software (shown "open-source") or not ("proprietary"): main and
// DKMS packages are free, non-free and non-free-firmware are not.
bool is_free_driver(const std::string &package, const std::string &component);

struct DriverOption {
  std::string package;
  std::string version;  // installed version, else the candidate; "" unknown
  std::string summary;  // one line (apt's description, else the reason)
  bool free_software = false;
  bool recommended = false;
  bool installed = false;
};

struct Device {
  DeviceKind kind = DeviceKind::Other;
  std::string name;                   // vendor and model
  std::vector<DriverOption> options;  // recommended first
};

struct AptVersions {
  std::string installed, candidate;  // "" for "(none)"
};

// Groups recommendations into devices. A recommendation goes to the PCI
// device of the same kind and vendor when there is one (so it is named
// "Realtek Semiconductor Co., Ltd.: RTL8852BE ..."); otherwise its device
// text names it. |versions| and |summaries| come from apt (may be empty).
std::vector<Device> group_devices(const std::vector<Recommendation> &recs,
                                  const std::vector<PciDevice> &pci,
                                  const std::map<std::string, AptVersions> &versions = {},
                                  const std::map<std::string, std::string> &summaries = {});

// The selection: per device the chosen option index, or kNoDriver for
// "Do not use the device".
constexpr int kNoDriver = -1;
// What is installed now: the installed driver (a non-firmware package
// first), else the installed firmware, else kNoDriver.
std::vector<int> initial_selection(const std::vector<Device> &devices);

enum class DeviceState { Recommended, Alternative, NoDriver };
DeviceState device_state(const Device &d, int selected);

struct Changes {
  std::vector<std::string> install, remove;
  bool empty() const { return install.empty() && remove.empty(); }
};
// Packages to install and remove to go from |initial| to |selection|. Only
// devices whose choice changed count. The chosen package is installed and
// the device's other installed packages are removed, except firmware, which
// is kept unless the user chose "Do not use the device".
Changes plan_changes(const std::vector<Device> &devices, const std::vector<int> &initial,
                     const std::vector<int> &selection);
// apt-get arguments: install -y A B C- (a trailing "-" removes a package in
// the same transaction). Empty when nothing changes, or when a name is not
// a valid Debian package name (valid_package_name).
std::vector<std::string> apt_arguments(const Changes &changes);
bool valid_package_name(const std::string &name);
// Installed proprietary packages (for "N proprietary drivers in use").
int proprietary_in_use(const std::vector<Device> &devices);

// "apt-cache policy A B" -> installed and candidate versions.
std::map<std::string, AptVersions> parse_apt_policy(const std::string &output);
// "apt-cache show --no-all-versions A B" -> package -> short description.
std::map<std::string, std::string> parse_apt_summaries(const std::string &output);
// One line of apt's status descriptor (APT::Status-Fd):
// "dlstatus:1:12.5:Downloading x", "pmstatus:pkg:42.0:Installing pkg".
// Returns false for other lines. Downloads are the first 30 %.
bool parse_apt_status(const std::string &line, double *percent, std::string *text);
// "model name : Intel(R) Core(TM) i5-8250U ..." of /proc/cpuinfo, or "".
std::string cpu_model_name(const std::string &cpuinfo);

// Everything above for this computer: reads /sys, the kernel log (journalctl
// -k or dmesg), /proc/cpuinfo, runs nvidia-detect / isenkram-lookup when
// installed and marks installed packages. Takes a second or two.
struct Scan {
  std::vector<Recommendation> items;
  std::vector<std::string> gpus;    // labels of the graphics cards
  std::string sources_file;         // the main Debian sources file
  bool sources_deb822 = false;
  bool non_free_firmware = false;   // enabled in the APT sources
  bool contrib_non_free = false;    // contrib and non-free enabled
  bool kernel_log_readable = true;  // false when dmesg/journal were refused
  bool isenkram = false;            // isenkram-lookup was used
  std::vector<Device> devices;      // |items| grouped by device (alpha51)
};
Scan scan_system();

// The Debian sources file to edit: /etc/apt/sources.list.d/debian.sources
// when it exists, otherwise /etc/apt/sources.list. $SDE_APT_SOURCES
// overrides it (tests).
std::string main_sources_file(bool *deb822 = nullptr);

}  // namespace drivers
}  // namespace sde
