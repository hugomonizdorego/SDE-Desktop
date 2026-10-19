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

Alpha 12 intentionally hides a known system applet icon when a native SDE
indicator already covers its function. The applet still runs. Disable the
corresponding native indicator in Panel settings to show the provider's icon.
Ordinary application icons remain in the tray; when Action Center is open they
move there by default. Click the clock to open it. An IBus/Fcitx indicator takes
the place of the generic keyboard icon.

## Panel stops with signal 11

Alpha 12 fixes a reproducible crash caused by an applet exiting while its tray
menu was open. Update the matching desktop packages using the artifact installer
and log out/in. `apt install --fix-broken` resolves missing packages but does not
repair this memory-lifetime bug in older panel binaries.

For a further crash, include `sde-panel --version`, the actions immediately before
it stopped, and `~/.local/state/sde/session.log` in the report. The screenshot
alone does not identify every possible crash cause.

## Display settings were not restored

Saved layouts are only applied when the same monitors are connected
(`[display] outputs=` in `~/.config/sde/sde.conf`). Remove the `[display]`
section to return to the automatic layout.
