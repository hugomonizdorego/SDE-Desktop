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
int match_score(const DesktopEntry &e, const std::vector<std::string> &terms);

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

// Index heading for a name in the alphabetical list: "A".."Z", "#" for
// digits and symbols, or the first (non-ASCII) UTF-8 character.
std::string index_letter(const std::string &name);

// Applications sorted by name, case-insensitively.
std::vector<const DesktopEntry *> alphabetical(const std::vector<DesktopEntry> &apps);

// Icon name for a file or folder found by search.
std::string file_icon(const std::string &path, bool dir);

// Names of files and folders below a few roots (not hidden, limited depth
// and count), searched by name.
class FileIndex {
 public:
  void build(const std::vector<std::string> &roots, size_t limit = 6000, int depth = 3);
  bool built() const { return built_; }
  void clear();
  size_t size() const { return items_.size(); }
  // Paths whose base name contains every term; names starting with the first
  // term come first, folders before files of equal rank.
  std::vector<std::string> search(const std::vector<std::string> &terms, size_t max) const;
  bool is_dir(const std::string &path) const;

 private:
  struct Item {
    std::string path, name_lower;
    bool dir;
  };
  void scan(const std::string &dir, int depth, size_t limit);
  std::vector<Item> items_;
  bool built_ = false;
};

}  // namespace menu
}  // namespace sde
