# Liria

Liria (`liria`, `src/liria`) is the file manager of SDE. Since 0.2.0
Alpha 4 it is SDE's own program, written from scratch by the Selviana DE
Project in C++17 on SDE-FLTK, cairo and GIO; up to 0.2.0 Alpha 3.1 it was
a fork of Caja (GTK 3, the MATE libraries), and nothing of that fork is
left. Its version is 2.0.0~alpha1 (`suite/versions.json`). Its window
follows the File Explorer of Windows 11, its design reference; the manual
page is `liria(1)` (`data/man/liria.1`).

## The window

From top to bottom:

- **Tabs in the title bar.** Liria draws its own title bar: the tabs, a
  **+**, and Minimize, Maximize and Close. Dragging the strip moves the
  window, a double click maximizes it, the window's edges resize it, and
  the window manager's menu and keys work as for any window. A tab's
  menu has New tab, Duplicate tab, Close tab, Close other tabs and Close
  tabs to the right; the middle button closes a tab and tabs move by
  dragging. `[liria] titlebar = system` in `~/.config/sde/sde.conf` (or
  `LIRIA_SYSTEM_TITLEBAR=1`) keeps the window manager's title bar instead.
- **Back, Forward, Up and Refresh**, then the **address bar**: breadcrumbs
  of the folder (from the home folder, a drive or a server), the **›**
  after each listing the folders inside it; a click on the empty part (or
  Ctrl+L, Alt+D, F4) edits the address, which takes paths and network
  addresses (below) and suggests folders and the servers opened before.
  Then the **search box** ("Search Documents"): what is typed filters the
  folder at once and then searches its subfolders in the background.
- **The command bar** instead of a ribbon: **New ▾** (folder, text
  document, the documents of the Templates folder), **Cut, Copy, Paste,
  Rename, Share** (copy the path, send by email, send to a drive), **Delete**, **Sort ▾**,
  **View ▾** (extra large, large, medium and small icons, compact,
  details; Show ▸ the navigation pane, hidden items, the Network locations
  and Recent files of Home) and **⋯** (select all, select none, invert the
  selection, Open in Terminal, Open as administrator, Connect to server,
  Empty Trash, Ask before moving items to the Trash, Properties, About).
  Commands that apply only to what is selected or shown appear only then:
  **Extract all** for archives (Taca), **Install** for Debian packages
  (Debrun), **Run** for AppImages (sde-appimage), **Restore** and **Empty
  Trash** in the Trash, **Connect to server** in Network, **Eject** (or
  Unmount) at the top of a removable drive.
- **The navigation pane**: Home, Trash, **Pinned** (the Quick access pins,
  each with its pin mark), **Drives** (with an eject button for removable
  ones) and **Network** with the open shares below it. Places take drops,
  and a folder dropped between the pins is pinned there. Its width is
  dragged at its edge.
- **The files** on a card, and the **status bar**: how many items, how
  many are selected and their size, the free space, and the details and
  icons view buttons.

## Home

Home is the first page of a new window and of a new tab, as in Windows 11:

- **Quick access**: only the folders, files and places the user pinned.
  Liria pins nothing by itself (no frequent folders, nothing taken from
  the recent files). Without a pin file the defaults are Desktop,
  Downloads, Documents, Pictures, Music and Videos (the XDG folders that
  exist); the file `~/.config/sde/liria-quick-access` is written only when
  the user pins, unpins, renames, moves or resets a pin, and an empty file
  means that everything was unpinned. The pins of the earlier Liria are
  read from the same file. Pin to Quick access is in every folder's menu.
- **Drives**: each with a bar of how full it is (accent color, red when
  nearly full) and its free space; a click opens it, mounting it first.
- **Network locations**: the shares that are mounted and the servers
  opened before. When there are none the section says so quietly and
  offers to turn itself off (View ▸ Show brings it back).
- **Recent files**: the recently used files of every program
  (`recently-used.xbel`), and Clear recent files in its menu.

Every section folds; what is folded and turned off is remembered.

## Folders

- **Icons** with thumbnails (the pictures gdk-pixbuf reads, through the
  freedesktop.org thumbnail cache shared with other programs), **details**
  (Name, Date modified, Type, Size; the Trash adds the original place and
  the date deleted; a click on a column sorts, again reverses) and
  **compact**. Ctrl+mouse wheel and Ctrl+plus/minus change the icon size;
  Ctrl+Shift+1 to 6 are the views of Windows Explorer. Each folder keeps
  its own layout and sorting (`~/.local/state/sde/liria-views`, the 500
  folders used last), the others follow the last choice.
- **Selecting**: click, Ctrl+click, Shift+click, the rubber band (with
  Ctrl it adds), the arrow keys, Home, End, Page Up/Down, Shift and Ctrl
  with them, Ctrl+A, Ctrl+I (invert), and typing the start of a name.
- **Opening**: double click or Enter opens with the default application
  (`mimeapps.list`, as Control Center ▸ Default Applications sets it);
  **Open with ▸** lists the applications for the type and Choose another
  application, which can make the choice the default. Folders open in the
  same tab, with the middle button or Ctrl in a new tab.
- **Copying and moving** (Ctrl+C, Ctrl+X, Ctrl+V; dragging, with Ctrl to
  copy, Shift to move and Ctrl+Shift to link, the default chosen as in
  Windows: a move on the same drive, a copy to another) run on SDE's file
  engine (`src/desktop/file_ops.h`): the old item stays until the new one
  is complete, and copies to USB sticks are written safely and announced
  only when their data is on the stick (`src/common/safe_copy.h`). An
  operation that takes more than half a second shows SDE's copy dialog
  (the penguin carries the papers; Pause, Cancel, the speed and the time
  left; `docs/TRANSFERS.md`). An item whose name is taken asks: Replace,
  Keep both ("name (2)"), Skip, or for two folders Merge, each "for all
  the others". A copy into its own folder is "name (2)". The clipboard is
  the desktop's (`x-special/gnome-copied-files`, `text/uri-list`), so files
  are pasted between Liria, the desktop and other file managers, and drag
  and drop works with every XDND program.
- **Renaming** in place (F2; the name without its extension is selected,
  as in Windows). **New folder** (Ctrl+Shift+N) and **New ▸** documents
  start renaming at once.
- **The Trash** of the freedesktop.org specification, shared with GTK and
  Qt programs: files go to the trash of their own drive (a USB stick keeps
  its own `.Trash-1000`), so deleting is instant; Restore puts items back
  where they were (making the folders again, "name (2)" when the name is
  taken), Empty Trash and Delete in the Trash delete for good after a
  question. Delete moves to the Trash without asking (as Windows 11 does;
  `[view] confirm_trash` asks first); Shift+Delete deletes for good after a
  question, and so does Delete where there is no trash (read-only drives,
  network shares).
- **Undo** (Ctrl+Z): the last 30 renames, moves, copies, new items and
  deletions to the Trash.
- **Properties** (Alt+Enter): name, type, location, size (counted in the
  background), dates, owner, permissions with "Allow running it as a
  program", and the application it opens with (Change…). For several items
  the total.
- SDE's own commands appear when they apply: Compress… and Extract all
  (Taca), Install with Debrun, Run (sde-appimage checks the AppImage
  first), Edit with ▸ for pictures, Set as wallpaper, Send to ▸ a removable
  drive, Open in Terminal (the default terminal) and Open as administrator
  (below).

## Drives and the network

Drives come from GIO's volume monitor (UDisks2): mounting on a click,
Unmount and Eject, with GIO's questions (which programs keep the drive
busy) in Liria's dialogs.

The address bar takes:

| Typed | Opened |
| --- | --- |
| `\\server\share\folder` | `smb://server/share/folder` (as in Windows) |
| `smb:`, `\\` | the Windows computers of the network |
| `smb://`, `sftp://`, `ssh://`, `ftp://`, `ftps://`, `dav://`, `davs://`, `webdav://`, `nfs://`, `afp://` | as typed, cleaned up |
| `192.168.1.10`, `[fe80::1]`, `localhost`, `fileserver`, `anna@nas`, `nas:445` | the shares of that computer (other protocols offered) |
| `/path`, `~/path`, a name | a folder, or an item of the folder shown |
| `C:\Users` | an explanation: drives are under Drives |

Network places are opened with GIO and GVfs (`gvfs-backends`). A server
that asks for a password gets Liria's own sign-in window: user, domain
for Windows shares, password, "Remember the password in the keyring" only
when a Secret Service keyring runs (GNOME Keyring, KeePassXC), and a
warning for FTP and WebDAV without TLS. A password typed in the address is
taken out of it and used for the sign-in, never shown or stored. GVfs's
questions (an unknown SSH host key) come in Liria's dialogs. Mounted shares
are listed through GVfs's FUSE folders (`$XDG_RUNTIME_DIR/gvfs`, package
`gvfs-fuse`) as local folders, so copying uses the same engine; without
FUSE Liria lists them through GIO. Connect to Server (⋯) takes an address,
marks the protocols this computer cannot open ("not installed") and lists
the recent servers (`~/.config/sde/liria-servers`, mode 0600, the last
30, never a password).

**Open as administrator** runs `sudo liria` in a terminal; as root, Liria
starts its own D-Bus session, so it never hands its folders to the user's
Liria.
`/etc/sudoers.d/sde-liria` keeps `DISPLAY` and `XAUTHORITY` for
`/usr/bin/liria` only, so that the root Liria can open its window; it
grants no permission (who may use sudo stays the system's policy).

## Keys

| Keys | Action |
| --- | --- |
| Ctrl+N | New window |
| Ctrl+Shift+N | New folder |
| Ctrl+T, Ctrl+W | New tab (Home), close the tab (the last one closes the window) |
| Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+Page Down/Up | Next, previous tab |
| Alt+Left, Alt+Right, Backspace | Back, forward, back |
| Mouse back and forward buttons | Back, forward |
| Alt+Up | The folder above |
| Alt+Home | Home |
| Ctrl+L, Alt+D, F4 | Edit the address |
| Ctrl+E, Ctrl+F, F3 | The search box |
| F5, Ctrl+R | Refresh |
| F2 | Rename |
| Alt+Enter | Properties |
| Enter | Open |
| Delete | Move to the Trash |
| Shift+Delete | Delete permanently (after a question) |
| Ctrl+A, Ctrl+I | Select all, invert the selection |
| Ctrl+C, Ctrl+X, Ctrl+V | Copy, cut, paste |
| Ctrl+Z | Undo |
| Ctrl+H | Show or hide hidden items |
| Ctrl+mouse wheel, Ctrl+plus, Ctrl+minus, Ctrl+0 | Larger, smaller, normal icons |
| Ctrl+Shift+1 to 4 | Extra large, large, medium, small icons |
| Ctrl+Shift+5, Ctrl+Shift+6 | Compact, details |
| Shift+F10, Menu | The menu of the selection (or of the folder) |
| F10 | The ⋯ menu |
| F11 | Full screen |
| Escape | Stop a search or renaming, close a menu |

## The command line and D-Bus

```
liria [--new-window] [--select] [URI|PATH...]
```

Each location opens as a tab of one new window; a file given as a location
is shown selected in its folder, and `--select` (`-s`) does that for
folders too. `-n` starts without a window, `-g WxH` sets its size, `-q`
quits the running Liria. The options of the earlier Liria are accepted:
`--no-desktop` and `--force-desktop` (SDE draws its own desktop),
`--browser`, `-t`, `-w`.

One Liria runs per session. It owns `org.debion.Liria`
(`/org/debion/Liria`: `Open(as locations, as select)`, `Quit()`); a second
`liria` hands its locations over and ends (in about 20 ms). Liria is also
the session's `org.freedesktop.FileManager1`
(`/org/freedesktop/FileManager1`: `ShowFolders`, `ShowItems`,
`ShowItemProperties`), which web browsers use for "Show in folder"; a file
manager that already owns the name keeps it. Both names are D-Bus
activatable (`liria --service`, `/usr/share/dbus-1/services/
org.debion.Liria.service` and `org.debion.Liria.FileManager1.service`): a
call starts Liria, which waits 15 s for calls when it has no window.

A Liria that put files on the clipboard keeps running after its last
window closes, so that other programs can still paste them; otherwise it
ends with its last window.

## Look

Liria draws itself in the SDE theme (family, light or dark, accent, the
radii and fonts of the UI kit) and follows a change of theme at once
(`_SDE_CONFIG_SERIAL`). In Liquid Glass it marks its window as an SDE glass
surface (`_SDE_SURFACE` = `window`): the compositor's glass shader turns
the title bar, the command bar and the navigation pane into glass, and the
files keep their opaque card. In the other translucent themes Liria stays
opaque: they make only the window manager's title bars translucent, and an
X window has a single opacity, so Liria's own title bar cannot be
translucent without its files.

## Files

| File | |
| --- | --- |
| `~/.config/sde/liria.conf` | layout, icon size, sorting, hidden items, Home sections, navigation pane, window size |
| `~/.config/sde/liria-quick-access` | the pins: one location per line, TAB and a label of one's own |
| `~/.config/sde/liria-servers` | servers opened before (0600, no passwords) |
| `~/.local/state/sde/liria-views` | the layout and sorting of folders |
| `~/.local/share/Trash`, `<drive>/.Trash-<uid>` | the Trash |
| `~/.config/mimeapps.list` | the default applications (Open with) |

## Source

`src/liria/` is in two parts. The display-free core (`liria-core`, used by
the unit tests): `location` (locations, breadcrumbs, GVfs folders), `pins`
(Quick access), `address` (what the address bar takes; recent servers),
`trash` (the freedesktop.org Trash), `fileops` (copy, move and merge plans
with their conflicts, renaming, new items, undo), `prefs`, `openwith`
(applications for a type, the default) and `viewmodel` (the items of a
folder, their order and selection). The window (`liria-ui`): `window`
(tabs, navigation, commands, keys, drag and drop, the QA description),
`widgets` (tab strip, address bar, search box, command bar), `fileview`
(the three views on an `Fl_Scroll` with one canvas that draws only the
visible items, rubber band, renaming in place), `sidebar`, `home`,
`menus`, `dialogs`, `jobs` (file operations on worker threads with the
copy dialog), `clipboard`, `drives` (GIO volume monitor, mounting, sign-in),
`glibloop` (the GLib main context in FLTK's loop) and `glyphs`; then
`service` (D-Bus), `app` (windows, folder watching with inotify) and
`main`. It reuses the SDE file chooser's model (`src/common/
file_chooser_model.*`: folder reading in the background, thumbnails,
search, recent files), the desktop's file engine and XDND code, the UI
kit's menus, icons, theme and dialogs, and Control Center's default
applications model.

## Packaging

`cmake --install` with `COMPONENT liria` installs:

- `/usr/bin/liria`
- `/usr/share/applications/liria.desktop` and
  `/usr/share/sde/applications/liria.desktop`
- `/usr/share/metainfo/org.debion.Liria.metainfo.xml`
- `/usr/share/man/man1/liria.1`
- `/usr/share/icons/hicolor/{16x16,22x22,24x24,32x32,48x48,64x64,128x128,256x256}/apps/liria.png`,
  `/usr/share/icons/hicolor/scalable/apps/liria.svg`
- `/usr/share/dbus-1/services/org.debion.Liria.service`,
  `/usr/share/dbus-1/services/org.debion.Liria.FileManager1.service`
- `/etc/sudoers.d/sde-liria` (mode 0440)

Build: GLib/GIO (`gio-2.0`, `gio-unix-2.0`), cairo, librsvg, libdbus,
Xlib (X11, Xrandr, Xi), libjpeg and gdk-pixbuf through SDE's common
library, SDE-FLTK (built in the tree), threads. Run: GLib and GIO; gvfs
and gvfs-backends for network places, gvfs-fuse (recommended) to copy
through the file engine, gvfs-daemons and udisks2 for drives; dbus;
sudo for Open as administrator; and, for their commands, taca, debrun,
sde-appimage, sde-view and a terminal (x-terminal-emulator). A Secret
Service keyring is optional.

## Tests

- `liria-core` (`tests/test_liria.cpp`): the core in a temporary home with
  a mount table of its own (no trash of a real drive is ever listed or
  emptied): locations, pins, addresses and servers, the Trash, copy, move,
  link and merge plans with every answer, renames, new items, undo,
  history, the view model, preferences and Open with.
- `liria-ui` (`tests/test_liria_ui.py`): the window on Xvfb with xdotool:
  Home, opening, selecting, the context menus, a copy with the copy
  dialog, tabs, Back/Forward/Up, the address bar, rename, Trash and undo,
  new folder, views, hidden items, search, 10 000 files, the taskbar state
  and the end of the program.
- `liria-dbus` (`tests/test_liria_dbus.py`): FileManager1 through D-Bus
  activation on a bus of the test, `org.debion.Liria.Open`, the hand-off of
  a second `liria` and `--quit`.
- `sde-native-branding` (`tests/test_native_forks.py`): the data files,
  the install rules of the `liria` component, the command line, and that
  nothing of the Caja fork is left.

With `SDE_PANEL_QA_DIR` set Liria describes its active window in
`Liria.txt` there: `@name:value` lines (location, tabs, layout, items,
selected, focus, menu, clipboard, renaming, jobs, startup and first-frame
times ...) and one `name x y w h active` line per part (tabs, buttons,
breadcrumbs, commands, places, Home cards, the visible items, columns);
`tools/showcase.py` uses it too. `LIRIA_TIMING=1` prints how long folders
take; `SDE_QA_COPY_RATE` (bytes per second) slows copies down for tests.

## Measurements

`tools/liria-perf.py`, median of three runs on Xvfb, in this container
(four cores shared with other builds), against the Liria of 0.2.0 Alpha
3.1 (`docs/0.2-ALPHA3.1.md`, the Caja fork, measured the same way). The
times include up to 50 ms of the QA description's own delay.

| | Alpha 3.1 (Caja fork) | 0.2.0 Alpha 4 |
| --- | --- | --- |
| Start until idle (Home shown) | 0.63 s, 0.33 s CPU | 0.12-0.24 s, 0.04 s CPU; first frame 27 ms after start |
| Open a folder of 1 000 files | 0.35 s, 0.13 s CPU | 0.07 s, under 0.01 s CPU |
| Open a folder of 10 000 files | 1.01 s, 0.67 s CPU | 0.13 s, 0.05 s CPU |
| ... then the details (list) view | 3.04 s, 2.65 s CPU | 0.08 s |
| ... the compact view | 1.16 s, 0.66 s CPU | 0.08 s |
| ... the icon view again | 1.33 s, 0.83 s CPU | 0.08-0.10 s |
| Select all 10 000 files | 0.49 s, 0.42 s CPU | 0.09 s, 0.02-0.03 s CPU |
| Scroll 40 wheel steps (10 000 files) | 1.0 s, 0.14 s CPU | 1.0 s (the steps themselves), 0.10-0.13 s CPU |

The pointer moving like a hand (CPU use of Liria, then of the X server;
1920x1080 at 150 %, as the Alpha 3.1 measurements; at 1366x768 and 100 %
the numbers are the same within 0.3 points):

| Over | Alpha 3.1 | 0.2.0 Alpha 4 |
| --- | --- | --- |
| Icon view, 10 000 files | 6 % | 0.3 % + 0.7 % X |
| Compact view, 10 000 files | 8 % | 0.5 % + 0.8 % X |
| List / details view | 13 % | 0.4 % + 0.7 % X |
| The sidebar | 15 % + 4 % X | 0.7 % + 0.5 % X |
| Home cards | 4.7 % | 0.4 % + 0.7 % X |
| Ribbon / command bar (tooltips) | 3.8 % | 2.6 % + 1.3 % X |

A hovered item repaints only its own rectangle (SDE's buffered windows),
and the views draw only the visible items, so a folder of 10 000 files
costs no more to move over than one of ten.

## Not yet

- Liquid Glass was checked through the generated compositor configuration
  and its unit test, not on screen: the showcase container has no OpenGL
  compositor (`sde-compositor`).
- Network places were tested through the address parser and the sign-in
  code paths, not against real servers.
