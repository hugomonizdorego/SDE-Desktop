# SDE 0.1 Alpha 10 — SDE Aero

Development patch for **DebION GNU/Linux** (Debian testing), amd64. Still alpha:
test it in a virtual machine or a spare installation before daily use.

## What changed

### SDE Aero theme (new default)

- **SDE Aero** and **SDE Aero Dark** for GTK 3 and GTK 4, plus matching SDE WM
  (Openbox) window frames. Glass title and tool bars with smooth gradients,
  rounded controls, soft blue selections, translucent menus and popovers.
- Design reference: [GTKAero](https://github.com/User738git/GTKAero)
  (GPL-3.0). SDE Aero is written from scratch in CSS — no images or CSS are
  copied — so it is smoother, scales cleanly and does not look identical.
- **Adjustable glass** in *Settings → Appearance → SDE Aero glass*:
  transparency (0–60 %), blur on/off and strength, rounded window corners and
  fade animations. The values are stored in `[aero]` in `sde.conf`:

  ```ini
  [aero]
  enabled=true
  transparency=18
  blur=true
  blur_strength=6
  rounded_corners=true
  animations=true
  ```

- The compositor configuration is generated from these values. It uses OpenGL
  (`glx`, dual-Kawase blur) when a DRM graphics device exists and XRender
  otherwise, and falls back to XRender automatically if OpenGL fails.
  `[compositor] backend=glx|xrender` forces one. Your own
  `~/.config/sde/compositor.conf` still takes priority.
- Accent color and glass opacity reach GTK through a marked block at the top
  of `~/.config/gtk-3.0/gtk.css` and `gtk-4.0/gtk.css`; the rest of those
  files is never touched.
- The default accent is now Aero blue (`#2672BE`). Teal remains available.

### Panel and Action Center

- **Duplicate tray icons fixed.** Three causes were found:
  1. the panel showed a second *quick controls* icon next to the Action
     Center button;
  2. the Action Center repeated every system indicator above the real tray;
  3. applets such as `nm-applet`, `blueman-applet` and the power manager
     showed their own icons next to SDE's native network, Bluetooth and
     battery indicators, and some applications registered the same
     StatusNotifierItem twice.

  The extra icon and the repeated row are gone; StatusNotifierItems are
  de-duplicated by owner and path; known applets keep running (nm-applet is
  still the Wi-Fi password agent, Blueman the pairing agent) but their icons
  are hidden. Configure with `[panel] hide_duplicate_tray` and
  `duplicate_tray_items`, or *Settings → Panel → Hide duplicate tray icons*.
- **Action Center redesign:** compact clock card with greeting and date beside
  the time, a single "Tray applications" row (XEmbed icons on the same line as
  AppIndicators), quick tiles, sound and brightness cards, media last, a back
  arrow on sub-pages and a user badge in the footer.
- Glass sheen on the panel and Start button; panel and popups use
  `_NET_WM_WINDOW_OPACITY` so the compositor can make them translucent.

### Applications

- **Photo** (formerly *SDE View*). The About dialog keeps every upstream
  author and copyright and says *Based on Eye of MATE*.
  - New **Effects and Filters** side pane (Tools menu, or press **E**):
    Original, Vivid, Warm, Cool, Mono, Noir, Sepia, Vintage, Matte and Invert,
    plus Brightness, Contrast, Saturation, Warmth, Fade, Vignette and Soften.
    Changes are previewed live on the picture; switching pictures restores the
    original pixels. **Save Copy…** writes a new file; the original is never
    overwritten.
  - Opens with the gallery strip at the bottom and a dark stage.
- **Niny** enables more plugins by default: Document Statistics, Modelines,
  File Browser, Spell Checker, Insert Date/Time, Sort, Trailing Spaces,
  Snippets, Quick Open and External Tools; also line numbers, current-line
  highlight, bracket matching and the overview map.
- **Liria** gets an icon-only glass command bar, rounded breadcrumb path,
  calmer sidebar and larger default icons, so it no longer looks like Caja.
- **SDE System Monitor** draws rounded, theme-aware graph cards with a soft
  grid and filled curves, uses a new palette and opens on Resources.
- The power manager's own tray icon is off by default (the panel shows the
  battery).

All of these are applied while preparing the forks
(`suite/redesign.py`, `suite/overlays/`) and as GSettings defaults
(`data/schemas/30_sde-suite.gschema.override`). Users can change every value.

### Session, login and settings

- The session is called **SDE-Desktop** everywhere: in LightDM
  (`sde-desktop.desktop`), `XDG_SESSION_DESKTOP=SDE-Desktop`,
  `DESKTOP_SESSION=sde-desktop` and `XDG_CURRENT_DESKTOP=SDE-Desktop:SDE`
  (the short name stays so `OnlyShowIn=SDE` launchers keep working). A hidden
  `sde.desktop` keeps old accounts working.
- `sde-desktop` makes SDE-Desktop LightDM's default session and styles the
  LightDM GTK greeter with SDE Aero Dark and the Selviana wallpaper.
- Settings fixes: open GTK applications now follow theme, icon and font
  changes immediately (xsettingsd is reloaded); turning compositing off stops
  it at once; process signals only reach the current user.

### Packages and screenshots on GitHub

Every successful build of `main` (or a manual *Run workflow*) now:

- replaces the **`alpha-latest`** pre-release with the fresh `sde` and
  `sde-desktop` packages, complete sources and `SHA256SUMS`;
- updates [`docs/screenshots/latest`](screenshots/latest/README.md) from the
  CI captures of the real programs.

Tagged releases (`v0.1.0-alpha10`, …) still use `release.yml`.

## Recommendations: what to fix and what to add next

Ordered by impact on a smooth DebION desktop.

### Fix

1. **Test on real hardware.** CI runs on a virtual display; OpenGL blur,
   brightness, Bluetooth, laptop lids and multi-monitor docks need physical
   testing on Intel, AMD and NVIDIA (nouveau and proprietary).
2. **Live compositor start.** Turning compositing *on* still needs a new
   login; give `sde-session` a small D-Bus or signal API to start/stop
   supervised components.
3. **Translations.** Most new strings (Aero settings, Photo Effects) are only
   in English; regenerate `po/sde.pot` and add Bahasa Indonesia first.
4. **Photo editing in place.** Effects save a copy only. Saving over the
   original needs integration with Photo's undo stack and its lossless JPEG
   path (rotation then save would otherwise mix both).
5. **Accessibility.** Check keyboard focus order in the Action Center and
   Settings with a screen reader, and add a high-contrast SDE Aero variant.
6. **Session robustness.** Add a watchdog for xsettingsd and the polkit
   agent, and show the rescue dialog when the compositor keeps failing
   instead of silently running without it.
7. **Packaging.** Split `sde` into `sde-shell`, `sde-suite` and
   `sde-themes` so DebION ISOs can pick components; add autopkgtests.

### Add

1. **Wayland plan.** Keep X11 for 0.1, but design the panel and settings
   so a wlroots/labwc session can be added for 0.3.
2. **Lock screen and greeter in SDE Aero** with blur of the wallpaper
   (lightdm-gtk-greeter is styled; the SDE Screen Lock dialog is not yet).
3. **Night light** (color temperature schedule) and **display brightness
   per monitor** in the Action Center.
4. **Notifications center history** grouped by application, with
   per-application mute.
5. **Clipboard history** and a **screenshot tool** bound to Print Screen.
6. **Online accounts / file sharing** (GVfs SMB/MTP browsing in Liria with
   a nicer network view).
7. **Software center** front-end for PackageKit next to SDE Update
   (Flatpak optional).
8. **Theme picker** for SDE Aero accent presets per wallpaper and an
   automatic light/dark schedule.
9. **First-run welcome app**: language, keyboard, theme, privacy and
   driver suggestions for DebION.
