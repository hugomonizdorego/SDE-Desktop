// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE System Monitor: a background thread that samples /proc and /sys once a
// second and publishes immutable snapshots. The UI thread is woken with
// Fl::awake() and only ever reads the latest snapshot.
#pragma once

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "procfs.h"

namespace sysmon {

struct ProcInfo {
  int pid = 0, ppid = 0, uid = -1;
  std::string user, comm, name;      // name: best short program name
  std::string cmdline;               // arguments joined with spaces
  std::string exe_base;              // basename of argv[0]
  std::string desktop_hint;          // GIO_LAUNCHED_DESKTOP_FILE of this process
  char state = '?';
  double cpu = 0;                    // % of the whole machine
  uint64_t memory = 0;               // resident bytes
  double read_rate = 0, write_rate = 0;  // bytes/s, -1 when not readable
  int threads = 0;
  long nice = 0;
  uint64_t start_ticks = 0, cpu_ticks = 0;
};

struct DiskSample {
  std::string name, model;
  uint64_t size = 0;      // bytes
  bool rotational = false, removable = false;
  double read_rate = 0, write_rate = 0, active = 0;  // bytes/s, %
};

struct NetSample {
  std::string name, address;
  bool wireless = false, up = false;
  double rx_rate = 0, tx_rate = 0;  // bytes/s
  uint64_t rx_total = 0, tx_total = 0;
};

struct GpuSample {
  std::string name, driver;
  double busy = -1;  // %
  uint64_t vram_used = 0, vram_total = 0;
  double temperature = -1;
};

struct FsInfo {
  std::string device, mount_point, type;
  uint64_t total = 0, used = 0, available = 0;
  bool read_only = false;
};

struct Snapshot {
  uint64_t serial = 0;
  double elapsed = 1;  // seconds since the previous sample

  // CPU
  std::string cpu_model;
  int cpu_count = 1;
  double cpu_total = 0;
  std::vector<double> cpu_cores;
  double cpu_mhz = 0, cpu_max_mhz = 0;
  double cpu_temperature = -1;
  double uptime = 0;
  LoadAvg load;
  uint64_t boot_time = 0;

  MemInfo mem;
  std::vector<DiskSample> disks;
  std::vector<NetSample> nets;
  std::vector<GpuSample> gpus;

  int battery_percent = -1;
  std::string battery_status;

  bool has_procs = false;
  std::vector<ProcInfo> procs;
  bool has_fs = false;
  std::vector<FsInfo> filesystems;
};

class Sampler {
 public:
  Sampler();
  ~Sampler();

  // |notify| runs on the sampler thread after each new snapshot; it should
  // only wake the UI (Fl::awake).
  void start(std::function<void()> notify);
  void stop();

  // What the visible page needs. Process and I/O scans cost the most and
  // only run while the Processes page is on screen.
  void want_processes(bool on, bool io);
  void want_filesystems(bool on);
  // Sample right away (e.g. after switching pages or ending a task).
  void poke();

  std::shared_ptr<const Snapshot> latest() const;

 private:
  struct PidCache {
    uint64_t start_ticks = 0, cpu_ticks = 0;
    uint64_t io_read = 0, io_write = 0;
    bool io_ok = false, io_denied = false, have_io = false;
    int uid = -1;
    std::string user, cmdline, exe_base, name, desktop_hint;
  };

  void run();
  void sample(Snapshot &s, double now);
  void sample_cpu(Snapshot &s);
  void sample_disks(Snapshot &s, double dt);
  void sample_net(Snapshot &s, double dt);
  void sample_gpu(Snapshot &s);
  void sample_sensors(Snapshot &s);
  void sample_procs(Snapshot &s, double dt);
  void sample_fs(Snapshot &s);
  std::string user_name(int uid);

  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool quit_ = false, poked_ = false;
  std::function<void()> notify_;
  std::shared_ptr<const Snapshot> latest_;
  std::atomic<bool> want_procs_{true}, want_io_{true}, want_fs_{false}, fs_now_{false};

  // Sampler thread state.
  uint64_t serial_ = 0;
  double last_time_ = 0, last_proc_time_ = 0, last_fs_time_ = -100;
  CpuStat prev_cpu_;
  bool have_prev_cpu_ = false;
  std::map<std::string, DiskStat> prev_disks_;
  std::map<std::string, NetDev> prev_nets_;
  std::map<int, PidCache> pids_;
  std::map<int, std::string> users_;
  std::string cpu_model_;
  double cpu_max_mhz_ = 0;
  std::vector<FsInfo> last_fs_;
  std::map<std::string, std::string> addresses_;
  double last_address_time_ = -100;
  long clock_ticks_ = 100;
  long page_size_ = 4096;
  int my_uid_ = 0;
};

// Reads a whole (small) file; /proc files report size 0, so this reads in a
// loop. Returns false when the file cannot be opened.
bool read_text(const std::string &path, std::string &out);

}  // namespace sysmon
