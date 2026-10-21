// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// Niny's themed popup menus: the File/Edit/View menus, toolbar dropdowns,
// status bar choices and the editor context menu with its row of quick
// action buttons. The menu is described as data (MenuSpec) so that it can be
// handed to the shared SDE menu kit later without touching the callers.
#pragma once

#include <functional>
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

struct MenuSpec {
  std::vector<QuickAction> quick;
  std::vector<MenuItem> items;
  int min_width = 0;  // unscaled pixels
};

// Shows the menu at root coordinates (x, y), runs until it closes and then
// runs the chosen action. When |anchor_h| > 0 the menu belongs to a button of
// that height at (x, y - anchor_h): it opens below the button, or above it
// when there is no room. A negative |anchor_h| opens the menu above y (status
// bar choices). Returns true when an action ran.
bool popup_menu(const MenuSpec &spec, int x, int y, int anchor_h = 0);

}  // namespace niny
