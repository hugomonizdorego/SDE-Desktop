# SDE 0.1 Alpha 9 — panel and Action Center

This is a development build for DebION GNU/Linux / Debian testing. It extends
Alpha 8's native Display Settings, category menu and source-built applications.
SDE and Liria share `0.1.0~alpha9`; other applications retain their own versions.

## Panel behavior

The full-edge panel uses compact symbolic indicators in the reference order:
tray expander, keyboard, Bluetooth, battery, power, media, network, sound,
controls, clock/date and the SDE Action Center button. Indicators without a
service or device are omitted. Each can be configured in Settings → Panel.

Action Center is aligned to the panel's right corner, touching its upper edge
for a bottom panel, or its lower edge for a top panel. One placement function
handles clock clicks, indicator clicks, shortcuts, live refresh and monitor
bounds. Narrow screens retain a scrollable body and fixed session footer.

With “Move status icons into Action Center” enabled (default), status indicators,
StatusNotifierItems and legacy XEmbed tray icons move into its status card. The
clock, launchers and taskbar stay on the panel. Closing, Escape, clicking outside
or changing focus returns the tray to the panel. XEmbed uses one persistent host:
selection ownership and embedded clients survive the move. XI2 outside-click
monitoring permits native child windows to receive pointer events. X servers
without XI2 use FLTK's legacy grab; native tray child interaction in that fallback
still needs hardware/server review. A crowded short panel exposes its tray via
the Action Center button instead of overlapping application buttons.

## Eduka behavior adapted to native SDE

Behavior reference: [eduka-panel](https://github.com/hugomonizdorego/Eduka-Desktop/blob/7a92f29f731576830e8c5bde79a34d4f20b6b774/usr/bin/eduka-panel),
Copyright © Hugo Moniz do Rego and Eduka contributors. The referenced repository
has no top-level license declaration. Its Python/Qt source and theme assets are
not bundled or relicensed here. SDE's C++ implementation is independent and
licensed with SDE under GPL-3.0-or-later; the short upstream link remains visible
in project credits. Existing upstream copyright notices remain intact.

| Eduka feature | SDE Alpha 9 implementation |
| --- | --- |
| Sound popup / application mixer | PipeWire-Pulse or PulseAudio output selection, mute, master and per-stream volume, microphone source/mute/volume |
| Media player integration | Session D-Bus MPRIS discovery, live metadata, previous/play/pause/next, raise player, installed media applications; respects player capabilities and owner changes |
| Bluetooth popup | BlueZ adapter selection, power, timed discovery, pair/connect/disconnect/remove; follows adapter and device signals |
| Wi-Fi and LAN | NetworkManager Wi-Fi list/scan/connect and radio toggle; wired connect/disconnect, IP, gateway and DNS details; advanced connection editor when installed |
| Wireless radios off | Saves and restores Wi-Fi, WWAN and powered Bluetooth adapter state; reports command failures |
| Compact tray / clock / footer | Movable native tray, clock/date, optional analog clock and greeting, calendar, settings, lock and session controls |
| Panel / Action Center preferences | SDE-specific indicator toggles, move-status toggle, media/mixer/microphone/tray cards, two or three tile columns, colorful tiles, existing full/floating/short/top/bottom panel settings |

SDE's default is a two-column layout with separate network/Bluetooth/sound/media
pages and its own configuration in `~/.config/sde/sde.conf`. Eduka's Qt profiles
are not written. Unrelated Eduka modules (weather, news, globe, printer, etc.) are
not part of this patch. They remain candidates for later alpha reviews.

## Services and alpha limits

Use the complete `sde-desktop` metapackage to install NetworkManager, BlueZ,
Blueman, PulseAudio tools and an audio server/session manager. The `sde` package
recommends these helpers; missing services show a clear unavailable state.
An SDE-only autostart entry starts Blueman's single-instance applet when
installed, providing the BlueZ pairing agent. Pairing with PIN/passkey
confirmation uses this agent; Bluetooth
Settings opens Blueman when confirmation is needed. SDE does not silently trust
a device or bypass authentication. Bluetooth discovery stops after 10 seconds,
when the selected adapter changes, or when Action Center closes.

Controls operate on existing system services with normal user permissions;
polkit/agent setup remains distribution-managed. Wireless-off is a software
radio toggle, not a claim that a physical airplane-mode switch has changed.
Real Bluetooth hardware, radio switches and multi-monitor hotplug still require
DebION hardware testing. Automated tests use isolated, explicitly labeled service
fixtures and are not hardware certification.

` sde-panel --section network|bluetooth|sound|media|tray|power ` opens a particular
page. `--action-center` / Super+A opens the overview; Super+C opens the calendar.

## Verification

The core build passes seven local CTest checks, including audio-stream/network
parsing, media matching and independent RandR preview rollback. The full Debian
build also includes the native Calculator test.

`tools/panel-smoke.py` runs the actual panel at 1366×768 / 100% and 1024×600 /
150%, using isolated QA services. It checks native XEmbed and SNI clicks after
transfer, return on Escape/outside clicks, clock opening, Bluetooth power,
pair/connect/disconnect and discovery cleanup, MPRIS capability gating,
master/microphone/per-stream volume and mute, disappearing adapters/services,
scrolling, top/bottom/full/floating/short placement and the move-status preference.
Both local configurations passed. The complete Debian push build also passed
all eight CTest checks, installation, Rust checks and both native GUI checks.
The preview test keeps Xvfb alive between independent client probes to avoid
a server reset race; this fixture change passed 12 local repetitions.
QA helper executables are not installed in
the desktop package.

The [Debian CI run](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39142810949)
builds the complete source-based suite and publishes `.deb` packages, matching
sources, checksums and native screenshots. Install the two runtime packages with
APT so Debian resolves system services:

```sh
sudo apt install ./sde_0.1.0~alpha9_amd64.deb ./sde-desktop_0.1.0~alpha9_all.deb
```

The optional `sde-dbgsym` package is only needed for debugging.
