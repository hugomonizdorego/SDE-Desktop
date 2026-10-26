# Niny

Niny (`niny`, `src/niny`) is the text editor of SDE: tabs on top, a menu
bar under them, the text, and a status bar with the line and column, the
characters, the file type, the zoom, the line ending and the encoding. It
highlights seventeen languages, finds and replaces with regular
expressions, renders Markdown, checks spelling with hunspell or aspell and
keeps unsaved changes across crashes. The manual page is `niny(1)`
(`data/man/niny.1`). This page describes the tabs and the menu bar as they
are since 0.2 Alpha 3.

## Tabs

Every document has a tab. A tab's **×** closes that tab, and nothing else
happens: after the tab is gone, whatever slides under the pointer — the
next tab, the empty strip or the **+** button — is left alone. A document
with unsaved changes asks first (*Close Without Saving*, *Cancel*,
*Save*); *Save* on a document that was never saved asks for a name.

**The only tab stays open.** While one tab is open its × is drawn
inactive, has no hover effect, does nothing when clicked, and its tooltip
says "Only one tab is open, so it cannot be closed". The other ways of
closing a tab follow the same rule: the middle mouse button, *Close Tab*
in the tab's menu and in File, Ctrl+W (the status bar then shows the same
message) and the close button in the side panel's list of documents. To
end Niny, close the window: its title bar, File → Quit (Ctrl+Q) or
Alt+F4.

Other things a tab strip does:

- **+** (or Ctrl+N) opens a new tab after the current one. Untitled
  documents are numbered with the smallest free number ("Untitled",
  "Untitled 2" ...).
- A **double click on the empty strip** opens a new tab — only when both
  clicks were on the empty strip.
- **Dragging** a tab moves it; the wheel over the strip switches tabs.
- **Many tabs** get narrower, down to 96 pixels (at 100 %); then the strip
  scrolls so that the current tab stays in view, and each visible × closes
  its own tab.
- **Close All Tabs** (Ctrl+Shift+W) closes every tab, asking for each
  modified one, and leaves one empty "Untitled"; with one tab it is
  inactive. **Close Other Tabs** is in the tab's menu (right click), with
  Save, Save As, Copy File Path and Show File in Side Panel.
- **Reopen Closed Tab** (Ctrl+Shift+T) brings back the last closed file.

### Before 0.2 Alpha 3

The × of the last tab "added a page": `App::close_document()` opened a new
untitled document as soon as the last one closed, so the × of a single
"Untitled" tab replaced it with "Untitled 2" (and the next click with
"Untitled 3"). And because the tab strip opens a tab on a double click on
its empty part, a quick second click on the × of the rightmost tab — after
the first had closed it — landed on the now empty strip and counted as a
double click (FLTK's click counter was still running), opening a new tab
right where one had just closed. Now the only tab is never closed, the
tab strip resets the click counter whenever a × (or +) acts, and a double
click opens a tab only when both of its clicks were on the empty strip.
The close box of each tab is computed by one function for drawing and for
the clicks, so the two cannot drift apart at any scale.

## The menu bar

The row under the tabs is a menu bar, shown from the start, with the
menus of other Linux editors (Pluma, Mousepad, Kate, Xed). On its right are
the Markdown formatting buttons (for Markdown documents) and the
Preferences gear.

| Menu | Contents |
|------|----------|
| **File** | New Tab (Ctrl+N), New Window (Ctrl+Shift+N), Open… (Ctrl+O), Open Recent ▸ · Save (Ctrl+S), Save As… (Ctrl+Shift+S), Save All (Ctrl+Alt+S), Reload from Disk · Close Tab (Ctrl+W), Close All Tabs (Ctrl+Shift+W), Reopen Closed Tab (Ctrl+Shift+T) · Quit (Ctrl+Q) |
| **Edit** | Undo (Ctrl+Z), Redo (Ctrl+Shift+Z or Ctrl+Y) · Cut, Copy, Paste, Delete, Select All · Copy File Path, Insert Date and Time (F5) · Format ▸ (Markdown: bold, italic, strikethrough, inline code, link, heading, lists, clear formatting), Text Tools ▸ (case, sorting, trimming, duplicate line, comments) · Preferences (Ctrl+,) |
| **View** | Menu Bar, Status Bar, Side Panel (F9) · Line Numbers, Highlight Current Line, Show Whitespace, Word Wrap (Alt+Z) · Font…, Zoom ▸ (Ctrl+Plus, Ctrl+Minus, Ctrl+0), Theme ▸ (Use system setting, Light, Dark) · Formatted Markdown View (Ctrl+Shift+M), Full Screen (F11) |
| **Search** | Find… (Ctrl+F), Find Next (F3), Find Previous (Shift+F3), Find and Replace… (Ctrl+H) · Go to Line… (Ctrl+G), Go to Matching Bracket (Ctrl+M), Quick Open… (Ctrl+P) · Search the Web (Ctrl+E) |
| **Document** | File Type ▸ (Plain text; Sources, Markup, Data and Configuration, Other), Syntax Highlighting · Indentation ▸ (spaces or tabs, width 2, 3, 4 or 8, Automatic Indentation), Encoding ▸ (the encoding used when saving; Reopen with Encoding ▸), Line Endings ▸ (Unix, Windows, classic Mac) · Previous Tab (Ctrl+Page Up), Next Tab (Ctrl+Page Down) |
| **Tools** | Check Spelling as You Type, Spelling Suggestions ▸ · Document Statistics… · Command Palette (Ctrl+Shift+P) |
| **Help** | Keyboard Shortcuts (F1), About Niny |

Items that do not apply are inactive rather than missing (Reload for an
unsaved document, Cut without a selection, Formatted Markdown View for
other documents). The file type, line ending and encoding segments of the
status bar open the same choices as the Document menu.

Compared with 0.2 Alpha 2 nothing was removed; the row had File, Edit and
View only, and some items moved: Find, Replace and Go To to **Search**
(Go To is now *Go to Line…*), Search the Web to Search, Font… from Edit to
**View** (it opens a Font dialog now instead of Settings), File Type and
Indentation from View to **Document**, the Command Palette to **Tools**,
About to **Help**, Settings to Edit → **Preferences**, Copy File Path from
File to Edit, *Exit* became *Quit* and *Time/Date* *Insert Date and Time*.

### Keyboard

- **Alt and the underlined letter** opens a menu: Alt+F File, Alt+E Edit,
  Alt+V View, Alt+S Search, Alt+D Document, Alt+T Tools, Alt+H Help. The
  letters come from the "&" in the translated titles; a translation
  without one gets the first free letter, and Z and the digits stay with
  Word Wrap and the tabs (Alt+Z, Alt+1 … Alt+9).
- **F10** opens File. A menu opened with a key has its first item
  highlighted.
- In a menu: Up and Down (Home, End) choose, Enter or Space runs, typing a
  letter jumps to the next item starting with it, Right opens a submenu,
  **Left and Right** elsewhere move to the previous and next menu (and
  wrap around), Alt and another title's letter switch to that menu,
  Escape closes a submenu or the menu, F10 closes the menus.
- **Alt pressed and released alone** gives the keyboard to the titles: the
  first one is outlined, Left and Right choose, Down or Enter (or a title's
  letter) opens, Escape or Alt again leaves.
- **Shift+F10** and the Menu key open the context menu at the cursor.

### Mouse

A click opens a title's menu; while it is open, moving the pointer onto
another title opens that menu, and clicking the open title closes it. A
pointer that has not moved since a menu was opened with a key does not
switch menus.

### Hiding the menu bar

View → **Menu Bar**, or Preferences → View → Menu bar, hides the row (and
the formatting buttons and gear in it). Alt or F10 then shows it while the
keyboard uses it, and it hides again when the menu closes. With the mouse,
the **Niny mark** at the start of the tab strip opens every menu in one
popup, with *Menu Bar* at the bottom to show the row again. The setting is
`menu_bar` in the `[view]` section of `~/.config/sde/niny.conf` (default
`true`).

### Small screens

A menu that is taller than the space on its screen gets lower rows, from
34 down to 24 pixels (at 100 %), before anything scrolls, and moves up
over the menu bar if needed; each menu of the menu bar has at most twelve
entries, and long lists are grouped into submenus (file types like Kate
and Mousepad). Every menu fits a 1024×600 screen at 150 % without
scrolling. When the window is narrow the titles move closer together and
the formatting buttons give way first, then the gear (Preferences stays in
the Edit menu).

## Font…

View → Font… opens a Font dialog inside the window: a list of the
monospaced families (as on the Preferences page, with Sans and Serif), a
search field that filters it as you type, the sizes from 8 to 32 points,
and a preview in the chosen font. Up and Down move in the list, Tab
switches between families and sizes, Enter or a double click selects,
Escape cancels, *Default* returns to Monospace 11. A new size resets the
zoom, like the size on the Preferences page.

## Keyboard Shortcuts and Preferences

Help → Keyboard Shortcuts (F1) shows every key on the Preferences page's
layout: the commands with keys by the menu that has them, then the tabs
(Ctrl+Tab, Alt+1 … Alt+8, Alt+9 for the last tab), the menus (F10, Alt
and each title's letter, Alt alone, the context menu) and a few more
(Ctrl+Y, Ctrl+Backspace and Ctrl+Delete, Ctrl and the wheel, Escape). The
back arrow or Escape returns to the text.

Edit → Preferences (Ctrl+, or the gear) is the page that was called
Settings: theme, font, word wrap, syntax colors, the menu bar, the status
bar, line numbers, whitespace, the current line, the side panel, how files
from other programs open, the session, recent files, spelling, the web
search, the editing options and recovery.

Tools → Document Statistics… counts the lines, words, characters (with and
without spaces) and bytes of the document, and of the selection.

## Tests

With `SDE_PANEL_QA_DIR` set, Niny describes itself for the UI tests in
`<dir>/Niny.txt` — `@tabs:`, `@active:`, `@menubar:`, `@titles:`,
`@sheet:`, `@dialog:`, `@page:`, `@toast:` and `@font:` lines, then
`tab:N:<title>`, `close:N` (ending in 1 when the × is active), `plus`,
`logo`, `menu:file` … `menu:help`, `gear` and the buttons of an open
question, in window pixels — and its open menus in `<dir>/Niny-menu.txt`
(`@open:<menu>`, `level:N` with 1 when it fits the work area and 1 when it
scrolls, `item:N:<label>`, `@hover:`), in screen pixels.

`tests/test_niny_ui.py` (ctest `niny-tabs-menus`) uses them at 1366×768
(100 %), 1920×1080 (150 %) and 1024×600 (150 %, with a 60 pixel panel):
the × of each tab, a double click on the × of the last tab, the only tab
(click, double click, middle button, Ctrl+W), a modified tab (Cancel,
Close Without Saving, Save), dragging, a scrolled strip, the menus from
the mouse and the keyboard (F10 and Alt by keycode through XTEST), their
fit, and the hidden menu bar. `niny-core` checks the menu letters, the
untitled numbers and the setting. The showcase has `niny-menu`,
`niny-font`, `niny-shortcuts` and `niny-single-tab`.
