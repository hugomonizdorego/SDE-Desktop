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
//
// 0.2 Alpha 2: tiles are dragged between groups, applications from the
// list or from the search results into a group, onto empty space for a new
// group; a group name is renamed by a click and moved by a drag; a group
// without tiles has a remove button. While dragging, a bar shows where the
// tile goes, the group under the pointer is highlighted and the tiles
// scroll near their top and bottom edges.
//
// 0.2 Alpha 3: the tiles start out as favorites in named groups again
// (start_tiles below). A tile dropped onto another makes a folder (its
// applications' small icons and its name); a click opens it in place below
// its row, where its applications are tiles to open, rearrange or drag
// out. Tiles, folders, list rows, search results and taskbar buttons show
// live badges (badges.h). See docs/START-MENU.md.
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
// groups saved in [menu] tiles. 0.2 Alpha 3: without saved tiles, the
// default favorites of the installed applications (menu::favorite_groups());
// the automatic groups of older versions give way to them
// (menu::adopt_favorites()).
namespace start_tiles {
// Group names by menu::TileCategory, translated (0.2 Alpha 2's groups, the
// names of new folders).
std::vector<std::string> group_names();
// The favorites groups by menu::FavoriteGroup, translated.
std::vector<std::string> favorite_names();
std::vector<sde::menu::TileGroup> load();
void save(const std::vector<sde::menu::TileGroup> &groups);
// Whether load() shows SDE's own tiles, which follow the installed
// applications: none saved, or an older version's not saved again yet.
bool automatic();
// Every pinned application, those in folders too.
std::vector<std::string> ids();
bool pinned(const std::string &id);
// Into the favorites group of its kind (menu::pin_favorite()).
void pin(const std::string &id);
// Out of its group or folder (a folder left with one application becomes
// its tile).
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
  // An application dragged out of the search results (0.2 Alpha 2): the
  // open menu goes on with the drag, the mouse button still down, and drops
  // it into the tile group under the pointer.
  void start_drag(const std::string &id);
  static AppMenu *instance;

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
    K_GROUP_REMOVE,  // remove button of a group without tiles; index: group
    K_HEADING,     // text only; index 5: the drop hint of a group without tiles
    // 0.2 Alpha 3 folders: a folder tile (index: tile number, group:
    // group), and in the open folder its applications (index: their place
    // in the folder) and its name (index: tile number).
    K_FOLDER,
    K_FOLDER_APP,
    K_FOLDER_NAME,
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
  // The tiles of the tiles area: applications, folders and the open
  // folder's applications.
  static bool tile_like(Kind k) { return k == K_TILE || k == K_FOLDER || k == K_FOLDER_APP; }

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
  // 0.2 Alpha 3: a live badge changed (badges.h): an open menu repaints its
  // tiles and its list.
  static void badges_changed(void *);
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
  // A group's name, or "Name group" for one without a name.
  std::string group_label(int group) const;
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
  // 0.2 Alpha 3: the right-click menus of a folder and of an application in
  // the open folder.
  void folder_menu(const Item &it, int px, int py);
  void folder_app_menu(const Item &it, int px, int py);
  // Renames group |group|, or with |folder| the open folder.
  void start_rename(int group, bool folder = false);
  // Opens the folder at (group, index) in place, gliding open (another open
  // folder closes); closes the open one, gliding shut.
  void expand_folder(int group, int index, bool animate = true);
  void collapse_folder(bool animate = true);
  bool folder_expanded() const { return folder_group_ >= 0; }
  static void folder_tick(void *);
  // The open folder after the tiles changed: the folder that holds most of
  // |apps| (the applications it held), else it closes.
  void find_open_folder(const std::vector<std::string> &apps);
  // The installed applications of the folder at (group, index).
  std::vector<const sde::DesktopEntry *> folder_entries(int group, int index) const;
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
  // Where the dragged tile would go at (x, y): false outside the tiles.
  bool drop_target(int x, int y, sde::menu::TileDrop &drop) const;
  void drop(int x, int y);
  // Where a dragged group goes: the index to insert it at (-1: outside the
  // tiles); |anchor| and |below| tell where to draw the bar.
  int group_target(int x, int y, int *anchor = nullptr, bool *below = nullptr) const;
  void drop_group(int x, int y);
  // Near the top or bottom of the tiles a drag scrolls them, faster closer
  // to the edge: pixels per frame (0: not there).
  int autoscroll_speed() const;
  static void autoscroll(void *);
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
  // Pressed tile, list row or group name, and its drag to a new place.
  Ref press_;
  int press_x_ = 0, press_y_ = 0, drag_x_ = 0, drag_y2_ = 0;
  bool dragging_ = false;
  int drag_group_ = -1;  // the group being moved by its name, else a tile or application
  std::string drag_id_;  // the application dragged ("" for a folder)
  sde::menu::TileSize drag_size_ = sde::menu::TileSize::Medium;
  // 0.2 Alpha 3: a folder dragged by its tile, at (group, index).
  int drag_folder_group_ = -1, drag_folder_index_ = -1;

  // 0.2 Alpha 3: the open folder (its group and tile number) and how far
  // it is open (0..1, eased while it glides open or shut).
  int folder_group_ = -1, folder_index_ = -1;
  std::vector<std::string> folder_key_;  // its applications, to find it again
  double folder_open_ = 0, folder_from_ = 0, folder_to_ = 0, folder_start_ = 0;
  bool renaming_folder_ = false;
  bool rename_when_open_ = false;  // a new folder: its name is asked once it is open
  Rect folder_band_;  // where the open folder shows its applications (window coordinates)
  int folder_notch_ = 0;  // x of the folder tile's middle, where the band points
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
