# SDE Suite for DebION GNU/Linux

Alpha5 integrates a complete application set into the SDE session. The target
distribution is **DebION GNU/Linux, based on Debian testing**. This remains a
development release of Selviana Desktop Environment.

| SDE application | Function | Source |
| --- | --- | --- |
| Niny | Text editor | [pluma](https://github.com/mate-desktop/pluma) |
| Liria | File manager | [caja](https://github.com/mate-desktop/caja) |
| Xanara | PDF, PostScript and EPUB viewer | [atril](https://github.com/mate-desktop/atril) |
| Taca | Archive manager | [engrampa](https://github.com/mate-desktop/engrampa) |
| SDE System Monitor | Processes and resources | [mate-system-monitor](https://github.com/mate-desktop/mate-system-monitor) |
| SDE Power Manager | Battery, lid and idle policies | [mate-power-manager](https://github.com/mate-desktop/mate-power-manager) |
| SDE View | Image viewer | [eom](https://github.com/mate-desktop/eom) |
| SDE Update | Debian update interface | [mintupdate](https://github.com/linuxmint/mintupdate) |

## What this version implements

Niny, Liria, Xanara, Taca, SDE View, System Monitor and Power Manager are **SDE
integration names for distribution-packaged MATE engines**. A single native Rust
launcher dispatches their commands without a shell. This alpha does not claim
that these applications have already been independently forked or rewritten in
Rust. Their internal application names, About dialogs, settings schemas, plugin
interfaces and translations remain upstream's. Using Debian's maintained engines
also retains distribution security fixes and avoids duplicate GTK libraries.

The SDE application menu presents the SDE names and suppresses corresponding
upstream duplicates only while their SDE entries are available. Other desktops'
menus are unchanged. Existing pins and user MIME preferences are retained.
New GTK suite processes can follow SDE light/dark mode; an explicit `GTK_THEME`
is respected, and Settings can keep the system GTK theme instead. Running
applications may need to be closed and reopened for this change.

New SDE defaults open directories in Liria, text in Niny, documents in Xanara,
images in SDE View and archives in Taca. `sde-mimeapps.list` is desktop-specific;
the installer does not overwrite users' `mimeapps.list`.

Liria passes `--no-desktop` to Caja: SDE keeps wallpaper, desktop icons and its
desktop context menu. The MATE power daemon and screen locker start through
SDE-only XDG autostart entries. Their upstream MATE-only entries are not started
by SDE's XDG launcher. `sde-lock` connects SDE's lock buttons to the installed
screen locker. Power management and screen locking still need real hardware and
login-session testing before a production DebION image is released.

The panel defaults to a full-width rectangle attached to the screen edge, with
square launcher, task and status controls. Floating and short styles remain
available in Settings. Users with an explicitly saved style keep their choice;
select **Settings → Panel → Full edge** to change an existing configuration.
Settings adds **SDE Suite** and **Power & Updates** pages. Source buttons use
short names such as `pluma`; Copyright opens the complete installed notice.

## SDE Update

SDE Update has a native FLTK interface. Its read-only Python APT provider adapts
the `Update` class from Mint Update to group binary packages by source, report
versions/download sizes and recognize security origins. Held packages are
excluded. Mint settings, kernel policies, browser security assumptions, Mint
repositories and Mint-only dependencies are not imported.

**Check Again** reads the local APT cache. **Refresh Sources** opens a terminal
for `pkgcli refresh` (or `pkcon refresh force` on older releases);
**Install Updates** opens `pkgcli update` or `pkcon update`. PackageKit
recalculates the actual transaction and presents its package changes and
confirmation in the terminal. System polkit controls authorization. After the
transaction, press Enter in that terminal to return and reload the preview.
Debian testing uses the newer client shipped inside `packagekit`; older
distributions install `packagekit-tools`.
The preview is not a promise that every Debian testing transition is currently
resolvable. Resolve transitions requiring manual package decisions with Debian's
normal APT tools. SDE does not change repositories, run unattended upgrades or
start a background updater. CLI `sde-update --check` prints the read-only preview protocol for diagnostics.
`sde-update --plan refresh|upgrade` only prints
the fixed transaction argument vector and performs no system changes.

## One installation entry point

DebION image builders should seed the **`sde-desktop` metapackage** in their own
signed Debian repository, together with the `sde` binary package. Users then run:

```sh
sudo apt install sde-desktop
```

`sde` contains SDE's native programs and requires the suite engines, power/lock,
APT provider and PackageKit. `sde-desktop` adds Xorg, a display manager, network,
audio, media/volume services, icons and GSettings support. This gives one package
to select in the DebION installer without installing the whole MATE, LXQt,
Budgie or COSMIC environment. Apt resolves libraries and architecture-specific
binaries. This repository does not publish or configure a DebION APT repository.

For a locally built pair of packages:

```sh
tools/build-deb.sh dist
sudo apt install ./dist/sde_*.deb ./dist/sde-desktop_*.deb
```

Then select **SDE** at the login screen. A source-only CMake install does not
install operating-system dependencies; use Debian packages for a complete DE.

## Reproducible source workspace

`upstream/sources.lock.json` records immutable revisions of all requested source
projects. Runtime MATE package versions follow the distribution; the source lock
records development references, not a false claim about the installed binaries.
Prepare the full upstream trees for downstream fork work with:

```sh
python3 tools/suite-source.py                    # all eight application sources
python3 tools/suite-source.py pluma caja          # selected sources
python3 tools/suite-source.py --verify-only       # verify clean pinned checkouts
python3 tools/suite-source.py --references        # also LXQt, Budgie and COSMIC
```

The tool preserves the complete tree, copyright headers, AUTHORS, COPYING,
library licenses, translations and trademark notices. It refuses a mismatched
or modified existing checkout and never discards development work. Fetching
sources does not build or install them. Building independently renamed forks,
updating plugin/schema namespaces and packaging each fork are future work; each
must preserve its actual upstream licenses and receive separate validation.

LXQt, Budgie and COSMIC are pinned **reference-only** projects in this release;
no source or runtime from them is represented as shipped SDE code. SDE continues
to use its own Rust core, native shell and Openbox instead of loading several
complete desktop shells. See [THIRD-PARTY.md](THIRD-PARTY.md) for attribution.

## Validation and screenshots

`tools/test-suite.py` checks installed alias routing, literal file arguments,
GTK theme preferences, missing-backend errors and fixed update action plans
without starting real engines or transactions. `tools/check-installed-suite.py`
checks MIME defaults/user overrides, power/locker autostart dry-runs and complete
installed copyright notices. The package CI runs these on Debian testing,
Debian 12/13 and Ubuntu 22.04/24.04.

On smaller screens the settings sidebar also scrolls and keeps the selected
category visible, including the final About page.

`tools/ui-smoke.py` exercises the native shell and settings at 1366×768/100% and
1024×600/150%. Wi-Fi, sound and APT candidates are explicit harmless fixtures;
no actual update, lock, suspend or power action is run. With `--suite-apps`, it
also opens real installed Niny/Pluma, Liria/Caja, Xanara/Atril, Taca/Engrampa and
SDE View/EOM engine windows with temporary sample files. Their original window
identity is intentional in this integration release. Native screenshots are
under `docs/screenshots/suite`; CI uploads the complete captures. Local source
checkouts were verified at all eleven pinned revisions. Full installed desktop
memory, hardware power/lock behavior and real update authorization still need
validation on a DebION installation.
