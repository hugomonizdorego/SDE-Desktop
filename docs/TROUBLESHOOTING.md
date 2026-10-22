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

### "Connected · Internet access", but Firefox cannot connect (alpha 50)

Report: SDE installed on top of Debian's *standard system* (netinst without a
desktop). The network flyout shows `Wired connection 1 — Connected · Internet
access`, `enp1s0` with `172.16.200.233/22`, gateway `172.16.200.1`, DNS
`10.10.2.11 | 10.10.2.12`, yet Firefox cannot open websites.

What the flyout proves: `Wired connection 1` is NetworkManager's automatic
profile, so NetworkManager (not ifupdown) manages `enp1s0`, has a DHCP lease,
a gateway and DNS servers, and its connectivity check passed ("Internet
access" = `Connectivity` 4, FULL). That check is **one plain HTTP request**
(`http://network-test.debian.org/nm`, from
`20-debion-connectivity.conf`), made by the NetworkManager daemon as root,
without any proxy, expecting the text `NetworkManager is online`. APT on a
netinst system also uses plain `http://deb.debian.org` by default. Firefox
instead uses **HTTPS** (port 443), its own certificate store, the desktop
proxy setting (default "Use system proxy settings", which on Linux reads
GSettings `org.gnome.system.proxy`), and may prefer IPv6. So the failure is
in something the check does not exercise. Ranked for this situation (private
`172.16.0.0/12` address, internal `10.10.x.x` resolvers: an organization's
network):

1. **The network requires a proxy (most likely).** Corporate, school and
   government networks commonly pass port 80 through a *transparent* proxy
   (so NetworkManager's check and APT work) and block direct HTTPS, expecting
   browsers to use the proxy, usually announced by WPAD
   (`http://wpad.<domain>/wpad.dat`, or DHCP option 252). Windows and GNOME
   machines pick it up from their system proxy settings; SDE had no proxy
   setting at all, so Firefox's "Use system proxy settings" meant *direct* and
   HTTPS timed out. If the Debian installer asked for "HTTP proxy information"
   and it was filled in, APT has it (`/etc/apt/apt.conf`), the desktop does
   not.
2. **HTTPS inspection with a private certificate authority.** The firewall
   replaces certificates; Firefox shows a certificate warning (or refuses
   HSTS sites outright). Plain HTTP checks pass.
3. **Wrong clock.** A standard system without working NTP (NTP blocked on
   the network, `systemd-timesyncd` missing, an RTC kept in local time by
   Windows) makes every certificate look not yet valid or expired. HTTP works,
   HTTPS fails.
4. **IPv6 announced but not routed.** Router advertisements give a global
   IPv6 address and default route, the uplink does not carry IPv6; programs
   that try IPv6 first wait for timeouts.
5. **Path MTU black hole** (VPN or tunnel on the way): the small HTTP check
   passes, TLS handshakes with large certificate chains stall.
6. **DNS differences**: Firefox's DNS over HTTPS (`network.trr.mode` 2/3)
   blocked by the network, or `/etc/resolv.conf` unreadable by users
   (NetworkManager, running as root, still resolves).
7. **Two network managers on one NIC** (less likely here, since
   NetworkManager manages `enp1s0`): an `allow-hotplug`/`inet dhcp` stanza
   left in `/etc/network/interfaces` or `interfaces.d`, a stray `dhclient`
   or `dhcpcd`, or `systemd-networkd`, giving two leases, competing routes
   and a `resolv.conf` rewritten behind NetworkManager's back.

Firefox's AppArmor/sandbox, missing `ca-certificates` (Firefox ships its own
roots) and `nsswitch.conf` are unlikely causes of "cannot connect" with every
site; `debion-network-check` checks them anyway.

#### Confirming on the affected machine

Run as the desktop user (not with sudo), in this order:

```sh
debion-network-check                       # does all of the following and explains it
nmcli -t -f STATE,CONNECTIVITY general     # connected:full = the HTTP check passed
nmcli device show enp1s0 | grep -E 'IP4|IP6|DHCP4.OPTION.*wpad'
ip route; ip -6 route                      # one default route? an IPv6 default route?
cat /etc/resolv.conf; ls -l /etc/resolv.conf
getent ahosts deb.debian.org               # DNS as programs see it
curl -sv  --noproxy '*' -o /dev/null http://deb.debian.org/debian/   # works = HTTP not blocked
curl -sv  --noproxy '*' -o /dev/null https://deb.debian.org/debian/  # timeout = proxy needed (1),
                                           #   "certificate" error = (2) or (3)
curl -4 -sS -o /dev/null https://deb.debian.org/ && echo v4 ok
curl -6 -sS -o /dev/null https://deb.debian.org/ && echo v6 ok   # v4 ok, v6 fails = (4)
curl -s http://wpad/wpad.dat | head        # a FindProxyForURL() function = WPAD proxy (1)
debion-network-check --wpad                # same, for every search domain and DHCP option 252
apt-config dump | grep -i proxy            # proxy given to the installer (1)
env | grep -i proxy; gsettings get org.gnome.system.proxy mode
timedatectl                                # "System clock synchronized: yes"? (3)
ps -eo pid,args | grep -E 'dhclient|dhcpcd' | grep -v grep   # anything not NetworkManager's? (7)
```

In Firefox: `about:networking#dns` (resolution and TRR), `about:preferences`
→ Network Settings (proxy mode), and the error page's "Advanced" button
(certificate issuer for 2, `SEC_ERROR_OCSP_FUTURE_RESPONSE` /
"clock" for 3).

#### Fixing it

* **Proxy (1):** Settings → Network shows the verdict of
  `debion-network-check --quick --json` in *Troubleshooting*; when the network
  publishes a WPAD file, or APT has a proxy, "Use Proxy" applies it. Otherwise
  choose *Proxy*: Automatic (detect), Automatic configuration URL or Manual
  (`host:port`), then Apply. Quick fix inside Firefox alone: Settings →
  Network Settings → "Auto-detect proxy settings for this network".
* **Certificates (2):** import the organization's CA in Firefox (Settings →
  Privacy & Security → Certificates → View Certificates → Authorities →
  Import) and system-wide with
  `sudo cp ca.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates`.
* **Clock (3):** `sudo debion-network-check --fix` sets the time once from
  the HTTP `Date` of deb.debian.org and turns on `systemd-timesyncd`; ask for
  the internal NTP server where public NTP is blocked
  (`/etc/systemd/timesyncd.conf`, `NTP=`).
* **IPv6 (4):** `nmcli connection modify "Wired connection 1" ipv6.method disabled`
  and reconnect.
* **Two DHCP clients (7):** `sudo debion-network-check --fix`.

### Diagnosis (alpha 15)

SDE itself does not change networking: `startsde` and `sde-session` export no
proxy variables and set no GSettings proxy (since alpha 50 only the proxy the
user chooses in Settings → Network), SDE starts no `nm-applet` or other
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

  Since alpha 50 `--fix` backs every file it edits up to
  `/var/backups/debion-network/<date>/` (never next to the file: ifupdown
  sources everything in `interfaces.d`), comments out the stanzas of
  interfaces configured *only* with DHCP/SLAAC or a `wpa-ssid`/`wpa-psk`
  network (imported as a NetworkManager profile, mode 600), takes them down
  with `ifdown` *before* editing (so ifupdown releases its lease) and
  restarts NetworkManager. Over SSH it leaves the runtime handover for the
  next boot unless `--now` is given. It also replaces a dangling, empty,
  systemd-resolved-without-resolved or dhclient-written `/etc/resolv.conf`
  with one written by NetworkManager, makes it world-readable, turns on
  network time and corrects a clock that is more than five minutes off.
  Static, bridge, bond, VLAN, `mapping`, hook (`up`/`post-up`...), `wpa-conf`
  and `wpa-roam` configurations (servers, virtualization hosts) are never
  touched.

### Automatic handover on installation (alpha 50)

`debion-desktop-base`'s postinst runs `debion-network-check --handover` once
per system (first installation, or the first upgrade to alpha 50; a dpkg
trigger on `/usr/sbin/NetworkManager` retries when NetworkManager is
unpacked later). It converts the interfaces described above and nothing
else, and changes nothing that is running: until the next boot ifupdown
keeps its lease and `/run/NetworkManager/conf.d/90-debion-handover.conf`
keeps NetworkManager away from those interfaces even if it is restarted, so
there are never two DHCP clients and an SSH session survives. After the
reboot only NetworkManager configures them. `sudo debion-network-check --fix`
finishes the handover immediately. Opt out before installing with
`sudo mkdir -p /etc/debion && sudo touch /etc/debion/keep-ifupdown`; undo by
copying the backup from `/var/backups/debion-network/` back.

### Proxy settings (alpha 50)

Settings → Network → Proxy writes GNOME's `org.gnome.system.proxy`
(mode `none`/`auto`/`manual`, `autoconfig-url`, `http`/`https` host and port,
`ignore-hosts`), which Firefox, GTK/GIO and libproxy read immediately, and
`~/.config/environment.d/90-sde-proxy.conf` (`http_proxy`, `https_proxy`,
`ftp_proxy`, `no_proxy` and upper-case forms; `auto_proxy` for a PAC URL,
used by Chromium), which `startsde` exports and passes to D-Bus/systemd at the
next login. `startsde` also sets `GIO_USE_PROXY_RESOLVER=gnome` when
glib-networking's GNOME proxy module is installed, so GIO programs follow
GSettings outside GNOME. "Set for APT" writes
`/etc/apt/apt.conf.d/80debion-proxy` through
`pkexec debion-network-check --apt-proxy URL` ("Clear for APT" removes it).
"Automatic (detect)" looks up the network's WPAD file
(`debion-network-check --wpad`: DHCP option 252, then
`http://wpad.<search domain>/wpad.dat` up the domain, never
`wpad.<public suffix>`) and stores its URL, because not every program does
WPAD itself.

### `debion-network-check --quick --json` (for the panel)

One flat JSON object on standard output, in at most about 15 s (5 s
timeouts); exit status 0:

```json
{"version":2,"verdict":"proxy-required","nm":"connected:full","default_route":true,
 "dns":true,"http_direct":0,"https_direct":28,"https_proxy":null,"proxy_mode":"none",
 "proxy_url":"","wpad_url":"http://wpad.corp.example/wpad.dat","apt_proxy":"",
 "clock_skew":null}
```

`verdict` is `ok`, `no-route`, `no-dns`, `proxy-required`, `clock`,
`tls-intercepted`, `offline` or `unknown` (curl missing); `*_direct` and
`https_proxy` are curl exit codes (0 = success, 28 timeout, 7 refused, 60
certificate); `https_proxy` is the HTTPS result through the desktop or
environment proxy (`null` = none configured). The panel can show "Internet
access" only when NetworkManager says FULL *and* `verdict` is `ok` (run it
when the primary connection changes, not on a timer), and "Network needs a
proxy" for `proxy-required`, linking to `sde-settings --page network`.

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
