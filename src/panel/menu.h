// SPDX-License-Identifier: GPL-3.0-or-later
// The SDE application menu.
//
//  * Header: the user's picture and name, and a Files button. Power,
//    session and Settings buttons live in the Action Center.
//  * Search field (top or bottom): typing replaces the content with results
//    (applications, Settings pages, session actions and files). Clicking the
//    empty field shows recent applications and quick settings.
//  * Category chips: each opens a page with that category's applications.
//    They wrap into rows (or become icons) so that all of them always fit.
//  * Home view: up to 12 "Favorites" as large tiles and below them the
//    "Recently used" applications, ranked by how often and how recently they
//    were opened (left); the alphabetical "All Programs" list with letter
//    headings (right).
//
// Right-click an application to add it to the favorites or the panel;
// right-click elsewhere to arrange the menu ([menu] in sde.conf).
#pragma once

#include <FL/Fl_Input.H>
#include <cairo.h>

#include <string>
#include <vector>

#include "desktop_entry.h"
#include "effects.h"
#include "menu_model.h"
#include "paint.h"
#include "popup.h"
#include "theme.h"

class AppMenu : public PanelPopup {
 public:
  AppMenu();
  ~AppMenu() override;

  void draw() override;
  int handle(int event) override;

 private:
  enum View { V_HOME, V_CATEGORY, V_SEARCH, V_SEARCH_HOME };
  // Everything that can be clicked, hovered or selected.
  enum Kind {
    K_NONE,
    K_HEADER_BUTTON,  // index: 0 files
    K_SEARCH,
    K_CATEGORY,       // index into categories_
    K_TILE,           // favorites (home) and recent tiles (search home)
    K_RECENT,         // "Recently used" row (home)
    K_ALL_ROW,        // "All Programs" application
    K_ALL_LETTER,     // letter heading in "All Programs"
    K_LETTER_JUMP,    // letter in the jump grid; index: letter
    K_BACK,
    K_CAT_TILE,       // application in a category page
    K_RESULT,         // search result
    K_CHIP,           // quick settings chip (search home)
  };
  // A scrolled part of the menu: its rectangle, scroll offset and content.
  struct Area {
    Kind kind = K_NONE;  // K_ALL_ROW, K_TILE (home left pane), K_CAT_TILE, K_RESULT
    int x = 0, y = 0, w = 0, h = 0;
    int content = 0;
    int *scroll = nullptr;
    int bar_x() const;  // left edge of the scrollbar
    bool scrolls() const { return content > h; }
  };
  struct Item {
    Kind kind = K_NONE;
    int index = -1;
    int x = 0, y = 0, w = 0, h = 0;
    const sde::DesktopEntry *entry = nullptr;  // launchable items
    int group = -1;                            // tile group or pane
    std::string label;                         // letter or group text
    bool visible = true;                       // inside its scrolled area
    // Clip rectangle of the scrolled area the item belongs to.
    int cx = 0, cy = 0, cw = 1 << 20, ch = 1 << 20;
    bool contains(int px, int py) const {
      return visible && px >= x && px < x + w && py >= y && py < y + h && px >= cx &&
             px < cx + cw && py >= cy && py < cy + ch;
    }
  };
  struct Ref {
    Kind kind = K_NONE;
    int index = -1;
    bool operator==(const Ref &o) const { return kind == o.kind && index == o.index; }
  };

  void prepare() override;
  void closed() override;
  void reload();
  void load_favorites();
  void save_favorites();
  void save_usage();
  void add_recent(const std::string &id);
  void layout_chips();
  static void poll_applications(void *);
  void metrics();
  void build_items();
  void layout_home(int top, int bottom);
  void layout_category(int top, int bottom);
  void layout_search(int top, int bottom);
  void layout_search_home(int top, int bottom);
  void filter();
  void set_view(View v);
  void open_category(int index);
  void go_back();
  void give_focus();
  void item_menu(Item it, int px, int py);
  void layout_menu();
  void activate(Item it);
  void launch(Item it);
  void move_selection(int key);
  void ensure_visible(const Item &it);
  void scroll_by(Kind area, int dy);
  const Area *area(Kind kind) const;
  const Area *area_at(int x, int y) const;
  void drag_scrollbar(int y);
  void qa_dump_menu();
  const Item *find(const Ref &r) const;
  const Item *item_at(int x, int y) const;
  void start_hover(const Ref &r);
  static void animate(void *);
  static void blink(void *);
  static void search_changed(Fl_Widget *, void *data);
  bool searching() const;

  sde::Theme theme_;
  sde::Effects effects_;
  Fl_Input *search_;
  std::vector<sde::DesktopEntry> apps_;
  std::vector<sde::DesktopEntry> extras_;  // Settings pages and session actions
  std::vector<sde::DesktopEntry> files_;   // file search results
  std::vector<std::string> categories_;   // non-empty menu categories
  std::vector<const sde::DesktopEntry *> sorted_;  // all applications, A-Z
  std::vector<const sde::DesktopEntry *> results_;
  std::vector<const sde::DesktopEntry *> category_apps_;
  std::vector<std::string> favorites_, recent_;  // recent_: by last use
  std::vector<sde::menu::Usage> usage_;           // launch counts and times
  bool favorites_default_ = false;
  std::vector<const sde::DesktopEntry *> fav_entries_, frecent_;
  sde::menu::FileIndex file_index_;
  std::vector<Item> items_;
  std::vector<Item> chips_;  // category chips, laid out by layout_chips()
  std::vector<Area> areas_;
  bool chips_icons_ = false;  // icon-only chips (labels as tooltips)
  cairo_surface_t *avatar_ = nullptr;
  std::string full_name_;

  View view_ = V_HOME, before_search_ = V_HOME;
  int category_ = -1;
  int all_scroll_ = 0, cat_scroll_ = 0, result_scroll_ = 0, home_scroll_ = 0;
  // Scrollbar drag: the area and where it started.
  Kind drag_area_ = K_NONE;
  int drag_y_ = 0, drag_scroll_ = 0;
  bool bar_hover_ = false;  // the pointer is over a scrollbar
  bool menu_busy_ = false;  // a context menu runs its own event loop
  std::string last_query_;
  bool letters_open_ = false;
  Ref hover_, selected_;
  bool keyboard_ = false;  // selection was moved with the keyboard
  bool caret_on_ = true;

  // Arrangement from [menu] in sde.conf.
  bool search_top_ = true, show_desc_ = true, show_recent_ = true;

  // Layout (window coordinates), from metrics().
  int pad_ = 12, header_h_ = 56, search_y_ = 56, search_h_ = 40, cats_y_ = 104, cats_h_ = 36;
  int body_top_ = 148, body_bottom_ = 0;
  bool compact_ = false;  // little height: smaller header, tiles and chips
  int split_x_ = 0;  // left edge of the "All Programs" pane
  int all_top_ = 0, all_bottom_ = 0;
  int view_top_ = 0, view_bottom_ = 0;  // scrolled area of category/search views

  // Animations: hover progress of one item and the launch effect.
  Ref anim_;
  double anim_hover_ = 0, anim_start_ = 0;
  bool launching_ = false;
  double launch_start_ = 0, launch_cx_ = 0, launch_cy_ = 0, launch_size_ = 32;
  std::string launch_icon_, launch_effect_;
};
