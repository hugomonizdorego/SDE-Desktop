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

#include <string>
#include <vector>

#include "desktop_entry.h"
#include "effects.h"
#include "menu_model.h"
#include "paint.h"
#include "popup.h"
#include "theme.h"

// Start tiles from other places (search results, the panel): the tile
// groups saved in [menu] tiles.
namespace start_tiles {
std::vector<sde::menu::TileGroup> load();
void save(const std::vector<sde::menu::TileGroup> &groups);
std::vector<std::string> ids();
bool pinned(const std::string &id);
void pin(const std::string &id);
void unpin(const std::string &id);
}  // namespace start_tiles

class AppMenu : public PanelPopup {
 public:
  AppMenu();
  ~AppMenu() override;

  void draw() override;
  int handle(int event) override;
  // The group name editor commits or cancels (Enter, Escape, focus lost).
  void finish_rename(bool commit);

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
  void save_tiles();
  static void poll_applications(void *);
  void filter();
  void metrics();
  void build_items();
  void layout_rail();
  void layout_list();
  void layout_tiles();
  const sde::DesktopEntry *app(const std::string &id) const;
  void activate(const Item &it);
  void launch(const Item &it);
  void rail_action(int index);
  void filter_menu();
  void tile_menu(const Item &it, int px, int py);
  void group_menu(int group, int px, int py);
  void row_menu(const Item &it, int px, int py);
  void background_menu(int px, int py);
  void start_rename(int group);
  void move_selection(int key);
  void ensure_visible(const Item &it);
  void scroll_by(Kind area, int dy);
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
  void qa_dump_menu();

  sde::Theme theme_;
  sde::Effects effects_;
  Fl_Input *rename_;  // group name editor
  int rename_group_ = -1;
  std::vector<sde::DesktopEntry> apps_;
  std::string apps_stamp_;
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
  cairo_surface_t *avatar_ = nullptr;
  std::string full_name_;

  int category_ = -1;  // list filter, -1 all applications
  int list_scroll_ = 0, tiles_scroll_ = 0;
  bool letters_open_ = false, rail_open_ = false;
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
