# DebION GNU/Linux desktop profile

Install `sde-desktop` for the complete DebION SDE desktop. `sde` contains the
source-built native applications; `debion-desktop-defaults` contains distro
configuration. Supplemental applications retain their own upstream identities
and copyrights, and their installed desktop files place them in the appropriate
Start Menu directories. New applications are discovered automatically.

## Installation from the alpha artifact

Extract the ZIP, verify `sha256sum -c SHA256SUMS.sha256`, then run:

```sh
sudo sh ./install-debion.sh .
```

The directory must contain exactly one matching version of `sde`, `sde-desktop`
and `debion-desktop-defaults`. `--yes` is available for automated image builds.
The installer enables the signed official Mozilla APT source by installing the
defaults package, refreshes APT, then installs the complete profile using
`--no-install-recommends`. All requested applications are hard dependencies;
this option avoids unrelated recommended applications. The script never installs
the debug symbol package automatically. Existing APT sources are preserved.

DebION ISO builders should install the defaults package and refresh APT before
resolving the desktop metapackage. Do not use `dpkg -i` alone for a full desktop:
it does not fetch dependencies. Normal subsequent updates use APT / SDE Update.

## Complementary applications and defaults

| Use / menu | Default application |
| --- | --- |
| Web / Internet | Firefox release (`firefox`), official Mozilla DEB, not ESR |
| Mail / Internet | Thunderbird |
| Torrents / Internet | Transmission GTK |
| Text / Accessories | Niny (native) |
| Files / System | Liria (native, shares the SDE version) |
| PDF and PostScript / Office | Xanara (native) |
| Archives / Accessories | Taca (native) |
| Images / Graphics | SDE View (native); Gallery for photo browsing and organization |
| Video / Multimedia | Celluloid |
| Music and playlists / Multimedia | Rhythmbox |
| Terminal / System | GNOME Terminal, including Ctrl+Alt+T and SDE Update transactions |
| Calculator / Accessories | Calculator (native) |
| Characters / Accessories | Gucharmap |
| Fonts / Accessories | GNOME Font Viewer |
| Passwords and keys / Accessories | Seahorse with GNOME Keyring |
| Screenshot / Accessories | GNOME Screenshot; Print opens its interactive dialog |
| Scanner / Graphics | Simple Scan |
| On-screen keyboard / Accessories | Onboard (available on demand) |
| Local DEB files / System | GDebi |
| Package management / Settings | Synaptic; SDE Update (native) handles update previews |
| Partitions / System | GParted |
| Firewall / Settings | GUFW (installed; activation remains the user's decision) |
| Printers / Settings | system-config-printer and its udev integration |
| Bluetooth / Settings | Blueman and BlueZ, integrated with the SDE Action Center |
| Login screen / Settings | LightDM Settings, with Slick Greeter |

The menu uses each upstream launcher's XDG Categories; translated directory
names can differ. Defaults are shipped as `/etc/xdg/sde-mimeapps.list` and
`/etc/xdg/sde-xdg-terminals.list`. Personal MIME/terminal preferences take
precedence. GNOME Terminal upstream restricts its launcher to GNOME/Unity, so an SDE-only
compatibility launcher exposes it in System. There is one visible terminal
entry in SDE. No “SDE Suite” menu directory is created.

Papirus is the default in the SDE config, GTK3/GTK4, XSETTINGS and GNOME interface
settings, and in Slick Greeter. Explicit SDE icon choices remain configurable.

## Services

- NetworkManager with `nm-connection-editor`, `wpasupplicant` and `wireless-regdb`
  handles wired and Wi-Fi connections. The native panel already provides network
  controls, so a second NetworkManager tray applet is unnecessary.
- `systemd-resolved` provides per-link DNS, including VPN DNS. The NetworkManager
  config selects it explicitly; package-provided setup manages resolver integration.
  Existing `/etc/network/interfaces` connections are not rewritten or taken over.
- `pipewire`, `pipewire-alsa`, `pipewire-pulse` and WirePlumber supply audio.
  `libspa-0.2-bluetooth` supplies Bluetooth audio; `pactl` comes from
  `pulseaudio-utils`. The PulseAudio daemon is not part of the profile.
- `systemd-timesyncd` provides time synchronization. Installing it can replace a
  competing time daemon through Debian's normal dependency resolution.
- LightDM selects Slick Greeter and the SDE session on a fresh installation.
  No running display manager or network service is restarted by our scripts.
  Existing local configuration overrides follow normal LightDM/NM precedence.
- GameMode is installed for applications that request it. The profile does not
  force an always-on gaming mode or enable the on-screen keyboard at every login.

## Firefox and player package policy

Debian testing currently supplies Firefox ESR, so this profile supplies the
[official Mozilla APT source](https://support.mozilla.org/en-US/kb/install-firefox-linux).
Its public signing key is shipped locally with fingerprint
`35BAA0B33E9EB396F59CA838C0BA5CE6DC6315A3`, checked against Mozilla's instructions.
Only `firefox` and `firefox-l10n-*` are pinned to Mozilla (priority 1000).
Thunderbird stays a normal Debian package. Firefox ESR is never a fallback.

The `mpv` player has APT pin priority -1 and conflicts with the full desktop
metapackage. **`libmpv2` remains allowed and required by Celluloid**. No binaries
are deleted and no dummy mpv package is used. A pre-existing mpv installation
must be resolved by APT when choosing this profile; review its install plan.
Purging the desktop defaults removes its conffiles and repository/pin settings.
APT itself always honors an explicit administrator override.

## Source and development credits

About dialogs label SDE development **DebION Dev-team**, linking to
[SDE-Desktop](https://github.com/hugomonizdorego/SDE-Desktop). Upstream authors,
short clickable source names, original copyright and licenses are preserved.
Supplemental applications keep their upstream About dialogs and packaging.
