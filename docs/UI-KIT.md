# SDE UI kit: menus, stock widgets and the About window

Alpha 16. Everything here lives in `sde-common` (`src/common/menu.h`,
`src/common/about.h`) and in SDE-FLTK (`sde-fltk/FL/Fl_SDE_Style.H`).

## What every program gets for free

Linking `sde-common` and drawing with `sde::Painter` (every SDE program does)
installs the SDE style at startup (`sde::install_ui_style()`, called from
`paint.cpp`). From then on SDE-FLTK draws, in the active theme:

| FLTK part | Look |
|---|---|
| Popup menus: `Fl_Menu_Item::popup()`, `Fl_Menu_Button`, right-click menus | rounded surface, 1 px border, padding, accent pill on the highlighted item, separators, right-aligned shortcuts, submenu chevrons, check marks and radio dots, optional icons, quick-action row |
| `Fl_Menu_Bar` | flat bar in the widget's `color()`, hover pill, open title with accent tint and underline |
| `Fl_Choice` | rounded field with a chevron; its list is a themed popup aligned on the field |
| `Fl_Scrollbar` (browsers, `Fl_Scroll`, text editors) | thin rounded thumb, wider with a track while hovered; no arrow buttons |
| Tooltips | theme surface and text colors, larger margins (unless the program set `Fl_Tooltip::color()`) |
| `fl_message`, `fl_alert`, `fl_ask`, `fl_choice`, `fl_input`, `fl_password`, `Fl_File_Chooser` | themed window, rounded buttons, accent default button, rounded fields, round accent icon |
| Text fields (`Fl_Input_`) | right click opens an SDE context menu: Cut, Copy, Paste, Delete, Undo, Select All |

Colors, accent, dark mode and scale come from `sde::active_theme()` (set by
`Theme::apply_to_fltk()`); programs that never call it (the panel) get the
configured theme, re-read each time a menu opens. Changing the theme in
the Control Center therefore applies to the next menu without a restart.
Keyboard navigation is FLTK's: arrows, Tab, Enter, Escape, `&` mnemonics
(underlined while Alt is held), shortcuts; Left/Right move along the quick
row, Down leaves it.

Popups are shaped with rounded corners (XShape); the compositor draws the
shadow along the shape and applies the theme's popup opacity under Aero and
Liquid Glass. A popup that would leave the work area is moved up into it
(dropdowns open above their widget when there is room).

Since alpha51 nothing is square: popups, tooltips and notifications are
shaped with the theme's `radius` (`sde::shape_rounded(window, r)`), and
`Theme::apply_to_fltk()` replaces FLTK's `FL_UP_BOX`, `FL_DOWN_BOX`, the thin
boxes and their frames with soft rounded ones (`sde::install_soft_boxes()`;
`radius_small`, or 4 px for boxes under 20 px such as check buttons). Use
`theme.radius_for(h)` for controls you draw yourself.

Opt out of the text field menu with `sde::set_input_context_menus(false)`.

## ContextMenu

```cpp
#include "menu.h"

sde::ContextMenu menu;
sde::EditActions edit;                // empty functions are left out
edit.cut = [&] { cut(); };
edit.copy = [&] { copy(); };
edit.paste = [&] { paste(); };
edit.del = [&] { remove(); };
edit.select_all = [&] { select_all(); };
edit.can_paste = clipboard_has_text;  // false: shown disabled
menu.edit_actions(edit);              // quick row + "Select All  Ctrl+A"
menu.quick(_("Rename"), "rename", [&] { rename(); });
menu.quick(_("Share"), "share", [&] { share(); });

menu.item(_("Open"), [&] { open(); }, {"open", FL_CTRL + 'o'});
menu.item(_("Open With..."), [&] { open_with(); });
menu.separator();                     // line below the last item
menu.check(_("Show Hidden Files"), hidden, [&] { toggle_hidden(); });

sde::ContextMenu sort;
sort.radio(_("Name"), by == 0, [&] { sort_by(0); });
sort.radio(_("Date"), by == 1, [&] { sort_by(1); });
menu.submenu(_("Sort By"), std::move(sort));

menu.popup();                         // at the pointer, runs the action
```

* `item(label, action, MenuItemOptions{icon, shortcut, enabled, divider})`
* `quick(label, icon, action, enabled)`: icon buttons in a row at the top
  (Windows 11 style); the label is the accessible name. Quick items always
  go first.
* `check()`, `radio()`, `submenu()`, `separator()`, `empty()`
* `popup()`, `popup(x, y)` (relative to the window of the last event),
  `popup_below(widget)`; they return true when an item was chosen.
* `items()` returns the FLTK array, e.g. for `Fl_Menu_Button::menu()`.

Labels are taken literally (`&` is escaped). Shortcuts are only displayed;
handle the keys in the window as usual.

### Icons

Icon names are the built-in line glyphs `cut copy paste delete select-all
undo redo rename share open new save print find settings info refresh
properties trash folder image link close` (the freedesktop names such as
`edit-copy` map to them too) or any freedesktop icon name, loaded from the
icon theme. Plain Fl_Menu_Item arrays and `Fl_Menu_::add()` menus can have
icons too:

```cpp
sde::set_menu_icon(&items[3], "properties");      // by address
sde::set_menu_icon_for_label(_("Copy"), "copy");  // by label (add() copies items)
```

and the `FL_MENU_SDE_QUICK` flag (`FL/Fl_SDE_Style.H`) puts leading items in
the quick row. Without a style (e.g. the system FLTK) the flag is ignored.

## Stock widgets in your own windows

`sde::apply_stock_style(window)` gives the stock buttons, inputs, browsers,
check buttons and choices of a window the same look as the stock dialogs.
Custom widgets keep drawing themselves (`sde::FlatButton`, `sde::Switch` ...).

## Copy and move progress (TransferDialog)

Since 0.2.0 Alpha 2 `sde::TransferDialog` (`src/common/transfer_dialog.h`)
shows a copy or move like Windows 10 does: the SDE mark, whose red, green
and blue stripes carry a pulse of light from left to right while the
operation runs (about 30 frames per second, only the mark is redrawn),
"Copying 1,234 items from Documents to Backup", "42% complete" with Pause
and Cancel, a slim progress bar, and with More details a speed graph (the
part done tinted, the speed along the progress as a filled area, a line at
the current speed with "Speed: 85.3 MB/s"), the item being copied, the time
remaining ("About 2 minutes") and the items remaining ("120 (1.2 GB)"). The
window title is the percentage; closing the window cancels. Paused, the mark
stands still, the bar and graph turn amber and the text says "Paused - 42%
complete"; the mark also stands still with reduced motion (`[aero]
animations=false` or `[appearance] low_resource=true` in sde.conf, or
`SDE_REDUCED_MOTION=1`). The desktop uses it for drops and Paste
(`src/desktop/main.cpp`, `FileJob`).

```cpp
#include "transfer_dialog.h"

auto *dialog = new sde::TransferDialog(sde::TransferDialog::Operation::Copy, "Documents", "Backup");
dialog->on_pause = [job](bool paused) { job->pause = paused; };
dialog->on_cancel = [job] { job->cancel = true; };
dialog->show();
// A few times a second, from the FLTK thread:
sde::TransferState state;
state.bytes_done = job->done;   state.bytes_total = job->total;  // 0: still counting
state.files_done = job->items;  state.files_total = job->all_items;
state.current = "holiday.mov";  state.finishing = job->syncing;  // "Finishing..."
dialog->update(state);
```

The speed (smoothed over about 2 s), the time remaining (the average and the
current speed together, so it neither jumps nor lags; paused time does not
count), the graph and the mark's animation come from
`src/common/transfer_model.h`, plain C that Liria's file operations window
uses too (copied into Liria as `liria-sde-transfer-model.h`), so both look
and estimate the same. `build/tests/sde-transfer-demo [--details]
[--paused] [--move] [--at 0.4]` shows the dialog with a simulated copy;
`sde-transfer-model` (ctest) tests the model and the texts and
`tests/test_transfer_dialog.py` the dialog on Xvfb (animation, Pause,
details, reduced motion, Cancel).

## About window, legal data and developer identity

Every SDE program is part of the **Selviana Desktop Environment** and names
the **Selviana DE Project** as its developer (since 0.2.0 Alpha 1; see
[LEGAL.md](LEGAL.md)).

```cpp
#include "about.h"

sde::AppInfo info;
info.name = _("Gallery");
info.program = "sde-gallery";       // also its entry in data/legal/components.json
info.version = SDE_VERSION;
info.icon = "sde-gallery";          // official icon; icon_fallbacks: older names
info.logo = draw_app_icon;          // optional: drawn when the icon is not installed
info.description = _("Browse your photo library.");
info.credits = {sde::toolkit_credit(), _("Camera details read with libexif")};
sde::show_about(info);
```

License, copyright lines, upstream projects and trademark notices come from
the legal data, `data/legal/components.json` (compiled in as
`generated/legal_data.h` by `tools/legal-data.py`, read through
`src/common/legal.h`). `AppInfo::license`, `copyright`, `upstream` and
`trademarks` override or extend it; `component` names another entry than
`program`. The window has four sections:

| Section | Content |
| --- | --- |
| About | icon, name, version, description, "Part of Selviana Desktop Environment" with the SDE mark, developer and website (button: Website) |
| Credits | developer, upstream projects (relation, copyright, license, address), acknowledgements, the EDE inspiration note |
| License | full license name and SPDX id, the standard notice with its warranty disclaimer, other licenses of a compound expression (button: View License, Alt+V: the full texts in a scrollable viewer) |
| Legal | copyright lines (the program's and its upstream projects') and the trademark notices: SDE's always, Linux and Debian only when the program's texts name them |

It follows the theme (light, dark, glass), fits 1024×600 at 150 % (sections
scroll) and is keyboard accessible: Ctrl+Tab / Ctrl+Shift+Tab or Ctrl+Page
Down/Up and Alt+1…4 change the section, Left/Right move along the section
bar, Up/Down/Page Up/Page Down/Home/End scroll, Tab moves between buttons,
Enter and Escape close. `SDE_ABOUT_QA_FILE=FILE` logs the shown sections and
viewers for tests.

Helpers:

* `sde::developer_line()`: "Developer: Selviana DE Project" for About pages;
  `sde::part_of_line()`: "Part of Selviana Desktop Environment".
* `sde::handle_about_event(event, info)`: for small dialogs without a menu;
  call it from `handle()`: F1 or an unused right click opens "About ...".
* `sde::show_license(info)`: the license viewer alone (e.g. a "Copyright and
  License" button); `sde::show_license_viewer(title, text)` shows any text.
* `sde::show_open_source_licenses()`: every SDE program and bundled
  component with its license and copyright (Control Center → About).
* `sde::print_version(info)` / `sde::version_line(info)`: GNU-style
  `--version` text: "taca (Selviana Desktop Environment) 2.0.0~alpha1", the
  SDE release, copyright, license and the warranty disclaimer.
* `sde::toolkit_credit()`: the SDE-FLTK / FLTK credit line.
* `sde::legal::branding_path("sde-logo")`, `legal::load_branding()`: the
  official logo files (`/usr/share/sde/branding`, SVG before PNG,
  `sde-logo-dark` on dark backgrounds, `sde-mark` for the mark alone); code
  falls back to the `sde-logo` icon and then to drawing nothing.
* `sde::legal::os_pretty_name()`: the operating system from os-release; SDE
  never hard-codes a distribution name.

Desktop files carry `X-Developer=Selviana DE Project`, and
`data/metainfo/<id>.metainfo.xml` describes each program for AppStream
software centers with `<developer id="io.github.hugomonizdorego"><name>Selviana
DE Project</name></developer>` and `<icon type="stock">`. New programs add
both and an entry in `data/legal/components.json`; the
`sde-developer-metadata` and `sde-legal-data` tests check them.

## Adopting it in an app (integration checklist)

1. Replace hand-made `Fl_Menu_Item` arrays for context menus with
   `sde::ContextMenu` (or keep them and add icons with `set_menu_icon()`).
2. Give edit menus the quick row with `edit_actions()`.
3. Drop code that colors menus by hand (`labelcolor_`, `labelsize_` on items,
   `Fl_Menu_Bar` subclasses only for colors): the style does it.
4. Replace private About windows with `sde::show_about()`; describe the
   program (license, copyright, upstream projects) in
   `data/legal/components.json` and keep other credits in `AppInfo::credits`.
5. Add `X-Developer` to the desktop file and a metainfo file.

`build/tests/sde-ui-kit-demo [--theme light|dark] [--show
context|menubar|choice|ask|message|input|about|licenses|license|tooltip|files]`
shows all of it in one window.

## SDE-FLTK hooks (for toolkit work)

`Fl_SDE_Style` (FL/Fl_SDE_Style.H) has virtual methods for popup metrics and
drawing (`menu_*`), `menubar_draw`, `choice_draw`, `scrollbar_draw`,
`tooltip_margin`/`tooltip_draw` and `dialog`. `fl_sde_style(style)` installs
one; with none installed SDE-FLTK draws exactly like FLTK 1.4.5. No FLTK
structure changed size. `sde-common` implements the hooks in
`src/common/menu.cpp`.

## SDE-FLTK 1.4 for SDE code

Since 0.2 Alpha 2 SDE-FLTK follows FLTK 1.4.5 (1.4.5-sde1; the changes are
listed in `sde-fltk/CHANGES.SDE-FLTK`). What SDE code needs to know:

* **Pixels, as before.** FLTK 1.4's own GUI scaling is off in SDE-FLTK: every
  screen keeps the scale factor 1, FLTK ignores `Xft.dpi` and
  `FLTK_SCALING_FACTOR`, and Ctrl+/-/0 do not zoom windows. Sizes come from
  `sde::S()` and the SDE scale setting only.
* **Drawing** uses Xlib with Xft fonts, as with 1.3: `fl_window` is the X
  drawable being drawn, the window or the pixmap of an offscreen
  (`fl_begin_offscreen()`, now an `Fl_Image_Surface` inside FLTK), which
  `sde::Painter` wraps in a cairo surface. `Fl_Region` is opaque in 1.4;
  with Xlib drawing it is an X `Region` (`static_cast<Region>(r)`).
  `Fl_Double_Window` draws into a plain pixmap (1.4 has no Xdbe).
* **Internals**: with `FL_INTERNALS`, `FL/platform.H` (included by
  `FL/x.H`) declares `Fl_X`; `Fl_X::i(win)->region`, `Fl_X::first` and
  SDE-FLTK's `Fl_X::before_map` work as before.
* **Renamed API**: `Fl_Input_::insert_position()` replaces `position()`,
  `Fl_Browser_::vposition()` replaces `Fl_Browser_::position()` (the old
  names are deprecated and warn).
* **Wheel**: Shift+wheel arrives as a horizontal wheel (`Fl::event_dx()`),
  and the back/forward mouse buttons (X buttons 8 and 9) as
  `FL_PUSH`/`FL_RELEASE` of buttons 4 and 5.
* **Dialogs**: the `fl_ask.H` dialogs are built by FLTK's `Fl_Message`;
  `Fl_SDE_Style::dialog()` still restyles them before they are shown.
* **Text widgets**: `Fl_Text_Display::scrollbar_width(int)` sets the global
  `Fl::scrollbar_size()` in 1.4; `scrollbar_size(int)` sets the width of one
  widget (and takes effect at the next full redraw). The line number column
  runs down to the bottom edge, below the horizontal scrollbar's row too.
* **Windows**: a window with a size range (`size_range(min_w, min_h)`)
  stays resizable when the program resizes it; 1.3 fixed the size of a
  window without a `resizable()` widget at the first program resize.
* **Events**: X11 pointer motion is no longer merged (every `FL_MOVE` and
  `FL_DRAG` arrives), and tooltips hide by themselves after 12 seconds
  (`Fl_Tooltip::hidedelay()`).
* **Colors**: `fl_contrast()` keeps the FLTK 1.3 rule in SDE-FLTK, so FLTK's
  own widgets pick the same text colors as before (selected text, say).
