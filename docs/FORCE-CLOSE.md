# Force Close: programs that freeze, never open or crash

Since 0.2.0 Alpha 3.1 SDE notices a program that stops responding or never
shows its window, and offers **Wait** or **Force Close**. It is built on the
standard X11 mechanisms (EWMH `_NET_WM_PING`, the X Resource extension,
`WM_CLIENT_MACHINE`/`_NET_WM_PID`, a child subreaper) rather than on
anything SDE-specific in the applications, so GTK, Qt, Electron, FLTK and
plain Xlib programs are all covered.

## What the user sees

* **Not Responding.** A window that does not answer for 5 seconds after it
  was used (focused, clicked, or asked to close) gets " - [Not Responding]"
  in its title bar (SDE WM), a dimmed taskbar button that says "*Title* (Not
  Responding)", and **Force Close** in the button's right-click menu. The
  first answer from the program removes all of it.
* **The dialog.** After 2 minutes without an answer - or at once when the
  user tries to close a window that is not responding - a dialog shows the
  program's icon and name: "*App* is not responding. You can wait for it or
  force it to close; unsaved changes will be lost." with **Wait** (the
  default button, also Escape and closing the dialog) and **Force Close**
  (red). One dialog per program. It closes by itself when the program
  answers again or its window goes away. After Wait it comes back only
  when the program still has not answered after another 2 minutes.
* **Programs that do not open.** When an application started from SDE (Start
  menu, taskbar, desktop, search, Run dialog) still runs after 2 minutes and
  neither it nor any process it started shows a window (also no tray icon
  window), the dialog says "*App* has not opened yet. It started 2 minutes
  ago but has not shown a window." Wait there ends the watch of that
  program (it may be meant to run in the background, like a tray-only
  application); the dialog closes by itself if a window appears.
* **Force Close** ends the program at once with `SIGKILL` - no `SIGTERM`
  first, as asked - and disconnects it from the X server, so its windows go
  immediately even when the process cannot take the signal yet. For a
  program that did not open, everything it started goes with it.
* **Crashes.** When an application started from SDE dies from a crash signal
  (`SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGSYS`, `SIGTRAP`),
  a notification says "*App* closed unexpectedly. It stopped because of an
  error (Segmentation fault)." with **Open Again** and **Details** (System
  Reports, which lists the crash from the session log).

## How it works

```
 sde-session ── supervises ──> sde-watchdog ──> sde-force-close (dialog)
     │  child subreaper             │ pings, marks       ▲
     │  (crash: --crashed)          │                    │ close attempt
     ▼                              ▼                    │
 launch records  <── launch_entry() / sde-run       SDE WM (patch 0005)
 $XDG_RUNTIME_DIR/sde/launches/<pid>                 panel: dimmed button,
                                                     Force Close (--kill-window)
```

### Where the watchdog lives, and why

`sde-watchdog` is a small program of its own, started and restarted by
`sde-session` like the panel, not a part of the panel:

* the panel is a large program that talks to many services; if it hangs or
  restarts (safe mode, theme changes, crashes), frozen applications would
  go unnoticed - and the panel itself may be the program that freezes;
* the launch watchdog and the crash path need the session's view of the
  programs SDE started (the records, `sde-session` as the subreaper), not
  the panel's;
* it never ends anything itself: it pings, marks and runs the dialog, so it
  needs no rights and holds no state that matters when it restarts (marks
  left by a previous watchdog are checked again).

The panel only reads the mark (dimmed button, the menu item) and runs
`sde-force-close --kill-window`; every kill goes through the one audited
program, `sde-force-close`.

### Detecting a frozen window: `_NET_WM_PING`

EWMH lets a window list `_NET_WM_PING` in `WM_PROTOCOLS`; a ping is a
`WM_PROTOCOLS` client message sent to the window, and a program that still
runs its event loop sends it back to the root window. `sde-watchdog` selects
`SubstructureNotifyMask` on the root window to see those answers (the
window manager's and the dialog's as well: any answer from a window means
its program responds) and pings:

* the active window whenever `_NET_ACTIVE_WINDOW` changes,
* the active window after a click anywhere (XInput 2 raw button presses,
  without grabs; the wheel's buttons 4-7 are ignored),
* a window someone asks to close with `_NET_CLOSE_WINDOW`,
* a pending window again every 2 seconds.

Its ping ids start at `0x5DE00000` (the dialog's at `0x5DF00000`), far from
SDE WM's small counter, so that neither takes the other's answer for its
own. A window that has not answered for `mark_after` seconds is marked,
together with every other managed window of the same process.

The mark is the property **`_SDE_NOT_RESPONDING`** (CARDINAL, when it
stopped answering, seconds since the epoch) on the client window. It is
set and removed by `sde-watchdog`, and by SDE WM when its own ping after a
close attempt finds the window dead or alive again; it is read by SDE WM
(the title), the panel (the button) and the dialog (it closes when the mark
goes).

**SDE-FLTK** did not answer pings before this release: its windows now list
`_NET_WM_PING` and `fl_handle()` returns the ping to the root window, so a
hung SDE program is detected like any other (`sde-fltk/CHANGES.SDE-FLTK`).

### SDE WM (`suite/patches/sde-wm/0005-sde-force-close-dialog.patch`)

Openbox's ping code stays as it is: closing a window that lists
`_NET_WM_PING` pings it, and after two unanswered pings (about 6 seconds)
it is "Not Responding". The patch changes only what follows:
`client_prompt_kill()` runs `sde-force-close --window XID --reason closing`
instead of Openbox's own prompt (which stays as the fallback when
`sde-force-close` cannot be started), and `_SDE_NOT_RESPONDING` is shared
both ways - SDE WM reads the watchdog's mark (so closing a marked window
shows the dialog at once, and the title says so) and sets or removes it
after its own pings. The title suffix is Openbox's English " - [Not
Responding]" (SDE WM is built without NLS); the panel shows its own,
translated label instead.

### Programs that do not open

`spawn_detached()` reports the started process's id (the intermediate child
writes it into a pipe), and `launch_entry()` - used by the Start menu,
taskbar, desktop, search and others - and `sde-run` (for commands that are
an installed application) write a record to
`$XDG_RUNTIME_DIR/sde/launches/<pid>` (`src/common/launches.h`): the pid,
its start time from `/proc/<pid>/stat` (against pid reuse), the time, name,
icon, desktop file and command. Records are written only inside an SDE
session (`SDE_SESSION_VERSION`) or with `SDE_LAUNCHES_DIR`.

`sde-watchdog` follows the directory with inotify. When a record's program
still runs after `timeout` seconds, it looks for a window of the program's
**family** - the process, its descendants, and the processes of the session
it leads (SDE starts programs with `setsid()`): a managed window whose X
client belongs to the family, or any mapped window of one of the family's X
connections (a tray icon, a splash screen). None: the dialog. Records of
programs that ended are removed by `sde-session` (or, for programs it did
not adopt, by the watchdog after a grace period). Records older than twice
the timeout are not watched after a restart of the watchdog.

### Force Close, and what it refuses

`sde-force-close` finds the window's process from the **X Resource
extension** (`XResQueryClientIds`, the X server's own record of the
connection's peer; right for Flatpak programs, whose `_NET_WM_PID` is a pid
in the sandbox), else from `_NET_WM_PID` when `WM_CLIENT_MACHINE` names this
computer. A program on another computer is only disconnected
(`XKillClient`). It then sends `SIGKILL` through a **pidfd**
(`pidfd_open`/`pidfd_send_signal`), so that a recycled process number never
gets the signal, and `XKillClient` for the window.

It refuses (and the dialog is not even shown for):

| Verdict | Rule |
| --- | --- |
| `pid1` | PID 1 |
| `self` | `sde-force-close` itself and its ancestors (`sde-session`, the display manager...), and the session's `sde-session` from its pid file |
| `other-user` | the real, effective or saved user id is not the user's (setuid programs too) |
| `session` | a part of the session, by the name of its executable or its 15-character command name: `sde-session`, `startsde`, `sde-wm`, `openbox`, `sde-panel`, `sde-desktop`, `sde-notifyd`, `sde-compositor`, `picom`, `sde-xsettingsd`, the polkit agents, `sde-lock`, `sde-power`, `sde-autostart`, `sde-watchdog`, `sde-force-close`, the X servers (`Xorg`, `Xvfb`, `Xwayland`...), `dbus-daemon`, `systemd`, display managers, `sshd`... |
| `zombie` | already ended |
| `unknown` | cannot be identified: no readable executable (kernel threads), a window that the window manager does not manage, or no process and no remote host |
| `no-process` | gone, or a different process than the one the window or record named |

`sde-force-close --check-window XID` and `--check-pid PID` print the
verdict without ending anything.

### Crashes

`sde-session` is the session's child subreaper (`PR_SET_CHILD_SUBREAPER`),
so the programs SDE starts detached end up as its children and their exit
status arrives there. For a recorded program that died from a crash
signal it logs "*Name* exited (killed by signal 11 (Segmentation fault))" -
the line System Reports turns into a report - and runs `sde-force-close
--crashed`, which sends the notification and waits (at most 3 hours) for
its action: Open Again starts the desktop file again (or the command),
Details opens `sde-system-reports`. `SIGKILL` (Force Close, the OOM killer)
and termination requests are not reported as crashes.

## Settings

`sde.conf`:

```ini
[force_close]
enabled=true     # sde-session starts sde-watchdog; false: nothing is watched
mark_after=5     # seconds without an answer before "Not Responding" (at least 1)
timeout=120      # seconds before the dialog, also for programs that do not open (at least 2)
```

The watchdog follows changes at once (`_SDE_CONFIG_SERIAL`).

## Programs

| Command | Does |
| --- | --- |
| `sde-watchdog [--verbose]` | the watchdog (started by `sde-session`) |
| `sde-force-close --window XID [--reason frozen\|closing]` | the dialog for a window |
| `sde-force-close --launch PID` | the dialog for a program that has not opened |
| `sde-force-close --kill-window XID` | Force Close at once (the taskbar menu) |
| `sde-force-close --check-window XID` / `--check-pid PID` | the verdict only |
| `sde-force-close --crashed --name N [--icon I] [--desktop-id D] [--signal S] [-- CMD...]` | the crash notification |

Exit status of the dialog: 0 forced to close, 1 Wait, 2 resolved by itself,
3 already asked (the open dialog was raised), 4 refused, 64 usage.

## Tests

* `tests/test_force_close.cpp` (`sde-force-close-rules`): the rules against
  a made-up `/proc` (PID 1, ancestors, the session's pid file, other users
  including setuid, session programs by executable and by cut command
  name, a program replaced on disk, zombies, unreadable executables),
  families, `kill_now()` on a `sleep` child of the test (refused for a
  wrong start time, then sent), the watchdog's decisions (pings, marks,
  the dialog after the timeout and at once after a close attempt, one per
  program, Wait for the whole program, recovery, turned off), the settings
  and the launch records.
* `tests/test_force_close_ui.py` (`sde-force-close-ui`, Xvfb and a window
  manager): an SDE-FLTK window answers pings; the test client
  (`tests/frozen_client_fixture.cpp`, frozen with `SIGUSR1`) is marked
  after a click, the dialog opens after the timeout, Wait keeps it and the
  dialog returns only after another timeout, recovery closes the dialog
  and removes the mark, Force Close ends exactly that process with
  `SIGKILL`; a program without a window ("has not opened yet", Force Close
  and Wait); refused targets that are never ended (PID 1, the X server,
  an ancestor, a test client run as `sde-panel`); with a patched SDE WM a
  close attempt opens the SDE dialog; `sde-session` with stub components
  logs and announces a recorded program killed by `SIGSEGV`, and Open
  Again in the notification starts it again. Only processes the test
  started are ever signalled.
* `tools/panel-smoke.py`: a marked window's taskbar button and its Force
  Close item.
* `tools/showcase.py`: `force-close` (Merkadu not responding) and
  `force-close-launch`.

## Limits

* Programs that do not list `_NET_WM_PING` (rare: some old Xlib and Java
  programs) cannot be detected as frozen; SDE WM still offers Openbox's
  `XKillClient` for windows without `WM_DELETE_WINDOW`.
* Key presses do not trigger a ping (only focus changes, clicks and close
  attempts do), so a program that freezes while one only types is marked
  at the next click.
* A background program started from the menu that only shows a
  StatusNotifierItem tray icon (no X window) is asked about once.
* SDE does not set `DESKTOP_STARTUP_ID` (startup notification) yet; the
  launch watchdog matches windows by process family instead.
