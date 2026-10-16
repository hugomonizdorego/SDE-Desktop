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
