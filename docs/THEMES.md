# SDE themes, transparency and icons (0.1.0~alpha50)

SDE has three theme families, SDE Graphite (default), SDE Fluent and SDE
Glass, each light or dark with nine accents. `data/themes/palettes.json` is
the single source of their colors and shapes: `tools/build-themes.py` builds
the GTK 3, GTK 4, libadwaita and SDE WM (Openbox) themes from it and writes
`generated/theme_palettes.h` for `sde::Theme`.

## Window buttons

Every family has the same standard caption buttons on the right:
minimize (—), maximize (□, a restore glyph of two squares while maximized)
and close (✕). They are wide (about 40 x 34 px at 100 %), show a hover
layer, and Close turns red on hover. Graphite and Glass no longer have round
buttons on the left.

* SDE WM: `<titleLayout>NLIMC</titleLayout>` (window icon, title, minimize,
  maximize, close). Configurations with the old left layouts of SDE (`CIML`,
  `CIMLN`) move to `NLIMC` the next time the session or Settings syncs the
  window manager; a layout chosen by hand is kept.
* GTK: `gtk-decoration-layout=:minimize,maximize,close` in `settings.ini`,
  XSETTINGS (`Gtk/DecorationLayout`) and the theme's `index.theme`. Header
  bar buttons of GTK 3 and GTK 4 applications are styled to match by
  `data/themes/overlays/caption-gtk-{3.0,4.0}.css` (all families).
* Size: SDE WM sizes the buttons from the title height, which follows the
  window title font and so the interface scale. This needs SDE WM's small
  patch `suite/patches/sde-wm/0001-wide-caption-buttons.patch`, applied by
  `tools/prepare-components.py`; it adds three theme keys (stock Openbox
  ignores them and shows its smaller buttons):

  | themerc key                     | meaning                                   |
  |---------------------------------|-------------------------------------------|
  | `window.button.width.percent`   | button width, percent of the title height |
  | `window.button.height.percent`  | button height, percent of the title height (100 = full height) |
  | `window.button.spacing`         | pixels between buttons and before the right edge |

  `palettes.json` sets them per family in `caption` (`width`, `height`,
  `spacing`), with the glyph style (`thin` for Fluent, `regular`) and the
  red of the close button (`close_hover`).

## Transparency and clarity

Settings > Appearance > Transparency and clarity has presets **Off** (0),
**Subtle** (25, the default), **Clear** (50) and **Glass** (80) and a slider
from 0 to 100. One key drives every translucent surface of every family:

```ini
[aero]
enabled=true        # translucent surfaces on or off
transparency=25     # 0 (opaque) to 100
blur=true
```

* The panel, SDE menus, the Start menu, the Action Center and popups get a
  whole-window opacity (`Theme::opacity`, `Theme::popup_opacity`).
* Window frames: compositor `frame-opacity` (`Theme::frame_opacity`), fully
  for Glass and partly for Graphite and Fluent.
* GTK 3/4 menus and popovers: a translucent background with opaque text, in
  the managed block of `~/.config/gtk-3.0/gtk.css` and `gtk-4.0/gtk.css`
  (`Theme::gtk_menu_alpha`). The rules apply only to `.csd` windows, which
  GTK marks only while a compositor runs, so without one everything stays
  opaque. The compositor then leaves GTK menus at full opacity.
* Text stays crisp: 100 is the most transparent each surface can be while
  its text keeps WCAG contrast over any backdrop (4.5:1 for text, 3:1 for
  secondary text; `readable_opacity()` in `src/common/theme.cpp`). The
  `sde-theme` test checks every family, mode and accent at 100.
* The lite compositor profile (`[compositor] effects=lite`, or `auto` in
  virtual machines and with software rendering) turns blur off and keeps
  surfaces more opaque (half the effect, opaque frames).

## Icons

Settings > Appearance > Icons shows a card for every installed icon theme
that has application or places icons (folder, home, settings, terminal and
picture previews), plus **Automatic**:

```ini
[appearance]
icon_theme=auto     # or a theme directory name, e.g. Papirus-Dark, Adwaita
```

* `auto` (default) is Papirus, or Papirus-Dark in dark mode, when
  `papirus-icon-theme` (recommended by the packages) is installed; then
  Adwaita, then another installed theme. A theme that is not installed
  falls back to `auto` (`resolve_icon_theme()` in `src/common/icons.cpp`).
* The same choice goes to GTK (`gtk-icon-theme-name`, XSETTINGS
  `Net/IconThemeName`, GSettings `icon-theme`), so Liria and GTK
  applications follow it, and to every SDE program through `IconLoader`:
  desktop icons, Start menu, panel and tray fallbacks, Settings, Gallery,
  Taca and the others. Programs reload it when the configuration serial
  changes, so a new choice applies at once.
* Lookup follows the Icon Theme Specification: the theme, its `Inherits=`
  parents depth first, then Adwaita, AdwaitaLegacy and hicolor, then
  `/usr/share/pixmaps`. Scaled directories (`Scale=2`) count with their
  pixel size. `tests/fixtures/icon-themes` exercises this.
