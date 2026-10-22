// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "drivers.h"

#include <algorithm>
#include <cctype>
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
    r.device = _("Processor");
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
