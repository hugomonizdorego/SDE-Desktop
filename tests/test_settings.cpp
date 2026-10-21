// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for sde-settings helpers without a display: graphics card
// detection (pci.ids, lspci -mm, sysfs) and the wallpaper scan.
// Usage: sde-settings-tests <tests/fixtures/pci.ids>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "devices.h"
#include "gpu.h"
#include "util.h"
#include "wallpapers.h"

using namespace sde;

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    auto va = (a);                                                                         \
    auto vb = (b);                                                                         \
    if (!(va == vb)) {                                                                     \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, \
              #a, #b, std::string(va).c_str());                                            \
      ++g_failures;                                                                        \
    }                                                                                      \
  } while (0)

static std::string g_ids;

static void put(const std::string &path, const std::string &text) {
  make_dirs(path.substr(0, path.rfind('/')));
  std::ofstream(path) << text;
}

static void test_pci_ids() {
  {
    std::ifstream in(g_ids);
    PciName n = lookup_pci_ids(in, "0x10DE", "0x2484");
    CHECK_EQ(n.vendor, "NVIDIA Corporation");
    CHECK_EQ(n.device, "GA104 [GeForce RTX 3070]");
  }
  {
    std::ifstream in(g_ids);
    PciName n = lookup_pci_ids(in, "1002", "73ff");
    CHECK_EQ(n.vendor, "Advanced Micro Devices, Inc. [AMD/ATI]");
    CHECK_EQ(n.device, "Navi 23 [Radeon RX 6600/6600 XT/6600M]");
  }
  {
    // Subsystem lines (two tabs) are never taken for devices.
    std::ifstream in(g_ids);
    PciName n = lookup_pci_ids(in, "1002", "1458");
    CHECK_EQ(n.vendor, "Advanced Micro Devices, Inc. [AMD/ATI]");
    CHECK_EQ(n.device, "");
  }
  {
    std::ifstream in(g_ids);
    PciName n = lookup_pci_ids(in, "8086", "46A6");
    CHECK_EQ(n.device, "Alder Lake-P GT2 [Iris Xe Graphics]");
  }
  {
    // Unknown vendor; the class list ("C 03") is not a vendor.
    std::ifstream in(g_ids);
    PciName n = lookup_pci_ids(in, "c003", "0000");
    CHECK_EQ(n.vendor, "");
    CHECK_EQ(n.device, "");
  }
  {
    std::istringstream in("10de  NVIDIA Corporation\r\n\t2484  GA104\r\n");
    CHECK_EQ(lookup_pci_ids(in, "10de", "2484").device, "GA104");
  }
}

static void test_names() {
  CHECK_EQ(short_vendor("Advanced Micro Devices, Inc. [AMD/ATI]"), "AMD/ATI");
  CHECK_EQ(short_vendor("NVIDIA Corporation"), "NVIDIA Corporation");
  CHECK_EQ(known_vendor("0x8086"), "Intel Corporation");
  CHECK_EQ(known_vendor("abcd"), "");
  Gpu g;
  g.vendor = "Advanced Micro Devices, Inc. [AMD/ATI]";
  g.model = "Navi 23 [Radeon RX 6600/6600 XT/6600M]";
  CHECK_EQ(gpu_label(g), "AMD/ATI Navi 23 [Radeon RX 6600/6600 XT/6600M]");
  Gpu unknown;
  unknown.vendor_id = "abcd";
  unknown.device_id = "1234";
  CHECK_EQ(gpu_label(unknown), "[abcd] [1234]");

  PciName l;
  CHECK(parse_lspci_mm(
      "01:00.0 \"VGA compatible controller\" \"NVIDIA Corporation\" \"GA104 [GeForce RTX 3070]\" "
      "-ra1 \"ASUSTeK Computer Inc.\" \"Device 87b8\"",
      l));
  CHECK_EQ(l.vendor, "NVIDIA Corporation");
  CHECK_EQ(l.device, "GA104 [GeForce RTX 3070]");
  CHECK(!parse_lspci_mm("garbage", l));

  CHECK_EQ(parse_gl_renderer("name of display: :0\ndirect rendering: Yes\n"
                             "    OpenGL renderer string: Mesa Intel(R) Xe Graphics (TGL GT2)\n"),
           "Mesa Intel(R) Xe Graphics (TGL GT2)");
  CHECK_EQ(parse_gl_renderer("OpenGL core profile renderer: llvmpipe (LLVM 19.1.7, 256 bits)\n"),
           "llvmpipe (LLVM 19.1.7, 256 bits)");
  CHECK_EQ(parse_gl_renderer("Error: unable to open display"), "");
}

static void test_sysfs(const std::string &tmp) {
  // A hybrid laptop: Intel (primary, i915) and NVIDIA (nvidia), plus a
  // network card that must be ignored.
  std::string sys = tmp + "/pci";
  auto dev = [&](const std::string &slot, const char *cls, const char *ven, const char *devid,
                 const char *driver, bool boot) {
    std::string d = sys + "/" + slot;
    put(d + "/class", std::string(cls) + "\n");
    put(d + "/vendor", std::string(ven) + "\n");
    put(d + "/device", std::string(devid) + "\n");
    if (boot) put(d + "/boot_vga", "1\n");
    if (driver) {
      std::string target = tmp + "/drivers/" + driver;
      make_dirs(target);
      CHECK(symlink(target.c_str(), (d + "/driver").c_str()) == 0);
    }
  };
  dev("0000:01:00.0", "0x030200", "0x10de", "0x1f99", "nvidia", false);
  dev("0000:00:02.0", "0x030000", "0x8086", "0x9a49", "i915", true);
  dev("0000:02:00.0", "0x020000", "0x8086", "0x15f3", "igc", false);
  dev("0000:03:00.0", "0x038000", "0xabcd", "0x0001", nullptr, false);
  setenv("SDE_PCI_IDS", g_ids.c_str(), 1);
  auto gpus = detect_gpus(sys, false);
  CHECK(gpus.size() == 3);
  if (gpus.size() == 3) {
    CHECK_EQ(gpus[0].slot, "0000:00:02.0");
    CHECK(gpus[0].boot_vga);
    CHECK_EQ(gpus[0].vendor, "Intel Corporation");
    CHECK_EQ(gpus[0].model, "TigerLake-LP GT2 [Iris Xe Graphics]");
    CHECK_EQ(gpus[0].driver, "i915");
    CHECK_EQ(gpus[1].model, "TU117M [GeForce GTX 1650 Mobile / Max-Q]");
    CHECK_EQ(gpus[1].driver, "nvidia");
    CHECK(!gpus[1].boot_vga);
    CHECK_EQ(gpus[2].vendor_id, "abcd");
    CHECK_EQ(gpus[2].driver, "");
    CHECK_EQ(gpu_label(gpus[2]), "[abcd] [0001]");
    CHECK(renderer_gpu(gpus, "NVIDIA GeForce GTX 1650/PCIe/SSE2") == 1);
    CHECK(renderer_gpu(gpus, "Mesa Intel(R) UHD Graphics (TGL GT2)") == 0);
    CHECK(renderer_gpu(gpus, "llvmpipe (LLVM 19.1.7, 256 bits)") == -1);
    CHECK(renderer_gpu(gpus, "AMD Radeon RX 6600 (radeonsi, navi23)") == -1);
  }
  CHECK(detect_gpus(tmp + "/missing", false).empty());
}

static void test_wallpapers(const std::string &tmp) {
  CHECK_EQ(wallpaper_label("/usr/share/backgrounds/sde/selviana-dark.svg"), "Selviana Dark");
  CHECK_EQ(wallpaper_label("/x/debion-theme/wallpaper/contents/images/1920x1080.svg"), "Debion");
  CHECK_EQ(wallpaper_label("/a/b/my_photo.JPG"), "My Photo");

  std::string bg = tmp + "/backgrounds", db = tmp + "/desktop-base";
  put(bg + "/sde/b.svg", "<svg/>");
  put(bg + "/sde/a.png", "png");
  put(bg + "/gnome/nested/deep/c.jpg", "jpg");
  put(bg + "/gnome/d.webp", "webp");
  put(bg + "/gnome/slideshow.xml", "<background/>");
  put(bg + "/gnome/readme.txt", "text");
  put(bg + "/.hidden/e.png", "png");
  put(db + "/debion-theme/wallpaper/contents/images/1280x720.svg", "<svg/>");
  put(db + "/debion-theme/wallpaper/contents/images/1920x1080.svg", "<svg/>");
  put(db + "/emerald-theme/wallpaper/contents/images/1920x1080.svg", "<svg/>");
  put(db + "/emerald-theme/wallpaper/contents/images/3840x2160.svg", "<svg/>");
  // /usr/share/backgrounds/sde/debion.svg links into the theme.
  CHECK(symlink((db + "/debion-theme/wallpaper/contents/images/1920x1080.svg").c_str(),
                (bg + "/sde/debion.svg").c_str()) == 0);
  // The same folder twice through a link is listed once.
  CHECK(symlink((bg + "/gnome").c_str(), (bg + "/gnome-link").c_str()) == 0);

  std::vector<std::string> roots{bg + "/sde", db + "/debion-theme/wallpaper/contents/images",
                                 db + "/emerald-theme/wallpaper/contents/images", bg};
  auto list = scan_wallpapers(roots, {"webp"});
  std::vector<std::string> names;
  for (auto &w : list) names.push_back(w.path.substr(tmp.size() + 1));
  std::string joined;
  for (auto &n : names) joined += n + "\n";
  std::string want =
      "backgrounds/sde/a.png\n"
      "backgrounds/sde/b.svg\n"
      "backgrounds/sde/debion.svg\n"
      "desktop-base/emerald-theme/wallpaper/contents/images/3840x2160.svg\n"
      "backgrounds/gnome/d.webp\n"
      "backgrounds/gnome/nested/deep/c.jpg\n";
  if (joined != want) {
    fprintf(stderr, "wallpapers:\n%s---want:\n%s", joined.c_str(), want.c_str());
    ++g_failures;
  }
  // Without webp support those files are left out; the limit is honored.
  CHECK(scan_wallpapers(roots, {}).size() == 5);
  CHECK(scan_wallpapers(roots, {"webp"}, 2).size() == 2);
}

static void test_devices() {
  auto sinks = parse_pactl_devices(
      "Sink #47\n\tState: RUNNING\n\tName: alsa_output.pci.analog-stereo\n"
      "\tDescription: Built-in Audio Analog Stereo\n\tMute: no\n"
      "Sink #52\n\tName: bluez_output.AA_BB.1\n\tDescription: Headphones\n");
  CHECK(sinks.size() == 2);
  if (sinks.size() == 2) {
    CHECK_EQ(sinks[0].name, "alsa_output.pci.analog-stereo");
    CHECK_EQ(sinks[0].description, "Built-in Audio Analog Stereo");
    CHECK_EQ(sinks[1].description, "Headphones");
  }
  auto sources = parse_pactl_devices(
      "Source #1\n\tName: alsa_output.pci.analog-stereo.monitor\n\tDescription: Monitor\n"
      "Source #2\n\tName: alsa_input.pci.analog-stereo\n\tDescription: Microphone\n");
  CHECK(sources.size() == 1 && sources[0].description == "Microphone");
  CHECK(first_percent("Volume: front-left: 42597 /  65% / -11.23 dB,") == 65);
  CHECK(first_percent("nothing") == -1);
  CHECK(parse_pactl_mute("Mute: yes\n"));
  CHECK(!parse_pactl_mute("Mute: no\n"));

  auto f = split_nmcli("wlan0:wifi:connected:Caf\\: Guest");
  CHECK(f.size() == 4 && f[3] == "Caf: Guest");
  auto nets = parse_nmcli_devices(
      "wlan0:wifi:connected:Home\nenp3s0:ethernet:unavailable:--\nlo:loopback:connected "
      "(externally):lo\np2p-dev-wlan0:wifi-p2p:disconnected:--\n");
  CHECK(nets.size() == 2);
  if (nets.size() == 2) {
    CHECK_EQ(nets[0].connection, "Home");
    CHECK_EQ(nets[1].type, "ethernet");
    CHECK_EQ(nets[1].connection, "");
  }

  auto bt = parse_bluetoothctl_devices(
      "Device 00:1A:7D:DA:71:13 WH-1000XM4\nDevice 11:22:33:44:55:66 Logitech MX Master 3\n"
      "Waiting to connect to bluetoothd...\n");
  CHECK(bt.size() == 2 && bt[1].name == "Logitech MX Master 3");
  CHECK_EQ(bluetoothctl_field("Controller 00:11 (public)\n\tName: laptop\n\tPowered: yes\n",
                              "Powered"),
           "yes");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s PCI_IDS_FIXTURE\n", argv[0]);
    return 2;
  }
  g_ids = argv[1];
  char tmpl[] = "/tmp/sde-settings-test-XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (!dir) return 2;
  std::string tmp = dir;
  setenv("HOME", tmp.c_str(), 1);
  test_pci_ids();
  test_names();
  test_devices();
  test_sysfs(tmp);
  test_wallpapers(tmp);
  std::string cmd = "rm -rf '" + tmp + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("sde-settings-tests: all passed\n");
  return 0;
}
