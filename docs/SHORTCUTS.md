# SDE keyboard shortcuts

SDE uses the keyboard shortcuts of Windows (since 0.2.0 Alpha 4), so what
your hands know from Windows works on SDE too. **Win** is the key with the
Windows logo (called Super on Linux keyboards). Where SDE had shortcuts of
its own before, they still work next to the Windows ones (Ctrl+Alt+T,
Ctrl+Alt+L, Ctrl+Alt+Left/Right, Alt+F1, Alt+F2).

This page is the source of the printable shortcut sheet
`docs/SDE-Keyboard-Shortcuts.pdf` (`tools/make-shortcut-sheet.py`),
installed as `/usr/share/sde/doc/SDE-Keyboard-Shortcuts.pdf` and opened from
Welcome, the Start menu's documents and Control Center > Workspaces.
`tests/test_shortcuts.py` checks that every shortcut below is in SDE's
shortcut list and, for the system shortcuts, bound in SDE WM.

## Windows

| Shortcut | What it does |
| --- | --- |
| Alt+F4 | Close the active window; on the desktop, shut down or sign out |
| Alt+Space | Open the window menu (move, size, workspaces, always on top) |
| Alt+Tab | Switch to the next window (keep Alt down and press Tab again) |
| Alt+Shift+Tab | Switch to the previous window |
| Alt+Esc | Send the active window to the back and go to the next one |
| Win+Up | Maximize the window (a snapped half: its top quarter) |
| Win+Down | Restore a maximized or snapped window, or minimize it |
| Win+Left | Snap the window to the left half of the screen |
| Win+Right | Snap the window to the right half of the screen |
| Win+Shift+Up | Stretch the window to the top and bottom of the screen |
| Win+Shift+Down | Give the window its height back |
| Win+Shift+Left | Move the window to the monitor on the left |
| Win+Shift+Right | Move the window to the monitor on the right |
| Win+H | Minimize the active window |
| Win+M | Minimize all windows |
| Win+Shift+M | Restore the windows that Win+M minimized |
| Win+Home | Minimize all but the active window (again: restore them) |
| Win+D | Show the desktop, or the windows again |

A snapped window remembers where it was: Win+Left then Win+Right (or
Win+Down) puts it back. From a half, Win+Up and Win+Down go to its top and
bottom quarters.

## Workspaces and Task View

| Shortcut | What it does |
| --- | --- |
| Win+Tab | Task View: every workspace and its windows |
| Ctrl+Alt+Tab | Task View to pick a window with the arrow keys and Enter |
| Ctrl+Win+D | Add a workspace and go to it (up to 5) |
| Ctrl+Win+F4 | Close this workspace; its windows move to the one next to it |
| Ctrl+Win+Left or Ctrl+Alt+Left | Go to the workspace on the left |
| Ctrl+Win+Right or Ctrl+Alt+Right | Go to the workspace on the right |
| Ctrl+Win+Shift+Left or Ctrl+Alt+Shift+Left | Move the window to the workspace on the left, and go with it |
| Ctrl+Win+Shift+Right or Ctrl+Alt+Shift+Right | Move the window to the workspace on the right, and go with it |

In Task View: the arrow keys and Tab choose a window, Enter opens it,
Delete closes it, Page Up and Page Down (or Ctrl+Left and Ctrl+Right) show
the windows of another workspace, 1 to 5 jump to a workspace, Esc or
Win+Tab leave. Drag a window onto a workspace to move it there.

## Start menu, search and apps

| Shortcut | What it does |
| --- | --- |
| Win or Ctrl+Esc or Alt+F1 | Open or close the Start menu |
| Win+S or Win+Q | Search for apps, files and settings |
| Win+R or Alt+F2 | Run a command |
| Win+E | Open Liria, the file manager |
| Win+I | Open the Control Center |
| Win+A | Open the Action Center (quick settings) |
| Win+N | Show the notifications |
| Win+C or Win+Alt+D or Win+W | Show the calendar, agenda and weather |
| Win+X | Quick Link menu: apps, power, devices, Terminal, shut down or sign out |
| Ctrl+Alt+T | Open a terminal |
| Ctrl+Shift+Esc | Open System Monitor (the task manager) |

The Win key opens the Start menu when it is let go, so Win+D and the other
shortcuts with Win never open it.

## Taskbar and system tray

| Shortcut | What it does |
| --- | --- |
| Win+1 … Win+9 | Open the app at that place on the taskbar, or switch to it |
| Win+Shift+1 … Win+Shift+9 | Open another window of the app at that place on the taskbar |
| Win+T | Go through the taskbar buttons (arrows, Enter opens, Esc leaves) |
| Win+B | Open the system tray |

The places count the pinned apps first, then the windows. On an app that
is already open, Win+number switches to its window (to the next one when
it has several), and minimizes it when it is the active one.

## Screenshots and emoji

| Shortcut | What it does |
| --- | --- |
| PrtScn or Win+PrtScn | Screenshot of the whole screen, copied and saved |
| Alt+PrtScn | Screenshot of the active window, copied and saved |
| Win+Shift+S | Snip: drag an area, or pick a window or the whole screen |
| Win+. or Win+; | Emoji picker (IBus) |

Every screenshot is copied to the clipboard (paste it with Ctrl+V) and
saved in `Pictures/Screenshots`. The emoji picker is IBus's; without the
ibus package a notification says what to install.

## Session, security and system

| Shortcut | What it does |
| --- | --- |
| Ctrl+Alt+Delete | Security options: lock, switch user, sign out, task manager |
| Win+L or Ctrl+Alt+L | Lock the screen |
| Win+P | Display settings: monitors, arrangement and projection |
| Win+Pause | About this computer and SDE |
| Win+K | Connect Bluetooth devices |
| Win+Space | Next keyboard layout (Alt+Shift switches too) |
| Alt+Shift+F12 | Turn the compositor (window effects) off or on |
| Ctrl+Win+Shift+B | Restart the compositor when the screen looks wrong |

## In SDE's applications

| Shortcut | What it does | Where |
| --- | --- | --- |
| Ctrl+C | Copy | Text fields, Liria, Niny, Xanara, Photo, Calculator |
| Ctrl+X | Cut | Text fields, Liria, Niny, Xanara |
| Ctrl+V | Paste | Text fields, Liria, Niny, Calculator, the desktop |
| Ctrl+Z | Undo | Text fields, Liria, Niny, Xanara, Photo |
| Ctrl+Y or Ctrl+Shift+Z | Redo | Ctrl+Y: Xanara; Ctrl+Shift+Z: Niny, Photo, Xanara |
| Ctrl+A | Select everything | Text fields, Liria, Niny, Gallery, Taca |
| Ctrl+F | Find | Liria, Niny, Xanara |
| Ctrl+N | New window, document or archive | Liria, Niny, Photo, Gallery, Taca |
| Ctrl+O | Open a file | Niny, Xanara, Photo, Gallery, Taca |
| Ctrl+S | Save | Niny, Xanara, Photo |
| Ctrl+P | Print | Niny, Xanara, Photo, Gallery |
| Ctrl+W | Close the tab or window | Liria, Niny, Xanara, Photo, Taca |
| Ctrl+Q | Quit the application | Liria, Niny, Xanara, Gallery, Taca |
| Ctrl+Tab or Ctrl+Shift+Tab | Next or previous tab | Liria, Niny, Xanara |
| Ctrl+Plus or Ctrl+Minus or Ctrl+0 | Zoom in, out, or back to 100 % | Liria, Niny, Xanara, Gallery |
| F1 | About the application and its help | SDE's programs |
| F2 | Rename the selected file | Liria, the desktop, Gallery, Taca |
| F5 | Refresh | Liria, the desktop, Gallery, Taca |
| F11 | Full screen | Liria, Niny, Xanara, Photo, Gallery |
| Shift+F10 or Menu | Open the context menu | Liria, Niny, the desktop, Gallery, Taca, the Start menu |
| Alt+Enter | Properties of the selected file | Liria |
| Delete or Shift+Delete | Move to the trash; with Shift, delete for good | Liria, the desktop |
| Alt+Left or Alt+Right or Alt+Up | Back, forward, up one folder | Liria |
| Ctrl+L | Type a location | Liria |
| Ctrl+Shift+N | New folder | Liria, the desktop |
| Esc | Cancel, or close a dialog or menu | Everywhere |

## With the mouse

| Mouse | What it does |
| --- | --- |
| Right click on a title bar, or a click on the window's icon | The window menu |
| Double click on a title bar | Maximize or restore |
| Alt + drag | Move a window from anywhere in it |
| Alt + right drag | Resize a window |
| Middle click on a title bar | Send the window to the back |
| Right click on a taskbar button | The window's menu, with the app's own items |
| Right click on the Start button | The Quick Link menu (as Win+X) |

## Not available in SDE

| Shortcut | On Windows | On SDE |
| --- | --- | --- |
| Win+V | Clipboard history | Not yet: SDE has no clipboard history. Ctrl+V pastes the last copy |
| Win+Plus or Win+Minus or Win+Esc | Magnifier | Not yet: the compositor cannot zoom the screen |
| Win+G | Game Bar | Not available |
| Win+U | Accessibility settings | Not available yet: text size and scale are in Control Center > Appearance |
| Win+Ctrl+Enter | Narrator | Not available: start the Orca screen reader from the Start menu when it is installed |
| Win+Ctrl+O | On-screen keyboard | Not available: start Onboard from the Start menu when it is installed |
| Win+F | Feedback Hub | Not available |
| Win+Z | Snap layouts | Not available: use Win+Left, Win+Right, Win+Up and Win+Down |
| Win+Comma | Peek at the desktop | Not available: Win+D shows the desktop |

Win+H is voice typing on Windows; SDE has no dictation, and Win+H
minimizes the active window as it always has on SDE.

## Changing a shortcut

The system shortcuts are written into SDE WM's configuration
(`~/.config/sde/openbox/rc.xml`) between two marks at every login and
settings change, from SDE's shortcut list (`src/common/shortcuts.cpp`;
`sde-windows shortcuts` prints it with the name of each shortcut).

* In `~/.config/sde/sde.conf`, `[shortcuts]` gives a shortcut other keys in
  Openbox notation: `W-` Win, `C-` Ctrl, `A-` Alt, `S-` Shift, several keys
  separated by spaces; `none` turns it off. For example:

  ```ini
  [shortcuts]
  terminal=C-A-t W-Return
  emoji=none
  ```

* A `<keybind>` of your own in rc.xml after the line
  `<!-- SDE shortcuts end -->` stays, and SDE leaves its key to it.

The keybinds of SDE 0.2.0 Alpha 3.1 and earlier are replaced at the first
login with Alpha 4; any of them that you had changed stays yours.
