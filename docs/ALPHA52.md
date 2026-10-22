# SDE 0.1 Alpha 52 — drag & drop, Liria Ribbon, a smarter panel

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still
alpha: test it in a virtual machine or a spare installation first. The
developer keeps monitoring reports and developing SDE.

## Internet works right after login

- **At installation** `debion-desktop-base` repairs the network once:
  a broken `/etc/resolv.conf` is handed back to NetworkManager, cards that
  nothing manages are given to NetworkManager, imported profiles connect
  automatically and a plugged-in cable gets a connection.
- **At every boot** `debion-network-repair.service` runs the same safe
  repair (it does nothing on a healthy system and never delays the login
  screen).
- **At login** the panel checks the connection at once. The LED stays
  yellow ("Checking internet access…") until DNS really works, then turns
  green. If DNS breaks, an administrator on the local session gets it
  repaired silently; anyone else sees "Internet needs a quick repair — Fix
  now".
- Opt out with `[network] auto_repair=false` in `/etc/debion/network.conf`.

## Drag and drop

- **Desktop:** move icons freely (snapped to a grid, positions remembered),
  rubber-band selection, Arrange Icons by name/size/type/date or
  Auto-arrange; drop on folders (move), with Ctrl (copy), Shift (move),
  Ctrl+Shift (link) or Alt (ask); drop on the Trash or on an application
  launcher; drag desktop files into Liria, Firefox and other apps; drop files,
  links or text from other apps onto the desktop. Nothing is overwritten
  without asking "Skip / Keep Both / Replace".
- **Liria:** drag Home cards (folders, drives, recent files) anywhere; drop
  files on folder and drive cards; drop a folder on Quick access or the
  Pinned sidebar heading to pin it; right-drag icons for Move / Copy / Link.

## Liria

- **Ribbon** (optional): Home, Share and View tabs, plus Drive and Archive
  tabs when they apply; buttons follow the selection. Three modes — full,
  tabs only, off — from the ⋮ menu, the View tab or **Ctrl+F1**. It works in
  every folder.
- **Home:** the overview replaces the folder grid (no more duplicate
  folders below it); Quick access shows pinned items with a pin and suggests
  frequent folders; Drives are cards in a grid; sections collapse.
- **Sidebar:** Home, Pinned, Bookmarks, Drives, Network with collapsible
  headings and the colour icons of your icon theme.
- **Path bar:** scroll arrows only for long paths; clicking the empty part
  opens the location entry; breadcrumbs always navigate.

## Panel

- **Right-click any icon** for its own menu: Start, search, taskbar windows,
  network, sound, Bluetooth, battery, keyboard, drives, touchpad, clock,
  notifications, Action Center, tray apps (their own menus) and the empty
  panel.
- **Search:** right-click → Show search box / Show search icon only /
  Hidden. Super still opens the Start Menu search.
- **Notifications** moved out of the Action Center into their own bell
  button with an unread count, grouped history, Clear all and Do Not
  Disturb.

## Action Center and clock

- **Never scrolls:** it chooses one or two columns to fit the screen.
  Compact shows the clock and the date ("Thursday, 22 October 2026"),
  quick toggles, sliders, the player while something plays and the power
  buttons; expanded adds the full calendar, agenda and time zone.
- **Big clock** over the animated weather, or a day/night sky when there is
  no weather.
- **Clock styles** for the panel and the Action Center: Digital, Blink
  (blinking colon), LED (seven segments) and Analog; 12/24 hour, seconds.
- **Weather units:** Celsius, Fahrenheit or Kelvin; km/h, mph, m/s or knots.
- New **Settings → Clock & Weather** page.

## Settings → Default Applications

Pick the web browser, mail, calendar, music, video, image viewer, text
editor, office documents, spreadsheet, presentation, PDF, file manager,
terminal and archive manager from lists of installed apps; "Other file
types…" for any other type; "Reset to defaults".

## Recommendations for the next patches

1. **Test on real hardware and the VirtualBox install:** network repair at
   install/boot/login, drag and drop between Liria, the desktop and Firefox,
   the Liria path bar.
2. **Translations** (Indonesian first) for the many new texts.
3. **Liria:** group by, file extensions, network sharing and disc burning in
   the Ribbon.
4. **Snapshots** and a **software center**.
5. **Signed APT repository** for DebION.

## Install for review

```sh
sha256sum -c SHA256SUMS
sudo apt install ./sde_0.1.0~alpha52_amd64.deb ./sde-desktop_0.1.0~alpha52_all.deb \
    ./debion-desktop-base_0.1.0~alpha52_all.deb
```

Log out and choose **SDE-Desktop** at the login screen. If something
crashes, send `~/.local/state/sde/session.log` (or use System Reports); for
network problems send `/var/log/debion-network-check.log`.
