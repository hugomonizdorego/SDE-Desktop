# SDE themes, transparency and icons (0.2.0 Alpha 4)

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

## How the colours are chosen (0.2.0 Alpha 4)

The rules every palette follows; `sde-theme` checks them for every family,
mode and accent, with and without the wallpaper's tint.

* **Lightness carries the structure, not colour.** Surfaces are grays with
  at most a faint cast (Graphite neutral, Fluent and Liquid Glass a cool
  slate): Oklab chroma at most about 0.02, at most 0.05 between the sRGB
  channels (the test's limit). Background, surface, sidebar
  (`surface_alt`), hover and border differ in lightness only, in steps of
  2 to 5 Oklab L. Oklab is perceptually even, so equal steps look equal:
  none is a jump, none disappears.
* **Raised means lighter.** In a light theme menus and cards are the
  lightest surfaces (near white over a slightly darker window) and soft
  shadows under them give the depth. In a dark theme what is raised is
  lighter than what is under it (menus and cards over the window, the
  Start menu's tiles over the menu), never darker, and shadows are
  stronger (alpha 0.44-0.50): a dark surface needs more to stand apart.
* **Text keeps WCAG AA with a margin:** text 7:1 on every surface (11 to
  16:1 in practice), dimmed text 4.5:1 also on sidebars, menus, the panel
  and hovered rows, titles 7:1. Disabled text is the dimmed text; disabled
  controls fade 55 % towards the sidebar colour.
* **The accent is a signal, not a surface.** Muted accents (Oklab chroma
  0.08 to 0.16), one shade per mode: in a light theme at Oklab L 50 to 70
  with a white or a dark label, whichever keeps 4.5:1; in a dark theme
  lighter (L 70 to 80) with dark labels, as in Windows 11, since a
  mid-tone accent on a dark surface is either too dark to see or too
  saturated to read. Accent-coloured text is `accent_text`, moved towards
  the text colour until it keeps 4.5:1. Accent fills are for selections,
  switches that are on, focus rings and small marks; larger areas take
  only a hint of the accent's hue at a low Oklab chroma at the same
  luminance (`tint_towards()`), because a warm hue at a low lightness is
  brown or olive. Since 0.2.0 Alpha 4 the Start menu's tiles in a dark
  theme are such raised grays with a hint of the accent (they were 16 % of
  the accent in the sidebar gray: the menu's own lightness, brown or olive
  for the warm accents), and the Action Center's multicolour tiles
  (`[action_center] multicolor`) are the family's own accents in the
  mode's shade (they were Tailwind colours about twice as saturated, and
  the tiles that were off took a muddy tint of them); a tile that is off
  is the plain tile colour with its icon in its colour.
* **Hover and selection:** hover is one lightness step from the surface it
  is on; selection is the accent with its label colour. Menus highlight
  with the accent in Graphite and Liquid Glass, with the hover gray in
  Fluent (Windows 11).

| Theme (Oklab L) | background | surface | surface_alt | hover | border | menu | text on surface | dimmed on surface_alt |
|---|---|---|---|---|---|---|---|---|
| Graphite light | 97 | 100 | 95 | 93 | 89 | 100 | 16.4:1 | 5.9:1 |
| Graphite dark | 27 | 31 | 25 | 35 | 38 | 29 | 11.3:1 | 6.8:1 |
| Fluent light | 97 | 99 | 95 | 92 | 88 | 98 | 16.3:1 | 5.7:1 |
| Fluent dark | 25 | 30 | 23 | 34 | 37 | 29 | 12.0:1 | 6.9:1 |
| Liquid Glass light | 97 | 98 | 94 | 91 | 86 | 98 | 16.2:1 | 5.8:1 |
| Liquid Glass dark | 23 | 28 | 31 | 34 | 36 | 28 | 13.2:1 | 5.6:1 |

## Menus, lists, check boxes (0.2.0 Alpha 3.1)

* **The highlight moves at once.** Graphite's Material transitions faded
  menu items and rows in and out (and SDE's soft layers added a 120 ms
  fade), so two or three items of a menu were lit at once while the
  pointer moved and menus felt late. Every SDE GTK 3 and GTK 4 theme and
  the libadwaita layer now have no transition or animation on menu items,
  popover menu buttons, menu bar items, combo box lists, list and tree
  rows, icon views and flow box children: `tools/build-themes.py` sets
  them to `none` in the upstream rules (`still_highlights()`) and the last
  layer, `data/themes/overlays/motion-gtk-{3.0,4.0}.css`, keeps them off
  in every state.
* **Menus appear at once.** With the full effects the compositor fades
  windows in and out (about 100 ms), but not menus: GTK's popup, dropdown
  and combo menus and SDE-FLTK's menus and tooltips are in the generated
  `fade-exclude` (picom 12 ignores `fade` in `wintypes`).
* **No tree view rule by row position.** A selector such as
  `treeview.view:nth-child(even)` makes GTK 3 style every cell by its row
  and cache none of them (Liria's list view and sidebar spent a fifth to a
  half more CPU on one such rule for SDE System Monitor's old GTK tables).
* **Check boxes are rounded squares, radio buttons circles,** in windows,
  dialogs and menus. Graphite drew GTK 3 check boxes as circles (its
  `checkbox-*-symbolic.svg` assets); SDE Graphite and SDE Liquid Glass
  ship rounded squares instead (`check_box_svgs()`). SDE-FLTK's check
  buttons (the file chooser, stock dialogs) get a rounded square with
  corners of a fifth of its side (`stock_radius()` in
  `src/common/menu.cpp`); 4 px corners had made a 13 px box a circle.
* `sde-theme-motion` (`tests/test_theme_motion.py`) checks the style
  sheets and indicators: no rule of a built style sheet that styles those
  widgets may have a transition or animation that takes time, no tree view
  selector may use `:nth-child`, and the indicators drawn by GTK 3, GTK 4
  and SDE-FLTK on Xvfb must fill at least 87 % (check box) or at most 84 %
  (radio button) of their box. `sde-tests` checks the menus in the
  compositor's `fade-exclude`.

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
blur_strength=6     # 1 (light) to 12 (strong), around the family's own blur
```

Since 0.2.0 Alpha 4 the sliders show their value at the right of the
track while they move (`Slider::show_value()` in
`src/common/widgets.cpp`): Transparency 0 to 100 %, Blur strength its
share of the strongest (6 of 12, the default, is 50 %), the Preview
window's Transparency slider the same as the page's. Their accessible
names, the tooltips that SDE's QA files and the Control Center's focus
keeping use, carry the value ("Transparency level: 25 %"). The arrow keys
and the wheel move a slider by a twentieth of its range: 5 % of the
transparency, 1 of the blur strength (it moved by 5 of 12).

Since 0.2.0 Alpha 3.1 (`apply_transparency()` in `src/common/theme.cpp`):

* **Even steps.** The slider is linear: Off, Subtle, Clear and Glass are
  even steps from opaque to the most each surface can be at 100 (0.2.0
  Alpha 2 had a square-root curve: Subtle already went half the way and
  the last steps hardly changed anything).
* **Only the floating layer is translucent.** The panel, SDE menus, the
  Start menu, the Action Center, notifications, OSDs and tooltips, GTK 3
  menus and the frames (title bars) of windows. The content of windows
  (the Control Center's pages, lists, documents) stays opaque.
* **Readable over anything.** A surface faded as a whole (Graphite and
  Fluent, other toolkits' menus, frames) never goes below what keeps text
  5:1 and dimmed text 3.8:1 over any flat backdrop
  (`readable_opacity()`), a margin over WCAG's 4.5:1 and 3:1 for what a
  blur leaves of a busy page; the family's own floor at 100 is
  1 - 2 x its `translucency` (Graphite 80 %, Fluent 72 %). Frames: Liquid
  Glass 72 %, Fluent 86 %, Graphite 90 % at 100.
* **Almost opaque without blur.** Without the OpenGL compositor's blur
  (the lite profile: `[compositor] effects=lite`, or `auto` in virtual
  machines and with software rendering, also on a GPU whose OpenGL renders
  in software, which `sde-session` checks at login with `sde-gl-probe` and
  writes to `session.log`; `[aero] blur=false`) a busy window
  behind would show through sharp: surfaces keep at least 97.5 % (XRender's
  small CPU blur 88 %) and frames are opaque. SDE then draws its glass
  with a sheen and light rims (`Painter::glass_surface()`).
* **Blur.** dual_kawase strength 9 for Liquid Glass and 7 for the other
  families with the default `blur_strength` (0.2.0 Alpha 2: 5 and 6), so
  a busy page behind turns into soft colour.
* **GTK menus** are opaque in CSS (`Theme::gtk_menu_alpha` is 1): SDE WM
  does not offer GTK the `_GTK_FRAME_EXTENTS` client-side shadows, so GTK 3
  menus are plain X windows, where the `.csd menu` rules of 0.2.0 Alpha 2
  never applied, and a translucent GTK 4 popover showed what is behind its
  shadow margin. The compositor rounds GTK 3 menus (`corner-radius-rules`),
  makes them glass in Liquid Glass and fades them like SDE's menus in the
  other families (`opacity-rule`, `Theme::toolkit_menu_opacity`).
* **One shadow.** Popups with an alpha channel and a shadow of their own
  (GTK 4 popovers and tooltips, browser menus) get no second, square
  shadow from the compositor and no blur behind their transparent margin
  (`override_redirect && argb && !_SDE_SURFACE` in `shadow-exclude` and
  `blur-background-exclude`): that was a hazy ghost frame around GTK 4
  menus.
* Shadows fall a little downwards, as from a light above, also on the lite
  profile; window corners follow the family (`corner-radius`).
* `sde-glass-busy-backdrop` (`tests/test_glass.cpp`) draws a busy,
  high-contrast web page (and a dark one), puts every translucent surface
  on it the way the compositor draws it with the configuration SDE
  generates (the Liquid Glass shader's frost, saturation, tint and
  legibility lift with the constants of the generated shader; the faded
  surfaces blended over the blurred or sharp page) and measures, for every
  family, mode, accent, transparency and compositor profile, text 4.5:1
  and dimmed text 3:1 against the surface around it, how much of the
  page's text and edges a surface keeps (at most 3 L*), even steps and
  almost opaque surfaces without blur. It measures text at least 5.8:1,
  dimmed text 3.1:1 and at most 2.6 L* of structure. Since 0.2.0 Alpha 4
  it takes the glass colour from the shader SDE writes (`KEY_A`), measures
  the panel with the colour it paints (the floating layer's surface; its
  opacity floor covers that colour and `panel_bg`), and measures every
  family and mode once more with the tint of a blue-green and of a red
  wallpaper (text at least 6.0:1, dimmed text 3.1:1).

## Wallpaper colours (0.2.0 Alpha 4)

The floating layer takes a calm tint of the wallpaper's colour: the panel,
the Start menu, the Action Center, flyouts and task previews, menus,
tooltips, notifications and OSDs lean towards the wallpaper's main hue, so
the transparency follows the wallpaper even over a white window, and the
surfaces keep the hue when they are opaque. It is in the colour itself,
as Windows 11's Mica is: it shows without blur, on the lite profile and
without a compositor, and costs nothing while drawing.

Control Center > Appearance:

* **Tint surfaces with the wallpaper color** (Transparency and clarity):
  on by default in Fluent and Liquid Glass, off in Graphite (calm and
  neutral by design). Once used, the switch holds for every family.
* **Accent color from the wallpaper** (Theme): the named accent nearest
  the wallpaper's hue, Graphite for a gray wallpaper. It is always a named
  accent, so the GTK themes and the window frames, built per accent,
  follow it as well. Choosing a swatch turns it off.

```ini
[appearance]
wallpaper_tint=auto          # auto (the family's default), true or false
accent_from_wallpaper=false
```

Both are part of the chosen look: Preview shows them, Apply applies them.
The family's default is in `palettes.json` (`wallpaper_tint`: `default`
and `amount`, the most Oklab chroma the tint may add in light and dark
mode: Fluent and Liquid Glass 0.020 and 0.022, Graphite 0.014 and 0.016).

**Measuring** (`src/common/wallpaper_colors.h`). The picture is read small
(SVG through librsvg, everything else through gdk-pixbuf, which lets
libjpeg decode at an eighth of the size) and its pixels are taken to
Oklab: the mean colour, and the most common hue, from a histogram of hues
weighted by chroma in which near-black and near-white pixels count little
(their hue is noise), with the mean colour of the pixels within 30 degrees
of it. A *strength* from 0 to 1 says how much that hue may tint: 0 for
gray, black and white pictures, less for a picture with little colour and
for one of many hues at once, 1 for one clear colour. Measured: the
Selviana light wallpaper teal (hue 217 degrees, strength 0.80), the dark
one blue (234 degrees, 0.44), a blue-green sky 0.93, a forest 0.82, a red
1.0, a sunset over the sea 0.18, a rainbow 0.41, gray, black and white 0.

**Tinting** (`tint_surface()`). A colour moves towards that hue by at most
the family's amount of Oklab chroma times the strength, at the same
relative luminance (only white, which has no room for a hue, may lose up
to 4.5 %), and stays a tinted gray (at most 0.045 between its sRGB
channels). Every contrast with text and every step between surface,
hover and border therefore stays what the palette has. `menu_bg`,
`menu_hover` (Fluent's), `menu_border`, `menu_separator` and `panel_bg`
have the tint in every program; `layer_surface`, `layer_surface_alt`,
`layer_hover` and `layer_border` are the floating layer's surface,
sidebar, hover and hairline, which `Theme::use_floating_layer()` puts in
place in the programs that draw only that layer: the panel,
`sde-notifyd` and the OSDs. Hairlines get three quarters of the tint;
text, the accent and accent highlights none. The Liquid Glass shader keys
the tinted colours (`KEY_A`, `KEY_B`), and GTK 3 menus, which the GTK
themes draw in the untinted menu colour (`Theme::gtk_menu_bg`, `KEY_C`),
become the same tinted glass. Appearance's miniature and Preview window
draw their panel and quick settings in the floating layer's colours.

**When it changes.** `Theme::from_config()` looks the colours up for the
picture of `wallpaper_in_use()` ([appearance] wallpaper, or for `auto` the
Selviana wallpaper of the mode), so a new picture reaches every program
with the configuration change that brings it: the Control Center's
Wallpaper card, Gallery's and Photo's Set as Wallpaper, the switch to dark
mode with the automatic wallpaper, and a slideshow, which sets
[appearance] wallpaper and announces the change like they do. The session
then rewrites the compositor's shaders with the new keys (reloaded in
place). The colours are cached per picture (path, size and modification
time, so a picture replaced under the same name is measured again) in
`~/.cache/sde/wallpaper-colors`, under a lock: of the programs that follow
one change, the first measures the picture and the others read its entry.
Gallery and Photo measure it before they announce the change
(`prepare_wallpaper_colors()`), the Control Center when it themes itself,
also before.

**For other programs**: `wallpaper_colors_of_pixels()` measures pixels
already in memory and `wallpaper_colors(path)` a file (cached); both
return the average and dominant colours, the overall chroma, the hue, its
share and the strength, and `wallpaper_accent()` the nearest named accent.

Limits: the content of windows keeps the theme's colours, in SDE programs
as in GTK and libadwaita applications (GTK reads the theme's colours when
it starts; a tint that changes with the wallpaper would leave running
applications with the old one). Window frames take the wallpaper through
their blur, not as a tint, and GTK 3 menus outside Liquid Glass fade over
what is behind them. A picture edited in place is measured again at the
next configuration change.

`sde-theme` measures sample wallpapers drawn with cairo and the Selviana
pictures (hue, strength, accent, the cache), the tint (the same luminance,
a tinted gray, the wallpaper's hue) and the floating layer of every
family, mode and accent with a sky and a red wallpaper (text 7:1, dimmed
text 4.5:1, contrast within 5 % of the untinted palette, readable at
transparency 100), and that the shader keys the tinted colours.

## SDE Liquid Glass (0.2.0 Alpha 2, frosted since 0.2.0 Alpha 3.1)

The Liquid Glass family follows Apple's Liquid Glass (macOS 26) and
[Liquid Glass for GNOME Shell](https://github.com/ryohsuke1231/liquid-glass)
(see `docs/THIRD-PARTY.md`) on SDE's X11 desktop: frosted glass that blurs,
saturates and brightens what is behind it into soft colour, frosted with
the surface colour, bends it a little along the edges like the rim of a
lens, a thin specular rim where the edges face the light (top left; a
fainter one opposite) and soft depth, on the panel, the Start menu, the
Action Center, flyouts, menus (SDE's and GTK 3's), tooltips, notifications
and OSDs, with capsule shapes and 24 px corners on big surfaces. 0.2.0
Alpha 2 had a clear glass (a light blur and 14-32 % of the surface colour)
that left a busy window behind almost as it was.

How it is drawn (`Theme::liquid`: the family with [aero] enabled,
transparency above 0, blur on and SDE's OpenGL configuration of SDE
Compositor):

* SDE marks its surfaces with the `_SDE_SURFACE` window property
  (`x11::set_surface()`: `panel`, `panel-round`, `popup`, `popup-top`,
  `popup-bottom`, `menu`, `tooltip`) and draws them flat in their surface
  color, without sheen or rims; the SDE GTK themes draw GTK 3 menus flat
  in the menu colour.
* `write_compositor_config()` writes the Liquid Glass shader
  (`kLiquidShader` in `src/common/integration.cpp`, filled in from the
  theme: `~/.config/sde/liquid-glass{,-top,-bottom}.glsl`, one for each set
  of square corners) and binds it to those windows and to GTK 3 menus
  (popup and drop-down menus without an alpha channel) with
  `window-shader-fg-rule`. The same rules give them their corner radius
  (`corner-radius-rules`), while other docks and tooltips stay square and
  menus with an alpha channel draw their own corners
  (`rounded-corners-exclude`).
* SDE Compositor's patch
  `suite/patches/sde-compositor/0001-window-shader-backdrop.patch` lets a
  window shader read what is behind its window (`backdrop_at()`, already
  blurred) and redraws such a window as a whole when anything under it
  changes. picom alone cannot bend the backdrop.
* Patch `0002-rebind-damaged-windows.patch` (0.2.0 Alpha 3.1): Mesa's
  software OpenGL (Xvfb, virtual machines without 3D, a machine without
  its graphics driver) copies a window into the compositor's texture when
  the compositor binds it, and picom 12 bound a window only when it was
  mapped or resized. A window then kept what it had at that moment: a
  window resized while its program was busy stayed at its old size, the
  rest see-through, and what a program drew a moment after mapping did not
  show. On a renderer that is not accelerated the compositor now binds a
  damaged window again before the next frame (accelerated drivers are not
  affected; `glx-no-rebind-pixmap = true` turns it off).
  `sde-compositor-redraw` (`tests/test_compositor_redraw.py`) checks it on
  Xvfb with the GLX backend.
* The shader frosts the compositor's blur further with a disc of twelve
  samples (`frost`, 12 px at 100 %), saturates (1.7) and brightens the
  backdrop, turns the surface color into glass and keeps everything else
  opaque (text, icons, highlights, and colors beyond the surface color such
  as white glyphs and knobs); SDE's fills on the glass (hover tints, cards,
  borders) are glass with their difference on top. The window opacity
  (`Theme::opacity`, `popup_opacity`) is how much of the surface color
  frosts the glass, linear with the slider from 1 down to the material's
  `tint` at 100 (36 %, dark 34 %): 84 % at Subtle, 68 % at Clear, 49 % at
  Glass. A dither of one 8-bit step keeps the smooth glass free of bands.
* Text stays readable over any backdrop: under text the glass is lifted (in
  dark mode deepened) just enough for 4.5:1 text, 3:1 dimmed text and, as
  far as the glass can stay clear, 3:1 accent-colored text
  (`liquid_legible_luminance()`; the `sde-theme` and
  `sde-glass-busy-backdrop` tests check every accent and mode).
* The material is in `palettes.json` (`glass.liquid`): the compositor's
  blur strength and the shader's frost, backdrop saturation and
  brightness, the tint at 100, the lens band and shift, the rim and its
  width, and `key_range` (how far a color may be from the surface to
  count as glass).
* GTK 3/4 applications get capsule buttons, a light rim on popovers and
  tooltips (`glass-gtk-{3.0,4.0}.css`) and glass GTK 3 menus.

Limits: application windows are not turned into glass (their frames are
translucent and blurred). The glass needs OpenGL: with XRender, the lite
profile or a `compositor.conf` of your own, SDE draws its own frosted glass
with a sheen and light rims instead, almost opaque without blur. The lens
bends the blurred backdrop; it is not a physical refraction. Pictures
inside SDE's surfaces that use the surface color itself (the light clouds
of the weather card) let a little glass through.

## Screenshots (`tools/showcase.py`)

The Liquid Glass sets (`glass-light`, `glass-dark`) capture with SDE
Compositor and SDE's full OpenGL configuration (the glass shader through
Mesa on Xvfb, which needs compositor patch 0002 to show what windows draw
after they were mapped or resized; the lite profile when the X server has
no GLX), the other sets with the lite profile; `--effects lite|full` forces one profile for
every set. `--resolution 1024x600 --scale 150` captures the selected sets
at another screen size and interface scale, and `--scope full` captures
every scene in every set. `sde-session --compositor-config` prints the
configuration the session would give the compositor.

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

## Switching themes (0.2.0 Alpha 3.1)

A new look (Control Center > Appearance > Apply, the panel's dark mode,
`sde-session --apply-appearance`, the automatic mode at 07:00 and 19:00)
reaches every program in one calm step.

**Who tells whom.** Everything derived from `sde.conf` is written first
(`write_appearance_files()` in `src/common/integration.cpp`: GTK
`settings.ini` and the managed `gtk.css` block, the GSettings keys in one
change, `~/.config/sde/xsettingsd.conf`, SDE WM's `rc.xml`, the compositor
configurations and their shaders). Then the session is the only one that
tells its programs, each once and only when its input changed since it last
read it: `sde-xsettingsd` gets SIGHUP (it publishes nothing when the file did
not change), SDE WM gets its `_OB_CONTROL` reconfigure message, the
compositor gets SIGUSR1, and SDE programs follow `_SDE_CONFIG_SERIAL`, which
the session bumps once. An accent changes GTK and the window frames only:
the compositor's configuration stays the same and it is not told. Without a
session, programs use `deliver_appearance()`, which finds this display's
daemons by their selections (`_XSETTINGS_S<n>`, `_NET_WM_CM_S<n>`,
`_NET_WM_PID`). Nothing runs `pkill` any more: up to 0.2.0 Alpha 3 the
Control Center and the session signalled every compositor, xsettingsd and
session of the user, also those of other displays.

**The compositor reloads in place.** SDE Compositor's patch
`suite/patches/sde-compositor/0003-live-configuration-reload.patch` makes
SIGUSR1 and changes of its configuration file reload the configuration
30 ms later (so the signal and the file watch give one reload) without
resetting picom: the windows, the redirection and the backend stay; the
options, window rules, shaders (kept compiled when unchanged), blur and
shadows (made again only when they changed) are replaced and the next frame
repaints the screen once. Upstream picom resets the whole session instead,
which showed the screen without compositing for a moment (a blank panel)
or, with OpenGL, froze it while the backend started again. Options the
running backend cannot follow (backend, vsync, damage, unredirection,
fading, custom animations, `rules`) still reset it. A file written again
with the same text does nothing; SIGUSR1 with nothing changed (sent by
scripts, and by `tools/showcase.py` after a resize with software OpenGL,
to have the screen drawn again, which upstream's reset did) gives every
window its image again and repaints the whole screen, also without a
reset.

**One calm step.** A switch asked for with `_SDE_APPEARANCE_APPLY` (a
client message to the owner of `_SDE_SESSION_S<screen>`, see
`request_appearance_apply()`) happens behind a picture of the screen: the
session copies the screen into an override-redirect window (input passes
through it; just below opaque, so the compositor keeps compositing; excluded
from fading by the generated configuration, `_SDE_CURTAIN`), publishes the
new look behind it, holds it until the windows have redrawn (XDamage: no
window repainted at least half of itself for 0.18 s, at least 0.25 s, at most
1.6 s; GTK 3 restyles in two visible steps 0.2 to 0.9 s apart, which nothing
outside GTK can merge) and fades it out in 0.22 s (without a compositor or
with animations off it simply goes). `_SDE_APPEARANCE_DONE` on the session's
window then holds the request's number. `SDE_THEME_SWITCH=instant` switches
without the picture, `SDE_THEME_SWITCH_DEBUG=1` logs the redraws and the
compositor lines that changed.

**GTK.** GTK reads `~/.config/gtk-3.0/gtk.css` and `gtk-4.0/gtk.css` only
when an application starts, while the theme itself reaches running
applications over XSETTINGS. The managed block therefore holds no color
copied from the theme (translucent GTK menus would name `@sde_menu`; since
0.2.0 Alpha 3.1 the compositor makes GTK 3 menus translucent instead, so a
new transparency level reaches them at once) and defines the accent only
for a color of your own: after a switch from light to dark, running GTK 3
applications had kept the old light menu background under the new light
text. A custom accent reaches running GTK applications when they start
again; libadwaita applications get the dark preference and the accent at
once (GSettings, the portal) and the theme's libadwaita colors when they
start again.

Measured on Xvfb (1366x768, sde-session with SDE WM, SDE Compositor, panel,
desktop, gtk3-widget-factory, Calculator and the Control Center; 30 fps
captures with ffmpeg x11grab, `SDE_GRAPHICS=hardware`): switching from SDE
Graphite to SDE Fluent took seven separate visible changes over 1.1 s with
XRender, with a frame of blank panel during the compositor's reset and up to
1.9 % of the screen showing neither the old nor the new look; with Mesa's
software OpenGL the compositor was reset twice and the screen froze 2.7 s,
then jumped. Now the screen stays as it was until it fades to the new look
(with XRender at most 0.01 % of the screen shows anything else, with
software OpenGL the switch is a single frame), the compositor keeps its
process, and an accent change does not touch it.
