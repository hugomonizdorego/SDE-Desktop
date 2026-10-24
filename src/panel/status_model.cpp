// SPDX-License-Identifier: GPL-3.0-or-later
#include "status_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "i18n.h"
#include "util.h"

using namespace sde;

// ------------------------------------------------------------------- text

std::string printable_text(const std::string &text) {
  std::string out;
  const auto *s = reinterpret_cast<const unsigned char *>(text.data());
  size_t n = text.size();
  for (size_t i = 0; i < n;) {
    unsigned char c = s[i];
    if (c < 0x80) {
      if (c >= 0x20 && c != 0x7F)
        out += char(c);
      else if (c == '\t' || c == '\n' || c == '\r')
        out += ' ';
      ++i;
      continue;
    }
    // A valid UTF-8 sequence is kept; anything else becomes U+FFFD.
    int len = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
    bool ok = len > 0 && i + size_t(len) <= n;
    for (int k = 1; ok && k < len; ++k) ok = (s[i + size_t(k)] & 0xC0) == 0x80;
    if (ok && len == 3 && c == 0xE0) ok = s[i + 1] >= 0xA0;  // overlong
    if (ok && len == 3 && c == 0xED) ok = s[i + 1] < 0xA0;   // surrogates
    if (ok && len == 4 && c == 0xF0) ok = s[i + 1] >= 0x90;
    if (ok && len == 4 && c == 0xF4) ok = s[i + 1] < 0x90;
    // C1 control characters (U+0080..U+009F) are dropped too.
    if (ok && len == 2 && c == 0xC2 && s[i + 1] < 0xA0) {
      i += 2;
      continue;
    }
    if (ok) {
      out.append(text, i, size_t(len));
      i += size_t(len);
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  // Collapse runs of spaces left by dropped line breaks.
  std::string collapsed;
  for (char ch : out)
    if (!(ch == ' ' && !collapsed.empty() && collapsed.back() == ' ')) collapsed += ch;
  return trim(collapsed);
}

std::string tray_item_name(const std::string &title, const std::string &tooltip,
                           const std::string &id, const std::string &service) {
  for (const std::string *s : {&title, &tooltip, &id}) {
    std::string t = printable_text(*s);
    if (!t.empty()) return t;
  }
  // "org.kde.StatusNotifierItem-1234-1" says nothing; ":1.42" even less.
  std::string svc = printable_text(service);
  if (starts_with(svc, "org.kde.StatusNotifierItem-") || starts_with(svc, ":") || svc.empty())
    return "Application";
  size_t dot = svc.rfind('.');
  return dot == std::string::npos ? svc : svc.substr(dot + 1);
}

// ------------------------------------------------------------ network LED

NetLed network_led(bool connected, int connectivity, bool probe_failed,
                   const std::string &verdict) {
  if (!connected) return NetLed::RED;
  if (!verdict.empty() && verdict != "ok" && verdict != "unknown") return NetLed::YELLOW;
  if (verdict == "ok" && connectivity == 0) return NetLed::GREEN;
  switch (connectivity) {
    case 4:
      return NetLed::GREEN;
    case 1:  // NONE while a connection is active: local network only
    case 2:  // PORTAL
    case 3:  // LIMITED
      return NetLed::YELLOW;
    default:  // UNKNOWN: NetworkManager does not check
      return probe_failed ? NetLed::YELLOW : NetLed::GREEN;
  }
}

std::string parse_network_verdict(const std::string &json) {
  size_t k = json.find("\"verdict\"");
  if (k == std::string::npos) return "";
  size_t q1 = json.find('"', json.find(':', k) + 1);
  if (q1 == std::string::npos) return "";
  size_t q2 = json.find('"', q1 + 1);
  if (q2 == std::string::npos) return "";
  return json.substr(q1 + 1, q2 - q1 - 1);
}

std::string network_verdict_text(const std::string &v) {
  // Plain English here; the flyout translates the sentence.
  if (v == "proxy-required") return "Proxy required: open Settings > Network";
  if (v == "no-dns") return "Name servers (DNS) do not answer";
  if (v == "no-route") return "No route to the internet (gateway)";
  if (v == "clock") return "The clock is wrong: secure sites fail";
  if (v == "tls-intercepted") return "Secure connections are intercepted";
  if (v == "offline") return "The network does not reach the internet";
  if (v == "checking") return "Checking internet access...";
  return "";
}

bool parse_json_bool(const std::string &json, const std::string &key, bool fallback) {
  size_t k = json.find("\"" + key + "\"");
  if (k == std::string::npos) return fallback;
  size_t c = json.find(':', k);
  if (c == std::string::npos) return fallback;
  size_t v = json.find_first_not_of(" \t", c + 1);
  if (v == std::string::npos) return fallback;
  if (json.compare(v, 4, "true") == 0) return true;
  if (json.compare(v, 5, "false") == 0) return false;
  return fallback;
}

bool network_verdict_fixable(const std::string &verdict, bool repairable) {
  if (verdict == "no-dns") return true;
  // Something the repair changes (an unmanaged device, ifupdown DHCP, a
  // resolv.conf nobody writes) explains a missing route or an offline network.
  return repairable && (verdict == "no-route" || verdict == "offline");
}

std::string network_repair_summary(const std::string &output, int status) {
  std::string fail, last;
  size_t start = 0;
  while (start < output.size()) {
    size_t end = output.find('\n', start);
    if (end == std::string::npos) end = output.size();
    std::string line = output.substr(start, end - start);
    start = end + 1;
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) continue;
    line = line.substr(b);
    if (line.rfind("DNS works", 0) == 0) return "ok";
    if (fail.empty() && (line.rfind("DNS still does not work", 0) == 0 ||
                         line.rfind("DNS does not work yet", 0) == 0 ||
                         line.rfind("DNS is not repaired", 0) == 0))
      fail = line;
    last = line;
  }
  if (!fail.empty()) return fail;
  if (status == 0) return "done";
  return last;
}

// ---------------------------------------------------- keyboard indicators

KeyboardApplet keyboard_applet(const std::string &raw) {
  std::string n = lower(trim(raw));
  if (n.empty()) return KeyboardApplet::NONE;
  for (const char *im : {"ibus", "fcitx", "fcitx5", "org.fcitx", "uim", "scim", "kimpanel",
                         "hime", "gcin", "nimf", "kime"})
    if (n == im || starts_with(n, std::string(im) + "-") || starts_with(n, std::string(im) + ".") ||
        starts_with(n, std::string(im) + " ") || starts_with(n, std::string(im) + "_"))
      return KeyboardApplet::INPUT_METHOD;
  if (n.find("ibus") != std::string::npos || n.find("fcitx") != std::string::npos)
    return KeyboardApplet::INPUT_METHOD;
  for (const char *xkb : {"gxkb", "xxkb", "fbxkb", "sbxkb", "kbdd", "xkb-switch", "xkbswitch",
                          "keyboard layout", "keyboard-layout", "keyboardlayout",
                          "xfce4-xkb-plugin", "lxpanel-xkb", "kbswitch", "xkblayout"})
    if (n == xkb || starts_with(n, std::string(xkb) + "-")) return KeyboardApplet::LAYOUT;
  return KeyboardApplet::NONE;
}

// ----------------------------------------------------------------- drives

std::string RemovableDrive::mountpoint() const {
  for (const auto &p : parts)
    if (!p.mountpoint.empty()) return p.mountpoint;
  return "";
}

std::string human_size(long long bytes) {
  double n = double(bytes);
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int u = 0;
  while (n >= 1000 && u < 4) {
    n /= 1000;
    ++u;
  }
  char buf[32];
  snprintf(buf, sizeof buf, (u < 2 || n >= 100) ? "%.0f %s" : "%.1f %s", n, units[u]);
  return buf;
}

namespace {
// Kind and icon from what is known about the drive.
void classify(RemovableDrive &r, bool card, bool rotating) {
  std::string lm = lower(r.model);
  if (card || lm.find("card") != std::string::npos || lm.find("sd/mmc") != std::string::npos) {
    r.kind = "card";
    r.icon = "media-flash";
  } else if (rotating) {
    r.kind = "hdd";
    r.icon = "drive-harddisk-usb";
  } else if (lm.find("ssd") != std::string::npos || lm.find("nvme") != std::string::npos ||
             r.size > 300LL * 1000 * 1000 * 1000) {
    r.kind = "ssd";
    r.icon = "drive-harddisk-solidstate";
  } else {
    r.kind = "usb";
    r.icon = "drive-removable-media-usb";
  }
}
bool system_mount(const std::string &mp) {
  for (const char *m : {"/", "/boot", "/boot/efi", "/home", "/usr", "/var", "[SWAP]"})
    if (mp == m) return true;
  return false;
}
std::string base_name(const std::string &dev) {
  size_t slash = dev.rfind('/');
  return slash == std::string::npos ? dev : dev.substr(slash + 1);
}
}  // namespace

std::vector<RemovableDrive> udisks_removable_drives(const std::vector<UdisksBlock> &blocks,
                                                    const std::vector<UdisksDrive> &drives) {
  std::vector<RemovableDrive> out;
  for (const auto &d : drives) {
    std::string bus = lower(d.connection_bus);
    bool card = bus == "sdio" || starts_with(lower(d.media), "flash_sd") ||
                starts_with(lower(d.media), "flash_mmc") || starts_with(lower(d.media), "flash_ms") ||
                starts_with(lower(d.media), "flash_cf");
    bool external = d.removable || d.media_removable || d.ejectable || bus == "usb" ||
                    bus == "sdio" || bus == "ieee1394";
    if (!external) continue;
    RemovableDrive r;
    r.object = d.object;
    r.model = trim(trim(d.vendor) + " " + trim(d.model));
    r.size = d.size;
    r.can_power_off = d.can_power_off;
    r.ejectable = d.ejectable;
    bool system = false;
    // The whole-disk block (the one without a partition, i.e. the device the
    // partitions belong to) names the drive.
    for (const auto &b : blocks) {
      if (b.drive != d.object) continue;
      for (const auto &mp : b.mount_points)
        if (system_mount(mp)) system = true;
      if (b.hint_system && !d.removable && !d.media_removable && bus != "usb" && bus != "sdio")
        system = true;
      // The whole-disk block (not a partition) names the drive.
      if (!b.partition && (r.path.empty() || b.device.size() < r.path.size())) {
        r.path = b.device;
        r.name = base_name(b.device);
        if (!r.size) r.size = b.size;
      }
      if (!b.filesystem || b.hint_ignore || b.usage == "other" || b.fstype == "swap") continue;
      DrivePartition p;
      p.object = b.object;
      p.path = b.device;
      p.label = !trim(b.hint_name).empty() ? trim(b.hint_name) : trim(b.label);
      p.fstype = b.fstype;
      p.size = b.size;
      p.mountpoint = b.mount_points.empty() ? "" : b.mount_points.front();
      r.parts.push_back(p);
    }
    if (system || r.parts.empty()) continue;
    if (r.path.empty()) {  // only partitions are known
      r.path = r.parts.front().path;
      r.name = base_name(r.path);
    }
    std::sort(r.parts.begin(), r.parts.end(),
              [](const DrivePartition &a, const DrivePartition &b) { return a.path < b.path; });
    classify(r, card || starts_with(r.name, "mmcblk"), d.rotation_rate > 0);
    for (const auto &p : r.parts)
      if (!p.label.empty()) {
        r.label = p.label;
        break;
      }
    if (r.label.empty()) r.label = r.model.empty() ? r.name : r.model;
    out.push_back(r);
  }
  std::sort(out.begin(), out.end(),
            [](const RemovableDrive &a, const RemovableDrive &b) { return a.path < b.path; });
  return out;
}

namespace {
std::vector<std::string> text_lines(const std::string &s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string l;
  while (std::getline(in, l)) out.push_back(l);
  return out;
}
// KEY="value" pairs of one `lsblk -P` line; \xNN escapes are decoded.
std::map<std::string, std::string> lsblk_pairs(const std::string &line) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    size_t eq = line.find("=\"", i);
    if (eq == std::string::npos) break;
    std::string key = line.substr(i, eq - i), value;
    size_t j = eq + 2;
    for (; j < line.size() && line[j] != '"'; ++j) {
      if (line[j] == '\\' && j + 3 < line.size() && line[j + 1] == 'x' &&
          isxdigit((unsigned char)line[j + 2]) && isxdigit((unsigned char)line[j + 3])) {
        value += char(strtol(line.substr(j + 2, 2).c_str(), nullptr, 16));
        j += 3;
      } else {
        value += line[j];
      }
    }
    // util-linux 2.37+ may call the column MOUNTPOINTS.
    if (key == "MOUNTPOINTS") key = "MOUNTPOINT";
    out[key] = value;
    i = j + 1;
  }
  return out;
}
}  // namespace

std::vector<RemovableDrive> parse_removable_drives(const std::string &text) {
  std::vector<RemovableDrive> drives;
  std::vector<std::map<std::string, std::string>> rows;
  for (const auto &l : text_lines(text))
    if (!l.empty()) rows.push_back(lsblk_pairs(l));
  for (const auto &d : rows) {
    auto get = [&](const char *k) {
      auto it = d.find(k);
      return it == d.end() ? std::string() : it->second;
    };
    if (get("TYPE") != "disk") continue;
    std::string name = get("NAME"), tran = get("TRAN");
    bool hot = get("HOTPLUG") == "1" || get("RM") == "1" || tran == "usb" || tran == "mmc" ||
               starts_with(name, "mmcblk");
    if (!hot || starts_with(name, "loop") || starts_with(name, "zram") || starts_with(name, "sr") ||
        starts_with(name, "ram"))
      continue;
    RemovableDrive r;
    r.name = name;
    r.path = get("PATH");
    if (r.path.empty()) r.path = "/dev/" + name;
    r.size = atoll(get("SIZE").c_str());
    std::string vendor = trim(get("VENDOR")), model = trim(get("MODEL"));
    r.model = trim((starts_with(vendor, "0x") ? "" : vendor) + " " +
                   (starts_with(model, "0x") ? "" : model));
    r.can_power_off = tran == "usb";
    r.ejectable = get("RM") == "1";
    bool system = false, has_children = false;
    for (const auto &c : rows) {
      auto cget = [&](const char *k) {
        auto it = c.find(k);
        return it == c.end() ? std::string() : it->second;
      };
      // PKNAME is the parent kernel name; older lsblk leave it out of -P
      // output for partitions, which then follow their disk with a
      // matching name prefix.
      std::string pk = cget("PKNAME"), cname = cget("NAME");
      bool child = pk == name || (pk.empty() && cname != name && starts_with(cname, name) &&
                                  cget("TYPE") == "part");
      if (!child) continue;
      has_children = true;
      std::string type = cget("TYPE");
      if (type != "part" && type != "crypt" && type != "lvm") continue;
      if (system_mount(cget("MOUNTPOINT"))) system = true;
      if (!cget("FSTYPE").empty() || !cget("MOUNTPOINT").empty()) {
        DrivePartition p;
        p.path = cget("PATH").empty() ? "/dev/" + cname : cget("PATH");
        p.label = cget("LABEL");
        p.mountpoint = cget("MOUNTPOINT");
        p.fstype = cget("FSTYPE");
        p.size = atoll(cget("SIZE").c_str());
        r.parts.push_back(p);
      }
    }
    if (system_mount(get("MOUNTPOINT"))) system = true;
    if (!has_children && (!get("FSTYPE").empty() || !get("MOUNTPOINT").empty())) {
      DrivePartition p;
      p.path = r.path;
      p.label = get("LABEL");
      p.mountpoint = get("MOUNTPOINT");
      p.fstype = get("FSTYPE");
      p.size = r.size;
      r.parts.push_back(p);
    }
    if (system || r.parts.empty()) continue;
    classify(r, starts_with(name, "mmcblk") || tran == "mmc", get("ROTA") == "1");
    for (const auto &p : r.parts)
      if (!p.label.empty()) {
        r.label = p.label;
        break;
      }
    if (r.label.empty()) r.label = r.model.empty() ? name : r.model;
    drives.push_back(r);
  }
  return drives;
}

std::vector<std::string> parse_busy_programs(const std::string &text) {
  // fuser -vm: "                     USER        PID ACCESS COMMAND"
  //            "/media/u/DATA:       root     kernel mount /media/u/DATA"
  //            "                     user       1234 F.... soffice.bin"
  // lsof -Fc:  "p1234" / "csoffice.bin"
  std::vector<std::string> out;
  auto add = [&](std::string name) {
    name = printable_text(name);
    if (name.empty() || name == "kernel" || name == "mount") return;
    if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
  };
  for (const auto &raw : text_lines(text)) {
    std::string l = trim(raw);
    if (l.empty()) continue;
    if (l.size() > 1 && l[0] == 'c' && raw[0] == 'c') {
      add(l.substr(1));
      continue;
    }
    if (l[0] == 'p' && l.size() > 1 && isdigit((unsigned char)l[1])) continue;
    std::vector<std::string> f;
    std::istringstream in(l);
    std::string w;
    while (in >> w) f.push_back(w);
    if (f.size() < 4 || f[0] == "USER") continue;
    if (f[1] == "kernel" || (f.size() >= 3 && f[2] == "kernel")) continue;
    // [mount:] user pid access command
    size_t k = f[0].back() == ':' ? 1 : 0;
    if (f.size() < k + 4) continue;
    if (!std::all_of(f[k + 1].begin(), f[k + 1].end(), [](char c) { return isdigit((unsigned char)c); }))
      continue;
    add(f[k + 3]);
  }
  return out;
}


// ------------------------------------------------------- battery (alpha51)

std::string battery_duration_text(int minutes) {
  if (minutes <= 0) return "";
  int h = minutes / 60, m = minutes % 60;
  char hours[64] = "", mins[64] = "";
  if (h) snprintf(hours, sizeof hours, ngettext("%d hour", "%d hours", (unsigned long)h), h);
  if (m) snprintf(mins, sizeof mins, ngettext("%d minute", "%d minutes", (unsigned long)m), m);
  if (h && m) return std::string(hours) + " " + mins;
  return h ? hours : mins;
}

std::string battery_status_line(bool charging, bool full, int minutes) {
  if (full) return _("Fully charged");
  std::string time = battery_duration_text(minutes);
  char buf[192];
  if (charging) {
    if (time.empty()) return _("Charging");
    snprintf(buf, sizeof buf, _("Charging - %s until full"), time.c_str());
    return buf;
  }
  if (time.empty()) return _("Using battery power");
  snprintf(buf, sizeof buf, _("Using battery power - %s remaining"), time.c_str());
  return buf;
}

// ------------------------------------------- battery indicator (0.2 Alpha 2)

BatteryPhase battery_phase(int state, double percent, int warning, bool on_battery, int low,
                           int critical) {
  switch (state) {
    case 1:
      return BatteryPhase::CHARGING;
    case 4:
      return BatteryPhase::FULL;
    case 5:
      return BatteryPhase::PLUGGED;
    case 2:
    case 3:
    case 6:
      break;
    default:
      // Unknown: on mains power it is not charging, else it discharges.
      if (!on_battery) return percent >= 99 ? BatteryPhase::FULL : BatteryPhase::PLUGGED;
  }
  if (state == 3 || warning >= 4 || percent <= critical) return BatteryPhase::CRITICAL;
  if (warning == 3 || percent <= low) return BatteryPhase::LOW;
  return BatteryPhase::DISCHARGING;
}

int battery_state_from_sysfs(const std::string &status, double percent, bool ac_online) {
  std::string s = lower(trim(status));
  if (s == "charging") return 1;
  if (s == "discharging") return 2;
  if (s == "full") return 4;
  // "Not charging" (a charge limit, or full on some firmware) and
  // "Unknown" on mains power.
  if (s == "not charging" || ac_online) return percent >= 95 ? 4 : 5;
  return 2;
}

const char *battery_phase_name(BatteryPhase phase) {
  switch (phase) {
    case BatteryPhase::LOW: return "low";
    case BatteryPhase::CRITICAL: return "critical";
    case BatteryPhase::CHARGING: return "charging";
    case BatteryPhase::FULL: return "full";
    case BatteryPhase::PLUGGED: return "plugged";
    case BatteryPhase::DISCHARGING: break;
  }
  return "discharging";
}

bool battery_on_battery(BatteryPhase phase) {
  return phase == BatteryPhase::DISCHARGING || phase == BatteryPhase::LOW ||
         phase == BatteryPhase::CRITICAL;
}

int battery_low_level(int power_low) { return std::max(20, std::min(50, power_low)); }
int battery_critical_level(int power_critical) { return std::max(10, std::min(30, power_critical)); }

BatteryDisplay parse_battery_display(const std::string &name) {
  std::string n = lower(trim(name));
  if (n == "percent" || n == "percentage") return BatteryDisplay::PERCENT;
  if (n == "label") return BatteryDisplay::LABEL;
  if (n == "label-percent" || n == "label+percent" || n == "both") return BatteryDisplay::LABEL_PERCENT;
  return BatteryDisplay::ICON;
}

const char *battery_display_name(BatteryDisplay display) {
  switch (display) {
    case BatteryDisplay::PERCENT: return "percent";
    case BatteryDisplay::LABEL: return "label";
    case BatteryDisplay::LABEL_PERCENT: return "label-percent";
    case BatteryDisplay::ICON: break;
  }
  return "icon";
}

std::string battery_label(BatteryPhase phase) {
  switch (phase) {
    case BatteryPhase::LOW: return _("Low battery");
    case BatteryPhase::CRITICAL: return _("Critically low");
    case BatteryPhase::CHARGING: return _("Charging");
    case BatteryPhase::FULL: return _("Full");
    case BatteryPhase::PLUGGED: return _("Plugged in");
    case BatteryPhase::DISCHARGING: break;
  }
  return _("On battery");
}

std::string battery_duration_short(int minutes) {
  if (minutes <= 0) return "";
  int h = minutes / 60, m = minutes % 60;
  if (h && m) return format(_("%d h %d min"), h, m);
  return h ? format(_("%d h"), h) : format(_("%d min"), m);
}

std::string battery_time_label(BatteryPhase phase, int minutes, bool calculating) {
  if (!battery_on_battery(phase) && phase != BatteryPhase::CHARGING) return "";
  std::string time = battery_duration_short(minutes);
  if (time.empty()) return calculating ? _("Calculating…") : "";
  return format(phase == BatteryPhase::CHARGING ? _("%s to full") : _("%s left"), time.c_str());
}

std::string battery_panel_text(BatteryDisplay display, BatteryPhase phase, int percent, int minutes,
                               bool calculating, bool time) {
  if (display == BatteryDisplay::ICON) return "";
  std::string pct = format("%d%%", std::max(0, std::min(100, percent)));
  if (display == BatteryDisplay::PERCENT) return pct;
  std::string label = time ? battery_time_label(phase, minutes, calculating) : "";
  if (label.empty()) label = battery_label(phase);
  return display == BatteryDisplay::LABEL ? label : label + " · " + pct;
}

std::string battery_phase_line(BatteryPhase phase, int minutes, bool calculating) {
  if (phase == BatteryPhase::FULL) return _("Fully charged");
  if (phase == BatteryPhase::PLUGGED) return _("Plugged in, not charging");
  std::string state = phase == BatteryPhase::CHARGING   ? _("Charging")
                      : phase == BatteryPhase::LOW      ? _("Low battery")
                      : phase == BatteryPhase::CRITICAL ? _("Critically low")
                                                        : _("Using battery power");
  std::string time = battery_duration_text(minutes);
  if (!time.empty())
    return state + " - " +
           format(phase == BatteryPhase::CHARGING ? _("%s until full") : _("%s remaining"),
                  time.c_str());
  if (calculating) return state + " - " + _("Calculating…");
  if (phase == BatteryPhase::CRITICAL) return state + " - " + _("plug in the charger");
  return state;
}

std::string battery_tooltip(BatteryPhase phase, int percent, int minutes, bool calculating) {
  std::string tip = format(_("Battery %d%%"), std::max(0, std::min(100, percent)));
  if (phase == BatteryPhase::FULL) return tip + " · " + _("fully charged");
  if (phase == BatteryPhase::PLUGGED) return tip + " · " + _("plugged in, not charging");
  if (phase == BatteryPhase::CHARGING) tip += std::string(" · ") + _("charging");
  if (phase == BatteryPhase::LOW) tip += std::string(" · ") + _("low");
  if (phase == BatteryPhase::CRITICAL) tip += std::string(" · ") + _("critically low");
  std::string time = battery_duration_text(minutes);
  if (!time.empty())
    tip += " · " + format(phase == BatteryPhase::CHARGING ? _("%s until full") : _("%s remaining"),
                          time.c_str());
  else if (calculating)
    tip += std::string(" · ") + _("calculating…");
  return tip;
}

bool battery_motion(const std::string &mode, const std::string &effects, bool low_resource,
                    bool reduced_motion, bool animations) {
  std::string m = lower(trim(mode)), e = lower(trim(effects));
  if (m == "false" || m == "off" || m == "no" || m == "0") return false;
  if (m == "true" || m == "on" || m == "yes" || m == "1") return true;
  bool effects_off = e == "false" || e == "off" || e == "no" || e == "0";
  return !effects_off && !low_resource && !reduced_motion && animations;
}

bool battery_animated(BatteryPhase phase) {
  return phase == BatteryPhase::CHARGING || phase == BatteryPhase::LOW ||
         phase == BatteryPhase::CRITICAL;
}

double battery_pulse_period(BatteryPhase phase) {
  if (phase == BatteryPhase::CRITICAL) return 1.2;
  return battery_animated(phase) ? 2.0 : 0;
}

void BatteryAnimation::set(int percent, BatteryPhase phase, bool motion, double t) {
  double level = std::max(0, std::min(100, percent));
  if (!started_) {
    started_ = true;
    from_ = to_ = level;
    since_ = start_ = t;
  } else if (level != to_) {
    // From wherever the level is shown now.
    from_ = motion ? frame(t).level : level;
    to_ = level;
    since_ = t;
  }
  if (phase != phase_) start_ = t;  // the sweep and pulse start from their beginning
  phase_ = phase;
  motion_ = motion;
}

BatteryFrame BatteryAnimation::frame(double t) const {
  BatteryFrame f;
  double e = motion_ && kEase > 0 ? std::max(0.0, std::min(1.0, (t - since_) / kEase)) : 1.0;
  double u = 1 - e;
  f.level = from_ + (to_ - from_) * (1 - u * u * u);  // ease-out cubic
  if (!motion_ || !battery_animated(phase_)) return f;
  double period = battery_pulse_period(phase_), cycle = std::fmod(std::max(0.0, t - start_), period) / period;
  if (phase_ == BatteryPhase::CHARGING)
    f.sweep = cycle;
  else  // a soft pulse from full strength down to 45 % and back
    f.pulse = 0.725 + 0.275 * std::cos(2 * M_PI * cycle);
  return f;
}

bool BatteryAnimation::running(double t) const {
  if (!started_) return false;
  if (motion_ && battery_animated(phase_)) return true;
  return motion_ && from_ != to_ && t - since_ < kEase;
}

namespace {
// 0 not moving (full, plugged in), 1 discharging, 2 charging.
int battery_group(BatteryPhase phase) {
  if (phase == BatteryPhase::CHARGING) return 2;
  return battery_on_battery(phase) ? 1 : 0;
}
}  // namespace

void BatteryEstimator::reset() { *this = BatteryEstimator(); }

void BatteryEstimator::update(const BatteryReading &r) {
  if (!started_ || battery_group(r.phase) != battery_group(phase_)) {
    // A new direction: the old rate says nothing about the new one.
    reset();
    started_ = true;
    since_ = last_t_ = r.t;
  }
  phase_ = r.phase;
  int group = battery_group(r.phase);
  if (group == 0) {
    minutes_ = -1;
    calculating_ = false;
    return;
  }
  // The energy in the reading's unit; without it the percentage.
  bool energy = r.energy_full > 0 && r.energy >= 0;
  double now = energy ? std::min(r.energy, r.energy_full) : r.percent;
  double full = energy ? r.energy_full : 100;
  double rate = energy ? std::fabs(r.rate) : 0;
  if (rate <= 0) {
    // No reported rate: how fast the charge moved over at least a minute.
    if (mark_t_ < 0) {
      mark_t_ = r.t;
      mark_energy_ = now;
    } else if (r.t - mark_t_ >= 60 && now != mark_energy_) {
      rate = std::fabs(now - mark_energy_) / ((r.t - mark_t_) / 3600.0);
      mark_t_ = r.t;
      mark_energy_ = now;
    }
  }
  if (rate > 0) {
    // Exponential smoothing weighted by the time since the last rate.
    if (rate_ <= 0)
      rate_ = rate;
    else
      rate_ += (1 - std::exp(-std::max(0.0, r.t - last_t_) / kRateTau)) * (rate - rate_);
    last_t_ = r.t;
  }
  long long upower = group == 2 ? r.time_to_full : r.time_to_empty;
  int m = -1;
  if (upower > 0) {
    m = int((upower + 30) / 60);
  } else if (rate_ > 0) {
    double hours = (group == 2 ? full - now : now) / rate_;
    if (hours >= 0) m = std::max(1, int(hours * 60 + 0.5));
  }
  if (m > kMaxHours * 60) m = -1;
  minutes_ = m;
  calculating_ = m < 0 && r.t - since_ < kCalculatingFor;
}

std::string battery_model_name(const std::string &manufacturer, const std::string &model) {
  std::string a = trim(manufacturer), b = trim(model);
  if (a.empty()) return b;
  if (b.empty()) return a;
  // Some firmware repeats the vendor in the model name.
  if (b.compare(0, a.size(), a) == 0) return b;
  return a + " " + b;
}

std::vector<std::string> ordered_power_profiles(const std::vector<std::string> &profiles) {
  std::vector<std::string> out;
  for (const char *known : {"power-saver", "balanced", "performance"})
    if (std::find(profiles.begin(), profiles.end(), known) != profiles.end()) out.push_back(known);
  for (const auto &p : profiles)
    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  return out;
}

// ------------------------------------------------ taskbar previews (alpha51)

PreviewSize preview_fit(int win_w, int win_h, int max_w, int max_h) {
  PreviewSize s;
  if (win_w <= 0 || win_h <= 0 || max_w <= 0 || max_h <= 0) return s;
  double k = std::min({1.0, double(max_w) / win_w, double(max_h) / win_h});
  s.w = std::max(1, int(win_w * k + 0.5));
  s.h = std::max(1, int(win_h * k + 0.5));
  return s;
}

int preview_column_width(int count, int preferred, int minimum, int gap, int available) {
  if (count <= 0) return preferred;
  int fit = (available - (count - 1) * gap) / count;
  return std::max(minimum, std::min(preferred, fit));
}
