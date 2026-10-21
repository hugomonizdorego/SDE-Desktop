// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <fstream>
#include <sstream>

#include "process.h"
#include "util.h"

namespace sde {

namespace {

// "0x10DE\n" -> "10de"
std::string norm_id(std::string id) {
  id = lower(trim(id));
  if (starts_with(id, "0x")) id = id.substr(2);
  return id;
}

bool is_hex4(const std::string &s, size_t at) {
  if (s.size() < at + 4) return false;
  for (size_t i = at; i < at + 4; ++i)
    if (!isxdigit(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

std::string read_value(const std::string &path) {
  std::string s;
  read_file(path, s);
  return trim(s);
}

}  // namespace

PciName lookup_pci_ids(std::istream &ids, const std::string &vendor_id,
                       const std::string &device_id) {
  PciName out;
  std::string v = norm_id(vendor_id), d = norm_id(device_id);
  std::string line;
  bool in_vendor = false;
  while (std::getline(ids, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    // The device classes section ("C 03  Display controller") ends the
    // vendor list.
    if (line[0] == 'C' && line.size() > 1 && line[1] == ' ') break;
    if (line[0] != '\t') {
      if (in_vendor) break;  // past our vendor without finding the device
      if (is_hex4(line, 0) && lower(line.substr(0, 4)) == v) {
        in_vendor = true;
        out.vendor = trim(line.substr(4));
      }
      continue;
    }
    if (!in_vendor || line.size() < 2 || line[1] == '\t') continue;  // subsystems
    if (is_hex4(line, 1) && lower(line.substr(1, 4)) == d) {
      out.device = trim(line.substr(5));
      break;
    }
  }
  return out;
}

std::string pci_ids_path() {
  std::vector<std::string> paths;
  std::string e = env("SDE_PCI_IDS");
  if (!e.empty()) paths.push_back(e);
  paths.push_back("/usr/share/misc/pci.ids");
  paths.push_back("/usr/share/hwdata/pci.ids");
  for (auto &p : paths)
    if (file_exists(p)) return p;
  return "";
}

PciName lookup_pci_ids(const std::string &vendor_id, const std::string &device_id) {
  std::string path = pci_ids_path();
  if (path.empty()) return {};
  std::ifstream in(path);
  return lookup_pci_ids(in, vendor_id, device_id);
}

bool parse_lspci_mm(const std::string &line, PciName &out) {
  // 01:00.0 "VGA compatible controller" "NVIDIA Corporation" "GA104 [GeForce RTX 3070]" -r01 ...
  std::vector<std::string> fields;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size()) break;
    std::string f;
    if (line[i] == '"') {
      ++i;
      while (i < line.size() && line[i] != '"') f += line[i++];
      ++i;
    } else {
      while (i < line.size() && line[i] != ' ') f += line[i++];
    }
    fields.push_back(f);
  }
  // Options such as -r01 or -p00 are not quoted values; drop them.
  std::vector<std::string> values;
  for (size_t k = 0; k < fields.size(); ++k)
    if (k < 4 || !starts_with(fields[k], "-")) values.push_back(fields[k]);
  if (values.size() < 4) return false;
  out.vendor = values[2];
  out.device = values[3];
  return true;
}

std::string short_vendor(const std::string &vendor) {
  size_t open = vendor.rfind('['), close = vendor.rfind(']');
  if (open != std::string::npos && close != std::string::npos && close > open + 1 &&
      close == vendor.size() - 1)
    return vendor.substr(open + 1, close - open - 1);
  return vendor;
}

std::string known_vendor(const std::string &vendor_id) {
  static const std::pair<const char *, const char *> table[] = {
      {"8086", "Intel Corporation"},
      {"10de", "NVIDIA Corporation"},
      {"1002", "AMD/ATI"},
      {"1022", "AMD"},
      {"1a03", "ASPEED Technology"},
      {"102b", "Matrox"},
      {"5143", "Qualcomm"},
      {"1af4", "Red Hat (virtio)"},
      {"1b36", "Red Hat (QXL)"},
      {"1234", "QEMU"},
      {"15ad", "VMware"},
      {"80ee", "VirtualBox"},
      {"1414", "Microsoft (Hyper-V)"},
      {"1013", "Cirrus Logic"},
  };
  std::string v = norm_id(vendor_id);
  for (auto &t : table)
    if (v == t.first) return t.second;
  return "";
}

std::string gpu_label(const Gpu &gpu) {
  std::string vendor = short_vendor(gpu.vendor);
  if (vendor.empty()) vendor = gpu.vendor_id.empty() ? "" : "[" + gpu.vendor_id + "]";
  std::string model = gpu.model.empty() ? "[" + gpu.device_id + "]" : gpu.model;
  if (vendor.empty()) return model;
  return vendor + " " + model;
}

std::vector<Gpu> detect_gpus(const std::string &sysfs_in, bool use_lspci) {
  std::string sysfs = sysfs_in;
  if (sysfs.empty()) sysfs = env("SDE_PCI_SYSFS", "/sys/bus/pci/devices");
  std::vector<Gpu> out;
  std::vector<std::string> slots = list_dir(sysfs);
  std::sort(slots.begin(), slots.end());
  for (auto &slot : slots) {
    std::string dir = sysfs + "/" + slot;
    std::string cls = norm_id(read_value(dir + "/class"));
    if (cls.size() != 6 || !starts_with(cls, "03")) continue;
    Gpu g;
    g.slot = slot;
    g.vendor_id = norm_id(read_value(dir + "/vendor"));
    g.device_id = norm_id(read_value(dir + "/device"));
    g.boot_vga = read_value(dir + "/boot_vga") == "1";
    char buf[PATH_MAX];
    ssize_t n = readlink((dir + "/driver").c_str(), buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = 0;
      g.driver = basename_of(buf);
    }
    PciName name = lookup_pci_ids(g.vendor_id, g.device_id);
    if ((name.vendor.empty() || name.device.empty()) && use_lspci &&
        !find_program("lspci").empty()) {
      PciName l;
      std::string slot_arg = slot;
      if (parse_lspci_mm(trim(run_sync({"lspci", "-mm", "-s", slot_arg}, nullptr, 2000)), l)) {
        if (name.vendor.empty()) name.vendor = l.vendor;
        if (name.device.empty()) name.device = l.device;
      }
    }
    g.vendor = name.vendor.empty() ? known_vendor(g.vendor_id) : name.vendor;
    g.model = name.device;
    out.push_back(g);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Gpu &a, const Gpu &b) { return a.boot_vga && !b.boot_vga; });
  return out;
}

std::string parse_gl_renderer(const std::string &text) {
  std::istringstream in(text);
  std::string line, fallback;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (starts_with(t, "OpenGL renderer string:")) return trim(t.substr(23));
    // eglinfo: "OpenGL core profile renderer: ..."
    size_t r = t.find(" renderer:");
    if (fallback.empty() && starts_with(t, "OpenGL") && r != std::string::npos)
      fallback = trim(t.substr(r + 10));
  }
  return fallback;
}

int renderer_gpu(const std::vector<Gpu> &gpus, const std::string &renderer) {
  std::string r = lower(renderer);
  if (r.empty() || r.find("llvmpipe") != std::string::npos ||
      r.find("softpipe") != std::string::npos || r.find("software") != std::string::npos)
    return -1;
  std::string vendor;
  if (r.find("nvidia") != std::string::npos || r.find("geforce") != std::string::npos ||
      r.find("quadro") != std::string::npos || r.find("nv1") != std::string::npos)
    vendor = "10de";
  else if (r.find("amd") != std::string::npos || r.find("radeon") != std::string::npos)
    vendor = "1002";
  else if (r.find("intel") != std::string::npos)
    vendor = "8086";
  else if (r.find("virgl") != std::string::npos || r.find("virtio") != std::string::npos)
    vendor = "1af4";
  else if (r.find("svga3d") != std::string::npos || r.find("vmware") != std::string::npos)
    vendor = "15ad";
  int found = -1;
  for (size_t i = 0; i < gpus.size(); ++i)
    if (gpus[i].vendor_id == vendor) {
      if (found >= 0) return -1;  // two cards of the same vendor: unknown
      found = int(i);
    }
  if (found < 0 && gpus.size() == 1 && vendor.empty()) return 0;
  return found;
}

}  // namespace sde
