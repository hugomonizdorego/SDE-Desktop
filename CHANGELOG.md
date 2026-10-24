# 0.2 Alpha 1 · 2026-10-24 — official logo, Control Center, faster Start Menu

- First release of the 0.2 series (0.2.0~alpha1); roadmap to 1.0 in
  docs/ROADMAP.md.
- The official SDE logo in About windows, Control Center → About, Welcome
  and the login session entry; a new icon for every SDE application.
- About windows with About, Credits, License (full text, open-source
  licenses) and Legal (copyright, trademark notices).
- SDE Settings is now the Control Center: overview with search and
  categories; new Users & Avatar, Start Menu and Date & Time pages.
- User pictures from the Start Menu or the Control Center; samples in
  /usr/share/Avatar.
- Start Menu opens in about 4 ms, search answers in about 2 ms, smooth
  scrolling, Super no longer lost; App info and Open file location.
- Liria: the rubber-band selection no longer hides the files.
- Release audit: every High and Medium finding fixed (see
  docs/AUDIT-0.2.0-alpha1.md); desktop and panel always visible at login.
- SDE and DebION GNU/Linux kept apart; screenshots of everything with every
  build. See docs/0.2-ALPHA1.md.

# Alpha52 patch · 2026-10-23 — drag & drop, Liria Ribbon, smarter panel

- Drag and drop on the desktop and in Liria.
- Liria: optional Ribbon, Windows 11-like Home and sidebar, path bar fixes.
- Settings: Default Applications, Clock & Weather.
- Panel: right-click menu per icon, search modes, notifications button.
- Action Center without scrolling; LED, analog and blinking clocks; weather
  units; day/night sky.
- Internet works right after login (install and boot repair). See
  docs/ALPHA52.md.

# Alpha51 patch · 2026-10-22 — DNS fix, lock screen, Software Updater, calmer look

- debion-network-check repairs DNS and verifies it; public DNS fallback.
- Lock screen always gets the keyboard and pointer; rescue prompt.
- Time zone sync in the clock; Software Updater with Critical, Security,
  Main and Flatpak groups.
- Taskbar window previews, compact popups and a Mint-like battery flyout.
- Calm palettes, soft corners, Mint-like window buttons.
- Minimal Welcome (not on live boot) and Driver Manager; Log Out hidden from
  the Start Menu. See docs/ALPHA51.md.

# Alpha50 patch · 2026-10-22 — panel focus, network fix, Calculator, drawing, themes

- Panel: network LED with Wi-Fi/LAN switches, Eject for removable drives,
  one keyboard indicator, touchpad indicator, no media button, fixed tray
  tooltips and popups after screenshots; Show Desktop strip.
- Action Center: animated weather behind the clock, Night Light and
  brightness settings, compact media card. Start Menu: Gallery and one
  Power off.
- Fix for "connected but no internet" on Debian standard installs (proxy,
  DNS, clock, interfaces) with debion-network-check and Settings cards.
- New native Calculator; drawing in Photo; albums and batch actions in
  Gallery; Liria Quick Access pins; Niny About crash and slow desktop
  Refresh fixed.
- Big window buttons on the right, adjustable transparency, icon themes
  everywhere, build history and PayPal donation in Settings > About.
- SDE Welcome, Driver Manager and System Reports. See docs/ALPHA50.md.

# Alpha16 patch · 2026-10-21 — themes, themed UI, redesigned apps, no MATE

- New theme families SDE Graphite (default), SDE Fluent and SDE Glass with
  accents, applied everywhere; themed native menus and dialogs.
- Taca, Xanara, Niny and the Start Menu redesigned; search in the panel;
  brightness in the Action Center.
- Liria re-forked without MATE branding; native SDE Power and Screen Lock.
- DebION Dev-Team named as developer in every app. See docs/ALPHA16.md.

# Alpha15 patch · 2026-10-21 — native apps, Menu, Panel, Settings, Liria, performance

- Niny, Xanara, Taca, Photo and SDE System Monitor are new native apps by
  the DebION Dev-Team; the MATE forks they replace are retired.
- Gallery menus, devices and Photo integration; Start Menu fixes; Settings
  wallpaper gallery, Computer icon and GPU details; Liria USB copy and
  clipboard fixes; partial repaints and a lite compositor profile.
- See docs/ALPHA15.md.

# Alpha14 patch · 2026-10-20 — Gallery, flyouts, new Menu, Action Center, Update, Liria

- Gallery is back as a new app (sde-gallery) by the DebION Dev-Team.
- Tray icons open their own flyouts; the Action Center has its own job,
  with the calendar merged in and compact/expanded modes.
- New Start Menu: search shows focus and suggestions at once; Favorites
  tiles, All Programs A–Z, category pages.
- SDE Update redesigned and extended (history, sources, maintenance,
  settings, background checks).
- Liria: Home overview and a new look in every SDE theme.
- SDE-FLTK fork in-tree; sde-xsettingsd and sde-polkit-agent written from
  scratch. See docs/ALPHA14.md.

# Alpha13 patch · 2026-10-20 — panel crash fix, new tray, own ecosystem

- Fixed: the panel crashed a few seconds after login ("The panel keeps
  stopping") on systems where a D-Bus service does not answer (Bluetooth
  without an adapter, e.g. VirtualBox). Safe-mode panel and crash
  backtraces in session.log as safety nets.
- New tray icons that show the real state; hardware icons (USB drive,
  Bluetooth, battery, media) appear and disappear with the hardware.
- Action Center button in the panel corner; the clock opens the calendar.
- SDE toolkit (FLTK), sde-xsettingsd and sde-polkit-agent built from source.
- See docs/ALPHA13.md.

# Alpha12 patch · 2026-10-20 — black screen fix, DebION desktop-base

- Fixed: black screen after login (VirtualBox and other virtual machines).
  The compositor uses XRender there, a compositor that draws only black is
  replaced automatically, and Alt+Shift+F12 turns compositing off or on.
- Fixed: a second compositor (system picom autostart) no longer starts.
- New package debion-desktop-base: DebION artwork for desktop-base
  (wallpaper, lock and login screens, GRUB, logos, Plymouth splash).
- See docs/ALPHA12.md.

# Alpha11 patch · 2026-10-20 — themes, Start Menu, Eduka features

- SDE Light, SDE Dark, SDE Aero and new SDE Liquid Glass themes for GTK 3/4
  and SDE WM, all installed in /usr/share/themes; theme cards in Settings.
- Start Menu search fixed (input method and focus), finds Settings and
  session actions; right-click Arrange menu; grid view; animations.
- From Eduka-Desktop: Effects page (animations, compositor mode, report,
  restart), Action Center drives (show files, eject) and power modes.
- Liria SDE Tools extension; cleaner Liria, Niny, Xanara and Taca styling.
- See docs/ALPHA11.md.

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
