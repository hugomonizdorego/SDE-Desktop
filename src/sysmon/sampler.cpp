// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "sampler.h"

#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>

namespace sysmon {

bool read_text(const std::string &path, std::string &out) {
  out.clear();
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    if (n == 0) break;
    out.append(buf, size_t(n));
    if (out.size() > (1u << 20)) break;  // never slurp huge files
  }
  ::close(fd);
  return true;
}

namespace {

double monotonic_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string read_line(const std::string &path) {
  std::string s;
  if (!read_text(path, s)) return "";
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) s.pop_back();
  size_t a = s.find_first_not_of(' ');
  return a == std::string::npos ? "" : s.substr(a);
}

double read_number(const std::string &path, double fallback = -1) {
  std::string s = read_line(path);
  if (s.empty()) return fallback;
  char *end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  return end == s.c_str() ? fallback : v;
}

std::vector<std::string> list_dir(const std::string &path) {
  std::vector<std::string> out;
  if (DIR *d = opendir(path.c_str())) {
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.') out.push_back(e->d_name);
    closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

std::string base_name(const std::string &path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Temperature sensors are found once; their sysfs paths do not change.
struct SensorPaths {
  bool scanned = false;
  std::string cpu_temp;
  std::string battery;
};
SensorPaths g_sensors;

struct GpuPath {
  std::string dir, name, driver, temp;
};
std::vector<GpuPath> g_gpus;
bool g_gpus_scanned = false;

}  // namespace

Sampler::Sampler() {
  clock_ticks_ = sysconf(_SC_CLK_TCK);
  if (clock_ticks_ <= 0) clock_ticks_ = 100;
  page_size_ = sysconf(_SC_PAGESIZE);
  if (page_size_ <= 0) page_size_ = 4096;
  my_uid_ = int(getuid());
  std::string info;
  if (read_text("/proc/cpuinfo", info)) cpu_model_ = parse_cpu_model(info);
  cpu_max_mhz_ = read_number("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", 0) / 1000.0;
}

Sampler::~Sampler() { stop(); }

void Sampler::start(std::function<void()> notify) {
  notify_ = std::move(notify);
  quit_ = false;
  thread_ = std::thread([this] { run(); });
}

void Sampler::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Sampler::want_processes(bool on, bool io) {
  bool was = want_procs_.exchange(on);
  want_io_ = io;
  if (on && !was) poke();
}

void Sampler::want_filesystems(bool on) {
  bool was = want_fs_.exchange(on);
  if (on && !was) {
    fs_now_ = true;
    poke();
  }
}

void Sampler::poke() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    poked_ = true;
  }
  wake_.notify_all();
}

std::shared_ptr<const Snapshot> Sampler::latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

void Sampler::run() {
  using clock = std::chrono::steady_clock;
  auto next = clock::now();
  for (;;) {
    auto snap = std::make_shared<Snapshot>();
    sample(*snap, monotonic_seconds());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      latest_ = snap;
    }
    if (notify_) notify_();
    next += std::chrono::seconds(1);
    auto now = clock::now();
    if (next < now) next = now + std::chrono::milliseconds(200);
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_until(lock, next, [this] { return quit_ || poked_; });
    if (quit_) return;
    if (poked_) {
      poked_ = false;
      // Wait a moment so that a just-killed process is really gone.
      wake_.wait_for(lock, std::chrono::milliseconds(150), [this] { return quit_; });
      if (quit_) return;
      next = clock::now();
    }
  }
}

void Sampler::sample(Snapshot &s, double now) {
  double dt = last_time_ > 0 ? now - last_time_ : 1.0;
  if (dt <= 0.01) dt = 0.01;
  last_time_ = now;
  s.serial = ++serial_;
  s.elapsed = dt;
  sample_cpu(s);

  std::string text;
  if (read_text("/proc/meminfo", text)) parse_meminfo(text, s.mem);
  if (read_text("/proc/loadavg", text)) parse_loadavg(text, s.load);
  if (read_text("/proc/uptime", text)) s.uptime = parse_uptime(text);

  sample_disks(s, dt);
  sample_net(s, dt);
  sample_gpu(s);
  sample_sensors(s);
  if (want_procs_) {
    double pdt = last_proc_time_ > 0 ? now - last_proc_time_ : 0;
    last_proc_time_ = now;
    sample_procs(s, pdt);
  } else {
    // Forget CPU baselines so that the first scan after a pause does not
    // average over the whole pause.
    last_proc_time_ = 0;
  }
  if (want_fs_ && (fs_now_.exchange(false) || now - last_fs_time_ >= 5)) {
    last_fs_time_ = now;
    sample_fs(s);
  } else {
    s.has_fs = !last_fs_.empty();
    s.filesystems = last_fs_;
  }
}

void Sampler::sample_cpu(Snapshot &s) {
  std::string text;
  CpuStat cur;
  if (!read_text("/proc/stat", text) || !parse_proc_stat(text, cur)) return;
  s.cpu_model = cpu_model_;
  s.boot_time = cur.boot_time;
  s.cpu_count = std::max<int>(1, int(cur.cores.size()));
  if (have_prev_cpu_) {
    s.cpu_total = cpu_percent(prev_cpu_.total, cur.total);
    s.cpu_cores.resize(cur.cores.size());
    for (size_t i = 0; i < cur.cores.size(); ++i)
      s.cpu_cores[i] = i < prev_cpu_.cores.size() ? cpu_percent(prev_cpu_.cores[i], cur.cores[i]) : 0;
  } else {
    s.cpu_cores.assign(cur.cores.size(), 0);
  }
  prev_cpu_ = cur;
  have_prev_cpu_ = true;

  double sum = 0;
  int n = 0;
  for (size_t i = 0; i < cur.cores.size(); ++i) {
    double khz = read_number("/sys/devices/system/cpu/cpu" + std::to_string(i) +
                             "/cpufreq/scaling_cur_freq");
    if (khz > 0) sum += khz / 1000.0, ++n;
  }
  if (n) s.cpu_mhz = sum / n;
  else if (read_text("/proc/cpuinfo", text)) s.cpu_mhz = parse_cpuinfo_mhz(text);
  s.cpu_max_mhz = cpu_max_mhz_;
}

void Sampler::sample_disks(Snapshot &s, double dt) {
  std::string text;
  if (!read_text("/proc/diskstats", text)) return;
  std::map<std::string, DiskStat> now;
  for (auto &d : parse_diskstats(text)) {
    if (!interesting_disk(d.name)) continue;
    std::string sys = "/sys/block/" + d.name;
    if (!exists(sys)) continue;  // a partition
    double sectors = read_number(sys + "/size", 0);
    if (sectors <= 0) continue;
    DiskSample out;
    out.name = d.name;
    out.size = uint64_t(sectors) * 512;
    out.model = read_line(sys + "/device/model");
    if (out.model.empty()) out.model = read_line(sys + "/device/name");
    out.rotational = read_number(sys + "/queue/rotational", 0) > 0;
    out.removable = read_number(sys + "/removable", 0) > 0;
    auto prev = prev_disks_.find(d.name);
    if (prev != prev_disks_.end()) {
      const DiskStat &p = prev->second;
      if (d.sectors_read >= p.sectors_read)
        out.read_rate = double(d.sectors_read - p.sectors_read) * 512 / dt;
      if (d.sectors_written >= p.sectors_written)
        out.write_rate = double(d.sectors_written - p.sectors_written) * 512 / dt;
      if (d.io_ticks_ms >= p.io_ticks_ms)
        out.active = std::min(100.0, double(d.io_ticks_ms - p.io_ticks_ms) / (dt * 10.0));
    }
    now[d.name] = d;
    s.disks.push_back(out);
  }
  prev_disks_.swap(now);
}

void Sampler::sample_net(Snapshot &s, double dt) {
  std::string text;
  if (!read_text("/proc/net/dev", text)) return;
  double mono = last_time_;
  if (mono - last_address_time_ >= 10) {
    last_address_time_ = mono;
    addresses_.clear();
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
      for (ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || !a->ifa_name) continue;
        char buf[INET6_ADDRSTRLEN] = {0};
        if (a->ifa_addr->sa_family == AF_INET) {
          inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr, buf, sizeof buf);
          addresses_[a->ifa_name] = buf;  // IPv4 wins
        } else if (a->ifa_addr->sa_family == AF_INET6 && !addresses_.count(a->ifa_name)) {
          auto *in6 = reinterpret_cast<sockaddr_in6 *>(a->ifa_addr);
          if (IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr)) continue;
          inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof buf);
          addresses_[std::string(a->ifa_name) + "#6"] = buf;
        }
      }
      freeifaddrs(list);
    }
  }
  std::map<std::string, NetDev> now;
  for (auto &d : parse_net_dev(text)) {
    if (!interesting_interface(d.name)) continue;
    NetSample out;
    out.name = d.name;
    std::string sys = "/sys/class/net/" + d.name;
    out.up = read_line(sys + "/operstate") == "up";
    out.wireless = exists(sys + "/wireless") || exists(sys + "/phy80211");
    if (!out.up && d.rx_bytes == 0 && d.tx_bytes == 0) continue;
    auto a = addresses_.find(d.name);
    if (a == addresses_.end()) a = addresses_.find(d.name + "#6");
    if (a != addresses_.end()) out.address = a->second;
    out.rx_total = d.rx_bytes;
    out.tx_total = d.tx_bytes;
    auto prev = prev_nets_.find(d.name);
    if (prev != prev_nets_.end()) {
      if (d.rx_bytes >= prev->second.rx_bytes) out.rx_rate = double(d.rx_bytes - prev->second.rx_bytes) / dt;
      if (d.tx_bytes >= prev->second.tx_bytes) out.tx_rate = double(d.tx_bytes - prev->second.tx_bytes) / dt;
    }
    now[d.name] = d;
    s.nets.push_back(out);
  }
  prev_nets_.swap(now);
}

void Sampler::sample_gpu(Snapshot &s) {
  if (!g_gpus_scanned) {
    g_gpus_scanned = true;
    for (auto &card : list_dir("/sys/class/drm")) {
      if (card.compare(0, 4, "card") != 0 || card.find('-') != std::string::npos) continue;
      std::string dev = "/sys/class/drm/" + card + "/device";
      // Only drivers that expose a cheap utilization counter (amdgpu, and
      // newer Intel xe/i915 builds that implement the same attribute).
      if (!exists(dev + "/gpu_busy_percent")) continue;
      GpuPath g;
      g.dir = dev;
      char link[512];
      ssize_t n = readlink((dev + "/driver").c_str(), link, sizeof link - 1);
      if (n > 0) link[n] = 0, g.driver = base_name(link);
      std::string vendor = read_line(dev + "/vendor");
      std::string brand = vendor == "0x1002" ? "AMD Radeon" : vendor == "0x8086" ? "Intel Graphics"
                          : vendor == "0x10de" ? "NVIDIA" : "GPU";
      g.name = brand + " (" + card + ")";
      for (auto &h : list_dir(dev + "/hwmon"))
        if (exists(dev + "/hwmon/" + h + "/temp1_input")) g.temp = dev + "/hwmon/" + h + "/temp1_input";
      g_gpus.push_back(g);
    }
  }
  for (auto &g : g_gpus) {
    GpuSample out;
    out.name = g.name;
    out.driver = g.driver;
    out.busy = read_number(g.dir + "/gpu_busy_percent");
    out.vram_used = uint64_t(std::max(0.0, read_number(g.dir + "/mem_info_vram_used", 0)));
    out.vram_total = uint64_t(std::max(0.0, read_number(g.dir + "/mem_info_vram_total", 0)));
    if (!g.temp.empty()) {
      double t = read_number(g.temp);
      if (t > 0) out.temperature = t / 1000.0;
    }
    s.gpus.push_back(out);
  }
}

void Sampler::sample_sensors(Snapshot &s) {
  if (!g_sensors.scanned) {
    g_sensors.scanned = true;
    std::string fallback;
    for (auto &h : list_dir("/sys/class/hwmon")) {
      std::string dir = "/sys/class/hwmon/" + h;
      std::string name = read_line(dir + "/name");
      if (!exists(dir + "/temp1_input")) continue;
      if (name == "k10temp" || name == "coretemp" || name == "zenpower" || name == "cpu_thermal" ||
          name == "soc_thermal") {
        if (g_sensors.cpu_temp.empty()) g_sensors.cpu_temp = dir + "/temp1_input";
      } else if (name == "acpitz" && fallback.empty()) {
        fallback = dir + "/temp1_input";
      }
    }
    if (g_sensors.cpu_temp.empty()) g_sensors.cpu_temp = fallback;
    for (auto &p : list_dir("/sys/class/power_supply")) {
      std::string dir = "/sys/class/power_supply/" + p;
      if (read_line(dir + "/type") == "Battery" && read_line(dir + "/scope") != "Device" &&
          exists(dir + "/capacity")) {
        g_sensors.battery = dir;
        break;
      }
    }
  }
  if (!g_sensors.cpu_temp.empty()) {
    double t = read_number(g_sensors.cpu_temp);
    if (t > 0) s.cpu_temperature = t / 1000.0;
  }
  if (!g_sensors.battery.empty()) {
    double c = read_number(g_sensors.battery + "/capacity");
    if (c >= 0) s.battery_percent = int(c);
    s.battery_status = read_line(g_sensors.battery + "/status");
  }
}

std::string Sampler::user_name(int uid) {
  auto it = users_.find(uid);
  if (it != users_.end()) return it->second;
  passwd pw, *res = nullptr;
  char buf[2048];
  std::string name;
  if (getpwuid_r(uid_t(uid), &pw, buf, sizeof buf, &res) == 0 && res) name = res->pw_name;
  else name = std::to_string(uid);
  users_[uid] = name;
  return name;
}

void Sampler::sample_procs(Snapshot &s, double dt) {
  DIR *dir = opendir("/proc");
  if (!dir) return;
  bool io = want_io_;
  std::set<int> seen;
  std::string text, path;
  s.procs.reserve(pids_.size() + 16);
  while (dirent *e = readdir(dir)) {
    if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
    char *end = nullptr;
    long pid = std::strtol(e->d_name, &end, 10);
    if (*end) continue;
    std::string base = std::string("/proc/") + e->d_name;
    PidStat st;
    if (!read_text(base + "/stat", text) || !parse_pid_stat(text, st)) continue;
    seen.insert(int(pid));
    PidCache &c = pids_[int(pid)];
    bool fresh = c.start_ticks != st.start_time || c.uid < 0;
    uint64_t ticks = st.utime + st.stime;
    ProcInfo p;
    if (fresh) {
      // A new process (or a recycled pid): read what never changes once.
      c = PidCache();
      c.start_ticks = st.start_time;
      struct stat sb;
      c.uid = ::stat(base.c_str(), &sb) == 0 ? int(sb.st_uid) : 0;
      c.user = user_name(c.uid);
      std::string raw;
      read_text(base + "/cmdline", raw);
      auto args = split_nul(raw);
      for (size_t i = 0; i < args.size(); ++i) c.cmdline += (i ? " " : "") + args[i];
      c.exe_base = args.empty() ? "" : base_name(args[0]);
      // Interpreters and wrappers often rewrite argv[0] with a path or flags.
      size_t sp = c.exe_base.find(' ');
      if (sp != std::string::npos) c.exe_base = c.exe_base.substr(0, sp);
      // comm is cut at 15 characters; argv[0] has the full name.
      c.name = st.comm;
      if (st.comm.size() >= 15 && c.exe_base.compare(0, st.comm.size(), st.comm) == 0)
        c.name = c.exe_base;
      if (c.uid == my_uid_ && read_text(base + "/environ", raw)) {
        for (auto &kv : split_nul(raw)) {
          if (kv.compare(0, 26, "GIO_LAUNCHED_DESKTOP_FILE=") == 0) c.desktop_hint = kv.substr(26);
          else if (kv.compare(0, 30, "GIO_LAUNCHED_DESKTOP_FILE_PID=") == 0 &&
                   std::atol(kv.c_str() + 30) != pid)
            c.desktop_hint = "-";  // inherited from the real launchee
        }
        if (c.desktop_hint == "-") c.desktop_hint.clear();
      }
      c.cpu_ticks = ticks;
      p.cpu = 0;
    } else {
      p.cpu = process_cpu_percent(c.cpu_ticks, ticks, dt, clock_ticks_, s.cpu_count);
      c.cpu_ticks = ticks;
    }
    p.read_rate = p.write_rate = -1;
    if (io && !c.io_denied) {
      PidIo pio;
      if (read_text(base + "/io", text) && parse_pid_io(text, pio)) {
        if (c.have_io && dt > 0) {
          p.read_rate = pio.read_bytes >= c.io_read ? double(pio.read_bytes - c.io_read) / dt : 0;
          p.write_rate = pio.write_bytes >= c.io_write ? double(pio.write_bytes - c.io_write) / dt : 0;
        } else {
          p.read_rate = p.write_rate = 0;
        }
        c.io_read = pio.read_bytes;
        c.io_write = pio.write_bytes;
        c.have_io = true;
      } else {
        c.io_denied = true;
      }
    } else if (!io) {
      c.have_io = false;
    }
    p.pid = int(pid);
    p.ppid = st.ppid;
    p.uid = c.uid;
    p.user = c.user;
    p.comm = st.comm;
    p.name = c.name;
    p.cmdline = c.cmdline;
    p.exe_base = c.exe_base;
    p.desktop_hint = c.desktop_hint;
    p.state = st.state;
    p.memory = uint64_t(std::max<int64_t>(0, st.rss_pages)) * uint64_t(page_size_);
    p.threads = st.threads;
    p.nice = st.nice;
    p.start_ticks = st.start_time;
    p.cpu_ticks = ticks;
    s.procs.push_back(std::move(p));
  }
  closedir(dir);
  for (auto it = pids_.begin(); it != pids_.end();)
    it = seen.count(it->first) ? std::next(it) : pids_.erase(it);
  s.has_procs = true;
}

void Sampler::sample_fs(Snapshot &s) {
  std::string text;
  last_fs_.clear();
  if (read_text("/proc/self/mounts", text)) {
    std::set<std::string> devices;
    for (auto &m : parse_mounts(text)) {
      if (!interesting_mount(m)) continue;
      // Bind mounts and btrfs subvolumes repeat the device; show it once.
      if (!devices.insert(m.device).second) continue;
      struct statvfs v;
      if (statvfs(m.mount_point.c_str(), &v) != 0 || v.f_blocks == 0) continue;
      FsInfo f;
      f.device = m.device;
      f.mount_point = m.mount_point;
      f.type = m.type;
      uint64_t unit = v.f_frsize ? v.f_frsize : v.f_bsize;
      f.total = uint64_t(v.f_blocks) * unit;
      f.available = uint64_t(v.f_bavail) * unit;
      f.used = uint64_t(v.f_blocks - v.f_bfree) * unit;
      f.read_only = (v.f_flag & ST_RDONLY) != 0;
      last_fs_.push_back(f);
    }
  }
  s.has_fs = true;
  s.filesystems = last_fs_;
}

}  // namespace sysmon
