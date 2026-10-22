// SPDX-License-Identifier: GPL-3.0-or-later
// sde-desktop drag and drop model (alpha 52), without FLTK or X:
//  * which action a drop performs (move, copy, link, ask), following the
//    Liria (Caja) and Nautilus conventions;
//  * text/uri-list encoding and decoding, and what a dropped payload is
//    (files, web links or plain text);
//  * "Keep both" names ("Photo (2).jpg");
//  * the icon position store (~/.config/sde/desktop-positions.ini) and the
//    grid placement that uses it.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace sde {
namespace desk {

// ---------------------------------------------------------------- actions

enum class DropAction { NoDrop, Move, Copy, Link, Ask };

// Keyboard modifiers held during a drag.
enum : unsigned { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4 };

// What the drag source allows (XdndActionList; everything when the source
// does not say).
struct ActionSet {
  bool move = true, copy = true, link = true, ask = true;
  bool allows(DropAction a) const;
};

// What the drop lands on.
enum class TargetKind { Desktop, Folder, Trash, Launcher, NoTarget };

// The action for a drop of local files:
//   Trash: always "move to the trash" (reported as Move);
//   Launcher (.desktop application): open the files with it (reported as
//   Copy, the source keeps its files);
//   otherwise Ctrl+Shift links, Ctrl copies, Shift moves, Alt (or a source
//   that suggests XdndActionAsk) asks, and with no modifier the files are
//   moved within one file system and copied across file systems.
// An action the source does not allow falls back to Copy, then Move; None
// when nothing fits.
DropAction choose_drop_action(TargetKind target, unsigned mods, bool same_fs, DropAction suggested,
                              const ActionSet &allowed);

// "XdndActionMove" <-> DropAction.
const char *xdnd_action_name(DropAction a);
DropAction xdnd_action_from_name(const std::string &name);

// --------------------------------------------------------------------- URIs

// "/home/a b" -> "file:///home/a%20b" (RFC 3986 unreserved and "/" kept).
std::string uri_from_path(const std::string &path);
// "file:///home/a%20b" (or "file://localhost/...") -> "/home/a b"; "" for
// anything that is not a local file URI.
std::string path_from_uri(const std::string &uri);
std::string percent_decode(const std::string &s);
// text/uri-list: one URI per line (CRLF or LF), "#" comments skipped.
std::vector<std::string> parse_uri_list(const std::string &text);
// Paths -> "file:///a\r\nfile:///b\r\n".
std::string format_uri_list(const std::vector<std::string> &paths);

// A dropped payload, sorted out.
struct DropPayload {
  std::vector<std::string> files;  // local paths
  std::vector<std::string> urls;   // http, https, ftp, mailto ... links
  std::string text;                // anything else (plain text)
  bool empty() const { return files.empty() && urls.empty() && text.empty(); }
};
// |uri_list| when the data came as text/uri-list. Plain text that is only
// file URIs, absolute paths or links (one per line) counts as those.
DropPayload classify_drop(const std::string &data, bool uri_list);

// File name for a dropped link: "https://www.example.org/a/b" ->
// "www.example.org.desktop"; mailto:x@y -> "x@y.desktop".
std::string link_file_name(const std::string &url);
// A Type=Link desktop entry for |url|.
std::string link_desktop_entry(const std::string &url, const std::string &name);

// ---------------------------------------------------------------- names

// "Photo.jpg" -> "Photo (2).jpg", "(3)" ... until |exists| is false;
// "archive.tar.gz" -> "archive (2).tar.gz"; folders and dot files keep
// the whole name as the stem.
std::string keep_both_name(const std::string &name, bool is_dir,
                           bool (*exists)(const std::string &name, void *data), void *data);

// ---------------------------------------------------------------- positions

struct Cell {
  int col = 0, row = 0;
  bool operator==(const Cell &o) const { return col == o.col && row == o.row; }
  bool operator<(const Cell &o) const { return col != o.col ? col < o.col : row < o.row; }
};

// Icon positions per file name (places use "@home", "@trash",
// "@computer"), stored as grid cells so that they survive icon size and
// screen changes. Auto-arrange ignores them.
class PositionStore {
 public:
  bool load(const std::string &path);
  bool save() const;
  // Parses / produces the file contents (the path is not touched).
  void parse(const std::string &text);
  std::string format() const;

  bool get(const std::string &key, Cell *out) const;
  void set(const std::string &key, Cell c);
  void erase(const std::string &key);
  // Keeps the position of a renamed file.
  void rename(const std::string &from, const std::string &to);
  void clear() { cells_.clear(); }
  size_t size() const { return cells_.size(); }
  // Drops entries whose key is not in |keep|.
  void prune(const std::vector<std::string> &keep);

  bool auto_arrange = false;
  const std::string &path() const { return path_; }

  static std::string escape_key(const std::string &key);
  static std::string unescape_key(const std::string &key);

 private:
  std::string path_;
  std::map<std::string, Cell> cells_;
};

// ~/.config/sde/desktop-positions.ini
std::string default_positions_path();

// Places |keys| (already in display order) on a |cols| x |rows| grid,
// filled column by column. Stored positions win (first come in |keys|
// order when two claim one cell); the others take the first free cells.
// When the grid is full the extra icons get columns beyond |cols|.
std::vector<Cell> place_icons(const std::vector<std::string> &keys, int cols, int rows,
                              const PositionStore &store);

}  // namespace desk
}  // namespace sde
