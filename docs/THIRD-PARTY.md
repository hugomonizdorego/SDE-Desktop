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
| Niny | [pluma](https://github.com/mate-desktop/pluma) | Pluma/gedit and MATE contributors | `sde/upstream/pluma` |
| Liria | [caja](https://github.com/mate-desktop/caja) | Caja/Nautilus, Eazel, GNOME and MATE contributors | `sde/upstream/caja` |
| Xanara | [atril](https://github.com/mate-desktop/atril) | Atril/Evince and MATE contributors | `sde/upstream/atril` |
| Taca | [engrampa](https://github.com/mate-desktop/engrampa) | Engrampa/File Roller and MATE contributors | `sde/upstream/engrampa` |
| SDE View | [eom](https://github.com/mate-desktop/eom) | Eye of GNOME and MATE contributors | `sde/upstream/eom` |
| SDE System Monitor | [mate-system-monitor](https://github.com/mate-desktop/mate-system-monitor) | GNOME System Monitor and MATE contributors | `sde/upstream/mate-system-monitor` |
| SDE Power Manager | [mate-power-manager](https://github.com/mate-desktop/mate-power-manager) | GNOME Power Manager and MATE contributors | `sde/upstream/mate-power-manager` |
| SDE Screen Lock | [mate-screensaver](https://github.com/mate-desktop/mate-screensaver) | GNOME Screensaver and MATE contributors | `sde/upstream/mate-screensaver` |

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


## Gallery (alpha10)

[gthumb](https://gitlab.gnome.org/GNOME/gthumb) 4.0 — Paolo Bacchilega and gThumb
contributors, GPL-2.0-or-later; individual source notices are retained.
SDE Gallery development: [Hugo Moniz do Rego](https://github.com/hugomonizdorego)
and SDE contributors. The verified official GNOME release archive is pinned in
`suite/gallery.lock.json`. The complete modified source tree and COPYING are
included in every `sde-native-sources_*.tar.xz`; installed legal notices are in
`/usr/share/doc/sde/upstream/gthumb/`. See [Alpha 10](ALPHA10.md).
