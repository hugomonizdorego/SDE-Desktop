# SDE 0.1 Alpha 8

SDE keeps its EDE-inspired component structure, C++/FLTK interface and Rust
configuration/autostart core. Liria follows desktop build `0.1.0~alpha8`;
Display Settings has its own `1.0.0~alpha1` build.

## Application placement

The Start Menu opens All applications with populated standard XDG categories.
There is no Suite category. Opening scans application directories; while open,
the menu refreshes every two seconds and preserves the category/query/selection.
New package and user `.desktop` entries use the same category rules, with XDG
precedence, Hidden/NoDisplay/OnlyShowIn/TryExec filtering. Unknown categories go
to Other. Uninstalled entries disappear without restarting the session.

| Category | Native applications |
| --- | --- |
| Accessories | Calculator, Niny, Taca, Run Command |
| Office | Xanara |
| Graphics | SDE View |
| System | Liria, SDE System Monitor, Log Out |
| Settings | Display Settings, SDE Settings, SDE Power Manager, SDE Screen Lock, SDE Update |

Alpha7's automatic Desktop/SDE Suite launchers stop being generated. Upgrade
cleanup removes only byte-identical copies in real directories, preserving
edited files, other files and symlinked directories. Add to Desktop remains an
explicit menu action. Settings → Applications retains version/source/copyright
credits. `sde-suite` stays as an internal compatibility/diagnostic command.

## Display Settings

[DisplayProfileManager](https://github.com/zac15987/DisplayProfileManager) uses
Windows .NET/WPF. Its MIT Profile/DisplaySetting model and unique profile naming
are ported to C++. Native Linux hardware operations use libXrandr and `xrandr`,
with a redesigned FLTK frontend. No Windows executable, Wine or .NET is required.
The full pinned source, port and original MIT notice ship with corresponding
sources; notices are installed under `/usr/share/doc/sde/upstream/DisplayProfileManager/`.

- EDID model/identity, physical size, connector, current/preferred resolution,
  refresh rates and supported rotations come from RandR. Invalid/missing EDID
  falls back to connector identity and is labelled in the UI.
- Select/drag monitors with edge snapping, enter exact pixel positions, choose
  main display, enable/disable, extend, mirror using a common resolution,
  built-in-only or external-only drafts. Identify shows numbered overlays.
- Detect probes hardware explicitly. A two-second check while the UI is open
  reloads changed connections/modes. Incompatible modes/profiles are rejected;
  a unique EDID permits a monitor to move to a renamed dock connector.
- Save/load/delete profiles under `~/.config/sde/display-profiles/`. Loading makes
  a draft; Apply is explicit. The last confirmed layout restores on login only
  with matching hardware and modes. Alpha7 layouts migrate through a strict
  whitelist and current-hardware validation, without executing raw saved commands.
- Apply starts an independent worker before changing hardware. Revert, the
  15-second confirmation timeout, parent death or a hung-UI deadline restores
  the old layout. Expired previews cannot later be saved. Changed hardware has
  a supported single-display fallback; GPU/driver/cable failures still need real
  hardware testing and can require manual recovery.
- Scale offers 100%, 125%, 150%, 175% and 200%. It changes SDE geometry and shared
  GTK text DPI through GTK3/4 settings and XSETTINGS, preserving pixel resolution.
  This X11 implementation uses global scale, not independent per-monitor
  fractional scaling. Existing applications may need reopening. Scale commits
  only after keeping the display preview.

Windows HDR, audio switching, hotkeys and `.dpm` import are outside this Linux
alpha. A Wayland display backend is future work. Controls scroll at small sizes.

## Validation and installation

The Debian testing workflow builds all pinned native sources, tests the C++,
Python and Rust code and Calculator, installs the binary package through APT,
checks ELF identities/versions, simulates the desktop metapackage transaction,
and captures real windows with Xvfb at 1366×768/100% and 1024×600/150%.
Display tests cover EDID, modes/rates/rotation, profiles, redocking, migration and
independent Keep/Revert/timeout/parent-death rollback. GUI checks exercise a live
application install, Display Settings confirmation and profile persistence.
Xvfb does not certify physical GPUs, docks or multi-monitor hardware.

Download matching packages and complete sources from the successful alpha8
`sde-debian-testing` workflow artifact. On DebION / Debian testing amd64:

```sh
sudo apt install ./sde_0.1.0~alpha8_amd64.deb ./sde-desktop_0.1.0~alpha8_all.deb
```

APT resolves desktop services and shared libraries. Native SDE executables are
included in `sde`; original MATE application packages are not dependencies.
