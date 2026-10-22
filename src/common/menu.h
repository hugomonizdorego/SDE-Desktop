// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE menus. Every SDE program draws FLTK's popups (context menus, menu bar
// dropdowns, Fl_Choice lists), menu bars, scroll bars, tooltips and stock
// dialogs in the SDE theme: the style is installed automatically by the
// common library (see install_ui_style()) and reads the active theme each
// time a menu opens, so it follows light/dark/accent changes.
//
// ContextMenu builds popups with icons, check marks, shortcuts, submenus and
// a Windows 11 like row of quick actions (Cut, Copy, Paste, ...) at the top:
//
//   sde::ContextMenu menu;
//   sde::EditActions edit;
//   edit.cut = [&] { cut(); };
//   edit.copy = [&] { copy(); };
//   edit.paste = [&] { paste(); };
//   menu.edit_actions(edit);
//   menu.item(_("Open"), [&] { open(); }, {"document-open", FL_CTRL + 'o'});
//   menu.check(_("Show hidden files"), hidden, [&] { toggle_hidden(); });
//   menu.separator();
//   menu.submenu(_("Sort by"), std::move(sort_menu));
//   menu.popup();  // at the pointer; runs the chosen action
//
// Plain Fl_Menu_Item arrays and Fl_Menu_ widgets get the same look; give
// their items icons with set_menu_icon() and mark quick actions with the
// FL_MENU_SDE_QUICK flag (FL/Fl_SDE_Style.H).
#pragma once

#include <FL/Fl_SDE_Style.H>
#include <FL/Fl_Menu_Item.H>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Fl_Input_;
class Fl_Widget;
class Fl_Window;

namespace sde {

// Installs the SDE-FLTK theme hooks. Called automatically when a program
// links the common library; calling it again is harmless.
void install_ui_style();

// Replaces FLTK's square stock boxes (FL_UP_BOX, FL_DOWN_BOX, the thin ones
// and their frames) with soft rounded ones in the theme's radii, so stock
// buttons, fields, check boxes and panels have no sharp corners (alpha51).
// Theme::apply_to_fltk() calls it after setting the FLTK scheme, which
// resets the box types; SDE_STOCK_FLTK=1 keeps FLTK's own boxes.
void install_soft_boxes();

// Gives a shown window rounded corners of radius |r| (already scaled) with
// XShape: popups, tooltips, notifications. Compositors draw the shadow
// along the shape. Calling it again (e.g. from draw()) is cheap.
void shape_rounded(Fl_Window *win, int r);

// Text fields (Fl_Input and friends) get an SDE context menu on right click
// (Cut, Copy, Paste, Delete, Select all). On by default.
void set_input_context_menus(bool enabled);

// Icons for menu items that ContextMenu did not build. |icon| is a
// freedesktop icon name or one of the built-in glyphs: "cut", "copy",
// "paste", "delete", "select-all", "undo", "redo", "rename", "share",
// "open", "new", "save", "print", "find", "settings", "info", "refresh",
// "properties", "trash", "folder", "image", "link", "close".
// By item address (arrays you keep alive)...
void set_menu_icon(const Fl_Menu_Item *item, const std::string &icon);
// ...or by label text, for menus built with Fl_Menu_::add(), which copies
// items. The label is compared without '&' mnemonics.
void set_menu_icon_for_label(const std::string &label, const std::string &icon);
std::string menu_icon(const Fl_Menu_Item *item);

struct MenuItemOptions {
  std::string icon;       // see set_menu_icon()
  int shortcut = 0;       // shown right aligned, e.g. FL_CTRL + 'c'
  bool enabled = true;
  bool divider = false;   // separator below the item
};

// Standard edit actions for edit_actions(); an empty function hides the
// action, |can_*| = false shows it disabled.
struct EditActions {
  std::function<void()> cut, copy, paste, select_all, del;
  bool can_cut = true, can_copy = true, can_paste = true, can_select_all = true, can_delete = true;
};

class ContextMenu {
 public:
  using Action = std::function<void()>;

  ContextMenu();
  ~ContextMenu();
  ContextMenu(ContextMenu &&) noexcept;
  ContextMenu &operator=(ContextMenu &&) noexcept;
  ContextMenu(const ContextMenu &) = delete;
  ContextMenu &operator=(const ContextMenu &) = delete;

  // A button in the quick-actions row (label is its accessible name).
  ContextMenu &quick(const std::string &label, const std::string &icon, Action action,
                     bool enabled = true);
  // Cut, Copy, Paste, Select all, Delete as quick actions (Ctrl+X/C/V/A, Del).
  ContextMenu &edit_actions(const EditActions &actions);
  ContextMenu &item(const std::string &label, Action action, const MenuItemOptions &opts = {});
  ContextMenu &check(const std::string &label, bool checked, Action action, bool enabled = true);
  ContextMenu &radio(const std::string &label, bool selected, Action action, bool enabled = true);
  ContextMenu &submenu(const std::string &label, ContextMenu &&sub, const std::string &icon = "");
  ContextMenu &separator();  // a line below the last item
  bool empty() const;

  // Shows the menu at the pointer (or at x, y relative to the window that
  // got the last event) and runs the chosen action. Returns true when an
  // item was chosen.
  bool popup();
  bool popup(int x, int y);
  // Shows the menu below a widget, aligned with its left edge.
  bool popup_below(Fl_Widget *anchor);

  // The FLTK array (valid until the menu changes), e.g. for Fl_Menu_::menu().
  const Fl_Menu_Item *items();

 private:
  struct Entry;
  std::vector<Entry> entries_;
  std::vector<Fl_Menu_Item> built_;
  std::vector<std::unique_ptr<Action>> actions_;
  void build();
  void flatten(std::vector<Fl_Menu_Item> &out);
  bool run(const Fl_Menu_Item *picked);
};

// Gives stock FLTK widgets in |root| (buttons, inputs, browsers, choices,
// check buttons; recursively) the SDE look: rounded themed boxes and theme
// colors. fl_ask.H dialogs and Fl_File_Chooser get it automatically.
void apply_stock_style(Fl_Widget *root);

// The standard SDE context menu of a text field.
void show_input_context_menu(Fl_Input_ *input);

}  // namespace sde
