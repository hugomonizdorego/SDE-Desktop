# SDE applications for DebION GNU/Linux

Alpha6 builds eight native application forks from pinned upstream source. Their
ELF executables, libraries, resources, schemas and helpers are included in the
single `sde` binary package. Installing the original MATE application packages
is not required. Shared libraries such as GTK and libmate-desktop, together with
their settings data in `mate-desktop-common`, are normal Debian dependencies;
they are not duplicated inside SDE.

| Application | Executable | Source |
| --- | --- | --- |
| Niny | `niny` | native SDE application (DebION Dev-Team) |
| Liria | `liria` | [caja](https://github.com/mate-desktop/caja) |
| Xanara | `xanara` | native SDE application (DebION Dev-Team) |
| Taca | `taca` | native SDE application (DebION Dev-Team) |
| Photo | `sde-view` | native SDE application (DebION Dev-Team) |
| SDE System Monitor | `sde-system-monitor` | native SDE application (DebION Dev-Team) |
| SDE Power Manager | `sde-power-manager`, `sde-power-preferences`, `sde-power-statistics` | [mate-power-manager](https://github.com/mate-desktop/mate-power-manager) |
| SDE Screen Lock | `sde-screensaver`, `sde-screensaver-command`, `sde-screensaver-preferences` | [mate-screensaver](https://github.com/mate-desktop/mate-screensaver) |
| Calculator | `calculator` | [scientific-calculator](https://github.com/raad-altaie/scientific-calculator) engine, native GTK4 frontend |
| Display Settings | `sde-display-settings` | [DisplayProfileManager](https://github.com/zac15987/DisplayProfileManager) profile model, native Linux frontend |
| SDE Update | `sde-update` | [mintupdate](https://github.com/linuxmint/mintupdate) model and Debian PackageKit |

These GTK forks retain their upstream C/C++ implementation. SDE's configuration,
panel geometry, autostart and suite diagnostics use Rust; the shell and update
interface use C++/FLTK. This release does not claim to rewrite the suite in Rust.

## Native forks

`suite/forks.lock.json` records the exact revision compiled for each fork.
`tools/prepare-native-suite.py` fetches those commits and their pinned submodules,
preserves licenses and copyright comments, and prepares editable downstream
source in `vendor/suite`. Application identifiers, schemas, D-Bus names, symbols,
resource paths and executable names are changed together. Shared MATE session
interfaces remain external system interfaces. Taca's file-manager extension
builds against the bundled Liria extension API.

Liria defaults to `--no-desktop`, including direct invocation: SDE retains its
wallpaper and desktop icons. Power and screen lock have SDE-only autostart entries.
The locker uses Debian's common PAM policy and the independent
`org.debion.session` idle-delay setting; it does not require MATE Session Manager.
Actual locking, suspend, lid and
battery behavior require testing in a real DebION login session.

New GTK processes follow SDE light/dark settings, respecting an existing
`GTK_THEME` or the system-theme option. SDE-specific MIME defaults preserve user
overrides. Xanara (native since alpha15) opens, annotates and fills in PDF files with
poppler; page operations use qpdf when it is installed.

The panel defaults to a full-width square-edged rectangle. Floating and short
styles remain available. Existing saved panel choices are retained. Settings → Applications shows short clickable source names and complete installed copyright
notices under `/usr/share/doc/sde/upstream/<upstream>/`.

## Installation

The supported full-suite target is **DebION GNU/Linux / Debian testing**.
DebION image builders should publish `sde` and `sde-desktop` in their own signed
APT repository and seed `sde-desktop` in the installer:

```sh
sudo apt install sde-desktop
```

`sde` contains the native suite. `sde-desktop` adds Xorg, login, network, audio,
removable-media support, icons and GSettings. These are operating-system
services required for a complete DE. This project does not configure an APT
repository. For local packages, let APT resolve dependencies:

```sh
sudo apt install ./dist/sde_*.deb ./dist/sde-desktop_*.deb
```

Select SDE at the login screen. Debug-symbol packages are optional and are not
needed for normal installation. `dpkg -i` alone does not fetch missing libraries
or system services; use `sudo apt --fix-broken install` to repair that state.

## SDE Update

SDE Update 2 is a native FLTK window with six pages, drawn with the SDE theme
family (SDE, Aero and Liquid Glass, light and dark) at every UI scale:

- **Updates**: status header (up to date / N updates, security count, download
  size, last refresh), Refresh and Install buttons, a reboot-required banner,
  search, type filters and a list grouped into Security, Kernel, Software and
  Backports with check boxes. The details pane shows versions, packages, origin
  and the changelog (`apt-get changelog`, PackageKit `get-update-detail`, then
  the installed changelog), loaded in the background with a time limit.
- **History** from `/var/log/apt/history.log*`, **Sources** (read-only one-line
  and deb822 entries, Software Sources when installed), **Maintenance** (refresh,
  clean cache, remove unused packages after review, repair, terminal update),
  **Settings** (`[update]` in sde.conf) and **About** with the mintupdate credits.

The read-only Python APT provider (`data/update/check.py`, protocol
`SDE-UPDATES 2`) uses the `Update` class adapted from Mint Update: candidates
are grouped by source, held packages are excluded and counted, Debian backports
get their own type. Mint repositories and Mint-only policies are not imported.

Transactions run in `transaction.py`, which streams progress (status, percent,
per-package items) to the window. It uses `pkcon` when PackageKit is running,
so polkit authorizes and the user can cancel; otherwise, and for cache cleaning
and autoremove, `apt-get` runs through `pkexec`. Names are validated and no
options or repository URLs are accepted. `sde-update --notifier` (autostarted)
checks in the background and notifies; nothing is installed automatically.
`--check`, `--history` and `--sources` print the helper protocols; `--plan`
prints a transaction's argument vector without changing the system. Debian
testing transitions may still require manual APT decisions.

## Sources and validation

See [BUILD.md](BUILD.md) for the source preparation and offline build workflow.
Every binary artifact is accompanied by a complete source archive containing
all prepared trees, submodules, build scripts and preserved licensing. Original
sources can be inspected alongside the downstream changes using the locked Git
revisions. Fork maintainers must review and import future upstream security
fixes; these private forks do not receive fixes merely by updating the original
MATE application packages.

CI builds and installs on Debian testing, checks actual native ELF files,
rejects original MATE application packages, checks MIME overrides and copyright
notices, and opens the five file-handling applications with sample files on X11.
Network, audio and APT screenshot fixtures are explicitly simulated. No test
performs a real update, lock, suspend or shutdown. Current alpha8 menu/display captures are under
`docs/screenshots/alpha8`; CI uploads the complete capture set, including Calculator
and the isolated administrator Liria window. Older captures remain as release history.
Hardware login, power, locking and whole-desktop idle memory remain to be
validated on DebION. See [THIRD-PARTY.md](THIRD-PARTY.md) for provenance.

LXQt, Budgie and COSMIC remain development references rather than extra desktop
shells shipped in SDE. `upstream/sources.lock.json` and `tools/suite-source.py`
prepare those original reference trees independently of the actual build lock.

See [alpha8 notes](ALPHA8.md) for current category placement and Display Settings.
