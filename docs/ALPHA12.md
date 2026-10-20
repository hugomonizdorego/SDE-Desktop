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

The provider check uses SNI `Id` or XEmbed `WM_CLASS`, never arbitrary window
names or icon text. Unknown application providers are retained.

| Function | Native indicator availability | Provider behavior |
| --- | --- | --- |
| Battery | Kernel battery present; indicator enabled | Hide known power-manager tray icons |
| Bluetooth | BlueZ adapter present; indicator enabled | Hide known Bluetooth applet icons |
| Network | NetworkManager available; indicator enabled | Hide known network applet icons |
| Sound | Pulse/PipeWire interface available; indicator enabled | Hide known volume applet icons |
| Input method | IBus/Fcitx provider present | Keep provider; hide generic keyboard glyph |

The same availability rules apply in Action Center. The redundant Overview
shortcut inside its status row is also removed. Existing saved `show_controls`
values no longer create an extra launcher. `panel.show_tray` controls application
icons on the panel; Action Center retains its own tray preference.

## Crash correction

Alpha 11 reproducibly segfaulted when a StatusNotifierItem provider exited while
its menu was open and a menu item was subsequently selected. The menu handler
retained references to a deleted provider. The handler now owns the destination
strings for the entire operation. Action Center also defers rebuilding its
widgets until nested menus and modal dialogs have finished.

A second Alpha 11 baseline test reproduced a double-free in the D-Bus timeout
handler when pending calls lost their provider. D-Bus watch and timeout
registrations now track removal explicitly, so dispatch does not dereference a
registration freed by an earlier callback. The same 128-call test passes with
the patch. The session rescue dialog distinguishes a signal crash from missing startup dependencies.

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

## Baseline comparison and memory checks — 19 October 2026

Production code commit: `928d2a1d1200823d62d50203299b090b1842aa08`.
Baseline: Alpha 11 `7bd98b72d36e27441d0ea02ad08606cd5b7f85af`.

The same new QA fixtures were used against both builds. Alpha 11 failed in
`SniHost::show_dbus_menu` after provider removal (SEGV) and in `on_timeout`
after pending-call provider disconnection (double-free). Alpha 12 passes both.
The baseline production sources were not patched for the comparison.

The [memory safety CI job](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39252605381/job/118059851326)
passed all nine core CTest checks and the extended panel GUI checks at both
sizes under ASan/UBSan. Rust formatting, Clippy, stable tests and the minimum
Rust 1.59 check passed too. Sanitizer reports and screenshots are available in
[the QA artifact](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39252605381/artifacts/12186228589).
Its downloaded ZIP digest was verified as
`a8d20e0e7ee974c17088e8726c25e0f17cd8a19699ca43388848a9373a42859b`.


## Verified Debian packages

Both the [push build](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39252605381)
and [pull request build](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39252612943)
passed the complete native compilation and all 12 CTest checks, actual installation
of the three desktop packages and every DebION complementary dependency, version
and copyright checks, MIME/menu/terminal defaults, Gallery/About checks, and GUI
checks at both sizes. The separate sanitizer and Rust jobs passed as well.

[Download Debian amd64 packages, installer and matching complete sources](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39252605381/artifacts/12187441719).
The downloaded ZIP SHA256 is
`7dd4e72386a8431c8b99c342af75d8edc7e35c82ed800036526e794da11e0995`.
All six entries in `SHA256SUMS.sha256` were verified, all four package versions
are `0.1.0~alpha12`, and archived production/test sources match the tested commit.
The installer selects the three desktop packages; debug symbols are optional.
The added user-session/audio dependencies are present in the metapackage, and
original MATE application packages are not dependencies of the native core.

[Current screenshots](screenshots/alpha12/README.md) are from the same installed
packages. The [complete UI artifact](https://github.com/hugomonizdorego/SDE-Desktop/actions/runs/39252605381/artifacts/12187681136)
was also downloaded and verified with ZIP SHA256
`739db87c7664ac2de6746c1e19e905a5c408efa17c5a0f57ba09c4b4df78f218`.
Documentation and screenshot updates after `928d2a1` do not change the tested
executable or packaging sources. These packages were installed in CI; installing
on a user's DebION system requires running the installer there and logging in again.
