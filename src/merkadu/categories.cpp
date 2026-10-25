// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2009-2013 Canonical Ltd.
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Port of CategoriesParser (Ubuntu Software Center, softwarecenter/db/
// categories.py, by Michael Vogt and the USC authors): the same menu format
// and the same rules, evaluated on Merkadu's apps instead of a Xapian query.
#include "categories.h"

#include <fnmatch.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>

#include "i18n.h"
#include "util.h"

namespace sde {
namespace merkadu {

namespace {

// The names of the shipped menus (data/merkadu/categories.menu), so that
// they are in the translation template.
const char *const kNames[] = {
    N_("Accessories"), N_("Universal Access"), N_("Developer Tools"), N_("Debugging"),
    N_("Graphic Interface Design"), N_("Haskell"), N_("IDEs"), N_("Java"), N_("Libraries"),
    N_("Lisp"), N_("Localization"), N_("Mono/CLI"), N_("OCaml"), N_("Perl"), N_("Profiling"),
    N_("Python"), N_("Ruby"), N_("Version Control"), N_("Web Development"), N_("Education"),
    N_("Science & Engineering"), N_("Astronomy"), N_("Biology"), N_("Chemistry"),
    N_("Computer Science & Robotics"), N_("Electronics"), N_("Engineering"), N_("Geography"),
    N_("Geology"), N_("Mathematics"), N_("Physics"), N_("Fonts"), N_("Games"), N_("Arcade"),
    N_("Board Games"), N_("Card Games"), N_("Puzzles"), N_("Role Playing"), N_("Simulation"),
    N_("Sports"), N_("Graphics"), N_("3D Graphics"), N_("Drawing"), N_("Painting & Editing"),
    N_("Photography"), N_("Publishing"), N_("Scanning & OCR"), N_("Viewers"), N_("Internet"),
    N_("Chat"), N_("File Sharing"), N_("Mail"), N_("Web Browsers"), N_("Sound & Video"),
    N_("Audio"), N_("Video"), N_("Office"), N_("Themes & Tweaks"), N_("System"),
    N_("Books & Magazines"),
};

std::string text_of(xmlNode *n) {
  xmlChar *c = xmlNodeGetContent(n);
  std::string s = c ? reinterpret_cast<const char *>(c) : "";
  if (c) xmlFree(c);
  return trim(s);
}

std::string name_of(xmlNode *n) { return reinterpret_cast<const char *>(n->name); }

std::string slug(const std::string &name) {
  std::string out;
  for (unsigned char c : name) {
    if (std::isalnum(c)) {
      out += char(std::tolower(c));
    } else if (!out.empty() && out.back() != '-') {
      out += '-';
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

std::string lower_copy(std::string s) {
  for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool leaf(const std::string &tag, Query &q) {
  if (tag == "Category")
    q.kind = Query::Category;
  else if (tag == "SCSection")
    q.kind = Query::Section;
  else if (tag == "SCPkgname")
    q.kind = Query::Pkgname;
  else if (tag == "SCPkgnameWildcard")
    q.kind = Query::PkgnameWildcard;
  else if (tag == "SCType")
    q.kind = Query::Type;
  else if (tag == "SCAppId")
    q.kind = Query::AppId;
  else if (tag == "SCSource")
    q.kind = Query::SourceIs;
  else
    return false;
  return true;
}

// <And>, <Or> and <Not> with their children (unknown elements are ignored,
// as USC did with a warning).
Query parse_query(xmlNode *n) {
  Query q;
  std::string tag = name_of(n);
  if (tag == "And")
    q.kind = Query::And;
  else if (tag == "Or")
    q.kind = Query::Or;
  else if (tag == "Not")
    q.kind = Query::Not;
  else if (leaf(tag, q)) {
    q.value = q.kind == Query::AppId ? normalize_id(text_of(n)) : lower_copy(text_of(n));
    return q;
  } else {
    q.kind = Query::All;
    return q;
  }
  for (xmlNode *c = n->children; c; c = c->next)
    if (c->type == XML_ELEMENT_NODE) q.children.push_back(parse_query(c));
  return q;
}

// <Include>: the first <And>/<Or>/<Category> child; empty matches all.
Query parse_include(xmlNode *n) {
  for (xmlNode *c = n->children; c; c = c->next)
    if (c->type == XML_ELEMENT_NODE) return parse_query(c);
  return Query();
}

bool parse_menu(xmlNode *n, const std::string &parent, Category &out) {
  bool has_query = false;
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE) continue;
    std::string tag = name_of(c);
    if (tag == "Name" || tag == "_Name") {
      // Inline translations (xml:lang) are ignored; gettext translates.
      if (xmlHasProp(c, reinterpret_cast<const xmlChar *>("lang"))) continue;
      out.name = text_of(c);
    } else if (tag == "SCIcon") {
      out.icon = text_of(c);
    } else if (tag == "Include") {
      out.query = parse_include(c);
      has_query = true;
    } else if (tag == "OnlyUnallocated") {
      out.only_unallocated = true;
    } else if (tag == "Flags") {
      for (xmlNode *f = c->children; f; f = f->next)
        if (f->type == XML_ELEMENT_NODE) out.flags.push_back(text_of(f));
    } else if (tag == "SCSortMode") {
      out.sort_mode = std::atoi(text_of(c).c_str());
    } else if (tag == "SCItemLimit") {
      out.item_limit = std::atoi(text_of(c).c_str());
    }
  }
  if (out.name.empty() || !has_query) return false;
  out.id = (parent.empty() ? "" : parent + "/") + slug(out.name);
  for (xmlNode *c = n->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE || name_of(c) != "Menu") continue;
    Category sub;
    if (parse_menu(c, out.id, sub)) out.subcategories.push_back(std::move(sub));
  }
  return true;
}

const Category *find_in(const std::vector<Category> &list, const std::string &id) {
  for (auto &c : list) {
    if (c.id == id) return &c;
    if (const Category *s = find_in(c.subcategories, id)) return s;
  }
  return nullptr;
}

const Category *parent_in(const std::vector<Category> &list, const Category *child) {
  for (auto &c : list) {
    for (auto &s : c.subcategories)
      if (&s == child) return &c;
    if (const Category *p = parent_in(c.subcategories, child)) return p;
  }
  return nullptr;
}

}  // namespace

bool Query::matches(const App &app) const {
  switch (kind) {
    case All:
      return true;
    case And:
      for (auto &c : children)
        if (!c.matches(app)) return false;
      return true;
    case Or:
      for (auto &c : children)
        if (c.matches(app)) return true;
      return children.empty();
    case Not:
      for (auto &c : children)
        if (c.matches(app)) return false;
      return true;
    case Category:
      for (auto &cat : app.categories)
        if (lower_copy(cat) == value) return true;
      return false;
    case Section: {
      std::string s = lower_copy(app.section);
      return !s.empty() && (s == value || ends_with(s, "/" + value) || ends_with(value, "/" + s));
    }
    case Pkgname:
      for (auto &p : app.pkgnames)
        if (p == value) return true;
      return false;
    case PkgnameWildcard:
      for (auto &p : app.pkgnames)
        if (fnmatch(value.c_str(), p.c_str(), 0) == 0) return true;
      return false;
    case Type:
      if (value == "application") return app.is_app();
      return lower_copy(app.kind) == value;
    case AppId:
      return app.id == value;
    case SourceIs: {
      Source s;
      return source_from_id(value, &s) && app.has_source(s);
    }
  }
  return false;
}

std::string Category::title() const { return name.empty() ? name : std::string(_(name.c_str())); }

bool Categories::parse(const std::string &xml, std::string *error) {
  (void)kNames;
  xmlDoc *doc = xmlReadMemory(xml.data(), int(xml.size()), "menu.xml", nullptr,
                              XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (!doc) {
    if (error) *error = "not well-formed XML";
    return false;
  }
  xmlNode *root = xmlDocGetRootElement(doc);
  if (!root || name_of(root) != "Menu") {
    if (error) *error = "the root element is not <Menu>";
    xmlFreeDoc(doc);
    return false;
  }
  for (xmlNode *c = root->children; c; c = c->next) {
    if (c->type != XML_ELEMENT_NODE || name_of(c) != "Menu") continue;
    Category cat;
    if (!parse_menu(c, "", cat)) continue;
    // A drop-in replaces a category of the same name.
    auto same = std::find_if(top_.begin(), top_.end(), [&](const Category &t) { return t.id == cat.id; });
    if (same != top_.end())
      *same = std::move(cat);
    else
      top_.push_back(std::move(cat));
  }
  xmlFreeDoc(doc);
  return true;
}

int Categories::load() {
  int files = 0;
  std::vector<std::string> paths = {join_path(data_dir(), "categories.menu")};
  for (const std::string &dir : {join_path(data_dir(), "menu.d"), std::string("/etc/merkadu/menu.d")}) {
    std::vector<std::string> names = list_dir(dir);
    std::sort(names.begin(), names.end());
    for (auto &n : names)
      if (ends_with(n, ".menu")) paths.push_back(join_path(dir, n));
  }
  for (auto &p : paths) {
    std::string text;
    if (read_file(p, text) && parse(text)) ++files;
  }
  return files;
}

std::vector<const Category *> Categories::sorted() const {
  std::vector<const Category *> out;
  for (auto &c : top_) out.push_back(&c);
  auto rank = [](const Category *c) { return c->name == "System" ? 2 : c->name == "Developer Tools" ? 1 : 0; };
  std::stable_sort(out.begin(), out.end(), [&](const Category *a, const Category *b) {
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    return lower_copy(a->title()) < lower_copy(b->title());
  });
  return out;
}

const Category *Categories::find(const std::string &id) const { return find_in(top_, id); }

const Category *Categories::parent_of(const Category *c) const { return parent_in(top_, c); }

const std::vector<Category> *Categories::siblings_of(const Category &c) const {
  if (const Category *p = parent_of(&c)) return &p->subcategories;
  return &top_;
}

bool Categories::own_match(const Category &c, const App &app) const {
  if (c.query.matches(app)) return true;
  for (auto &s : c.subcategories)
    if (own_match(s, app)) return true;
  return false;
}

bool Categories::matches(const Category &c, const App &app) const {
  if (!own_match(c, app)) return false;
  if (!c.only_unallocated) return true;
  for (auto &other : *siblings_of(c))
    if (&other != &c && !other.only_unallocated && own_match(other, app)) return false;
  return true;
}

const Category *Categories::category_of(const App &app) const {
  for (const Category *c : sorted())
    if (!c->only_unallocated && matches(*c, app)) return c;
  for (const Category *c : sorted())
    if (c->only_unallocated && matches(*c, app)) return c;
  return nullptr;
}

std::vector<const App *> Categories::apps_in(const Category &c, const Catalog &catalog) const {
  std::vector<const App *> out;
  for (const App &a : catalog.apps())
    if (matches(c, a)) out.push_back(&a);
  sort_by_name(out);
  return out;
}

std::vector<std::string> snap_categories(const std::string &category_id) {
  std::string top = category_id.substr(0, category_id.find('/'));
  if (top == "accessories") return {"utilities"};
  if (top == "developer-tools") return {"development"};
  if (top == "education") return {"education"};
  if (top == "science-engineering") return {"science"};
  if (top == "games") return {"games"};
  if (top == "graphics") return {"art-and-design", "photo-and-video"};
  if (top == "internet") return {"social"};
  if (top == "sound-video") return {"music-and-audio", "photo-and-video"};
  if (top == "office") return {"productivity"};
  if (top == "themes-tweaks") return {"personalisation"};
  if (top == "books-magazines") return {"books-and-reference"};
  if (top == "system") return {"server-and-cloud", "security"};
  return {};
}

}  // namespace merkadu
}  // namespace sde
