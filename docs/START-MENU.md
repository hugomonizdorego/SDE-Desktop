# The Start Menu

The Start menu of SDE (`sde-panel`, `src/panel/menu.*`) has three parts: a
rail on the left (the menu button, the account, Documents, Pictures,
Gallery, the Control Center and, apart, Power off), the applications A–Z
with "Recently added" and "Most used" above them and a category filter
("All apps") at the top, and the user's favorite applications as tiles in
named groups on the right, some of them in folders. Applications with news
(mail, messages) show a live badge there, in the list, in the search and
on the taskbar. Search lives in the panel (`src/panel/search.*`). This page
describes how it behaves since 0.2 Alpha 3.

## Typing searches

Typing any character while the Start menu is open opens the panel search
with that text, wherever the keyboard is: the list, the tiles, the rail,
the A–Z jump grid, after the category filter was used, after a context
menu was closed — and also *while* the category filter or a context menu
is open, and while the account popup is open: the popup closes and the
search gets the text. Keys with Ctrl, Alt or Super type nothing. A letter
heading (A, B, ...) opens the jump grid only when it is clicked (or chosen
with the keyboard and Enter).

The panel's popup windows hold the pointer and keyboard grabs. FLTK
releases every grab when one of its popup menus closes, so before 0.2
Alpha 2 the Start menu lost them after its category filter or a context
menu: a click outside went to the desktop instead of closing the menu, and
the keyboard could follow it. `PanelPopup::popup_menu()` (`popup.h`) gives
the grabs and the focus back and closes a popup menu when a character is
typed in it.

## Tile groups

### Favorites

The list on the left, with its category filter, has every application by
category. The tiles are the user's favorites, in named groups — as in 0.2
Alpha 1; 0.2 Alpha 2's automatic groups by category (Internet, Office,
Multimedia...) are gone. Until the user arranges them, the tiles are a
small set made of what is installed, never every application:

| Group | Main applications (medium tiles) | Tools (small tiles) |
| --- | --- | --- |
| **Work** | the web browser, the file manager (Liria), the mail client, the text editor (Niny), the document viewer (Xanara) | Calculator, the archive manager (Taca) |
| **Photos and media** | Gallery, Photo | the video player, the music player |
| **System** | the Control Center, the software center (Merkadu) | Software Updater, System Monitor, the terminal |

Each place takes SDE's own application when it is installed, else the
application chosen in Control Center > Default Applications (the web
browser, mail, files, PDF documents, video, music, and the terminal of
`[applications] terminal`), else the best match by category (SDE's own
first, then A–Z); a place without an application is left out, and so is a
group without any. Up to four tools share the room of one medium tile and
the first tile of a row is wide where that fills the row (W M / M M M), so
the groups fill whole rows and all of them fit without scrolling at 1366×768.
They follow the installed applications and the default applications until
something is changed; *Restore Default Tiles* (right-click the menu
background) returns to them.

### Arranging

- **Drag a tile** to another place or group. While dragging, the group
  under the pointer is tinted, a bar with round ends shows where the tile
  goes, the tile follows the pointer and the tiles scroll while the
  pointer rests near their top or bottom edge.
- **Drag an application** from the A–Z list, or **from the search
  results** (the search hands the drag over to the Start menu, which opens
  under the pointer), into a group.
- **Drop on empty space** (the dashed "New group" place below the groups)
  to make a new group; its name is asked at once (Enter keeps it).
- **Click a group name** to rename it; **drag a group name** to move the
  group (a bar shows where it goes). The group's menu (right-click its
  name) has Rename, Move Up/Down and Unpin Group.
- A group without tiles shows "Drag apps here" and a **remove button**
  (×); its menu says *Remove Group*. A group whose last tile is dragged
  away or unpinned goes with it; a new group left without a name and
  without tiles is not kept.
- **Pin to Start** (in the list, the search and the panel) puts an
  application into the favorites group of its kind: **Photos and media**
  for Multimedia, Graphics and Games, **System** for System and Settings,
  **Work** for the others — the group with that name, else the group
  holding most applications of that kind (a renamed one), else the first
  group.

### Folders

Like the Start menu of Windows 10, tiles can be gathered in folders: a
folder tile shows the small icons of its applications (2×2 on a small or
medium tile, 4×2 on a wide one, 3×3 on a large one) and its name.

- **Make one**: drag a tile, an application from the list or a search
  result onto the middle of another tile. That tile lights up and the
  dragged one shrinks to its icon; dropped, the two become a folder in the
  size of the tile below, named after the category they share (Firefox and
  Thunderbird: "Internet") or without a name. It opens at once with its
  name selected: type another one and press Enter (Escape keeps it).
- **Open and shut**: a click (or Enter) opens a folder in place: below its
  row a band slides open with its applications as tiles, pointing at the
  folder, and the tiles below move down; the folder and as much of the
  band as fits come into view (scroll for the rest on a small screen). A
  click on the folder again, Escape, or a click elsewhere shuts it, gliding
  (at once with *Low resource mode*). One folder is open at a time; every
  opening of the menu starts with the folders shut.
- **Arrange**: drop a tile onto a folder to add it (last), or into the
  open folder where the bar shows (before or after one of its
  applications); drag its applications to move them inside it, or out of
  it into any group. Click the name in the open folder (or *Rename Folder*
  in its menu) to rename it.
- **Remove**: an application's menu in the open folder has *Remove from
  Folder* (it goes right after the folder) and *Unpin from Start*; the
  folder's menu has *Open Folder*, *Rename Folder*, *Resize*, *Move to
  Group*, *Ungroup Folder* (its applications get their own tiles where it
  was) and *Unpin Folder from Start*. A folder left with one application
  becomes that application's tile, in the folder's size.
- **Keyboard**: the arrows reach folders and the open folder's
  applications; Enter opens a folder on its first application, Escape
  shuts it on the folder; the Menu key (or Shift+F10) opens the menus.

Any application can be in a folder, an application only in one place.

### Configuration

`[menu]` in `~/.config/sde/sde.conf`:

| Key | Value |
| --- | --- |
| `tiles` | `Name,app.desktop:m,other.desktop:w;Next group,...` — sizes `s`, `m`, `w`, `l`; names percent-escaped (`,;:%`); `;` alone: nothing pinned. A folder is `folder:Name/a.desktop/b.desktop:m`, its name and IDs percent-escaped (`,;:%/`). Unset: the favorites above. |
| `tiles_version` | `3` since 0.2 Alpha 3 (folders). |
| `favorites` | The pinned applications as a flat list (those in folders too), for older SDE versions. Read only when `tiles` is unset (alpha15 and older): each favorite goes into the favorites group of its kind. |

**Older versions** read a folder as the tile of an application that is not
installed: they hide it and keep it when they save (and write
`tiles_version=2`, which 0.2 Alpha 3 reads as below, keeping the folder).

**Migration** (tiles saved with `tiles_version` below 3, when they are
read; saved with the next change):

- 0.2 Alpha 1 and older (no `tiles_version`): the old default groups
  **Productivity** and **Explore** give way to the favorites, at their
  place.
- 0.2 Alpha 2 (`tiles_version=2`): when the saved groups are still the
  groups by category as SDE made them — only groups named after categories,
  in their order, each with at most six medium or wide tiles of its
  category — they give way to the favorites. Once the user arranged them
  (a renamed, moved, added or emptied group, a tile moved to another
  category's group, a small or large tile), they stay as they are.
- Applications the user had pinned into the old groups (ones SDE did not
  put there itself) join the favorites group of their kind, in their size;
  the user's own groups stay where they are, and what they hold is not
  repeated among the favorites.

The model (`src/panel/menu_model.*`: `favorite_groups()`,
`category_shaped()`, `adopt_favorites()`, `pin_favorite()`, the folders'
`drop_app()`, `drop_tile()`, `take_app()`, `folder_name()`) has no FLTK and
is covered by `tests/test_menu_model.cpp`.

## Live badges

Since 0.2 Alpha 3 an application with news shows a badge with a count on
its tiles (a folder: its applications' together), its row in the A–Z list,
its search results, its pinned launcher and its taskbar buttons — in the
application's own colour: the hue most of its icon's coloured pixels
share, darkened or lightened until white (or, on light colours, dark) text
on it has a contrast of at least 4.5:1; grey icons take the accent colour.
An application that only asks for attention (urgent) shows a dot; one that
reports progress a thin bar on its tile and below its taskbar icon. The
code is `src/panel/badges.*` (live state, drawing) and
`src/panel/badge_model.*` (the rules, `tests/test_badges.cpp`).

The news comes from two places, neither of them polled; only the parts
that show a badge are repainted when one changes:

1. **SDE's notifications** (`sde-notifyd`, the panel's notification
   center): each unread notification counts for its application, found by
   its `desktop-entry` hint, the program of its sender (the process behind
   its D-Bus name: the executable, the script an interpreter runs, a
   Flatpak's ID), its application name (Name=, the program, or one name
   beginning with the other: "Telegram Desktop" for Telegram) or its icon.
   A notification counts until it is **read** or gone: seen in the
   notifications flyout (when the flyout closes, like the count on its
   bell), opened (a click on it or one of its buttons), dismissed, withdrawn
   by the application — or when a **window of its application becomes
   active** (newer notifications count again).
2. **The Unity LauncherEntry API**: the `Update` signal of
   `com.canonical.Unity.LauncherEntry` (`application://NAME.desktop` with
   `count`, `count-visible`, `progress`, `progress-visible` and `urgent`),
   which Telegram, Chromium, Chrome and their installed web apps (WhatsApp
   Web, Gmail), Electron and libunity applications send. The panel owns the
   name `com.canonical.Unity` on the session bus, as docks do, so that
   libunity applications send their counts (Unity or another dock may take
   it over). The first Update of an application is completed with its
   `Query` method (an Update carries only what changed); an application
   that leaves the bus takes its badges with it. An application ID that is
   not an installed desktop file is matched by its sender's program, like
   a notification.

An application's own LauncherEntry count, when it shows one, wins over its
unread notifications (Telegram sends both for the same messages).

The entries of `sde-notifyd`'s history (`org.sde.Notifications.GetHistory`,
`a(ussssasuubss)` since 0.2 Alpha 3) end with two more fields, the
`desktop-entry` hint and the sender's program (the panel of 0.2 Alpha 2
reads the first nine), and `MarkIdsRead(au)` marks some notifications
read.

## Uninstall

Right-clicking an application in the A–Z list, a tile or a search result
offers **Uninstall...** (last, after a divider) when Merkadu, SDE's
software center, is installed (`merkadu` in `PATH`), the application has a
desktop file and it is not one of SDE's own applications (`sde-*`, and
every desktop file SDE installs, which has a copy in
`/usr/share/sde/applications`). It runs

    merkadu --uninstall <absolute path of the .desktop file>

which asks first, shows the progress and exits 0 (removed), 1 (failed) or
2 (cancelled). After 0, once the desktop file is gone, the application's
tiles, its panel pin and its launch history are removed too; the Start
menu and the search drop it with the next application catalog snapshot
(the catalog watches the applications directories, `app_catalog.h`).

## Application icons

The Start menu, the search, the taskbar, the desktop and the Control
Center's Default Applications draw an application with
`IconLoader::app_icon()` (`src/common/icons.h`):

1. **Icon=** from the icon theme: the theme, its `Inherits=` parents,
   Adwaita, AdwaitaLegacy, hicolor, then **Papirus** for what none of them
   has (Papirus is recommended by the sde package), then
   `/usr/share/pixmaps`. An absolute path is used when it exists.
2. **Near matches** when the theme lacks Icon=: the name without a file
   extension, without `-symbolic`, in lower case and the last part of a
   reverse-DNS name (`org.gnome.Maps` → `Maps`, `maps`); then the same made
   of the desktop file ID, the program of `Exec=` (past `env`,
   interpreters, `sudo`/`pkexec` and `flatpak run <id>`) and
   `StartupWMClass=`; then SDE's own drawing `sde-app-<desktop file ID>`
   (or `sde-app-<program>`). When Icon= is a generic name of the Icon
   Naming Specification (`preferences-*`, `applications-*`,
   `utilities-*`...), these names come first: an icon drawn for the
   application itself says more than a generic one.
3. **Category icons**: the most specific category first (network settings:
   `preferences-system-network`, `network-workgroup`, `nm-device-wired`;
   terminal, file manager, browser, mail, chat, editor, calculator,
   archiver, system monitor, package manager, office documents, players,
   hardware and desktop settings...), then the main category
   (`applications-internet`..., with an icon that plain Adwaita has last).
4. `application-x-executable`.

The answer is cached per desktop entry until the icon theme changes or
applications are installed (`IconLoader::forget_missing()`); the Start
menu resolves its icons while it preloads them on idle, not while drawing.
`tests/test_app_icons.cpp` covers the chain.

SDE draws an application only where no theme draws it well:
`sde-app-nm-connection-editor` (Advanced Network Configuration, whose
Icon= `preferences-system-network` is an Ethernet port in Papirus and
missing in plain Adwaita). See [BRANDING.md](BRANDING.md).

## Tests and timing

- `tools/panel-smoke.py --only start-menu`: the Start menu, search,
  Alpha 2 and Alpha 3 checks (typing from every place, the favorites with
  the default applications and their migrations, Pin to Start, drags with
  their drop target and autoscroll, new, renamed, moved and removed groups,
  a drag from the search, Uninstall..., icon fallbacks; folders made,
  renamed, opened and shut by click, Escape and the keyboard, applications
  dropped in, moved inside and taken out, resized and ungrouped; badges
  from `notify-send` and from `sde-launcher-entry-fixture`, a fake
  LauncherEntry application: Query, progress, urgent, quitting).
  The menu describes itself in `$SDE_PANEL_QA_DIR/SDE-Menu.txt` (`@drop:`
  with the kind of drop — place, onto a tile, into the open folder — and
  the place in the folder, `@group-drop:`, `@new-group-zone`,
  `group-remove:`, `@icon:`, `@group-tiles:<group>=<id>:<size>,...`,
  `folder:`, `folder-app:`, `folder-name:`, `@folder-open:<name>` with how
  far it is open, `@folder-apps:`, `@badge:<item>=<count>[:urgent][:#colour]`
  lines) and its popup menus in `SDE-Menu-popup.txt`; the search adds
  `@badge:result:<name>=<count>`.
- `tests/test_menu_model.cpp` (favorites, migrations, Pin to Start,
  folders) and `tests/test_badges.cpp` (LauncherEntry properties, matching
  senders to applications, counts, colours and their contrast).
- `tools/menu-bench.py`: opening, typing, scrolling and hover timings
  (targets: open < 80 ms, keystroke < 16 ms).
