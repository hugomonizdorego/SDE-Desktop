// SPDX-License-Identifier: GPL-3.0-or-later
// The SDE application menu: search, categories, application list or grid
// and the files/settings/power buttons. Right-click the menu (outside an
// application) to arrange it: search at the top or bottom, categories on
// the left, right or hidden, list or grid, size and descriptions.
#pragma once

#include <FL/Fl_Input.H>

#include "popup.h"

#include <string>
#include <vector>

#include "desktop_entry.h"
#include "effects.h"
#include "theme.h"

class AppMenu : public PanelPopup {
 public:
  AppMenu();
  ~AppMenu() override;

  void draw() override;
  int handle(int event) override;

 private:
  struct Row {
    const sde::DesktopEntry *entry;
    bool extra = false;  // a Settings page or session action found by search
  };
  enum Zone { Z_NONE, Z_CATEGORY, Z_APP, Z_FILES, Z_SETTINGS, Z_POWER, Z_SEARCH };

  void prepare() override;
  void closed() override;
  void reload();
  static void poll_applications(void *);
  void layout();
  void item_menu(int row);
  void layout_menu();
  void filter();
  void launch(int row);
  void ensure_visible(int row);
  Zone zone_at(int x, int y, int &index) const;
  bool grid_mode() const { return grid_; }
  int columns() const;
  int list_rows_visible() const;
  void tile_rect(int i, int &x, int &y, int &w, int &h) const;
  void start_hover(int index);
  static void animate(void *);
  static void search_changed(Fl_Widget *, void *data);

  sde::Theme theme_;
  sde::Effects effects_;
  Fl_Input *search_;
  std::vector<sde::DesktopEntry> apps_;
  std::vector<sde::DesktopEntry> extras_;  // Settings pages and session actions
  std::vector<std::string> categories_;   // "All applications" + non-empty ones
  std::vector<Row> rows_;
  int category_ = 0;
  int category_scroll_ = 0;
  int selected_ = 0;
  int scroll_ = 0;
  int hover_zone_ = Z_NONE, hover_index_ = -1;

  // Arrangement from [menu] in sde.conf.
  bool search_top_ = true, grid_ = false, show_desc_ = true;
  std::string cat_side_ = "left";  // left, right or hidden

  // Layout (window coordinates).
  int pad_ = 16, header_h_ = 48, footer_h_ = 60, cat_w_ = 190, row_h_ = 46, cat_row_h_ = 36;
  int icon_ = 32, body_top_ = 112, body_bottom_ = 0, search_y_ = 58;
  int cat_x_ = 0, list_x_ = 0, list_w_ = 0;

  // Animations: hover progress of one tile and the launch effect.
  int anim_index_ = -1;
  double anim_hover_ = 0, anim_start_ = 0;
  int launch_index_ = -1;
  double launch_start_ = 0;
  std::string launch_effect_;
};
