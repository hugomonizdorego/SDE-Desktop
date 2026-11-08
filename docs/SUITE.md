# SDE applications

Every application of the suite is SDE's own program, written from scratch
by the Selviana DE Project in C++17 on SDE-FLTK (Display Settings and the
Software Updater port the models of the projects named below). Up to
0.2.0 Alpha 3.1 Liria was the last application built from a fork of a MATE
application (Caja); since 0.2.0 Alpha 4 it is SDE's own too
([LIRIA.md](LIRIA.md)), so no source tree of another application is
prepared or compiled any more. Since 0.2.0 Alpha 4 every SDE
application is a Debian package of its own and SDE Core is the package
`sde-core`; see [PACKAGING.md](PACKAGING.md).

| Application | Executable | Source |
| --- | --- | --- |
| Niny | `niny` | native SDE application (Selviana DE Project); tabs and menu bar: [NINY.md](NINY.md) |
| Liria | `liria` | native SDE application (Selviana DE Project, 0.2.0 Alpha 4): [LIRIA.md](LIRIA.md) |
| Xanara | `xanara` | native SDE application (Selviana DE Project) |
| Taca | `taca` | native SDE application (Selviana DE Project) |
| Photo | `sde-view` | native SDE application (Selviana DE Project) |
| SDE System Monitor | `sde-system-monitor` | native SDE application (Selviana DE Project); processes and kernel threads: [below](#sde-system-monitor) |
| SDE Power | `sde-power` (Control Center > Power) | native SDE application (Selviana DE Project, alpha16) |
| SDE Screen Lock | `sde-lock` (Control Center > Lock Screen) | native SDE application (Selviana DE Project, alpha16) |
| Calculator | `calculator` | native SDE application (Selviana DE Project, alpha50; replaced the GTK4 scientific-calculator frontend) |
| Display Settings | `sde-display-settings` | [DisplayProfileManager](https://github.com/zac15987/DisplayProfileManager) profile model, native Linux frontend |
| Software Updater (formerly SDE Update) | `sde-update` | [mintupdate](https://github.com/linuxmint/mintupdate) model, Debian PackageKit (the operating system; apps are updated in Merkadu) |

SDE's configuration, panel geometry, autostart and suite diagnostics use
Rust; the shell and the applications use C++/FLTK.

## Liria

Liria, the file manager, has its own document: [LIRIA.md](LIRIA.md) (what
it does, its files, keys, D-Bus interfaces, tests and measurements). The
Caja fork it replaces in 0.2.0 Alpha 4 is described in the release notes
of the versions that shipped it (docs/ALPHA16.md to docs/0.2-ALPHA3.1.md);
its measurements are the reference LIRIA.md compares the new Liria with.

## SDE Power and SDE Screen Lock (alpha16)

The former power-manager and screen-saver forks were replaced in alpha16 by
programs written from scratch (`src/power`, `src/lock`), both started by
SDE-only XDG autostart entries:

* `sde-power --daemon`: UPower battery warnings and the critical action,
  logind lid/power-button inhibitors, idle dim/blank/suspend (separately on
  battery and AC), brightness keys with an OSD, power profiles. D-Bus
  `org.debion.SDE.Power`. Settings in sde.conf `[power]`.
* `sde-lock --daemon`: `org.freedesktop.ScreenSaver` (Lock, GetActive,
  SetActive, Inhibit/UnInhibit, ...) and `org.debion.SDE.ScreenLock`, idle
  lock, lock before suspend (logind delay inhibitor), logind Lock/Unlock.
  The lock screen is a separate process watched by the daemon; PAM service
  `/etc/pam.d/sde-lock` (`@include common-auth`, `common-account`).
  Settings in sde.conf `[lock]`.

`sde-power-preferences`, `sde-screensaver-preferences` (open the Control
Center pages) and `sde-screensaver-command` (forwards `--lock`, `--query`, `--poke`
to `sde-lock`) remain as small compatibility scripts.

New GTK processes follow SDE light/dark settings, respecting an existing
`GTK_THEME` or the system-theme option. SDE-specific MIME defaults preserve user
overrides. Xanara (native since alpha15) opens, annotates and fills in PDF files with
poppler; since 0.2 Alpha 3 its own PDF engine (libqpdf, HarfBuzz, FreeType) edits the
text and pictures of the pages in place and does every page operation itself, without
running any other program (docs/XANARA.md).

The panel defaults to a full-width square-edged rectangle. Floating and short
styles remain available. Existing saved panel choices are retained. Control Center → Applications shows short clickable source names and complete installed copyright
notices under `/usr/share/doc/sde/upstream/<upstream>/`.

## Installation

The supported target is **Debian testing**. Distributions that ship SDE, such
as DebION GNU/Linux, should publish all SDE packages in their own signed APT
repository and seed `sde-desktop` in the installer:

```sh
sudo apt install sde-desktop
```

`sde-core` is the desktop and each application is a package of its own
(`liria`, `xanara`, `niny`, `merkadu`, `sde-calculator`, `taca`, `sde-photo`,
`sde-gallery`). `sde-desktop` installs them with GNOME Terminal, the LightDM
login screen (Slick Greeter), Xorg, network, audio, removable-media support,
icons and GSettings. These are operating-system
services required for a complete DE. This project does not configure an APT
repository. For local packages, let APT resolve dependencies:

```sh
sudo apt install $(ls ./dist/*.deb | grep -v -e -dbgsym_ -e /debion-desktop-base_)
```

Select SDE-Desktop at the login screen. Debug-symbol packages are optional and are not
needed for normal installation. `dpkg -i` alone does not fetch missing libraries
or system services; use `sudo apt --fix-broken install` to repair that state.

## Software Updater

Software Updater (`sde-update`, called SDE Update before Alpha 51) is a native
FLTK window in the style of a classic software updater, drawn with the active
SDE theme (colors and radii follow the theme, also when it changes while the
window is open) at every UI scale. It updates the operating system: the Debian
packages. Applications from Flathub and the Snap Store are updated in Merkadu,
the software center ([MERKADU.md](MERKADU.md)), whose Updates page sends system
updates back here; since 0.2 Alpha 3 the Software Updater has no Flatpak group,
and its up-to-date view offers **Open Merkadu** when Merkadu is installed.

- **Updater**: the question "Updated software is available for this computer.
  Do you want to install it now?" (or "up to date" with the last check), a
  collapsible **Details of updates** tree with the groups Critical, Security
  and Main (tri-state check boxes, sizes, application names and icons
  from the packages' desktop files), a collapsible **Technical description**
  (versions, origin, size and the changelog from `apt-get changelog`,
  PackageKit `get-update-detail`, then the installed changelog, loaded in the
  background), the download size, and **Settings…**, **Remind Me Later** and
  **Install Now**. Progress, the log and a **Restart Now** / **Later** notice
  appear in the same window.
- Critical: Priority required/important, the kernel, libc, systemd, D-Bus,
  APT/dpkg, boot loader, firmware and microcode, urgency high/critical, and
  anything that needs a restart. Security: the Debian security archive. Main:
  everything else (backports included).
- **Settings…** opens the other pages with a sidebar (Escape goes back):
  **History** from `/var/log/apt/history.log*`, **Sources** (read-only one-line
  and deb822 entries, Software Sources when installed), **Maintenance** (refresh,
  clean cache, remove unused packages after review, repair, terminal update),
  **Settings** (`[update]` in sde.conf) and **About** with the mintupdate credits.

The read-only Python APT provider (`data/update/check.py`, protocol
`SDE-UPDATES 2`, with optional priority, section, urgency and application
fields since Alpha 51) uses the `Update` class adapted from Mint Update: candidates
are grouped by source, held packages are excluded and counted, Debian backports
get their own type. Mint repositories and Mint-only policies are not imported.

Transactions run in `transaction.py`, which streams progress (status, percent,
per-package items) to the window. It uses `pkcon` when PackageKit is running,
so polkit authorizes and the user can cancel; otherwise, and for cache cleaning
and autoremove, `apt-get` runs through `pkexec`. Package names are validated and
no options or repository URLs are accepted. `sde-update --notifier` (autostarted)
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
validated on real hardware. See [THIRD-PARTY.md](THIRD-PARTY.md) for provenance.

LXQt, Budgie and COSMIC remain development references rather than extra desktop
shells shipped in SDE. `upstream/sources.lock.json` and `tools/suite-source.py`
prepare those original reference trees independently of the actual build lock.

See [alpha8 notes](ALPHA8.md) for current category placement and Display Settings.

## Calculator (alpha50)

Calculator (`calculator`, desktop id `org.debion.Calculator`) is a native
SDE-FLTK application written from scratch by the Selviana DE Project
(`src/calculator/`). It replaces the GTK4 frontend of the scientific-calculator
component, which cleared the input instead of computing "2 + 2".

* Modes: Standard, Scientific (deg/rad/grad, 2nd and hyp functions, log/ln,
  powers and roots, n!, π, e, parentheses, %, EXP, memory MC/MR/M+/M−/MS),
  Programmer (HEX/DEC/OCT/BIN, clickable bit view, AND/OR/XOR/NOT/NAND/NOR,
  shifts, BYTE/WORD/DWORD/QWORD) and Converter (length, mass, temperature,
  area, volume, speed, time, data and currency with an offline table that
  `~/.config/sde/calculator-currency.conf` can override).
* Expressions are typed or clicked: operator precedence, right associative
  `^` (2^3^2 = 512), unary minus (-2^2 = -4), implicit multiplication (2π,
  2(3+4)), pocket-calculator percent (200 + 10% = 220), live preview, inline
  errors ("Cannot divide by zero") that keep the input. Results use 64-bit
  mantissas shown with 16 significant digits (0.1 + 0.2 = 0.3).
* History panel (click to reuse, right click for more), persisted in
  `$XDG_STATE_HOME/sde/calculator-history.tsv`; preferences and memory in
  `$XDG_STATE_HOME/sde/calculator.ini`.
* `calculator --eval EXPR [--angle deg|rad|grad]`, `--eval EXPR --base hex
  [--bits 32]` and `--convert VALUE FROM TO` work without a display.
* `SDE_CALC_QA_FILE=path` makes the window write its mode, expression,
  result and error to `path` after every change (used by `tools/ui-smoke.py`).

Tests: `calculator-engine` (tests/test_calculator.cpp) and `sde-calculator`
(tests/test_calculator.py).

## SDE System Monitor

SDE System Monitor (`sde-system-monitor`, `src/sysmon/`, manual page
`sde-system-monitor(1)`) has the pages Processes, Performance, File Systems
and Startup Apps. Since 0.2.0 Alpha 4 its process list starts like the
monitors of GNOME and KDE:

* **My processes** (the default): your apps (they own a window or were
  started from a .desktop file) and your background processes, the programs
  and services that run as you. **All processes** adds the system's
  processes (root's and the other users') in a group of their own. The list
  is sorted by CPU until another column is chosen.
* **The threads of the Linux kernel are hidden** (0.2.0 Alpha 3.1 listed
  every kworker, ksoftirqd, rcu_preempt and migration thread as a background
  process); the line under the list says how many. **Show kernel threads**
  lists them in a group of their own, "Linux kernel", each with its own name
  and what it does ("Linux kernel — worker thread"); the tooltip and the
  line under the list explain the one selected. Kernel threads are told
  from programs by the flag `PF_KTHREAD` of `/proc/PID/stat` (on kernels
  without it: children of kthreadd without a command line) and described by
  their names (`describe_kernel_thread()` in `src/sysmon/procfs.cpp`):
  kworker threads are the kernel's workers, which do the work that drivers,
  file systems and the network hand over (their name gives the processor,
  `u` for any, and after `+` or `-` the work queue); ksoftirqd finishes
  interrupts, rcu_* keep shared memory consistent, migration moves tasks
  between processors, and so on. They belong to no program, use no memory
  of their own and cannot be ended: End Task, Kill, Stop, Continue, Change
  Priority and Open File Location are never offered for them.
* The view and the switch are kept in `[system-monitor]` of
  `~/.config/sde/sde.conf` (`processes=mine|all`, `kernel-threads=`).

Tests: `sde-sysmon-parsers` (tests/test_sysmon.cpp: the parsers, the
kernel-thread flag and the descriptions) and `sde-sysmon-ui`
(tests/test_sysmon_ui.py, on Xvfb: My processes without kernel threads,
Show kernel threads with their group and descriptions, End Task off for
them, All processes with another user's process, the choices kept). With
`SDE_PANEL_QA_DIR` the window writes `System-Monitor.txt` (the view, the
groups, each row and the buttons).
