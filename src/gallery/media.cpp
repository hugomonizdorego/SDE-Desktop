// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "media.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

#include "util.h"

namespace gallery {

bool MountEntry::read_only() const {
  for (const auto &o : sde::split(options, ','))
    if (o == "ro") return true;
  return false;
}

std::string MountEntry::label() const {
  if (!name.empty()) return name;
  std::string l = sde::basename_of(mount_point);
  return l.empty() ? mount_point : l;
}

std::string unescape_mount_field(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size()) {
      const char *p = s.c_str() + i + 1;
      if (p[0] >= '0' && p[0] <= '3' && p[1] >= '0' && p[1] <= '7' && p[2] >= '0' && p[2] <= '7') {
        out += char((p[0] - '0') * 64 + (p[1] - '0') * 8 + (p[2] - '0'));
        i += 3;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

std::vector<MountEntry> parse_mountinfo(const std::string &text) {
  std::vector<MountEntry> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    // 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
    std::vector<std::string> f;
    std::istringstream ls(line);
    std::string tok;
    while (ls >> tok) f.push_back(tok);
    size_t dash = 0;
    for (size_t i = 6; i < f.size(); ++i)
      if (f[i] == "-") {
        dash = i;
        break;
      }
    if (f.size() < 10 || !dash || dash + 2 >= f.size()) continue;
    MountEntry m;
    m.mount_point = unescape_mount_field(f[4]);
    m.options = f[5];
    m.fs_type = f[dash + 1];
    m.source = unescape_mount_field(f[dash + 2]);
    if (dash + 3 < f.size()) m.options += "," + f[dash + 3];
    out.push_back(std::move(m));
  }
  return out;
}

bool is_removable_mount(const MountEntry &m) {
  static const std::set<std::string> pseudo{
      "proc",  "sysfs",   "tmpfs",     "devtmpfs", "devpts", "cgroup", "cgroup2",  "overlay",
      "squashfs", "autofs", "fusectl", "debugfs", "tracefs", "securityfs", "pstore", "bpf",
      "mqueue", "hugetlbfs", "configfs", "binfmt_misc", "efivarfs", "nfs", "nfs4", "cifs",
      "smb3", "fuse.gvfsd-fuse", "fuse.portal", "ramfs", "nsfs"};
  if (pseudo.count(m.fs_type)) return false;
  const std::string &mp = m.mount_point;
  auto below = [&](const std::string &dir) { return mp.size() > dir.size() && sde::starts_with(mp, dir); };
  return below("/media/") || below("/run/media/");
}

std::vector<MountEntry> removable_mounts(const std::string &text) {
  std::vector<MountEntry> out;
  std::set<std::string> seen;
  for (auto &m : parse_mountinfo(text)) {
    if (!is_removable_mount(m)) continue;
    // A later mount on the same point hides the earlier one.
    if (seen.count(m.mount_point)) {
      for (auto &o : out)
        if (o.mount_point == m.mount_point) o = m;
      continue;
    }
    seen.insert(m.mount_point);
    out.push_back(std::move(m));
  }
  return out;
}

std::vector<MountEntry> current_removable_mounts() {
  std::string text;
  if (!sde::read_file("/proc/self/mountinfo", text)) return {};
  return removable_mounts(text);
}

std::string gvfs_device_name(const std::string &folder) {
  size_t eq = folder.find("host=");
  std::string host = eq == std::string::npos ? folder : folder.substr(eq + 5);
  // Older gvfs escape the USB port: "%5Busb%3A001%2C005%5D".
  std::string decoded;
  for (size_t i = 0; i < host.size(); ++i) {
    if (host[i] == '%' && i + 2 < host.size() && isxdigit(static_cast<unsigned char>(host[i + 1])) && isxdigit(static_cast<unsigned char>(host[i + 2]))) {
      decoded += char(std::stoi(host.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      decoded += host[i];
    }
  }
  if (!decoded.empty() && decoded[0] == '[') return sde::starts_with(folder, "mtp") ? "Phone" : "Camera";
  // "Google_Pixel_7_2A111FDH200D4A": words, without the serial number at the end.
  auto words = sde::split(decoded, '_');
  if (words.size() > 1) {
    const std::string &last = words.back();
    bool serial = last.size() >= 6;
    int digits = 0;
    for (char c : last) {
      if (isdigit(static_cast<unsigned char>(c))) ++digits;
      else if (!isupper(static_cast<unsigned char>(c))) serial = false;
    }
    if (serial && digits > 0) words.pop_back();
  }
  std::string out;
  for (const auto &w : words) out += (out.empty() ? "" : " ") + w;
  return out.empty() ? decoded : out;
}

std::vector<MountEntry> gvfs_cameras(const std::string &runtime_dir) {
  std::vector<MountEntry> out;
  std::string base = runtime_dir + "/gvfs";
  auto names = sde::list_dir(base);
  std::sort(names.begin(), names.end());
  for (const auto &n : names) {
    bool mtp = sde::starts_with(n, "mtp:host="), ptp = sde::starts_with(n, "gphoto2:host=");
    if (!mtp && !ptp) continue;
    MountEntry m;
    m.source = n;
    m.mount_point = base + "/" + n;
    m.fs_type = mtp ? "mtp" : "gphoto2";
    m.name = gvfs_device_name(n);
    std::string host = n.substr(n.find("host=") + 5);
    m.uri = (mtp ? "mtp://" : "gphoto2://") + host + "/";
    out.push_back(m);
  }
  return out;
}

std::vector<GioVolume> parse_gio_volumes(const std::string &text) {
  // Volume(0): Pixel 7
  //   Type: GProxyVolume (GProxyVolumeMonitorMTP)
  //   activation_root=mtp://Google_Pixel_7_2A111FDH200D4A/
  //   ...
  // Mount(0): Pixel 7 -> mtp://Google_Pixel_7_2A111FDH200D4A/
  std::vector<GioVolume> out;
  std::vector<std::string> mounted;
  GioVolume *cur = nullptr;
  for (const auto &raw : sde::split(text, '\n')) {
    std::string line = sde::trim(raw);
    if (sde::starts_with(line, "Volume(")) {
      size_t colon = line.find("): ");
      out.push_back(GioVolume());
      cur = &out.back();
      cur->name = colon == std::string::npos ? "" : line.substr(colon + 3);
    } else if (sde::starts_with(line, "Mount(") || sde::starts_with(line, "Drive(")) {
      cur = nullptr;
      size_t arrow = line.find(" -> ");
      if (sde::starts_with(line, "Mount(") && arrow != std::string::npos) mounted.push_back(line.substr(arrow + 4));
    } else if (cur && sde::starts_with(line, "activation_root=")) {
      cur->root = line.substr(16);
    }
  }
  std::vector<GioVolume> cams;
  for (auto &v : out) {
    if (!sde::starts_with(v.root, "mtp://") && !sde::starts_with(v.root, "gphoto2://")) continue;
    v.mounted = std::find(mounted.begin(), mounted.end(), v.root) != mounted.end();
    cams.push_back(v);
  }
  return cams;
}

std::vector<std::pair<std::string, std::string>> parse_gphoto2_cameras(const std::string &text) {
  // Model                          Port
  // ----------------------------------------------------------
  // Canon EOS 600D                 usb:001,005
  std::vector<std::pair<std::string, std::string>> out;
  bool table = false;
  for (const auto &line : sde::split(text, '\n')) {
    if (sde::starts_with(line, "----")) {
      table = true;
      continue;
    }
    if (!table) continue;
    std::string t = sde::trim(line);
    // The port is the last word ("usb:001,005"), the model the rest.
    size_t space = t.find_last_of(" \t");
    if (t.empty() || space == std::string::npos) continue;
    std::string port = t.substr(space + 1), model = sde::trim(t.substr(0, space));
    if (!model.empty() && port.find(':') != std::string::npos) out.push_back({model, port});
  }
  return out;
}

std::string import_root(const std::string &pictures) { return pictures + "/Imported"; }

std::vector<ImportItem> plan_import(const std::vector<Photo> &photos, const std::string &dest_root,
                                    const std::function<long long(const std::string &)> &exists) {
  std::vector<ImportItem> out;
  std::set<std::string> taken;  // destinations planned in this run
  for (const auto &p : photos) {
    struct tm tm {};
    localtime_r(&p.mtime, &tm);
    char day[16];
    strftime(day, sizeof day, "%Y-%m-%d", &tm);
    std::string dir = dest_root + "/" + day;
    std::string name = p.name.empty() ? sde::basename_of(p.path) : p.name;
    std::string stem = name, ext;
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) {
      stem = name.substr(0, dot);
      ext = name.substr(dot);
    }
    ImportItem item;
    item.source = p.path;
    for (int n = 1; n < 10000; ++n) {
      std::string cand = dir + "/" + (n == 1 ? name : stem + " (" + std::to_string(n) + ")" + ext);
      if (taken.count(cand)) continue;
      long long size = exists ? exists(cand) : -1;
      if (size < 0) {
        item.dest = cand;
        break;
      }
      if (size == p.size) {
        item.dest = cand;
        item.skip = true;
        break;
      }
    }
    if (item.dest.empty()) continue;
    taken.insert(item.dest);
    out.push_back(std::move(item));
  }
  return out;
}

bool copy_file(const std::string &from, const std::string &to, std::string *error) {
  auto fail = [&](const std::string &what) {
    if (error) *error = what + ": " + strerror(errno);
    return false;
  };
  int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return fail(from);
  struct stat st {};
  fstat(in, &st);
  std::string dir = to.substr(0, to.rfind('/'));
  if (!dir.empty() && !sde::make_dirs(dir)) {
    close(in);
    return fail(dir);
  }
  std::string tmp = dir + "/.sde-import-" + std::to_string(getpid()) + "-" + sde::basename_of(to);
  int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    close(in);
    return fail(tmp);
  }
  char buf[1 << 16];
  bool ok = true;
  for (;;) {
    ssize_t n = read(in, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(out, buf + off, size_t(n - off));
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        ok = false;
        break;
      }
      off += w;
    }
    if (!ok) break;
  }
  int saved = errno;
  close(in);
  if (close(out) != 0) ok = false;
  if (ok) {
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    utimensat(AT_FDCWD, tmp.c_str(), times, 0);
    if (rename(tmp.c_str(), to.c_str()) != 0) ok = false;
  }
  if (!ok) {
    errno = saved ? saved : errno;
    unlink(tmp.c_str());
    return fail(to);
  }
  return true;
}

}  // namespace gallery
