# SDE Panel

How parts of the panel behave, which settings change them and how they are
tested: the battery icon and the clock (since 0.2.0 Alpha 2), the session
buttons, windows that do not respond, badges, and since 0.2.0 Alpha 4 the
tray, the Start button and the Task View button. The battery code is in
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

## Tray icons

The tray shows two kinds of application icons: XEmbed icons (the System
Tray Protocol: GtkStatusIcon, IBus on SDE, Wine, older Qt and Electron) in
one host window that moves between the panel and the Tray flyout
(`src/panel/tray.*`), and StatusNotifierItems (AppIndicator, KDE, Qt,
Electron) that the panel draws itself (`src/panel/sni.*`, with the
`org.kde.StatusNotifierWatcher` service). Since 0.2.0 Alpha 4 a tray
program that misbehaves can no longer freeze the desktop, fill the tray or
turn icons black.

### What went wrong up to 0.2.0 Alpha 3.1

Reproduced on Xvfb with ibus-daemon and its GTK panel and with fake tray
programs (`tests/test_tray.py`):

* **The whole desktop froze** after a click on a StatusNotifierItem without
  an `Activate` method (libappindicator programs): the panel opened the
  item's menu inside the D-Bus reply handler, the menu's event loop
  dispatched the next session bus message, and libdbus waited for its own
  dispatch lock forever, with the menu's pointer and keyboard grabs held.
  Nothing on the screen took input any more.
* **The window manager hung after IBus restarted** (`ibus restart`, or its
  panel or XIM server crashing): SDE WM used the input method of
  `XMODIFIERS` for its key bindings and waited in `XDestroyIC()` for the
  IBus that was gone, so no window could be mapped, moved or focused, and a
  restarted panel stayed invisible.
* **Icons disappeared**: FLTK hides the `DestroyNotify` of foreign windows
  from `Fl::add_handler` handlers while an input context exists, so the
  tray never saw an icon go. Every restart of IBus left an empty slot;
  after two the icons went behind the overflow button, and a new icon that
  got the old window id was refused as a duplicate.
* **Icons turned black**: the X server copies a window of another depth
  (an ARGB icon) into its parent without the alpha channel, so transparent
  pixels were black; the host window was created black and kept the old
  colour after a theme switch; when the panel crashed, the icons were
  mapped as framed windows in the middle of the screen.
* Opening an item's menu blocked the panel for up to 2 s when the program
  did not answer, and each `NewIcon` signal cost one property read.

### How the tray works now

* The tray's X events come through `Fl::add_system_handler`, before FLTK.
* Dock requests are queued and handled in batches of 16, deduplicated and
  checked: never the root window, the panel's own windows or a mapped
  application window; a window inside another one only when it is an
  XEmbed client (it has `_XEMBED_INFO`). At most 64 icons. A window that
  docks more than 8 times in 3 seconds is docked once more 3 seconds later.
  Requests on icons run under an error trap; the tray waits only for the X
  server, never for a tray program.
* A dock request for an embedded window only refreshes it; for a window id
  that left the host it docks the new icon.
* ARGB icons are redirected (XComposite) and drawn by the tray with their
  alpha channel when they change (XDamage). The tray advertises the
  panel's visual (`_NET_SYSTEM_TRAY_VISUAL`), so GTK and Qt make icons with
  a ParentRelative background, which shows the host's colour.
* The host starts in the theme's colour; a new colour repaints the host and
  asks every icon to redraw.
* When the panel dies, the X server puts the icons back on the root window
  unmapped (XFixes save-set, as GTK's own sockets do); GTK then docks a new
  plug in the restarted panel. An icon that something else unmaps while it
  should show is mapped again (three times in ten seconds at most).
* Only shown icons (`XEMBED_MAPPED`) take a slot; only changed geometry is
  sent to icons.
* StatusNotifierItems: every call is asynchronous; a menu is shown from the
  event loop with a copy of the item's names (an item can go while its menu
  is open); without a layout within 2 s the program is asked for its own
  menu (`ContextMenu`). Property reads are coalesced per item. Pixmaps with
  too little data are skipped; a picture sent only above 256 px is used.
  At most 256 items and 500 menu entries.
* `sde::dbus::Bus` never dispatches again while it dispatches: messages that
  arrive during a handler's nested loop are delivered when it returns.
* SDE WM uses only the local input method (Compose); programs it starts
  keep `XMODIFIERS` (`suite/patches/sde-wm/0006-no-input-method-server.patch`).

`SDE_TRAY_DEBUG=1` makes the panel log what the tray docks, refuses and
undocks on stderr (the session log).

### Tests

* `tests/test_tray.py` (CTest `sde-panel-tray`, Xvfb and a private session
  bus): with `sde-tray-client` (`tests/tray_client_fixture.cpp`) a GTK-like
  icon and an ARGB icon and their colours, also after a theme switch; a
  program that quits and comes back with the same window id; 300 icons
  created and destroyed at once; 200 dock requests for one window; an icon
  taken back and docked 30 times; a hung program; dock requests for the
  root window, the panel, a missing window and an application window;
  programs that die while docking. With `sde-sni-demo`: an item without
  `Activate` (its menu, bus traffic while it is open, Escape), a menu that
  is never answered, 3000 `NewIcon` signals, an ARGB pixmap. With IBus when
  it is installed: two restarts, its panel killed, the SDE panel killed and
  restarted; with `--wm` (CMake passes the SDE WM it built) the window
  manager must still map the panel and a new window. After every step the
  panel must answer `_NET_WM_PING` within 1.5 s and hold no grab.
* `tests/test_dbus_timeout.cpp` (CTest `sde-dbus-timeouts`): a reply handler
  with a blocking call and a nested loop while signals arrive.
* The QA dump lists `@tray-icon:<class>:<depth>:<shown|hidden>` with the
  icon's rectangle and `@tray-count:<n>`.

## Start button

The Start button shows the SDE logo and "SDE" unless changed in Control
Center > **Panel** > **Start button** (`sde-settings --page start-button`,
also *Change the Start Button...* in its right-click menu):

| Key (`[panel]`) | Values |
| --- | --- |
| `start_style` | `both` (default), `icon`, `text` |
| `start_label` | the text, at most 24 characters (default `SDE`; empty: the picture alone) |
| `start_icon` | a prepared picture (default: the SDE logo) |

**Choose a Picture...** opens the SDE file chooser for a PNG, JPEG or SVG
picture. The page asks for a square picture of at least 64 × 64 pixels
and says at which size the panel shows it (its icon size: 20, 24 or 28
pixels at 100 %, depending on the panel's size). The centred square is cut
out and stored at 128 × 128 next to sde.conf as
`~/.config/sde/start-button-<time>.png` (a new name each time; older ones
are removed); a picture under 64 × 64 or not square is used with a word
about it. The preview is drawn with the panel's own layout
(`src/common/start_button.*`) at the panel's size. **Reset to Default**
removes the three keys and the stored picture. The panel follows every
change at once; the logo and "SDE" keep the size of 0.2.0 Alpha 3.1 (within 2 pixels). The
QA dump has `@start-style:`, `@start-label:` and `@start-icon:` (`default`,
`none` or the picture's path).

Tests: `tests/test_start_button.cpp` (CTest `sde-start-button`: styles, the
text, the settings, the layout at 100 % and 200 %, the prepared picture);
`tools/panel-smoke.py --only start-menu` (each style on the panel);
`tools/showcase.py` scene `panel-start-button`.

## Task View button

With `[panel] show_task_view=true` (off by default; Control Center >
Panel > **Task View**, `sde-settings --page task-view`) a Task View button
follows the search. It runs `sde-taskview` (the workspaces and their
windows, Super+Tab) and is shown only while that program is installed; the
panel looks again when applications are installed. Its right-click menu
opens Task View, hides the button or opens the panel settings. The QA dump
names it `task-view`.
