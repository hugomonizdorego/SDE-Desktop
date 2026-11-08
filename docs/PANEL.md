# SDE Panel: battery indicator and clock

How the panel's battery icon and clock behave since 0.2.0 Alpha 2, which
settings change them and how they are tested. The code is in
`src/panel/status_model.*` (display-free rules, unit tested),
`src/panel/quick.cpp` (reading the battery), `src/panel/panel.cpp` (the icon
on the panel), `src/common/tray_icons.cpp` (its drawing) and
`src/panel/clockface.*` (the clock).

## Battery indicator

### Where the values come from

The panel asks **UPower** for its `DisplayDevice` (the batteries combined)
on the system bus: `Percentage`, `State`, `WarningLevel`, `Energy`,
`EnergyFull`, `EnergyRate`, `TimeToEmpty`, `TimeToFull`, and `OnBattery` of
the daemon. It reads them every 30 seconds and when UPower announces a
change. When UPower is not running or reports no battery, the kernel's
`power_supply` class (`/sys/class/power_supply`) is read instead: `capacity`,
`status`, `energy_now`/`energy_full`/`power_now` (or the `charge_*` and
`current_now` files) and the `online` state of a mains adapter. The maker
and model shown in the flyout (`45% LGES AP23A8L`) always come from
`power_supply`.

### Phases, colours and animations

| Phase | When | Icon | Animation |
| --- | --- | --- | --- |
| On battery | discharging, above the low level | the level in the text colour | none |
| Low battery | on battery at 20 % or less (or `[power] battery_low` when that is higher), or UPower's warning level "low" | amber level, the body tinted amber | a gentle pulse, 2 s |
| Critically low | on battery at 10 % or less (or `[power] battery_critical` when higher), UPower's warning level "critical" or "action", or an empty battery | red body with "!" | a pulse, 1.2 s |
| Charging | UPower "charging" / power_supply `Charging` | green level with a bolt | a lighter band fills the rest of the battery and fades, 2 s |
| Full | on mains power, fully charged | full green battery with a plug | none |
| Plugged in | on mains power, not charging (a charge limit; UPower "pending-charge", power_supply `Not charging`) | the level with a plug | none |

When the percentage changes, the level eases to the new value in 0.8 s.

The animations are cheap: a timer runs only while the battery charges, is
low or critically low, or while the level eases; it draws 12 frames per
second (6 on the power saver profile) and repaints only the battery's slot
of the panel. Nothing runs while the panel is hidden (auto-hide) or the icon
is not shown.

**Reduced motion.** `[panel] battery_animation=auto` (the default) animates
unless the Effects page turned effects off (`[effects] enabled=false`), Low
resource mode is on (`[appearance] low_resource`), `[appearance]
reduced_motion=true` or `[aero] animations=false`; the icon then shows the
same colours and marks without moving. `true` and `false` decide on their
own.

### Text after the icon

`[panel] battery_display` chooses what follows the icon:

| Value | Panel |
| --- | --- |
| `icon` (default) | the icon alone |
| `percent` | `45%` |
| `label` | `On battery`, `Low battery`, `Critically low`, `Charging`, `Full`, `Plugged in` |
| `label-percent` | `On battery · 45%` |

With `[panel] battery_time=true` the label shows the estimate instead of the
state while there is one: `3 h 57 min left`, `1 h 10 min to full`,
`Calculating…`.

Both are chosen from the battery icon's right-click menu (**Show on the
Panel**: Icon Only, Percentage, Label, Label and Percentage, Time Remaining
in the Label) and in Control Center > **Power** > Battery (**On the panel**,
**Show the time remaining in the label**); the menu's **Power Settings...**
opens that card (`sde-settings --page battery`). The panel lays itself out again
at once, in every panel style and position. The text's width is measured
with every digit as "8", so the panel does not move each time a digit
changes; when the status area has no room for it, a label gives way to the
percentage and then to the icon alone.

### Time estimates

* While charging: the time until full; on battery: the time until empty.
* UPower's own `TimeToFull`/`TimeToEmpty` are used whenever UPower has them.
* Otherwise the energy left (or still missing until full) is divided by the
  rate. The rate is smoothed exponentially (time constant 150 s, weighted by
  the time between readings), so that a burst of load moves the estimate
  gently instead of making it jump: 30 seconds at 30 W after a steady 10 W
  turn 3 h 57 min into about 2 h 54 min, not 1 h 19 min; a lasting change is
  followed within a few minutes.
* Without a reported rate, the rate comes from how fast the energy (or else
  the percentage) changed over at least a minute.
* Switching between charging and discharging starts over: "Calculating…"
  is shown until there is a rate, for at most ten minutes. Estimates over
  48 hours are not shown.
* Full and plugged-in batteries have no estimate.

The estimate appears in the icon's tooltip (`Battery 45% · 3 hours 57
minutes remaining`, `Battery 52% · charging · calculating…`), in the first
line of the right-click menu, in the battery flyout (`Using battery power -
3 hours 57 minutes remaining`, `Charging - 1 hour 10 minutes until full`,
`Low battery - 25 minutes remaining`, `Charging - Calculating…`) and, with
`battery_time`, in the label.

## Clock

The clock on the panel and the Action Center's clock share one setting:

* **Style** (`[action_center] clock_style`): Digital, LED (seven-segment
  digits) or Analog.
* **Show seconds** (`[panel] clock_seconds`): the seconds in the digital and
  LED clocks, a seconds hand on the analog face.
* **Blink the colon** (`[panel] clock_blink`): the colon of the digital and
  LED clocks flashes once a second. The analog face has no colon: the
  clock menus grey the option out while it is shown, and the choice stays
  for the digital and LED faces.

Seconds and the blinking colon are chosen independently, in Control Center
> **Clock & Weather** (three style cards and two switches), in Control
Center > Action Center (Clock card), in the panel clock's right-click menu
(Clock Style, Show Seconds, Blink the Colon) and in the right-click menu of
the Action Center's clock. The panel redraws its clock once a second only
when seconds or a blinking colon are shown; otherwise once a minute (an LED
clock no longer wakes the panel every second).

Up to 0.2 Alpha 1 "Blink" was a style of its own. `clock_style=blink` (or
`[clock] style=blink`) now means the digital clock with the blinking colon,
and the panel and Control Center rewrite it once to `clock_style=digital`
with `[panel] clock_blink=true` (a `clock_blink` the user set stays).
`[clock] style`, `[clock] seconds` and `[clock] blink` are read as aliases
when the keys above are not set.

## Tests

* `tests/test_panel_status.cpp`: phases from UPower and power_supply,
  display modes and texts, tooltip and flyout lines, reduced motion, the
  estimator (UPower times, smoothing, bursts, phase changes, energy and
  percentage rates, "Calculating…") and the animation timeline.
* `tests/test_clockface.cpp`: styles, the two options, the aliases and the
  migration of the old style.
* `tools/panel-smoke.py` (`--only battery-clock` runs just these): the
  fixture's UPower (`tests/panel_service_fixture.cpp`, mode
  `battery state=charging percent=52 rate=25 ttf=4200 ...`, modes
  `upower-off`/`upower-on`), display modes from sde.conf and the menu in
  every panel style, each phase with its animation (pixels of the icon's
  slot change, or not), reduced motion, estimates and "Calculating…", the
  power_supply fallback; the clock styles with every option on the panel
  and in the Action Center, both clock menus and the migration. The panel
  describes the battery in its QA dump: `@battery:<phase>:<anim|steady>`,
  `@battery-text:`, `@battery-time:` (minutes, -1 or `calculating`) and
  `@battery-source:`; the Action Center adds `@clock-seconds` and
  `@clock-blink`.
* `tools/showcase.py`: the battery scenes (charging with the label, low
  battery) and the clock styles with their options.

## Session buttons

Since 0.2.0 Alpha 3.1 **Sleep**, **Log Out**, **Restart** and **Shut Down**
are always in the Action Center's footer, a row of four round icon buttons
right of the user, Control Center and Lock Screen (there is no Power button
to open them any more). The row has the same place and height in every
layout of the Action Center (compact, expanded, one or two columns), so it
never pushes the tiles, sliders or calendar around; at 1024x600 at 150 % the
user's name gives way (with an ellipsis) before anything else does.

* **Icons.** SDE's own glyphs, drawn with cairo on the 24-unit grid of the
  tray icons (`src/panel/session_actions.cpp`): a crescent moon with a "z"
  (Do Not Disturb keeps the filled moon), a door with an arrow leaving it,
  a circular arrow and the power symbol. They are sharp at every scale and
  take the theme's colours: the text colour at rest, the accent on Sleep and
  the theme's error colour on the other three while hovered; Shut Down keeps
  a hint of the error colour at rest.
* **Names.** Each button's tooltip is its name ("Sleep", "Log Out",
  "Restart", "Shut Down"); the QA dump and the UI tests name them by it.
* **Keyboard.** Tab and the arrow keys reach the buttons like every control
  of the Action Center; Enter or Space presses the focused one.
* **Accidental clicks.** Sleep acts at once (nothing is lost). Log Out,
  Restart and Shut Down ask for a second click on the same button: the first
  click arms it - it turns red, a ring inside it runs down for four seconds
  and "Click again to shut down" takes the place of the user and the two
  buttons beside it - and a second click (or Enter) within those four
  seconds runs it. A second click within 0.4 s of the first is ignored (a
  double click or a touchpad tap counted twice is not a confirmation);
  waiting, a click on the question, Escape or closing the Action Center
  disarms. This was chosen over sde-logout's confirmation dialog because it
  stays in place: no second window, no change of keyboard focus (the armed
  button keeps it, so Enter twice works), and one deliberate gesture is
  still needed. The Start menu's one Power off button keeps its question.
* **What the system cannot do** is left out: the panel asks
  systemd-logind (`CanSuspend`, `CanReboot`, `CanPowerOff` on the system
  bus) when it starts and whenever the Action Center opens, without
  waiting for the answers. `na` (e.g. no suspend support) and `no` (polkit
  refuses) hide the button; `challenge` keeps it (polkit asks for a password
  then). Without logind (no answer) the buttons stay, and Log Out is always
  there. The buttons run `sde-logout --action suspend|logout|reboot|poweroff`.

Tests: `tests/test_session_actions.cpp` (order and commands, logind's
answers, arming and its timing, the glyphs inside their box at 16-48 px and
different from each other; `sde-session-actions-tests FILE.png` draws them
for a look); `tools/panel-smoke.py` (`--only action-center` runs these
too): the row in every plan of the alpha52 checks (one row, in order, round,
inside the window, below the body, overlapping nothing), the first click
arming Restart (`@session-armed:reboot` in the QA dump, the question in
place of the user, nothing run), disarming after four seconds and with a
click on the question, two clicks running `sde-logout --action reboot`,
Return on the armed Shut Down, Sleep at once, and logind's answers from the
fixture (`mode login1 suspend=na reboot=challenge poweroff=no`: Sleep and
Shut Down hidden, `@session-can:na,?,challenge,no,`). `tools/showcase.py`:
`action-center` and `action-center-power` (Shut Down armed).

## Windows that do not respond

Since 0.2.0 Alpha 3.1 a window whose program stopped answering
(`_SDE_NOT_RESPONDING`, set by `sde-watchdog` or SDE WM; see
[FORCE-CLOSE.md](FORCE-CLOSE.md)) has a dimmed taskbar button labelled
"*Title* (Not Responding)" in the user's language (SDE WM's English title
suffix is left out of the button), and the button's right-click menu
offers **Force Close** above Close Window, which runs `sde-force-close
--kill-window` (the checks and the kill stay in that one program). The
panel follows the property without polling; the QA dump lists such
buttons as `@task-not-responding:<title>`. Tested in `tools/panel-smoke.py`
(the a52 menu checks).

## Badges on the taskbar

Since 0.2.0 Alpha 3 the pinned launchers and the taskbar buttons show the
live badges of the Start menu ([START-MENU.md](START-MENU.md#live-badges)):
the count of an application's unread notifications, or the count it sends
with the Unity LauncherEntry API, on the top right corner of its icon, in
the application's colour with a ring of the panel's colour ("99+" above
99); a dot when it only asks for attention; its progress as a thin bar
below the icon. A taskbar button finds its application by its window's
class (`WM_CLASS`: the desktop file's name, `StartupWMClass=`, the program
of `Exec=`). When a window of the application becomes active, its
notifications are read and their count goes; a count the application sends
itself stays until the application changes it. The badges change without
polling, and only the launchers and buttons are repainted.

Tests: `tools/panel-smoke.py --only start-menu` sends notifications with
`notify-send` (with and without a `desktop-entry` hint) and counts with
`sde-launcher-entry-fixture` (`tests/launcher_entry_fixture.cpp`), opens
windows of the application under `sde-wm` and reads the panel's QA dump:
`@app-badge:<desktop ID>=<count>[:urgent][:progress <percent>]`,
`@pin-badge:<desktop ID>=<count>:#<colour>` and
`@task-badge:<window title>=<count>:#<colour>`.

## Workspaces and keyboard shortcuts on the taskbar (0.2.0 Alpha 4)

The taskbar shows the windows of the workspace in view and those on every
workspace; it followed no workspace before. A switch of workspace
(`_NET_CURRENT_DESKTOP`) and a window moving to another one (its
`_NET_WM_DESKTOP`) change the buttons at once. `[workspaces] taskbar_all=true`
(Control Center > Workspaces) shows every window. See
[WINDOWS.md](WINDOWS.md).

A taskbar button's menu has the window's items of its title bar menu
(src/common/window_menu.cpp): Restore, Move, Size, Minimize, Maximize,
Always on Top, Move to Workspace (with All Workspaces and New Workspace),
Close (Alt+F4), around the application's own (its other windows, New
Window, Pin, Force Close, Close All Windows).

The Start button's right-click menu is the **Quick Link menu** of Windows,
also opened with **Win+X** (`sde-panel --quick-link-menu`): Apps (Merkadu's
installed apps), Power Options, System Reports, System, Driver Manager,
Network Connections, Disks, Terminal, System Monitor, Control Center,
Files, Search, Run Command, Shut Down or Log Out (Lock, Log Out, Sleep,
Restart, Shut Down), Desktop, Software Updater and Panel Settings.

Keyboard (src/panel/taskbar_keys.cpp, [SHORTCUTS.md](SHORTCUTS.md)):

* **Win+1 … Win+9** (`sde-panel --taskbar N`): place N counts the visible
  pinned apps, then the windows. A pinned app starts, or its window comes
  up (the next one when it has several), or goes down when it is the
  active one; a window button acts as a click.
* **Win+Shift+1 … 9** (`--taskbar-new N`): another window of that app.
* **Win+T** (`--focus-taskbar`): the taskbar takes the keyboard; each Win+T,
  Tab or arrow key goes to the next button and shows its preview, Enter
  opens it, the Menu key or Shift+F10 its menu, Esc goes back to the window
  that was active. Clicking elsewhere ends it.
* **Win+N** (`--notifications`) opens the notifications, **Win+B**
  (`--section tray`) the tray.

Menus opened by a shortcut wait until the window manager lets go of the
keyboard (it holds it until the key is released), so their keys work.
`tools/panel-smoke.py` checks the taskbar menu's items and the Quick Link
menu; `tests/test_workspaces_ui.py` the taskbar per workspace.
