# The SDE core controller (sde-core)

0.2.0 Alpha 4. `sde-core` is the hub every SDE program talks to: one
service on the session bus, `org.selviana.Core`, that owns the settings,
knows the look of the desktop, follows the programs SDE started and speaks
for the session. Up to 0.2.0 Alpha 3.1 the programs talked to each other
through files and properties of the X root window: every program re-read
`sde.conf` whenever `_SDE_CONFIG_SERIAL` moved, none knew which keys
changed, and each one rewrote the whole file (the Control Center undid
changes the panel had made meanwhile).

## How it runs

- `sde-session` starts `sde-core` first, before the window manager, as a
  component of the session ("core controller" in `session.log`), and starts
  it again when it crashes (at most five times a minute). An `sde-core`
  that finds the name taken ends with status 0 and is not restarted;
  `sde-core --replace` takes the name over (for development).
- It runs in SDE sessions only: there is no D-Bus activation file, and
  clients never start it (`auto_start` is off for their calls).
- Programs keep working without it (outside SDE, or while it restarts):
  the client falls back to what SDE did before (see *Fallbacks*).
- The session tells it what happens to the session's programs
  (`session/core_link.cpp`): components started, stopped, crashed or given
  up, applications that crashed, and its phase. When `sde-core` starts
  again it gets the components that run.

## Why Rust behind a C++ front end

The controller's decisions are in the safe Rust core
(`rust/sde-core/src/controller.rs`, `schema.rs`, `migrate.rs`), unit tested
without a display or a bus and reached through the existing private C ABI
(`rust/sde-config-ffi/src/controller.rs`, `src/common/rust_core.h`):

- the schema of `sde.conf` and the checks of every write;
- all-or-nothing writes merged with the file as it is on disk;
- which keys changed since the last announcement (deferred for a theme
  switch until the session shows it);
- the single instance of each application, the problems the session and
  the watchdog report, the components, the session phase and workspaces;
- the colors of the wallpaper.

The front end (`src/core/main.cpp`) is C++ on `src/common/dbus.*` and
FLTK's X connection: it only speaks D-Bus and X11. The Rust workspace has no
registry dependencies and keeps none: a D-Bus implementation in Rust would
mean writing the wire protocol, authentication and marshalling from
scratch, or raw unsafe bindings to libdbus, next to the tested `dbus.cpp`
every SDE program already uses. The look's values come from the C++ theme
code (`Theme::from_config()`), the one place that computes them for every
SDE program, so the controller cannot disagree with them.

## D-Bus API

Bus name `org.selviana.Core`, object `/org/selviana/Core`. The service
introspects itself (`gdbus introspect --session --dest org.selviana.Core
--object-path /org/selviana/Core`). API version 1.

Errors: `org.selviana.Core.Error.InvalidKey`, `InvalidValue` (the
message says why, e.g. `[panel] position: "left" is not one of bottom,
top`), `UnknownKey`, `NotFound`, `Failed`; `org.freedesktop.DBus.Error.
AccessDenied` for programs in a Flatpak or Snap sandbox, which may read
but change nothing; `InvalidArgs` and `UnknownMethod`.

### org.selviana.Core

| Member | Signature | |
| --- | --- | --- |
| `Version()` | → `(s version, u api)` | SDE's version (`VERSION`), API 1 |
| `Capabilities()` | → `as` | `settings`, `settings.schema`, `settings.appearance-switch`, `look`, `look.wallpaper-colors`, `apps.instances`, `apps.launch`, `apps.problems`, `apps.running`, `session.lock`, `session.logout`, `session.workspaces`, `session.phase` |

### org.selviana.Core.Settings

| Member | Signature | |
| --- | --- | --- |
| `Get(s section, s key)` | → `v` | The value programs read (the user's over the defaults): `b` for booleans, `i` for integers, `s` otherwise. `UnknownKey` when no layer has it. |
| `GetAll()` | → `a{sa{sv}}` | Every value, by section. |
| `Set(s section, s key, v value)` | | `b`, any integer, `d` or `s`; checked against the schema and written in the schema's form (`Yes` → `true`, `Top` → `top`, `#abc` → `#AABBCC`). |
| `Reset(s section, s key)` | | Back to the default (removed from the user's file). |
| `SetMany(a(ssv) values, a(ss) resets, a{sv} options)` | → `a(ss) changed` | All or nothing. Option `appearance` (`b`): a theme switch, announced once the session shows it (below). |
| `Schema()` | → `a(ssssbss)` | section, key, type, values, has default, default, Control Center page. |
| signal `Changed(a(ss) keys)` | | The keys whose value changed, whoever changed them. |

Writes are merged with the file as it is on disk, so a program never
undoes what another one wrote. A program that still writes `sde.conf` itself
and bumps `_SDE_CONFIG_SERIAL` is read again and announced with `Changed`;
the controller's own writes bump the serial too, for the programs that do
not use it yet and for the session, which tells GTK (XSETTINGS), the window
manager and the compositor what changed.

**Theme switches.** With `appearance`, `sde-core` writes the file and asks
the session for its calm switch (`_SDE_APPEARANCE_APPLY`, docs/THEMES.md
"Switching themes"); the session shows a picture of the screen, publishes
the look behind it and announces it, and only then `Changed` and
`LookChanged` go out, so the programs restyle behind the picture. Without a
session the look is applied and announced at once; after 3 seconds without
an announcement it is announced anyway.

### The schema

`data/sde-schema.tsv`, read at build time. Every key of `data/sde.conf`
belongs to it, also the commented `#key=value` ones, with a type inferred
from its default (`true`/`false` boolean, whole numbers integer, else text);
the file makes types stricter and lists keys `sde.conf` does not:

```
# section  key  type  values  page
panel	position	enum	bottom|top	panel
aero	transparency	int	0..100	appearance
appearance	scale	int-or-auto	50..400	display
night_light	from	time
```

Types: `bool`, `int` (`min..max`), `int-or-auto`, `enum` (`a|b|c`, any case,
written in lower case), `color` (`#RGB` or `#RRGGBB`), `time` (`HH:MM`),
`text`. No value holds a line break or starts or ends with a space. A key in
neither file is accepted as text: a new page may add keys before the schema
lists them (add them, with their page). The page column is the one
Control Center page that edits the key (docs/CONTROL-CENTER.md).

Old duplicate keys are merged whenever `sde.conf` is loaded, by every program
(`rust/sde-core/src/migrate.rs`): `[clock] style`, `seconds` and `blink` become
`[action_center] clock_style` and `[panel] clock_seconds` and `clock_blink`
(the newer key wins when both are set), and `[panel] clock_24h` follows
`[clock] format` where they disagreed. `sde-core` writes the merged file back
at start.

### org.selviana.Core.Look

| Member | Signature | |
| --- | --- | --- |
| `Get()` | → `a{sv}` | The look in use. |
| signal `LookChanged(a{sv})` | | When any value of it changed. |

Keys: `family` (graphite, fluent, glass), `mode` (light, dark, auto),
`dark` (`b`, now), `accent-name`, `accent` (`#RRGGBB` in use for the mode),
`gtk-theme`, `icon-theme`, `translucent`, `transparency` (`i`), `blur`,
`blur-strength`, `rounded-corners`, `animations`, `low-resource`, `lite`
(the compositor's lite profile), `liquid-glass`, `scale` (`d`), `font`,
`font-size` (points), `wallpaper` (the file shown, also for `auto`),
`wallpaper-mode`, `wallpaper-colors` (`as`, up to five `#RRGGBB`, most used
first), `wallpaper-dark` (`b`). The wallpaper's colors are read on a thread
from a small copy of the picture: `LookChanged` comes again when they are
known.

### org.selviana.Core.Apps

| Member | Signature | |
| --- | --- | --- |
| `Running()` | → `a(ussss)` | pid, kind (`component`, `application`), name, id (desktop file), state (`running`, `stopped`, `given-up`, `not-responding`, `not-opened`). Components from the session, applications from the launch records (docs/FORCE-CLOSE.md). |
| `Problems()` | → `a(xussss)` | time, pid, kind (`crashed`, `not-responding`, `not-opened`, `component-crashed`, `component-given-up`), name, id, detail (signal, window, reason). The last 64. |
| signal `Problem(x, u, s, s, s, s)` | | The same, as it happens. |
| `Launch(s desktop_id, as uris)` | → `b` | Starts an application like the Start menu does (recorded for the watchdog). |
| `Register(s app_id)` | → `b primary` | The caller becomes the instance of `app_id`, unless another one is. |
| `Unregister(s app_id)` | | |
| `Activate(s app_id, as args)` | → `b delivered` | Hands `args` to the instance, which exports `org.selviana.Core.Instance.Activate(s app_id, as args)` at `/org/selviana/Core/Instance`. An instance that does not answer in 3 s, or whose connection closed, is dropped: the new start becomes the instance. |
| `Report(s kind, u pid, s name, s id, s detail)` | | For the session and `sde-watchdog`: `crashed`, `not-responding`, `responding`, `not-opened`, `opened`, `ended`, `component-started`, `component-stopped`, `component-crashed`, `component-given-up`. A hang reported twice is one problem. |

### org.selviana.Core.Session

| Member | Signature | |
| --- | --- | --- |
| `Lock()` | | `sde-lock --lock`. |
| `Logout(b confirm)` | | `confirm`: the Leave dialog (`sde-logout`); otherwise ends the session like `sde-session --logout`. |
| `Info()` | → `a{sv}` | `phase`, `display`, `version`, `session-pid`. |
| `Workspaces()` | → `(u count, as names, u current)` | The window manager's (`_NET_NUMBER_OF_DESKTOPS`, `_NET_DESKTOP_NAMES`, `_NET_CURRENT_DESKTOP`); read-only here. |
| `SetPhase(s phase)` | | The session: `starting`, `running`, `ending`. |
| signals `PhaseChanged(s)`, `WorkspacesChanged(u, as, u)` | | |

### Examples

```sh
gdbus call --session --dest org.selviana.Core --object-path /org/selviana/Core \
  --method org.selviana.Core.Settings.Set panel position "<'top'>"
gdbus call --session --dest org.selviana.Core --object-path /org/selviana/Core \
  --method org.selviana.Core.Look.Get
gdbus monitor --session --dest org.selviana.Core
```

## The client (src/common/core_client.h)

What SDE programs use. Everything runs in the FLTK event loop, never
blocks except where noted, and never connects when no session bus is known
(`DBUS_SESSION_BUS_ADDRESS` or `$XDG_RUNTIME_DIR/bus`), so no bus is started
for a program on its own.

```cpp
#include "core_client.h"
using namespace sde;

core::set("panel", "autohide", "true");               // or core::reset()
core::write({{"appearance", "theme", "dark", false}},   // a calm theme switch
            core::Mode::APPEARANCE);
core::write(core::diff(saved, cfg));                    // what changed since |saved|
core::watch_settings([](const std::vector<core::Key> &keys) { ... });
core::watch_look([] { apply_theme(); });
if (!core::single_instance("sde-settings", args, on_activated)) return 0;
core::report("not-responding", pid, name, desktop_id, window);
```

### Fallbacks (0.2.0 Alpha 4, for one release)

Without `sde-core`, or when it does not answer, `write()` saves `sde.conf`
directly: merged with the file on disk, still checked by the same schema
(`sde_schema_check()`), announced with `_SDE_CONFIG_SERIAL` (and the
appearance files when no session runs; `apply_appearance()` for a theme
switch). `watch_settings()` and `watch_look()` then follow the serial of
the root window (an empty key list: something changed). `single_instance()`
makes every start its own instance. The programs of 0.2.0 Alpha 3 that do
not use the client keep working through the serial, which `sde-core` keeps
bumping; that path goes away after this release.

### Who uses it

| Program | |
| --- | --- |
| sde-session | starts and supervises sde-core; reports components, crashes, phase |
| sde-panel | writes settings (menus, pins, Do Not Disturb, dark mode); follows `Changed` |
| sde-settings (Control Center) | writes only what its pages changed; follows `Changed`; one window per session (another `--page` start opens the page there) |
| sde-watchdog | reports hangs and programs that never open |
| Xanara, Taca, Photo, Niny, Calculator, Merkadu | follow `LookChanged` (Merkadu followed the look only at start) |
| Gallery | follows `Changed` (its settings are in sde.conf) |
| Liria | a GTK program: follows the theme through XSETTINGS, which the session updates |

## Tests

- `rust/sde-core`: schema (every default valid, normalization, refusals),
  merged and deferred writes, migration, instances, problems, components,
  session state, palette (CTest `sde-rust-tests`; also with Rust 1.59).
- `tests/test_core.py` (CTest `sde-core-service`): the service on a private
  bus and Xvfb with a temporary home, through `gdbus` and
  `tests/core_probe.cpp` (`sde-core-probe`, not installed): every interface,
  a program writing `sde.conf` itself, a theme switch, the wallpaper's
  colors, instances handing over arguments and registering again after a
  restart, and the client without `sde-core`.

## Not yet

- The session, the desktop, the lock screen and the notification service
  still read `sde.conf` themselves (they work through the serial); the
  session must work while `sde-core` restarts.
- Niny keeps its own single instance (a socket); the Control Center uses
  `sde-core`'s.
- Liria (C, GTK) does not use the client; a GDBus reader for its start-up
  theme could use `Look.Get`.
- No D-Bus activation: programs outside an SDE session use the fallbacks.
