# SDE 0.1 Alpha 13 — panel crash fix, new tray, own ecosystem

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still
alpha: test it in a virtual machine or a spare installation first.

## Fixed: "The panel keeps stopping (Segmentation fault)"

With alpha12 the panel appeared for a moment after login, then crashed, five
times in a row, and SDE showed its rescue dialog. Reported in VirtualBox.

**Cause.** The panel asks system services over D-Bus for their state. When a
service does not answer, the call times out. On a machine without a Bluetooth
adapter (VirtualBox, most desktops), `bluetooth.service` does not start, so
the panel's first question to BlueZ timed out after 2.5 s. At that moment
libdbus removes and frees the call's timer from inside the timer callback,
but SDE's event loop then scheduled the freed timer again. It ran on freed
memory about 2.5 s later and the panel crashed. CI did not see it: its
containers have no system bus, so no call ever timed out.

**Fix.** SDE's D-Bus glue (`src/common/dbus.cpp`) keeps a list of the live
timers and watches and never touches one that libdbus has removed. A new test
(`tests/test_dbus_timeout.cpp`) makes calls to a service that never answers
and checks that they time out cleanly; it ran into the crash before the fix.

**Safety nets.**

- If the panel still crashes three times, sde-session starts it in **safe
  mode** (`sde-panel --safe`: no tray icons or device status) and shows a
  notification, instead of leaving the desktop without a panel.
- The panel, desktop, notifications and Settings now write a **backtrace** to
  `~/.local/state/sde/session.log` when they crash, for bug reports.
- Hovering over a tray icon could read past the end of the tooltip table;
  tooltips now come from a function that names the current state.

## New tray icons

All status icons were redrawn on a 24-unit grid with one stroke weight, and
each one shows the real state:

| Icon | Shows |
| --- | --- |
| Wi-Fi | signal strength in bars; cross when Wi-Fi is on but not connected |
| Wired | an RJ45 plug when connected by cable |
| Offline | a globe with a slash when there is no network |
| Volume | 0–3 waves, or a cross when muted |
| Battery | the charge level, a bolt while charging, red when low |
| Bluetooth | on/off; two dots when a device is connected |
| Keyboard | the layout code (US, ID, …) |
| Do Not Disturb | a moon while notifications are paused (click to resume) |

**Hardware icons appear and disappear with the hardware:** a USB-drive or
memory-card icon while one is plugged in (sde-panel listens to udev, so it
appears at once), Bluetooth when an adapter exists, the battery on laptops
and media controls while a player runs. Each tooltip tells the state, for
example "Wi-Fi · HomeNet" or "Battery 64% · charging".

## Action Center button in the corner

The **Action Center has its own button in the right corner of the panel**.
The clock opens the **calendar** (Super+C), and the Action Center opens with
Super+A. A thin Show Desktop strip is optional
(Settings → Panel → *Show Desktop button*); Super+D always works. The
separate power icon in the tray is off by default, because the Start menu and
the Action Center both have one.

## Own ecosystem

SDE already built its window manager (SDE WM, from Openbox) and compositor
(from picom) from pinned sources. Alpha 13 does the same for the remaining
desktop infrastructure it used from the distribution:

| SDE component | From | How |
| --- | --- | --- |
| SDE toolkit | FLTK 1.3.11 | built at configure time and linked **statically** into every SDE program |
| sde-xsettingsd | xsettingsd 1.0.2 | `/usr/bin/sde-xsettingsd`, serves SDE's GTK settings |
| sde-polkit-agent | mate-polkit 1.28.1 | `/usr/libexec/sde/sde-polkit-agent`, started by sde-session |

The `sde` package no longer depends on `libfltk1.3`, `xsettingsd`,
`policykit-1-gnome`, `mate-polkit` or `lxpolkit`. Exact revisions are in
`suite/components.lock.json`, sources are in `sde-native-sources_*.tar.xz`,
and licenses are in `/usr/share/doc/sde/upstream/`. Libraries that are part
of the operating system (glibc, X11, cairo, GTK, D-Bus) stay distribution
packages.

## Install for review

```sh
sha256sum -c SHA256SUMS --ignore-missing
sudo apt install ./sde_0.1.0~alpha13_amd64.deb ./sde-desktop_0.1.0~alpha13_all.deb \
    ./debion-desktop-base_0.1.0~alpha13_all.deb
```

Log out and choose **SDE-Desktop** at the login screen.
