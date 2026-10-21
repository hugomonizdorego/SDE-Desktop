# SDE 0.1 Alpha 16 — new themes, themed UI everywhere, redesigned apps, no MATE

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still
alpha: test it in a virtual machine or a spare installation first.

## Themes, rebuilt from scratch

Three theme families, each in Light and Dark with nine accent colours
(blue, teal, green, yellow, orange, red, pink, purple, graphite):

| Family | Style | Based on |
| --- | --- | --- |
| **SDE Graphite** (default) | macOS / Human Interface Guidelines: soft depth, translucency, round traffic-light window buttons on the left | [Graphite-gtk-theme](https://github.com/vinceliuice/Graphite-gtk-theme) (GPL-3.0) |
| **SDE Fluent** | Windows Fluent Design: Mica-tinted backgrounds, Acrylic-like translucent menus, rounded layers, Windows-style caption buttons on the right | [Flat Remix GTK](https://github.com/daniruiz/flat-remix-gtk) and the colours of [Flat Remix GNOME](https://github.com/daniruiz/flat-remix-gnome) (GPL-3.0 / CC BY-SA 4.0) |
| **SDE Glass** | Frosted glass on top of Graphite (replaces Aero and Liquid Glass) | SDE |

- One choice in **Settings → Appearance** (family cards with previews,
  Light/Dark/Auto, accent swatches) now reaches everything: GTK 3 and GTK 4
  apps, libadwaita apps, Liria, the window frames and menus of the window
  manager, the panel, Start Menu, Action Center, notifications and every
  native SDE app. Changes apply live.
- Auto switches to Dark at 19:00 and back to Light at 07:00.
- Saved Alpha 15 choices are migrated (SDE → Graphite, Aero → Fluent,
  Liquid Glass → Glass, old accent colours → the nearest named accent).
- Upstream themes are pinned in `suite/themes.lock.json` and built at
  package build time; their licenses are installed in
  `/usr/share/doc/sde/themes`.

## Every menu and dialog follows the theme

Right-click menus, menu bars, drop-down lists, tooltips, scroll bars,
message boxes and the file chooser in all native SDE programs now use the
theme: rounded popups with a shadow, accent highlight, shortcuts on the
right, and a row of Cut / Copy / Paste / Select all / Delete icons at the
top of edit menus. Text fields get an SDE right-click menu too.

## DebION Dev-Team as developer

Every SDE program shows **Developer: DebION Dev-Team** in its About window,
and every desktop file and AppStream entry (`/usr/share/metainfo`) names
DebION Dev-Team as developer.

## Redesigned applications

- **Taca** — classic archiver layout: menu bar (File, Commands, Tools,
  Favorites, Options, Help), large toolbar (Add, Extract To, Test, View,
  Delete, Find, Wizard, Info, Repair), address bar, details list that
  browses folders and archives, status bar with totals. New Find, Wizard,
  Repair (writes a "(repaired)" copy) and Convert. All icons are SDE's own.
- **Xanara** — PDF editor layout: document tabs with a Home tab, mode tabs
  Annotate / Edit / Page / Form / Fill / Convert / Protect / Tools, a tool
  bar per mode, a left rail (search, thumbnails, outline, bookmarks,
  annotations, signatures) and a properties panel. Annotations, stamps,
  signatures, fill & sign, export (PNG, JPEG, text, HTML, Markdown),
  images to PDF and compare work out of the box; page operations, form
  fields, watermark, header/footer, password, redaction and merge use
  `qpdf`; OCR uses `tesseract-ocr`; compress prefers `ghostscript`.
- **Niny** — a simple Notepad-style editor: tabs on top, File/Edit/View,
  Markdown helpers (headings, lists, bold, italic, link, formatted view),
  status bar (Ln/Col, characters, type, zoom, line ending, encoding), a
  context menu with Cut/Copy/Paste/Select all/Delete icons, Search the
  Web, text tools, spelling (with `hunspell`) and Unicode tools, and a
  Settings page inside the window.
- **Start Menu and panel** — see the section below.

## MATE removed

- **Liria** was forked again from Caja 1.28.0 and rebranded completely:
  no MATE or Caja name, logo, link or icon is visible any more
  (`tools/liria-branding-check.py` checks this). Its window has one compact
  header with 16 px icons, a main menu, grouped right-click menus with
  "Send To" removable drives, a tidy sidebar and 48 px icons. The GPL
  requires the original authors' copyright notices to stay: they remain
  in the source files, in the License part of the About window and in
  `/usr/share/doc/sde/upstream/caja/`.
- **SDE Power** (`sde-power`) and **SDE Screen Lock** (`sde-lock`), written
  from scratch, replace the MATE power manager and screensaver: battery
  warnings, lid and power button, dim / screen off / suspend when idle,
  brightness keys with an on-screen display, power profiles; a new lock
  screen (blurred wallpaper, clock, avatar, password, Switch User) that
  stays locked even if its window crashes. New **Power** and **Lock
  Screen** pages in Settings.

## Start Menu, panel search and Action Center

- Start Menu in the style of the reference: a narrow left rail (user,
  Documents, Pictures, Settings, Power), the app list A–Z with recently
  added apps, a tidy category filter, and **big tiles for your favorites**
  in named groups (resize, move, rename by right-click).
- **Search lives in the panel** ("Type here to search") next to the Start
  button; typing with the menu open goes there too.
- **Action Center:** a brightness slider next to the volume slider.

## Recommendations for the next patches

1. **Test on real hardware** — lock screen, lid, suspend, brightness keys,
   USB copies in Liria, touchpad scrolling in the Start Menu and the
   compositor on a real GPU cannot be fully checked in CI.
2. **Translations** — many new texts (Xanara modes, Niny, Taca, Liria
   About/help, Settings pages) are English only; refresh the `.po` files
   and add Indonesian (`id`).
3. **Wayland plan** — SDE is X11 only; plan a compositor path (e.g. a
   wlroots-based WM) before 1.0.
4. **Accessibility** — keyboard navigation everywhere, screen-reader
   (AT-SPI) support for native SDE-FLTK apps, high-contrast theme.
5. **Installer and first-run** — a welcome app (theme, accent, Wi-Fi,
   updates, privacy), and a DebION live/installer image with SDE.
6. **Software center** — a GUI for installing apps (APT + Flatpak) built on
   the SDE Update backend.
7. **Settings coverage** — printers, users and accounts, date/time and
   region, default applications, online accounts.
8. **Notifications center** — history and per-app settings in the Action
   Center.
9. **Liria** — it is still a large C/GTK3 fork; consider a native SDE-FLTK
   file manager in the long term, like the other apps.
10. **Signed packages and an APT repository** for DebION instead of zip
    downloads.

## Install for review

```sh
sha256sum -c SHA256SUMS --ignore-missing
sudo apt install ./sde_0.1.0~alpha16_amd64.deb ./sde-desktop_0.1.0~alpha16_all.deb \
    ./debion-desktop-base_0.1.0~alpha16_all.deb
```

Log out and choose **SDE-Desktop** at the login screen. If something
crashes, please send `~/.local/state/sde/session.log`.
