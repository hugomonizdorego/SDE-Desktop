// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Unit tests for the SDE System Monitor /proc and /sys parsers.
#include <cmath>
#include <cstdio>
#include <string>

#include "procfs.h"

using namespace sysmon;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static void test_proc_stat() {
  const std::string a =
      "cpu  100 0 50 800 50 0 0 0 0 0\n"
      "cpu0 60 0 30 380 30 0 0 0 0 0\n"
      "cpu1 40 0 20 420 20 0 0 0 0 0\n"
      "intr 12345 1 2 3\n"
      "ctxt 987654\n"
      "btime 1760000000\n"
      "processes 4242\n"
      "procs_running 3\n"
      "procs_blocked 0\n";
  CpuStat s;
  CHECK(parse_proc_stat(a, s));
  CHECK(s.cores.size() == 2);
  CHECK(s.total.user == 100 && s.total.system == 50 && s.total.idle == 800 && s.total.iowait == 50);
  CHECK(s.total.total() == 1000);
  CHECK(s.total.busy() == 150);
  CHECK(s.boot_time == 1760000000ULL);
  CHECK(s.context_switches == 987654ULL);
  CHECK(s.procs_running == 3);

  // Second sample: 100 more ticks in total, 25 of them busy.
  const std::string b =
      "cpu  115 0 60 870 55 0 0 0 0 0\n"
      "cpu0 75 0 40 380 30 0 0 0 0 0\n"
      "cpu1 40 0 20 490 25 0 0 0 0 0\n";
  CpuStat s2;
  CHECK(parse_proc_stat(b, s2));
  CHECK(near(cpu_percent(s.total, s2.total), 25.0));
  CHECK(near(cpu_percent(s.cores[0], s2.cores[0]), 100.0));  // 25 busy of 25
  CHECK(near(cpu_percent(s.cores[1], s2.cores[1]), 0.0));
  // No time passed and counter resets stay in range.
  CHECK(near(cpu_percent(s.total, s.total), 0.0));
  CHECK(near(cpu_percent(s2.total, s.total), 0.0));

  // An offline core leaves a gap: cpu0 and cpu2 only.
  CpuStat gap;
  CHECK(parse_proc_stat("cpu 1 2 3 4\ncpu0 1 1 1 1\ncpu2 1 1 1 1\n", gap));
  CHECK(gap.cores.size() == 3);
  CHECK(!parse_proc_stat("garbage\n", gap));
}

static void test_process_cpu() {
  // 50 ticks in one second on a 100 Hz clock is one full core: 25 % of 4.
  CHECK(near(process_cpu_percent(1000, 1050, 1.0, 100, 4), 12.5));
  CHECK(near(process_cpu_percent(1000, 1100, 1.0, 100, 4), 25.0));
  CHECK(near(process_cpu_percent(1000, 1100, 2.0, 100, 1), 50.0));
  CHECK(near(process_cpu_percent(1000, 1000, 1.0, 100, 4), 0.0));
  CHECK(near(process_cpu_percent(1100, 1000, 1.0, 100, 4), 0.0));  // pid reuse
  CHECK(near(process_cpu_percent(0, 100, 0.0, 100, 4), 0.0));       // no time
  CHECK(near(process_cpu_percent(0, 100000, 1.0, 100, 1), 100.0));  // clamped
}

static void test_meminfo() {
  const std::string text =
      "MemTotal:       16303428 kB\n"
      "MemFree:         1203456 kB\n"
      "MemAvailable:    9876543 kB\n"
      "Buffers:          234567 kB\n"
      "Cached:          6543210 kB\n"
      "SwapCached:            0 kB\n"
      "Shmem:            345678 kB\n"
      "SReclaimable:     456789 kB\n"
      "SwapTotal:       2097148 kB\n"
      "SwapFree:        2000000 kB\n"
      "Dirty:               128 kB\n"
      "HugePages_Total:       0\n";
  MemInfo m;
  CHECK(parse_meminfo(text, m));
  CHECK(m.total == 16303428ULL * 1024);
  CHECK(m.available == 9876543ULL * 1024);
  CHECK(m.used() == (16303428ULL - 9876543ULL) * 1024);
  CHECK(m.cached == 6543210ULL * 1024);
  CHECK(m.swap_used() == (2097148ULL - 2000000ULL) * 1024);
  CHECK(m.dirty == 128ULL * 1024);

  // Old kernels: MemAvailable is estimated from free + buffers + cached.
  MemInfo old;
  CHECK(parse_meminfo("MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 250 kB\n", old));
  CHECK(old.available == 400ULL * 1024);
  CHECK(!parse_meminfo("", old));
}

static void test_pid_stat() {
  PidStat st;
  const std::string plain =
      "1234 (bash) S 1000 1234 1234 34816 5678 4194304 1500 2000 0 0 120 45 3 1 20 0 1 0 "
      "567890 12345678 1024 18446744073709551615 1 1 0 0 0 0 65536 3670020 1266777851 0 0 0 17 "
      "3 0 0 0 0 0\n";
  CHECK(parse_pid_stat(plain, st));
  CHECK(st.pid == 1234);
  CHECK(st.comm == "bash");
  CHECK(st.state == 'S');
  CHECK(st.ppid == 1000);
  CHECK(st.utime == 120 && st.stime == 45);
  CHECK(st.priority == 20 && st.nice == 0);
  CHECK(st.threads == 1);
  CHECK(st.start_time == 567890ULL);
  CHECK(st.vsize == 12345678ULL);
  CHECK(st.rss_pages == 1024);

  // Names may contain spaces and parentheses; only the last ')' ends them.
  const std::string tricky =
      "42 (Web Content (x) )) R 7 42 42 0 -1 4194560 10 0 0 0 300 200 0 0 20 -5 27 0 99 "
      "2000000 5000 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 1 0 0 0 0 0\n";
  CHECK(parse_pid_stat(tricky, st));
  CHECK(st.pid == 42);
  CHECK(st.comm == "Web Content (x) )");
  CHECK(st.state == 'R');
  CHECK(st.ppid == 7);
  CHECK(st.utime == 300 && st.stime == 200);
  CHECK(st.nice == -5);
  CHECK(st.threads == 27);
  CHECK(st.start_time == 99);

  const std::string spaces =
      "77 (tmux: server) S 1 77 77 0 -1 0 0 0 0 0 1 2 0 0 20 19 1 0 5 100 3 0 0 0 0 0 0 0 0 0\n";
  CHECK(parse_pid_stat(spaces, st));
  CHECK(st.comm == "tmux: server");
  CHECK(st.nice == 19);

  CHECK(!parse_pid_stat("", st));
  CHECK(!parse_pid_stat("12 (short) S 1 2", st));
  CHECK(!parse_pid_stat("no parentheses here", st));

  // Kernel thread with an empty argv and a slash in its name.
  CHECK(parse_pid_stat(
      "9 (kworker/0:1-events) I 2 0 0 0 -1 69238880 0 0 0 0 0 5 0 0 20 0 1 0 12 0 0 "
      "18446744073709551615 0 0 0 0 0 0 0 2147483647 0 0 0 0 17 0 0 0 0 0 0\n", st));
  CHECK(st.comm == "kworker/0:1-events");
  CHECK(st.state == 'I');
  CHECK(std::string(state_name('I')) == "Idle");
  CHECK(std::string(state_name('Z')) == "Zombie");
}

static void test_pid_misc() {
  PidIo io;
  CHECK(parse_pid_io("rchar: 100\nwchar: 200\nsyscr: 1\nsyscw: 2\nread_bytes: 4096\n"
                     "write_bytes: 8192\ncancelled_write_bytes: 0\n", io));
  CHECK(io.read_bytes == 4096 && io.write_bytes == 8192);
  CHECK(!parse_pid_io("rchar: 1\n", io));

  CHECK(parse_status_uid("Name:\tbash\nUmask:\t0022\nUid:\t1000\t1000\t1000\t1000\nGid:\t1000\n") == 1000);
  CHECK(parse_status_uid("Name:\tx\n") == -1);

  std::string cmd("/usr/bin/python3\0-m\0http.server\0", 32);
  auto v = split_nul(cmd);
  CHECK(v.size() == 3);
  if (v.size() == 3) CHECK(v[0] == "/usr/bin/python3" && v[1] == "-m" && v[2] == "http.server");
  CHECK(split_nul("").empty());
}

static void test_diskstats() {
  const std::string text =
      " 259       0 nvme0n1 120000 3000 9600000 45000 80000 6000 6400000 70000 0 50000 115000 0 0 0 0\n"
      " 259       1 nvme0n1p1 100 0 2048 10 0 0 0 0 0 20 10 0 0 0 0\n"
      "   7       0 loop0 50 0 400 2 0 0 0 0 0 4 2 0 0 0 0\n"
      "   8       0 sda 10 0 80 1 2 0 16 1 0 3 2\n"
      "short line\n";
  auto d = parse_diskstats(text);
  CHECK(d.size() == 4);
  if (d.size() == 4) {
    CHECK(d[0].name == "nvme0n1" && d[0].major == 259 && d[0].minor == 0);
    CHECK(d[0].reads == 120000 && d[0].sectors_read == 9600000);
    CHECK(d[0].writes == 80000 && d[0].sectors_written == 6400000);
    CHECK(d[0].io_ticks_ms == 50000);
    CHECK(d[3].name == "sda" && d[3].sectors_written == 16 && d[3].io_ticks_ms == 3);
  }
  CHECK(interesting_disk("nvme0n1"));
  CHECK(interesting_disk("sda"));
  CHECK(interesting_disk("mmcblk0"));
  CHECK(!interesting_disk("loop3"));
  CHECK(!interesting_disk("zram0"));
  CHECK(!interesting_disk("dm-0"));
}

static void test_net_dev() {
  const std::string text =
      "Inter-|   Receive                                                |  Transmit\n"
      " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
      "    lo:  123456     789    0    0    0     0          0         0   123456     789    0    0    0     0       0          0\n"
      "  eth0: 987654321 654321   0    0    0     0          0      1234 123456789 234567    0    0    0     0       0          0\n"
      "wlan0:1000 10 0 0 0 0 0 0 2000 20 0 0 0 0 0 0\n";
  auto n = parse_net_dev(text);
  CHECK(n.size() == 3);
  if (n.size() == 3) {
    CHECK(n[0].name == "lo");
    CHECK(n[1].name == "eth0" && n[1].rx_bytes == 987654321ULL && n[1].rx_packets == 654321);
    CHECK(n[1].tx_bytes == 123456789ULL && n[1].tx_packets == 234567);
    CHECK(n[2].name == "wlan0" && n[2].rx_bytes == 1000 && n[2].tx_bytes == 2000);
  }
  CHECK(!interesting_interface("lo"));
  CHECK(!interesting_interface("veth1a2b"));
  CHECK(interesting_interface("enp3s0"));
  CHECK(interesting_interface("wlp2s0"));
}

static void test_misc_files() {
  LoadAvg l;
  CHECK(parse_loadavg("0.52 0.58 0.59 2/1234 56789\n", l));
  CHECK(near(l.one, 0.52) && near(l.five, 0.58) && near(l.fifteen, 0.59));
  CHECK(l.running == 2 && l.threads == 1234);
  CHECK(!parse_loadavg("1 2", l));

  CHECK(near(parse_uptime("12345.67 45678.90\n"), 12345.67));
  CHECK(parse_uptime("x") < 0);

  const std::string cpuinfo =
      "processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD Ryzen 7  5800X 8-Core Processor\n"
      "cpu MHz\t\t: 3000.000\n\nprocessor\t: 1\nmodel name\t: AMD Ryzen 7  5800X 8-Core Processor\n"
      "cpu MHz\t\t: 4000.000\n";
  CHECK(parse_cpu_model(cpuinfo) == "AMD Ryzen 7 5800X 8-Core Processor");
  CHECK(near(parse_cpuinfo_mhz(cpuinfo), 3500.0));
  CHECK(parse_cpu_model("processor : 0\nBogoMIPS : 48\nModel : Raspberry Pi 4 Model B\n") ==
        "Raspberry Pi 4 Model B");
  CHECK(near(parse_cpuinfo_mhz("processor : 0\n"), 0));
}

static void test_mounts() {
  const std::string text =
      "sysfs /sys sysfs rw,nosuid 0 0\n"
      "/dev/nvme0n1p2 / ext4 rw,relatime 0 0\n"
      "/dev/nvme0n1p1 /boot/efi vfat rw 0 0\n"
      "tmpfs /run tmpfs rw 0 0\n"
      "/dev/sdb1 /media/user/My\\040Disk exfat rw 0 0\n"
      "/dev/loop0 /snap/core/1 squashfs ro 0 0\n";
  auto m = parse_mounts(text);
  CHECK(m.size() == 6);
  if (m.size() == 6) {
    CHECK(m[1].device == "/dev/nvme0n1p2" && m[1].mount_point == "/" && m[1].type == "ext4");
    CHECK(m[4].mount_point == "/media/user/My Disk");
    CHECK(!interesting_mount(m[0]));
    CHECK(interesting_mount(m[1]));
    CHECK(interesting_mount(m[2]));
    CHECK(!interesting_mount(m[3]));
    CHECK(interesting_mount(m[4]));
    CHECK(!interesting_mount(m[5]));
  }
  CHECK(unescape_mount("a\\011b\\134c") == "a\tb\\c");
  CHECK(unescape_mount("trailing\\04") == "trailing\\04");
}

static void test_autostart() {
  const std::string on =
      "[Desktop Entry]\nType=Application\nName=Foo\nExec=foo\n\n[Desktop Action new]\nName=New\n";
  CHECK(autostart_enabled(on));
  std::string off = set_autostart_hidden(on, true);
  CHECK(!autostart_enabled(off));
  CHECK(off.find("Hidden=true") != std::string::npos);
  // The key goes into [Desktop Entry], not the action group.
  CHECK(off.find("Hidden=true") < off.find("[Desktop Action new]"));
  std::string back = set_autostart_hidden(off, false);
  CHECK(autostart_enabled(back));
  CHECK(back.find("Hidden=true") == std::string::npos);
  // Exactly one Hidden key after repeated toggles.
  std::string twice = set_autostart_hidden(set_autostart_hidden(off, true), true);
  CHECK(twice.find("Hidden=") == twice.rfind("Hidden="));

  const std::string gnome = "[Desktop Entry]\nName=Bar\nX-GNOME-Autostart-enabled=false\n";
  CHECK(!autostart_enabled(gnome));
  CHECK(autostart_enabled(set_autostart_hidden(gnome, false)));
  // A Hidden key in another group does not count.
  CHECK(autostart_enabled("[Desktop Entry]\nName=A\n[Other]\nHidden=true\n"));
}

static void test_format() {
  CHECK(format_bytes(0) == "0 B");
  CHECK(format_bytes(512) == "512 B");
  CHECK(format_bytes(1536) == "1.50 KiB");
  CHECK(format_bytes(1024ULL * 1024 * 300) == "300 MiB");
  CHECK(format_bytes(uint64_t(15.5 * 1024 * 1024 * 1024)) == "15.5 GiB");
  CHECK(format_rate(0) == "0 B/s");
  CHECK(format_rate(2048) == "2.00 KiB/s");
  CHECK(format_bits_rate(125000) == "1.0 Mbit/s");
  CHECK(format_duration(59) == "00:00:59");
  CHECK(format_duration(3 * 86400 + 4 * 3600 + 12 * 60 + 9) == "3 d 04:12:09");
  CHECK(format_percent(12.34) == "12.3 %");
  CHECK(format_percent(100) == "100 %");
  CHECK(near(nice_ceiling(0.5, 1024), 1024));
  CHECK(near(nice_ceiling(3000, 1), 5000));
  CHECK(near(nice_ceiling(1000, 1), 1000));
  CHECK(near(nice_ceiling(1001, 1), 2000));
}

int main() {
  test_proc_stat();
  test_process_cpu();
  test_meminfo();
  test_pid_stat();
  test_pid_misc();
  test_diskstats();
  test_net_dev();
  test_misc_files();
  test_mounts();
  test_autostart();
  test_format();
  if (g_failures) {
    fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  printf("sysmon: all %d checks passed\n", g_checks);
  return 0;
}
