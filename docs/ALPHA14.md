# SDE 0.1 Alpha 14 — Gallery, flyouts, new Menu and Action Center, Update, Liria

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still
alpha: test it in a virtual machine or a spare installation first.

## Gallery is back

**Gallery** (`sde-gallery`) is a new photo library written from scratch by
the DebION Dev-Team. (In alpha13 the only gallery was the strip inside the
Photo viewer.)

- Sidebar: All Photos, Favorites, Recent (30 days), Screenshots and your
  folders (Pictures plus folders you add); counts for each.
- Grid grouped by month, zoom slider, search, sort by newest, oldest or name,
  keyboard navigation, multi-select (Ctrl/Shift) with Favorite and Move to
  Trash.
- Thumbnails are made in the background and cached in `~/.cache/thumbnails`
  (shared with other apps).
- Viewer: previous/next, zoom, rotate, slideshow, details; Edit in Photo,
  Open With, Set as Wallpaper, Show in Files, Copy Path, Move to Trash.

## Panel: tray flyouts and a new Action Center

**Each tray icon opens its own flyout** above it instead of the Action
Center: Sound (volume, output device, microphone, application volumes),
Network, Bluetooth, Power (battery, power mode, brightness), Media, Drives,
Keyboard (switch layouts) and Tray (hidden application icons). One click on
another panel button switches; a second click on the same button closes.

**The Action Center has its own job**, opened with its corner button or
Super+A:

- Header with time and greeting, and a **Show less / Show more** button:
  compact mode shows tiles and sliders; expanded mode adds media, the
  calendar and notifications. The choice is remembered
  (`[action_center] expanded`).
- Tiles: Wi-Fi and Bluetooth (with a shortcut to their flyout), Airplane
  mode, Do Not Disturb, Dark Mode, Display, Screenshot, Settings.
- **The calendar is part of the Action Center.** The clock (and Super+C)
  opens it at the calendar.

## Start Menu

- **Search shows that it is ready.** FLTK ignored focus changes while the
  menu held the pointer grab, so the field never got the keyboard. Now the
  field has an accent ring and a blinking caret as soon as the menu opens,
  and clicking it shows recent apps and quick settings before you type.
  Results include applications, Settings pages, session actions and files.
- **New layout:** a header with your name and Files, Settings, Lock and
  Power buttons; category chips that open their own pages; **Favorites** as
  groups of large tiles on pages with dots; **All Programs** as an A–Z list
  with letter headings and a letter-jump grid.
- Right-click an app: Add to/Remove from Favorites, Move, Pin to Panel, Add
  to Desktop. Right-click elsewhere: Arrange Menu.

## SDE Update

A redesigned app with pages for **Updates** (grouped into Security, Kernel,
Software and Backports, details with versions and changelogs, progress with
Cancel, restart notice), **History**, **Sources**, **Maintenance** (clean
cache, remove unused packages, repair) and **Settings** (background checks
and notifications, `[update]` in sde.conf). Updates are never installed
automatically. Credits to Linux Mint's mintupdate are kept in About.

## Liria

- A **Home overview** above your home folder: Quick access cards, Drives
  with usage bars ("X free of Y"), Recent files; it can be hidden.
- One command row (navigation, address bar, view, search, terminal, a main
  menu button), sidebar sections Favorites, Drives, Bookmarks, Network.
- Styled from the palette of every SDE theme (Light, Dark, Aero, Liquid
  Glass and their dark variants), so Liria matches the desktop after a
  theme change. The MATE/Caja credits stay in About.

## SDE's own building blocks

| Component | What it is |
| --- | --- |
| **SDE-FLTK** | the DebION Dev-Team fork of FLTK 1.3.11, now kept in this repository (`sde-fltk/`) and linked statically |
| **sde-xsettingsd** | new XSETTINGS manager, written from scratch |
| **sde-polkit-agent** | new password dialog for administrative actions, written from scratch |

FLTK's license (LGPL 2 with the FLTK exceptions) requires the original
copyright notices to stay, so SDE-FLTK is a maintained fork: DebION owns its
changes, the FLTK authors keep theirs. SDE WM (Openbox) and the SDE
compositor (picom) remain source-built forks.

## Install for review

```sh
sha256sum -c SHA256SUMS --ignore-missing
sudo apt install ./sde_0.1.0~alpha14_amd64.deb ./sde-desktop_0.1.0~alpha14_all.deb \
    ./debion-desktop-base_0.1.0~alpha14_all.deb
```

Log out and choose **SDE-Desktop** at the login screen. If something
crashes, please send `~/.local/state/sde/session.log`.
