//
// SDE-FLTK drawing hooks.
//
// Copyright 2026 Selviana DE Project and contributors. Same license as FLTK (LGPL-2 with the FLTK
// exceptions, see COPYING).
//
// SDE-FLTK lets the desktop draw FLTK's own popups and stock widgets in its
// theme. A program installs an Fl_SDE_Style object with fl_sde_style(); while
// one is installed:
//
//  * popup menus (Fl_Menu_Item::popup()/pulldown(): context menus, the
//    dropdowns of Fl_Menu_Bar, Fl_Menu_Button and Fl_Choice) use a padded
//    layout with variable item heights and call the menu_* methods to draw;
//    items flagged FL_MENU_SDE_QUICK at the start of a popup form a row of
//    icon buttons ("quick actions") above the other items;
//  * Fl_Menu_Bar, Fl_Choice, Fl_Scrollbar and the tooltip window call the
//    matching method and fall back to the stock drawing when it returns 0;
//  * fl_message()/fl_ask()/fl_choice()/fl_input() and Fl_File_Chooser call
//    dialog() before their window is shown so that it can restyle it.
//
// Without a style (the default) SDE-FLTK draws exactly like FLTK 1.4.5.
// The hooks only add code paths; no FLTK structure changes size, so the
// library stays source and binary compatible with FLTK 1.4.
//

#ifndef Fl_SDE_Style_H
#define Fl_SDE_Style_H

#include "Fl_Export.H"
#include "Enumerations.H"

struct Fl_Menu_Item;  // a struct in FLTK (Clang -Wmismatched-tags)
class Fl_Menu_;
class Fl_Menu_Bar;
class Fl_Choice;
class Fl_Scrollbar;
class Fl_Window;

/// Extra Fl_Menu_Item flags of SDE-FLTK (above FLTK's own bits).
enum {
  /// Show the item as an icon button in the quick-actions row at the top of
  /// a popup. Only consecutive items at the start of a (sub)menu qualify;
  /// without a style the flag is ignored and the item is a normal entry.
  FL_MENU_SDE_QUICK = 0x10000
};

/// Theme hooks; see the comment at the top of FL/Fl_SDE_Style.H.
/// Coordinates are relative to the window being drawn (Fl_Window::current()).
class FL_EXPORT Fl_SDE_Style {
public:
  virtual ~Fl_SDE_Style();

  /// Called once before a popup menu hierarchy opens (e.g. to reload colors).
  virtual void menu_begin() {}
  /// Called after each popup window is mapped (shape, opacity, ...).
  virtual void menu_window_shown(Fl_Window *) {}

  // --- popup metrics, in pixels
  virtual int menu_padding() { return 4; }
  /// Height of a normal item whose label measured \p label_h pixels.
  virtual int menu_item_height(const Fl_Menu_Item *, const Fl_Menu_ *, int label_h) {
    return label_h + 8;
  }
  /// Space added below an item flagged FL_MENU_DIVIDER.
  virtual int menu_divider_height() { return 8; }
  /// Width of the column in front of the labels (icons, check marks), given
  /// the first visible item of the popup.
  virtual int menu_leading_width(const Fl_Menu_Item *, const Fl_Menu_ *) { return 8; }
  /// Width of the space after the labels (submenu arrows).
  virtual int menu_trailing_width() { return 20; }
  /// Gap between a label and its shortcut text.
  virtual int menu_shortcut_gap() { return 24; }
  /// Size of a square quick-action button.
  virtual int menu_quick_size() { return 34; }

  // --- popup drawing
  virtual void menu_draw_background(int w, int h) = 0;
  /// \p selected: 1 when highlighted. \p lead: width of the icon column;
  /// \p shortcut_w: width reserved for the shortcut text.
  virtual void menu_draw_item(const Fl_Menu_Item *, const Fl_Menu_ *, int x, int y, int w,
                              int h, int selected, int lead, int shortcut_w) = 0;
  /// Divider line for an item flagged FL_MENU_DIVIDER, centered in the
  /// band x, y, w, h.
  virtual void menu_draw_divider(int x, int y, int w, int h) = 0;
  /// A quick-action button.
  virtual void menu_draw_quick(const Fl_Menu_Item *, const Fl_Menu_ *, int x, int y, int w,
                               int h, int selected) = 0;
  /// Title box of popup(title) menus and the pulled-down Fl_Menu_Bar title
  /// (\p menubar 1), which is drawn in a window placed over the bar.
  virtual void menu_draw_title(const Fl_Menu_Item *, const Fl_Menu_ *, int w, int h,
                               int menubar) = 0;

  // --- stock widgets: return 1 when drawn, 0 for the stock look
  virtual int menubar_draw(Fl_Menu_Bar *, int hover_index) { return 0; }
  virtual int choice_draw(Fl_Choice *) { return 0; }
  /// \p X, \p Y, \p W, \p H: the whole scrollbar; \p tx..\p th: the thumb.
  /// \p pushed: 1/2 when an end is pressed. \p hover: pointer inside.
  virtual int scrollbar_draw(Fl_Scrollbar *, int X, int Y, int W, int H, int tx, int ty,
                             int tw, int th, int hover) { return 0; }
  /// Tooltip text margins (\p vertical 0: left/right, 1: top/bottom) or -1
  /// for Fl_Tooltip's own.
  virtual int tooltip_margin(int vertical) { return -1; }
  /// Tooltip window contents, laid out with tooltip_margin().
  virtual int tooltip_draw(int w, int h, const char *text) { return 0; }
  /// Restyles a stock dialog window (fl_ask.H dialogs, Fl_File_Chooser).
  virtual void dialog(Fl_Window *) {}
};

/// The installed style or NULL.
FL_EXPORT Fl_SDE_Style *fl_sde_style();
/// Installs \p s (not copied; NULL restores the stock FLTK look).
FL_EXPORT void fl_sde_style(Fl_SDE_Style *s);

/// Draws a menu item's label (any labeltype, '&' mnemonics hidden unless
/// \p mnemonic) left aligned in the box with color \p c. For styles.
FL_EXPORT void fl_sde_draw_menu_label(const Fl_Menu_Item *, const Fl_Menu_ *, int x, int y,
                                      int w, int h, Fl_Color c, int mnemonic);
/// Width of a menu item's label without check box space. For styles.
FL_EXPORT int fl_sde_measure_menu_label(const Fl_Menu_Item *, const Fl_Menu_ *, int *h);
/// Sets fl_font() to the font of a menu item. For styles.
FL_EXPORT void fl_sde_menu_font(const Fl_Menu_Item *, const Fl_Menu_ *);

#endif
