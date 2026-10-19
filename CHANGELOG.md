# Alpha10 patch · 2026-10-19 — SDE Aero

- SDE Aero / SDE Aero Dark: new default GTK 3, GTK 4 and SDE WM theme with
  adjustable glass transparency, blur, rounded corners and animations.
- Fix duplicate tray icons; redesign the Action Center layout.
- SDE View is now Photo, with a live Effects and Filters side pane.
- Niny default plugins; Liria command bar; System Monitor graph cards.
- SDE-Desktop session name for LightDM and applications; SDE Aero greeter.
- Settings: Aero glass card, live XSETTINGS reload, immediate compositor off.
- CI publishes the alpha-latest pre-release and the latest screenshots.
- See docs/ALPHA10.md for details and recommendations.

# Alpha7 patch · 2026-10-18

- Add native GTK4 Calculator using the attributed Scientific Calculator engine.
- Redesign Start Menu with ordered Suite tiles, search and scrollable categories.
- Add categorized first-login Suite launchers without replacing user files.
- Build SDE WM (Openbox) and SDE Compositor (picom) from immutable source pins.
- Add GTK3/GTK4 settings, XSETTINGS and portal integration.
- Isolate `sudo liria` into root preferences and a private D-Bus session.
- Track independent application versions; Liria follows the desktop version.
- Keep the EDE-inspired component structure and all upstream notices.

# Changelog

## 0.1.0 Alpha 6 — native DebION suite

- Build Niny, Liria, Xanara, Taca, SDE View, System Monitor, Power Manager and
  Screen Lock from pinned source as native SDE executables in one `sde` package.
- Remove original MATE application package dependencies; retain shared libraries.
- Rename application namespaces, schemas, resources and Liria's extension API;
  preserve all upstream legal notices and publish matching complete sources.
- Keep SDE desktop ownership, GTK theme integration, MIME defaults and the
  full-edge square panel; target the complete suite at Debian testing.

## 0.1.0~alpha5 (0.1 Alpha) - 2026-10-17

- Default to a full-edge square panel and rectangular launcher/status controls.
- Add the Rust SDE Suite dispatcher and Niny, Liria, Xanara, Taca, SDE View,
  System Monitor and Power Manager integration using maintained MATE engines.
- Add SDE-specific MIME defaults, duplicate-menu suppression, GTK light/dark
  launch preferences, and SDE-only power/screen-lock autostarts.
- Add native SDE Update with a credited GPLv3+ Mint Update model, read-only APT
  previews and authenticated Debian PackageKit transactions.
- Add Settings suite/source/copyright and Power & Updates pages.
- Add `sde-desktop`, the complete DebION installation metapackage, and Debian
  testing CI alongside the existing four distribution builds.
- Record pinned upstream source references and a non-destructive source workspace
  tool; preserve upstream notices. MATE engines are packaged dependencies, and
  LXQt/Budgie/COSMIC remain source references in this alpha.
- Add update-model, launcher/MIME/autostart integration and real engine UI checks.

## 0.1.0~alpha4 (0.1 Alpha) - 2026-10-17

### Added
- Eduka-inspired full, floating and short panel styles, live previews and width control.
- Panel geometry implemented in safe Rust, with monitor bounds/strut tests.
- Action Center: clock card (digital/analog), greeting, six quick controls, optional
  colorful tiles, expandable Wi-Fi connections, sound/brightness cards and a fixed
  session footer. Content scrolls on small screens and at larger interface scales.
- Settings overview with category cards, grouped navigation and an Action Center page.
- `sde-panel --action-center` (the existing `--quick-settings` option remains an alias).

### Changed
- The panel clock now opens the Action Center; Super+C and the Calendar tile open
  the existing month calendar. The status area remains reachable on short panels.
- Floating panel corners use XShape and do not require a compositor.
- The Rust configuration core remains in use throughout the desktop.

## 0.1.0~alpha3 (0.1 Alpha) - 2026-10-17

### Added
- Dependency-free Rust workspace, built offline with Cargo from CMake and Debian
  packaging. No Rust compiler or Cargo is needed on the installed desktop.
- `sde-autostart`: native Rust XDG launcher with `--dry-run`, per-user overrides,
  Hidden/OnlyShowIn/NotShowIn/TryExec checks, locale-aware names, terminal support,
  and argument-vector execution without a shell.
- Rust unit and CLI tests, C++/Rust ownership tests, and Rust formatting/lint/MSRV
  checks in CI. `ctest` runs both the Rust and existing C++ tests.

### Changed
- All desktop components now use the Rust configuration core through a small C
  ABI and the existing C++ Config API. Settings paths and INI format are retained.
- Built-in defaults are embedded from `data/sde.conf`, avoiding a second defaults
  list. User-only saves use exclusive temporary files, mode 0600 and atomic rename.
- Invalid integer values, including overflow, fall back to the caller's default.
- The session delegates XDG autostart to Rust once, and `--check` checks the helper.

The graphical interface and session supervisor remain C++17 in this milestone.

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
