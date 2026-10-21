// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE System Monitor: parsers for the text files of /proc and /sys. They take
// the file contents as a string so that the unit tests can feed fixtures; no
// function here touches FLTK or X11.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sysmon {

// ------------------------------------------------------------- /proc/stat

struct CpuTimes {
  uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0,
           steal = 0;
  // guest/guest_nice are already included in user/nice and are not added.
  uint64_t total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
  uint64_t idle_all() const { return idle + iowait; }
  uint64_t busy() const { return total() - idle_all(); }
};

struct CpuStat {
  CpuTimes total;
  std::vector<CpuTimes> cores;  // indexed by the N of "cpuN"
  uint64_t boot_time = 0;       // btime, seconds since the epoch
  uint64_t context_switches = 0;
  int procs_running = 0;
};

bool parse_proc_stat(const std::string &text, CpuStat &out);

// Busy share between two samples, 0-100. Returns 0 when no time passed and
// clamps counter resets (e.g. a core taken offline) to the valid range.
double cpu_percent(const CpuTimes &prev, const CpuTimes &cur);

// CPU share of one process between two samples as a percentage of the whole
// machine (100 % = every core busy). |ticks| are utime + stime.
double process_cpu_percent(uint64_t prev_ticks, uint64_t cur_ticks, double seconds,
                           long clock_ticks, int cpus);

// ---------------------------------------------------------- /proc/meminfo

struct MemInfo {
  uint64_t total = 0, free = 0, available = 0, buffers = 0, cached = 0, reclaimable = 0,
           shmem = 0, swap_total = 0, swap_free = 0, dirty = 0;  // bytes
  uint64_t used() const { return total > available ? total - available : 0; }
  uint64_t swap_used() const { return swap_total > swap_free ? swap_total - swap_free : 0; }
};

bool parse_meminfo(const std::string &text, MemInfo &out);

// ------------------------------------------------------- /proc/<pid>/stat

struct PidStat {
  int pid = 0;
  std::string comm;  // may contain spaces and parentheses
  char state = '?';
  int ppid = 0;
  uint64_t utime = 0, stime = 0;  // clock ticks
  long priority = 0, nice = 0;
  int threads = 0;
  uint64_t start_time = 0;  // clock ticks after boot
  uint64_t vsize = 0;       // bytes
  int64_t rss_pages = 0;
};

bool parse_pid_stat(const std::string &text, PidStat &out);

// /proc/<pid>/io: bytes that really went to or came from storage.
struct PidIo {
  uint64_t read_bytes = 0, write_bytes = 0;
};
bool parse_pid_io(const std::string &text, PidIo &out);

// "Uid:" (real uid) from /proc/<pid>/status; -1 when missing.
int parse_status_uid(const std::string &text);

// NUL separated /proc/<pid>/cmdline (or environ) into its strings.
std::vector<std::string> split_nul(const std::string &text);

// Human readable state for the State column ("R" -> "Running" ...).
const char *state_name(char state);

// ------------------------------------------------------- /proc/diskstats

struct DiskStat {
  std::string name;
  unsigned major = 0, minor = 0;
  uint64_t reads = 0, sectors_read = 0, writes = 0, sectors_written = 0, io_ticks_ms = 0;
};

std::vector<DiskStat> parse_diskstats(const std::string &text);

// True for block devices worth a graph: no loop, ram, zram, partitions are
// filtered by the caller via /sys/block.
bool interesting_disk(const std::string &name);

// --------------------------------------------------------- /proc/net/dev

struct NetDev {
  std::string name;
  uint64_t rx_bytes = 0, rx_packets = 0, tx_bytes = 0, tx_packets = 0;
};

std::vector<NetDev> parse_net_dev(const std::string &text);
bool interesting_interface(const std::string &name);

// -------------------------------------------------------- misc /proc files

struct LoadAvg {
  double one = 0, five = 0, fifteen = 0;
  int running = 0, threads = 0;
};
bool parse_loadavg(const std::string &text, LoadAvg &out);
double parse_uptime(const std::string &text);  // seconds, -1 on error
std::string parse_cpu_model(const std::string &cpuinfo);
double parse_cpuinfo_mhz(const std::string &cpuinfo);  // average "cpu MHz", 0 if none

// ------------------------------------------------------------ mounts

struct MountEntry {
  std::string device, mount_point, type, options;
};
// /proc/self/mounts with octal escapes (\040 ...) decoded.
std::vector<MountEntry> parse_mounts(const std::string &text);
std::string unescape_mount(const std::string &s);
// Local storage worth showing on the File systems page.
bool interesting_mount(const MountEntry &m);

// --------------------------------------------------------- autostart

// Returns |text| (a .desktop file) with Hidden= set for disabling (true) or
// enabling (false) the entry in its [Desktop Entry] group. Also clears the
// GNOME specific X-GNOME-Autostart-enabled=false when enabling.
std::string set_autostart_hidden(const std::string &text, bool hidden);
// True when the entry would be started (Hidden and X-GNOME-Autostart-enabled).
bool autostart_enabled(const std::string &text);

// ----------------------------------------------------------- formatting

std::string format_bytes(uint64_t bytes);        // "1.4 GiB"
std::string format_rate(double bytes_per_second);  // "12.3 MiB/s"
std::string format_bits_rate(double bytes_per_second);  // "8.1 Mbit/s"
std::string format_duration(double seconds);     // "3 d 04:12:09"
std::string format_percent(double pct);          // "12.3 %"
// Rounds a graph maximum up to 1, 2 or 5 times a power of ten (min |floor|).
double nice_ceiling(double value, double floor);

}  // namespace sysmon
