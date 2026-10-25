// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests for SDE Welcome, the Driver Manager and System Reports without
// a display: kernel log -> firmware package mapping, GPU heuristics,
// nvidia-detect / isenkram / dpkg parsers, APT sources rewriting, crash log
// parsing and grouping, the GitHub issue URL and the Welcome model.
// Usage: sde-sysreport-tests <tests/fixtures/sysreport>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "config.h"
#include "drivers.h"
#include "gpu.h"
#include "model.h"
#include "process.h"
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

  // [welcome] show defaults to false (alpha51: the first login opens SDE
  // Welcome through the stamp instead) and round-trips through sde.conf.
  char tmpl[] = "/tmp/sde-sysreport-XXXXXX";
  std::string home = mkdtemp(tmpl) ? tmpl : "/tmp";
  setenv("XDG_CONFIG_HOME", home.c_str(), 1);
  Config cfg;
  cfg.load();
  CHECK(!welcome::show_at_startup(cfg));
  // First login: no stamp, no [welcome] in the user's sde.conf.
  CHECK(!welcome::welcome_done());
  CHECK(welcome::mark_welcome_done());
  CHECK(welcome::welcome_done());
  CHECK(file_exists(home + "/sde/welcome-done"));
  unlink((home + "/sde/welcome-done").c_str());
  welcome::set_show_at_startup(cfg, true);
  CHECK(cfg.save());
  Config again;
  again.load();
  CHECK(welcome::show_at_startup(again));
  // Upgrades from alpha50: a [welcome] section in the user's file counts as seen.
  CHECK(welcome::welcome_done());
  unlink((home + "/sde/sde.conf").c_str());
  rmdir((home + "/sde").c_str());
  rmdir(home.c_str());

  // Autostart: never live; first login always; later only with the check box.
  CHECK(!welcome::should_autostart(true, false, true));
  CHECK(!welcome::should_autostart(true, true, true));
  CHECK(welcome::should_autostart(false, false, false));
  CHECK(!welcome::should_autostart(false, true, false));
  CHECK(welcome::should_autostart(false, true, true));
}

// 0.2 Alpha 2: the window's texts, icons and commands come from
// /usr/share/SDE-Welcome, overridden by /etc/SDE-Welcome.
static void put(const std::string &path, const std::string &text) {
  make_dirs(path.substr(0, path.rfind('/')));
  CHECK(write_file_atomic(path, text));
}

static std::string joined(const std::vector<std::string> &v) {
  std::string out;
  for (auto &s : v) out += (out.empty() ? "" : "|") + s;
  return out;
}

static void test_welcome_content(const std::string &shipped) {
  setenv("LANG", "id_ID.UTF-8", 1);  // before the first localized() call
  welcome::KeyFile kf;
  kf.parse("# comment\n[Welcome]\nTitle=Hello\nTitle[id]=Halo\nTitle[de]=Hallo\nSubtitle=a\\sb\n"
           "[Other]\nTitle=x\n");
  CHECK_EQ(kf.localized("Welcome", "Title"), "Halo");
  CHECK_EQ(kf.get("Welcome", "Subtitle"), "a b");
  CHECK_EQ(kf.get("Other", "Title"), "x");
  CHECK(!kf.has("Welcome", "Logo"));
  welcome::KeyFile over;
  over.parse("[Welcome]\nSubtitle=changed\nLogo=icons/x.svg\n");
  kf.merge(over);
  CHECK_EQ(kf.get("Welcome", "Subtitle"), "changed");
  CHECK_EQ(kf.get("Welcome", "Title"), "Hello");
  CHECK_EQ(kf.get("Welcome", "Logo"), "icons/x.svg");

  char tmpl[] = "/tmp/sde-welcome-XXXXXX";
  std::string root = mkdtemp(tmpl) ? tmpl : "/tmp/sde-welcome";
  std::string etc = root + "/etc", usr = root + "/usr", bin = root + "/bin";
  for (const char *prog : {"tool-a", "tool-b", "tool-c"}) put(bin + "/" + prog, "#!/bin/sh\n");
  for (const char *prog : {"tool-a", "tool-b", "tool-c"}) chmod((bin + "/" + prog).c_str(), 0755);
  std::string old_path = env("PATH");
  setenv("PATH", bin.c_str(), 1);
  setenv("SDE_WELCOME_DIRS", (etc + ":" + usr).c_str(), 1);

  // Without welcome.conf: the built-in texts.
  welcome::Content c = welcome::load_content("Debian GNU/Linux");
  CHECK_EQ(c.title, "Welcome to Debian GNU/Linux");
  CHECK_EQ(std::to_string(c.columns), "2");
  put(usr + "/welcome.conf", "[Welcome]\nTitle=Welcome to %OS%!\nSubtitle=Packaged\nWidth=800\nColumns=9\n"
                             "Logo=icons/logo.svg\n");
  put(etc + "/welcome.conf", "[Welcome]\nSubtitle=Ours\nSubtitle[id]=Milik kami\nShowDesktopLine=false\n");
  put(etc + "/icons/logo.svg", "<svg/>");
  c = welcome::load_content("DebION");
  CHECK_EQ(c.title, "Welcome to DebION!");
  CHECK_EQ(c.subtitle, "Milik kami");  // /etc wins, in the user's language
  CHECK_EQ(std::to_string(c.width), "800");
  CHECK_EQ(std::to_string(c.columns), "4");  // clamped
  CHECK(!c.desktop_line);
  CHECK_EQ(c.logo, etc + "/icons/logo.svg");
  CHECK_EQ(welcome::load_content("").title, "Welcome");  // no NAME in os-release

  // Steps: file name order; /etc replaces a file or hides it; programs that
  // are missing hand over to their fallback; NoDisplay files show only so.
  put(usr + "/steps/10-a.desktop", "[Desktop Entry]\nType=Application\nName=A\nName[id]=A id\nComment=a\n"
                                    "Icon=icon-a\nX-SDE-Fallback-Icons=icon-b;icons/c.png\nExec=tool-a --page \"x y\" %U\n");
  put(usr + "/steps/20-missing.desktop", "[Desktop Entry]\nType=Application\nName=Missing\nExec=no-such-tool\n"
                                          "X-SDE-Welcome-Fallback=21-also-missing.desktop\n");
  put(usr + "/steps/21-also-missing.desktop", "[Desktop Entry]\nName=Also\nExec=tool-x\nNoDisplay=true\n"
                                               "X-SDE-Welcome-Fallback=22-there.desktop\n");
  put(usr + "/steps/22-there.desktop", "[Desktop Entry]\nName=There\nExec=tool-b\nNoDisplay=true\n");
  put(usr + "/steps/30-link.desktop", "[Desktop Entry]\nType=Link\nName=Link\nURL=https://example.org/\n");
  put(usr + "/steps/40-hidden-by-etc.desktop", "[Desktop Entry]\nName=Hidden\nExec=tool-a\n");
  put(usr + "/steps/50-replaced.desktop", "[Desktop Entry]\nName=Old\nExec=tool-a\n");
  put(usr + "/steps/60-try.desktop", "[Desktop Entry]\nName=Try\nExec=tool-a\nTryExec=no-such-tool\n");
  put(usr + "/steps/70-loop.desktop", "[Desktop Entry]\nName=Loop\nExec=nothing\n"
                                       "X-SDE-Welcome-Fallback=70-loop.desktop\n");
  put(usr + "/steps/80-script.desktop", "[Desktop Entry]\nName=Script\nExec=scripts/run.sh\n");
  put(usr + "/scripts/run.sh", "#!/bin/sh\n");
  chmod((usr + "/scripts/run.sh").c_str(), 0755);
  put(usr + "/steps/notes.txt", "not a step");
  put(etc + "/steps/40-hidden-by-etc.desktop", "[Desktop Entry]\nName=Hidden\nHidden=true\n");
  put(etc + "/steps/50-replaced.desktop", "[Desktop Entry]\nName=New\nExec=tool-c\n");
  put(etc + "/steps/90-added.desktop", "[Desktop Entry]\nName=Added\nExec=tool-a\n");
  auto steps = welcome::load_steps();
  std::string names;
  for (auto &st : steps) names += st.name + ",";
  CHECK_EQ(names, "A id,There,Link,New,Script,Added,");
  if (steps.size() == 6) {
    CHECK_EQ(joined(steps[0].argv), "tool-a|--page|x y");
    CHECK_EQ(joined(steps[0].icons), "icon-a|icon-b|icons/c.png");  // no such file: kept as given
    CHECK_EQ(steps[1].file, "22-there.desktop");
    CHECK_EQ(steps[2].url, "https://example.org/");
    CHECK(steps[2].argv.empty());
    CHECK_EQ(steps[3].argv[0], "tool-c");
    CHECK_EQ(steps[4].argv[0], usr + "/scripts/run.sh");
  }

  // The files SDE ships: every tile shows when its program is installed.
  if (!shipped.empty()) {
    setenv("SDE_WELCOME_DIRS", shipped.c_str(), 1);
    for (const char *prog : {"sde-settings", "sde-update", "sde-drivers", "sde-system-reports"}) {
      put(bin + "/" + prog, "#!/bin/sh\n");
      chmod((bin + "/" + prog).c_str(), 0755);
    }
    names.clear();
    for (auto &st : welcome::load_steps()) names += st.file + ",";
    CHECK_EQ(names, "10-appearance.desktop,30-updates.desktop,40-drivers.desktop,52-system-reports.desktop,"
                    "60-control-center.desktop,70-help.desktop,80-donate.desktop,");
    for (const char *prog : {"merkadu", "timeshift-gtk"}) {
      put(bin + "/" + prog, "#!/bin/sh\n");
      chmod((bin + "/" + prog).c_str(), 0755);
    }
    names.clear();
    for (auto &st : welcome::load_steps()) names += st.file + ",";
    CHECK_EQ(names, "10-appearance.desktop,20-software.desktop,30-updates.desktop,40-drivers.desktop,"
                    "51-snapshots-gtk.desktop,60-control-center.desktop,70-help.desktop,80-donate.desktop,");
    c = welcome::load_content("Debian GNU/Linux");
    CHECK_EQ(c.title, "Welcome to Debian GNU/Linux");
    CHECK_EQ(c.subtitle, "A few first steps to make this computer yours.");
    CHECK(c.desktop_line);
  }
  unsetenv("SDE_WELCOME_DIRS");
  setenv("PATH", old_path.c_str(), 1);
  run_sync({"/bin/rm", "-rf", root});
}

static welcome::LiveProbe live_probe(const std::string &name) {
  welcome::LiveProbe p;
  p.cmdline = fixture("live/" + name + ".cmdline");
  p.mounts = fixture("live/" + name + ".mounts");
  p.user = "anna";
  return p;
}

static void test_live_session() {
  // Debian live ISO (live-boot): boot=live, / is an overlay on the squashfs.
  welcome::LiveProbe live = live_probe("debian-live");
  CHECK(welcome::is_live_session(live));
  CHECK_EQ(welcome::live_reason(live), "kernel command line: boot=live");
  live.cmdline = "BOOT_IMAGE=/live/vmlinuz quiet";  // the mounts alone are enough
  CHECK_EQ(welcome::live_reason(live), "root file system: overlay on squashfs (/run/live/rootfs/filesystem.squashfs)");
  live.cmdline = "boot=live toram";
  CHECK_EQ(welcome::live_reason(live), "kernel command line: boot=live toram");

  // Installed system (a snap squashfs elsewhere does not count).
  welcome::LiveProbe inst = live_probe("installed");
  CHECK(!welcome::is_live_session(inst));
  CHECK_EQ(welcome::live_reason(inst), "");
  // toram alone (e.g. a GRUB entry typo) is not a live boot.
  inst.cmdline += " toram";
  CHECK(!welcome::is_live_session(inst));
  inst.cmdline = fixture("live/installed.cmdline");

  // Installed system with overlayroot: the lower layer is ext4, not squashfs.
  welcome::LiveProbe ovl = live_probe("overlayroot");
  CHECK(!welcome::is_live_session(ovl));
  // ... but the same overlay over a squashfs image is live.
  ovl.mounts += "/dev/loop9 /media/root-ro squashfs ro 0 0\n";
  CHECK(welcome::is_live_session(ovl));

  // Ubuntu-style casper.
  welcome::LiveProbe casper = live_probe("casper");
  CHECK_EQ(welcome::live_reason(casper), "kernel command line: boot=casper");
  casper.cmdline = "quiet splash";
  CHECK_EQ(welcome::live_reason(casper), "root file system: overlay on squashfs (/rofs)");

  // Other live markers on an otherwise installed-looking system.
  welcome::LiveProbe m = inst;
  m.cmdline = "rd.live.image root=live:CDLABEL=DebION";
  CHECK(welcome::is_live_session(m));
  m = inst;
  m.cmdline = "live-media=/dev/sdb1 quiet";
  CHECK(welcome::is_live_session(m));
  m = inst;
  m.run_live = true;
  CHECK_EQ(welcome::live_reason(m), "/run/live exists");
  m = inst;
  m.live_medium = true;
  CHECK(welcome::is_live_session(m));
  m = inst;
  m.mounts = "tmpfs / tmpfs rw 0 0\n/dev/loop0 /lib/live/mount/rootfs/filesystem.squashfs squashfs ro 0 0\n";
  CHECK(welcome::is_live_session(m));
  m = inst;
  m.user = "user";
  CHECK(!welcome::is_live_session(m));  // "user" alone is a valid login name
  m.live_config = true;
  CHECK_EQ(welcome::live_reason(m), "live user with live-config");
  m = inst;
  m.installer_launcher = true;
  CHECK_EQ(welcome::live_reason(m), "installer launcher on the desktop");

  // probe_live_session() with the paths redirected.
  char tmpl[] = "/tmp/sde-live-XXXXXX";
  std::string root = mkdtemp(tmpl) ? tmpl : "/tmp/sde-live";
  setenv("SDE_PROC_CMDLINE", join_path(g_dir, "live/installed.cmdline").c_str(), 1);
  setenv("SDE_PROC_MOUNTS", join_path(g_dir, "live/installed.mounts").c_str(), 1);
  setenv("SDE_LIVE_ROOT", root.c_str(), 1);
  setenv("SDE_DESKTOP_DIR", (root + "/Desktop").c_str(), 1);
  CHECK(!welcome::is_live_session());
  make_dirs(root + "/Desktop");
  write_file_atomic(root + "/Desktop/install-debian.desktop", "[Desktop Entry]\n");
  CHECK(welcome::probe_live_session().installer_launcher);
  CHECK(welcome::is_live_session());
  unlink((root + "/Desktop/install-debian.desktop").c_str());
  make_dirs(root + "/run/live");
  CHECK(welcome::probe_live_session().run_live);
  rmdir((root + "/run/live").c_str());
  make_dirs(root + "/etc/live/config.conf.d");
  CHECK(welcome::probe_live_session().live_config);
  setenv("SDE_PROC_CMDLINE", join_path(g_dir, "live/debian-live.cmdline").c_str(), 1);
  CHECK(welcome::is_live_session());
  rmdir((root + "/etc/live/config.conf.d").c_str());
  rmdir((root + "/etc/live").c_str());
  rmdir((root + "/etc").c_str());
  rmdir((root + "/run").c_str());
  rmdir((root + "/Desktop").c_str());
  rmdir(root.c_str());
  unsetenv("SDE_PROC_CMDLINE");
  unsetenv("SDE_PROC_MOUNTS");
  unsetenv("SDE_LIVE_ROOT");
  unsetenv("SDE_DESKTOP_DIR");
}

static void test_driver_choices() {
  using drivers::DeviceKind;
  CHECK_EQ(drivers::dkms_device("rtw89-dkms"), "Realtek Wi-Fi");
  CHECK_EQ(drivers::dkms_device("r8168-dkms"), "Realtek Ethernet");
  CHECK_EQ(drivers::dkms_device("broadcom-sta-dkms"), "Broadcom Wi-Fi");
  CHECK_EQ(drivers::dkms_device("virtualbox-dkms"), "");
  CHECK_EQ(drivers::dkms_device("nvidia-kernel-dkms"), "");
  CHECK_EQ(drivers::dkms_device("rtw89"), "");
  CHECK(drivers::is_firmware_package("firmware-realtek"));
  CHECK(drivers::is_firmware_package("intel-microcode"));
  CHECK(!drivers::is_firmware_package("rtw89-dkms"));
  CHECK(drivers::is_free_driver("rtw89-dkms", "main"));
  CHECK(!drivers::is_free_driver("nvidia-driver", "non-free"));
  CHECK(!drivers::is_free_driver("firmware-iwlwifi", "non-free-firmware"));
  CHECK(drivers::pci_kind("0x028000") == DeviceKind::Wifi);
  CHECK(drivers::pci_kind("020000") == DeviceKind::Network);
  CHECK(drivers::pci_kind("0x0d1100") == DeviceKind::Bluetooth);
  CHECK(drivers::pci_kind("0x040300") == DeviceKind::Audio);
  CHECK(drivers::pci_kind("0x030000") == DeviceKind::Graphics);
  CHECK(drivers::pci_kind("0x060400") == DeviceKind::Other);

  // PCI devices from a fake sysfs (graphics cards are left to detect_gpus).
  setenv("SDE_PCI_IDS", join_path(g_dir, "missing-pci.ids").c_str(), 1);
  auto pci = drivers::detect_pci_devices(join_path(g_dir, "pci"));
  CHECK_EQ(std::to_string(pci.size()), "2");
  if (pci.size() == 2) {
    CHECK_EQ(pci[0].vendor_id, "8086");
    CHECK_EQ(pci[1].device_id, "b852");
    CHECK(drivers::pci_kind(pci[1].class_id) == DeviceKind::Wifi);
  }

  // apt output.
  auto pol = drivers::parse_apt_policy(
      "rtw89-dkms:\n  Installed: 7.3+klbkresolute\n  Candidate: 7.3+klbkresolute\n  Version table:\n"
      " *** 7.3+klbkresolute 100\n        100 /var/lib/dpkg/status\n"
      "firmware-realtek:\n  Installed: (none)\n  Candidate: 20250410-1\n"
      "nvidia-driver:amd64:\n  Installed: (none)\n  Candidate: (none)\n");
  CHECK_EQ(pol["rtw89-dkms"].installed, "7.3+klbkresolute");
  CHECK_EQ(pol["firmware-realtek"].installed, "");
  CHECK_EQ(pol["firmware-realtek"].candidate, "20250410-1");
  CHECK(pol.count("nvidia-driver") == 1);
  CHECK_EQ(pol["nvidia-driver"].candidate, "");
  auto sum = drivers::parse_apt_summaries(
      "Package: firmware-realtek\nVersion: 20250410-1\nDescription-en: Binary firmware for Realtek "
      "wired/wifi/BT adapters\n Long text.\n\nPackage: rtw89-dkms\nDescription: rtw89 DKMS modules for "
      "Debian\n");
  CHECK_EQ(sum["firmware-realtek"], "Binary firmware for Realtek wired/wifi/BT adapters");
  CHECK_EQ(sum["rtw89-dkms"], "rtw89 DKMS modules for Debian");
  double pct = -1;
  std::string what;
  CHECK(drivers::parse_apt_status("dlstatus:1:50.0:Retrieving file 1 of 2", &pct, &what));
  CHECK(pct > 14.9 && pct < 15.1);
  CHECK_EQ(what, "Retrieving file 1 of 2");
  CHECK(drivers::parse_apt_status("pmstatus:nvidia-driver:100:Installed nvidia-driver (535:1)", &pct, &what));
  CHECK(pct > 99.9);
  CHECK_EQ(what, "Installed nvidia-driver (535:1)");
  CHECK(!drivers::parse_apt_status("Reading package lists...", &pct, &what));
  CHECK(!drivers::parse_apt_status("pmstatus:broken", &pct, &what));
  CHECK_EQ(drivers::cpu_model_name("processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel(R) "
                                   "Core(TM) i5-8250U CPU @ 1.60GHz\n"),
           "Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz");

  // Grouping: an NVIDIA card with the proprietary driver and the firmware
  // for nouveau (installed), a Realtek Wi-Fi card that runs a DKMS driver
  // installed by hand, and processor microcode.
  std::vector<drivers::Recommendation> recs(5);
  recs[0] = {"intel-microcode", "Intel(R) Core(TM) i5-8250U", "Microcode.", "non-free-firmware", true, true};
  recs[1] = {"firmware-realtek", "Realtek Wi-Fi", "The kernel could not load rtw89/x.bin.", "non-free-firmware",
             false, true};
  recs[2] = {"nvidia-driver", "NVIDIA GeForce RTX 3070", "Proprietary NVIDIA driver. More text.", "non-free",
             false, true};
  recs[3] = {"firmware-nvidia-graphics", "NVIDIA GeForce RTX 3070", "Firmware for nouveau.",
             "non-free-firmware", true, false};
  recs[4] = {"rtw89-dkms", "Realtek Wi-Fi", "DKMS.", "main", true, false};
  std::vector<drivers::PciDevice> devs(1);
  devs[0].class_id = "028000";
  devs[0].vendor_id = "10ec";
  devs[0].device_id = "b852";
  devs[0].vendor = "Realtek Semiconductor Co., Ltd.";
  devs[0].model = "RTL8852BE PCIe 802.11ax Wireless Network Controller";
  auto devices = drivers::group_devices(recs, devs, pol, sum);
  CHECK_EQ(std::to_string(devices.size()), "3");
  if (devices.size() != 3) return;
  CHECK(devices[0].kind == DeviceKind::Graphics);
  CHECK_EQ(devices[0].name, "NVIDIA GeForce RTX 3070");
  CHECK_EQ(devices[0].options[0].package, "nvidia-driver");
  CHECK_EQ(devices[0].options[0].summary, "Proprietary NVIDIA driver.");
  CHECK(devices[1].kind == DeviceKind::Wifi);
  CHECK_EQ(devices[1].name, "Realtek Semiconductor Co., Ltd.: RTL8852BE PCIe 802.11ax Wireless Network Controller");
  CHECK_EQ(devices[1].options[0].package, "firmware-realtek");
  CHECK_EQ(devices[1].options[0].version, "20250410-1");
  CHECK(!devices[1].options[0].free_software);
  CHECK_EQ(devices[1].options[1].package, "rtw89-dkms");
  CHECK_EQ(devices[1].options[1].version, "7.3+klbkresolute");
  CHECK_EQ(devices[1].options[1].summary, "rtw89 DKMS modules for Debian");
  CHECK(devices[1].options[1].free_software);
  CHECK(devices[2].kind == DeviceKind::Processor);

  auto initial = drivers::initial_selection(devices);
  CHECK_EQ(std::to_string(initial[0]), "1");  // firmware for nouveau
  CHECK_EQ(std::to_string(initial[1]), "1");  // the DKMS driver, not the firmware
  CHECK_EQ(std::to_string(initial[2]), "0");
  CHECK(drivers::device_state(devices[0], initial[0]) == drivers::DeviceState::Alternative);
  CHECK(drivers::device_state(devices[1], initial[1]) == drivers::DeviceState::Alternative);
  CHECK(drivers::device_state(devices[2], initial[2]) == drivers::DeviceState::Recommended);
  CHECK(drivers::device_state(devices[2], drivers::kNoDriver) == drivers::DeviceState::NoDriver);
  CHECK_EQ(std::to_string(drivers::proprietary_in_use(devices)), "2");

  // Nothing changed (or Revert): nothing to do.
  CHECK(drivers::plan_changes(devices, initial, initial).empty());
  CHECK(drivers::apt_arguments(drivers::plan_changes(devices, initial, initial)).empty());

  // NVIDIA driver: installed, the nouveau firmware is kept.
  auto sel = initial;
  sel[0] = 0;
  auto c = drivers::plan_changes(devices, initial, sel);
  CHECK_EQ(std::to_string(c.install.size()), "1");
  CHECK_EQ(c.install[0], "nvidia-driver");
  CHECK(c.remove.empty());

  // Back to the recommended firmware for Wi-Fi: the DKMS driver goes.
  sel = initial;
  sel[1] = 0;
  c = drivers::plan_changes(devices, initial, sel);
  CHECK_EQ(c.install[0], "firmware-realtek");
  CHECK_EQ(c.remove[0], "rtw89-dkms");

  // "Do not use the device" removes everything, firmware included.
  sel = initial;
  sel[2] = drivers::kNoDriver;
  sel[0] = 0;
  sel[1] = 0;
  c = drivers::plan_changes(devices, initial, sel);
  std::string args;
  for (auto &a : drivers::apt_arguments(c)) args += a + " ";
  CHECK_EQ(args, "install -y nvidia-driver firmware-realtek rtw89-dkms- intel-microcode- ");
  // Root's apt-get only ever gets package names (never an option).
  CHECK(drivers::valid_package_name("nvidia-driver") && drivers::valid_package_name("g++-14"));
  CHECK(!drivers::valid_package_name("-o") && !drivers::valid_package_name("Nvidia"));
  CHECK(!drivers::valid_package_name("a b") && !drivers::valid_package_name("x"));
  drivers::Changes bad = c;
  bad.install.push_back("--allow-unauthenticated");
  CHECK(drivers::apt_arguments(bad).empty());

  // Choosing an option that is installed already only removes the others.
  sel = initial;
  sel[0] = 0;
  devices[0].options[0].installed = true;
  initial = drivers::initial_selection(devices);
  CHECK_EQ(std::to_string(initial[0]), "0");  // the driver wins over the firmware
  sel = initial;
  sel[0] = 1;
  c = drivers::plan_changes(devices, initial, sel);
  CHECK(c.install.empty());
  CHECK_EQ(c.remove[0], "nvidia-driver");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: sde-sysreport-tests FIXTURE_DIR [WELCOME_DATA_DIR]\n");
    return 2;
  }
  g_dir = argv[1];
  test_firmware_log();
  test_gpus();
  test_misc_parsers();
  test_sources();
  test_reports();
  test_welcome();
  test_welcome_content(argc > 2 ? argv[2] : "");
  test_live_session();
  test_driver_choices();
  if (g_failures) fprintf(stderr, "%d check(s) failed\n", g_failures);
  else printf("sysreport: all checks passed\n");
  return g_failures ? 1 : 0;
}
