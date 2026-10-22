# SDE 0.1 Alpha 51 — DNS fix, lock screen, Software Updater, calmer look

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still
alpha: test it in a virtual machine or a spare installation first. The
developer keeps monitoring reports and developing SDE.

## Internet: DNS repaired and verified

In Alpha 50, `sudo debion-network-check --fix` emptied `/etc/resolv.conf`,
restarted NetworkManager and reported success without checking. On many
Debian installs the file then stayed empty (NetworkManager set to `dns=none`
or handing servers to an unused `resolvconf`, a stopped systemd-resolved, an
immutable file, `ignore-auto-dns`, an interface still run by ifupdown, or a
DHCP lease without DNS). `connected:full` is misleading in that case:
NetworkManager's own check asks the DNS servers directly, programs such as
Firefox read `/etc/resolv.conf`.

`--fix` now diagnoses all of these, repairs them (writing
`/etc/NetworkManager/conf.d/90-debion-dns.conf` only when needed), waits and
**verifies** that `deb.debian.org` resolves, and prints the outcome. If the
network gives no DNS server at all:

```sh
sudo debion-network-check --dns-fallback       # 1.1.1.1 and 9.9.9.9 on the active connection
sudo debion-network-check --dns-fallback off   # undo
```

Settings → Network → Troubleshooting shows **Use Public DNS** when DNS is the
problem. The package also repairs an empty `resolv.conf` at install time.

## Lock screen always takes the keyboard

The lock screen could appear but ignore mouse and keyboard, forcing a power
off. Fixes: the input method (IBus/Fcitx) is not used on the lock screen; the
keyboard and pointer grabs are retried until they succeed and re-checked
every second (panel popups and stuck menus are closed first); the lock
re-raises itself, follows monitor changes after resume, abandons a PAM check
that hangs; a lock screen that stops responding is replaced, and after
repeated failures the daemon shows its own simple password prompt.

## Panel

- **Window previews:** hover a taskbar button to see a thumbnail of the
  window above the panel, also for minimized windows; click to switch, ×
  to close.
- **Compact popups:** every flyout and the Action Center are smaller, with
  text rows instead of big cards and soft corners.
- **Battery flyout** like Linux Mint: percentage and battery model, time
  remaining, brightness slider, Power Saver / Balanced / Performance, Power
  Settings.
- **Clock → Time zone:** Sync detects the time zone from the network, sets
  it and resynchronises the clock; optional automatic time zone.

## Software Updater

SDE Update is now **Software Updater**: updates grouped as **Critical**
(kernel, libc, systemd, apt/dpkg, firmware, anything needing a restart),
**Security**, **Main** and **Flatpak**, with check boxes and sizes, a
technical description with changelog, "N MB will be downloaded",
Settings… / Remind Me Later / Install Now, progress in the same window and a
restart notice when needed. Colours follow the theme.

## Calmer look

- New low-chroma palettes for Graphite (neutral grey), Fluent (slate) and
  Glass (warm stone) with muted accents; no glow, no colourful gradients.
- Soft corners everywhere: windows, menus, popups, dialogs, buttons,
  fields, scroll bars, GTK 3/4 and libadwaita.
- Window buttons in the style of Linux Mint: minimise, maximise and a soft
  round close button on the right; close turns red only on hover.
- All SDE programs, GTK and Qt applications follow the theme colours.

## Welcome and Driver Manager

- **Driver Manager** in the style of Linux Mint's, more minimal: one row per
  device with driver choices, Revert and Apply Changes.
- **Welcome** is one small window with six shortcuts. It is **not** shown
  on a live boot; after installation it appears once on each user's first
  login (and again only if "Show this dialog at startup" is ticked).
- **Start Menu:** "Log Out" and other helper entries no longer appear in
  the application list (log out from the Action Center).

## Recommendations for the next patches

1. **Test on real hardware:** lock/resume with monitors, touchpad, battery
   flyout, taskbar previews with the compositor, Driver Manager installs.
2. **Translations** (Indonesian first).
3. **Snapshots** (Timeshift-like) in Welcome and Software Updater.
4. **Software center** for installing applications (APT + Flatpak).
5. **Signed APT repository** for DebION.

## Install for review

```sh
sha256sum -c SHA256SUMS
sudo apt install ./sde_0.1.0~alpha51_amd64.deb ./sde-desktop_0.1.0~alpha51_all.deb \
    ./debion-desktop-base_0.1.0~alpha51_all.deb
sudo debion-network-check --fix     # if websites do not open
```

Log out and choose **SDE-Desktop** at the login screen. If something
crashes, send `~/.local/state/sde/session.log` (or use System Reports).
