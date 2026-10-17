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
