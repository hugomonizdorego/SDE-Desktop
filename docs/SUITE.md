# SDE Suite for DebION GNU/Linux

Alpha6 builds eight native application forks from pinned upstream source. Their
ELF executables, libraries, resources, schemas and helpers are included in the
single `sde` binary package. Installing the original MATE application packages
is not required. Shared libraries such as GTK and libmate-desktop, together with
their settings data in `mate-desktop-common`, are normal Debian dependencies;
they are not duplicated inside SDE.

| Application | Executable | Source |
| --- | --- | --- |
| Niny | `niny` | [pluma](https://github.com/mate-desktop/pluma) |
| Liria | `liria` | [caja](https://github.com/mate-desktop/caja) |
| Xanara | `xanara` | [atril](https://github.com/mate-desktop/atril) |
| Taca | `taca` | [engrampa](https://github.com/mate-desktop/engrampa) |
| SDE View | `sde-view` | [eom](https://github.com/mate-desktop/eom) |
| SDE System Monitor | `sde-system-monitor` | [mate-system-monitor](https://github.com/mate-desktop/mate-system-monitor) |
| SDE Power Manager | `sde-power-manager`, `sde-power-preferences`, `sde-power-statistics` | [mate-power-manager](https://github.com/mate-desktop/mate-power-manager) |
| SDE Screen Lock | `sde-screensaver`, `sde-screensaver-command`, `sde-screensaver-preferences` | [mate-screensaver](https://github.com/mate-desktop/mate-screensaver) |
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
overrides. Xanara supports PDF, PostScript, DjVu and comic archives; the EPUB,
DVI and XPS backends are disabled to avoid extra runtime stacks in this build.

The panel defaults to a full-width square-edged rectangle. Floating and short
styles remain available. Existing saved panel choices are retained. Settings →
SDE Suite shows short clickable source names and complete installed copyright
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

The native FLTK interface uses a read-only Python APT provider adapted from Mint
Update's `Update` class. It groups candidates by source and excludes held
packages. Mint repositories and Mint-only policies are not imported.

Check Again reads the local cache. Refresh Sources and Install Updates open
interactive `pkgcli` or `pkcon` transactions in a terminal; PackageKit computes
the transaction and polkit authorizes it. SDE does not run unattended upgrades.
`--check` prints the preview protocol; `--plan refresh|upgrade` prints the fixed
argument vector without changing the system. Debian testing transitions may
still require manual APT decisions.

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
performs a real update, lock, suspend or shutdown. Native alpha6 captures of Niny and Suite settings are under
`docs/screenshots/suite`; CI uploads the complete capture set.
Hardware login, power, locking and whole-desktop idle memory remain to be
validated on DebION. See [THIRD-PARTY.md](THIRD-PARTY.md) for provenance.

LXQt, Budgie and COSMIC remain development references rather than extra desktop
shells shipped in SDE. `upstream/sources.lock.json` and `tools/suite-source.py`
prepare those original reference trees independently of the actual build lock.
