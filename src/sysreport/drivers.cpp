// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "drivers.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <set>
#include <sstream>

#include "gpu.h"
#include "i18n.h"
#include "integration.h"
#include "process.h"
#include "util.h"

namespace sde {
namespace drivers {

namespace {

std::string lower_copy(std::string s) {
  for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void add_unique(std::vector<std::string> &list, const std::string &v) {
  if (!v.empty() && std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
}

// The first whitespace-delimited word of |s| starting at |pos|.
std::string word_at(const std::string &s, size_t pos) {
  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
  size_t end = pos;
  while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
  return s.substr(pos, end - pos);
}

std::string clean_file(std::string f) {
  // Trailing punctuation of log lines: "file.bin," "(file.bin)" "file.bin:".
  while (!f.empty() && (f.back() == ',' || f.back() == ':' || f.back() == ')' || f.back() == '.' ||
                        f.back() == '\'' || f.back() == '"'))
    f.pop_back();
  while (!f.empty() && (f.front() == '(' || f.front() == '\'' || f.front() == '"')) f.erase(0, 1);
  if (starts_with(f, "/lib/firmware/")) f = f.substr(14);
  if (starts_with(f, "/usr/lib/firmware/")) f = f.substr(18);
  return f;
}

struct FirmwareRule {
  const char *prefix;
  const char *package;
  const char *device;
};

// Longest prefix first where prefixes overlap (intel/ibt- before intel/).
const FirmwareRule kFirmwareRules[] = {
    {"iwlwifi-", "firmware-iwlwifi", N_("Intel Wi-Fi")},
    {"intel/ibt-", "firmware-iwlwifi", N_("Intel Bluetooth")},
    {"intel/sof", "firmware-sof-signed", N_("Intel audio (Sound Open Firmware)")},
    {"intel/avs", "firmware-sof-signed", N_("Intel audio")},
    {"i915/", "firmware-intel-graphics", N_("Intel graphics")},
    {"xe/", "firmware-intel-graphics", N_("Intel graphics")},
    {"intel/", "firmware-intel-misc", N_("Intel device")},
    {"rtl_nic/", "firmware-realtek", N_("Realtek Ethernet")},
    {"rtlwifi/", "firmware-realtek", N_("Realtek Wi-Fi")},
    {"rtw88/", "firmware-realtek", N_("Realtek Wi-Fi")},
    {"rtw89/", "firmware-realtek", N_("Realtek Wi-Fi")},
    {"rtl_bt/", "firmware-realtek", N_("Realtek Bluetooth")},
    {"rtl8", "firmware-realtek", N_("Realtek device")},
    {"ath3k", "firmware-atheros", N_("Qualcomm Atheros Bluetooth")},
    {"ar3k/", "firmware-atheros", N_("Qualcomm Atheros Bluetooth")},
    {"qca/", "firmware-atheros", N_("Qualcomm Atheros Bluetooth")},
    {"ath6k/", "firmware-atheros", N_("Qualcomm Atheros Wi-Fi")},
    {"ath9k_htc/", "firmware-atheros", N_("Qualcomm Atheros Wi-Fi")},
    {"htc_", "firmware-atheros", N_("Qualcomm Atheros Wi-Fi")},
    {"ath10k/", "firmware-atheros", N_("Qualcomm Atheros Wi-Fi")},
    {"ath11k/", "firmware-atheros", N_("Qualcomm Wi-Fi")},
    {"ath12k/", "firmware-atheros", N_("Qualcomm Wi-Fi")},
    {"brcm/", "firmware-brcm80211", N_("Broadcom Wi-Fi or Bluetooth")},
    {"cypress/", "firmware-brcm80211", N_("Cypress Wi-Fi")},
    {"amdgpu/", "firmware-amd-graphics", N_("AMD graphics")},
    {"radeon/", "firmware-amd-graphics", N_("AMD/ATI graphics")},
    {"amd-ucode/", "amd64-microcode", N_("AMD processor microcode")},
    {"nvidia/", "firmware-nvidia-graphics", N_("NVIDIA graphics")},
    {"mediatek/", "firmware-mediatek", N_("MediaTek Wi-Fi or Bluetooth")},
    {"mt7", "firmware-mediatek", N_("MediaTek Wi-Fi")},
    {"ti-connectivity/", "firmware-ti-connectivity", N_("Texas Instruments Wi-Fi")},
    {"libertas/", "firmware-libertas", N_("Marvell Wi-Fi")},
    {"mrvl/", "firmware-libertas", N_("Marvell Wi-Fi or Bluetooth")},
    {"qcom/", "firmware-qcom-soc", N_("Qualcomm device")},
    {"bnx2x/", "firmware-bnx2x", N_("Broadcom NetXtreme II Ethernet")},
    {"bnx2/", "firmware-bnx2", N_("Broadcom NetXtreme II Ethernet")},
    {"regulatory.db", "wireless-regdb", N_("Wi-Fi regulatory database")},
};

const FirmwareRule *rule_for(const std::string &file) {
  std::string f = lower_copy(clean_file(file));
  for (const auto &r : kFirmwareRules)
    if (starts_with(f, r.prefix)) return &r;
  return nullptr;
}

std::string component_of(const std::string &package) {
  if (package == "wireless-regdb") return "main";
  if (starts_with(package, "broadcom-sta")) return "non-free";
  if (starts_with(package, "firmware-") || ends_with(package, "-microcode")) return "non-free-firmware";
  if (package.find("nvidia") != std::string::npos) return "non-free";
  return "main";
}

bool is_debian_uri(const std::string &uri) {
  std::string u = lower_copy(uri);
  if (u.find("debian.org") != std::string::npos) return true;
  while (!u.empty() && u.back() == '/') u.pop_back();
  return ends_with(u, "/debian") || ends_with(u, "/debian-security");
}

// One-line style entry: "deb [opts] uri suite comp..." -> parts.
struct OneLine {
  bool valid = false;
  bool deb = false;
  std::string uri;
  std::vector<std::string> components;
};

OneLine parse_one_line(const std::string &raw) {
  OneLine out;
  std::string line = raw.substr(0, raw.find('#'));
  std::string t = trim(line);
  if (!starts_with(t, "deb ") && !starts_with(t, "deb-src ") && !starts_with(t, "deb\t") &&
      !starts_with(t, "deb-src\t"))
    return out;
  out.deb = starts_with(t, "deb ") || starts_with(t, "deb\t");
  // Drop the [options] block, which may contain spaces.
  size_t open = t.find('[');
  if (open != std::string::npos) {
    size_t close = t.find(']', open);
    if (close == std::string::npos) return out;
    t = t.substr(0, open) + t.substr(close + 1);
  }
  std::istringstream in(t);
  std::string type, uri, suite, comp;
  in >> type >> uri >> suite;
  if (uri.empty() || suite.empty()) return out;
  while (in >> comp) out.components.push_back(comp);
  out.uri = uri;
  out.valid = true;
  return out;
}

struct Stanza {
  size_t first = 0, last = 0;  // line indexes
  bool debian = false, deb = false, enabled = true;
  int components_line = -1;
  std::vector<std::string> components;
};

std::vector<Stanza> parse_deb822(const std::vector<std::string> &lines) {
  std::vector<Stanza> out;
  Stanza cur;
  bool open = false;
  auto close = [&](size_t last) {
    if (open) {
      cur.last = last;
      out.push_back(cur);
    }
    cur = Stanza();
    open = false;
  };
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string t = trim(lines[i]);
    if (t.empty()) {
      close(i);
      continue;
    }
    if (t[0] == '#') continue;
    if (!open) {
      open = true;
      cur.first = i;
    }
    size_t colon = t.find(':');
    if (colon == std::string::npos) continue;
    std::string key = lower_copy(trim(t.substr(0, colon)));
    std::string value = trim(t.substr(colon + 1));
    if (key == "uris") {
      for (auto &u : split(value, ' '))
        if (is_debian_uri(u)) cur.debian = true;
    } else if (key == "types") {
      for (auto &ty : split(value, ' '))
        if (ty == "deb") cur.deb = true;
    } else if (key == "enabled") {
      cur.enabled = lower_copy(value) != "no";
    } else if (key == "components") {
      cur.components_line = int(i);
      cur.components = split(value, ' ');
    }
  }
  close(lines.size());
  return out;
}

std::vector<std::string> lines_of(const std::string &text) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

std::string join_lines(const std::vector<std::string> &lines, bool trailing_newline) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size() || trailing_newline) out += '\n';
  }
  return out;
}

bool has(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

std::vector<std::string> parse_firmware_failures(const std::string &log) {
  std::vector<std::string> out;
  std::istringstream in(log);
  std::string line;
  while (std::getline(in, line)) {
    std::string l = lower_copy(line);
    size_t p;
    if ((p = l.find("firmware: failed to load ")) != std::string::npos) {
      add_unique(out, clean_file(word_at(line, p + 25)));
    } else if ((p = l.find("direct firmware load for ")) != std::string::npos) {
      add_unique(out, clean_file(word_at(line, p + 25)));
    } else if ((p = l.find("failed to load firmware ")) != std::string::npos) {
      add_unique(out, clean_file(word_at(line, p + 24)));
    } else if ((l.find("failed to load") != std::string::npos ||
                l.find("unable to load") != std::string::npos) &&
               (l.find("firmware") != std::string::npos || l.find("regulatory.db") != std::string::npos)) {
      // "[drm] Failed to load DMC firmware i915/adlp_dmc.bin.", "cfg80211:
      // failed to load regulatory.db": the first word that looks like a file.
      std::istringstream words(line.substr(l.find("load")));
      std::string w;
      while (words >> w) {
        std::string f = clean_file(w);
        if (f.find(':') != std::string::npos || f.find('[') != std::string::npos) continue;
        if (f.find('/') != std::string::npos || ends_with(f, ".bin") || ends_with(f, ".fw") ||
            ends_with(f, ".ucode") || ends_with(f, ".db") || ends_with(f, ".sfi")) {
          add_unique(out, f);
          break;
        }
      }
    }
  }
  // Lines such as "failed to load firmware" without a file name.
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const std::string &f) {
                             return f.empty() || f[0] == '/' || f[0] == '-' ||
                                    (f.find('.') == std::string::npos && f.find('/') == std::string::npos);
                           }),
            out.end());
  return out;
}

std::string firmware_package(const std::string &file) {
  const FirmwareRule *r = rule_for(file);
  return r ? r->package : "firmware-misc-nonfree";
}

std::string firmware_device(const std::string &file) {
  const FirmwareRule *r = rule_for(file);
  return r ? _(r->device) : _("Device firmware");
}

std::string parse_nvidia_detect(const std::string &output) {
  size_t p = output.find("recommended to install the");
  if (p == std::string::npos) return "";
  std::string pkg = word_at(output, p + 26);
  if (pkg.empty() || pkg == "package.") return "";
  return pkg;
}

std::vector<Recommendation> gpu_recommendations(const std::vector<Gpu> &gpus,
                                                const std::string &nvidia_detect) {
  std::vector<Recommendation> out;
  for (const Gpu &g : gpus) {
    std::string label = gpu_label(g);
    std::string vid = lower_copy(g.vendor_id);
    if (starts_with(vid, "0x")) vid = vid.substr(2);
    if (vid == "10de") {
      Recommendation r;
      std::string pkg = parse_nvidia_detect(nvidia_detect);
      r.package = pkg.empty() ? "nvidia-driver" : pkg;
      r.device = label;
      r.reason = pkg.empty()
                     ? _("Proprietary NVIDIA driver: full 3D speed, CUDA and power management. The "
                         "free nouveau driver works without it. Install nvidia-detect to check "
                         "which driver series supports this card.")
                     : _("Proprietary NVIDIA driver recommended by nvidia-detect for this card. The "
                         "free nouveau driver works without it.");
      r.component = "non-free";
      out.push_back(r);
      if (g.driver != "nvidia") {
        Recommendation fw;
        fw.package = "firmware-nvidia-graphics";
        fw.device = label;
        fw.reason = _("Firmware for the free nouveau driver (needed by recent GeForce cards).");
        fw.component = "non-free-firmware";
        fw.recommended = false;  // the alternative to nvidia-driver
        out.push_back(fw);
      }
    } else if (vid == "1002") {
      Recommendation r;
      r.package = "firmware-amd-graphics";
      r.device = label;
      r.reason = _("Firmware for the amdgpu and radeon drivers: 3D acceleration, video decoding "
                   "and external displays.");
      r.component = "non-free-firmware";
      out.push_back(r);
    } else if (vid == "8086") {
      Recommendation r;
      r.package = "firmware-intel-graphics";
      r.device = label;
      r.reason = _("GuC, HuC and DMC firmware for Intel graphics: power saving and hardware "
                   "video decoding.");
      r.component = "non-free-firmware";
      out.push_back(r);
    }
  }
  return out;
}

std::string microcode_package(const std::string &cpuinfo, bool virtual_machine) {
  if (virtual_machine) return "";
  if (cpuinfo.find("GenuineIntel") != std::string::npos) return "intel-microcode";
  if (cpuinfo.find("AuthenticAMD") != std::string::npos) return "amd64-microcode";
  return "";
}

std::vector<std::string> parse_isenkram(const std::string &output) {
  std::vector<std::string> out;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    bool ok = true;
    for (char c : line)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.')) ok = false;
    if (ok) add_unique(out, line);
  }
  return out;
}

std::vector<std::string> parse_dpkg_installed(const std::string &output) {
  std::vector<std::string> out;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("install ok installed") == std::string::npos) continue;
    std::string pkg = word_at(line, 0);
    size_t colon = pkg.find(':');
    if (colon != std::string::npos) pkg = pkg.substr(0, colon);
    add_unique(out, pkg);
  }
  return out;
}

bool sources_have_component(const std::string &text, bool deb822, const std::string &component) {
  auto lines = lines_of(text);
  if (deb822) {
    for (const Stanza &s : parse_deb822(lines))
      if (s.debian && s.deb && s.enabled && has(s.components, component)) return true;
    return false;
  }
  for (const auto &l : lines) {
    OneLine e = parse_one_line(l);
    if (e.valid && e.deb && is_debian_uri(e.uri) && has(e.components, component)) return true;
  }
  return false;
}

std::string enable_components(const std::string &text, bool deb822,
                              const std::vector<std::string> &components) {
  auto lines = lines_of(text);
  bool changed = false;
  if (deb822) {
    for (const Stanza &s : parse_deb822(lines)) {
      if (!s.debian || s.components_line < 0) continue;
      std::string &l = lines[size_t(s.components_line)];
      for (const auto &c : components)
        if (!has(s.components, c)) {
          // Keep any trailing spaces out of the value.
          while (!l.empty() && std::isspace(static_cast<unsigned char>(l.back()))) l.pop_back();
          l += " " + c;
          changed = true;
        }
    }
  } else {
    for (auto &l : lines) {
      OneLine e = parse_one_line(l);
      if (!e.valid || !is_debian_uri(e.uri)) continue;
      size_t hash = l.find('#');
      std::string body = hash == std::string::npos ? l : l.substr(0, hash);
      std::string rest = hash == std::string::npos ? "" : l.substr(hash);
      while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.pop_back();
      bool line_changed = false;
      for (const auto &c : components)
        if (!has(e.components, c)) {
          body += " " + c;
          line_changed = true;
        }
      if (line_changed) {
        l = body + (rest.empty() ? "" : " " + rest);
        changed = true;
      }
    }
  }
  if (!changed) return text;
  return join_lines(lines, !text.empty() && text.back() == '\n');
}

std::vector<Recommendation> merge(const std::vector<Recommendation> &list) {
  std::vector<Recommendation> out;
  for (const auto &r : list) {
    auto it = std::find_if(out.begin(), out.end(),
                           [&](const Recommendation &o) { return o.package == r.package; });
    if (it == out.end()) {
      out.push_back(r);
    } else {
      if (!r.device.empty() && it->device.find(r.device) == std::string::npos)
        it->device += ", " + r.device;
      if (!r.reason.empty() && it->reason.find(r.reason) == std::string::npos)
        it->reason += " " + r.reason;
      it->installed = it->installed || r.installed;
    }
  }
  return out;
}

// ---- devices and driver choices

namespace {

std::string norm_hex(std::string s) {
  s = lower_copy(trim(s));
  if (starts_with(s, "0x")) s = s.substr(2);
  return s;
}

std::string read_value(const std::string &path) {
  std::string s;
  read_file(path, s);
  return trim(s);
}

// PCI vendor IDs named in a recommendation's device text or package.
std::vector<std::string> vendor_ids(const Recommendation &r) {
  std::string t = lower_copy(r.device + " " + r.package);
  std::vector<std::string> out;
  auto add = [&](const char *word, std::initializer_list<const char *> ids) {
    if (t.find(word) != std::string::npos)
      for (const char *id : ids) add_unique(out, id);
  };
  add("intel", {"8086"});
  add("iwlwifi", {"8086"});
  add("realtek", {"10ec"});
  add("rtw8", {"10ec"});
  add("atheros", {"168c", "17cb"});
  add("qualcomm", {"168c", "17cb"});
  add("broadcom", {"14e4"});
  add("cypress", {"14e4"});
  add("mediatek", {"14c3"});
  add("marvell", {"11ab", "1b4b"});
  add("texas", {"104c"});
  add("amd", {"1002", "1022"});
  add("nvidia", {"10de"});
  return out;
}

std::string first_sentence(const std::string &s) {
  size_t p = s.find(". ");
  return p == std::string::npos ? s : s.substr(0, p + 1);
}

}  // namespace

DeviceKind pci_kind(const std::string &class_id) {
  std::string c = norm_hex(class_id);
  if (starts_with(c, "0280")) return DeviceKind::Wifi;
  if (starts_with(c, "02")) return DeviceKind::Network;
  if (starts_with(c, "0d11")) return DeviceKind::Bluetooth;
  if (starts_with(c, "0403") || starts_with(c, "0401")) return DeviceKind::Audio;
  if (starts_with(c, "03")) return DeviceKind::Graphics;
  return DeviceKind::Other;
}

std::string dkms_device(const std::string &package) {
  if (!ends_with(package, "-dkms") || package.find("nvidia") != std::string::npos) return "";
  static const struct {
    const char *prefix, *device;
  } table[] = {
      {"rtw89", N_("Realtek Wi-Fi")},   {"rtw88", N_("Realtek Wi-Fi")},
      {"rtl8", N_("Realtek Wi-Fi")},    {"8821", N_("Realtek Wi-Fi")},
      {"88x2", N_("Realtek Wi-Fi")},    {"r8168", N_("Realtek Ethernet")},
      {"r8125", N_("Realtek Ethernet")}, {"broadcom-sta", N_("Broadcom Wi-Fi")},
      {"bcmwl", N_("Broadcom Wi-Fi")},  {"mt76", N_("MediaTek Wi-Fi")},
      {"iwlwifi", N_("Intel Wi-Fi")},   {"backport-iwlwifi", N_("Intel Wi-Fi")},
  };
  for (const auto &e : table)
    if (starts_with(package, e.prefix)) return _(e.device);
  return "";
}

DeviceKind device_kind(const Recommendation &r) {
  const std::string &p = r.package;
  std::string d = lower_copy(r.device + " " + dkms_device(p));
  if (p.find("nvidia") != std::string::npos || p.find("graphics") != std::string::npos ||
      d.find("graphics") != std::string::npos)
    return DeviceKind::Graphics;
  if (ends_with(p, "-microcode")) return DeviceKind::Processor;
  if (d.find("wi-fi") != std::string::npos || d.find("wireless") != std::string::npos)
    return DeviceKind::Wifi;
  if (d.find("bluetooth") != std::string::npos) return DeviceKind::Bluetooth;
  if (d.find("ethernet") != std::string::npos) return DeviceKind::Network;
  if (d.find("audio") != std::string::npos) return DeviceKind::Audio;
  return DeviceKind::Other;
}

bool is_firmware_package(const std::string &package) {
  return starts_with(package, "firmware-") || ends_with(package, "-microcode") ||
         package == "wireless-regdb";
}

bool is_free_driver(const std::string &package, const std::string &component) {
  if (package.find("nvidia") != std::string::npos) return false;
  return component.empty() || component == "main";
}

std::vector<PciDevice> detect_pci_devices(const std::string &sysfs_in) {
  std::string sysfs = sysfs_in.empty() ? env("SDE_PCI_SYSFS", "/sys/bus/pci/devices") : sysfs_in;
  std::vector<PciDevice> out;
  std::vector<std::string> slots = list_dir(sysfs);
  std::sort(slots.begin(), slots.end());
  for (auto &slot : slots) {
    std::string dir = sysfs + "/" + slot;
    PciDevice d;
    d.class_id = norm_hex(read_value(dir + "/class"));
    DeviceKind k = pci_kind(d.class_id);
    if (k == DeviceKind::Other || k == DeviceKind::Graphics) continue;
    d.slot = slot;
    d.vendor_id = norm_hex(read_value(dir + "/vendor"));
    d.device_id = norm_hex(read_value(dir + "/device"));
    char buf[PATH_MAX];
    ssize_t n = readlink((dir + "/driver").c_str(), buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = 0;
      d.driver = basename_of(buf);
    }
    PciName name = lookup_pci_ids(d.vendor_id, d.device_id);
    d.vendor = name.vendor.empty() ? known_vendor(d.vendor_id) : name.vendor;
    d.model = name.device;
    out.push_back(d);
  }
  return out;
}

std::vector<Device> group_devices(const std::vector<Recommendation> &recs,
                                  const std::vector<PciDevice> &pci,
                                  const std::map<std::string, AptVersions> &versions,
                                  const std::map<std::string, std::string> &summaries) {
  std::vector<Device> out;
  for (const auto &r : recs) {
    DeviceKind kind = device_kind(r);
    std::string name = r.device;
    if (kind != DeviceKind::Graphics && kind != DeviceKind::Processor) {
      auto ids = vendor_ids(r);
      for (const auto &p : pci) {
        if (pci_kind(p.class_id) != kind || !has(ids, p.vendor_id)) continue;
        name = p.model.empty() ? p.vendor : (p.vendor.empty() ? p.model : p.vendor + ": " + p.model);
        break;
      }
    }
    DriverOption o;
    o.package = r.package;
    o.installed = r.installed;
    o.recommended = r.recommended;
    o.free_software = is_free_driver(r.package, r.component);
    auto v = versions.find(r.package);
    if (v != versions.end()) o.version = v->second.installed.empty() ? v->second.candidate : v->second.installed;
    auto s = summaries.find(r.package);
    o.summary = s != summaries.end() && !s->second.empty() ? s->second : first_sentence(r.reason);
    auto it = std::find_if(out.begin(), out.end(), [&](const Device &d) { return d.name == name; });
    if (it == out.end()) {
      Device d;
      d.kind = kind;
      d.name = name;
      out.push_back(d);
      it = out.end() - 1;
    }
    it->options.push_back(o);
  }
  for (auto &d : out)
    std::stable_sort(d.options.begin(), d.options.end(),
                     [](const DriverOption &a, const DriverOption &b) { return a.recommended && !b.recommended; });
  std::stable_sort(out.begin(), out.end(), [](const Device &a, const Device &b) { return a.kind < b.kind; });
  return out;
}

std::vector<int> initial_selection(const std::vector<Device> &devices) {
  std::vector<int> out;
  for (const auto &d : devices) {
    int pick = kNoDriver;
    for (size_t j = 0; j < d.options.size() && pick == kNoDriver; ++j)
      if (d.options[j].installed && !is_firmware_package(d.options[j].package)) pick = int(j);
    for (size_t j = 0; j < d.options.size() && pick == kNoDriver; ++j)
      if (d.options[j].installed) pick = int(j);
    out.push_back(pick);
  }
  return out;
}

DeviceState device_state(const Device &d, int selected) {
  if (selected < 0 || selected >= int(d.options.size())) return DeviceState::NoDriver;
  return d.options[size_t(selected)].recommended ? DeviceState::Recommended : DeviceState::Alternative;
}

Changes plan_changes(const std::vector<Device> &devices, const std::vector<int> &initial,
                     const std::vector<int> &selection) {
  Changes c;
  for (size_t i = 0; i < devices.size() && i < initial.size() && i < selection.size(); ++i) {
    int sel = selection[i];
    if (sel == initial[i]) continue;
    const Device &d = devices[i];
    if (sel >= int(d.options.size())) continue;
    if (sel >= 0 && !d.options[size_t(sel)].installed) add_unique(c.install, d.options[size_t(sel)].package);
    for (size_t j = 0; j < d.options.size(); ++j) {
      const DriverOption &o = d.options[j];
      if (int(j) == sel || !o.installed) continue;
      if (sel == kNoDriver || !is_firmware_package(o.package)) add_unique(c.remove, o.package);
    }
  }
  c.remove.erase(std::remove_if(c.remove.begin(), c.remove.end(),
                                [&](const std::string &p) { return has(c.install, p); }),
                 c.remove.end());
  return c;
}

std::vector<std::string> apt_arguments(const Changes &changes) {
  if (changes.empty()) return {};
  std::vector<std::string> out = {"install", "-y"};
  for (const auto &p : changes.install) out.push_back(p);
  for (const auto &p : changes.remove) out.push_back(p + "-");
  return out;
}

int proprietary_in_use(const std::vector<Device> &devices) {
  std::set<std::string> pkgs;
  for (const auto &d : devices)
    for (const auto &o : d.options)
      if (o.installed && !o.free_software) pkgs.insert(o.package);
  return int(pkgs.size());
}

std::map<std::string, AptVersions> parse_apt_policy(const std::string &output) {
  std::map<std::string, AptVersions> out;
  std::istringstream in(output);
  std::string line, pkg;
  auto value = [](const std::string &t, size_t at) {
    std::string v = trim(t.substr(at));
    return v == "(none)" ? std::string() : v;
  };
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (!std::isspace(static_cast<unsigned char>(line[0])) && line.back() == ':') {
      pkg = line.substr(0, line.size() - 1);
      size_t colon = pkg.find(':');  // "pkg:amd64"
      if (colon != std::string::npos) pkg = pkg.substr(0, colon);
      out[pkg];
      continue;
    }
    if (pkg.empty()) continue;
    std::string t = trim(line);
    if (starts_with(t, "Installed:")) out[pkg].installed = value(t, 10);
    else if (starts_with(t, "Candidate:")) out[pkg].candidate = value(t, 10);
  }
  return out;
}

std::map<std::string, std::string> parse_apt_summaries(const std::string &output) {
  std::map<std::string, std::string> out;
  std::istringstream in(output);
  std::string line, pkg;
  while (std::getline(in, line)) {
    if (trim(line).empty()) {
      pkg.clear();
      continue;
    }
    if (starts_with(line, "Package:")) {
      pkg = trim(line.substr(8));
    } else if (!pkg.empty() && (starts_with(line, "Description:") || starts_with(line, "Description-en:"))) {
      std::string v = trim(line.substr(line.find(':') + 1));
      if (!v.empty() && !out.count(pkg)) out[pkg] = v;
    }
  }
  return out;
}

bool parse_apt_status(const std::string &line, double *percent, std::string *text) {
  bool dl = starts_with(line, "dlstatus:"), pm = starts_with(line, "pmstatus:");
  if (!dl && !pm) return false;
  size_t a = line.find(':');
  size_t b = line.find(':', a + 1);
  size_t c = b == std::string::npos ? b : line.find(':', b + 1);
  if (c == std::string::npos) return false;
  char *end = nullptr;
  std::string num = line.substr(b + 1, c - b - 1);
  double v = std::strtod(num.c_str(), &end);
  if (end == num.c_str()) return false;
  v = std::max(0.0, std::min(100.0, v));
  if (percent) *percent = dl ? v * 0.3 : 30.0 + v * 0.7;
  if (text) *text = trim(line.substr(c + 1));
  return true;
}

std::string cpu_model_name(const std::string &cpuinfo) {
  std::istringstream in(cpuinfo);
  std::string line;
  while (std::getline(in, line)) {
    if (!starts_with(line, "model name")) continue;
    size_t colon = line.find(':');
    if (colon != std::string::npos) return trim(line.substr(colon + 1));
  }
  return "";
}

std::string main_sources_file(bool *deb822) {
  std::string over = env("SDE_APT_SOURCES");
  std::string path;
  if (!over.empty()) path = over;
  else if (file_exists("/etc/apt/sources.list.d/debian.sources")) path = "/etc/apt/sources.list.d/debian.sources";
  else path = "/etc/apt/sources.list";
  if (deb822) *deb822 = ends_with(path, ".sources");
  return path;
}

Scan scan_system() {
  Scan scan;
  std::vector<Recommendation> recs;

  std::vector<Gpu> gpus = detect_gpus();
  for (const auto &g : gpus) scan.gpus.push_back(gpu_label(g));
  std::string nvidia;
  bool has_nvidia = std::any_of(gpus.begin(), gpus.end(), [](const Gpu &g) {
    return lower_copy(g.vendor_id) == "10de";
  });
  if (has_nvidia && !find_program("nvidia-detect").empty()) nvidia = run_sync({"nvidia-detect"}, nullptr, 8000);
  for (auto &r : gpu_recommendations(gpus, nvidia)) recs.push_back(r);

  // Kernel log: a fixture, the journal (adm/systemd-journal members) or dmesg.
  std::string log;
  std::string fixture = env("SDE_KERNEL_LOG");
  if (!fixture.empty()) {
    read_file(fixture, log);
  } else {
    int status = -1;
    if (!find_program("journalctl").empty())
      log = run_sync({"journalctl", "-k", "-b", "--no-pager", "-q", "-o", "cat"}, &status, 6000);
    if (status != 0 || trim(log).empty()) {
      int st2 = -1;
      std::string d = find_program("dmesg").empty() ? "" : run_sync({"dmesg"}, &st2, 4000);
      if (st2 == 0 && !trim(d).empty()) log = d;
      else if (status != 0 || trim(log).empty()) scan.kernel_log_readable = false;
    }
  }
  for (const auto &f : parse_firmware_failures(log)) {
    Recommendation r;
    r.package = firmware_package(f);
    r.device = firmware_device(f);
    r.reason = format(_("The kernel could not load %s."), f.c_str());
    r.component = component_of(r.package);
    recs.push_back(r);
  }

  std::string cpuinfo;
  read_file(env("SDE_CPUINFO", "/proc/cpuinfo"), cpuinfo);
  std::string mc = microcode_package(cpuinfo, !virtual_machine_name().empty());
  if (!mc.empty()) {
    Recommendation r;
    r.package = mc;
    r.device = cpu_model_name(cpuinfo);
    if (r.device.empty()) r.device = _("Processor");
    r.reason = _("Processor microcode updates fix CPU bugs and security issues. They are loaded "
                 "at every start.");
    r.component = "non-free-firmware";
    recs.push_back(r);
  }

  if (!find_program("isenkram-lookup").empty()) {
    scan.isenkram = true;
    for (const auto &p : parse_isenkram(run_sync({"isenkram-lookup"}, nullptr, 15000))) {
      Recommendation r;
      r.package = p;
      r.device = _("Detected hardware");
      r.reason = _("Suggested by isenkram for a device of this computer.");
      r.component = component_of(p);
      recs.push_back(r);
    }
  }

  scan.items = merge(recs);
  if (!scan.items.empty() && !find_program("dpkg-query").empty()) {
    std::vector<std::string> argv = {"dpkg-query", "-W", "-f=${Package} ${Status}\n"};
    for (auto &r : scan.items) argv.push_back(r.package);
    auto installed = parse_dpkg_installed(run_sync(argv, nullptr, 5000));
    for (auto &r : scan.items) r.installed = has(installed, r.package);
  }
  // DKMS drivers installed by hand (rtw89-dkms, r8168-dkms ...) show up as
  // the alternative driver of their device.
  if (!find_program("dpkg-query").empty()) {
    auto dkms = parse_dpkg_installed(
        run_sync({"dpkg-query", "-W", "-f=${Package} ${Status}\n", "*-dkms"}, nullptr, 5000));
    for (const auto &p : dkms) {
      std::string dev = dkms_device(p);
      bool known = std::any_of(scan.items.begin(), scan.items.end(),
                               [&](const Recommendation &r) { return r.package == p; });
      if (dev.empty() || known) continue;
      Recommendation r;
      r.package = p;
      r.device = dev;
      r.reason = _("A driver module that DKMS builds for every installed kernel.");
      r.component = component_of(p);
      r.installed = true;
      r.recommended = false;
      scan.items.push_back(r);
    }
  }
  std::map<std::string, AptVersions> versions;
  std::map<std::string, std::string> summaries;
  if (!scan.items.empty() && !find_program("apt-cache").empty()) {
    std::vector<std::string> policy = {"apt-cache", "policy"}, show = {"apt-cache", "show", "--no-all-versions"};
    for (auto &r : scan.items) {
      policy.push_back(r.package);
      show.push_back(r.package);
    }
    versions = parse_apt_policy(run_sync(policy, nullptr, 8000));
    summaries = parse_apt_summaries(run_sync(show, nullptr, 8000));
  }
  scan.devices = group_devices(scan.items, detect_pci_devices(), versions, summaries);

  scan.sources_file = main_sources_file(&scan.sources_deb822);
  std::string sources;
  read_file(scan.sources_file, sources);
  scan.non_free_firmware = sources_have_component(sources, scan.sources_deb822, "non-free-firmware");
  scan.contrib_non_free = sources_have_component(sources, scan.sources_deb822, "contrib") &&
                          sources_have_component(sources, scan.sources_deb822, "non-free");
  return scan;
}

}  // namespace drivers
}  // namespace sde
