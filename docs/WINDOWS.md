# Windows and workspaces in SDE

How SDE manages windows since 0.2.0 Alpha 4: the window menu, snapping,
workspaces, Task View, the taskbar per workspace, Ctrl+Alt+Delete and
screenshots. Every keyboard shortcut is in [SHORTCUTS.md](SHORTCUTS.md)
(and the printable sheet `SDE-Keyboard-Shortcuts.pdf`).

SDE's window manager is **SDE WM**: Openbox 3.6 built from source with
SDE's patches (`suite/patches/sde-wm`). Its configuration is
`~/.config/sde/openbox/rc.xml`, made from `data/openbox/rc.xml` at the first
login; SDE keeps the theme, the button layout, the workspaces and the block
of keyboard shortcuts in it up to date (`sync_openbox()`, src/common/wm.cpp)
at every login and settings change.

## The window menu

A right click on a title bar, a click on the window's icon at its left end
and **Alt+Space** open the window menu, drawn by SDE in its menu style,
theme and scale:

| Item | What it does |
| --- | --- |
| Restore | A maximized, minimized or full-screen window goes back to its size |
| Move | Move the window with the arrow keys or the mouse; Enter ends, Esc cancels |
| Size | Resize it the same way |
| Minimize, Maximize | As the caption buttons |
| Always on Top | The window stays above the others (a check mark when on) |
| Move to Workspace ▸ | The workspaces by name, All Workspaces (the window is on each), New Workspace |
| Close (Alt+F4) | Close the window |

Items that do not apply are greyed out (Restore for a normal window,
Maximize for a maximized one, what the window does not allow according to
`_NET_WM_ALLOWED_ACTIONS`). From the mouse the menu opens at the pointer;
from the keyboard and the icon, below the title bar at the window's left
edge (above the window when there is no room below).

The taskbar button's menu has the same items in the same order
(src/common/window_menu.cpp), with the application's own around them: its
other windows, New Window, Pin to Panel, Force Close for a program that does
not respond, Close All Windows.

How it works: SDE WM patch 0006 (`0006-sde-window-menu.patch`) makes
Openbox's `ShowMenu` of `client-menu` run
`sde-window-menu --window XID --at X,Y [--above Y]`. The program waits until
the window manager lets go of the key or button that opened it (so its keys
work), shows the menu and sends EWMH requests for the chosen item
(`_NET_WM_STATE`, `_NET_WM_MOVERESIZE` keyboard move and size,
`_NET_WM_DESKTOP`, `_NET_CLOSE_WINDOW`). When the program cannot be started,
Openbox shows its own menu as before. `SDE_WM_WINDOW_MENU` in SDE WM's
environment names another program, `openbox` keeps Openbox's menu.
`SDE_WINDOW_MENU_QA_DIR` makes the menu describe its items for the tests.

## Snapping and minimizing

| Keys | A window... |
| --- | --- |
| Win+Left, Win+Right | ...goes to the left or right half of the work area (the panel stays free) |
| Win+Up | ...is maximized; a half becomes its top quarter, a bottom quarter its half |
| Win+Down | ...is restored when maximized; a top quarter becomes its half, a half its bottom quarter; otherwise it is minimized |
| Win+Left on a right half (and the other way) | ...goes back to where it was before it was snapped |
| Win+Shift+Up / Down | ...is stretched to the top and bottom of the screen / gets its height back |
| Win+Shift+Left / Right | ...moves to the next monitor |

A snapped window remembers its place before the snap in the `_SDE_SNAP`
property (zone, x, y, width, height). When it is moved or resized by hand it
no longer fills its zone and counts as not snapped. The monitor is the one
under the window's centre (XRandR), the work area leaves out the panel
(`_NET_WORKAREA`).

**Win+M** minimizes every window of the workspace in view and remembers
them (`_SDE_MINIMIZED_ALL` on the root window); **Win+Shift+M** brings back
those, or every minimized window of the workspace when Win+M did not
minimize any. **Win+Home** minimizes all windows but the active one;
pressed again (while the same window is active) it brings them back
(`_SDE_MINIMIZED_OTHERS`). **Win+D** shows the desktop and the windows again.

These are `sde-windows` commands (`snap left|right|up|down`, `minimize-all`,
`restore-minimized`, `minimize-others`) that SDE WM runs for the keys:
plain Xlib requests, a few milliseconds each.

## Workspaces

SDE has 1 to 5 workspaces (2 unless you change it), each with a name.
**Control Center > Workspaces** (`sde-settings --page workspaces`, also
"desktops" and "multitasking") sets:

* the number of workspaces, 1 to 5; windows on a workspace that goes away
  move to the last one that stays;
* their names (an empty name is "Workspace N" in your language);
* whether Ctrl+Win+Left/Right go round from the last workspace to the first;
* whether the taskbar and Alt+Tab show the windows of every workspace (off:
  only those of the workspace in view, as on Windows).

The page shows the workspaces as small screens (a click goes to one) and
opens Task View and the shortcut sheet.

| Keys | |
| --- | --- |
| Ctrl+Win+Left / Right (or Ctrl+Alt+Left / Right) | the workspace on the left / right |
| Ctrl+Win+Shift+Left / Right (or Ctrl+Alt+Shift+...) | take the active window there and follow it |
| Ctrl+Win+D | add a workspace (up to 5) and go to it |
| Ctrl+Win+F4 | close the workspace in view |
| Win+Tab | Task View |

Closing a workspace moves its windows to the workspace before it (the next
one when it is the first), the workspaces after it move down by one with
their names, and the neighbour is shown. Ctrl+Win+D and Ctrl+Win+F4 change
the number in sde.conf too, so it stays the next time; at 5 and at 1 they do
nothing.

In sde.conf:

```ini
[workspaces]
count=2          # 1 to 5
name1=           # name1 ... name5; empty: "Workspace N"
wrap=false       # Ctrl+Win+Left/Right go round
taskbar_all=false
alt_tab_all=false
```

How it works (src/common/workspaces.cpp): Openbox keeps the number of
desktops it has when it reloads its configuration, so SDE sets both: the
`<desktops>` of rc.xml (for SDE WM's next start) and, at once, the EWMH
properties of the running window manager (`_NET_NUMBER_OF_DESKTOPS`
request, `_NET_DESKTOP_NAMES`). Windows move with `_NET_WM_DESKTOP`
requests. `sde-windows workspace new|close [N]|list` does what the keys do.

### The taskbar

The taskbar shows the windows of the workspace in view and those on every
workspace (since Alpha 4; before, it showed all of them). It follows
workspace switches and windows moving between workspaces at once.
`[workspaces] taskbar_all=true` shows every window.

## Task View

**Win+Tab** (and Ctrl+Alt+Tab) opens Task View, `sde-taskview`: over a
soft, dim picture of the screen, the windows of the workspace in view as
large cards, and the workspaces in a row at the bottom with
**New workspace**.

* Click a window to go to it (and to its workspace); middle click or × to
  close it; right click for its window menu.
* Drag a window onto a workspace (or onto New workspace) to move it there.
* Point at a workspace for a moment to see its windows; click it to go
  there; × on a workspace closes it.
* Keys: arrows and Tab choose a window, Enter opens it, Delete closes it,
  Page Up / Page Down or Ctrl+Left / Ctrl+Right show another workspace's
  windows, 1 to 5 jump to a workspace, Ctrl+Win+D adds one, Esc or
  Win+Tab again leave. A click on the empty background leaves too.

The cards of the workspace in view show the windows as they are: the
compositor's picture of each window when SDE's compositor runs (also of
windows that others cover), otherwise the window as it is on the screen.
Windows on other workspaces and minimized ones are not on the screen (SDE
WM unmaps them), so their cards show the application's icon and title.
Running `sde-taskview` again closes it. `SDE_TASKVIEW_QA_DIR` makes it
describe its cards for the tests.

## The Win key

The Win key alone opens the Start menu when it is let go. SDE WM patch 0007
(`0007-keybinds-on-release.patch`) gives Openbox keybinds with
`onRelease="yes"`: their actions run when the key is let go, and only if no
other key or mouse button was pressed while it was down. Before Alpha 4 the
Start menu opened as soon as Win went down, so every Win+key shortcut also
opened it.

## Ctrl+Alt+Delete

`sde-logout --security` shows the security options of Windows over the
dimmed screen: **Lock**, **Switch User** (with LightDM's dm-tool),
**Log Out**, **Change a Password** (Control Center > Users & Avatar) and
**Task Manager** (System Monitor), **Cancel**, and the power button at the
bottom right with Sleep, Restart and Shut Down. Arrows or Tab and Enter
choose, Esc cancels. Before Alpha 4, Ctrl+Alt+Delete opened the log out
dialog. Alt+F4 on the desktop opens that dialog, as on Windows.

## Screenshots

`sde-screenshot` takes the screenshots of PrtScn (the whole screen),
Alt+PrtScn (the active window, with its title bar) and Win+Shift+S (a snip:
drag an area, or choose Window or Full screen in the toolbar at the top;
Esc cancels). Each picture is copied to the clipboard as PNG and saved as
`~/Pictures/Screenshots/Screenshot <date> <time>.png` (the folder follows
`user-dirs.dirs`); a notification says so. A small process of its own
serves the clipboard until another program takes it, at the latest after
12 hours. `--no-save`, `--no-clipboard`, `--output FILE` and
`--delay SECONDS` change that.

## The programs

| Program | |
| --- | --- |
| `sde-window-menu` | the window menu (SDE WM runs it) |
| `sde-windows` | snapping, minimizing, workspace new/close, next keyboard layout (Win+Space), emoji (Win+.), the shortcut list (`shortcuts`), the sheet (`sheet`), `list` |
| `sde-taskview` | Task View |
| `sde-screenshot` | screenshots |
| `sde-logout --security` | Ctrl+Alt+Delete |

Code: src/common/workspaces.*, shortcuts.*, window_menu.*;
src/windows/ (the programs; snap, taskview_model and screenshot_model need
no display); src/settings/workspaces_page.cpp; src/panel/taskbar_keys.cpp
(Win+1..9, Win+T, Win+X); src/logout/security.cpp.

Tests: `tests/test_windows.cpp` (sde-windows-model: the shortcut list and
its keybinds, the rc.xml migration, workspaces, snapping, Task View's
layout), `tests/test_shortcuts.py` (the sheet against the list),
`tests/test_window_menu_ui.py` and `tests/test_workspaces_ui.py` (on Xvfb;
the parts that need SDE WM's patches are skipped with another window
manager).

## Not yet

* Thumbnails of windows on other workspaces (SDE WM unmaps them; Task View
  shows their icons).
* Snap layouts (Win+Z) and snapping by dragging a window to a screen edge.
* A clipboard history (Win+V) and a magnifier (Win+Plus).
* Openbox's Alt+Tab switcher stays Openbox's, in the SDE window theme.
