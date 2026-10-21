// SPDX-License-Identifier: GPL-3.0-or-later
// Graphics card detection for Settings > About: PCI display controllers from
// sysfs, named with the pci.ids database (or "lspci -mm" when it is not
// installed), the kernel driver bound to each and the OpenGL renderer.
// Nothing here touches FLTK, so it is unit tested with fixtures.
#pragma once

#include <istream>
#include <string>
#include <vector>

namespace sde {

struct Gpu {
  std::string slot;       // "0000:01:00.0"
  std::string vendor_id;  // "10de" (lower-case hex, no 0x)
  std::string device_id;  // "2484"
  std::string vendor;     // "NVIDIA Corporation"
  std::string model;      // "GA104 [GeForce RTX 3070]"
  std::string driver;     // "nvidia", "" when no driver is bound
  bool boot_vga = false;  // the firmware's primary display
};

struct PciName {
  std::string vendor, device;
};

// Looks |vendor_id|/|device_id| (hex, any case, with or without 0x) up in a
// pci.ids stream. Missing names are left empty.
PciName lookup_pci_ids(std::istream &ids, const std::string &vendor_id,
                       const std::string &device_id);
// Same, reading the first existing database: $SDE_PCI_IDS,
// /usr/share/misc/pci.ids, /usr/share/hwdata/pci.ids.
PciName lookup_pci_ids(const std::string &vendor_id, const std::string &device_id);
std::string pci_ids_path();

// Parses one line of "lspci -mm" (quoted fields: slot "class" "vendor"
// "device" ...). Returns false when the line has too few fields.
bool parse_lspci_mm(const std::string &line, PciName &out);

// "Advanced Micro Devices, Inc. [AMD/ATI]" -> "AMD/ATI"; others unchanged.
std::string short_vendor(const std::string &vendor);
// Names for common vendors when no database is available.
std::string known_vendor(const std::string &vendor_id);
// "AMD/ATI Navi 23 [Radeon RX 6600/6600 XT/6600M]" style label.
std::string gpu_label(const Gpu &gpu);

// Display controllers (PCI class 0x03xxxx) below |sysfs| (default
// $SDE_PCI_SYSFS or /sys/bus/pci/devices), primary first. Names come from
// pci.ids, then "lspci -mm" when |use_lspci|, then known_vendor().
std::vector<Gpu> detect_gpus(const std::string &sysfs = "", bool use_lspci = true);

// "OpenGL renderer string: X" from "glxinfo -B" (or "eglinfo -B") output.
std::string parse_gl_renderer(const std::string &glxinfo_output);
// Index of the card that draws |renderer| (by vendor name), or -1 (also for
// software rendering such as llvmpipe).
int renderer_gpu(const std::vector<Gpu> &gpus, const std::string &renderer);

}  // namespace sde
