# Legal: copyright, licenses and trademarks

This page summarises who makes the Selviana Desktop Environment, under which
licenses it and its bundled components are distributed, which trademark
notices it shows, and how SDE relates to DebION GNU/Linux. The machine-readable
form is [`data/legal/components.json`](../data/legal/components.json); the
packaging form is [`debian/copyright`](../debian/copyright).

## SDE and DebION are separate projects

* The **Selviana Desktop Environment** (short **SDE**) is a desktop
  environment. It is made by the **Selviana DE Project**, which every SDE
  program names as its developer (About windows, `--version`, desktop files,
  AppStream metadata). Website:
  <https://github.com/hugomonizdorego/SDE-Desktop>.
* **DebION GNU/Linux** is a separate project: a Debian-based distribution
  (Linux kernel) that ships SDE. DebION is not SDE's developer, and SDE is
  not part of DebION. SDE is built and tested on Debian testing and works on
  distributions based on it.
* Only the distribution integration keeps the DebION name: the
  `debion-desktop-base` package (its artwork for Debian's desktop-base in
  `data/desktop-base`, the distribution wallpapers, logos and boot splash,
  and its network defaults `debion-network-check`, `debion-network-repair`,
  `/etc/debion/network.conf`, the polkit action `org.debion.network-check`),
  and the LightDM greeter's login background that comes from it.
* Places that show the **operating system** read its name from
  `/etc/os-release` (`NAME`, `PRETTY_NAME`, `LOGO`) and never hard-code a
  distribution: SDE Welcome ("Welcome to <NAME>", with the desktop shown
  apart as "Desktop: Selviana Desktop Environment <version>") and Control
  Center → About → System.
* Up to 0.1.0~alpha52 SDE named the "DebION Dev-Team" as its developer; since
  0.2.0 Alpha 1 it is the Selviana DE Project. Copyright headers of SDE's
  own files were updated accordingly; the changelogs and older release notes
  keep the name they had at the time.

## Copyright

SDE's own code, data and documentation:
**Copyright © 2026 Selviana DE Project and contributors.** Contributors keep
the copyright of their contributions. License: **GNU General Public License,
version 3 or later** (`GPL-3.0-or-later`, [LICENSE](../LICENSE)).

Bundled forks and adapted code keep their upstream licenses and every
original notice:

| Component | License (SPDX) | Copyright | Origin |
| --- | --- | --- | --- |
| SDE (session, panel, desktop, notifications, Run, Log Out, Power, Screen Lock, Welcome, Driver Manager, System Reports, Software Updater, System Monitor, Niny, Xanara, Taca, Photo, Gallery, Calculator, Rust core) | GPL-3.0-or-later | Selviana DE Project and contributors | written for SDE |
| Display Settings, Control Center (display profile model) | GPL-3.0-or-later AND MIT | Selviana DE Project and contributors; © 2025 zac15987 | profile model ported from [DisplayProfileManager](https://github.com/zac15987/DisplayProfileManager) (MIT) |
| Software Updater (update model, `data/update/model.py`) | GPL-3.0-or-later | © 2007–2015 Clement Lefebvre; Selviana DE Project and contributors | adapted from [mintupdate](https://github.com/linuxmint/mintupdate) (GPL-3+) |
| Merkadu (software center, package `merkadu`) | GPL-3.0-or-later AND GPL-3.0-only | © 2009–2013 Canonical Ltd.; Selviana DE Project and contributors | based on [Ubuntu Software Center](https://github.com/mortenpi/ubuntu-software-center) (GPL-3); its category parser and menu are ported (`src/merkadu/categories.*`, `data/merkadu/categories.menu`) |
| Liria | GPL-2.0-or-later AND LGPL-2.0-or-later | © 1999–2009 The Nautilus authors, © 2011–2021 The Caja authors (and Eazel, Red Hat, Novell and others per file); Selviana DE Project and contributors (SDE changes) | fork of [Caja](https://github.com/mate-desktop/caja) 1.28.0; eel and libcaja-extension are LGPL |
| SDE-FLTK | LGPL-2.0-only WITH FLTK-exception | © 1998–2026 Bill Spitzak and others; Selviana DE Project and contributors (SDE-FLTK changes) | fork of [FLTK](https://www.fltk.org) 1.4.5 |
| SDE WM | GPL-2.0-or-later | © 2002–2007 Dana Jansens, © 2004–2006 Mikael Magnusson, © 2003 Derek Foreman and the Openbox contributors | built from [Openbox](https://github.com/Mikachu/openbox) with SDE patches |
| SDE Compositor | MPL-2.0 AND MIT | © 2003 Keith Packard (xcompmgr), © 2011–2013 Christopher Jeffrey (Compton), © 2019–2023 Yuxuan Shui and the picom contributors | built from [picom](https://github.com/yshui/picom) |
| SDE themes | GPL-3.0-only (Graphite, Flat Remix GTK) with SDE's GPL-3.0-or-later overlays; CC-BY-SA-4.0 (Flat Remix GNOME, palette reference only) | © 2021–2025 Vince Liu; © 2016–2025 Daniel Ruiz de Alegría; Selviana DE Project and contributors | see [THIRD-PARTY.md](THIRD-PARTY.md) |
| SDE artwork (logo, mark, application icons, wallpapers, sample avatars) | GPL-3.0-or-later | © 2026 Selviana DE Project | made for SDE; the name and logo are trademarks, see below |
| DebION integration (`debion-desktop-base`) | GPL-3.0-or-later | Selviana DE Project and contributors | distribution integration, not part of SDE |

The theme sources' LICENSE files contain the GNU GPL version 3 without a
later-version grant, so they are treated as version 3 only. Every row, with
the exact upstream commits, is in `data/legal/components.json`; source
links and the history of the forks are in [THIRD-PARTY.md](THIRD-PARTY.md).
Shared libraries of the distribution (GTK, cairo, poppler, libarchive, ...)
are dependencies with their own copyright files and are not bundled.

## Trademarks

SDE shows only these notices, and only where they are relevant:

* "Selviana Desktop Environment, SDE and the SDE logo are trademarks of the
  Selviana DE Project." — in every About window (Legal section), Control
  Center → About, the AppStream description of the desktop, README and
  `debian/copyright`. No registration is claimed (no ®).
* "Linux® is the registered trademark of Linus Torvalds in the U.S. and other
  countries." — only where Linux is named: Control Center → About → System
  (kernel), and About windows whose texts name it (e.g. credits to Linux
  Mint).
* "Debian is a registered trademark owned by Software in the Public
  Interest, Inc." — only where Debian is named (e.g. Software Updater and
  Driver Manager, which use Debian's packages; Control Center when the
  operating system's name contains Debian).
* "PayPal is a trademark of PayPal, Inc." — next to the donation button in
  Control Center → About.

The copyright licenses cover the files; they do not grant rights to the SDE
name and logo. How the name and logo may be used is described in
[BRANDING.md](BRANDING.md). Other names appear only to identify the projects
SDE credits.

## EDE

SDE is inspired by the Equinox Desktop Environment (EDE,
<https://edeproject.org>) and **contains no EDE code**. `bootstrap/` only
fetches EDE sources for reference; nothing of them is built or installed. SDE
may be described as "inspired by EDE", never as a fork of EDE.

## Historical identifiers

Some technical identifiers start with `org.debion` because they were chosen
before the projects were separated. They are **stable identifiers**, not a
statement of who develops SDE, and stay unchanged so that user settings,
file associations, software-center metadata and D-Bus clients keep working:

* AppStream and desktop-file ids: `org.debion.SDE`, `org.debion.Settings`,
  `org.debion.Calculator` (`org.debion.Calculator.desktop`), `org.debion.Niny`,
  `org.debion.Liria`, `org.debion.Taca`, `org.debion.Xanara`, ... (the
  `data/metainfo/org.debion.*.metainfo.xml` files);
* D-Bus names: `org.debion.SDE.Power`, `org.debion.SDE.ScreenLock`;
* the GSettings schema `org.debion.session` and Liria's
  `org.debion.liria.*` schemas;
* binary and package names (`sde-*`, `niny`, `liria`, `xanara`, `taca`,
  `calculator`, `sde`, `sde-desktop`, `debion-desktop-base`) and the icon
  compatibility names `sdeview` and `org.debion.Calculator`.

The AppStream `<developer>` id is `io.github.hugomonizdorego`: the
reverse-DNS form of the GitHub account that hosts the project, the only
namespace the Selviana DE Project currently controls. It changes if the
project gets its own domain.

## Where the information is shown

* **About window** of every SDE program (`sde::show_about`): sections About
  (part of SDE, developer, website), Credits (upstream projects and their
  licenses), License (license name, the standard notice including the
  warranty disclaimer, and the full text from `/usr/share/common-licenses`
  or the installed upstream notice) and Legal (copyright and trademarks).
* **Control Center → About**: the Selviana Desktop Environment part
  (official logo, version, build, copyright, license, trademarks,
  Open-source licenses with every component, donation) and, separately, the
  System part (operating system from os-release, kernel, hardware).
* **`--version`** of every program: GNU format with copyright, license and
  the warranty disclaimer.
* **Packages**: `/usr/share/doc/sde/copyright` (`debian/copyright`),
  `/usr/share/sde/legal/components.json`, `/usr/share/doc/sde/upstream/*`,
  `/usr/share/doc/sde/sde-fltk/COPYING` and `CREDITS.txt`, `/usr/share/doc/sde/themes/*`.

## Keeping it consistent

`data/legal/components.json` is the single source of truth. When a program
or bundled component is added or changes its license:

1. add or edit its entry (id, name, license as an SPDX expression, copyright
   lines, upstream projects, trademark keys, icons, desktop and metainfo
   files, source paths);
2. give its sources SPDX headers and, if they are not GPL-3.0-or-later,
   a stanza in `debian/copyright`;
3. set the same `project_license` in its metainfo and the same `Icon=` in
   its desktop file.

`tools/legal-data.py --check` validates the file, and CMake turns it into
`generated/legal_data.h`. The `sde-legal-data` test compares it with the
desktop files, the metainfo, `data/suite.tsv`, the SPDX header of every
source file and `debian/copyright`; `sde-developer-metadata` checks the
developer name and id.
