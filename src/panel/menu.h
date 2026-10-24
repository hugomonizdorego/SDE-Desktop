// SPDX-License-Identifier: GPL-3.0-or-later
// The SDE Start menu.
//
//  * Left rail: the menu button (expands the rail with labels) and, at the
//    bottom, the account, Documents, Pictures, Gallery, Settings and, apart,
//    Power off (asks first; the other session actions are in the Action
//    Center).
//  * Middle: "Recently added" and "Most used" applications, then every
//    application A-Z under letter headings (click a heading for the letter
//    grid). A small category filter at the top narrows the list.
//  * Right: the user's pinned applications as tiles (small, medium, wide,
//    large) in named groups. Drag tiles to arrange them, drag an
//    application from the list to pin it; right-click for Resize, Move to
//    group, Unpin and Rename group.
//
// Search lives in the panel (see search.h): typing while the menu is open
// moves to the panel's search. The menu can be resized by dragging its
// outer edges; [menu] in sde.conf keeps the tiles and the size.
#pragma once

#include <FL/Fl_Input.H>
#include <cairo.h>

#include <memory>
#include <string>
#include <vector>

#include "app_catalog.h"
#include "desktop_entry.h"
#include "effects.h"
#include "menu_model.h"
#include "paint.h"
#include "popup.h"
#include "theme.h"

class AccountPopup;

// Start tiles from other places (search results, the panel): the tile
// groups saved in [menu] tiles. 0.2 Alpha 2: without saved tiles, groups by
// category of the installed applications (menu::category_groups()); tiles
// saved by older versions are migrated (menu::migrate_tile_groups()).
namespace start_tiles {
// Group names by menu::TileCategory, translated.
std::vector<std::string> group_names();
std::vector<sde::menu::TileGroup> load();
void save(const std::vector<sde::menu::TileGroup> &groups);
std::vector<std::string> ids();
bool pinned(const std::string &id);
// Into the group of the application's category (menu::pin_by_category()).
void pin(const std::string &id);
void unpin(const std::string &id);
}  // namespace start_tiles

// Icon size of the Start menu's list rows (and the search's result rows, so
// that one preloaded icon serves both).
int start_row_icon_size();

// The right-click actions of an application in the menu and the search.
namespace start_apps {
void show_info(const sde::DesktopEntry &e);      // "App info...": a small window
void open_location(const sde::DesktopEntry &e);  // "Open file location": its .desktop folder
// SDE's own applications: "sde-*" and every desktop file that SDE installs
// (a copy of each is in <datadir>/sde/applications).
bool sde_app(const sde::DesktopEntry &e);
// "Uninstall..." (0.2 Alpha 2) is offered when Merkadu is installed, for
// applications with a desktop file that are not SDE's own.
bool can_uninstall(const sde::DesktopEntry &e);
// Runs `merkadu --uninstall <desktop file>`, which asks first and shows the
// progress (exit 0: removed, 1: failed, 2: cancelled). Once the application
// is gone its tiles, panel pin and launch history go too; the menu and the
// search drop it with the next catalog snapshot (app_catalog.h).
void uninstall(const sde::DesktopEntry &e);
}  // namespace start_apps

class AppMenu : public PanelPopup {
 public:
  AppMenu();
  ~AppMenu() override;

  void draw() override;
  int handle(int event) override;
  void flush() override;  // timing hook (menu_perf.h)
  // The group name editor commits or cancels (Enter, Escape, focus lost).
  void finish_rename(bool commit);
  // sde.conf changed: an open menu follows the theme, accent and effects.
  void config_changed();
  // The size the menu opens at ([menu] width/height or size, kept on the
  // monitor) and whether that is the compact layout.
  static void preferred_size(int &w, int &h);

 private:
  enum Kind {
    K_NONE,
    K_RAIL,        // index: 0 menu, 1 account, 2 documents, 3 pictures, 4 settings, 5 power off, 6 gallery
    K_FILTER,      // category filter of the list
    K_ROW,         // application in the list; group: 0 A-Z, 1 recently added, 2 most used
    K_LETTER,      // letter heading in the list
    K_JUMP,        // letter in the jump grid; index: letter
    K_GROUP,       // tile group heading; index: group
    K_TILE,        // tile; index: tile number, group: group
    K_HEADING,     // text only
  };
  struct Area {
    Kind kind = K_NONE;  // K_ROW (list) or K_TILE (tiles)
    int x = 0, y = 0, w = 0, h = 0;
    int content = 0;
    int *scroll = nullptr;
    int bar_x() const { return x + w - sde::S(7); }
    bool scrolls() const { return content > h; }
  };
  struct Item {
    Kind kind = K_NONE;
    int index = -1;
    int group = -1;
    int x = 0, y = 0, w = 0, h = 0;
    const sde::DesktopEntry *entry = nullptr;
    sde::menu::TileSize size = sde::menu::TileSize::Medium;
    std::string label;
    bool visible = true;
    int cx = 0, cy = 0, cw = 1 << 20, ch = 1 << 20;  // clip of its scrolled area
    bool clipped() const { return ch < (1 << 19); }
    bool contains(int px, int py) const {
      return visible && px >= x && px < x + w && py >= y && py < y + h && px >= cx &&
             px < cx + cw && py >= cy && py < cy + ch;
    }
  };
  struct Ref {
    Kind kind = K_NONE;
    int index = -1, group = -1;
    bool operator==(const Ref &o) const {
      return kind == o.kind && index == o.index && group == o.group;
    }
  };

  void prepare() override;
  void closed() override;
  void reload();
  void load_tiles();
  // Saves the groups; |emptied|: the group a tile just left, removed when
  // that was its last tile.
  void save_tiles(int emptied = -1);
  static void poll_applications(void *);
  // The application catalog published a new snapshot (app_catalog.h).
  static void catalog_changed(void *);
  void apply_catalog();
  bool busy() const;
  void filter();
  void metrics();
  // Lays the items out; cheap when nothing changed since the last call.
  void build_items();
  void invalidate() { ++layout_rev_; }
  void layout_rail();
  void layout_list();
  void layout_tiles();
  const sde::DesktopEntry *app(const std::string &id) const;
  void activate(const Item &it);
  void launch(const Item &it);
  void rail_action(int index);
  // Fl_Menu_Item::popup() for the menus below; typing in one searches.
  const Fl_Menu_Item *run_menu(const Fl_Menu_Item *items, int x, int y);
  void filter_menu();
  void tile_menu(const Item &it, int px, int py);
  void group_menu(int group, int px, int py);
  void row_menu(const Item &it, int px, int py);
  void background_menu(int px, int py);
  void start_rename(int group);
  void move_selection(int key);
  void move_zone(bool back);  // Tab / Shift+Tab
  void move_end(bool end);    // Home / End
  void ensure_visible(const Item &it);
  void scroll_by(Kind area, int dy);
  // Scrolls |area| to |offset|: at once, or gliding there (smooth scrolling).
  void scroll_to(Kind area, int offset, bool smooth);
  static void glide(void *);
  void rehover();
  const Area *area(Kind kind) const;
  const Area *area_at(int x, int y) const;
  void drag_scrollbar(int y);
  const Item *find(const Ref &r) const;
  const Item *item_at(int x, int y) const;
  int edge_at(int x, int y) const;  // 1 width, 2 height, 3 both
  void apply_size(int w, int h);
  void drop_target(int x, int y, int &group, int &index) const;
  void drop(int x, int y);
  void redirect_to_search(const std::string &text);
  bool handing_to_search_ = false;
  void start_hover(const Ref &r);
  static void animate(void *);
  // Repaints one item (and |grow| pixels around it), or one scrolled area.
  void damage_item(const Ref &r, int grow = 0);
  void damage_area(Kind kind);
  void damage_tooltip(const Ref &r);
  // Icons of the list rows and tiles, loaded on idle after the panel
  // started, so that opening and scrolling never wait for the disk.
  void schedule_preload(double delay);
  static void preload(void *);
  void qa_dump_menu();
  static void qa_dump_later(void *);
  double qa_dumped_ = 0;
  bool perf_layout_ = false;  // log the layout with the first frame (menu_perf.h)

  sde::Theme theme_;
  sde::Effects effects_;
  bool smooth_ = true;  // smooth scrolling ([appearance] low_resource turns it off)
  Fl_Input *rename_;  // group name editor
  int rename_group_ = -1;
  // The applications shown (kept while a context menu or a drag runs; a
  // newer snapshot is taken when the menu is not busy).
  app_catalog::SnapshotPtr apps_;
  bool apps_stale_ = false;
  std::vector<std::string> categories_;
  std::vector<const sde::DesktopEntry *> sorted_, list_apps_, added_, most_used_;
  std::vector<sde::menu::TileGroup> groups_;
  std::vector<sde::menu::Usage> usage_;
  bool tiles_default_ = false;  // not saved yet: follows the installed applications
  std::vector<Item> items_;
  std::vector<Area> areas_;
  struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  std::vector<Rect> group_rects_;  // heading and tiles of each group (window coordinates)
  Rect new_group_;                 // drop zone for a new group while dragging
  // The account button's popup (account.h).
  std::unique_ptr<AccountPopup> account_;

  int category_ = -1;  // list filter, -1 all applications
  int list_scroll_ = 0, tiles_scroll_ = 0;
  // Smooth scrolling: where each area glides to.
  struct Glide {
    bool on = false;
    double from = 0, start = 0;
    int to = 0;
  };
  Glide list_glide_, tiles_glide_;
  double glide_frame_ = 0;  // when glide() last drew
  bool letters_open_ = false, rail_open_ = false;
  // What the items were last laid out for (build_items()).
  unsigned layout_rev_ = 1, built_rev_ = 0;
  int built_w_ = -1, built_h_ = -1, built_list_ = -1, built_tiles_ = -1, built_category_ = -2;
  bool built_letters_ = false, built_rail_ = false, built_dragging_ = false;
  int bar_hot_ = 0;  // scrollbar under the pointer: 1 list, 2 tiles
  // The Super key closes the menu (the menu holds the keyboard, so the
  // window manager's Super binding cannot see it).
  bool super_down_ = false, super_alone_ = false;
  // Icons waiting to be preloaded: application, size (of preload_apps_).
  std::vector<std::pair<const sde::DesktopEntry *, int>> preload_;
  app_catalog::SnapshotPtr preload_apps_;
  size_t preload_next_ = 0;
  unsigned preload_rev_ = 0;
  Kind drag_area_ = K_NONE;  // scrollbar drag
  int drag_y_ = 0, drag_scroll_ = 0;
  // Pressed tile or list row, and its drag to a new place.
  Ref press_;
  int press_x_ = 0, press_y_ = 0, drag_x_ = 0, drag_y2_ = 0;
  bool dragging_ = false;
  std::string drag_id_;
  sde::menu::TileSize drag_size_ = sde::menu::TileSize::Medium;
  // Resizing the menu by its edge.
  int resize_edge_ = 0, resize_rx_ = 0, resize_ry_ = 0, resize_w_ = 0, resize_h_ = 0;
  int cursor_edge_ = 0;
  bool menu_busy_ = false;
  Ref hover_, selected_;
  bool keyboard_ = false;

  // Arrangement from [menu] in sde.conf.
  bool show_added_ = true, show_most_used_ = true;

  // Layout (window coordinates), from metrics().
  int pad_ = 12, rail_w_ = 48, list_x_ = 48, list_w_ = 260, tiles_x_ = 0, tiles_w_ = 0;
  int header_h_ = 44, list_top_ = 44, bottom_ = 0;
  int cell_ = 46, gap_ = 4, group_w_ = 296, group_cols_ = 1;
  bool compact_ = false;

  // Animations: hover progress of one item and the launch effect.
  Ref anim_;
  double anim_hover_ = 0, anim_start_ = 0;
  bool launching_ = false;
  double launch_start_ = 0, launch_cx_ = 0, launch_cy_ = 0, launch_size_ = 32;
  std::string launch_icon_, launch_effect_;
};
