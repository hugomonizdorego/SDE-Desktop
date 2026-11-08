# Roadmap — from 0.2.0 Alpha 4 to 1.0 Final

Selviana Desktop Environment (SDE) is a desktop environment. DebION GNU/Linux
is a separate project: a Debian-based distribution (Linux kernel) that ships
SDE. This roadmap covers SDE; what the distribution needs is listed in its own
section at the end.

## How versions work

| Stage | Debian version | Meaning |
| --- | --- | --- |
| Alpha | `0.2.0~alpha1`, `~alpha2`, … | New features land; things may change or break. |
| Beta | `0.2.0~beta1`, … | Feature freeze: only fixes, translations and polish. |
| Release candidate | `0.2.0~rc1`, … | Only release-blocking fixes. |
| Release | `0.2.0`, then `0.2.1`, … | Stable for that series; fixes only. |

`~` sorts before the final version, so every Alpha, Beta and RC upgrades
cleanly to the next one and to the release. Dates below are indicative
targets, not promises.

## Done: 0.1 Alpha series (alpha1 – alpha52, 2026)

The foundation: a desktop written from the ground up in C++17 on SDE-FLTK,
cairo and Xlib, with a Rust configuration core.

- Session manager with rescue dialog, panel, desktop, Start Menu, Action
  Center, notifications, Run and Log Out, screen lock and power management
  (written from scratch), polkit agent and XSETTINGS daemon.
- Native applications written from scratch: Niny, Xanara, Taca, Photo,
  Gallery, Calculator, System Monitor, Software Updater, Welcome, Driver
  Manager, System Reports; Liria (file manager, fork of Caja).
- Window manager (sde-wm, from Openbox) and compositor (from picom) built
  from source; SDE Graphite, Fluent and Glass themes for GTK 3/4.
- Panel flyouts, taskbar previews, per-icon menus, notification history,
  drag and drop on the desktop and in Liria, Default Applications, network
  repair for Debian standard installs.

## Done: 0.2.0 Alpha 1 — identity and consolidation (October 2026)

- [x] Official SDE logo; a cohesive icon family for all SDE applications
- [x] About windows with copyright, license and trademark information
- [x] SDE and DebION clearly separated (developer, texts, metadata)
- [x] SDE Settings renamed **Control Center**, organised as one central hub
- [x] User avatars (Control Center and Start Menu; samples in `/usr/share/Avatar`)
- [x] Start Menu made faster and more reliable
- [x] Liria: visible rubber-band selection
- [x] Release audit: builds, hardening, sanitizers, security review
- [x] Screenshot showcase of every component for each build

## Done: 0.2.0 Alpha 2 — software, accounts and polish (October 2026)

- [x] **Merkadu**, the SDE software center: Debian packages, Flatpak and Snap
      ([0.2-ALPHA2.md](0.2-ALPHA2.md), [MERKADU.md](MERKADU.md))
- [x] Uninstall from the Start Menu; tile groups by category with drag and
      drop; typing always searches; icons for apps without one
- [x] User accounts in the Control Center; maximized Control Center; 75 %
      and 175 % interface sizes
- [x] Animated battery icon with display modes and time estimates; clock
      options Show seconds and Blink the colon
- [x] Liria: plain folder icon, color ribbon, faster big folders, copy
      progress with the animated SDE logo (also on the desktop)
- [x] Every theme redesigned; SDE Liquid Glass with a compositor shader;
      sharper window buttons at 125–200 % (audit A1-39)
- [x] SDE Welcome editable by distributions (`/usr/share/SDE-Welcome`)
- [x] SDE-FLTK on FLTK 1.4.5

## Done: 0.2.0 Alpha 3 — editing, installing and copying safely (October 2026)

- [x] Xanara edits PDFs directly and needs no other program
      ([0.2-ALPHA3.md](0.2-ALPHA3.md), [XANARA.md](XANARA.md))
- [x] Merkadu: real catalogs refreshed at start, All Software like Synaptic,
      Flathub out of the box, Snap on request, app updates only
- [x] Start Menu: favorites again, tile folders, live badges
- [x] Copies to USB sticks and external drives finished when they say so; a
      copy dialog of our own (the penguin)
- [x] Liria: smooth hover; network places in the address bar
- [x] Niny: tabs and a full menu bar; Photo collages; Gallery Edit With and
      plugins for both
- [x] Debrun for downloaded `.deb` files; AppImages after verification;
      renaming accounts at the next restart

## Done: 0.2.0 Alpha 3.1 — fixes from the review on a real laptop (November 2026)

- [x] Merkadu ends when closed and no longer freezes; Pause and Cancel;
      Debian, Flatpak and Snap apart; rotating highlights; only real sources;
      Flatpak remotes and snaps after Enable Snap Support fixed
      ([0.2-ALPHA3.1.md](0.2-ALPHA3.1.md), [MERKADU.md](MERKADU.md))
- [x] A modern file chooser in every SDE program and for portal users
      ([FILE-CHOOSER.md](FILE-CHOOSER.md))
- [x] Liquid Glass as frosted, readable glass; even transparency; no stale,
      see-through windows on software OpenGL; menus without fades
- [x] Appearance: Preview, Revert and Apply; glitch-free theme switches
- [x] Force Close for frozen programs ([FORCE-CLOSE.md](FORCE-CLOSE.md));
      session buttons always in the Action Center
- [x] Liria: faster context menus and highlights

## Now: 0.2.0 Alpha 4 — SDE's own core, one package per application (November 2026)

- [x] SDE Core: the core controller `sde-core` (settings with a schema, one
      Control Center page per setting, the look, apps, the session)
      ([0.2-ALPHA4.md](0.2-ALPHA4.md), [CORE.md](CORE.md))
- [x] SDE Core and one Debian package per application; GNOME Terminal,
      LightDM and Slick Greeter in `sde-desktop` ([PACKAGING.md](PACKAGING.md))
- [x] Liria written from scratch, nothing left of Caja ([LIRIA.md](LIRIA.md))
- [x] Up to 5 workspaces, Task View, SDE's window menu and the Windows keyboard
      shortcuts with a printable sheet ([WINDOWS.md](WINDOWS.md),
      [SHORTCUTS.md](SHORTCUTS.md))
- [x] Tray and IBus no longer freeze the desktop; Start button text and
      picture; Recently added Clear
- [x] Themes tinted by the wallpaper; smooth touch screens
      ([TOUCH.md](TOUCH.md)); fingerprint ([FINGERPRINT.md](FINGERPRINT.md))
- [x] Xanara's start page, System Monitor's kernel threads, Merkadu's storage

## 0.2 series — usable every day (target: Q1 2027)

Alpha 5 and later:

- [ ] Alpha 4 on real hardware: touch screens (gesture values, the Start
      Menu's grab, Firefox), fingerprint readers with LightDM and sudo, the
      LightDM greeter and live login, IBus with newer releases
- [ ] Liria: network places against real servers, burning discs, group by;
      Liria through the SDE core client (look, wallpaper)
- [ ] Clipboard history (Win+V) and a magnifier (Win+Plus)

- [ ] Merkadu, Debrun and user accounts validated on real installations
      (PackageKit, Flathub, snapd, AccountsService); copies to real FAT USB
      sticks measured; Liria against NFS, WebDAV and domain logins
- [ ] Liquid Glass, the compositor patches and Force Close measured on real
      GPUs (Intel, AMD, NVIDIA) and on the owner's laptop; the file chooser
      portal through the real xdg-desktop-portal
- [ ] Xanara: editing rotated text and right-to-left scripts; faster undo for
      large files

- [ ] Translations: Indonesian first, then Portuguese and Tetum; translation
      workflow (Weblate or po files in the repository) and 100% string coverage
- [ ] Real-hardware validation programme: lock/suspend/resume, lid, battery,
      brightness, touchpad, multi-monitor, Wi‑Fi, USB drives — on at least
      five laptops and two desktops, results published per build
- [ ] Notifications: grouping rules and per-application settings
- [ ] Input devices plugged in during the session get the saved settings
- [ ] Paste files on the desktop (clipboard file lists)
- [ ] Settings coverage in Control Center: printers, region and language,
      online accounts (evaluate), accessibility page
- [ ] Upgrade path tests from every 0.1 alpha to 0.2 (configuration migration)
- [ ] Open items of the [0.2 Alpha 1 audit](AUDIT-0.2.0-alpha1.md): a Driver
      Manager helper with its own polkit action, a bounded tray icon cache,
      picom v13

Beta (feature freeze) → RC → **0.2.0**:

- [ ] No known crash or data-loss bug; all automated suites green on CI
- [ ] Documentation: user guide for every application, administrator notes
- [ ] Security review repeated; hardening flags on every binary

## 0.3 — quality and access (target: Q2 2027)

- [ ] Accessibility: full keyboard navigation everywhere, screen-reader
      support (AT-SPI) for SDE-FLTK applications, high-contrast theme, large
      text, reduced motion everywhere
- [ ] HiDPI completeness: fractional scales, per-monitor scale, crisp window
      buttons and icons at 150–200%
- [ ] Performance budgets enforced in CI: login to usable desktop < 2 s,
      Start Menu open < 80 ms, idle CPU ≈ 0%, memory budget per component
- [ ] Workspace switcher and window overview
- [ ] Panel applets API (documented, versioned) and panel position options

## 0.4 — ecosystem (target: Q3 2027)

- [ ] Software center on top of the Software Updater backend (APT + Flatpak)
- [ ] Snapshots/backup integration (Timeshift-style) in Welcome and Updater
- [ ] Rust migration steps from [RUST.md](RUST.md): shared desktop-entry
      parsing, session supervision, notification service
- [ ] Theme and icon packaging guidelines for third parties

## 0.5 Beta — feature freeze for 1.0 (target: Q4 2027)

- [ ] Decision on the window manager: keep improving sde-wm (Openbox based)
      or move to an SDE window manager; written plan for Wayland
- [ ] Stable configuration format with versioned migrations
- [ ] Stable D-Bus interfaces (org.sde.*) documented
- [ ] Public beta with release notes and a bug-triage process

## 0.9 / 1.0 RC — stabilisation (target: Q1 2028)

- [ ] Only release-blocking fixes; translation freeze
- [ ] Three months of public testing without critical regressions
- [ ] Independent security review of privileged components

## 1.0 Final (target: Q2 2028)

Release criteria:

- [ ] No open critical or high-severity bugs; no known data-loss bugs
- [ ] Upgrade path from 0.2 onwards tested automatically
- [ ] Accessibility review passed; at least three complete translations
- [ ] Complete user and administrator documentation
- [ ] Packages signed and published in a public APT repository
- [ ] Wayland plan published (implementation may follow after 1.0)

## DebION GNU/Linux integration (separate project)

These items belong to the distribution, not to SDE, and are tracked here only
because they affect testing:

- Live image and installer that ship SDE; the Welcome screen is already
  hidden on live sessions
- Signed APT repository hosting for SDE packages
- Distribution branding (wallpapers, `debion-desktop-base`) kept separate
  from SDE branding
