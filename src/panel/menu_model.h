// SPDX-License-Identifier: GPL-3.0-or-later
// Data behind the SDE application menu, kept free of FLTK so it can be unit
// tested: search scoring, favorites and recent lists, the alphabetical
// "All Programs" index and a small file name index of the home folder.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "desktop_entry.h"

namespace sde {
namespace menu {

// Higher is better; 0 means "does not match". |terms| are lower case.
// match_keys(search_keys(e), terms) for one entry.
int match_score(const DesktopEntry &e, const std::vector<std::string> &terms);

// ---------------------------------------------------------------- search
// Text folded for matching: lower case, Latin letters without accents
// (E acute "crire" -> "ecrire", "Stra" sharp-s "e" -> "strasse"), Greek and
// Cyrillic in lower case; other characters stay as they are.
std::string fold(const std::string &text);
// One line of text to draw: control characters become spaces, bytes that
// are not UTF-8 are read as Windows-1252 (legacy Latin-1 files), outer
// spaces are removed.
std::string clean_text(const std::string &text);

// Search keys of an application, folded once (the panel keeps them with
// its application list, see src/panel/app_catalog.h).
struct SearchKeys {
  std::string name, generic, comment, exec;
  std::vector<std::string> keywords;
  std::vector<std::string> id_parts;  // "org.gnome.Nautilus.desktop": gnome, nautilus
};
SearchKeys search_keys(const DesktopEntry &e);
// The folded terms of a query, split at spaces.
std::vector<std::string> search_terms(const std::string &query);
// Higher is better, 0: no match; every term must match. Per term: name
// prefix 100, start of a word of the name 80, inside the name 60, start of a
// word of the generic name 45, inside it 40, the program 35, a keyword (or a
// word of one) 30, a part of the desktop file ID 25, inside the comment 15,
// the letters of the name in order from a word start (3 letters or more:
// "lbo" finds LibreOffice) 8.
int match_keys(const SearchKeys &k, const std::vector<std::string> &terms);

// "a.desktop;b.desktop;" <-> {"a.desktop", "b.desktop"} (trimmed, no
// duplicates, no empty items).
std::vector<std::string> parse_id_list(const std::string &list);
std::string join_id_list(const std::vector<std::string> &ids);

// Moves |id| to the front of |list|, keeping at most |cap| items.
void remember(std::vector<std::string> &list, const std::string &id, size_t cap);

// A sensible first set of favorites from the installed applications: a file
// manager, web browser, terminal, text editor, office and media programs...
std::vector<std::string> default_favorites(const std::vector<DesktopEntry> &apps,
                                           size_t max = 8);

// Launch history of one application, for "Recently used": how often and
// when (seconds since the epoch) it was last opened from the menu.
struct Usage {
  std::string id;
  unsigned count = 0;
  long long last = 0;
};

// The state file: one "id<TAB>count<TAB>last" line per application. Lines
// that do not parse are skipped; a bare id (the older menu-recent list) counts
// as one launch, older the further down it is.
std::vector<Usage> parse_usage(const std::string &text, long long now = 0);
std::string format_usage(const std::vector<Usage> &usage);

// Counts one launch of |id| at |now|. At most |cap| applications are kept;
// the ones with the lowest frecency are dropped first.
void record_launch(std::vector<Usage> &usage, const std::string &id, long long now,
                   size_t cap = 64);
void forget(std::vector<Usage> &usage, const std::string &id);

// Frequency weighted by recency: count * 2^(-age / one week), with a bonus
// for the last day so that something just opened shows up at once.
double frecency(const Usage &u, long long now);

// The |max| best application ids by frecency (ties: most recent first),
// leaving out |exclude| (the favorites).
std::vector<std::string> rank_frecent(const std::vector<Usage> &usage, long long now,
                                      size_t max,
                                      const std::vector<std::string> &exclude = {});
// Ids by last use, most recent first.
std::vector<std::string> by_last_use(const std::vector<Usage> &usage, size_t max);

// Index heading for a name in the alphabetical list: "A".."Z", "#" for
// digits and symbols, or the first (non-ASCII) UTF-8 character.
std::string index_letter(const std::string &name);

// Applications sorted by name, case-insensitively.
std::vector<const DesktopEntry *> alphabetical(const std::vector<DesktopEntry> &apps);

// Icon name for a file or folder found by search.
std::string file_icon(const std::string &path, bool dir);

// ---------------------------------------------------------------- tiles
// The Start menu's pinned applications are tiles in named groups. Sizes are
// measured in grid cells: small 1x1, medium 2x2, wide 4x2, large 4x4, in a
// group six cells wide.
enum class TileSize { Small, Medium, Wide, Large };
struct Tile {
  std::string id;
  TileSize size = TileSize::Medium;
};
struct TileGroup {
  std::string name;
  std::vector<Tile> tiles;
};
const int kGroupColumns = 6;
int tile_cols(TileSize s);
int tile_rows(TileSize s);
char tile_size_code(TileSize s);  // 's', 'm', 'w', 'l'
TileSize tile_size_from(char code);

// [menu] tiles: "Name,a.desktop:m,b.desktop:w;Other,c.desktop:s". Group
// names are percent-escaped (",;:%"). Empty groups are kept (a group the user
// named before dragging tiles into it); unknown sizes read as medium; an id
// is kept only once.
std::vector<TileGroup> parse_tile_groups(const std::string &text);
std::string format_tile_groups(const std::vector<TileGroup> &groups);
// Every tile id, in order (kept in [menu] favorites for older readers).
std::vector<std::string> tile_ids(const std::vector<TileGroup> &groups);
// The first tile groups: alpha15 favorites (|user_list|) become one group
// named |first|; the default favorites (|user_list| false) are split into
// |first| (up to six) and |second|.
std::vector<TileGroup> groups_from_favorites(const std::vector<std::string> &favorites,
                                             bool user_list, const std::string &first,
                                             const std::string &second);

// ------------------------------------------------- tile groups by category
// 0.2 Alpha 2: the first tile groups gather the installed applications by
// category (a browser, a mail client and a chat in Internet...), and an
// application pinned later joins the group of its category.
enum class TileCategory {
  Internet, Office, Multimedia, Graphics, Development, Games, Education, System, Accessories,
  Other  // no main category
};
const int kTileCategories = 9;  // Internet .. Accessories, the order of the groups
// The group of an application: its main menu category, with Settings and
// System together and Education and Science together.
TileCategory tile_category(const DesktopEntry &e);

// The default groups: per category the applications that fit its roles
// best, one per role first (Internet: web browser, mail, chat...; System:
// file manager, terminal, settings...), then the others; within a role the
// ones in |preferred| (SDE's own applications) first, then A-Z. At most
// |per_group| tiles each, medium and wide so that they fill whole rows;
// categories without applications are left out. |names|: the group names
// by TileCategory (kTileCategories + 1, translated by the caller).
std::vector<TileGroup> category_groups(const std::vector<DesktopEntry> &apps,
                                       const std::vector<std::string> &preferred,
                                       const std::vector<std::string> &names, size_t per_group = 6);

// Tile groups saved by 0.2 Alpha 1 and older: the tiles of the default
// groups named in |old_names| ("Productivity", "Explore") are sorted into
// category groups, which take the place of the first of them; the user's
// own groups stay as they are (a group already named like a category gets
// that category's tiles). Tiles of applications that are not installed go
// with the first new group. Groups without old names come back unchanged.
std::vector<TileGroup> migrate_tile_groups(const std::vector<TileGroup> &groups,
                                           const std::vector<std::string> &old_names,
                                           const std::vector<DesktopEntry> &apps,
                                           const std::vector<std::string> &names);

// Pins |e| as a tile ("Pin to Start"): into the group named after its
// category, else the group with the most applications of that category,
// else a new group of the category, placed in the order of the categories.
// False when it is pinned already.
bool pin_by_category(std::vector<TileGroup> &groups, const DesktopEntry &e,
                     const std::vector<DesktopEntry> &apps, const std::vector<std::string> &names,
                     TileSize size = TileSize::Medium);

// Where a tile sits in its group, in cells.
struct TileCell {
  int col = 0, row = 0, cols = 2, rows = 2;
};
// First-fit placement of |tiles| (in order) in a grid |columns| cells wide.
// |rows| receives the height of the group in cells.
std::vector<TileCell> pack_tiles(const std::vector<Tile> &tiles, int columns, int *rows);

// Names of files and folders below a few roots (not hidden, limited depth
// and count), searched by name.
class FileIndex {
 public:
  void build(const std::vector<std::string> &roots, size_t limit = 6000, int depth = 3);
  // The same in steps, so that a big home folder does not stall the panel:
  // begin(), then step() on idle until it returns true.
  void begin(const std::vector<std::string> &roots, size_t limit = 6000, int depth = 3);
  bool step(double budget_ms);
  bool building() const { return building_; }
  bool built() const { return built_; }
  void clear();
  size_t size() const { return items_.size(); }
  // Paths whose base name contains every term; names starting with the first
  // term come first, folders before files of equal rank.
  std::vector<std::string> search(const std::vector<std::string> &terms, size_t max) const;
  struct Hit {
    std::string path;
    bool dir = false;
  };
  std::vector<Hit> search_hits(const std::vector<std::string> &terms, size_t max) const;
  bool is_dir(const std::string &path) const;

 private:
  struct Item {
    std::string path, name_lower;
    bool dir;
  };
  struct Pending {
    std::string dir;
    int depth;
  };
  void scan_one(const std::string &dir, int depth);
  void finish();
  std::vector<Item> items_;
  std::vector<Pending> stack_;        // directories left to scan, depth first
  std::vector<Pending> roots_;        // roots not started yet
  size_t limit_ = 6000;
  bool built_ = false, building_ = false;
};

}  // namespace menu
}  // namespace sde
