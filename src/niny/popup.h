// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Niny's themed popup menus: the menus of the menu bar, toolbar dropdowns,
// status bar choices and the editor context menu with its row of quick
// action buttons. The menu is described as data (MenuSpec) so that it can be
// handed to the shared SDE menu kit later without touching the callers.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace niny {

struct MenuItem {
  std::string label;
  std::string shortcut;  // shown on the right ("Ctrl+S")
  int icon = -1;         // icon::Kind, -1 for none
  bool enabled = true;
  bool checked = false;  // check mark (or radio dot when radio)
  bool radio = false;
  bool separator = false;  // a divider line instead of an item
  std::string hint;        // dim text under a disabled entry's label
  std::vector<MenuItem> sub;
  std::function<void()> run;

  static MenuItem sep() {
    MenuItem m;
    m.separator = true;
    return m;
  }
};

// One button of the icon row at the top of a context menu.
struct QuickAction {
  std::string label;
  int icon = -1;
  bool enabled = true;
  std::function<void()> run;
};

// A menu opened from a menu bar title. While it is open, Left and Right on
// its first level, the pointer moving onto another title and Alt with
// another title's letter close it, and |next| names the title the bar opens
// instead (|next_keyboard| when a key asked for it, so that its first item
// gets highlighted). A click on the open title just closes the menu.
struct MenuBarLink {
  int current = 0;                                      // the open title
  std::function<int(int root_x, int root_y)> title_at;  // title under a point, 0 for none
  std::function<int(int key)> title_for_key;            // title of Alt+key, 0 for none
  std::function<int(int dir)> neighbour;                // title left (-1) or right (+1) of current
  int next = 0;
  bool next_keyboard = false;
};

struct MenuSpec {
  std::vector<QuickAction> quick;
  std::vector<MenuItem> items;
  int min_width = 0;      // unscaled pixels
  bool keyboard = false;  // opened with a key: its first item is highlighted
  std::shared_ptr<MenuBarLink> bar;
  std::function<void()> closed;  // runs when the menu closes, before the chosen action
  // UI test hook: with SDE_PANEL_QA_DIR set, open menus are described in
  // <dir>/Niny-menu.txt ("@open:<qa_name>", then "level:N" and
  // "item:N:<label>" lines with root geometry); "@open:none" once closed.
  std::string qa_name = "popup";
};

// Shows the menu at root coordinates (x, y), runs until it closes and then
// runs the chosen action. When |anchor_h| > 0 the menu belongs to a button of
// that height at (x, y - anchor_h): it opens below the button, or above it
// when there is no room. A negative |anchor_h| opens the menu above y (status
// bar choices). A menu taller than the screen gets lower rows (down to 24
// pixels) before it scrolls. Returns true when an action ran.
bool popup_menu(const MenuSpec &spec, int x, int y, int anchor_h = 0);

}  // namespace niny
