// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Unit tests for SDE Welcome, the Driver Manager and System Reports without
// a display: kernel log -> firmware package mapping, GPU heuristics,
// nvidia-detect / isenkram / dpkg parsers, APT sources rewriting, crash log
// parsing and grouping, the GitHub issue URL and the Welcome model.
// Usage: sde-sysreport-tests <tests/fixtures/sysreport>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "config.h"
#include "drivers.h"
#include "gpu.h"
#include "model.h"
#include "reports.h"
#include "util.h"

using namespace sde;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                        \
  do {                                                                                        \
    auto va = (a);                                                                            \
    auto vb = (b);                                                                            \
    if (!(va == vb)) {                                                                        \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, \
              #a, #b, std::string(va).c_str());                                               \
      ++g_failures;                                                                           \
    }                                                                                         \
  } while (0)

static std::string g_dir;

static std::string fixture(const std::string &name) {
  std::string s;
  if (!read_file(join_path(g_dir, name), s)) {
    fprintf(stderr, "missing fixture %s\n", name.c_str());
    ++g_failures;
  }
  return s;
}

static void test_firmware_log() {
  auto files = drivers::parse_firmware_failures(fixture("kernel.log"));
  std::vector<std::string> want = {
      "iwlwifi-so-a0-gf-a0-89.ucode",
      "intel/ibt-0040-0041.sfi",
      "rtl_nic/rtl8168h-2.fw",
      "i915/adlp_dmc.bin",
      "amdgpu/navi23_sos.bin",
      "brcm/brcmfmac43455-sdio.raspberrypi,4-model-b.bin",
      "ath10k/QCA6174/hw3.0/firmware-6.bin",
      "mediatek/WIFI_RAM_CODE_MT7961_1.bin",
      "regulatory.db",
      "vendor/unknown-chip.bin",
  };
  CHECK_EQ(std::to_string(files.size()), std::to_string(want.size()));
  for (size_t i = 0; i < want.size() && i < files.size(); ++i) CHECK_EQ(files[i], want[i]);

  CHECK_EQ(drivers::firmware_package("iwlwifi-so-a0-gf-a0-89.ucode"), "firmware-iwlwifi");
  CHECK_EQ(drivers::firmware_package("intel/ibt-0040-0041.sfi"), "firmware-iwlwifi");
  CHECK_EQ(drivers::firmware_package("rtl_nic/rtl8168h-2.fw"), "firmware-realtek");
  CHECK_EQ(drivers::firmware_package("rtw89/rtw8852b_fw.bin"), "firmware-realtek");
  CHECK_EQ(drivers::firmware_package("rtl_bt/rtl8761bu_fw.bin"), "firmware-realtek");
  CHECK_EQ(drivers::firmware_package("i915/adlp_dmc.bin"), "firmware-intel-graphics");
  CHECK_EQ(drivers::firmware_package("amdgpu/navi23_sos.bin"), "firmware-amd-graphics");
  CHECK_EQ(drivers::firmware_package("radeon/R600_rlc.bin"), "firmware-amd-graphics");
  CHECK_EQ(drivers::firmware_package("brcm/brcmfmac43455-sdio.bin"), "firmware-brcm80211");
  CHECK_EQ(drivers::firmware_package("ath10k/QCA6174/hw3.0/firmware-6.bin"), "firmware-atheros");
  CHECK_EQ(drivers::firmware_package("qca/rampatch_usb_00000302.bin"), "firmware-atheros");
  CHECK_EQ(drivers::firmware_package("mediatek/WIFI_RAM_CODE_MT7961_1.bin"), "firmware-mediatek");
  CHECK_EQ(drivers::firmware_package("nvidia/ga104/gsp/gsp-535.113.01.bin"), "firmware-nvidia-graphics");
  CHECK_EQ(drivers::firmware_package("intel/sof/sof-tgl.ri"), "firmware-sof-signed");
  CHECK_EQ(drivers::firmware_package("regulatory.db"), "wireless-regdb");
  CHECK_EQ(drivers::firmware_package("/lib/firmware/iwlwifi-7265D-29.ucode"), "firmware-iwlwifi");
  CHECK_EQ(drivers::firmware_package("vendor/unknown-chip.bin"), "firmware-misc-nonfree");
  CHECK(!drivers::firmware_device("rtl_bt/x.bin").empty());

  // Quoting and punctuation around the file name.
  auto q = drivers::parse_firmware_failures(
      "x: failed to load firmware \"rtw88/rtw8822c_fw.bin\", err -2\n");
  CHECK_EQ(std::to_string(q.size()), "1");
  if (!q.empty()) CHECK_EQ(q[0], "rtw88/rtw8822c_fw.bin");
  CHECK(drivers::parse_firmware_failures("").empty());
}

static void test_gpus() {
  Gpu nv;
  nv.vendor_id = "10de";
  nv.device_id = "2484";
  nv.vendor = "NVIDIA Corporation";
  nv.model = "GA104 [GeForce RTX 3070]";
  nv.driver = "nouveau";
  Gpu amd;
  amd.vendor_id = "1002";
  amd.vendor = "Advanced Micro Devices, Inc. [AMD/ATI]";
  amd.model = "Navi 23";
  amd.driver = "amdgpu";
  Gpu intel;
  intel.vendor_id = "8086";
  intel.vendor = "Intel Corporation";
  intel.model = "Alder Lake-P GT2";
  Gpu vm;
  vm.vendor_id = "1af4";  // virtio-gpu
  vm.vendor = "Red Hat, Inc.";

  auto recs = drivers::gpu_recommendations({nv, amd, intel, vm});
  CHECK_EQ(std::to_string(recs.size()), "4");
  if (recs.size() == 4) {
    CHECK_EQ(recs[0].package, "nvidia-driver");
    CHECK_EQ(recs[0].component, "non-free");
    CHECK_EQ(recs[1].package, "firmware-nvidia-graphics");
    CHECK_EQ(recs[2].package, "firmware-amd-graphics");
    CHECK_EQ(recs[3].package, "firmware-intel-graphics");
    CHECK(recs[2].device.find("AMD") != std::string::npos);
  }
  // With the proprietary driver bound no nouveau firmware is suggested.
  nv.driver = "nvidia";
  auto only = drivers::gpu_recommendations({nv});
  CHECK_EQ(std::to_string(only.size()), "1");

  std::string detect = fixture("nvidia-detect.txt");
  CHECK_EQ(drivers::parse_nvidia_detect(detect), "nvidia-tesla-470-driver");
  auto legacy = drivers::gpu_recommendations({nv}, detect);
  CHECK(!legacy.empty() && legacy[0].package == "nvidia-tesla-470-driver");
  CHECK_EQ(drivers::parse_nvidia_detect("No NVIDIA GPU detected."), "");
  CHECK(drivers::gpu_recommendations({vm}).empty());
}

static void test_misc_parsers() {
  CHECK_EQ(drivers::microcode_package("vendor_id\t: GenuineIntel\n"), "intel-microcode");
  CHECK_EQ(drivers::microcode_package("vendor_id\t: AuthenticAMD\n"), "amd64-microcode");
  CHECK_EQ(drivers::microcode_package("vendor_id\t: GenuineIntel\n", true), "");
  CHECK_EQ(drivers::microcode_package("Hardware\t: BCM2835\n"), "");

  auto isen = drivers::parse_isenkram("firmware-iwlwifi\n\nfirmware-realtek\n# comment\nbad line here\n");
  CHECK_EQ(std::to_string(isen.size()), "2");

  auto inst = drivers::parse_dpkg_installed(
      "firmware-iwlwifi install ok installed\nfirmware-realtek unknown ok not-installed\n"
      "intel-microcode:amd64 install ok installed\n");
  CHECK_EQ(std::to_string(inst.size()), "2");
  if (inst.size() == 2) CHECK_EQ(inst[1], "intel-microcode");

  drivers::Recommendation a{"firmware-realtek", "Realtek Wi-Fi", "A.", "non-free-firmware"};
  drivers::Recommendation b{"firmware-realtek", "Realtek Bluetooth", "B.", "non-free-firmware"};
  auto merged = drivers::merge({a, b, a});
  CHECK_EQ(std::to_string(merged.size()), "1");
  if (!merged.empty()) CHECK_EQ(merged[0].device, "Realtek Wi-Fi, Realtek Bluetooth");
}

static void test_sources() {
  std::string one = fixture("sources.list");
  CHECK(!drivers::sources_have_component(one, false, "non-free-firmware"));
  CHECK(drivers::sources_have_component(one, false, "main"));
  std::string fixed = drivers::enable_components(one, false, {"non-free-firmware"});
  CHECK(drivers::sources_have_component(fixed, false, "non-free-firmware"));
  // Debian lines gained the component, the third-party line and comments did not change.
  CHECK(fixed.find("deb http://deb.debian.org/debian testing main contrib non-free-firmware\n") !=
        std::string::npos);
  CHECK(fixed.find("deb [signed-by=/usr/share/keyrings/x.gpg] https://example.org/repo stable main\n") !=
        std::string::npos);
  CHECK(fixed.find("# deb cdrom:[Debian GNU/Linux testing]/ testing main\n") != std::string::npos);
  CHECK(fixed.find("deb http://security.debian.org/debian-security testing-security main "
                   "non-free-firmware # security\n") != std::string::npos);
  // Idempotent.
  CHECK_EQ(drivers::enable_components(fixed, false, {"non-free-firmware"}), fixed);

  std::string d822 = fixture("debian.sources");
  CHECK(!drivers::sources_have_component(d822, true, "non-free-firmware"));
  std::string fixed822 = drivers::enable_components(d822, true, {"non-free-firmware", "contrib", "non-free"});
  CHECK(drivers::sources_have_component(fixed822, true, "non-free-firmware"));
  CHECK(drivers::sources_have_component(fixed822, true, "non-free"));
  CHECK(fixed822.find("Components: main non-free-firmware contrib non-free\n") != std::string::npos);
  // The third-party stanza keeps its components.
  CHECK(fixed822.find("Components: stable-only\n") != std::string::npos);
  CHECK_EQ(drivers::enable_components(fixed822, true, {"non-free-firmware"}), fixed822);
  // A disabled stanza does not count as enabled.
  CHECK(!drivers::sources_have_component("Types: deb\nURIs: http://deb.debian.org/debian\nSuites: sid\n"
                                         "Components: main non-free-firmware\nEnabled: no\n",
                                         true, "non-free-firmware"));
}

static void test_reports() {
  auto list = reports::parse_crash_log(fixture("session.log"), "session.log");
  // Two panel backtraces (their "exited" lines are folded in), the desktop
  // abort; "status 1" is not a crash.
  CHECK_EQ(std::to_string(list.size()), "3");
  if (list.size() == 3) {
    CHECK_EQ(list[0].program, "sde-panel");
    CHECK_EQ(list[0].signal, "Segmentation fault");
    CHECK_EQ(list[0].time, "09:20:11");
    CHECK_EQ(list[1].time, "09:31:45");
    CHECK(list[0].has_backtrace);
    CHECK(list[0].backtrace.find("_ZN3sde5Panel4drawEv") != std::string::npos);
    CHECK(list[0].backtrace.find("end of backtrace") == std::string::npos);
    CHECK_EQ(list[2].program, "desktop");
    CHECK_EQ(list[2].signal, "Aborted");
    CHECK_EQ(list[2].time, "09:40:00");
    CHECK(!list[2].has_backtrace);
    // Same crash at different addresses: one group of two.
    CHECK_EQ(list[0].signature(), list[1].signature());
  }
  auto groups = reports::group(list);
  CHECK_EQ(std::to_string(groups.size()), "2");
  if (!groups.empty()) CHECK_EQ(std::to_string(groups[0].count), "2");

  CHECK_EQ(reports::url_encode("a b&c=d/é"), "a%20b%26c%3Dd%2F%C3%A9");
  reports::CrashReport r = list.empty() ? reports::CrashReport() : list[0];
  std::string title = reports::issue_title(r, "0.1.0~alpha50");
  CHECK_EQ(title, "[crash] sde-panel: Segmentation fault (0.1.0~alpha50)");
  std::string body = reports::report_text(r, "SDE: test\n");
  CHECK(body.find("```\nsde-panel(+0x8a1f2)") != std::string::npos);
  std::string url = reports::issue_url(title, body);
  CHECK(starts_with(url, "https://github.com/hugomonizdorego/SDE-Desktop/issues/new?title=%5Bcrash%5D%20sde-panel"));
  CHECK(url.find("&body=") != std::string::npos);
  std::string big(20000, 'x');
  for (int i = 0; i < 400; ++i) big[size_t(i) * 50] = '\n';
  CHECK(reports::issue_url(title, big, 4000).size() <= 4000);

  CHECK_EQ(reports::cpu_model("processor\t: 0\nmodel name\t: Intel(R) Core(TM)  i5-1135G7 @ 2.40GHz\n"),
           "Intel(R) Core(TM) i5-1135G7 @ 2.40GHz");
  CHECK_EQ(reports::memory_total("MemTotal:       16253252 kB\nMemFree: 1 kB\n"), "15.5 GiB");

  // The directory scan picks up session.log and other *.log files.
  setenv("SDE_REPORTS_DIR", g_dir.c_str(), 1);
  auto files = reports::report_files();
  CHECK(!files.empty() && ends_with(files[0], "/session.log"));
  CHECK_EQ(std::to_string(reports::collect().size()), "3");
}

static void test_welcome() {
  CHECK(welcome::ufw_conf_enabled("# comment\nENABLED=yes\nLOGLEVEL=low\n"));
  CHECK(!welcome::ufw_conf_enabled("ENABLED=no\n"));
  CHECK(welcome::ufw_conf_enabled("ENABLED=\"yes\"\n"));
  CHECK(!welcome::ufw_conf_enabled(""));
  CHECK_EQ(welcome::parse_ufw_status("Status: active\n\nTo Action From\n"), "active");
  CHECK_EQ(welcome::parse_ufw_status("Status: inactive\n"), "inactive");
  CHECK_EQ(welcome::parse_ufw_status("ERROR: You need to be root to run this script"), "");
  auto missing = welcome::missing_packages(welcome::codec_packages(), {"gstreamer1.0-plugins-good"});
  CHECK_EQ(std::to_string(missing.size()), "3");
  CHECK_EQ(std::to_string(welcome::panel_layouts().size()), "3");

  // [welcome] show defaults to true and round-trips through sde.conf.
  char tmpl[] = "/tmp/sde-sysreport-XXXXXX";
  std::string home = mkdtemp(tmpl) ? tmpl : "/tmp";
  setenv("XDG_CONFIG_HOME", home.c_str(), 1);
  Config cfg;
  cfg.load();
  CHECK(welcome::show_at_startup(cfg));
  welcome::set_show_at_startup(cfg, false);
  CHECK(cfg.save());
  Config again;
  again.load();
  CHECK(!welcome::show_at_startup(again));
  unlink((home + "/sde/sde.conf").c_str());
  rmdir((home + "/sde").c_str());
  rmdir(home.c_str());
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: sde-sysreport-tests FIXTURE_DIR\n");
    return 2;
  }
  g_dir = argv[1];
  test_firmware_log();
  test_gpus();
  test_misc_parsers();
  test_sources();
  test_reports();
  test_welcome();
  if (g_failures) fprintf(stderr, "%d check(s) failed\n", g_failures);
  else printf("sysreport: all checks passed\n");
  return g_failures ? 1 : 0;
}
