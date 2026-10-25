# SDE themes, transparency and icons (0.2.0 Alpha 2)

SDE has three theme families, SDE Graphite (default), SDE Fluent and SDE
Liquid Glass (named SDE Glass from alpha51 to 0.2.0 Alpha 1; its id and
directories stay `glass` and `SDE-Glass-*`), each light or dark with nine
accents. `data/themes/palettes.json` is the single source of their colors
and shapes: `tools/build-themes.py` builds the GTK 3, GTK 4, libadwaita and
SDE WM (Openbox) themes from it and writes `generated/theme_palettes.h` for
`sde::Theme`.

## Palettes and corners (0.2.0 Alpha 2)

The palettes are calm, low-chroma neutrals with muted accents (no neon, no
glow, no colorful gradients): Graphite is neutral gray (after Linux Mint),
Fluent a cool slate after Windows 11, Liquid Glass a cool, clear white (and
a deep graphite in dark mode). 0.2.0 Alpha 2 redesigned all three: lighter,
cleaner surfaces with one step between background, surface, cards and
borders, quieter accents (each light and dark pair checked for contrast),
and the dark modes built with the same care (Liquid Glass cards one step
lighter than the surface, never darker). Text keeps WCAG contrast (7:1 on
surfaces, 4.5:1 for dimmed text, also on sidebars, menus and the panel);
the `sde-theme` test checks every family, mode and accent, and that the
neutrals stay grays (chroma at most 0.05). Accent colors of alpha16 to
0.2.0 Alpha 1 move to the new ones (`legacy.accents`).

Every family has soft corners, `radius_small` (buttons, fields, menu items,
list selections, tooltips), `radius` (windows, menus, cards) and
`radius_large` (panel popups: Start menu, Action Center, flyouts,
notifications, OSDs; message dialogs): Graphite 6/10/14, Fluent 4/8/8
(Windows 11: 4 px controls, 8 px windows and flyouts), Liquid Glass
10/16/24 (the softest, with capsule buttons). `sde::Theme` scales them with
the interface and has `radius_for(h)` for slim controls. They reach:

* SDE programs: SDE-FLTK menus, popups, tooltips and notifications are
  shaped (`sde::shape_rounded()`), and FLTK's stock up/down/thin boxes are
  soft rounded boxes (`sde::install_soft_boxes()`, called by
  `Theme::apply_to_fltk()`); scroll bars, switches and sliders are pills.
* Window frames: the compositor's `corner-radius` (also on the lite
  profile since alpha51).
* GTK 3/4 and libadwaita: `data/themes/overlays/soft-gtk-{3.0,4.0}.css`
  (every family) puts the SDE palette on the main surfaces and rounds
  windows, header bars, popovers, menus, buttons, fields, lists, tooltips,
  scroll bars, switches and sliders. SDE Graphite is compiled with the SDE
  palette; Flat Remix (SDE Fluent) is recolored onto it. Since 0.2.0
  Graphite's few accent-colored PNG assets (the sliders of scales with
  marks, the checked selection-mode check box) are moved to the SDE accent
  too (`recolor_graphite_assets()`: only their palettes change), and the
  rubber band (selection rectangle) is a 20 % accent sheet with a 1 px
  accent border in GTK 3 and GTK 4, also in icon views that draw it with
  the `.rubberband` class on the view itself (Liria, Nemo, Thunar, desktop
  icons), where the layer's view colour had made it an opaque sheet; and
  the layer no longer has a `:not()` with a compound selector, which
  GTK 3 rejected with a parsing warning in every application.
* Qt applications use the GTK palette (`QT_QPA_PLATFORMTHEME=gtk3`, set by
  `startsde` and `sde-session`).

Controls (0.2.0 Alpha 2) look the same in SDE programs and in GTK 3/4
applications of every family: switches with a filled track (gray while
off, the accent while on) and a light knob with a soft shadow, sliders with
a thin track and the same knob, and one keyboard focus ring, a solid 2 px
accent ring just inside the control (`focus_ring()` in
`src/common/widgets.cpp`; `@RADIUS_FOCUS@` and the `sde_control`,
`sde_trough` and `sde_knob` colors in the GTK layers). SDE's popups, menus,
tooltips and notifications have a hairline of light inside the top of
their edge (`Painter::surface_edge()`), Liquid Glass without its compositor
shader a glass sheen (`Painter::glass_surface()`). Menus and tooltips have
more room around their items, and menu separators are inset.

## Window buttons

Every family has the same caption buttons on the right, after Mint-Y:
minimize (a thin dash), maximize (a small square; two squares while
maximized) and close, a soft gray disc with the cross cut out. Hovered,
minimize and maximize get the same disc, close turns a muted red; there
are no traffic-light colors and no square hover layers. The hit area stays
large (95 % of the title height, about 30 x 32 px at 100 %).

* SDE WM: `<titleLayout>NLIMC</titleLayout>` (window icon, title, minimize,
  maximize, close). Configurations with the old left layouts of SDE (`CIML`,
  `CIMLN`) move to `NLIMC` the next time the session or the Control Center syncs the
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
  `spacing`, `glyph`: `soft` since alpha51, and `close_hover`, the red).
  The glyphs are 14 px masks (`soft_caption_masks()` in
  `tools/build-themes.py`), so they also fit stock Openbox's buttons; the
  colors come from `caption_colors()`, shared with GTK and `sde::Theme`
  (`caption_glyph`, `caption_disc`, `caption_close`, ...).
* Sharp at every scale (0.2.0 Alpha 2, audit A1-39): next to the 1-bit
  masks every theme ships 96 px antialiased 8-bit pictures of the same
  buttons (`close.png`, `max_hover_toggled.png`, ...; `caption_pictures()`
  draws them from signed distance fields). SDE WM's patch
  `suite/patches/sde-wm/0004-antialiased-caption-buttons.patch` tints each
  state's picture with its `image.color` and scales it with the button, so
  discs and glyphs stay smooth at 125, 150 and 200 % instead of the jagged
  scaled masks. It also adds `padding.width.percent` and
  `padding.height.percent` (52 % of the title font height in SDE themes), so
  the title bar keeps its proportions when SDE scales the title font. Stock
  Openbox ignores both and keeps using the masks.

## Transparency and clarity

Control Center > Appearance > Transparency and clarity has presets **Off** (0),
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
* Menus of other toolkits (Qt, Xlib) fade as a whole and keep their own
  readable opacity (`Theme::toolkit_menu_opacity`).
* Shadows fall a little downwards, as from a light above, also on the lite
  profile; window corners follow the family (`corner-radius`).

## SDE Liquid Glass (0.2.0 Alpha 2)

The Liquid Glass family recreates the look of
[Liquid Glass for GNOME Shell](https://github.com/ryohsuke1231/liquid-glass)
(see `docs/THIRD-PARTY.md`) on SDE's X11 desktop: clear glass that blurs,
saturates and brightens what is behind it, bends it along the edges like
the rim of a lens, a thin light rim where the edges face the light (top
left) and soft depth, on the panel, the Start menu, the Action Center,
flyouts, menus, tooltips, notifications and OSDs, with capsule shapes and
24 px corners on big surfaces.

How it is drawn (`Theme::liquid`: the family with [aero] enabled,
transparency above 0, blur on and SDE's OpenGL configuration of SDE
Compositor):

* SDE marks its surfaces with the `_SDE_SURFACE` window property
  (`x11::set_surface()`: `panel`, `panel-round`, `popup`, `popup-top`,
  `popup-bottom`, `menu`, `tooltip`) and draws them flat in their surface
  color, without sheen or rims.
* `write_compositor_config()` writes the Liquid Glass shader
  (`kLiquidShader` in `src/common/integration.cpp`, filled in from the
  theme: `~/.config/sde/liquid-glass{,-top,-bottom}.glsl`, one for each set
  of square corners) and binds it to those windows with
  `window-shader-fg-rule`. The same property gives them their exact corner
  radius (`corner-radius-rules`), while other docks, menus and tooltips
  stay square (`rounded-corners-exclude`).
* SDE Compositor's patch
  `suite/patches/sde-compositor/0001-window-shader-backdrop.patch` lets a
  window shader read what is behind its window (`backdrop_at()`, already
  blurred) and redraws such a window as a whole when anything under it
  changes. picom alone cannot bend the backdrop.
* The shader turns the surface color into glass and keeps everything else
  opaque (text, icons, highlights, and colors beyond the surface color such
  as white glyphs and knobs); SDE's fills on the glass (hover tints, cards,
  borders) are glass with their difference on top. The window opacity
  (`Theme::opacity`, `popup_opacity`) is how much of the surface color
  tints the glass: 41 % at Subtle (25) down to 20 % at 100.
* Text stays readable over any backdrop: under text the glass is lifted (in
  dark mode deepened) just enough for 4.5:1 text and 3:1 dimmed text
  (`liquid_legible_luminance()`; the `sde-theme` test checks every accent
  and mode), so the glass is clear over calm backdrops and frosted over
  busy or contrasting ones.
* The material is in `palettes.json` (`glass.liquid`): blur strength,
  backdrop saturation and brightness, the lens band and shift, the rim and
  its width, and `key_range` (how far a color may be from the surface to
  count as glass).
* GTK 3/4 applications get capsule buttons, a light rim on menus, popovers
  and tooltips (`glass-gtk-{3.0,4.0}.css`) and the translucent GTK menus of
  every family.

Limits: application windows are not turned into glass (their frames are
translucent and blurred as before). The glass needs OpenGL: with XRender,
the lite profile or a `compositor.conf` of your own, SDE draws its own
frosted glass with a sheen and light rims instead, as SDE Glass did. The
lens bends the blurred backdrop; it is not a physical refraction. Pictures
inside SDE's surfaces that use the surface color itself (the light clouds
of the weather card) let a little glass through.

## Screenshots (`tools/showcase.py`)

`tools/showcase.py --theme glass-light,glass-dark --effects full` captures
with SDE Compositor and SDE's full OpenGL configuration (Liquid Glass);
`--resolution 1024x600 --scale 150` captures the selected sets at another
screen size and interface scale, and `--scope full` captures every scene in
every set. `sde-session --compositor-config` prints the configuration the
session would give the compositor.

## Icons

Control Center > Appearance > Icons shows a card for every installed icon theme
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
  desktop icons, Start menu, panel and tray fallbacks, the Control Center, Gallery,
  Taca and the others. Programs reload it when the configuration serial
  changes, so a new choice applies at once.
* Lookup follows the Icon Theme Specification: the theme, its `Inherits=`
  parents depth first, then Adwaita, AdwaitaLegacy and hicolor, then
  `/usr/share/pixmaps`. Scaled directories (`Scale=2`) count with their
  pixel size. `tests/fixtures/icon-themes` exercises this.
