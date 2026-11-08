# SDE applications

Since 0.2.0 Alpha 4 every SDE application is a Debian package of its own and
SDE Core is the package `sde-core`; the native suite of GTK forks described
below ended with Liria's Caja fork. See [PACKAGING.md](PACKAGING.md).

Alpha6 builds eight native application forks from pinned upstream source. Their
ELF executables, libraries, resources, schemas and helpers are included in the
single `sde` binary package. Installing the original MATE application packages
is not required. Shared libraries such as GTK and libmate-desktop, together with
their settings data in `mate-desktop-common`, are normal Debian dependencies;
they are not duplicated inside SDE.

| Application | Executable | Source |
| --- | --- | --- |
| Niny | `niny` | native SDE application (Selviana DE Project); tabs and menu bar: [NINY.md](NINY.md) |
| Liria | `liria` | [caja](https://github.com/mate-desktop/caja) |
| Xanara | `xanara` | native SDE application (Selviana DE Project) |
| Taca | `taca` | native SDE application (Selviana DE Project) |
| Photo | `sde-view` | native SDE application (Selviana DE Project) |
| SDE System Monitor | `sde-system-monitor` | native SDE application (Selviana DE Project); processes and kernel threads: [below](#sde-system-monitor) |
| SDE Power | `sde-power` (Control Center > Power) | native SDE application (Selviana DE Project, alpha16) |
| SDE Screen Lock | `sde-lock` (Control Center > Lock Screen) | native SDE application (Selviana DE Project, alpha16) |
| Calculator | `calculator` | native SDE application (Selviana DE Project, alpha50; replaced the GTK4 scientific-calculator frontend) |
| Display Settings | `sde-display-settings` | [DisplayProfileManager](https://github.com/zac15987/DisplayProfileManager) profile model, native Linux frontend |
| Software Updater (formerly SDE Update) | `sde-update` | [mintupdate](https://github.com/linuxmint/mintupdate) model, Debian PackageKit (the operating system; apps are updated in Merkadu) |

These GTK forks retain their upstream C/C++ implementation. SDE's configuration,
panel geometry, autostart and suite diagnostics use Rust; the shell and update
interface use C++/FLTK. This release does not claim to rewrite the suite in Rust.

## Native forks

`suite/forks.lock.json` records the exact revision compiled for each fork.
`tools/prepare-native-suite.py` fetches those commits and their pinned submodules,
preserves licenses and copyright comments, and prepares editable downstream
source in `vendor/suite`. Application identifiers, schemas, D-Bus names, symbols,
resource paths and executable names are changed together. Shared MATE session
interfaces remain external system interfaces. Taca's file-manager extension
builds against the bundled Liria extension API.

Liria defaults to `--no-desktop`, including direct invocation: SDE retains its
wallpaper and desktop icons.

Since alpha16 Liria is regenerated from Caja v1.28.0 with complete SDE
branding (About: Developer Selviana DE Project; desktop entries, AppStream data,
man pages, local help in `/usr/share/liria/help`, translations and
`liria --version`). The original authors' copyright lines stay in every
source file, in the License section of the About window and under
`/usr/share/doc/sde/upstream/caja/`, as the GPL requires;
`tools/liria-branding-check.py` checks a prepared tree or an installed
prefix for any other occurrence. The window has one compact header (Back,
Forward, Parent, breadcrumb path with an edit toggle, icon/list/compact
buttons, Search, main menu on F10) with 16 px symbolic icons, grouped
context menus with Send To for removable drives, a sidebar (colour theme icons since alpha52) and
Liria's own style sheet that uses only standard GTK theme colours.
Screenshots: `docs/screenshots/alpha16/liria-*.png`.

Since alpha50 the Home overview's Quick access is a user-editable pin list:
any folder, file or location (remote URIs, folders on removable drives) is
pinned with "Pin to Quick Access" in the file and sidebar menus, by dropping
it on the cards or with the "+" card, and a card's menu renames, moves,
unpins or resets the list (drag to reorder; Delete, F2 and
Ctrl+Shift+Left/Right on a focused card). Pins are stored in
`~/.config/sde/liria-quick-access`: UTF-8, one URI per line in card order,
optionally followed by a TAB and a custom label, `#` comments allowed. A
missing file means the defaults (Desktop, Documents, Downloads, Pictures,
Music, Videos, Trash); "Reset to Defaults" deletes it. Every Liria window
watches the file. The code is `src/liria-sde-pins.c` (shared by the window
and the SDE Tools extension); its unit test `sde-tools/test-liria-sde-pins.c`
runs in `make check`.

Since alpha52 (`src/liria-sde-ribbon.c`, `src/liria-sde-model.c` and
`suite/redesign.py` `liria_home_view`, `liria_ribbon`,
`liria_sidebar_pinned`, `liria_pathbar`, `liria_right_drag`):

* **Home view.** The overview is the Home view: while it is shown the
  slot hides its folder grid (no second copy of Desktop, Documents...)
  and the overview scrolls; "Hide overview" brings the folder view back.
  Quick access shows the pins (with a pin marker) followed by folders
  suggested from recently used files ("Frequent folder", Pin button on
  hover); Quick access, Drives (a grid of cards with a usage bar) and
  Recent files have collapsible headers.
* **Sidebar.** Home, Pinned (the same pin list), Bookmarks, Drives and
  Network with collapsible headings, the icon theme's full-colour icons
  at 18 px times the interface scale, and a pin on the hovered pinned row.
* **Ribbon.** Home, Share and View tabs (plus Drive and Archive when they
  apply) under the header of every folder window. Modes: full, tabs only
  (Ctrl+F1) or off, chosen in Main Menu > Ribbon and kept in
  `~/.config/sde/liria.conf` (`[ribbon] mode=full|collapsed|off`); the
  same file holds the collapsed sections (`[home]`, `[sidebar]`).
* **Drag and drop.** Every Home card is a drag source (text/uri-list);
  files dropped on folder and drive cards are copied, moved or linked by
  Liria (application action `sde-drop`), locations dropped on Quick
  access or the Pinned heading are pinned, and a right-button drag of
  icons asks Move/Copy/Link Here (Alt+drag belongs to SDE WM). The SDE
  desktop accepts files dragged from Liria (moved on the same drive,
  copied otherwise) and its files can be dragged into Liria.
  `tools/liria-dnd-smoke.py --prefix <built prefix>` checks real drags on
  Xvfb; `sde-tools/test-liria-sde-model.c` tests the pin-on-drop,
  ribbon-preference and Home layout decisions.
* **Path bar.** The scroll arrows appear only for long paths (Caja showed
  them always, inert); a click on the empty part of the path opens the
  location entry.

Since 0.2.0 Alpha 1:

* **Rubber band and selection.** Dragging a selection rectangle over files
  shows a translucent accent sheet (20 %) with a 1 px accent border in
  every theme, so the files being selected stay visible. It was an opaque
  sheet in the view colour (white in light themes): Liria's style sheet
  and the SDE themes' soft layer gave every state of `.view` that colour,
  and the icon view takes the colours of its band, of selected labels and
  of the tint of selected icons from its own `.view` style context (with
  `.rubberband` or `:selected`). Selected files now have a soft accent
  highlight in the icon, compact and list views (the list showed none in
  some themes) and selected icons keep their lightness in dark themes.
  `tools/liria-rubberband-smoke.py --prefix <built prefix> [--all]` drags
  a real rubber band on Xvfb and checks the pixels under it (with `--all`
  every family, light and dark, 100 and 150 %, without and with a
  compositor); `tests/test_native_forks.py` checks the style sheet cascade
  (`LiriaRubberBandCssTest`). The list view has no rubber band (as in
  Caja); Ctrl+A or Shift/Ctrl+click select there.
* **About.** Liria's developer is the Selviana DE Project; the About
  window shows the version, "Part of Selviana Desktop Environment",
  Copyright © 2026 Selviana DE Project and contributors and the SDE
  trademark notice. Its License page starts with every copyright notice of
  the original work (Nautilus authors, Caja authors, MATE developers) and
  keeps the GPL text, with the license `GPL-2.0-or-later AND
  LGPL-2.0-or-later` (eel and the extension library are LGPL) as in
  `data/legal/components.json`, which a unit test compares it with;
  Credits lists the developer, "Based on Caja 1.28 (MATE)" and GNOME
  Nautilus and the original authors. The logo and the default window icon
  are `liria` (hicolor), with `system-file-manager` as the fallback.
* **Themes.** Keyboard focus rings are visible (half the text colour
  instead of 10 %) and tree views (the list, the sidebar) show drop
  targets with the accent: a tinted row, or a 2 px line between rows.
* **Sidebar, header and ribbon.** The Pinned and Network headings are as
  high as the others (the bold heading was measured as two lines). The
  location entry is as wide as the breadcrumbs (Caja's spinner spacer took
  half of the free width). The ribbon's commands get the selected files (it
  used the view's GFile locations as files: no URIs for Copy path, Pin,
  Move to, Share or Extract and critical warnings on every selection
  change), and the ribbon scrolls sideways with the mouse wheel when it is
  wider than the window (1024x600 at 150 %).

Since 0.2.0 Alpha 2:

* **Icon.** Liria's icon is the folder it had, without the lily emblem
  (`data/icons/hicolor/scalable/apps/liria.svg` and the hand-tuned 16, 24
  and 32 px drawings in `artwork/icons/`; [BRANDING.md](BRANDING.md)).
* **Ribbon icons in colour.** Every command of the ribbon has a list of
  full-colour icon names (best first; `base+emblem` draws an emblem on a
  base icon) that is looked up in the current icon theme at the sizes
  themes draw: 32 and 16 px at 100 %, 48 and 22 px at 125 %, 48 and 24
  px at 150 % and 64 and 32 px at 200 % (GTK renders them for the
  screen's scale factor, so they stay sharp on HiDPI screens). An icon
  that the theme draws in grey only (most action icons of Papirus are such
  line icons) is replaced by the theme's symbolic glyph in a theme colour:
  the accent, amber (Paste, Move to, Copy to), green (Open with, Extract
  here) or red (Cut, Format). The icons are chosen again while Liria runs
  when the icon theme or the interface scale changes. `tools/liria-ribbon-smoke.py
  --prefix <built prefix> [--scale 150] [--screenshots DIR]` checks it on
  Xvfb: an icon for every command, the sizes, colour on the screen, the
  Home, Share, View and Archive tabs and a change of icon theme
  (`LIRIA_SDE_QA_RIBBON=FILE` writes the chosen icons and the tabs). The
  Drive tab appears for folders on ejectable drives only.
* **Speed.** `tools/liria-perf.py --prefix <built prefix> [--repeat 3]`
  drives Liria on Xvfb like a user (1366x768, SDE Graphite Light,
  Papirus, the ribbon shown, folders of 1 000 and 10 000 files and 300
  pictures) and measures until Liria is idle again. Median of three runs
  (wall time, Liria's CPU time), the Liria of 0.2.0 Alpha 1 and now:

  | Scenario | Before | After |
  |---|---|---|
  | Start until idle (window shown) | 0.54 s (0.35 s) | 0.63 s (0.33 s), the same within the spread of runs |
  | Open a folder of 1 000 files | 0.41 s, 0.16 s CPU | 0.35 s, 0.13 s CPU |
  | Open a folder of 10 000 files | 1.41 s, 1.09 s CPU | 1.01 s, 0.67 s CPU |
  | ... then the list view (Ctrl+2) | 7.86 s, 7.45 s CPU | 3.04 s, 2.65 s CPU |
  | ... the compact view (Ctrl+3) | 7.94 s, 7.40 s CPU | 1.16 s, 0.66 s CPU |
  | ... the icon view again (Ctrl+1) | 6.38 s, 5.94 s CPU | 1.33 s, 0.83 s CPU |
  | Select all 10 000 files (Ctrl+A) | 1.00 s, 0.87 s CPU | 0.49 s, 0.42 s CPU |
  | Scroll 40 wheel steps (icons, list) | 1.0 s, 0.14 / 0.56 s CPU | the same |
  | Ten single clicks (the ribbon follows) | 0.32 s CPU | 0.28 s CPU |
  | Home overview (Alt+Home) | 0.72 s, 0.24 s CPU | 0.73 s, 0.23 s CPU |
  | 300 thumbnails | 2.97 s | 2.92 s |
  | Ten bookmarks added (sidebar) | 0.16 s CPU | 0.17 s CPU |

  The causes, found with `perf record --call-graph dwarf` (`--perf`):
  readiness checks of whole-folder requests scanned every file after every
  file's answer (quadratic in the folder size); every file got its own
  file system query and the view waited for all of them; each file looked
  its folder up by URI for every attribute; any change of the folder's own
  file (free space, modification time, item count) updated every icon and
  row; and after every selection change the ribbon and the SDE Tools menu
  asked every MIME type question of every selected file
  (`suite/redesign.py` `liria_folder_loading`, `liria-sde-ribbon.c`,
  `liria-sde-tools.c`). Still slow: the first switch of 10 000 files to
  the list view, where GTK's tree view measures every row.
* **File operations (0.2.0 Alpha 3).** The file operations window has the
  design of SDE's copy and move dialog ([TRANSFERS.md](TRANSFERS.md)): SDE's
  own picture of a penguin that carries papers from one folder to the other
  (a bin for Delete and the Trash; it sits when paused, pushes the last
  paper in while the last data is written, jumps when done, is puzzled on
  errors), "Copying 3 items to Backup" with Pause, Queue and Cancel, a slim
  bar, "780 MB of 1.2 GB · 22 MB/s · About 40 seconds left", the item being
  copied and Details (from, to, items); further operations stand under the
  first as its queue. The picture moves at about 30 frames per second while
  it is visible and stands still when paused, queued, minimized and with
  reduced motion; it costs about 2 % of a CPU core. The picture and the
  model are SDE's own (`src/common/transfer_scene.h` and
  `transfer_model.h`, copied in as
  `libliria-private/liria-sde-transfer-scene.h` and
  `liria-sde-transfer-model.h`), so Liria and the SDE desktop look and
  estimate the same; the Windows-like speed graph of 0.2.0 Alpha 2 is
  gone. At the end a copy says for a moment "Copied 3 items to Backup"
  and, for a removable drive, "The files are on SANDISK — you can remove
  it."; a failed one stays until its cross is clicked, while the queue goes
  on.
* **Safe copying (0.2.0 Alpha 3).** Copies and moves to removable and
  external drives (USB, SD cards, FireWire, Thunderbolt, also under LUKS or
  LVM), network shares and FAT, exFAT, NTFS, UDF and HFS+ file systems go
  through SDE's safe writer (`src/common/safe_copy.h`, shared with the
  desktop and copied in as `liria-sde-safe-copy.h`; `liria-sde-copy.c`):
  about a second of the drive's speed in flight, progress only for what the
  drive has taken, checkpoints (`syncfs`, `fsync` of the folder) every 5 s
  and at the end, "done" only after the last one, the sources of a move
  deleted only after a checkpoint; deletions, the Trash and an emptied
  Trash on such drives are written to them before the operation ends. On a
  drive limited to 20 MB/s Liria said "done" after 0.6 s with nothing on the
  drive before, and now after 6.2 s with everything on it. Internal disks
  keep the page cache (`[files] safe_copy = auto | always | never` in
  sde.conf). GLib 2.80 copies a local file with one `copy_file_range ()`
  call and reports its progress when it is there, so files of 32 MiB and
  more on internal disks are copied in slices of about a tenth of a second
  (`liria_sde_copy_large_file ()`, 0.2.0 Alpha 2), and Pause stops a copy
  in the middle of a file. `tools/liria-progress-smoke.py --prefix <built
  prefix>` copies real files on Xvfb (slowed down with
  `LIRIA_SDE_QA_COPY_RATE`) and checks the texts, the moving and the still
  picture, Details, Pause in the middle of a file, Resume, Cancel, reduced
  motion, a complete copy byte for byte and a safe copy's finished state
  (`LIRIA_SDE_QA_PROGRESS=FILE` writes what the window shows).

Since 0.2.0 Alpha 3:

* **Smooth under the pointer.** Parts of the window stuttered or lagged
  behind the pointer. `tools/liria-perf.py --hover --prefix <built
  prefix> [--size 1920x1080 --scale 150] [--repeat 3]` moves the pointer
  like a hand (XTest, 100 motion events a second) over the icon, list and
  compact views of 1 000 and 10 000 files, the sidebar, the ribbon and its
  tooltips, the breadcrumbs of a deep folder, the header buttons, four
  tabs, the status bar and the Home cards, and measures Liria's and the X
  server's CPU use while the pointer moves, the frames drawn, how long the
  slowest frames took to lay out and paint and the area repainted
  (`LIRIA_SDE_QA_FRAMES=FILE`, `src/liria-sde-qa.c`, records every frame
  and pointer event; SIGUSR2 writes where the parts of the window are).
  The causes, found with it, `perf` and GTK's frame clock:
  * the sidebar repainted all its rows on every pointer motion (to show
    the pin of the pinned row under the pointer); now only the two rows
    that gain or lose the pin, when the pointer reaches another row;
  * a tab under the pointer faded in the theme's hover colour and the
    close button's icon colour, and every step of the fade laid out the
    whole window again and repainted the tabs and the page; the tabs' own
    style has no fades and shows the hover inside the tab;
  * the SDE themes fade the hover of a button in over 75 ms, and GTK 3
    works out the button's style again on every frame of the fade (2-4 ms
    each); Liria's own buttons (header, breadcrumbs, ribbon, Home) show
    the hover at once;
  * the icon and compact views asked every icon of the folder whether it
    is under the pointer, on every motion; the canvas keeps its icons by
    rows (`CANVAS_ROWS` in `suite/redesign.py`), so a motion asks the
    icons of one row only;
  * every change of the active pane added one more style provider to the
    location entry.

  Median of three runs at 1920x1080 and 150 % (SDE Graphite Light,
  Papirus, Xvfb on a shared virtual machine; runs of 0.2.0 Alpha 2 and of
  this version alternated): the CPU use of Liria and of the X server in
  per cent of one core while the pointer moves, the frames drawn and the
  area they painted, and how long Liria took for the slowest 5 % of the
  pointer events.

  | Pointer over | Before | After |
  |---|---|---|
  | The sidebar | 40 % + 14 % X, 126 frames, 27 Mpx | 15 % + 4 % X, 40 frames, 0.6 Mpx |
  | Four tabs | 12 % + 16 % X, 72 frames, 81 Mpx | 6 % + 2 % X, 27 frames, 0.2 Mpx |
  | Icon view, 10 000 files | 15 %, events 2.7 ms | 6 %, events 0.5 ms |
  | Compact view, 10 000 files | 16 %, events 2.7 ms | 8 %, events 0.8 ms |
  | Icon and compact views, 1 000 files | 6.5-7 % | 5 % |
  | Breadcrumbs of a deep folder | 9 %, 119 frames | 5 %, 36 frames |
  | Ribbon | 5.6 %, 131 frames | 3.8 %, 24 frames |
  | Header buttons | 5.5 %, 70 frames | 4.5 %, 43 frames |
  | Home cards | 6.3 %, 87 frames, 3.1 Mpx | 4.7 %, 19 frames, 0.6 Mpx |
  | List view, 1 000 or 10 000 files | 13 % | 13 % |

  Still at GTK's own cost: GtkTreeView paints a whole row whenever the
  pointer reaches another row, which is the list view's 13 % and most of
  the sidebar's, and the canvas of the icon views still updates every icon
  of the folder when one changes (`eel_canvas_group_update ()`).

* **Network places from the location bar.** Ctrl+L (or the edit button
  of the path) takes, besides paths, `\\server\share` and
  `smb://server/share` (written as in Windows, with spaces, `DOMAIN\user@`),
  an IP address or a computer name such as `192.168.1.10`, `nas`,
  `localhost` or `fe80::1` (its Windows shares; a name that is also a
  file or folder in the current folder opens that), `sftp://user@host:port/path`
  (and `ssh://`), `ftp://`, `ftps://`, `nfs://`, `dav://`, `davs://`
  (and `webdav(s)://`), `afp://`, and `smb:` or `\\` for the Windows
  network (`src/liria-sde-address.c`, unit test
  `sde-tools/test-liria-sde-address.c`). What is not a location
  (`C:\Users`, `sftp://` without a server, a port out of range) gets a
  message under the entry that says why, and the text stays to be
  corrected. While typing, Liria suggests the recent servers
  (`~/.config/sde/liria-servers`: the last 30 servers and shares opened,
  mode 0600, never a password) and, for a computer's name or address, its
  Windows shares and then the other protocols that this computer can open
  (SSH, WebDAV over HTTPS, NFS, WebDAV, FTP; `src/liria-sde-network.c`).
  * **What is installed decides.** Liria opens what GVfs can open here
    (`g_vfs_get_supported_uri_schemes ()`); without gvfs-backends (or
    GVfs) the message names the package to ask the administrator for
    ("Windows shares need the package gvfs-backends. It is not installed
    on this computer; ask your administrator to install it."), in the
    location bar and in Connect to Server.
  * **Sign in.** A server that asks for a password gets Liria's own
    dialog ("Sign in to “share” on nas"): the user name (the login name is
    suggested, never "guest"), the domain (`DOMAIN\user` is split as in
    Windows), the password with a button that shows it, "As a guest" when
    the server allows it, a warning for FTP and WebDAV without HTTPS
    (passwords travel unencrypted) and, after a wrong password, "not
    accepted, try again". "Remember the password in the keyring" is
    offered only when a Secret Service (gnome-keyring, KeePassXC) runs or
    can be started on the session bus; otherwise the dialog says that
    passwords can be remembered when a keyring runs. A password typed in
    the address (`smb://anna:secret@nas/`) is used once and taken out of
    the address. An unknown SSH host key is GVfs's question (Log In
    Anyway or Cancel Login, Cancel by default) and goes to the user's
    `~/.ssh/known_hosts`; ssh refuses a changed key.
  * **Failures explained.** No such computer, access denied, no answer, no
    such share or folder and the Windows network that cannot be listed (it
    needs SMB1) each have their own message. When the Windows shares of a
    typed computer cannot be opened, the message lists the other ways to
    reach it, encrypted ones first.
  * **The system's settings stay in charge.** Liria sets nothing about
    the protocols: Samba's `smb.conf` (by default SMB2 or later and NTLMv2,
    no plain-text passwords), the ssh configuration and known hosts and
    GVfs decide, so there is no fallback to SMB1, NTLMv1 or plain-text
    logins, and a password is kept only in the keyring and only when the
    user asks. Connect to Server lists Windows shares first, then SSH,
    WebDAV (HTTPS), WebDAV, FTP, public FTP, NFS and AFP, offers Remember
    only with a keyring and names gvfs-backends when it is missing.
  * `tools/liria-network-smoke.py --prefix <built prefix> [--screenshots
    DIR]` types into the location bar on Xvfb like a user and checks what
    Liria shows (`LIRIA_SDE_QA_NETWORK=FILE`): without GVfs the messages;
    with `--smb smb://server/share --user U --password P` the sign-in
    dialog and the share opened and remembered without its password,
    `--remember` with a keyring; `--refused HOST` the failure and the
    protocols offered; `--sftp sftp://host:port/ --accept-host-key` the
    host key question. It passes against a Samba share and sshd on the
    same computer (`tests/test_native_forks.py` `LiriaAlpha3Test` runs it
    when `SDE_LIRIA_PREFIX` names a built Liria).
  * GLib 2.76 and later warn about file attributes that are read without
    being there; Liria read some (hidden, backup, symbolic link, display
    name, icon) of files on network locations that do not have them, and
    now reads them only when they are there.

Since 0.2.0 Alpha 3.1:

* **Menus at once.** A right-click felt slow, and the highlight in menus
  lagged behind the pointer with two or three items blue at once.
  `tools/liria-menu-smoke.py --prefix <built prefix> [--slow]
  [--compositor] [--repeat 3] [--json FILE]` drives Liria on Xvfb like a
  user (XTest: press, 60 ms, release): right-clicks on a folder, a picture,
  a PDF, a Debian package, an AppImage, a zip and a tar archive, text, a
  script, a program and a web page, on the empty part of a folder, a row of
  the list view, a sidebar place, a Home card and one of 1 000 selected
  files, the main menu, the pointer moving over a context menu and the main
  menu, and a rest on a submenu. `LIRIA_SDE_QA_FRAMES` also records every
  click and key with its X server time, every menu mapped, every frame of
  a menu and every highlight change (`src/liria-sde-qa.c`; SIGUSR2 also
  writes the places of files, sidebar places and menu items), so the time
  is measured from the click to the end of the frame that painted the
  menu. With `--slow`, Xvfb and Liria share one CPU core with a busy
  process (`taskset`). It fails when a menu takes more than 250 ms (500 ms
  with `--slow`), the highlight more than 100 ms (200 ms) at the 95th
  percentile, an item change more than three frames (a fade) or Liria
  prints critical warnings (`tests/test_native_forks.py`
  `LiriaAlpha31Test` runs it when `SDE_LIRIA_PREFIX` names a built Liria).
  The causes, found with it, `perf` and gdb:
  * the SDE themes fade a menu item's highlight out over 120 ms (the two
    or three blue items of the video), and GTK 3 worked out the item's
    style again and laid out and repainted the whole menu on every frame
    of a fade: five frames for every item the pointer crossed. Liria's
    style sheet turns menu item transitions off;
  * an open menu takes the keyboard focus from the folder view, and GTK
    restyles the view. The icon view then measured every label and updated
    every icon of the folder again (`liria_icon_container_request_update_all
    ()`: 60-70 ms for 1 000 files and more in larger folders, when a menu
    opened and again when it closed), and the background
    (`EelBackground`) made a picture as large as the view again. Both now
    react only to changes that matter to them: the font and resolution the
    labels are measured with, and the view colour and scale;
  * the window's repaint for the lost focus (the whole view, 13-30 ms on
    one slow core) came before the menu's first frame, and a new menu
    window is painted only after the X server has mapped it. The window
    now waits until the menu is painted (`eel_sde_paint_menu_first ()`,
    for every context menu, the main menu and the ribbon's menus);
  * the menu of a file opens when the button is released (a right-button
    drag moves files), but was made ready only then, or 100 ms after the
    selection changed. A right-button press now makes it ready right after
    the press is painted, so the release only shows it;
  * every menu's grab walks all widgets of the window
    (`gtk_grab_notify ()`), and the Emblems panel of the side pane, never
    shown, had about 300 of them; it is filled when it is first shown;
  * Caja's cache of tinted icon pictures was used only for selected icons,
    so every repaint of the view made a new picture for every other icon;
  * every menu update (100 ms after the selection changed, or when a menu
    opens) read the information of every selected file again in the main
    loop (`liria_file_refresh_info ()`: the content type sniffed from the
    file, thumbnails and metadata looked up), to notice a file whose type
    changed since it was listed: hundreds of files after Ctrl+A and a
    network round trip for each file of a remote location. Only up to
    eight local files are checked now (folder monitors report the
    others); Ctrl+A over 10 000 files takes 720 ms instead of 970 ms.

  Median of the runs (Xvfb on a shared virtual machine, 1366x768, SDE
  Graphite Light, Papirus; 0.2.0 Alpha 3 and this version alternated),
  from the click that opens the menu to the menu painted, in brackets on
  one CPU core shared with a busy process:

  | Menu | Before | After |
  |---|---|---|
  | Right-click on a file (eleven types, icon view) | 33 ms (60 ms) | 26 ms (42 ms) |
  | ... on one of 1 000 selected files | 77 ms (190 ms) | 16 ms (37 ms) |
  | ... on a row of the list view | 38 ms (70 ms) | 23 ms (47 ms) |
  | ... on a sidebar place | 20 ms (37 ms) | 13 ms (21 ms) |
  | ... on the empty part of a folder | 25 ms (35 ms) | 21 ms (36 ms) |
  | ... on a Home card | 30 ms (48 ms) | 26 ms (56 ms) |
  | Main menu | 32 ms (64 ms) | 33 ms (54 ms) |
  | Highlight after the pointer reached an item (median / 95 %) | 12.8 / 20.7 ms (16.4 / 22.3 ms) | 5.8 / 9.7 ms (7.7 / 14.0 ms) |
  | Frames for each item the pointer crosses; Liria's CPU use meanwhile | 5.3, 30 % | 1.4, 12 % |

  Still: GTK 3 opens a submenu 225 ms after the pointer reaches its item
  (fixed in GTK, the same for every GTK 3 program); the first menu after
  Liria starts takes 10-20 ms more (its window is made), and the first
  menu of a file type reads its applications' icons from the disk (half a
  second right after a shared virtual machine restarted). Most of what
  remains is GTK styling and measuring the new menu items when the menu
  opens (the main menu is made anew every time, the context menus get new
  items for the applications and extensions of the selection). Under SDE
  Compositor with all effects (on computers with a GPU) menus fade in
  (`fading` in the compositor's configuration, `src/common/integration.cpp`),
  about 100 ms more before they are fully visible. GTK 3 menus have no shadow of their own under
  SDE WM (it does not announce `_GTK_FRAME_EXTENTS`, so GTK draws them as
  plain windows) and the compositor draws one; with window managers that
  announce it GTK draws the shadow and SDE Compositor leaves those windows
  out (`shadow-exclude`), so a menu never has two.
* **Pointer motion over the list view** cost 13 % of a core (the icon
  view 6 %). Every motion that the folder view does not stop (the list
  view, the empty parts of the icon views, the Home cards) bubbled up to
  the tabs' GtkNotebook, which asked the X server where its window is
  (`gdk_window_get_origin ()`, a synchronous round trip) for each one;
  `LiriaNotebook` now passes it only the motions over its own parts and
  those while a button is held (dragging a tab). The rest is GtkTreeView,
  which repaints the two rows that gain and lose the hover, styling each
  cell from scratch: GTK does not cache the style of nodes whose look may
  depend on their position, and a rule of the SDE themes
  (`.sde-system-monitor treeview.view:nth-child(even)` in
  `data/themes/overlays/apps-gtk-3.0.css`, for the GTK System Monitor of
  earlier versions; GTK matches from the right, so
  `treeview.view:nth-child(even)` reaches every tree view cell of every
  program) makes all tree view cells such nodes.

  Median of three runs of `tools/liria-perf.py --hover` (1366x768): Liria's
  CPU use while the pointer moves, how long Liria took for the slowest 5 %
  of the pointer events and the slowest 5 % of the frames, before, after
  and after with that theme rule removed:

  | Pointer over | Before | After | Without the rule |
  |---|---|---|---|
  | List view, 1 000 files | 11.7 %, 0.52 ms, 7.8 ms | 11.3 %, 0.19 ms, 5.5 ms | 9.3 %, 0.21 ms, 3.7 ms |
  | List view, 10 000 files | 11.8 %, 0.86 ms, 6.2 ms | 11.2 %, 0.19 ms, 5.9 ms | 9.0 %, 0.20 ms, 3.6 ms |
  | Sidebar | 17.8 %, 0.86 ms, 14.1 ms | 17.4 %, 0.62 ms, 11.2 ms | 11.6 %, 0.55 ms, 6.4 ms |
  | Icon view, 1 000 / 10 000 files | 4.7 / 5.4 %, 0.50 / 0.45 ms | 4.5 / 5.0 %, 0.20 / 0.17 ms | |
  | Home cards | 5.2 %, 0.54 ms | 4.8 %, 0.22 ms | |

## SDE Power and SDE Screen Lock (alpha16)

The former power-manager and screen-saver forks were replaced in alpha16 by
programs written from scratch (`src/power`, `src/lock`), both started by
SDE-only XDG autostart entries:

* `sde-power --daemon`: UPower battery warnings and the critical action,
  logind lid/power-button inhibitors, idle dim/blank/suspend (separately on
  battery and AC), brightness keys with an OSD, power profiles. D-Bus
  `org.debion.SDE.Power`. Settings in sde.conf `[power]`.
* `sde-lock --daemon`: `org.freedesktop.ScreenSaver` (Lock, GetActive,
  SetActive, Inhibit/UnInhibit, ...) and `org.debion.SDE.ScreenLock`, idle
  lock, lock before suspend (logind delay inhibitor), logind Lock/Unlock.
  The lock screen is a separate process watched by the daemon; PAM service
  `/etc/pam.d/sde-lock` (`@include common-auth`, `common-account`).
  Settings in sde.conf `[lock]`.

`sde-power-preferences`, `sde-screensaver-preferences` (open the Control
Center pages) and `sde-screensaver-command` (forwards `--lock`, `--query`, `--poke`
to `sde-lock`) remain as small compatibility scripts.

New GTK processes follow SDE light/dark settings, respecting an existing
`GTK_THEME` or the system-theme option. SDE-specific MIME defaults preserve user
overrides. Xanara (native since alpha15) opens, annotates and fills in PDF files with
poppler; since 0.2 Alpha 3 its own PDF engine (libqpdf, HarfBuzz, FreeType) edits the
text and pictures of the pages in place and does every page operation itself, without
running any other program (docs/XANARA.md).

The panel defaults to a full-width square-edged rectangle. Floating and short
styles remain available. Existing saved panel choices are retained. Control Center → Applications shows short clickable source names and complete installed copyright
notices under `/usr/share/doc/sde/upstream/<upstream>/`.

## Installation

The supported target is **Debian testing**. Distributions that ship SDE, such
as DebION GNU/Linux, should publish all SDE packages in their own signed APT
repository and seed `sde-desktop` in the installer:

```sh
sudo apt install sde-desktop
```

`sde-core` is the desktop and each application is a package of its own
(`liria`, `xanara`, `niny`, `merkadu`, `sde-calculator`, `taca`, `sde-photo`,
`sde-gallery`). `sde-desktop` installs them with GNOME Terminal, the LightDM
login screen (Slick Greeter), Xorg, network, audio, removable-media support,
icons and GSettings. These are operating-system
services required for a complete DE. This project does not configure an APT
repository. For local packages, let APT resolve dependencies:

```sh
sudo apt install $(ls ./dist/*.deb | grep -v -e -dbgsym_ -e /debion-desktop-base_)
```

Select SDE-Desktop at the login screen. Debug-symbol packages are optional and are not
needed for normal installation. `dpkg -i` alone does not fetch missing libraries
or system services; use `sudo apt --fix-broken install` to repair that state.

## Software Updater

Software Updater (`sde-update`, called SDE Update before Alpha 51) is a native
FLTK window in the style of a classic software updater, drawn with the active
SDE theme (colors and radii follow the theme, also when it changes while the
window is open) at every UI scale. It updates the operating system: the Debian
packages. Applications from Flathub and the Snap Store are updated in Merkadu,
the software center ([MERKADU.md](MERKADU.md)), whose Updates page sends system
updates back here; since 0.2 Alpha 3 the Software Updater has no Flatpak group,
and its up-to-date view offers **Open Merkadu** when Merkadu is installed.

- **Updater**: the question "Updated software is available for this computer.
  Do you want to install it now?" (or "up to date" with the last check), a
  collapsible **Details of updates** tree with the groups Critical, Security
  and Main (tri-state check boxes, sizes, application names and icons
  from the packages' desktop files), a collapsible **Technical description**
  (versions, origin, size and the changelog from `apt-get changelog`,
  PackageKit `get-update-detail`, then the installed changelog, loaded in the
  background), the download size, and **Settings…**, **Remind Me Later** and
  **Install Now**. Progress, the log and a **Restart Now** / **Later** notice
  appear in the same window.
- Critical: Priority required/important, the kernel, libc, systemd, D-Bus,
  APT/dpkg, boot loader, firmware and microcode, urgency high/critical, and
  anything that needs a restart. Security: the Debian security archive. Main:
  everything else (backports included).
- **Settings…** opens the other pages with a sidebar (Escape goes back):
  **History** from `/var/log/apt/history.log*`, **Sources** (read-only one-line
  and deb822 entries, Software Sources when installed), **Maintenance** (refresh,
  clean cache, remove unused packages after review, repair, terminal update),
  **Settings** (`[update]` in sde.conf) and **About** with the mintupdate credits.

The read-only Python APT provider (`data/update/check.py`, protocol
`SDE-UPDATES 2`, with optional priority, section, urgency and application
fields since Alpha 51) uses the `Update` class adapted from Mint Update: candidates
are grouped by source, held packages are excluded and counted, Debian backports
get their own type. Mint repositories and Mint-only policies are not imported.

Transactions run in `transaction.py`, which streams progress (status, percent,
per-package items) to the window. It uses `pkcon` when PackageKit is running,
so polkit authorizes and the user can cancel; otherwise, and for cache cleaning
and autoremove, `apt-get` runs through `pkexec`. Package names are validated and
no options or repository URLs are accepted. `sde-update --notifier` (autostarted)
checks in the background and notifies; nothing is installed automatically.
`--check`, `--history` and `--sources` print the helper protocols; `--plan`
prints a transaction's argument vector without changing the system. Debian
testing transitions may still require manual APT decisions.

## Sources and validation

See [BUILD.md](BUILD.md) for the source preparation and offline build workflow.
Every binary artifact is accompanied by a complete source archive containing
all prepared trees, submodules, build scripts and preserved licensing. Original
sources can be inspected alongside the downstream changes using the locked Git
revisions. Fork maintainers must review and import future upstream security
fixes; these private forks do not receive fixes merely by updating the original
MATE application packages.

CI builds and installs on Debian testing, checks actual native ELF files,
rejects original MATE application packages, checks MIME overrides and copyright
notices, and opens the five file-handling applications with sample files on X11.
Network, audio and APT screenshot fixtures are explicitly simulated. No test
performs a real update, lock, suspend or shutdown. Current alpha8 menu/display captures are under
`docs/screenshots/alpha8`; CI uploads the complete capture set, including Calculator
and the isolated administrator Liria window. Older captures remain as release history.
Hardware login, power, locking and whole-desktop idle memory remain to be
validated on real hardware. See [THIRD-PARTY.md](THIRD-PARTY.md) for provenance.

LXQt, Budgie and COSMIC remain development references rather than extra desktop
shells shipped in SDE. `upstream/sources.lock.json` and `tools/suite-source.py`
prepare those original reference trees independently of the actual build lock.

See [alpha8 notes](ALPHA8.md) for current category placement and Display Settings.

## Calculator (alpha50)

Calculator (`calculator`, desktop id `org.debion.Calculator`) is a native
SDE-FLTK application written from scratch by the Selviana DE Project
(`src/calculator/`). It replaces the GTK4 frontend of the scientific-calculator
component, which cleared the input instead of computing "2 + 2".

* Modes: Standard, Scientific (deg/rad/grad, 2nd and hyp functions, log/ln,
  powers and roots, n!, π, e, parentheses, %, EXP, memory MC/MR/M+/M−/MS),
  Programmer (HEX/DEC/OCT/BIN, clickable bit view, AND/OR/XOR/NOT/NAND/NOR,
  shifts, BYTE/WORD/DWORD/QWORD) and Converter (length, mass, temperature,
  area, volume, speed, time, data and currency with an offline table that
  `~/.config/sde/calculator-currency.conf` can override).
* Expressions are typed or clicked: operator precedence, right associative
  `^` (2^3^2 = 512), unary minus (-2^2 = -4), implicit multiplication (2π,
  2(3+4)), pocket-calculator percent (200 + 10% = 220), live preview, inline
  errors ("Cannot divide by zero") that keep the input. Results use 64-bit
  mantissas shown with 16 significant digits (0.1 + 0.2 = 0.3).
* History panel (click to reuse, right click for more), persisted in
  `$XDG_STATE_HOME/sde/calculator-history.tsv`; preferences and memory in
  `$XDG_STATE_HOME/sde/calculator.ini`.
* `calculator --eval EXPR [--angle deg|rad|grad]`, `--eval EXPR --base hex
  [--bits 32]` and `--convert VALUE FROM TO` work without a display.
* `SDE_CALC_QA_FILE=path` makes the window write its mode, expression,
  result and error to `path` after every change (used by `tools/ui-smoke.py`).

Tests: `calculator-engine` (tests/test_calculator.cpp) and `sde-calculator`
(tests/test_calculator.py).

## SDE System Monitor

SDE System Monitor (`sde-system-monitor`, `src/sysmon/`, manual page
`sde-system-monitor(1)`) has the pages Processes, Performance, File Systems
and Startup Apps. Since 0.2.0 Alpha 4 its process list starts like the
monitors of GNOME and KDE:

* **My processes** (the default): your apps (they own a window or were
  started from a .desktop file) and your background processes, the programs
  and services that run as you. **All processes** adds the system's
  processes (root's and the other users') in a group of their own. The list
  is sorted by CPU until another column is chosen.
* **The threads of the Linux kernel are hidden** (0.2.0 Alpha 3.1 listed
  every kworker, ksoftirqd, rcu_preempt and migration thread as a background
  process); the line under the list says how many. **Show kernel threads**
  lists them in a group of their own, "Linux kernel", each with its own name
  and what it does ("Linux kernel — worker thread"); the tooltip and the
  line under the list explain the one selected. Kernel threads are told
  from programs by the flag `PF_KTHREAD` of `/proc/PID/stat` (on kernels
  without it: children of kthreadd without a command line) and described by
  their names (`describe_kernel_thread()` in `src/sysmon/procfs.cpp`):
  kworker threads are the kernel's workers, which do the work that drivers,
  file systems and the network hand over (their name gives the processor,
  `u` for any, and after `+` or `-` the work queue); ksoftirqd finishes
  interrupts, rcu_* keep shared memory consistent, migration moves tasks
  between processors, and so on. They belong to no program, use no memory
  of their own and cannot be ended: End Task, Kill, Stop, Continue, Change
  Priority and Open File Location are never offered for them.
* The view and the switch are kept in `[system-monitor]` of
  `~/.config/sde/sde.conf` (`processes=mine|all`, `kernel-threads=`).

Tests: `sde-sysmon-parsers` (tests/test_sysmon.cpp: the parsers, the
kernel-thread flag and the descriptions) and `sde-sysmon-ui`
(tests/test_sysmon_ui.py, on Xvfb: My processes without kernel threads,
Show kernel threads with their group and descriptions, End Task off for
them, All processes with another user's process, the choices kept). With
`SDE_PANEL_QA_DIR` the window writes `System-Monitor.txt` (the view, the
groups, each row and the buttons).
