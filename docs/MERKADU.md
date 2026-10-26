# Merkadu, the SDE software center

Merkadu (package `merkadu`, command `merkadu`, icon `merkadu`) finds,
installs, updates and removes software from three sources in one window:

| Source | What it offers | How Merkadu talks to it |
| --- | --- | --- |
| **Debian** (the label is the distribution's name from `/etc/os-release`) | the packages of the system's APT sources: the apps of the archive's AppStream catalog, and in **All Software** every package | PackageKit over D-Bus (`org.freedesktop.PackageKit`) for changes; the AppStream catalog (DEP-11, read with libappstream) for apps; libapt-pkg (read-only) for the package lists and the simulation of marked changes |
| **Flatpak** | apps of the remotes of the system installation (Flathub is set up when Merkadu is installed) and of the user installation | libflatpak (`FlatpakInstallation`, `FlatpakTransaction`); catalog from the remotes' AppStream data |
| **Snap** | the Snap Store | snapd's REST API on `/run/snapd.socket`; **Enable Snap Support** installs snapd |

Two ways in, kept apart: **applications** by department, with the Home page,
search and app pages (Ubuntu Software Center), and **All Software**, every
package of the repositories by Debian section and status, with changes
marked first and applied together (Synaptic).

It is based on **Ubuntu Software Center** (Copyright © 2009–2013 Canonical
Ltd., GPL-3; see [THIRD-PARTY.md](THIRD-PARTY.md)): the departments and their
subcategories, the app pages, the installed apps grouped by department, the
history and the progress view follow it. Its category parser and menu are
ported to C++ (`src/merkadu/categories.*`, `data/merkadu/categories.menu`,
GPL-3.0-only). All Software follows how **Synaptic** works with packages
(sections, status filters, marking, the summary before applying); no Synaptic
code is used. The rest is new code (GPL-3.0-or-later) in SDE-FLTK with the
SDE UI kit. No Python is involved, and nothing depends on Ubuntu's online
services, which no longer exist (ratings and reviews, the software center
agent, purchases, recommendations): Highlights, Popular Apps and the
recommendations are lists the distribution chooses
([configuration](#configuration-for-distributions)), "New & Updated" comes
from the release dates in the AppStream data.

## Using it

    merkadu [--page home|installed|updates|packages|history|progress|sources]
            [--search TEXT] [--category ID] [--details APPID] [--package NAME]
    merkadu appstream://APPID
    merkadu --uninstall /usr/share/applications/APP.desktop
    merkadu --self-test [--expect REF,...] [--expect-flatpak N] [--list]
    merkadu --version | --help

The header has **Back**, **Forward** and **Home**, the page's title with a
status line below it (what the refresh is doing, or that some lists could
not be refreshed), the search field and **Refresh** (it spins while the
software lists are refreshed). The sidebar has Home, Installed, Updates (with
the number of app updates), All Software (with the number of marked
changes), History, Sources, In Progress (while changes run) and the
departments under "Applications".

- **Home**: a banner with the first highlighted app, **Highlights**,
  **Recommended for You** (well-known apps of the departments whose apps you
  use and do not have yet; nothing leaves the computer), **New & Updated**,
  **Popular Apps**, the **Categories**, a card that leads to All Software
  and, at the bottom, how many apps each source has and when the lists were
  last refreshed. While the catalog is read, the page shows its shape
  (placeholders), never an empty window. Apps named in the configuration
  that no source has are left out.
- **Departments** (Accessories, Universal Access, Developer Tools,
  Education, Science & Engineering, Fonts, Games, Graphics, Internet, Sound
  & Video, Office, Themes & Tweaks, System, Books & Magazines) with their
  subcategories and app counts.
- **Search** (Ctrl+F) covers every source at once. Each result shows badges
  for the sources that offer it; chips filter by source. Enter also asks the
  Snap Store, which is searched online. On All Software the field searches
  the packages, by name and description, in place.
- **App page**: icon, name, developer, summary, a chip per source (the one
  to install from is chosen with them), Install/Open/Update/Remove, the
  screenshots (click to enlarge; "Screenshot not available" when one cannot
  be downloaded), the description, "What's New" (the latest release notes),
  and a details card: version, download and installed size, license, source,
  isolation (Flatpak sandbox, snap strict or classic confinement, or "not
  isolated" for Debian packages), developer, department, links and, for
  Debian apps, the package in All Software. When the chosen source cannot be
  used, the page says why (PackageKit not running, Flatpak or snapd not
  installed) and offers the fix.
- **Installed**: installed apps by department, filtered by source.
- **Updates**: the updates of apps from Flathub (and other Flatpak remotes)
  and the Snap Store, with **Update All**; see
  [Updates and the Software Updater](#updates-and-the-software-updater).
- **History**: installations, updates and removals by day, from
  `/var/log/apt/history.log` (and its rotated copies), `flatpak history` and
  snapd's changes, with source filters.
- **In Progress**: the queue of changes. Changes run one after the other;
  the running one shows the animated SDE logo, its status and percentage and
  can be cancelled when its source allows it. Finished and failed changes
  stay listed until "Clear Finished".
- **Sources**: the state of the three sources and of the Debian catalog
  (whether APT downloads it, how many catalogs and icon archives there are,
  when the lists were refreshed, the errors of the last refresh), **Set Up
  Flathub** when the system installation has no Flathub remote, **Install
  the Catalog** (`appstream`) or **Install Flatpak** when they are missing,
  **Enable Snap Support**, and where snap data is kept (see
  [the snap folder](#the-snap-folder)).

Keys: Ctrl+F or Ctrl+K search; Alt+Left, Backspace or Escape go back,
Alt+Right forward, Alt+Home home; F5 or Ctrl+R refresh the software lists;
F1 opens About Merkadu; Ctrl+Q quits. In the package list: arrows, Page Up,
Page Down, Home and End move, Space marks the most likely change (or
unmarks), `+` or Insert marks for installation (or update), `-` or Delete
marks for removal, Enter or a double click opens the package, the Menu key
or a right click opens the marking menu.

## The software lists and the refresh

Merkadu shows what it has at once (the catalogs and lists of the last
refresh) and refreshes the software lists in the background when it starts
(`[Refresh] on-start=true`), with the progress in the header:

- **Debian**: PackageKit's `RefreshCache`, which runs `apt update`: the
  package lists and, with them, the archive's AppStream catalog (DEP-11,
  `/var/lib/apt/lists/*_dep11_Components-*.yml.gz`) and its icons
  (`*_dep11_icons-*.tar.gz`). appstream's APT hook then runs `appstreamcli
  refresh`, which unpacks the icons and links the catalogs where libappstream
  reads them (`/var/lib/swcatalog`). The refresh at start asks for nothing
  (PackageKit lets the active user refresh); when an administrator must
  allow it, when the computer is offline or APT is busy, the page says so
  and keeps the old lists.
- **Flatpak**: the AppStream data of every enabled remote of the system and
  user installations (`flatpak update --appstream`, through libflatpak).
- **Snap**: nothing to download; the store is asked when you search.

Lists younger than `[Refresh] min-age=` minutes (30) are not downloaded again
at start. The Debian lists are refreshed at start whatever their age when the
catalog is missing: no DEP-11 catalog at all, or no icon archive although
Merkadu's `/etc/apt/apt.conf.d/60merkadu` asks for them (both happen right
after `appstream` or `merkadu` was installed, before the next `apt update`).
**Refresh** always refreshes everything. Afterwards the catalog is read again
and every page updated.

DEP-11 catalogs that AppStream did not link yet (`appstreamcli refresh` did
not run) are read by Merkadu from links in `~/.cache/merkadu/dep11`.

### Why Merkadu could be empty

On a Debian testing installation, Merkadu 0.2 Alpha 2 showed the departments
and an empty page. The causes, and what Merkadu does about them now:

1. **No catalog yet.** AppStream reads the archive's catalog only once APT
   downloaded it and `appstreamcli refresh` ran; `appstream` came with
   `merkadu`, after the last `apt update`, so there was nothing to read until
   the next refresh. Merkadu now refreshes the lists itself (above) and reads
   unlinked catalogs on its own.
2. **No icons.** Debian's `50appstream` downloads the DEP-11 icon archives
   only on request (`DefaultEnabled "false"`), so the catalog's cached icons
   were missing and libappstream dropped them. `merkadu` installs
   `/etc/apt/apt.conf.d/60merkadu`, which turns on the 48×48 and 64×64
   archives (`DEP-11-icons-small`, `DEP-11-icons`); icons whose files are
   missing are downloaded from the catalog's media server, and every app has
   at least a generic icon (see [icons](#app-icons)).
3. **Errors were swallowed.** A missing catalog, a stopped PackageKit or a
   failed refresh left the page empty. Now an empty catalog is explained on
   Home ("The software lists have not been downloaded yet", "The software
   catalog is not installed", "No apps were found") with **Refresh Software
   Lists**, **Install the Catalog**, **All Software** and **Sources**, and
   notices name what is missing (PackageKit, the catalog, Flatpak, Flathub)
   with the button that fixes it.
4. **No Flathub.** Flathub was only offered on the Sources page; it is now
   set up when Merkadu is installed (below).
5. **Flatpak apps missing from menus.** The session did not always have the
   Flatpak export directories in `XDG_DATA_DIRS` (Flatpak installed after
   login, or a session not started through `Xsession`). `startsde` and
   `sde-session` now put `~/.local/share/flatpak/exports/share`,
   `/var/lib/flatpak/exports/share` (and the other installations of `flatpak
   --installations`) first, add `/var/lib/snapd/desktop` and `/snap/bin`, and
   pass `XDG_DATA_DIRS` to D-Bus activated services.

`merkadu --self-test` prints the same diagnosis without a window (see
[Diagnosis](#diagnosis-merkadu---self-test)); Merkadu also logs it in one line
on standard error when it reads the catalog.

## All Software

All Software is for everything that is not an app (libraries, tools,
fonts, drivers) and for people who know Synaptic:

- **Status** chips: All, Installed, Updates, Not Installed, Marked, and
  Settings Left (removed packages whose configuration files are still there),
  each with its count.
- **Sections**: Debian's sections with readable names ("libdevel" is
  "Libraries - Development"; `contrib/` and `non-free/` packages are in the
  same sections), with counts for the status; a list beside the packages on
  wide windows, a menu on narrow ones.
- **The list**: icon (the app's, else one for the section), name, summary,
  installed and latest version, size and a status column (installed, update,
  marked change, "system"); the selected package's details below it, with
  the buttons to mark it.
- **Search**: the field in the header searches name and description in
  place; names first, then the start of a name, then the description.
- **Marking**: Mark for Installation, Mark for Update, Mark for Removal and
  Unmark, from the buttons, the right-click menu or the keys. The bar at the
  bottom counts the marked changes; **Review and Apply…** opens the review.
- **Review**: APT's simulation of all marked changes together
  (`pkgDepCache` with `pkgProblemResolver`, the way Synaptic resolves them):
  what is installed, updated, downgraded and removed, the packages that come
  "with the others", the download size and the disk space used or freed.
  **Apply Changes** runs them with PackageKit, removals first, then
  installations, then updates, one after the other in In Progress.
- **Package page** (`--package NAME`, Enter or Details): summary,
  description, version, origin, section, priority, sizes, the dependencies
  by kind (Depends, Recommends, Suggests, Conflicts, Breaks, Replaces, …),
  the installed packages that need it, what it provides and every available
  version with its origin and pin priority.

**What cannot be removed.** Packages the system needs are never removed:
Essential packages, Priority required packages and the running kernel's
package. Neither are SDE's own packages (`sde`, `sde-desktop`, `merkadu`
and `[Uninstall] protected=`). Their Mark for Removal is not offered (the
page says why), and a review whose removals would take one of them away
cannot be applied: it names the package and the reason ("part of the base
system: the computer may not start or work without it", "removed together
with the desktop, not on its own"). Everything else, preinstalled or not,
can be removed; the review and the uninstall window list what goes with it.

The package lists are read with libapt-pkg on a worker thread (about 2
seconds for 88,000 packages); Merkadu never writes APT's state itself.

## Updates and the Software Updater

Since 0.2 Alpha 3 the work is split without overlap:

- **Software Updater** (`sde-update`) updates the **operating system**:
  every Debian package, with its critical, security and main groups. It no
  longer has a Flatpak group. When everything is up to date it says that
  apps from Flathub and the Snap Store are updated in Merkadu and offers
  **Open Merkadu** (`merkadu --page updates`).
- **Merkadu's Updates** page lists the updates of **apps** from Flatpak
  remotes and the Snap Store, with **Update All**. Below them a **System
  Updates** card says how many Debian packages can be updated (naming the
  apps among them) and offers **Open Software Updater**. A Debian app can
  still be updated alone from its own page (or marked in All Software), but
  there is no second "update everything" for the system.

## App icons

Every app and package has an icon, in this order:

1. the icon theme's own drawing of the app's icon name;
2. the catalog's icons of at least the size drawn (90 % is enough), the
   smallest first, local files before downloads: the DEP-11 icons APT
   downloaded and AppStream unpacked (`/var/lib/swcatalog/icons`, also
   looked up by origin when libappstream lost the path), the icons of the
   Flatpak remotes' AppStream data, the catalog's media server
   (`MediaBaseUrl`, downloaded once to `~/.cache/merkadu/images`), the Snap
   Store's icon URLs;
3. smaller icons of the catalog, the largest first;
4. an icon for the app's kind (fonts, add-ons, console tools), its main
   freedesktop category or its Debian section, then a generic application
   icon;
5. at worst a tile in a color of the app's name with a package symbol:
   never an empty square.

Packages without an app use the icon of their section.

## Flathub

The `merkadu` package depends on `flatpak`, and its `postinst` sets up
Flathub for the system installation on new installations and on the upgrade
from an earlier version:

    flatpak remote-add --system --if-not-exists flathub /usr/share/merkadu/flathub.flatpakrepo

`/usr/share/merkadu/flathub.flatpakrepo` is Flathub's official repository
file (`https://dl.flathub.org/repo/flathub.flatpakrepo`, SHA-256
`3371dd250e61d9e1633630073fefda153cd4426f72f4afa0c3373ae2e8fea03a`): the URL
and the public key that signs the repository, so no download is needed. A
distribution that does not want Flathub writes `setup-flathub=false` in the
`[Flatpak]` section of `/etc/merkadu/merkadu.conf` before installing
Merkadu; this also hides the offers to set it up. When Flathub is missing
(removed, or the setup failed without network), Home and Sources offer **Set
Up Flathub**, which uses the same file (or `flathub-url`). Flathub's catalog
is downloaded by the first refresh.

## Snap

Snap is off until the user enables it: with snapd missing, the Snap source
is "not installed" and Sources (and app pages of snaps) offer **Enable Snap
Support**. It asks once, then runs two changes in In Progress:

1. installs the `snapd` package from the distribution (PackageKit; the
   administrator password is asked for);
2. installs the `snapd` snap from the Snap Store, as Debian's instructions
   describe. snapd needs a moment after its installation (it does not
   listen yet, or answers "too early" while it seeds itself): Merkadu shows
   "Waiting for snapd to start" and asks again every three seconds for up to
   two minutes.

No terminal is needed. Afterwards Merkadu explains that the computer should
be restarted (or the user log out and back in): only a new session has
`/snap/bin` in `PATH` and `/var/lib/snapd/desktop` in `XDG_DATA_DIRS`, which
snap commands and the menu entries of snaps need (SDE's session adds both).

### The snap folder

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
packages the system needs (Essential, Priority required, the running kernel)
are not removed; an app whose removal would also remove one of them (for
example a library the desktop or the system needs) is refused, naming the
package; a launcher no package owns outside the user's home cannot be removed
by Merkadu. Preinstalled apps that are none of these can be uninstalled like
any other.

## The progress animation

While a change runs, Merkadu shows the SDE mark (the monitor of the SDE
logo, `sde-mark.svg` from the branding directory) with its three red, green
and blue stripes filling one after the other and the power LED flickering.
The layers of the SVG are rendered once per size and composed at 30 frames
per second only while a change runs (about 2 % of one CPU core, measured on
Xvfb at 1366×768; nothing when idle). When the user turned the desktop
effects off (Control Center → Appearance → Effects; reduced motion), the logo
stays still. The widget is `src/merkadu/logo_anim.*`.

## Architecture

    main.cpp ─┬─ window.cpp, pages.cpp, ui.cpp   main window, pages, widgets
              ├─ packages_ui.cpp                 All Software, package page, review
              ├─ uninstall.cpp                   --uninstall dialog
              ├─ selftest.cpp                    --self-test (no display)
              └─ store.cpp  ── Store (singleton, FLTK thread)
                   │  catalog, refresh, package lists and marks, jobs
                   │  queue, history, events → pages
                   ├─ packagekit.cpp   Backend: PackageKit (D-Bus, async)
                   ├─ flatpak.cpp      Backend: libflatpak (worker threads)
                   ├─ snap.cpp         Backend: snapd REST (worker threads)
                   └─ fake.cpp         Backend: MERKADU_FAKE (tests)
    merkadu-core (no display; unit-tested):
      model.*            App, Origin, Catalog (merging), search, icons,
                         history parsers, launcher classification, removal
                         plans, settings (merkadu.conf)
      packages.*         All Software: sections, filters, marks, plans
      apt_cache.*        libapt-pkg: the package lists, details and the
                         simulation of marked changes (one worker thread)
      categories.*       departments (Ubuntu Software Center's menu format)
      appstream_catalog.* AppStream components → apps (libappstream); the
                         diagnosis of the catalog sources
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
  D-Bus on the same loop (`sde::dbus`); AppStream loading, libapt-pkg,
  libflatpak transactions, snapd requests, `dpkg-query` and `flatpak history`
  run on worker threads and hand their results back with `Fl::awake()`.
- **Jobs.** Install, remove and update requests become jobs in one queue;
  one runs at a time, with progress events to the pages and the uninstall
  dialog; a job can wait for another (Apply Changes, Enable Snap Support).
  Cancel maps to PackageKit's `Cancel`, a `GCancellable` for libflatpak and
  `abort` of the snapd change.
- **Departments.** `categories.menu` uses Ubuntu Software Center's format
  (`<Menu>`, `<Name>`, `<SCIcon>`, `<Include>` with `<And>`, `<Or>`,
  `<Not>`, `<Category>`, `<SCSection>`, `<SCPkgname>`, `<SCType>`,
  `<SCAppId>`, `<SCSource>`, `<OnlyUnallocated/>`). Snaps carry store
  categories, which are mapped to freedesktop categories.

## Permissions

Merkadu runs as the user and never as root. Reading (catalogs, package lists,
installed apps, history) needs no authorization. Changes are authorized by
polkit, whose dialog in SDE is the SDE authentication agent:

| Change | Who asks | polkit action |
| --- | --- | --- |
| refresh the package lists | PackageKit (`RefreshCache`; not interactive at start) | `org.freedesktop.packagekit.system-sources-refresh` |
| install, remove, update Debian packages (also Apply Changes, the snapd package) | PackageKit (`SetHints interactive=true`) | `org.freedesktop.packagekit.package-install`, `package-remove`, `system-update` |
| install, remove, update Flatpaks of the system installation; set up Flathub; refresh its catalogs | the Flatpak system helper | `org.freedesktop.Flatpak.app-install`, `app-uninstall`, `app-update`, `runtime-install`, `modify-repo`, `appstream-update` |
| install, remove, refresh snaps | snapd (`X-Allow-Interaction: true`) | `io.snapcraft.snapd.manage` |
| hide the snap folder | snapd (`PUT /v2/snaps/system/conf`) | `io.snapcraft.snapd.manage-configuration` |

Apps installed for the user (Flatpak `--user`) need no authorization.
Merkadu never stores a password and never calls `sudo` or `pkexec`.

## Configuration for distributions

`/usr/share/merkadu/merkadu.conf` holds the defaults. Keys in
`$XDG_CONFIG_DIRS/merkadu/merkadu.conf` (for example
`/etc/xdg/merkadu/merkadu.conf`) and then `/etc/merkadu/merkadu.conf`
replace the same keys of the earlier files; `MERKADU_CONFIG=FILE` uses one
file instead of all of them.

    [Highlights]
    # the Home banner (first app found) and the Highlights row, in order
    apps=org.gimp.GIMP;org.inkscape.Inkscape;deb:firefox-esr|org.mozilla.firefox|deb:firefox

    [Popular]
    # "Popular Apps"
    apps=deb:firefox-esr|org.mozilla.firefox;deb:libreoffice-writer|org.libreoffice.LibreOffice;org.videolan.VLC

    [Recommended]
    # "Recommended for You": department id = well-known apps of it, shown to
    # people who have apps of that department and not these yet
    graphics=org.gimp.GIMP;org.inkscape.Inkscape;org.kde.krita
    sound-video=org.videolan.VLC;org.audacityteam.Audacity;com.obsproject.Studio

    [New]
    # how many "New & Updated" apps (newest AppStream releases); 0 hides it
    count=12

    [Refresh]
    # refresh the software lists when Merkadu starts, unless they are younger
    # than min-age minutes
    on-start=true
    min-age=30

    [Sources]
    # preferred source when an app is in several
    order=debian;flatpak;snap

    [Flatpak]
    # false: the package does not set up Flathub, and Merkadu does not offer it
    setup-flathub=true
    # the repository file "Set Up Flathub" uses when the shipped one is missing
    flathub-url=https://dl.flathub.org/repo/flathub.flatpakrepo

    [Snap]
    # hide ~/snap as ~/.snap/data (see above)
    hide-home-folder=true

    [Uninstall]
    # packages never removed besides sde, sde-desktop, merkadu and the
    # packages the system needs
    protected=

Apps are named by their AppStream id, which finds them in every source, or
by `deb:PACKAGE`, `flatpak:APPID` or `snap:NAME`; alternatives separated by
`|` are tried in order, because Debian and Flathub do not always use the
same id; where they differ the shipped lists put the distribution's package
first (`deb:firefox-esr|org.mozilla.firefox|deb:firefox`). Names that are
in no catalog are skipped, so a list can name apps of sources the system
does not have. The shipped lists name real apps: Firefox, Thunderbird,
LibreOffice, GIMP, Inkscape, Krita, Blender, Kdenlive, OBS Studio,
Audacity, VLC, Shotwell and others. `[Featured]` and `[Top]` (0.2 Alpha 2)
are still read when `[Highlights]` and `[Popular]` are missing.

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

## Diagnosis: merkadu --self-test

`merkadu --self-test` loads what the window would show, without a display,
and prints where it comes from:

    AppStream catalog: 2327 apps (2329 Debian, 0 Flatpak components) in 0.4 s
      APT downloads the catalog (appstream's 50appstream): yes
      DEP-11 catalogs: 15 downloaded, 15 read by AppStream, 0 read from Merkadu's own links
      icon archives downloaded: 0; catalogs with unpacked icons: 0; other catalog files: 0
      last refresh of the package lists: 2026-10-26 02:59
      Flatpak remotes: none; with AppStream data: none
      icons: 0 files, 522 downloaded from the catalog's media, 1192 of the icon theme, 613 generic
    Packages (libapt-pkg): 87831 packages, 1397 installed, 87 with an update, 58 sections in 2.1 s

(Ubuntu 24.04 lists in a container without the icon archives.) `--expect
REF,...` checks that apps are there with an icon (references as in the
configuration), `--expect-flatpak N` that at least N Flatpak apps are, and
`--list` prints every app with its icon; `--simulate
install:PKG,remove:PKG,update:PKG` prints APT's plan for marked changes.
The exit status is 0 when everything expected was found. CI runs it in
Debian testing with the real archive and Flathub (see `.github/workflows/build.yml`):

    merkadu --self-test --expect deb:firefox-esr,deb:gimp,deb:vlc,org.mozilla.firefox --expect-flatpak 1

## Tests and screenshots

- `merkadu-model` (`tests/test_merkadu.cpp`, `ctest`): JSON, ids and
  descriptions, catalog merging and search, PackageKit ids, the APT,
  Flatpak and snapd history parsers, launcher classification, `dpkg-query`
  output, removal plans (protected and system packages, the explanations),
  settings (including app alternatives), the departments of the shipped
  menu, snapd's JSON and HTTP framing (with a one-shot UNIX socket server),
  the AppStream conversion of `tests/fixtures/merkadu`, the icon order and
  fallbacks, All Software's sections, filters, search, marks and plans, and
  the catalog diagnosis on a fake root.
- `merkadu-ui` (`tests/test_merkadu_ui.py`, needs Xvfb and xdotool): runs
  Merkadu with the fake sources on a private X server and checks, through
  the QA dumps, Home, the refresh at start and with Refresh, a department,
  the search, an app page, an installation, Installed, History, Sources, All
  Software (filters, a protected system package, the search, marking an
  update and an installation, the review with what comes with them, Apply),
  Back, Forward and Home, the Updates page, an empty catalog, every
  `--uninstall` outcome (exit codes 0, 1 and 2) and the window at 1024×600
  and 150 %. `SDE_XVFB_DISPLAYS=FIRST-LAST` keeps its X servers in a range.
- **Fake sources**: `MERKADU_FAKE=DIR` replaces the three backends and the
  package lists with a simulation of the apps and packages in
  `DIR/catalog.json` (fictional apps, icons and screenshots in
  `tests/fixtures/merkadu`): nothing is installed, removed or downloaded and
  no root or network is needed. `MERKADU_FAKE_SECONDS` sets how long a change
  takes (2.5 s), `MERKADU_FAKE_FAIL=TARGET` makes the changes of a package,
  ref or snap fail and `MERKADU_FAKE_HOLD=PERCENT` stops them at that point
  (screenshots of progress). `MERKADU_FAKE_REFRESH=SECONDS` sets how long the
  refresh takes (1 s) and `MERKADU_FAKE_REFRESH_FAIL=debian|flatpak|snap`
  makes one source's refresh fail. `MERKADU_FAKE_EMPTY=1` gives an empty
  catalog (as before the first refresh), `=all` also no installed apps. With
  `SDE_PANEL_QA_DIR=DIR`, Merkadu writes what it shows (`Merkadu.txt`,
  `Merkadu-Uninstall.txt`). `MERKADU_LOGO_PHASE=P` freezes the logo
  animation at moment `P` (0 to 3.6: stripe `n` fills between `n` and `n+1`)
  for screenshots.
- Other variables: `MERKADU_DATA_DIR` (instead of `/usr/share/merkadu`),
  `MERKADU_SNAPD_SOCKET`, `MERKADU_APT_HISTORY`, `MERKADU_APT_LISTS` (instead
  of `/var/lib/apt/lists`, for the age of the lists),
  `MERKADU_PACKAGEKIT_BUS=session` (a PackageKit on the session bus),
  `MERKADU_FLATHUB_URL`.
- Screenshots: `tools/showcase.py` has Merkadu scenes (Home, a department,
  an app page, search, installed apps, history, progress, All Software with
  marked changes, the review, app updates, the uninstall confirmation and its
  progress), light and dark.

## Packaging

The Debian source builds a separate binary package `merkadu` (Depends:
`sde`, `packagekit`, `appstream`, `flatpak`, `curl`; Suggests: `snapd`);
`sde-desktop` recommends it. It installs the conffile
`/etc/apt/apt.conf.d/60merkadu` (the DEP-11 icon archives) and
`/usr/share/merkadu/flathub.flatpakrepo`, and its `postinst` sets up
Flathub ([above](#flathub)). `debian/rules` gives every path containing
"merkadu" to that package. Build dependencies: `libappstream-dev` (>= 1.0),
`libflatpak-dev` (>= 1.12), `libapt-pkg-dev`, `libxml2-dev`, `zlib1g-dev`.
Without libflatpak, Merkadu is built without the Flatpak source; without
libapt-pkg, without All Software; without libappstream or libxml2 it is not
built (`-DSDE_BUILD_MERKADU=OFF` skips it).

## Limitations

- PackageKit 1.x D-Bus API; the PackageKit 2 interface is not supported yet.
- All Software reads APT's lists itself but applies changes through
  PackageKit, one batch of removals, installations and updates after the
  other; PackageKit resolves their dependencies again, which can differ from
  the review when the lists changed in between.
- Snap search needs the network; the Snap Store's own featured lists and
  ratings are not used.
- Flatpak runtimes and extensions are installed and removed with their apps
  by libflatpak, but are not listed on their own.
- The DEP-11 icon archives come with the next `apt update` after Merkadu is
  installed (Merkadu's refresh at start does it); until then the catalog's
  icons are downloaded from its media server, or generic icons are shown
  offline.
- Paid apps and reviews are not supported (Ubuntu Software Center's services
  for them are gone).
