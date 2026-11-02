# The SDE file chooser

0.2.0 Alpha 3.1. Every SDE program that opens or saves files or picks a
folder shows the same dialog, `sde::FileChooser` (`src/common/file_chooser.h`),
in Liria's design. Before, the Control Center (avatar and wallpaper pickers)
and Debrun showed FLTK's stock `Fl_File_Chooser` (a bare list starting with
`../`, the raw pattern `Pictures (*.{png,jpg,…` as the filter, a
"Favorites" drop-down, no Back button, no places), and Niny, Photo,
Gallery, Taca and Xanara showed `Fl_Native_File_Chooser`, which on SDE is
the same FLTK dialog.

```
[<] [>] [^]  ( ⌂ Home › Pictures › Nature              ✎ )  [⌕] [▦|☰] [◧] [⋮]
 Recent      │ [thumb] [thumb] [thumb]                  │  [the picture]
 Home        │ Misty   Mountain Northern                │  Mountain Lake.png
 Desktop     │ Hills   Lake     Lights                  │  Type        PNG image
 Documents   │                                          │  Dimensions  1200 × 800
 …           │ 3 files                     18.4 GB free │  Size        160.4 kB
 Name  [holiday.png                              ] [Pictures       ▾]   (saving)
 [New Folder]  ☐ Show hidden files                      [Cancel] [ Open ]
```

Screenshots: the showcase scenes `file-chooser` (Control Center → Users &
Avatar → Choose a picture…, in every theme set), `file-chooser-list` (Niny's
Open in list view), `file-chooser-save` (Niny's Save As) and
`file-chooser-folder` (a folder picker).

## What it does

**Header.** Back, Forward and Up (Alt+Left, Alt+Right, Alt+Up, Backspace,
the mouse's back and forward buttons; going back or up selects the folder
one came from). The path bar shows the folders from Home (or the drive, or
the file system) as buttons; the current one is highlighted, and after
going up the deeper folders stay on the bar until another branch is
chosen, as in GTK. A long path gives way at the front to "…", which lists
the hidden folders. Clicking the bar's empty part or the pencil, Ctrl+L, or
typing "/" or "~" in the files turns it into a location entry: a path,
`~/…`, a `file://` URI or a name relative to the folder, with completion
(a list under the entry; Tab completes the common part or the chosen line,
Up and Down choose, Enter goes there; a typed file is chosen at once when
opening, its folder and name are taken when saving). Escape closes the
completion, then the entry. The magnifier (Ctrl+F) turns the bar into a
search field: the folder is filtered as one types (any case and accent:
"cafe" finds "Café"), and after a short pause the folders below it are
searched on a worker thread (up to eight levels, hidden folders only when
hidden files are shown); results from subfolders show their folder under
their name and in a Location column. The view switch shows the grid
(thumbnails; Ctrl+1) or the list (name, size, modified, type, sorted by a
click on a column, again for the other direction; Ctrl+2). The pane button
shows or hides the preview; the ⋮ menu has the view, Sort By (and
Descending, Folders First), Show Hidden Files, Show Preview, New Folder,
Reload and Copy Location.

**Places.** Recent (the files of `~/.local/share/recently-used.xbel`, newest
first; not when choosing folders), Home, Desktop, Documents, Downloads,
Music, Pictures and Videos (the XDG user folders that exist), the GTK/Liria
bookmarks (`~/.config/gtk-3.0/bookmarks`), the mounted drives and removable
media (from `/proc/self/mountinfo`: below `/media`, `/run/media/<user>`,
`/mnt` and the home folder, without pseudo file systems or snaps; network
mounts get a globe), "File System" (`/`) and the locations GVfs (Liria)
mounted, labelled "alex on example.org". The list follows drives being
plugged in while the dialog is open (checked every two seconds). Up and
Down move through the places.

**Files.** Folders are read on a worker thread and handed over in batches:
the first entries show within a frame and a folder of 10 000 files fills in
(its status line says "Loading… 3,412 items") without the dialog ever
waiting. Names sort naturally ("file2" before "file10") and ignore case.
Thumbnails come from the freedesktop.org cache (`~/.cache/thumbnails`,
normal/large/x-large, checked against the file's time), so pictures,
PDFs and videos that Liria or another program already made show at once;
pictures that have none are made on two worker threads with gdk-pixbuf and
stored there for the next program (a picture that cannot be read is
remembered under `thumbnails/fail/sde-file-chooser`). Only the files in view
and a screen around them are made, nearest first, and far ones are
forgotten when there are many. Other files show their icon from the icon
theme (by MIME type). Type-ahead selects the first name that starts with
what was typed; double click or Enter opens a folder or chooses a file;
Ctrl+click and Shift+click select several (when allowed), Ctrl+A all. Ctrl+H
or the "Show hidden files" check box show hidden files (dot files, backups
ending in `~`, names in a folder's `.hidden`); F5 or Ctrl+R reload. A file
or folder dropped from Liria or the desktop opens its folder and is
selected (a file of another type than the filter's is refused with a
message); when saving, its folder and name are taken. Right click: Open or
Choose, Open Containing Folder (for found files), Copy Location, Show Hidden
Files, Show Preview, Reload. A status line counts folders and files (and
what is selected) and shows the free space of the drive.

**Preview.** The selected picture as large as the pane allows (decoded at
the pane's size, upright), with its type, dimensions, size and date; other
files show their thumbnail or icon and information, folders how many items
they hold, several files their total size. It can be hidden; the avatar,
wallpaper and other picture pickers show it whatever the user chose last.

**Footer.** The filter menu shows human names ("Pictures", "PDF documents",
"All files"), never the patterns. Saving: the name field starts with the
proposed name, the part before the extension selected (so typing replaces
it and keeps ".txt"; `archive.tar.gz` keeps `.tar.gz`); the filter's
extension is added when the name has none ("notes" + Markdown →
"notes.md") and replaced when the name has another filter's extension
(choosing PNG turns "photo.jpg" into "photo.png"); the filter that fits the
proposed name is chosen at first; a folder's name or a path goes there;
replacing a file asks first ("A file named … already exists in … Do you
want to replace it?", Cancel / Replace); a folder one cannot write to is
refused with a message. New Folder (Ctrl+Shift+N) opens a small card with
the name selected; the new folder is selected (when saving, opened). The
default button says Open, Save, Select Folder or Select Folders and stays
disabled until there is something to choose: a file (or a folder to open)
when opening, a name when saving, a folder (the one shown when none is
selected) when choosing folders. Escape closes the completion, the new
folder card, the location entry, the search, then cancels.

**Looks.** Drawn with `sde::Painter` in the active theme: light, dark,
Graphite, Fluent and Liquid Glass with any accent, at every interface scale
(1024×600 at 150 % with a panel: the places, the preview and the hidden
files check box give way to the files when the window is narrow). The
stock parts come from the SDE UI kit and follow it: the filter menu
(`Fl_Choice`), the check box (`Fl_Check_Button`), the buttons
(`sde::FlatButton`), the scroll bar and the text fields' context menus. It
is a modal dialog window, transient for the program's window and centred
on it. All texts are translated (`_()`).

**Remembers** (`$XDG_STATE_HOME/sde/file-chooser.conf`, shared by all
programs): the dialog's size, grid or list, the sort order, hidden files and
the preview, and the last folder of each program (`[folders]
sde-settings=/home/alex/Pictures/Nature`). Chosen files are added to the
recently used files with the program's name, as GTK programs do, so they
show under Recent in every file chooser.

## Speed

On the test machine (Xvfb, Openbox): the first frame within 6–25 ms of
`run()`, also for a folder of 10 000 files, which is completely read about
0.1 s later; the reader hands over its first batch after about 2 ms
(`sde-file-chooser-model`). `SDE_FILE_CHOOSER_TIMING=1` prints the time of
the first frame; the QA dump has `@opened-ms` and `@loaded-ms`.

## Using it

```cpp
#include "file_chooser.h"

sde::FileChooser chooser(sde::FileChooser::Mode::Open);  // OpenMultiple, Save, Folder, Folders
chooser.title(_("Choose a picture"));
chooser.filters(std::string(_("Pictures")) + "\t*.{png,jpg,jpeg,svg}");
chooser.all_files(false);                 // no "All files" in the menu
chooser.default_folder(user_dir("PICTURES"));
chooser.preview(true);
chooser.parent(this);                     // optional: the program's window
if (chooser.run()) use(chooser.path());   // paths() for several
```

* `folder(dir)` starts there (the folder of the current document);
  `default_folder(dir)` starts there unless the program last chose
  something elsewhere (Pictures for a picture, then where the last one
  came from). Neither: the last folder, else home.
* `file_name(name)`: saving, the proposed name (a path sets the folder
  too); opening, a file to select.
* `filters(spec)` accepts Fl_Native_File_Chooser's syntax
  (`"JPEG\t*.{jpg,jpeg}\nPNG\t*.png"`), fl_file_chooser's
  (`"Pictures (*.{png,jpg})"`) and plain patterns (`"*.deb"`); patterns
  ignore case. `add_filter(name, patterns, mime_types)` adds MIME types
  (`image/*`; subclasses match). "All files" is added when opening with
  filters unless `all_files(false)`. `filter_index()` after `run()` is the
  filter in use.
* `confirm_overwrite(false)`, `new_folder(false)`, `accept_label(...)`,
  `add_to_recent(false)`, `program(id)` (the key of the remembered folder;
  the program's name by default), `parent_xid(xid)` (a window of another
  program).

Who uses it: Control Center (Users & Avatar: your picture and another
user's, Appearance: the wallpaper — all three with the preview), Debrun
(a package), Niny (Open Files, Save As), Photo (Open, Save Photo As, Save a
Copy Without Details, the collage's Add Photos, Replace and Export), Gallery
(Open a Folder, Open a Photo, Add a Folder to Gallery, Save as PDF, the
folders of Copy/Move/Export and Organize), Taca (folders, files and
archives to add, Extract To) and Xanara (Open Documents, Save a Copy As,
Choose an Image, a signature, the folders of Split and Export Images).

## Programs that use the portal

`xdg-desktop-portal-sde` (`src/portal/main.cpp`, installed in
`/usr/libexec/sde`) implements `org.freedesktop.impl.portal.FileChooser`
(`OpenFile`, `SaveFile`, `SaveFiles`) with the same dialog, so Flatpak
programs and the programs that ask xdg-desktop-portal for their file
dialogs (Firefox, Chromium, GTK 4 and Qt programs) show the SDE file
chooser too. `data/portals/sde.portal` declares it, D-Bus starts it on
demand (`org.freedesktop.impl.portal.desktop.sde`, activation file from
`data/portals/*.service.in`), and `data/portals/sde-portals.conf` chooses it
for `org.freedesktop.impl.portal.FileChooser` in SDE sessions (GTK stays
the default for the other portals). It understands `accept_label` (GTK's
`_` mnemonics dropped), `multiple`, `directory`, `filters` (globs and MIME
types, shown by their names), `current_filter` (chosen in the menu, or the
only filter when there is no list), `choices` (no controls are shown; their
default values are returned), `current_name`, `current_folder`,
`current_file` and `files` (`SaveFiles`: the chosen folder with the
program's names), and the parent window `x11:<XID>`: the dialog is
transient for that window and centred on it. `modal` is always honoured.
The reply has `uris`, `current_filter`, `choices` and `writable` (for
`OpenFile`); Escape answers 1 and `Request.Close` on the request's handle
closes its dialog (2). Each program remembers its own last folder (under
its application ID), and chosen files go to the recent files under that
ID. Dialogs run from the event loop, not inside libdbus's dispatch, so a
`Close` (or another program's request) gets through while one is open. The
service quits a minute after its last dialog (`SDE_PORTAL_IDLE_SECONDS`).

## Tests

* `sde-file-chooser-portal` (`tests/test_file_chooser_portal.py`, under
  `dbus-run-session` with Xvfb): the portal called with `gdbus` as
  xdg-desktop-portal calls it — `OpenFile` with several files, filters,
  `current_filter`, choices, `current_folder` and a parent window (the
  dialog is transient for it), a folder, `SaveFile`, `SaveFiles`, Escape and
  `Request.Close`.
* `sde-file-chooser-model` (`tests/test_file_chooser.cpp`, no display):
  natural sorting by name, size, date and type with folders first; hidden
  files (dot files, backups, `.hidden`), links; accent-free search; the
  three filter syntaxes, braces, MIME types, the extension of a filter;
  the selected part of a name and the name for a filter; Back, Forward,
  paths and locations (`~`, `file://`, relative); completion (any case,
  folders only, hidden names, UTF-8); the XDG places, GTK bookmarks,
  mounted drives from a mountinfo fixture and GVfs locations; the XBEL
  list (parsing, newest first, recording a file without losing the other
  programs' entries); MIME types, icons, sizes and dates; a folder of
  10 000 files read in batches, a new folder cancelling the old one, an
  unreadable folder, the search below a folder; thumbnails made, stored
  (mode 0600), found again and remembered as failed; the state file.
* `sde-file-chooser-ui` (`tests/test_file_chooser_ui.py`, Xvfb and
  xdotool): the dialog through `sde-file-chooser-demo` in a temporary home —
  places, thumbnails in the cache, double click, Back/Forward/Up with keys,
  the mouse's back button and Backspace, type-ahead, Enter, the recent list,
  the remembered folder, Escape; several files; folders and New Folder;
  saving (the selected stem, typing over it, the replace question, the
  extension following the filter menu); the location entry with completion,
  the path bar, hidden files, search below the folder, list view sorting,
  Recent; a drop over XDND (`sde-xdnd-peer`); 10 000 files (the first frame
  under 400 ms on a loaded CI machine, every file read); 1024×600 at 150 %
  with a panel.
* `sde-file-chooser-demo [--mode open|multi|save|folder|folders] [--title T]
  [--folder DIR] [--default-folder DIR] [--name NAME] [--filters SPEC]
  [--filter N] [--preview] [--no-all-files] [--parent] …` shows the dialog on
  its own (not installed; kept with the QA tools for CI).
* With `SDE_PANEL_QA_DIR` set the dialog writes `<dir>/File-Chooser.txt`:
  `@mode:`, `@location:`, `@view:`, `@selected:` (names, `|` between),
  `@name:`, `@name-selection:`, `@filter:`, `@query:`, `@loading:`,
  `@thumbnailing:`, `@items:`, `@total:`, `@accept:` … lines, then its
  controls (`back`, `crumb:N:<label>`, `place:<id>`, `item:<name>`,
  `column:<key>`, `name`, `filter`, `hidden`, `new-folder`, `cancel`,
  `accept` …) as `name x y w h active 0` in window pixels.

## Not done, ideas

* Unmounted drives are not listed (the panel's drive menu mounts them);
  drives cannot be ejected from the dialog.
* Network places are only those GVfs already mounted; there is no "Connect
  to Server".
* No rubber-band selection; the grid has one size (no zoom).
