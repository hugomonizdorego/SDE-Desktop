# Copyright and source credits

SDE shell and integration code: Copyright © 2026 Hugo Moniz do Rego and SDE
contributors, GPL-3.0-or-later. Names below are clickable source links.

## Code adapted into this repository

**[mintupdate](https://github.com/linuxmint/mintupdate)** — Copyright © 2007–2015
Clement Lefebvre and subsequent upstream contributors. `data/update/model.py`
adapts the `Update` class from `usr/lib/linuxmint/mintUpdate/Classes.py` at
`b4c586f9839d2943944390f4117a10acd4ba865a`. License: GPL-3.0-or-later.
SDE modifications: standalone Python model, no GI/Mint settings, removal of
Mint-specific classification and browser security heuristics. Original copyright
and full license are preserved in `data/update/COPYRIGHT` and `COPYING`, installed
under `/usr/share/sde/update`. The installed package's copyright records this
adaptation separately. No Mint artwork or trademarks are rebranded.

## Themes built from upstream sources

The SDE themes in `/usr/share/themes` are built by `tools/build-themes.py`
from `data/themes/palettes.json` and the upstream commits pinned in
`suite/themes.lock.json` (fetched by `tools/prepare-themes.py`; nothing is
vendored in this repository). Upstream licenses, READMEs and the exact source
of each are installed under `/usr/share/doc/sde/themes/<id>/`.

| SDE theme | Upstream | Commit | License | Used for |
|---|---|---|---|---|
| SDE Graphite (default), `SDE-Graphite-{Light,Dark}-<Accent>` | **[Graphite-gtk-theme](https://github.com/vinceliuice/Graphite-gtk-theme)** by Vince Liu | `364173f47407164948788e4abb5e2eb46600f71a` | GPL-3.0 | GTK 3/4 style sheets compiled with sassc for nine accents (`rimless`, `normal` sidebar tweaks), accent assets |
| SDE Glass, `SDE-Glass-{Light,Dark}-<Accent>` | Graphite-gtk-theme (as above) | as above | GPL-3.0 | imports SDE Graphite and adds SDE's frosted layer |
| SDE Fluent, `SDE-Fluent-{Light,Dark}-<Accent>` | **[flat-remix-gtk](https://github.com/daniruiz/flat-remix-gtk)** by Daniel Ruiz de Alegría | `919494f4f4ede88e2efb45cd48b98db7cc23f6ee` | GPL-3.0 | prebuilt GTK 3, GTK 4 and libadwaita color variants (Blue, Teal, Green, Yellow, Orange, Red, Magenta, Violet, Grey) |
| SDE Fluent palette (panel, menus, Action Center, frames) | **[flat-remix-gnome](https://github.com/daniruiz/flat-remix-gnome)** by Daniel Ruiz de Alegría | `95cb659507188db7d2ffdaa6f714db0e3555baf4` | CC-BY-SA-4.0 | colors and shapes of the GNOME Shell theme taken as the reference for SDE's own palette; no Shell theme is installed |

SDE's additions (GPL-3.0-or-later): the SDE overlays in
`data/themes/overlays/` (Fluent Mica tint, Windows 11 caption buttons, round
caption buttons for Graphite, the glass layer, suite application styling), the
libadwaita color files of Graphite and Glass, and every Openbox (SDE WM)
window theme, drawn from the same palette. Theme names are SDE's; the
upstream projects are credited in each theme's `index.theme` and CSS header.
The former names `SDE`, `SDE-Dark`, `SDE-Aero(-Dark)` and
`SDE-Liquid-Glass(-Dark)` remain as aliases of the new themes.

## Design references

**[GTKAero](https://github.com/User738git/GTKAero)** by User738git, a fork of
the B00MERANG Project Windows 7 GTK theme — GPL-3.0. The former SDE Aero theme
(0.1.0~alpha10 to alpha15) took its design idea (glass title and tool bars,
soft blue selections) from GTKAero; SDE Glass continues the idea. No GTKAero
images or CSS files were copied.

**[Liquid Glass for GNOME Shell](https://github.com/ryohsuke1231/liquid-glass)**
by Ryosuke Watanabe — MIT. An unofficial fan project, not affiliated with
Apple. The former SDE Liquid Glass theme and now SDE Glass recreate its look
(frosted translucent layers, large corners) in CSS; the SDE compositor
provides the blur. No code, shaders or images are copied.

**[Eduka-Desktop](https://github.com/hugomonizdorego/Eduka-Desktop)** —
the Edukasaun desktop suite by the same author. SDE ports behavior from its
Python/PyQt5 panel, Action Center and settings (effects model and capability
rule, compositor modes and report, removable drives, power profiles) into
SDE's C++/FLTK code while keeping SDE's own design.

## Native application forks

Eight application source trees are prepared and compiled by SDE. Exact upstream
versions and commits are recorded in `suite/forks.lock.json`. No upstream
copyright ownership is replaced by an SDE name. The original COPYING, AUTHORS,
COPYRIGHT, ChangeLog and trademark notices are preserved, as are copyright
comments in transformed source. Installed combined notices include original
copyright headers and full top-level licenses. Complete corresponding sources
and submodules are published with the binaries.

| SDE fork | Source | Historical authors | Installed notice directory |
| --- | --- | --- | --- |
| Niny | written by the DebION Dev-Team (alpha15) | DebION Dev-Team | `sde` |
| Liria | [caja](https://github.com/mate-desktop/caja) | Caja/Nautilus, Eazel, GNOME and MATE contributors | `sde/upstream/caja` |
| Xanara | written by the DebION Dev-Team (alpha15) | DebION Dev-Team | `sde` |
| Taca | written by the DebION Dev-Team (alpha15) | DebION Dev-Team | `sde` |
| Photo (SDE View) | written by the DebION Dev-Team (alpha15) | DebION Dev-Team | `sde` |
| SDE System Monitor | written by the DebION Dev-Team (alpha15) | DebION Dev-Team | `sde` |
| SDE Power | written by the DebION Dev-Team (alpha16) | DebION Dev-Team | `sde` |
| SDE Screen Lock | written by the DebION Dev-Team (alpha16) | DebION Dev-Team | `sde` |

Directories in the last column are relative to `/usr/share/doc`; each contains
`copyright` and the retained license files. These projects principally use
GPL-2.0-or-later. Individual libraries, translations, documentation and bundled
files retain their own LGPL, permissive or other licenses; the full source is
authoritative. Application rebranding is not a relicensing of those files.
SDE-specific theme glue is GPL-2.0-or-later; the shell's GPLv3 license remains
separate. Shared distribution libraries and PackageKit are dependencies with
their own distribution copyright notices.

## Design and development references

- [Eduka-Desktop](https://github.com/hugomonizdorego/Eduka-Desktop) — Hugo Moniz do
  Rego and Eduka contributors; source mapping in [EDUKA-DESIGN.md](EDUKA-DESIGN.md).
- [MATE](https://github.com/mate-desktop) — upstream GTK application sources.
- [LXQt](https://github.com/lxqt) — lightweight desktop development reference;
  pinned `lxqt-session` source, no copied implementation in this release.
- [budgie-desktop](https://github.com/buddiesofbudgie/budgie-desktop) — Buddies of
  Budgie and contributors; reference only, no imported code or artwork.
- [cosmic-epoch](https://github.com/pop-os/cosmic-epoch) — System76 and
  contributors; Rust desktop reference only, no imported code or artwork.

The immutable development revisions are in `upstream/sources.lock.json`.
`tools/suite-source.py` retains each project's complete original licensing when
preparing its source tree. A project being listed as a reference does not imply
that its code is licensed under SDE's GPLv3 or that it is installed with SDE.

## Alpha7 native components

| SDE component | Source | License | Integration |
|---|---|---|---|
| Calculator | [scientific-calculator](https://github.com/raad-altaie/scientific-calculator) | AGPL-3.0-only; Copyright (c) 2026 Raad Altaie | Unmodified pinned calculation engine embedded in a separate GTK4 native executable; SDE frontend also AGPL-3.0-only |
| SDE WM | [openbox](https://github.com/Mikachu/openbox) | GPL-2.0-or-later; all original contributors retained | Private source build, SDE executable/WM identity, SDE rc.xml and themes |
| SDE Compositor | [picom](https://github.com/yshui/picom) | MPL-2.0 AND MIT | Private source build and independent build version; lightweight XRender defaults |
| SDE-FLTK | fork of [FLTK 1.3.11](https://github.com/fltk/fltk), in `sde-fltk/` | LGPL-2.0 with FLTK static-linking exceptions; FLTK copyrights retained | DebION Dev-Team fork kept in this repository, linked statically into every SDE program (alpha14) |

sde-xsettingsd and sde-polkit-agent (alpha14) are new programs written from
scratch by the DebION Dev-Team (GPL-3.0-or-later); alpha13 used builds of
xsettingsd and mate-polkit instead.

Exact revisions are in `suite/components.lock.json`. Preparation retains complete
source trees, license texts, authors and historical notices. These are included
in `sde-native-sources_*.tar.xz` beside every Debian package. Installed copyright
files are under `/usr/share/doc/sde/upstream/`. Calculator embeds only the
calculator engine; browser-specific web panels are not represented as ported
native features. There is no browser or web view in the Calculator executable.

### Display Settings (alpha8)

[DisplayProfileManager](https://github.com/zac15987/DisplayProfileManager),
Copyright (c) 2025 zac15987, MIT, pinned to
`c873b66ba54b6efb111620e736878c21eda22f51` in `suite/components.lock.json`.
`src/display/profile.h` ports Profile/DisplaySetting from `Core/Profile.cs`;
`profile.cpp` ports case-insensitive GetUniqueProfileName from
`Core/ProfileManager.cs`. The complete MIT notice is `profile.LICENSE`.
Native Linux RandR and FLTK integration is GPL-3.0-or-later SDE code. The original
WPF/Win32 executable is not run as a Linux program. Full corresponding sources
contain both the pinned upstream tree and native port; installed notices are
under `/usr/share/doc/sde/upstream/DisplayProfileManager/`.

## Alpha 9 panel service behavior

[eduka-panel](https://github.com/hugomonizdorego/Eduka-Desktop/blob/7a92f29f731576830e8c5bde79a34d4f20b6b774/usr/bin/eduka-panel)
by Hugo Moniz do Rego and Eduka contributors is the behavior reference for
Bluetooth, audio streams/MPRIS, network controls and compact tray handling.
SDE implements these independently in native C++/FLTK; no Qt code/assets are
copied. The reference repo has no top-level license declaration. See
[Alpha 9](ALPHA9.md) for attribution, feature mapping and alpha limitations.
