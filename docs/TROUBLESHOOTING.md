# Troubleshooting

## Black screen after choosing SDE at the login screen

SDE 0.1.0~alpha2 and later show a dialog with the reason instead of a black
screen. In earlier builds, or if nothing appears at all:

1. Switch to a text console with **Ctrl+Alt+F3** and log in.
2. Run the system check:

   ```sh
   DISPLAY=:0 sde-session --check
   ```

   Every line must say `OK`. A `FAIL` with "error while loading shared libraries"
   or "not installed" means a dependency is missing.
3. Install the missing packages:

   ```sh
   sudo apt install --fix-broken
   sudo apt install --reinstall sde
   ```

4. Read the session log for details: `~/.local/state/sde/session.log`
   (the previous session is in `session.log.old`).

### Building a live ISO (Cubic, live-build)

Install the package with `apt`, not only with `dpkg -i`, so that its dependencies
come along:

```sh
apt install ./sde_0.1.0-alpha2_ubuntu24.04_amd64.deb
```

If you did use `dpkg -i`, run `apt install --fix-broken` in the chroot before
generating the ISO. Use the package built for the distribution of the ISO (for
Linux Mint 22 use the Ubuntu 24.04 package).

## No sound, network or brightness controls in quick settings

Quick settings use the system's services and hide what is not available:

| Section | Needs |
| --- | --- |
| Network | NetworkManager (`network-manager`, command `nmcli`) |
| Sound | PulseAudio or PipeWire with `pactl` (`pulseaudio-utils`) |
| Brightness | a backlight in `/sys/class/backlight` and systemd-logind |
| Battery | a battery in `/sys/class/power_supply` |

## Tray icons are missing

SDE shows both XEmbed icons and StatusNotifierItem/AppIndicator icons. Only one
program can own each tray; if another panel or tray runs in the session, SDE logs
"another system tray is running" in the session log.

## Display settings were not restored

Saved layouts are only applied when the same monitors are connected
(`[display] outputs=` in `~/.config/sde/sde.conf`). Remove the `[display]`
section to return to the automatic layout.

## Wi-Fi or Ethernet shows "connected" but there is no internet

### Diagnosis (alpha 15)

SDE itself does not change networking: `startsde` and `sde-session` export no
proxy variables and set no GSettings proxy, SDE starts no `nm-applet` or other
network daemon (it only hides duplicate tray icons), and it ships no polkit
rules for NetworkManager. The panel reads and controls NetworkManager through
`nmcli`. What it shows as "connected" is NetworkManager's *device* state
(associated with the access point / link up, address obtained), which says
nothing about reaching the internet. On DebION/Debian the usual causes are:

1. **Interfaces still configured in `/etc/network/interfaces`.** The Debian
   installer writes `allow-hotplug <nic>` / `iface <nic> inet dhcp` (and
   `wpa-ssid`/`wpa-psk` for Wi-Fi) when the system was installed without a
   desktop. Debian's `NetworkManager.conf` has `[ifupdown] managed=false`, so
   NetworkManager leaves those interfaces *unmanaged*: the panel cannot
   control them, and ifupdown's dhclient keeps a default route with metric 0
   (and rewrites `/etc/resolv.conf`). NetworkManager's routes use metric 100
   (Ethernet) / 600 (Wi-Fi), so after unplugging the cable or changing
   networks traffic still goes to the dead ifupdown route: Wi-Fi "connected",
   no internet. Running both on one interface is worse (two DHCP clients).
   Setting `managed=true` instead is *not* a fix while `networking.service`
   still brings the interface up at boot.
2. **Broken DNS.** `/etc/resolv.conf` is a symlink to
   `/run/systemd/resolve/stub-resolv.conf` while `systemd-resolved` is not
   installed or not running (common after removing it or restoring a
   backup), or it was emptied by a DHCP client. Pings to IP addresses work,
   names do not. NetworkManager writes a plain file by default; it needs no
   `resolvconf`, `openresolv` or `systemd-resolved`.
3. **No connectivity checking.** Debian's NetworkManager has no `[connectivity]
   uri` unless `network-manager-config-connectivity-debian` is installed, so
   its `Connectivity` property cannot report a captive portal or a network
   without internet (it stays "unknown", or is assumed "full" when there is
   a default route). Applets therefore cannot show "connected, no internet".
4. **A captive portal** (hotel/campus Wi-Fi): connected, but HTTP is
   redirected until you log in through a browser.
5. **A stale proxy** in `/etc/environment`, `/etc/profile.d`, APT or GNOME's
   `org.gnome.system.proxy` (set by another desktop): browsers and APT fail
   while `ping` works.

### What alpha 15 ships

* `debion-desktop-base` installs
  `/usr/lib/NetworkManager/conf.d/20-debion-connectivity.conf`, which enables
  NetworkManager connectivity checking against Debian's endpoint
  (`http://network-test.debian.org/nm`, the same as
  `network-manager-config-connectivity-debian`), every 300 s. Turn it off or
  change it with a file in `/etc/NetworkManager/conf.d/`
  (`[connectivity]` `enabled=false`). The postinst reloads NetworkManager's
  configuration (connections stay up).
* `debion-desktop-base` recommends `network-manager` (`sde-desktop` already
  depends on it).
* `/usr/sbin/debion-network-check` explains all of the above:

  ```sh
  debion-network-check          # report, any user
  sudo debion-network-check --fix
  ```

  `--fix` backs `/etc/network/interfaces` up to
  `/etc/network/interfaces.debion-<date>`, comments out the stanzas of
  interfaces configured *only* with DHCP/SLAAC/wpa_supplicant so that
  NetworkManager manages them, and replaces a dangling, empty or
  systemd-resolved-without-resolved `/etc/resolv.conf` with one written by
  NetworkManager. Static, bridge, bond and VLAN configurations (servers,
  virtualization hosts) are never touched. Reboot afterwards, or run
  `sudo ifdown --force <nic>; sudo systemctl restart NetworkManager`.
  On a first installation the package only prints a warning when it finds
  such interfaces; `/etc/network/interfaces` belongs to the administrator and
  is never edited without `--fix`.

### Checking by hand

```sh
nmcli general                       # STATE and CONNECTIVITY
nmcli networking connectivity check # forces a check: none, portal, limited, full, unknown
nmcli device                        # "unmanaged" = left to ifupdown
ip route show default               # more than one default route?
ls -l /etc/resolv.conf; cat /etc/resolv.conf
getent hosts deb.debian.org         # DNS works?
grep -ri proxy /etc/environment /etc/apt/apt.conf.d/ 2>/dev/null
```

`limited` usually means no default route or broken DNS, `portal` a captive
portal (open any http:// page in the browser).

### D-Bus properties for the panel's network flyout

The Action Center network flyout should use NetworkManager's D-Bus API
(service `org.freedesktop.NetworkManager`, object
`/org/freedesktop/NetworkManager`, interface `org.freedesktop.NetworkManager`)
instead of polling `nmcli`:

| Property / method | Type | Meaning |
| --- | --- | --- |
| `State` | u | 10 asleep, 20 disconnected, 30 disconnecting, 40 connecting, 50 connected (local), 60 connected (site), 70 connected (global) |
| `Connectivity` | u | 0 unknown, 1 none, 2 portal, 3 limited, 4 full |
| `ConnectivityCheckAvailable` | b | false when no `[connectivity] uri` is configured |
| `ConnectivityCheckEnabled` | b (rw) | the user/admin switch |
| `ConnectivityCheckUri` | s | the check URI (empty when unavailable) |
| `PrimaryConnection` | o | active connection carrying the default route |
| `CheckConnectivity()` | → u | forces a check (needs polkit `network-control`, allowed in an active local session) |

Changes arrive as `org.freedesktop.DBus.Properties.PropertiesChanged` on that
object. Show "No internet" only for `Connectivity` 1 or 3, "Sign in to the
network" for 2, and treat 0 (unknown, e.g. checking unavailable) as connected
without a warning; otherwise a system without the check reports a false "no
internet". `nmcli -t -f STATE,CONNECTIVITY general` gives the same values as
text (`connected:full`, `connected:limited`, `connected:portal`,
`connected:unknown`).
