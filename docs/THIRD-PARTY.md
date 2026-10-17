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

## Engines installed as package dependencies

Their source code is not copied into SDE's GPLv3 shell or relicensed. Debian
installs the maintained binaries and full historical copyright/license records.
Each source includes GNOME/MATE and other historical contributors; the abbreviated
credits here do not replace the complete per-file notices.

| SDE integration | Source | Copyright ownership | Full installed notices |
| --- | --- | --- | --- |
| Niny | [pluma](https://github.com/mate-desktop/pluma) | Pluma/gedit authors and MATE contributors | `/usr/share/doc/pluma/copyright` |
| Liria | [caja](https://github.com/mate-desktop/caja) | Caja/Nautilus authors, Eazel, GNOME and MATE contributors | `/usr/share/doc/caja/copyright` |
| Xanara | [atril](https://github.com/mate-desktop/atril) | Atril/Evince authors and MATE contributors | `/usr/share/doc/atril/copyright` |
| Taca | [engrampa](https://github.com/mate-desktop/engrampa) | Engrampa/File Roller authors and MATE contributors | `/usr/share/doc/engrampa/copyright` |
| SDE System Monitor | [mate-system-monitor](https://github.com/mate-desktop/mate-system-monitor) | GNOME System Monitor authors and MATE contributors | `/usr/share/doc/mate-system-monitor/copyright` |
| SDE Power Manager | [mate-power-manager](https://github.com/mate-desktop/mate-power-manager) | GNOME Power Manager authors and MATE contributors | `/usr/share/doc/mate-power-manager/copyright` |
| SDE View | [eom](https://github.com/mate-desktop/eom) | Eye of GNOME authors and MATE contributors | `/usr/share/doc/eom/copyright` |
| Screen lock | [mate-screensaver](https://github.com/mate-desktop/mate-screensaver) | GNOME Screensaver authors and MATE contributors | `/usr/share/doc/mate-screensaver/copyright` |
| Update transactions | [PackageKit](https://github.com/PackageKit/PackageKit) | PackageKit authors and contributors | `/usr/share/doc/packagekit/copyright` |

The MATE applications principally use GPL-2.0-or-later; individual libraries,
documentation, translations and bundled files can have LGPL, permissive or other
licenses. Use the complete notices for the exact installed version. Caja's
upstream `TRADEMARK_NOTICE` also remains part of the source workspace. No upstream
copyright claim is replaced by the SDE integration names.

## Design and development references

- [Eduka-Desktop](https://github.com/hugomonizdorego/Eduka-Desktop) — Hugo Moniz do
  Rego and Eduka contributors; source mapping in [EDUKA-DESIGN.md](EDUKA-DESIGN.md).
- [MATE](https://github.com/mate-desktop) — maintained GTK application engines.
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
