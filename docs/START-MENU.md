# The Start Menu

The Start menu of SDE (`sde-panel`, `src/panel/menu.*`) has three parts: a
rail on the left (the menu button, the account, Documents, Pictures,
Gallery, the Control Center and, apart, Power off), the applications A–Z
with "Recently added" and "Most used" above them and a category filter
("All apps") at the top, and the pinned applications as tiles in named
groups on the right. Search lives in the panel (`src/panel/search.*`).
This page describes how it behaves since 0.2 Alpha 2.

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

### Groups by category

Until the user arranges the tiles, the Start menu builds them from the
installed applications, by category, in this order: **Internet**,
**Office**, **Multimedia**, **Graphics**, **Development**, **Games**,
**Education & Science**, **System & Settings** and **Accessories**; only
the groups with applications are shown. A group takes one application per
role first, then the others (Internet: web browser, mail, chat, video
calls, file transfer, remote access, news; Office: word processor,
spreadsheet, presentation, document viewer, calendar...; System &
Settings: file manager, terminal, desktop settings, package manager,
system monitor...; Accessories: text editor, calculator, archive
manager...); within a role SDE's own applications come first, then A–Z.
At most six tiles per group, sized to fill whole rows (three medium tiles,
or a wide and a medium tile). These groups follow the installed
applications until something is changed; *Restore Default Tiles* (right-
click the menu background) returns to them.

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
  application into the group of its category — the group with that name,
  else the group holding most applications of that category (a renamed
  one), else a new group placed in the category order.

### Configuration

`[menu]` in `~/.config/sde/sde.conf`:

| Key | Value |
| --- | --- |
| `tiles` | `Name,app.desktop:m,other.desktop:w;Next group,...` — sizes `s`, `m`, `w`, `l`; names percent-escaped (`,;:%`); `;` alone: nothing pinned. Unset: the groups by category above. |
| `tiles_version` | `2` since 0.2 Alpha 2. Tiles saved before (no `tiles_version`) are migrated when read: the old default groups **Productivity** and **Explore** become category groups at their place (tile sizes kept, tiles of a category go into an existing group of that name); the user's own groups stay. The migrated groups are saved with the next change. |
| `favorites` | The pinned applications as a flat list, for older SDE versions. Read only when `tiles` is unset (alpha15 and older): each favorite goes into the group of its category. |

The model (`src/panel/menu_model.*`: `category_groups()`,
`migrate_tile_groups()`, `pin_by_category()`) has no FLTK and is covered by
`tests/test_menu_model.cpp`.

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

- `tools/panel-smoke.py --only start-menu`: the Start menu, search and
  Alpha 2 checks (typing from every place, category groups and migration,
  drags with their drop target and autoscroll, new, renamed, moved and
  removed groups, a drag from the search, Uninstall..., icon fallbacks).
  The menu describes itself in `$SDE_PANEL_QA_DIR/SDE-Menu.txt` (`@drop:`,
  `@group-drop:`, `@new-group-zone`, `group-remove:`, `@icon:` lines) and
  its popup menus in `SDE-Menu-popup.txt`.
- `tools/menu-bench.py`: opening, typing, scrolling and hover timings
  (targets: open < 80 ms, keystroke < 16 ms).
