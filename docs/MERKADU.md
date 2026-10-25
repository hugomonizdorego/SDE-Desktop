# Merkadu, the SDE software center

Merkadu (package `merkadu`, command `merkadu`, icon `merkadu`) finds,
installs, updates and removes apps from three sources in one window:

| Source | What it offers | How Merkadu talks to it |
| --- | --- | --- |
| **Debian** (the label is the distribution's name from `/etc/os-release`) | the packages of the system's APT sources | PackageKit over D-Bus (`org.freedesktop.PackageKit`), described by the archive's AppStream catalog (`/usr/share/swcatalog`, read with libappstream) |
| **Flatpak** | apps of the remotes of the system installation (Flathub and others); apps installed for the user are shown and managed too | libflatpak (`FlatpakInstallation`, `FlatpakTransaction`); catalog from the remotes' AppStream data |
| **Snap** | the Snap Store | snapd's REST API on `/run/snapd.socket` |

It is based on **Ubuntu Software Center** (Copyright © 2009–2013 Canonical
Ltd., GPL-3; see [THIRD-PARTY.md](THIRD-PARTY.md)): the lobby with featured,
new and top apps, the departments and their subcategories, the app pages,
the installed apps grouped by department, the history and the progress view
follow it. Its category parser and menu are ported to C++
(`src/merkadu/categories.*`, `data/merkadu/categories.menu`, GPL-3.0-only);
the rest is new code (GPL-3.0-or-later) in SDE-FLTK with the SDE UI kit. No
Python is involved, and nothing depends on Ubuntu's online services, which no
longer exist (ratings and reviews, the software center agent, purchases,
recommendations): "Top Apps" and "Featured" are lists the distribution
chooses ([configuration](#configuration-for-distributions)), "New & Updated"
comes from the release dates in the AppStream data.

## Using it

    merkadu [--page explore|installed|updates|history|progress|sources]
            [--search TEXT] [--category ID] [--details APPID]
    merkadu appstream://APPID
    merkadu --uninstall /usr/share/applications/APP.desktop
    merkadu --version | --help

- **Explore**: a banner and a row of featured apps, "New & Updated", "Top
  Apps" and the departments (Accessories, Universal Access, Developer
  Tools, Education, Science & Engineering, Fonts, Games, Graphics,
  Internet, Sound & Video, Office, Themes & Tweaks, System, Books &
  Magazines) with their subcategories and app counts.
- **Search** (Ctrl+F) covers every source at once. Each result shows badges
  for the sources that offer it; chips filter by source. Enter also asks the
  Snap Store, which is searched online.
- **App page**: icon, name, developer, summary, a chip per source (the one
  to install from is chosen with them), Install/Open/Update/Remove, the
  screenshots (click to enlarge), the description, "What's New" (the latest
  release notes), and a details card: version, download and installed size,
  license, source, isolation (Flatpak sandbox, snap strict or classic
  confinement, or "not isolated" for Debian packages), developer,
  department and links.
- **Installed**: installed apps by department, filtered by source; **Updates**:
  app updates of every source with "Update All". System updates (the kernel,
  libraries, security updates) belong to the Software Updater (`sde-update`),
  which the page links to.
- **History**: installations, updates and removals by day, from
  `/var/log/apt/history.log` (and its rotated copies), `flatpak history` and
  snapd's changes, with source filters.
- **In Progress**: the queue of changes. Changes run one after the other;
  the running one shows the animated SDE logo, its status and percentage and
  can be cancelled when its source allows it. Finished and failed changes
  stay listed until "Clear Finished".
- **Sources**: the state of the three sources, "Add Flathub" when the system
  installation has no Flathub remote, "Install Flatpak Support" or "Install
  Snap Support" (the `flatpak` or `snapd` package) when they are missing,
  and where snap data is kept (see [the snap folder](#the-snap-folder)).

Keys: Ctrl+F or Ctrl+K search, Alt+Left, Backspace or Escape go back, F5
reloads the sources, F1 opens About Merkadu, Ctrl+Q quits.

### The progress animation

While a change runs, Merkadu shows the SDE mark (the monitor of the SDE
logo, `sde-mark.svg` from the branding directory) with its three red, green
and blue stripes filling one after the other and the power LED flickering.
The layers of the SVG are rendered once per size and composed at 30 frames
per second only while a change runs (about 2 % of one CPU core, measured on
Xvfb at 1366×768; nothing when idle).
When the user turned the desktop effects off (Control Center → Appearance →
Effects; reduced motion), the logo stays still. The widget is
`src/merkadu/logo_anim.*`.

## Uninstalling from the Start menu

The Start menu's "Uninstall…" runs

    merkadu --uninstall /absolute/path/of/the/app.desktop

and Merkadu shows a small window instead of the main one:

1. It finds where the app comes from: a launcher with `X-Flatpak=` is a
   Flatpak (system or user installation, from the path), one with
   `X-SnapInstanceName=` (or under `/var/lib/snapd/desktop`) is a snap, a
   launcher in the user's home that no package installed is a user launcher,
   and anything else belongs to the Debian package that owns the file
   (`dpkg-query -S`).
2. It shows the app's icon and name, its origin ("Debian package gimp",
   "Flatpak org.gimp.GIMP from flathub", "Snap gimp (latest/stable)"), the
   disk space it frees and, for packages, the other packages that are
   removed with it (PackageKit's simulation of the removal). Cancel has the
   keyboard focus.
3. "Uninstall" removes it with the animated SDE logo and a progress bar;
   the user can cancel while the source allows it. A user launcher is only
   deleted ("Remove Launcher").
4. It reports the result; "Close" (or Return) ends the program.

Exit status: **0** the app was removed, **1** it could not be removed or
Merkadu refused (the error is shown first), **2** the user cancelled.

Refusals, explained in the window: launchers of SDE's own packages (`sde`,
`sde-desktop`, `merkadu`, plus `[Uninstall] protected=` of the
configuration) are installed and removed with the desktop, not on their own;
an app whose removal would also remove one of them (for example a library the
desktop needs) is refused, naming the package; a launcher no package owns
outside the user's home cannot be removed by Merkadu.

## Architecture

    main.cpp ─┬─ window.cpp, pages.cpp, ui.cpp   main window, pages, widgets
              ├─ uninstall.cpp                   --uninstall dialog
              └─ store.cpp  ── Store (singleton, FLTK thread)
                   │  catalog, jobs queue, history, events → pages
                   ├─ packagekit.cpp   Backend: PackageKit (D-Bus, async)
                   ├─ flatpak.cpp      Backend: libflatpak (worker threads)
                   ├─ snap.cpp         Backend: snapd REST (worker threads)
                   └─ fake.cpp         Backend: MERKADU_FAKE (tests)
    merkadu-core (no display; unit-tested):
      model.*            App, Origin, Catalog (merging), search, history
                         parsers, launcher classification, removal plans,
                         settings (merkadu.conf)
      categories.*       departments (Ubuntu Software Center's menu format)
      appstream_catalog.* AppStream components → apps (libappstream)
      snapd.*            HTTP over a UNIX socket, snapd's JSON
      pkenum.*, json.*   PackageKit enums and texts, a small JSON parser
    logo_anim.*, image_cache.*   the SDE logo animation; icons and
                         screenshots (downloaded with curl to
                         ~/.cache/merkadu/images, at most four at a time)

- **One app, several origins.** Every source yields `App`s with `Origin`s.
  The catalog merges them by their AppStream id (`org.gimp.GIMP`; desktop ids
  and `.desktop` suffixes are normalized), so GIMP from Debian and GIMP from
  Flathub are one app with two origins; snaps join by the AppStream ids the
  store publishes for them (`common-ids`), otherwise they stay `snap:NAME`.
  Installed state comes from the sources (PackageKit `GetPackages`,
  `flatpak_installation_list_installed_refs`, `/v2/snaps`), never from the
  catalog. The preferred origin is the installed one, then the order of
  `[Sources] order=`; the app page lets the user pick another.
- **Threads.** The UI runs on the FLTK thread. PackageKit is asynchronous
  D-Bus on the same loop (`sde::dbus`); AppStream loading, libflatpak
  transactions, snapd requests, `dpkg-query` and `flatpak history` run on
  worker threads and hand their results back with `Fl::awake()`.
- **Jobs.** Install, remove and update requests become jobs in one queue;
  one runs at a time, with progress events to the pages and the uninstall
  dialog. Cancel maps to PackageKit's `Cancel`, a `GCancellable` for
  libflatpak and `abort` of the snapd change.
- **Departments.** `categories.menu` uses Ubuntu Software Center's format
  (`<Menu>`, `<Name>`, `<SCIcon>`, `<Include>` with `<And>`, `<Or>`,
  `<Not>`, `<Category>`, `<SCSection>`, `<SCPkgname>`, `<SCType>`,
  `<SCAppId>`, `<SCSource>`, `<OnlyUnallocated/>`). Snaps carry store
  categories, which are mapped to freedesktop categories.

## Permissions

Merkadu runs as the user and never as root. Reading (catalogs, installed
apps, history) needs no authorization. Changes are authorized by polkit,
whose dialog in SDE is the SDE authentication agent:

| Change | Who asks | polkit action |
| --- | --- | --- |
| install, remove, update Debian packages | PackageKit (`SetHints interactive=true`) | `org.freedesktop.packagekit.package-install`, `package-remove`, `system-update` |
| install, remove, update Flatpaks of the system installation; add Flathub | the Flatpak system helper | `org.freedesktop.Flatpak.app-install`, `app-uninstall`, `app-update`, `runtime-install`, `modify-repo` |
| install, remove, refresh snaps | snapd (`X-Allow-Interaction: true`) | `io.snapcraft.snapd.manage` |
| hide the snap folder | snapd (`PUT /v2/snaps/system/conf`) | `io.snapcraft.snapd.manage-configuration` |

Apps installed for the user (Flatpak `--user`) need no authorization.
Merkadu never stores a password and never calls `sudo` or `pkexec`.

## The snap folder

**What was asked:** snapd puts a `snap` folder in every user's home; SDE
wanted it moved out of the home folder, to something like
`/usr/share/SDE/SNAPD`.

**What Merkadu does:** before the first snap is installed (and on the
Sources page, "Hide the Snap Folder"), Merkadu asks snapd — with the
user's authorization — to set its option

    snap set system experimental.hidden-snap-folder=true

With it, snapd keeps each user's snap data in the hidden `~/.snap/data`
instead of `~/snap`, and moves existing data there when the snaps are next
refreshed. The home folder no longer shows a `snap` folder. Merkadu reads the
state from snapd's configuration, or from the feature file
`/var/lib/snapd/features/hidden-snap-folder` that snapd exports when the
option is on. Set `[Snap] hide-home-folder=false` to leave snapd's default.

**Why not `/usr/share/SDE/SNAPD`:** it is not possible without replacing
snapd's confinement, and it would be wrong even if it were:

- Snap data is *per user*: each snap writes its settings and documents to
  `$SNAP_USER_DATA` (`<home>/snap/<name>/<revision>`) and
  `$SNAP_USER_COMMON`, owned by that user. snapd computes these paths from
  the home folder only: in its source, `snap/info.go` (`UserDataDir`,
  `snapDataDir`) chooses between `dirs.UserHomeSnapDir` (`snap`),
  `dirs.HiddenSnapDataHomeDir` (`.snap/data`, the experimental option) and,
  for `snap migrate-home`, `dirs.ExposedSnapHomeDir` (`Snap`, visible) —
  all relative to the home folder. There is no setting for another place.
- Confinement enforces it: the AppArmor template every strictly confined snap
  runs under (`interfaces/apparmor/template.go`) only allows writes to
  `owner @{HOME}/snap/<snap>/<revision>/**`, `owner
  @{HOME}/.snap/data/<snap>/…` and `owner @{HOME}/Snap/<snap>/**`. AppArmor
  checks the resolved path, so a symbolic link from `~/snap` to another
  folder is denied, and a bind mount would still need a `~/snap` mount point
  plus a root helper at every login.
- `/usr/share` belongs to the package manager and is read-only for users;
  a folder there writable by every user would mix the data of all users and
  let one user read or change another's — the reason snapd keeps it in the
  home folder.

So the honest choice is snapd's own hidden folder: the data stays where
snapd and the confinement expect it, private to each user, and out of
sight. Liria also hides dot-folders by default.

## Configuration for distributions

`/usr/share/merkadu/merkadu.conf` holds the defaults. Keys in
`$XDG_CONFIG_DIRS/merkadu/merkadu.conf` (for example
`/etc/xdg/merkadu/merkadu.conf`) and then `/etc/merkadu/merkadu.conf`
replace the same keys of the earlier files; `MERKADU_CONFIG=FILE` uses one
file instead of all of them.

    [Featured]
    # the Explore banner (first app) and the Featured row, in order
    apps=org.inkscape.Inkscape;org.gimp.GIMP;org.blender.Blender

    [Top]
    # "Top Apps"
    apps=org.mozilla.firefox;org.libreoffice.LibreOffice;org.videolan.VLC

    [New]
    # how many "New & Updated" apps (newest AppStream releases); 0 hides it
    count=12

    [Sources]
    # preferred source when an app is in several
    order=debian;flatpak;snap

    [Flatpak]
    # the repository file "Add Flathub" uses
    flathub-url=https://dl.flathub.org/repo/flathub.flatpakrepo

    [Snap]
    # hide ~/snap as ~/.snap/data (see above)
    hide-home-folder=true

    [Uninstall]
    # packages never removed besides sde, sde-desktop and merkadu
    protected=

Apps are named by their AppStream id, which finds them in every source, or
by `deb:PACKAGE`, `flatpak:APPID` or `snap:NAME`; names that are in no
catalog are skipped, so a list can name apps of sources the system does not
have.

Departments: `/usr/share/merkadu/categories.menu`. Drop-in files
`/usr/share/merkadu/menu.d/*.menu` and `/etc/merkadu/menu.d/*.menu` (the
same format) add departments, and replace the one with the same `<Name>`.
For example, a distribution's own department:

    <Menu>
      <Menu>
        <Name>DebION Tools</Name>
        <SCIcon>applications-system</SCIcon>
        <Include><Or>
          <SCPkgname>debion-*</SCPkgname>
          <SCAppId>org.debion.NetworkCheck</SCAppId>
        </Or></Include>
      </Menu>
    </Menu>

The Debian catalog is the AppStream metadata of the archive, which APT
downloads when the `appstream` package is installed (a dependency of
`merkadu`); `apt update` refreshes it.

## Tests and screenshots

- `merkadu-model` (`tests/test_merkadu.cpp`, `ctest`): JSON, ids and
  descriptions, catalog merging and search, PackageKit ids, the APT,
  Flatpak and snapd history parsers, launcher classification, `dpkg-query`
  output, removal plans (protected packages), settings, the departments of
  the shipped menu, snapd's JSON and HTTP framing (with a one-shot UNIX
  socket server) and the AppStream conversion of `tests/fixtures/merkadu`.
- `merkadu-ui` (`tests/test_merkadu_ui.py`, needs Xvfb and xdotool): runs
  Merkadu with the fake sources on a private X server and checks the pages,
  an installation, the history and every `--uninstall` outcome (exit codes
  0, 1 and 2) through the QA dumps.
- **Fake sources**: `MERKADU_FAKE=DIR` replaces the three backends with a
  simulation of the apps in `DIR/catalog.json` (fictional apps, icons and
  screenshots in `tests/fixtures/merkadu`): nothing is installed, removed or
  downloaded and no root or network is needed. `MERKADU_FAKE_SECONDS`
  sets how long a change takes (2.5 s), `MERKADU_FAKE_FAIL=TARGET` makes the
  changes of a package, ref or snap fail. With `SDE_PANEL_QA_DIR=DIR`,
  Merkadu writes what it shows (`Merkadu.txt`, `Merkadu-Uninstall.txt`).
  `MERKADU_LOGO_PHASE=P` freezes the logo animation at moment `P` (0 to
  3.6: stripe `n` fills between `n` and `n+1`) for screenshots.
- Other variables: `MERKADU_DATA_DIR` (instead of `/usr/share/merkadu`),
  `MERKADU_SNAPD_SOCKET`, `MERKADU_APT_HISTORY`, `MERKADU_PACKAGEKIT_BUS=session`
  (a PackageKit on the session bus), `MERKADU_FLATHUB_URL`.
- Screenshots: `tools/showcase.py` has Merkadu scenes (lobby, a department,
  an app page, search, installed apps, history, the uninstall confirmation
  and its progress), light and dark.

## Packaging

The Debian source builds a separate binary package `merkadu` (Depends:
`sde`, `packagekit`, `appstream`, `curl`; Recommends: `flatpak`; Suggests:
`snapd`); `sde-desktop` recommends it. `debian/rules` gives every path
containing "merkadu" to that package. Build dependencies: `libappstream-dev`
(>= 1.0), `libflatpak-dev` (>= 1.12), `libxml2-dev`, `zlib1g-dev`. Without
libflatpak, Merkadu is built without the Flatpak source; without
libappstream or libxml2 it is not built (`-DSDE_BUILD_MERKADU=OFF` skips it).

## Limitations

- PackageKit 1.x D-Bus API; the PackageKit 2 interface is not supported yet.
- The Debian catalog lists what the archive's AppStream data describes
  (desktop apps, console apps with metadata, fonts); other packages appear
  in Installed and History but cannot be searched for by name in the
  catalog.
- Snap search needs the network; the Snap Store's own featured lists and
  ratings are not used.
- Flatpak runtimes and extensions are installed and removed with their apps
  by libflatpak, but are not listed on their own.
- Paid apps and reviews are not supported (Ubuntu Software Center's services
  for them are gone).
