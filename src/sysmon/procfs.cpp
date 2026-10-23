// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "procfs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace sysmon {

namespace {

// Splits a line on blanks. /proc files never quote fields.
std::vector<std::string> fields(const std::string &line) {
  std::vector<std::string> out;
  size_t i = 0, n = line.size();
  while (i < n) {
    while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t start = i;
    while (i < n && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
  return out;
}

uint64_t to_u64(const std::string &s) { return std::strtoull(s.c_str(), nullptr, 10); }
int64_t to_i64(const std::string &s) { return std::strtoll(s.c_str(), nullptr, 10); }

template <typename Fn>
void each_line(const std::string &text, Fn fn) {
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    if (end > pos) fn(text.substr(pos, end - pos));
    pos = end + 1;
  }
}

bool starts(const std::string &s, const char *prefix) {
  return s.compare(0, std::strlen(prefix), prefix) == 0;
}

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

}  // namespace

// ------------------------------------------------------------- /proc/stat

static CpuTimes cpu_times(const std::vector<std::string> &f) {
  CpuTimes t;
  uint64_t *slots[] = {&t.user, &t.nice, &t.system, &t.idle, &t.iowait, &t.irq, &t.softirq, &t.steal};
  for (size_t i = 0; i < 8 && i + 1 < f.size(); ++i) *slots[i] = to_u64(f[i + 1]);
  return t;
}

bool parse_proc_stat(const std::string &text, CpuStat &out) {
  out = CpuStat();
  bool have_total = false;
  each_line(text, [&](const std::string &line) {
    if (starts(line, "cpu")) {
      auto f = fields(line);
      if (f.empty()) return;
      if (f[0] == "cpu") {
        out.total = cpu_times(f);
        have_total = true;
      } else {
        char *end = nullptr;
        long index = std::strtol(f[0].c_str() + 3, &end, 10);
        if (!end || *end || index < 0 || index > 8192) return;
        if (out.cores.size() <= size_t(index)) out.cores.resize(index + 1);
        out.cores[index] = cpu_times(f);
      }
    } else if (starts(line, "btime ")) {
      out.boot_time = to_u64(line.substr(6));
    } else if (starts(line, "ctxt ")) {
      out.context_switches = to_u64(line.substr(5));
    } else if (starts(line, "procs_running ")) {
      out.procs_running = int(to_i64(line.substr(14)));
    }
  });
  return have_total;
}

double cpu_percent(const CpuTimes &prev, const CpuTimes &cur) {
  if (cur.total() <= prev.total()) return 0;
  double total = double(cur.total() - prev.total());
  double busy = cur.busy() >= prev.busy() ? double(cur.busy() - prev.busy()) : 0;
  return std::max(0.0, std::min(100.0, busy * 100.0 / total));
}

double process_cpu_percent(uint64_t prev_ticks, uint64_t cur_ticks, double seconds,
                           long clock_ticks, int cpus) {
  if (cur_ticks <= prev_ticks || seconds <= 0 || clock_ticks <= 0 || cpus <= 0) return 0;
  double pct = double(cur_ticks - prev_ticks) * 100.0 / (seconds * double(clock_ticks) * cpus);
  return std::min(100.0, pct);
}

// ---------------------------------------------------------- /proc/meminfo

bool parse_meminfo(const std::string &text, MemInfo &out) {
  out = MemInfo();
  bool have_available = false, have_total = false;
  each_line(text, [&](const std::string &line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return;
    std::string key = line.substr(0, colon);
    uint64_t kb = to_u64(trim(line.substr(colon + 1)));
    uint64_t v = kb * 1024;
    if (key == "MemTotal") out.total = v, have_total = true;
    else if (key == "MemFree") out.free = v;
    else if (key == "MemAvailable") out.available = v, have_available = true;
    else if (key == "Buffers") out.buffers = v;
    else if (key == "Cached") out.cached = v;
    else if (key == "SReclaimable") out.reclaimable = v;
    else if (key == "Shmem") out.shmem = v;
    else if (key == "SwapTotal") out.swap_total = v;
    else if (key == "SwapFree") out.swap_free = v;
    else if (key == "Dirty") out.dirty = v;
  });
  // Kernels before 3.14 have no MemAvailable.
  if (!have_available) out.available = std::min(out.total, out.free + out.buffers + out.cached);
  return have_total;
}

// ------------------------------------------------------- /proc/<pid>/stat

bool parse_pid_stat(const std::string &text, PidStat &out) {
  out = PidStat();
  // The command name is in parentheses and may itself contain ") (" or
  // spaces, so it ends at the LAST closing parenthesis of the line.
  size_t open = text.find('(');
  size_t close = text.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return false;
  out.pid = int(std::strtol(text.c_str(), nullptr, 10));
  out.comm = text.substr(open + 1, close - open - 1);
  auto f = fields(text.substr(close + 1));
  // f[0] is field 3 (state) of proc(5).
  if (f.size() < 22) return false;
  out.state = f[0].empty() ? '?' : f[0][0];
  out.ppid = int(to_i64(f[1]));
  out.utime = to_u64(f[11]);
  out.stime = to_u64(f[12]);
  out.priority = long(to_i64(f[15]));
  out.nice = long(to_i64(f[16]));
  out.threads = int(to_i64(f[17]));
  out.start_time = to_u64(f[19]);
  out.vsize = to_u64(f[20]);
  out.rss_pages = to_i64(f[21]);
  return out.pid > 0;
}

bool parse_pid_io(const std::string &text, PidIo &out) {
  out = PidIo();
  int found = 0;
  each_line(text, [&](const std::string &line) {
    if (starts(line, "read_bytes:")) out.read_bytes = to_u64(trim(line.substr(11))), ++found;
    else if (starts(line, "write_bytes:")) out.write_bytes = to_u64(trim(line.substr(12))), ++found;
  });
  return found == 2;
}

int parse_status_uid(const std::string &text) {
  int uid = -1;
  each_line(text, [&](const std::string &line) {
    if (uid < 0 && starts(line, "Uid:")) {
      auto f = fields(line.substr(4));
      if (!f.empty()) uid = int(to_i64(f[0]));
    }
  });
  return uid;
}

std::vector<std::string> split_nul(const std::string &text) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\0', pos);
    if (end == std::string::npos) end = text.size();
    out.push_back(text.substr(pos, end - pos));
    pos = end + 1;
  }
  return out;
}

const char *state_name(char state) {
  switch (state) {
    case 'R': return "Running";
    case 'S': return "Sleeping";
    case 'D': return "Waiting";  // uninterruptible disk sleep
    case 'T': return "Stopped";
    case 't': return "Traced";
    case 'Z': return "Zombie";
    case 'X': return "Dead";
    case 'I': return "Idle";
    case 'P': return "Parked";
    default: return "Unknown";
  }
}

// ------------------------------------------------------- /proc/diskstats

std::vector<DiskStat> parse_diskstats(const std::string &text) {
  std::vector<DiskStat> out;
  each_line(text, [&](const std::string &line) {
    auto f = fields(line);
    if (f.size() < 14) return;
    DiskStat d;
    d.major = unsigned(to_u64(f[0]));
    d.minor = unsigned(to_u64(f[1]));
    d.name = f[2];
    d.reads = to_u64(f[3]);
    d.sectors_read = to_u64(f[5]);
    d.writes = to_u64(f[7]);
    d.sectors_written = to_u64(f[9]);
    d.io_ticks_ms = to_u64(f[12]);
    out.push_back(d);
  });
  return out;
}

bool interesting_disk(const std::string &name) {
  for (const char *skip : {"loop", "ram", "zram", "fd", "sr", "md", "dm-", "nbd"})
    if (starts(name, skip)) return false;
  return !name.empty();
}

// --------------------------------------------------------- /proc/net/dev

std::vector<NetDev> parse_net_dev(const std::string &text) {
  std::vector<NetDev> out;
  each_line(text, [&](const std::string &line) {
    size_t colon = line.find(':');
    // The two header lines contain '|' but no ':'.
    if (colon == std::string::npos) return;
    NetDev d;
    d.name = trim(line.substr(0, colon));
    auto f = fields(line.substr(colon + 1));
    if (d.name.empty() || f.size() < 10) return;
    d.rx_bytes = to_u64(f[0]);
    d.rx_packets = to_u64(f[1]);
    d.tx_bytes = to_u64(f[8]);
    d.tx_packets = to_u64(f[9]);
    out.push_back(d);
  });
  return out;
}

bool interesting_interface(const std::string &name) {
  if (name.empty() || name == "lo") return false;
  for (const char *skip : {"veth", "virbr", "docker", "br-", "vnet", "tap"})
    if (starts(name, skip)) return false;
  return true;
}

// -------------------------------------------------------- misc /proc files

bool parse_loadavg(const std::string &text, LoadAvg &out) {
  out = LoadAvg();
  auto f = fields(trim(text));
  if (f.size() < 4) return false;
  out.one = std::strtod(f[0].c_str(), nullptr);
  out.five = std::strtod(f[1].c_str(), nullptr);
  out.fifteen = std::strtod(f[2].c_str(), nullptr);
  size_t slash = f[3].find('/');
  if (slash != std::string::npos) {
    out.running = int(to_i64(f[3].substr(0, slash)));
    out.threads = int(to_i64(f[3].substr(slash + 1)));
  }
  return true;
}

double parse_uptime(const std::string &text) {
  char *end = nullptr;
  double v = std::strtod(text.c_str(), &end);
  return end == text.c_str() ? -1 : v;
}

std::string parse_cpu_model(const std::string &cpuinfo) {
  std::string model;
  each_line(cpuinfo, [&](const std::string &line) {
    if (!model.empty()) return;
    // x86 "model name", ARM "Model"/"Hardware", POWER "cpu".
    for (const char *key : {"model name", "Model", "Hardware", "cpu model", "Processor"}) {
      if (starts(line, key)) {
        size_t colon = line.find(':');
        std::string k = trim(line.substr(0, colon));
        if (colon != std::string::npos && k == key) {
          model = trim(line.substr(colon + 1));
          break;
        }
      }
    }
  });
  // Collapse runs of blanks some vendors pad their strings with.
  std::string out;
  for (char c : model) {
    if (c == ' ' && !out.empty() && out.back() == ' ') continue;
    out += c;
  }
  return out;
}

double parse_cpuinfo_mhz(const std::string &cpuinfo) {
  double sum = 0;
  int n = 0;
  each_line(cpuinfo, [&](const std::string &line) {
    if (starts(line, "cpu MHz")) {
      size_t colon = line.find(':');
      if (colon != std::string::npos) {
        sum += std::strtod(line.c_str() + colon + 1, nullptr);
        ++n;
      }
    }
  });
  return n ? sum / n : 0;
}

// ------------------------------------------------------------ mounts

std::string unescape_mount(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7' && s[i + 2] >= '0' && s[i + 2] <= '7' &&
        s[i + 3] >= '0' && s[i + 3] <= '7') {
      out += char((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::vector<MountEntry> parse_mounts(const std::string &text) {
  std::vector<MountEntry> out;
  each_line(text, [&](const std::string &line) {
    auto f = fields(line);
    if (f.size() < 4) return;
    out.push_back({unescape_mount(f[0]), unescape_mount(f[1]), f[2], f[3]});
  });
  return out;
}

bool interesting_mount(const MountEntry &m) {
  static const char *skip_types[] = {"squashfs", "iso9660", "overlay", "tmpfs", "devtmpfs",
                                     "proc", "sysfs", "cgroup", "cgroup2", "autofs"};
  for (const char *t : skip_types)
    if (m.type == t) return false;
  if (starts(m.mount_point, "/snap/") || starts(m.mount_point, "/proc") ||
      starts(m.mount_point, "/sys") || starts(m.mount_point, "/run/credentials"))
    return false;
  // Real block devices, plus file systems that name a pool instead.
  return starts(m.device, "/dev/") || m.type == "zfs" || m.type == "fuseblk";
}

// --------------------------------------------------------- autostart

namespace {
struct Line {
  std::string text, key, group;
};
std::vector<Line> desktop_lines(const std::string &text) {
  std::vector<Line> out;
  std::string group;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    Line l;
    l.text = text.substr(pos, end - pos);
    std::string t = trim(l.text);
    if (!t.empty() && t[0] == '[') group = t;
    else if (!t.empty() && t[0] != '#') {
      size_t eq = t.find('=');
      if (eq != std::string::npos) l.key = trim(t.substr(0, eq));
    }
    l.group = group;
    out.push_back(l);
    pos = end + 1;
  }
  return out;
}
std::string value_of(const std::string &line) {
  size_t eq = line.find('=');
  return eq == std::string::npos ? "" : trim(line.substr(eq + 1));
}
}  // namespace

std::string set_autostart_hidden(const std::string &text, bool hidden) {
  auto lines = desktop_lines(text);
  std::string out;
  bool written = false, in_entry = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const Line &l = lines[i];
    bool entry = l.group == "[Desktop Entry]";
    if (in_entry && !entry && !written) {
      // Leaving [Desktop Entry] without a Hidden key: add it at its end.
      out += std::string("Hidden=") + (hidden ? "true" : "false") + "\n";
      written = true;
    }
    in_entry = entry;
    if (entry && l.key == "Hidden") {
      if (!written) out += std::string("Hidden=") + (hidden ? "true" : "false") + "\n";
      written = true;
      continue;
    }
    if (entry && !hidden && l.key == "X-GNOME-Autostart-enabled") {
      out += "X-GNOME-Autostart-enabled=true\n";
      continue;
    }
    out += l.text + "\n";
  }
  if (!written) {
    if (!in_entry) out += "[Desktop Entry]\n";
    out += std::string("Hidden=") + (hidden ? "true" : "false") + "\n";
  }
  return out;
}

bool autostart_enabled(const std::string &text) {
  bool enabled = true;
  for (auto &l : desktop_lines(text)) {
    if (l.group != "[Desktop Entry]") continue;
    if (l.key == "Hidden" && value_of(l.text) == "true") enabled = false;
    if (l.key == "X-GNOME-Autostart-enabled" && value_of(l.text) == "false") enabled = false;
  }
  return enabled;
}

// ----------------------------------------------------------- formatting

static std::string printf_string(const char *fmt, double v, const char *unit) {
  char buf[64];
  std::snprintf(buf, sizeof buf, fmt, v, unit);
  return buf;
}

std::string format_bytes(uint64_t bytes) {
  static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double v = double(bytes);
  int u = 0;
  while (v >= 1024 && u < 5) v /= 1024, ++u;
  if (u == 0) return printf_string("%.0f %s", v, units[u]);
  return printf_string(v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

std::string format_rate(double bps) {
  if (bps < 0.5) return "0 B/s";
  return format_bytes(uint64_t(bps + 0.5)) + "/s";
}

std::string format_bits_rate(double bps) {
  static const char *units[] = {"bit/s", "kbit/s", "Mbit/s", "Gbit/s", "Tbit/s"};
  double v = bps * 8;
  int u = 0;
  while (v >= 1000 && u < 4) v /= 1000, ++u;
  if (u == 0) return printf_string("%.0f %s", v, units[u]);
  return printf_string(v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

std::string format_duration(double seconds) {
  if (seconds < 0) seconds = 0;
  uint64_t s = uint64_t(seconds);
  uint64_t d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60, sec = s % 60;
  char buf[64];
  if (d) std::snprintf(buf, sizeof buf, "%llu d %02llu:%02llu:%02llu", (unsigned long long)d,
                       (unsigned long long)h, (unsigned long long)m, (unsigned long long)sec);
  else std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", (unsigned long long)h,
                     (unsigned long long)m, (unsigned long long)sec);
  return buf;
}

std::string format_percent(double pct) {
  char buf[32];
  std::snprintf(buf, sizeof buf, pct >= 99.95 || pct == 0 ? "%.0f %%" : "%.1f %%", pct);
  return buf;
}

double nice_ceiling(double value, double floor) {
  if (value <= floor) return floor;
  double p = std::pow(10.0, std::floor(std::log10(value)));
  for (double m : {1.0, 2.0, 5.0, 10.0})
    if (value <= m * p) return m * p;
  return 10 * p;
}

}  // namespace sysmon
