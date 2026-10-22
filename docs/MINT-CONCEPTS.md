# Linux Mint concepts in SDE (0.1.0~alpha50)

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 DebION Dev-Team -->

The request: take the ideas a new Linux Mint user meets in the first hour
(the "Using Linux Mint for the first time" walkthrough on reallinuxuser.com)
and apply them to SDE and the SDE Panel. Mint's code, names and artwork are
**not** used: the programs below are written from scratch by the DebION
Dev-Team in C++17 on SDE-FLTK, cairo and Xlib, under GPL-3.0-or-later, and
install Debian's own packages.

| Mint concept | SDE |
|---|---|
| Welcome Screen with First Steps, shown at the first login, "Show this dialog at startup" | **SDE Welcome** (`sde-welcome`): at the first login of each user after installation, later while `[welcome] show=true`, never in the live session (alpha51) |
| First Steps: desktop colors (light/dark, accent) | Welcome > First Steps > Desktop colors (writes `[appearance] theme`, `accent_name`, `accent`; GTK, Openbox, compositor and `_SDE_CONFIG_SERIAL` follow at once) |
| First Steps: panel layout (traditional / modern) | Welcome > First Steps > Panel layout (`[panel] style=full|short|floating`, position, Show desktop button) |
| Update Manager | opens SDE Update (`sde-update`, already modelled on mintupdate) |
| Driver Manager | **SDE Driver Manager** (`sde-drivers`; one row per device with radio choices since alpha51) |
| System Snapshots (Timeshift) | launches Timeshift, or explains it and installs `timeshift` |
| Firewall (ufw/gufw) | status from `/etc/ufw/ufw.conf`, "Turn On" (`pkexec ufw enable`), installs `ufw gufw` |
| Multimedia codecs | installs `gstreamer1.0-plugins-good/-bad/-ugly gstreamer1.0-libav` (all in Debian main) |
| Software Manager | opens GNOME Software, KDE Discover or Synaptic, or installs `gnome-software` |
| System Reports | **SDE System Reports** (`sde-system-reports`, also a Welcome page) |
| Help, Documentation, Forums, Contribute, Donate | Welcome > Documentation and Contribute (GitHub docs, issues, Debian wiki and forums, translations, PayPal donation) |
| "Show desktop" button at the end of the panel | already in the panel as an option; the Welcome turns it on; patch below moves it to the far end and adds `sde-panel --show-desktop` |
| Workspace switcher applet, hot corners | proposals below (panel and Settings owners) |

## SDE Welcome (`sde-welcome`), since 0.1.0~alpha51

One calm window (720 x 480 at 100 %, colours from the active theme, 8-10 px
rounded corners, no gradients): the DebION logo with "Welcome to DebION" and
one line, then six first steps as tiles (two columns, one below 520 px),
each an icon, a label and a one-line hint:

| Tile | Opens |
|---|---|
| Appearance | `sde-settings --page appearance` |
| Software Updater | `sde-update` |
| Drivers | `sde-drivers` |
| Snapshots (Timeshift installed) or System Reports | `timeshift-launcher` / `sde-system-reports` |
| Help | the documentation on GitHub |
| Donate | https://paypal.me/hugocenturion0311 |

At the bottom: the check box "Show this dialog at startup" (`[welcome]
show`) and Close. Esc closes, F1 or a right click on empty space shows
About. The alpha50 sidebar pages (colour and panel pickers, firewall, codecs,
the embedded Driver Manager and System Reports pages) are gone: each of
those has its own program, opened from the tiles or the Start menu.
`--page NAME` is still accepted and ignored.

### Live session and first login

`/etc/xdg/autostart/sde-welcome.desktop` runs `sde-welcome --autostart` at
every login; `welcome::should_autostart(live, done, show)` decides:

1. **Live session** (the DebION ISO): never. The program exits 0 silently.
2. **First login of a user after installation** (no stamp): always. Opening
   the window on an installed system writes the per-user stamp
   `$XDG_CONFIG_HOME/sde/welcome-done` (`~/.config/sde/welcome-done`).
   Upgrades from alpha50 and older count as done when the user's sde.conf
   already has a `[welcome]` section.
3. **Later logins**: only while "Show this dialog at startup" is ticked
   (`[welcome] show=true`; the default is `false` since alpha51).

Started from the menu, the window always opens. In a live session the check
box is hidden and a note reads "Live session — install DebION to keep your
settings"; no stamp is written.

`welcome::is_live_session()` (`src/welcome/model.cpp`) returns true when
any one of these signals is present (`live_reason()` names the first found;
`sde-welcome --live-status` prints it):

* `/proc/cmdline` has `boot=live` (live-boot, Debian live ISOs),
  `boot=casper` or `casper` (Ubuntu-style), `rd.live.image` (dracut
  dmsquash-live), `live-media=` or `live-media-path=`. `toram` alone is not
  enough; it is only reported next to one of these.
* `/run/live`, `/lib/live/mount/medium` or `/run/live/medium` exists.
* The root file system (the last `/` entry of `/proc/mounts`) is a
  squashfs, or an **overlay whose `lowerdir` lies on a squashfs mount**
  (`/run/live/rootfs/filesystem.squashfs`, casper's `/rofs`), or a tmpfs
  while a squashfs is mounted. An installed system with **overlayroot**
  (lower layer `/media/root-ro` on ext4/btrfs) is therefore *installed*;
  squashfs mounts elsewhere (snaps under `/snap`) do not count.
* The user is `user` or `live` **and** the live-config marker
  `/etc/live/config*` exists (the installer removes live-config).
* A Calamares or debian-installer launcher (`*calamares*.desktop`,
  `install-debian.desktop`, `debian-installer*.desktop`,
  `install-debion*.desktop`) is on the desktop.

Test overrides: `SDE_PROC_CMDLINE`, `SDE_PROC_MOUNTS`, `SDE_LIVE_ROOT`
(prefix for `/run/live`, `/lib/live`, `/etc/live`) and `SDE_DESKTOP_DIR`.
`tests/fixtures/sysreport/live/` holds command lines and mount tables of a
Debian live ISO, an installed system, an installed system with overlayroot
and a casper live system; `sde-sysreport-model` checks each one plus the
autostart table.

### The alpha50 Welcome (for reference)

Sidebar pages (Ctrl+Page Up/Down moves between them, Esc closes, F1 or a
right click on empty space shows About):

* **Welcome**: banner with the SDE logo, version and OS name, links to First
  Steps and the documentation, a short "About DebION and SDE".
* **First Steps**: Desktop colors (Light / Dark / Auto preview tiles and the
  accent swatches; "Appearance Settings" opens `sde-settings --page
  appearance`), Panel layout (Traditional / Centered / Floating tiles,
  position, the Show desktop switch, "Panel Settings"), then one card per
  step with its state: Update Manager, Driver Manager, System Snapshots,
  Firewall, Multimedia Codecs, Software, System Reports.
* **Driver Manager**, **System Reports**: the same pages as the stand-alone
  programs.
* **Documentation**: docs on GitHub (and the local copy), troubleshooting,
  issues, Debian wiki and forums.
* **Contribute**: Donate with PayPal (https://paypal.me/hugocenturion0311),
  code, translations, testing.

The sidebar switch "Show this dialog at startup" writes `[welcome] show` in
`~/.config/sde/sde.conf` (default `true`, in `data/sde.conf`).
`/etc/xdg/autostart/sde-welcome.desktop` runs `sde-welcome --autostart`,
which exits at once when the key is false. `sde-welcome --page NAME` opens a
page (welcome, first-steps, drivers, reports, help, contribute).

Administrator actions run through `pkexec` (the SDE polkit agent asks for the
password) without blocking the window; a banner shows what is running and the
apt output is shown when it fails. Installs use `pkexec env
DEBIAN_FRONTEND=noninteractive apt-get install -y PACKAGES`.

## SDE Driver Manager (`sde-drivers`)

### The window (since 0.1.0~alpha51)

Like Mint's Driver Manager, only more minimal (`drivers_main.cpp`): the
line "Additional and proprietary drivers" under the title bar, then one row
per device: a device icon (graphics, Wi-Fi, Bluetooth, network, audio,
processor or other, from the icon theme), the vendor and model ("Realtek
Semiconductor Co., Ltd.: RTL8852BE PCIe 802.11ax Wireless Network
Controller", named from the PCI device of the same kind and vendor in
`/sys/bus/pci/devices`), one status line ("This device is using the
recommended driver." / "... an alternative driver." / "... no additional
driver."), and radio choices: each package with a muted accent tag
"(open-source)" / "(proprietary)" plus ", recommended", "Version x.y"
(installed, else the candidate of `apt-cache policy`) and a one-line
description (`apt-cache show`), then "Do not use the device". Below a thin
separator: the status ("No proprietary drivers are in use." / "N
proprietary drivers in use.") and **Revert** / **Apply Changes**, disabled
until the selection changes. Without devices it shows "No additional drivers
are needed." in the middle. The list scrolls (1024x600 at 150 %), arrows
move between choices, Esc closes.

Selection model (`drivers.h`, unit tested): `group_devices()` groups the
recommendations by device (recommended first); `initial_selection()` is
the installed driver (a non-firmware package wins over firmware), else the
installed firmware, else "Do not use"; `plan_changes(devices, initial,
selection)` lists what to install and remove for the devices whose choice
changed: the chosen package is installed and the device's other installed
packages are removed, except firmware/microcode, which stays unless "Do not
use the device" was chosen. Installed hardware DKMS drivers (`rtw89-dkms`,
`rtw88`, `rtl8*`, `r8168`, `r8125`, `broadcom-sta`, `mt76`, `iwlwifi`
`-dkms`) appear as the alternative driver of their device (`dkms_device()`);
firmware for nouveau is the alternative to `nvidia-driver`.

**Apply Changes** runs one authorization: `pkexec /bin/sh -c SCRIPT sh
TMP SOURCES apt-get-args`, where the script first installs the rewritten
sources file and runs `apt-get update` when a needed area
(`non-free-firmware`, `contrib non-free`) is off (a notice above the list
says so), then `DEBIAN_FRONTEND=noninteractive apt-get -o
APT::Status-Fd=1 install -y A B C-` (a trailing `-` removes C in the same
transaction). apt's status lines (`parse_apt_status()`) drive a slim
progress line on the separator and the status text; a failure shows the
last lines of the output. Afterwards the system is scanned again and the
status says to restart. **Revert** restores the initial selection.

`SDE_DRIVERS_DEMO=1` (or `=empty`) shows example devices for screenshots
and installs nothing; `SDE_DRIVERS_DEMO_HOLD=1` freezes the fake progress.

### Detection

`src/sysreport/drivers.{h,cpp}`, display-free and unit tested:

* **Graphics** (PCI class 0x03 from `/sys`, via `detect_gpus()` of
  `sde-settings-model`, the same code as Settings > About): NVIDIA ->
  `nvidia-driver` (or the series `nvidia-detect` names, e.g.
  `nvidia-tesla-470-driver`) plus `firmware-nvidia-graphics` while nouveau is
  bound; AMD -> `firmware-amd-graphics`; Intel -> `firmware-intel-graphics`.
  Virtual GPUs get nothing.
* **Missing firmware** from the kernel log (`journalctl -k -b`, else `dmesg`;
  a note explains the `adm` group when neither is readable): lines "firmware:
  failed to load X", "Direct firmware load for X failed", "failed to load
  firmware X", "Failed to load DMC firmware X" ... are mapped by path prefix
  to Debian's packages: `iwlwifi-*`, `intel/ibt-*` -> firmware-iwlwifi;
  `rtl_nic/`, `rtlwifi/`, `rtw88/`, `rtw89/`, `rtl_bt/` -> firmware-realtek;
  `ath*/`, `qca/`, `ar3k/` -> firmware-atheros; `brcm/`, `cypress/` ->
  firmware-brcm80211; `amdgpu/`, `radeon/` -> firmware-amd-graphics; `i915/`,
  `xe/` -> firmware-intel-graphics; `intel/sof*` -> firmware-sof-signed;
  `mediatek/`, `mt7*` -> firmware-mediatek; `nvidia/` ->
  firmware-nvidia-graphics; `ti-connectivity/`, `libertas/`, `mrvl/`,
  `qcom/`, `bnx2*/`; `regulatory.db` -> wireless-regdb; anything else ->
  firmware-misc-nonfree.
* **Microcode**: `intel-microcode` / `amd64-microcode` from `/proc/cpuinfo`
  (not in virtual machines).
* **isenkram-lookup** suggestions when `isenkram-cli` is installed.
* Installed state from `dpkg-query`, Install / Install All buttons.
* **Software sources**: whether `/etc/apt/sources.list.d/debian.sources`
  (deb822) or `/etc/apt/sources.list` enables `non-free-firmware` (and
  `contrib non-free` when NVIDIA's driver is recommended). "Enable" appends
  the missing components to the Debian entries only (comments, options and
  third-party repositories are untouched), saves a copy of the old file in
  `~/.local/state/sde/apt-backup/`, then one `pkexec /bin/sh -c 'install -m
  0644 -- "$1" "$2" && apt-get update'` puts the new file in place (paths are
  arguments, never pasted into the script).

`sde-drivers --list` prints the same as text. Test overrides:
`SDE_KERNEL_LOG`, `SDE_APT_SOURCES`, `SDE_CPUINFO`, `SDE_PCI_SYSFS`.

## SDE System Reports (`sde-system-reports`)

`src/sysreport/reports.{h,cpp}` reads `~/.local/state/sde/session.log`,
`session.log.old` and other `*.log` files there: the backtraces written by the
sde-common crash handler ("X crashed: SIGNAL. Backtrace ... -- end of
backtrace") and sde-session's "NAME exited (killed by signal N (...))"
lines. Repeats of the same crash (same program, signal and frames, addresses
ignored) are grouped with a count and the time of the latest. Each report
has **Copy Report** (Markdown with backtrace and system summary) and
**Report on GitHub**, which copies the full report and opens
`https://github.com/hugomonizdorego/SDE-Desktop/issues/new?title=...&body=...`
(the body is shortened to keep the URL below 7000 bytes; the clipboard keeps
everything). "This computer" shows SDE/OS/kernel/CPU/memory/graphics (with the
bound driver)/rendering class/VM/session, with Copy and New Issue.
`sde-system-reports --list` and `--summary` print them. `SDE_REPORTS_DIR`
overrides the directory.

## Files

* `src/welcome/main.cpp`, `src/welcome/model.{h,cpp}`, `src/welcome/CMakeLists.txt`
  (one `add_subdirectory(welcome)` line in `src/CMakeLists.txt`)
* `src/sysreport/{drivers,reports,shell,pages}.{h,cpp}`,
  `drivers_main.cpp`, `reports_main.cpp`. The window shell reuses the
  sde-settings page widgets (`src/settings/ui.cpp`, compiled in, unchanged).
* `data/applications/sde-welcome.desktop`, `sde-drivers.desktop`,
  `sde-system-reports.desktop`; `data/autostart/sde-welcome.desktop`;
  `data/metainfo/org.debion.{Welcome,DriverManager,SystemReports}.metainfo.xml`;
  `[welcome]` in `data/sde.conf`.
* `tests/test_sysreport.cpp` + `tests/fixtures/sysreport/` (kernel log,
  session log, sources.list, debian.sources, nvidia-detect output): ctest
  `sde-sysreport-model`.

The new programs are system tools, not suite applications, so
`data/suite.tsv` is unchanged (its row count stays 11). Packaging: they
install with the main `sde` package through CMake; `pkexec` (polkitd) is
already needed by SDE Update.

## SDE Panel: proposals for the panel owner

### 1. Show desktop at the far end (patch, tested)

The panel already has a Show Desktop strip (`[panel] show_desktop_button`,
off by default, drawn *before* the Action Center button). Mint (and Windows)
put it in the screen corner, where it is an infinitely large target. The
patch moves it to the very end, makes it 12 px, turns it on by default, and
adds `sde-panel --show-desktop` (a plain `_NET_SHOWING_DESKTOP` request that
works without a running panel, for key bindings and launchers). It applied
cleanly on `ebf825d`, built, and was checked on Xvfb, then reverted from this
branch because `src/panel` belongs to the panel agent.

```diff
diff -ruN a/data/sde.conf b/data/sde.conf
--- a/data/sde.conf
+++ b/data/sde.conf
@@ -99,8 +99,8 @@
 autohide=false
 clock_24h=true
 show_date=true
-# A thin Show Desktop strip next to the Action Center button (Super+D works always).
-show_desktop_button=false
+# A thin Show Desktop strip at the far end of the panel (Super+D works always).
+show_desktop_button=true
 show_battery=true
 show_keyboard=true
 show_bluetooth=true
diff -ruN a/src/panel/main.cpp b/src/panel/main.cpp
--- a/src/panel/main.cpp
+++ b/src/panel/main.cpp
@@ -55,6 +55,12 @@
       std::string section = argv[++i];
       if (section != "overview" && !QuickSettings::valid_section(section)) return 2;
       command = "quick:" + section;
+    } else if (!strcmp(argv[i], "--show-desktop")) {
+      // A plain EWMH request: works without a running panel too.
+      fl_open_display();
+      sde::x11::set_showing_desktop(!sde::x11::showing_desktop());
+      XFlush(fl_display);
+      return 0;
     } else if (!strcmp(argv[i], "--safe")) {
       Panel::safe_mode = true;
     } else if (!strcmp(argv[i], "--calendar")) {
@@ -80,6 +86,7 @@
                      "                    and agenda\n"
                      "  --alarms          open or close the clock's alarms\n"
                      "  --new-alarm       open the alarms with a new alarm\n"
+                     "  --show-desktop    minimize every window, or bring them back\n"
                      "  --safe            start without tray icons and device status (after crashes)\n"));
       return strcmp(argv[i], "--help") ? 2 : 0;
     }
diff -ruN a/src/panel/panel.cpp b/src/panel/panel.cpp
--- a/src/panel/panel.cpp
+++ b/src/panel/panel.cpp
@@ -736,15 +736,16 @@
   start_w_ = H + S(46);
   pin_w_ = H;
 
-  // The Action Center has its own button in the corner of the panel; the
-  // optional Show Desktop strip sits just before it.
+  // The Show Desktop strip sits at the very end of the panel (an easy
+  // target in the screen corner, as in Linux Mint and Windows); the Action
+  // Center button comes just before it.
   int right = w();
+  show_desktop_w_ = cfg_.get_bool("panel", "show_desktop_button", true) ? S(12) : 0;
+  show_desktop_x_ = right - show_desktop_w_;
+  right = show_desktop_x_;
   action_w_ = H >= S(40) ? S(40) : S(34);
   action_x_ = right - action_w_ - S(4);
   right = action_x_ - S(2);
-  show_desktop_w_ = cfg_.get_bool("panel", "show_desktop_button", false) ? S(10) : 0;
-  show_desktop_x_ = right - show_desktop_w_;
-  right = show_desktop_x_ - (show_desktop_w_ ? S(4) : 0);
   text_font(theme_.font_size, false);
   bool show_date = cfg_.get_bool("panel", "show_date", true);
   bool two_lines = H >= S(40) && show_date;
@@ -1086,7 +1087,7 @@
     if (show_desktop_w_) {
       if (hover_ == Target{SHOW_DESKTOP, 0})
         p.fill_rect(show_desktop_x_, 0, show_desktop_w_, H, t.hover);
-      p.line(show_desktop_x_ + 0.5, S(10), show_desktop_x_ + 0.5, H - S(10), t.border);
+      p.line(show_desktop_x_ + S(2) + 0.5, S(10), show_desktop_x_ + S(2) + 0.5, H - S(10), t.border);
     }
 
     // The launcher being dragged follows the pointer.
```

Apply with `git apply` (save the block as a file). A tooltip "Show Desktop"
on hover would complete it. If the default should stay off, drop the
`data/sde.conf` hunk and keep `false` as fallback: SDE Welcome's First Steps
switch turns it on per user either way.

### 2. Workspace switcher (optional applet)

Mint's panel shows a small workspace switcher. Sketch for the panel:

* `[panel] show_workspaces=false` (opt-in), placed after the task buttons.
* Read `_NET_NUMBER_OF_DESKTOPS` and `_NET_CURRENT_DESKTOP` on the root
  window (`x11::get_cardinals`) and redraw on their `PropertyNotify` (the
  panel already watches root properties for `_NET_CLIENT_LIST`).
* Draw one `S(22)` x `H - S(16)` rounded cell per desktop: accent fill for the
  current one, a dot per window whose `_NET_WM_DESKTOP` is that desktop.
* Click: client message `_NET_CURRENT_DESKTOP` (data.l[0] = index, l[1] =
  timestamp); scroll wheel: previous/next. Openbox already provides the
  desktops (`data/openbox/rc.xml` sets 2).
* Hit test: a new `WORKSPACES` target next to `SHOW_DESKTOP`.

### 3. Hot corners (Settings > Desktop)

* `sde.conf [hotcorners] top_left=none|show-desktop|menu|action-center|
  command:<argv>`, same for `top_right`, `bottom_left`, `bottom_right`,
  `delay=200` (ms, avoids accidental triggers).
* Implementation in `sde-desktop` (always running, owns the root area): one
  1x1 `InputOnly` override-redirect window per enabled corner of each monitor,
  kept above (`XRaiseWindow` on `_NET_CLIENT_LIST_STACKING` change), with
  `EnterWindowMask`; on enter start a timer of `delay` ms, fire if the pointer
  is still in the corner (`XQueryPointer`); fire again only after the pointer
  left. Actions map to existing commands: `sde-panel --show-desktop` (patch
  1), `sde-panel --menu`, `sde-panel --action-center`.
* Settings page: four choice rows (one per corner) and a delay slider, in a
  new `src/settings/hotcorners.cpp` with a one-line page registration.

### 4. Menu favorites and sounds (later)

Mint's menu has a Favorites column; SDE's Start menu has pinned apps, so a
"Favorites" section is a menu-model change (`src/panel/menu_model.cpp`:
`[menu] favorites=` desktop IDs, "Add to favorites" in the item context
menu). Event sounds (login, logout, plug/unplug) would go through
`canberra-gtk-play` when installed, with a switch in Settings > Sound.
