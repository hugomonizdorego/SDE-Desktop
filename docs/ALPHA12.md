# SDE 0.1 Alpha 12 — panel stability

This patch addresses the panel crash and duplicate indicators reported during
DebION GNU/Linux testing. SDE and Liria use `0.1.0~alpha12`; the other native
application versions are unchanged from Alpha 11.

## Panel and Action Center

- Click the clock to open Action Center. The separate sliders button, quick
  controls button and permanent tray arrow are removed. `Super+A` still works.
- Keep one native indicator per system function. Known power, Bluetooth,
  network and audio tray providers are hidden when SDE already represents that
  function; their processes and services continue running. Disabling the native
  indicator restores the provider icon. Ordinary application icons stay visible.
- Prefer an actual IBus/Fcitx input-method indicator over the generic keyboard
  glyph. The media indicator appears only when a player is available.
- Constrain XEmbed clients to the tray's icon size, honor their mapped state,
  publish the tray visual and clear stale backgrounds when moving the host.
  Application icons move into Action Center when configured, without losing
  their original windows or menus.

## Crash correction

Alpha 11 reproducibly segfaulted when a StatusNotifierItem provider exited while
its menu was open and a menu item was subsequently selected. The menu handler
retained references to a deleted provider. The handler now owns the destination
strings for the entire operation. Action Center also defers rebuilding its
widgets until nested menus and modal dialogs have finished.

D-Bus watch and timeout registrations now track removal explicitly, so dispatch
does not dereference a registration freed by an earlier callback. The session
rescue dialog distinguishes a signal crash from missing startup dependencies.

## Dependencies and installation

The full desktop now explicitly depends on `dbus-user-session`, `libpam-systemd`
and `rtkit` for the user bus, login session and audio scheduling. Build tests
explicitly require `dbus-daemon`. These packages complement the existing
NetworkManager, BlueZ/Blueman and PipeWire/WirePlumber profile; an extra panel
applet is not required. See [the complete profile](DEBION.md).

Extract the matching alpha artifact and install through APT:

```sh
sha256sum -c SHA256SUMS.sha256
sudo sh ./install-debion.sh .
```

This is still an alpha for DebION / Debian testing. The reported crash is
reproduced by a controlled applet disconnect test; this does not establish that
every possible cause of a panel crash or every physical hardware combination
has been covered. Keep `~/.local/state/sde/session.log` for any further report.

## Regression coverage

Tests exercise real libdbus connections, SNI aliases and disappearing providers,
XEmbed resize/map requests, native/provider deduplication, icon transfer and
menu use while a provider exits. CI also runs the core panel with AddressSanitizer
and UndefinedBehaviorSanitizer, and the full Debian native suite and GUI checks
at 1366×768 / 100% and 1024×600 / 150%. Test applets simulate hardware services;
they are not installed in the desktop packages.

Build results and current screenshots are recorded after verification.
