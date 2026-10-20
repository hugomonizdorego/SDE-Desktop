// SPDX-License-Identifier: GPL-3.0-or-later
// The SDE application menu: search, categories, application list and the
// settings/power buttons.
#pragma once

#include <FL/Fl_Input.H>

#include "popup.h"

#include <string>
#include <vector>

#include "desktop_entry.h"
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
  };
  enum Zone { Z_NONE, Z_CATEGORY, Z_APP, Z_FILES, Z_SETTINGS, Z_POWER };

  void prepare() override;
  void reload();
  static void poll_applications(void *);
  void layout();
  void item_menu(int row);
  void filter();
  void launch(int row);
  void ensure_visible(int row);
  Zone zone_at(int x, int y, int &index) const;
  bool grid_mode() const;
  int columns() const;
  int list_rows_visible() const;
  static void search_changed(Fl_Widget *, void *data);

  sde::Theme theme_;
  Fl_Input *search_;
  std::vector<sde::DesktopEntry> apps_;
  std::vector<std::string> categories_;  // "All applications" + non-empty ones
  std::vector<Row> rows_;
  int category_ = 0;
  int category_scroll_ = 0;
  int selected_ = 0;
  int scroll_ = 0;
  int hover_zone_ = Z_NONE, hover_index_ = -1;

  // Layout.
  int pad_ = 16, header_h_ = 64, footer_h_ = 60, cat_w_ = 190, row_h_ = 46, cat_row_h_ = 36;
  int icon_ = 32;
};
