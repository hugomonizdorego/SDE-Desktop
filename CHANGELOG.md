# Changelog

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
