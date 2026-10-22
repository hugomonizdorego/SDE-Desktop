// SPDX-License-Identifier: GPL-3.0-or-later
#include "status_model.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

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

NetLed network_led(bool connected, int connectivity, bool probe_failed) {
  if (!connected) return NetLed::RED;
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

