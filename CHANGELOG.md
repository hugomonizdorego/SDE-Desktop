# Changelog

## 0.1.0~alpha2 (0.1 Alpha) - 2026-10-16

### Fixed
- Black screen when a component cannot start (for example when the package was
  installed with `dpkg -i` without its libraries): `sde-session` now logs to
  `~/.local/state/sde/session.log`, paints the background, shows a rescue dialog
  with a terminal, and `sde-session --check` reports missing programs and libraries.
- Panel windows lost their redraw events under Openbox; cairo drawing inside
  scrolled areas ignored clipping.

### Added
- Quick settings in the panel: Wi-Fi and Ethernet (NetworkManager), volume, mute and
  output device (PulseAudio/PipeWire), brightness (systemd-logind), battery with time
  remaining, Do Not Disturb.
- `sde-notifyd`: desktop notifications (org.freedesktop.Notifications 1.2) with
  action buttons, images and urgency; Do Not Disturb keeps only critical ones.
- StatusNotifierItem/AppIndicator tray with dbusmenu menus, next to the XEmbed tray.
- Calendar on the clock; `sde-panel --quick-settings` (Super+A) and `--calendar` (Super+C).
- Pin and unpin applications by right-clicking them in the menu or taskbar; drag
  pinned icons to reorder them; manage them in Settings > Panel.
- Panel at the bottom or top, and auto-hide.
- Desktop right-click menu combining Windows and Linux habits: View (icon size),
  Sort by, Refresh (F5), New folder/document, Open Terminal Here, Open in File
  Manager, Display Settings, Change Wallpaper, Desktop Settings; icon menu with
  Open, Rename (F2), Copy Location, Move to Trash (Delete).
- Settings: interface size 100-200 % (or automatic from the DPI), text size,
  Displays (resolution, main display, arrangement with automatic revert),
  Keyboard & Mouse (layouts, key repeat, pointer, touchpad), Notifications pages;
  `sde-settings --apply` restores display and input settings at login.
- HiDPI: every program, Openbox title bars included, follows the interface size.
- Accessibility: keyboard focus rings on all controls, keyboard navigation in the
  calendar, desktop and settings, WCAG contrast unit tests (the green accent was
  darkened to pass).
- Translations: GNU gettext with US English as the source language (`po/sde.pot`).
- GitHub release workflow: one package per distribution, `SHA256SUMS` and notes
  from this changelog.

### Changed
- Much lower memory use: icon lookups no longer keep directory listings in memory
  (panel private memory 41 MB to 4 MB).

## 0.1.0~alpha1 (0.1 Alpha) - 2026-10-16

First build of the Selviana Desktop Environment, written from scratch.

- New C++17 code base (FLTK 1.3, cairo, librsvg, Xlib) built with CMake.
- `sde-session`: session manager with crash recovery, polkit agent and XDG autostart.
- `sde-panel`: application menu with search and categories, pinned apps, taskbar,
  system tray (XEmbed), battery and clock.
- `sde-desktop`: wallpaper and desktop icons, live updates of `~/Desktop`.
- `sde-settings`: light/dark theme, accent color, icon theme, wallpaper, panel,
  desktop and About pages.
- `sde-run` and `sde-logout` dialogs.
- Openbox configuration with SDE keyboard shortcuts; `SDE` and `SDE-Dark` window themes.
- Selviana light and dark wallpapers and the SDE logo.
- Debian packaging for Debian 12/13 and Ubuntu 22.04/24.04; CI builds all four.
- `tools/install-deps.sh`, `tools/build-deb.sh` and `tools/screenshot.sh`.
