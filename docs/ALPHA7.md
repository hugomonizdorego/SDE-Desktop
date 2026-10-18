# SDE alpha7 development build

This is an alpha patch for DebION GNU/Linux / Debian testing, amd64. The EDE
inspired component structure is retained: session, panel, desktop, settings and
small helpers remain in their existing directories; the FLTK shell and Rust
configuration/autostart core are kept. The original EDE provenance and licenses
under `bootstrap/` and `upstream/` are unchanged.

## Calculator

`calculator` is a compiled GTK4 executable. Its calculation engine is the
unchanged Scientific Calculator engine from the commit pinned in
`suite/components.lock.json`. JavaScriptCore runs
the engine directly; there is no browser extension host, WebKit view, Electron
or network dependency. Calculator defaults to Cairo software rendering for
lightweight X11 and virtual displays; an explicit `GSK_RENDERER` is respected.
The frontend supports expressions, precedence, brackets,
scientific functions, degrees/radians/gradians, memory, answer reuse, clipboard
copy and ten persisted history entries. Browser graph, solver and conversion
panels are not part of this first native alpha frontend.

Examples: `calculator --eval 'sin(30)+2^3'`,
`calculator --eval 'cos(pi)' --angle rad`, `calculator --version`.

## Placement and Start Menu

The Start Menu opens on an ordered SDE Suite tile page, with searchable results
and the standard XDG categories. Category and application lists scroll
independently, including on small displays. The footer opens Files, Settings
and session actions. Existing panel pins and user launchers are preserved.

On first login SDE adds missing launchers to `Desktop/SDE Suite/`, under
Accessories, Office, Graphics, System and Settings (or the desktop directory
configured by XDG user directories). It never replaces existing launchers.
This gives every suite application a stable home without filling the desktop
with loose shortcuts. The files also remain in their usual Start Menu category.

## Windows, compositor and GTK

`sde-wm` is built from pinned Openbox 3.6.1 source, with static private Openbox
libraries and SDE window-manager identity. `sde-compositor` is built from pinned
picom source. Neither original application package is needed. Existing SDE
Openbox rc.xml customization is retained; an old `window_manager=openbox` default
is migrated to the source-built SDE WM. Explicit custom WM commands are kept.

The compositor uses XRender, damage tracking and modest shadows; no blur,
animations or rounded window masks. It runs as an optional supervised session
component, and failure does not prevent the desktop from starting. Disable it
in Settings > Power & Updates > Compositing, or set `[compositor] mode=off` in
`sde.conf`; changes apply on the next login. A custom configuration can be
placed at `~/.config/sde/compositor.conf`.

GTK3 and GTK4 are supported together. Calculator uses GTK4; the mature MATE
derived suite applications still use GTK3. This is not a claim that all forks
have been ported to GTK4. SDE synchronizes theme, font and icons through separate
GTK3/GTK4 settings files, XSETTINGS, and the desktop color-scheme preference.
Unrelated settings.ini keys are preserved. Traditional icons that current
Adwaita no longer includes fall back to AdwaitaLegacy (or a complete installed
theme such as Papirus). A GTK portal preference is included
for SDE. No incompatible GTK_MODULES variable is exported across toolkits.

## Administrator Liria

Run `sudo liria [PATH]` from an authorized SDE X11 terminal. Sudo still prompts
according to the distribution's policy. The narrowly scoped sudoers setting
keeps DISPLAY and the existing XAUTHORITY only for `/usr/bin/liria`.

The native Liria executable starts its root process on a private D-Bus session,
uses root's home/preferences and leaves the user's D-Bus connection and files
alone. It does not grant X server access with xhost, install a setuid binary,
add a passwordless sudo rule or replace the SDE desktop. An active X11 session
and valid existing X authorization are required.

## Versions and packages

`suite/versions.json` records separate component build versions. Liria follows
SDE (`0.1.0~alpha7`); Calculator and the other suite applications start their own
`1.0.0~alpha1` series. Versions appear in native `--version`, the suite helper,
About dialogs and the Suite settings cards. Upstream ABI/library versions are
kept separate from product build labels.

Install the two release packages from the same artifact directory:

```sh
sudo apt install ./sde_0.1.0~alpha7_amd64.deb ./sde-desktop_0.1.0~alpha7_all.deb
```

The `sde` binary package contains the native suite, WM, compositor and resources.
`sde-desktop` supplies the complete session's system dependencies. Debug symbols
are optional. Use APT for dependency resolution; `dpkg -i` alone does not fetch
the required libraries and services. Full matching sources and SHA256 checksums
are included beside the binaries.

Hardware login, graphics drivers, PAM unlock and power policies still need
real DebION testing. Automated screenshots use an isolated Xvfb display.
