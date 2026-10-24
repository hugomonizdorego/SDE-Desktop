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
**Show the time remaining in the label**). The panel lays itself out again
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
  LED clocks flashes once a second. The analog face has no colon; the
  option is greyed out there.

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
