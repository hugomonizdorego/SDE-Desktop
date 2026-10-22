// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
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
};
Scan scan_system();

// The Debian sources file to edit: /etc/apt/sources.list.d/debian.sources
// when it exists, otherwise /etc/apt/sources.list. $SDE_APT_SOURCES
// overrides it (tests).
std::string main_sources_file(bool *deb822 = nullptr);

}  // namespace drivers
}  // namespace sde
