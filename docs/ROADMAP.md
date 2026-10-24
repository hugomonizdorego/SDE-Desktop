# Roadmap — from 0.2.0 Alpha 1 to 1.0 Final

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

## Now: 0.2.0 Alpha 1 — identity and consolidation (October 2026)

- [x] Official SDE logo; a cohesive icon family for all SDE applications
- [x] About windows with copyright, license and trademark information
- [x] SDE and DebION clearly separated (developer, texts, metadata)
- [x] SDE Settings renamed **Control Center**, organised as one central hub
- [x] User avatars (Control Center and Start Menu; samples in `/usr/share/Avatar`)
- [x] Start Menu made faster and more reliable
- [x] Liria: visible rubber-band selection
- [x] Release audit: builds, hardening, sanitizers, security review
- [x] Screenshot showcase of every component for each build

## 0.2 series — usable every day (target: Q1 2027)

Alpha 2 and later:

- [ ] Translations: Indonesian first, then Portuguese and Tetum; translation
      workflow (Weblate or po files in the repository) and 100% string coverage
- [ ] Real-hardware validation programme: lock/suspend/resume, lid, battery,
      brightness, touchpad, multi-monitor, Wi‑Fi, USB drives — on at least
      five laptops and two desktops, results published per build
- [ ] Liria: Ribbon completion (group by, file extensions, network shares,
      disc burning), search, tabs polish
- [ ] Gallery: places map and duplicate finder
- [ ] Notifications: grouping rules and per-application settings
- [ ] Input devices plugged in during the session get the saved settings
- [ ] Paste files on the desktop (clipboard file lists)
- [ ] Settings coverage in Control Center: printers, region and language,
      online accounts (evaluate), accessibility page
- [ ] Upgrade path tests from every 0.1 alpha to 0.2 (configuration migration)
- [ ] Open items of the [0.2 Alpha 1 audit](AUDIT-0.2.0-alpha1.md): a Driver
      Manager helper with its own polkit action, a bounded tray icon cache,
      sharper window buttons at 150 %, picom v13

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
