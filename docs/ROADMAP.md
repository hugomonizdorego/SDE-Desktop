# Roadmap

SDE is built from the ground up and grows in small, usable steps.

## 0.1 Alpha (this release)

- [x] Session manager, panel, desktop, settings, run and log out programs
- [x] Light and dark themes with accent colors
- [x] Openbox integration and SDE window themes
- [x] Earlier shell packages for Debian 12/13 and Ubuntu 22.04/24.04 (alpha2)
- [x] Rescue dialog, session log and `sde-session --check` (alpha2)
- [x] Notification daemon with Do Not Disturb (alpha2)
- [x] Quick settings: network, volume, output device, brightness, battery (alpha2)
- [x] Display settings (XRandR) and keyboard, mouse and touchpad settings (alpha2)
- [x] StatusNotifierItem/AppIndicator tray (alpha2)
- [x] Interface size 100-200 %, keyboard focus, contrast checks (alpha2)
- [x] gettext translation support (alpha2)
- [x] Per-distribution GitHub releases with checksums (alpha2)
- [x] Shared Rust configuration core and native XDG autostart launcher (alpha3)
- [x] Offline Cargo/CMake build and cross-language configuration tests (alpha3)

- [x] Native source-built DebION application suite in one package (alpha6)
- [x] Complete desktop metapackage targeting Debian testing (alpha6)
- [x] Panel focus: network LED and switches, removable drives, touchpad indicator (alpha50)
- [x] Native Calculator, Photo drawing, Gallery albums (alpha50)
- [x] Welcome, Driver Manager and System Reports from Linux Mint concepts (alpha50)
- [x] Proxy-aware network diagnosis for Debian standard installs (alpha50)
- [ ] Validate real DebION login, locking and hardware power policies

## Rust migration

- [x] Safe Rust core with an isolated C ABI for the existing interface
- [ ] Share Rust desktop-entry parsing with the panel and desktop
- [ ] Port session supervision, preserving rescue and logout behavior
- [ ] Port notification service and D-Bus integration
- [ ] Choose a GUI integration strategy and port individual applications
- [ ] Measure startup time, memory and accessibility before replacing the panel

See [RUST.md](RUST.md) for the current boundary and validation requirements.

## 0.2 Alpha

- [ ] First translations (Indonesian, Portuguese)
- [ ] Notification history and grouping
- [ ] Bluetooth tile in quick settings
- [ ] Apply input settings to devices plugged in during the session
- [ ] Paste files on the desktop (clipboard file lists)

## 0.3 Beta

- [x] Liria native file-manager fork (alpha6)
- [x] SDE Power and SDE Screen Lock, written from scratch (alpha16)
- [ ] Hardware validation of idle, lid, suspend and locking
- [ ] Panel applets API and panel position options
- [x] Show Desktop strip (alpha50)
- [ ] Workspace switcher and window previews

## 0.5 Beta

- [ ] Own window manager or deeper Openbox replacement
- [ ] Accessibility review (keyboard navigation, contrast, scaling)
- [ ] HiDPI scaling

## 1.0 Final

- [ ] Stable configuration format and upgrade path
- [ ] Packages in a public APT repository
- [ ] Complete user documentation
- [ ] Wayland research and plan
