# SDE 0.1 Alpha 15 — native apps, Start Menu, Panel, Settings, Liria, performance

> **Note (0.2.0 Alpha 1):** these notes are historical. Up to 0.1.0~alpha52 SDE named
> the "DebION Dev-Team" as its developer; since 0.2.0 Alpha 1 the developer is the
> **Selviana DE Project**. DebION GNU/Linux is a separate Debian-based distribution
> that ships SDE. See [LEGAL.md](LEGAL.md).

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still
alpha: test it in a virtual machine or a spare installation first.

## Applications written from scratch by the DebION Dev-Team

Five applications that were rebranded MATE programs are now new SDE-FLTK
programs owned by the DebION Dev-Team (GPL-3.0-or-later). The MATE forks they
replace (Pluma, Atril, Engrampa, Eye of MATE, MATE System Monitor) are no
longer built or installed. Each reports version 2.0.0~alpha1.

| Application | What is new |
| --- | --- |
| **Niny** (text editor) | Tabs in the header, syntax highlighting for 17 languages (incremental, fine for 10 MB files), find & replace card with regex and match marks, command palette (Ctrl+Shift+P), encodings and line endings, side panel with documents and folder, session restore and crash recovery |
| **Xanara** (PDF) | Sharp pages rendered at screen resolution, thumbnails, outline, search, night/sepia modes, presentation. **Edit PDFs:** highlight/underline/strike-out, notes, text boxes, pen, shapes, signature image, form filling; rotate, reorder, insert, delete and extract pages (with `qpdf`) |
| **Taca** (archives) | New look; zip, 7z, rar/rar5, tar.gz/bz2/xz/zst/lz4, iso, cab, deb, rpm and more through libarchive, with 7-Zip/unrar for encrypted archives; create zip/7z/tar with passwords and split volumes; protection against archives that write outside the target folder |
| **Photo** | Instant opening, smooth zoom, info panel with camera (EXIF) and GPS details, crop/rotate/resize, adjustments and filters with undo, lossless JPEG rotation, set as wallpaper; works together with Gallery |
| **SDE System Monitor** | No MATE images; Processes (apps vs background, tree), Performance graphs (CPU, memory, disks, network, GPU when available), File Systems, Startup Apps |

**Gallery** gained File/Edit/View/Help menus, a **Devices** section for USB
drives and SD cards (browse in place or import), and works together with
Photo: Photo steps through the same album, "Show in Gallery" (Ctrl+G), and
edits, deletions and favourites appear in Gallery at once.

## Start Menu

- All categories fit without scrolling (up to three rows, icons only on very
  small screens).
- Mouse wheel scrolls whatever is under the pointer; scrollbars can be dragged;
  Page Up/Down work.
- Slimmer: about 580 px wide on a normal screen.
- **Favorites:** at most 12 large tiles. **Recently used** below them ranks
  apps by how often and how recently you open them.
- Lock, power and Settings buttons moved to the Action Center.

## Panel and Action Center

- **Honest internet status.** The network icon and flyout use
  NetworkManager's connectivity state: "Connected, no internet" (with a
  warning badge), "Sign in to network" for captive portals (Open sign-in
  page), or "Connected · Internet access". **Troubleshoot** re-checks and
  reconnects to renew the address and DNS.
- **Tray:** the ^ overflow button appears only when there are many app icons
  (`[panel] tray_overflow_threshold`, default 4); otherwise they sit on the panel.
- **Clock → Alarms.** Create alarms with time, repeat days, name and snooze.
  They ring with a notification and sound (Snooze/Dismiss) even when the
  popup is closed. Stored in `~/.config/sde/alarms.ini`. Super+C still opens
  the calendar.
- **Agenda in the calendar.** Pick a day, press **+**, write the item (time,
  note) and choose a reminder (at start, 5/15/30 min, 1 hour, 1 day before).
  Days with items get a dot; reminders arrive as notifications. Stored in
  `~/.local/share/sde/agenda.ini`.
- **Footer with SDE Settings, Lock Screen and Power** (Sleep, Log Out,
  Restart, Shut Down) — these buttons are no longer in the Start Menu.
- Each flyout's settings button opens its own page of SDE Settings.
- Fewer repaints: status changes are merged into one panel update.

## SDE Settings

- **Desktop → Computer** icon (opens Liria at `computer:///`).
- **Appearance → Wallpaper:** every picture under `/usr/share/backgrounds/`
  as a thumbnail grid; click to apply.
- **About → Graphics:** graphics card model, driver and OpenGL renderer.
- New **Sound**, **Network** and **Bluetooth** pages; every page opens
  directly with `sde-settings --page NAME` (see `sde-settings --list-pages`).

## Liria

- **Clear** recent files on the Home page, and Remove from Recent.
- **USB copies:** progress shows what reached the drive, the copy ends with
  "Finishing… writing data", invalid FAT/exFAT names are fixed before copying,
  files over 4 GiB on FAT are refused up front, and Eject waits for the data.
- **Copy/paste** works with files copied in other applications, and the
  clipboard survives closing the window.

## Smoother desktop

- Windows repaint only what changed; the wallpaper and icons stay on the X
  server. Desktop-icon hover costs about 20× less CPU.
- In virtual machines and without a working GPU driver the compositor uses a
  **lite** profile (no blur, fades or rounded corners). Choose
  `[compositor] effects=auto|full|lite` in `sde.conf`.
- The window manager's minimize animation is off.

## "Connected but no internet"

SDE does not change network settings. `debion-desktop-base` now enables
NetworkManager's connectivity check and ships `debion-network-check`, which
reports the usual DebION causes (interfaces still in `/etc/network/interfaces`,
a broken `/etc/resolv.conf`, proxies). `sudo debion-network-check --fix`
repairs DHCP/Wi-Fi entries and `resolv.conf`; static, bridge and VLAN setups
are never touched. See docs/TROUBLESHOOTING.md.

## Install for review

```sh
sha256sum -c SHA256SUMS --ignore-missing
sudo apt install ./sde_0.1.0~alpha15_amd64.deb ./sde-desktop_0.1.0~alpha15_all.deb \
    ./debion-desktop-base_0.1.0~alpha15_all.deb
```

Log out and choose **SDE-Desktop** at the login screen. If something
crashes, please send `~/.local/state/sde/session.log`.
