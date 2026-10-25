// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2009-2013 Canonical Ltd.
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's departments: the category menu of Ubuntu Software Center
// (softwarecenter/db/categories.py, data/software-center.menu.in), ported
// to C++. A menu file holds <Menu> elements with a <Name>, an <SCIcon>, an
// <Include> query and sub-menus:
//
//   <Include><And>
//     <Category>Utility</Category>
//     <Not><Category>Accessibility</Category></Not>
//   </And></Include>
//
// Query elements: <And>, <Or>, <Not>, <Category> (freedesktop category),
// <SCSection> (Debian section, with or without a component prefix),
// <SCPkgname>, <SCPkgnameWildcard> (glob), <SCType> (AppStream component
// kind; "application" means a desktop application) and, new in Merkadu,
// <SCAppId> (AppStream id) and <SCSource> (debian, flatpak or snap). An
// empty <Include> matches everything; <OnlyUnallocated/> keeps only the apps
// that no sibling category takes. <Flags>, <SCSortMode> and <SCItemLimit>
// are kept for menus written for Ubuntu Software Center.
//
// The categories of USC's recommendation and rating services (dead) are
// not supported.
#pragma once

#include <string>
#include <vector>

#include "model.h"

namespace sde {
namespace merkadu {

struct Query {
  enum Kind { All, And, Or, Not, Category, Section, Pkgname, PkgnameWildcard, Type, AppId, SourceIs };
  Kind kind = All;
  std::string value;  // lower case
  std::vector<Query> children;
  bool matches(const App &app) const;
};

struct Category {
  std::string id;    // "games", "graphics/photography" (slug of the untranslated names)
  std::string name;  // untranslated (translated with title())
  std::string icon;  // freedesktop icon name ("" when none)
  Query query;
  bool only_unallocated = false;
  std::vector<std::string> flags;
  int sort_mode = 0;
  int item_limit = 0;
  std::vector<Category> subcategories;
  std::string title() const;
};

class Categories {
 public:
  // Adds the menus of |xml|; false (with |error|) when it is not a menu.
  bool parse(const std::string &xml, std::string *error = nullptr);
  // data_dir()/categories.menu, then data_dir()/menu.d/*.menu and
  // /etc/merkadu/menu.d/*.menu (sorted). Returns the number of files read.
  int load();

  const std::vector<Category> &top() const { return top_; }
  // Top-level categories in display order: by title, with "Developer
  // Tools" and "System" last (as in Ubuntu Software Center).
  std::vector<const Category *> sorted() const;
  const Category *find(const std::string &id) const;
  // The parent of a subcategory (nullptr for top-level ones).
  const Category *parent_of(const Category *c) const;
  // Whether |app| belongs to |c| (its own query or a subcategory's, minus
  // what siblings take for <OnlyUnallocated/>).
  bool matches(const Category &c, const App &app) const;
  // The first top-level category (sorted()) of |app|; nullptr when none.
  const Category *category_of(const App &app) const;
  // Apps of |c| in |catalog| sorted by name.
  std::vector<const App *> apps_in(const Category &c, const Catalog &catalog) const;

 private:
  bool own_match(const Category &c, const App &app) const;
  const std::vector<Category> *siblings_of(const Category &c) const;
  std::vector<Category> top_;
};

// Snap Store categories (snapd /v2/find?category=) that correspond to a
// department ("graphics" -> {"art-and-design", "photo-and-video"}).
std::vector<std::string> snap_categories(const std::string &category_id);

}  // namespace merkadu
}  // namespace sde
