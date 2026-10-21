// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>

#include "build_config.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "panel.h"
#include "search.h"
#include "spawn.h"
#include "suite.h"
#include "util.h"

using namespace sde;
using menu::Tile;
using menu::TileGroup;
using menu::TileSize;

namespace {

// "Recently added": applications installed in the last two weeks.
const long long kRecentlyAdded = 14 * 86400;
const size_t kMaxAdded = 3, kMaxMostUsed = 5;

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

long long epoch_now() { return (long long)time(nullptr); }

std::string category_icon(const std::string &cat) {
  if (cat == "Accessories") return "applications-accessories";
  if (cat == "Education") return "applications-education";
  if (cat == "Games") return "applications-games";
  if (cat == "Graphics") return "applications-graphics";
  if (cat == "Internet") return "applications-internet";
  if (cat == "Multimedia") return "applications-multimedia";
  if (cat == "Office") return "applications-office";
  if (cat == "Development") return "applications-development";
  if (cat == "Science") return "applications-science";
  if (cat == "Settings") return "preferences-desktop";
  if (cat == "System") return "applications-system";
  return "applications-other";
}

// Freedesktop main categories under friendlier names.
std::string category_label(const std::string &cat) {
  if (cat == "Accessories") return _("Accessories");
  if (cat == "Education") return _("Education");
  if (cat == "Games") return _("Games");
  if (cat == "Graphics") return _("Graphics");
  if (cat == "Internet") return _("Internet");
  if (cat == "Multimedia") return _("Sound & Video");
  if (cat == "Office") return _("Office");
  if (cat == "Development") return _("Development");
  if (cat == "Science") return _("Science");
  if (cat == "Settings") return _("Settings");
  if (cat == "System") return _("System");
  return _("Other");
}

std::string size_label(TileSize s) {
  switch (s) {
    case TileSize::Small: return _("Small");
    case TileSize::Medium: return _("Medium");
    case TileSize::Wide: return _("Wide");
    case TileSize::Large: return _("Large");
  }
  return "";
}

// ------------------------------------------------------------ rail glyphs

void stroke(Painter &p, const Color &c, double width) {
  p.set(c);
  cairo_set_line_width(p.cr(), width);
  cairo_set_line_cap(p.cr(), CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(p.cr(), CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(p.cr());
}

void hamburger_glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_new_path(cr);
  for (double f : {0.28, 0.5, 0.72}) {
    cairo_move_to(cr, x + s * 0.18, y + s * f);
    cairo_line_to(cr, x + s * 0.82, y + s * f);
  }
  stroke(p, c, std::max(1.2, s * 0.08));
}

void document_glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  double l = x + s * 0.22, r = x + s * 0.78, t = y + s * 0.12, b = y + s * 0.88, f = s * 0.18;
  cairo_new_path(cr);
  cairo_move_to(cr, l, t);
  cairo_line_to(cr, r - f, t);
  cairo_line_to(cr, r, t + f);
  cairo_line_to(cr, r, b);
  cairo_line_to(cr, l, b);
  cairo_close_path(cr);
  cairo_move_to(cr, r - f, t);
  cairo_line_to(cr, r - f, t + f);
  cairo_line_to(cr, r, t + f);
  for (double k : {0.48, 0.62, 0.76}) {
    cairo_move_to(cr, l + s * 0.12, y + s * k);
    cairo_line_to(cr, r - s * 0.12, y + s * k);
  }
  stroke(p, c, std::max(1.1, s * 0.07));
}

void picture_glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  double l = x + s * 0.12, r = x + s * 0.88, t = y + s * 0.2, b = y + s * 0.8;
  cairo_new_path(cr);
  cairo_rectangle(cr, l, t, r - l, b - t);
  cairo_move_to(cr, l, b - s * 0.08);
  cairo_line_to(cr, x + s * 0.4, y + s * 0.48);
  cairo_line_to(cr, x + s * 0.58, y + s * 0.66);
  cairo_line_to(cr, x + s * 0.7, y + s * 0.55);
  cairo_line_to(cr, r, b - s * 0.1);
  stroke(p, c, std::max(1.1, s * 0.07));
  p.circle(x + s * 0.67, y + s * 0.36, s * 0.06, c);
}

void person_glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_new_path(cr);
  cairo_arc(cr, x + s * 0.5, y + s * 0.36, s * 0.17, 0, 2 * M_PI);
  stroke(p, c, std::max(1.1, s * 0.07));
  cairo_new_path(cr);
  cairo_arc(cr, x + s * 0.5, y + s * 0.95, s * 0.33, M_PI * 1.15, M_PI * 1.85);
  stroke(p, c, std::max(1.1, s * 0.07));
}

void chevron_down(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_new_path(cr);
  cairo_move_to(cr, x + s * 0.28, y + s * 0.4);
  cairo_line_to(cr, x + s * 0.5, y + s * 0.62);
  cairo_line_to(cr, x + s * 0.72, y + s * 0.4);
  stroke(p, c, std::max(1.2, s * 0.09));
}

void open_folder(const std::string &path) {
  make_dirs(path);
  if (!find_program("liria").empty()) spawn_detached({"liria", "--no-desktop", path});
  else open_uri(path);
}

std::string user_folder(const char *key, const char *fallback) {
  std::string p = user_dir(key);
  if (p.empty() || p == home_dir()) p = home_dir() + "/" + fallback;
  return p;
}

// Group name editor: Enter keeps the name, Escape restores it.
class RenameInput : public Fl_Input {
 public:
  RenameInput(AppMenu *menu) : Fl_Input(0, 0, 10, 10), menu_(menu) {}
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key();
      if (k == FL_Enter || k == FL_KP_Enter || k == FL_Escape) {
        menu_->finish_rename(k != FL_Escape);
        return 1;
      }
    }
    return Fl_Input::handle(e);
  }

 private:
  AppMenu *menu_;
};

}  // namespace

// ================================================================ tiles

namespace start_tiles {

std::vector<TileGroup> load() {
  const Config &cfg = Panel::instance->config();
  const std::string sentinel = "\x01";
  std::string v = cfg.get("menu", "tiles", sentinel);
  if (v != sentinel) return menu::parse_tile_groups(v);
  // alpha15 and older: up to 12 favorites. They become the first group.
  std::string fav = cfg.get("menu", "favorites", sentinel);
  bool user = fav != sentinel;
  auto ids = user ? menu::parse_id_list(fav) : menu::default_favorites(load_applications("SDE"), 8);
  return menu::groups_from_favorites(ids, user, _("Productivity"), _("Explore"));
}

void save(const std::vector<TileGroup> &groups) {
  Panel::instance->set_setting("menu", "tiles", menu::format_tile_groups(groups));
  // Older SDE versions read the flat list.
  std::string ids = menu::join_id_list(menu::tile_ids(groups));
  Panel::instance->set_setting("menu", "favorites", ids.empty() ? ";" : ids);
}

std::vector<std::string> ids() { return menu::tile_ids(load()); }

bool pinned(const std::string &id) {
  auto all = ids();
  return std::find(all.begin(), all.end(), id) != all.end();
}

void pin(const std::string &id) {
  auto groups = load();
  for (auto &g : groups)
    for (auto &t : g.tiles)
      if (t.id == id) return;
  if (groups.empty()) groups.push_back(TileGroup{_("Productivity"), {}});
  groups.front().tiles.push_back(Tile{id, TileSize::Medium});
  save(groups);
}

void unpin(const std::string &id) {
  auto groups = load();
  for (auto &g : groups)
    g.tiles.erase(std::remove_if(g.tiles.begin(), g.tiles.end(),
                                 [&](const Tile &t) { return t.id == id; }),
                  g.tiles.end());
  groups.erase(std::remove_if(groups.begin(), groups.end(),
                              [](const TileGroup &g) { return g.tiles.empty(); }),
               groups.end());
  save(groups);
}

}  // namespace start_tiles

// ================================================================= menu

AppMenu::AppMenu() : PanelPopup("SDE Menu") {
  rename_ = new RenameInput(this);
  rename_->box(FL_FLAT_BOX);
  rename_->hide();
  end();
  callback([](Fl_Widget *w, void *) { static_cast<AppMenu *>(w)->close(); });
}

AppMenu::~AppMenu() {
  Fl::remove_timeout(poll_applications, this);
  Fl::remove_timeout(animate, this);
  if (avatar_) cairo_surface_destroy(avatar_);
}

// ---------------------------------------------------------------------------
// Data

void AppMenu::reload() {
  apps_stamp_ = applications_stamp();
  apps_ = load_applications("SDE");
  sorted_ = menu::alphabetical(apps_);
  categories_.clear();
  for (auto &c : menu_categories())
    for (auto &a : apps_)
      if (a.menu_category() == c) {
        categories_.push_back(c);
        break;
      }
}

const DesktopEntry *AppMenu::app(const std::string &id) const {
  for (auto &a : apps_)
    if (a.id == id) return &a;
  return nullptr;
}

void AppMenu::load_tiles() {
  tiles_default_ = Panel::instance->config().get("menu", "tiles", "\x01") == "\x01" &&
                   Panel::instance->config().get("menu", "favorites", "\x01") == "\x01";
  groups_ = start_tiles::load();
  usage_ = panel_search::load_usage();
}

void AppMenu::save_tiles() {
  // Groups emptied by a drag or by unpinning disappear.
  groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                               [](const TileGroup &g) { return g.tiles.empty(); }),
                groups_.end());
  tiles_default_ = false;
  start_tiles::save(groups_);
}

// Refresh XDG application entries during package installs/removals without
// restarting the panel, keeping the scroll positions.
void AppMenu::poll_applications(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  Fl::repeat_timeout(2, poll_applications, data);
  if (self->menu_busy_ || self->launching_ || self->dragging_ || self->rename_group_ >= 0) return;
  // Parsing every .desktop file every 2 s while the menu is open showed up
  // as periodic hitches; only reload when an applications directory changed.
  std::string stamp = applications_stamp();
  if (stamp == self->apps_stamp_) return;
  std::string category = self->category_ >= 0 && self->category_ < int(self->categories_.size())
                             ? self->categories_[size_t(self->category_)]
                             : "";
  self->reload();
  if (self->tiles_default_) self->groups_ = start_tiles::load();
  auto cat = std::find(self->categories_.begin(), self->categories_.end(), category);
  self->category_ = cat != self->categories_.end() ? int(cat - self->categories_.begin()) : -1;
  self->filter();
  self->redraw();
}

void AppMenu::filter() {
  list_apps_.clear();
  for (auto *a : sorted_)
    if (category_ < 0 || (category_ < int(categories_.size()) &&
                          a->menu_category() == categories_[size_t(category_)]))
      list_apps_.push_back(a);
  most_used_.clear();
  if (show_most_used_)
    for (auto &id : menu::rank_frecent(usage_, epoch_now(), 32))
      if (const DesktopEntry *e = app(id))
        if (most_used_.size() < kMaxMostUsed) most_used_.push_back(e);
  added_.clear();
  if (show_added_) {
    std::vector<std::pair<long long, const DesktopEntry *>> fresh;
    long long limit = epoch_now() - kRecentlyAdded;
    for (auto &a : apps_) {
      struct stat st;
      if (!a.path.empty() && stat(a.path.c_str(), &st) == 0 && (long long)st.st_mtime > limit)
        fresh.push_back({(long long)st.st_mtime, &a});
    }
    std::stable_sort(fresh.begin(), fresh.end(),
                     [](auto &x, auto &y) { return x.first > y.first; });
    for (auto &f : fresh)
      if (added_.size() < kMaxAdded) added_.push_back(f.second);
  }
}

// ---------------------------------------------------------------------------
// Opening

void AppMenu::prepare() {
  theme_ = Panel::instance->theme();
  effects_ = effects_from_config(Panel::instance->config());
  const Config &cfg = Panel::instance->config();
  show_added_ = cfg.get_bool("menu", "show_recently_added", true);
  show_most_used_ = cfg.get_bool("menu", "show_most_used", cfg.get_bool("menu", "show_recent", true));
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  // [menu] width/height (dragged by the user, in unscaled pixels), else the
  // size preset.
  int lw = cfg.get_int("menu", "width", 0), lh = cfg.get_int("menu", "height", 0);
  if (lw <= 0 || lh <= 0) {
    std::string sz = cfg.get("menu", "size", "medium");
    lw = sz == "small" ? 600 : sz == "large" ? 940 : 680;
    lh = sz == "small" ? 560 : sz == "large" ? 700 : 620;
  }
  size(std::max(S(360), std::min(S(lw), mw - S(16))),
       std::max(S(300), std::min(S(lh), mh - Panel::instance->h() - S(24))));
  rename_->color(theme_.surface_alt.fl());
  rename_->textcolor(theme_.text.fl());
  rename_->cursor_color(theme_.accent.fl());
  rename_->selection_color(theme_.accent.fl());
  rename_->textsize(theme_.font_size + 1);
  rename_->hide();
  rename_group_ = -1;
  category_ = -1;
  list_scroll_ = tiles_scroll_ = 0;
  drag_area_ = K_NONE;
  letters_open_ = rail_open_ = false;
  hover_ = selected_ = anim_ = press_ = {};
  dragging_ = false;
  resize_edge_ = cursor_edge_ = 0;
  keyboard_ = false;
  launching_ = false;
  reload();
  load_tiles();
  filter();

  full_name_ = user_name();
  if (avatar_) cairo_surface_destroy(avatar_);
  avatar_ = nullptr;
  int ad = S(28);
  for (const std::string &f : {home_dir() + "/.face", home_dir() + "/.face.icon",
                               "/var/lib/AccountsService/icons/" + env("USER")}) {
    if (file_exists(f) && (avatar_ = load_image(f, ad * 2, ad * 2))) break;
  }
  Fl::remove_timeout(poll_applications, this);
  Fl::add_timeout(2, poll_applications, this);
}

void AppMenu::closed() {
  Fl::remove_timeout(animate, this);
  launching_ = false;
  letters_open_ = rail_open_ = false;
  dragging_ = false;
  press_ = {};
  rename_->hide();
  rename_group_ = -1;
  cursor(FL_CURSOR_DEFAULT);
}

// ---------------------------------------------------------------------------
// Layout

void AppMenu::metrics() {
  int W = w(), H = h();
  compact_ = H < S(470);
  pad_ = S(12);
  rail_w_ = S(48);
  header_h_ = S(48);
  bottom_ = H;
  int avail = W - rail_w_;
  list_x_ = rail_w_;
  list_w_ = std::max(S(190), std::min(S(270), int(avail * 0.4)));
  tiles_x_ = list_x_ + list_w_ + S(10);
  tiles_w_ = std::max(S(60), W - tiles_x_ - S(10));
  gap_ = S(4);
  int col_gap = S(24), nominal = S(46);
  int gw = 6 * nominal + 5 * gap_;
  group_cols_ = std::max(1, (tiles_w_ - S(10) + col_gap) / (gw + col_gap));
  if (group_cols_ == 1)
    cell_ = std::max(S(24), std::min(S(52), (tiles_w_ - S(10) - 5 * gap_) / 6));
  else
    cell_ = std::min(S(52), ((tiles_w_ - S(10) + col_gap) / group_cols_ - col_gap - 5 * gap_) / 6);
  group_w_ = 6 * cell_ + 5 * gap_;
}

void AppMenu::build_items() {
  items_.clear();
  areas_.clear();
  metrics();
  layout_list();
  layout_tiles();
  layout_rail();  // on top: the expanded rail covers the list
}

void AppMenu::layout_rail() {
  int H = h();
  int rw = rail_open_ ? std::min(S(240), w() - S(40)) : rail_w_, bh = S(44);
  auto add = [&](int index, int y, const std::string &label) {
    Item it;
    it.kind = K_RAIL;
    it.index = index;
    it.label = label;
    it.x = 0;
    it.y = y;
    it.w = rw;
    it.h = bh;
    items_.push_back(it);
  };
  add(0, S(4), _("Start"));
  int y = H - S(4) - bh;
  const char *names[] = {nullptr, nullptr, N_("Documents"), N_("Pictures"), N_("Settings"),
                         N_("Power")};
  for (int i = 5; i >= 1; --i) {
    add(i, y, i == 1 ? (full_name_.empty() ? std::string(_("Account")) : full_name_)
                     : std::string(_(names[i])));
    y -= bh;
  }
}

void AppMenu::layout_list() {
  int H = h();
  Item f;
  f.kind = K_FILTER;
  f.x = list_x_ + S(6);
  f.y = S(8);
  f.w = list_w_ - S(12);
  f.h = S(34);
  f.label = category_ < 0 ? std::string(_("All apps")) : category_label(categories_[size_t(category_)]);
  items_.push_back(f);
  list_top_ = header_h_;
  int area_h = H - S(6) - list_top_;
  if (letters_open_) {
    // Jump grid: every letter, the ones with applications enabled.
    std::vector<std::string> letters = {"#"};
    for (char c = 'A'; c <= 'Z'; ++c) letters.push_back(std::string(1, c));
    std::vector<std::string> present;
    for (auto *a : list_apps_) {
      std::string l = menu::index_letter(a->name);
      if (std::find(present.begin(), present.end(), l) == present.end()) present.push_back(l);
    }
    for (auto &l : present)
      if (std::find(letters.begin(), letters.end(), l) == letters.end()) letters.push_back(l);
    int gc = 4, cell = std::min((list_w_ - S(16)) / gc, S(50));
    int grows = (int(letters.size()) + gc - 1) / gc;
    cell = std::min(cell, std::max(S(20), area_h / std::max(1, grows)));
    int gx = list_x_ + (list_w_ - cell * gc) / 2;
    int gy = list_top_ + std::max(0, (area_h - cell * grows) / 2);
    for (size_t i = 0; i < letters.size(); ++i) {
      Item l;
      bool has = std::find(present.begin(), present.end(), letters[i]) != present.end();
      l.kind = has ? K_JUMP : K_HEADING;
      l.index = has ? int(i) : 4;  // 4: disabled letter
      l.label = letters[i];
      l.x = gx + int(i % size_t(gc)) * cell + S(2);
      l.y = gy + int(i / size_t(gc)) * cell + S(2);
      l.w = l.h = cell - S(4);
      items_.push_back(l);
    }
    return;
  }
  int row_h = compact_ ? S(32) : S(36), head_h = compact_ ? S(28) : S(32);
  int x = list_x_ + S(4), rw = list_w_ - S(14);
  std::vector<Item> rows;
  int y = 0;
  auto heading = [&](const std::string &label) {
    Item h;
    h.kind = K_HEADING;
    h.index = 0;
    h.label = label;
    h.x = x + S(8);
    h.y = y;
    h.w = rw - S(8);
    h.h = head_h;
    rows.push_back(h);
    y += head_h;
  };
  auto row = [&](const DesktopEntry *e, int group, int index) {
    Item r;
    r.kind = K_ROW;
    r.group = group;
    r.index = index;
    r.entry = e;
    r.x = x;
    r.y = y;
    r.w = rw;
    r.h = row_h;
    rows.push_back(r);
    y += row_h;
  };
  if (category_ < 0 && !added_.empty()) {
    heading(_("Recently added"));
    for (size_t i = 0; i < added_.size(); ++i) row(added_[i], 1, int(i));
    y += S(4);
  }
  if (category_ < 0 && !most_used_.empty()) {
    heading(_("Most used"));
    for (size_t i = 0; i < most_used_.size(); ++i) row(most_used_[i], 2, int(i));
    y += S(4);
  }
  std::string last;
  int idx = 0;
  for (auto *a : list_apps_) {
    std::string l = menu::index_letter(a->name);
    if (l != last) {
      Item h;
      h.kind = K_LETTER;
      h.index = int(rows.size());
      h.label = l;
      h.x = x;
      h.y = y;
      h.w = rw;
      h.h = head_h;
      rows.push_back(h);
      y += head_h;
      last = l;
    }
    row(a, 0, idx++);
  }
  if (list_apps_.empty()) heading(_("No applications in this category."));
  Area a;
  a.kind = K_ROW;
  a.x = list_x_;
  a.y = list_top_;
  a.w = list_w_;
  a.h = area_h;
  a.content = y + S(4);
  a.scroll = &list_scroll_;
  areas_.push_back(a);
  list_scroll_ = std::max(0, std::min(list_scroll_, a.content - a.h));
  for (auto &r : rows) {
    r.y += list_top_ - list_scroll_;
    r.cx = a.x;
    r.cy = a.y;
    r.cw = a.w;
    r.ch = a.h;
    r.visible = r.y + r.h > a.y && r.y < a.y + a.h;
    items_.push_back(r);
  }
}

void AppMenu::layout_tiles() {
  group_rects_.clear();
  int H = h();
  Area a;
  a.kind = K_TILE;
  a.x = tiles_x_;
  a.y = S(4);
  a.w = w() - tiles_x_;
  a.h = H - S(8);
  a.scroll = &tiles_scroll_;
  int head = S(36), col_gap = S(24);
  std::vector<int> heights(size_t(group_cols_), 0);
  std::vector<Item> tiles;
  for (size_t g = 0; g < groups_.size(); ++g) {
    size_t col = size_t(std::min_element(heights.begin(), heights.end()) - heights.begin());
    int gx = tiles_x_ + S(4) + int(col) * (group_w_ + col_gap), gy = heights[col] + S(4);
    Item hd;
    hd.kind = K_GROUP;
    hd.index = int(g);
    hd.label = groups_[g].name;
    hd.x = gx;
    hd.y = gy;
    hd.w = group_w_;
    hd.h = head;
    tiles.push_back(hd);
    int rows = 0;
    auto cells = menu::pack_tiles(groups_[g].tiles, menu::kGroupColumns, &rows);
    // Tiles of applications that are no longer installed are hidden.
    int shown = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
      const DesktopEntry *e = app(groups_[g].tiles[i].id);
      if (!e) continue;
      Item t;
      t.kind = K_TILE;
      t.group = int(g);
      t.index = int(i);
      t.entry = e;
      t.size = groups_[g].tiles[i].size;
      t.x = gx + cells[i].col * (cell_ + gap_);
      t.y = gy + head + cells[i].row * (cell_ + gap_);
      t.w = cells[i].cols * cell_ + (cells[i].cols - 1) * gap_;
      t.h = cells[i].rows * cell_ + (cells[i].rows - 1) * gap_;
      tiles.push_back(t);
      ++shown;
    }
    if (!shown) rows = 2;  // room to drop tiles into an empty group
    int gh = head + rows * (cell_ + gap_);
    group_rects_.push_back({gx, gy, group_w_, gh});
    heights[col] = gy + gh + S(14);
  }
  int content = *std::max_element(heights.begin(), heights.end());
  // While dragging: the place for a new group, below the shortest column.
  size_t col = size_t(std::min_element(heights.begin(), heights.end()) - heights.begin());
  new_group_ = {tiles_x_ + S(4) + int(col) * (group_w_ + col_gap), heights[col] + S(4), group_w_,
                2 * cell_ + gap_ + head};
  if (dragging_) content = std::max(content, new_group_.y + new_group_.h + S(8));
  if (groups_.empty()) {
    Item hint;
    hint.kind = K_HEADING;
    hint.index = 2;
    hint.label = _("Drag apps here, or right-click an app and choose \"Pin to Start\".");
    hint.x = tiles_x_ + S(8);
    hint.y = S(60);
    hint.w = tiles_w_ - S(16);
    hint.h = S(60);
    items_.push_back(hint);
  }
  a.content = content;
  tiles_scroll_ = std::max(0, std::min(tiles_scroll_, a.content - a.h));
  for (auto &r : group_rects_) r.y += a.y - tiles_scroll_;
  new_group_.y += a.y - tiles_scroll_;
  for (auto &t : tiles) {
    t.y += a.y - tiles_scroll_;
    t.cx = a.x;
    t.cy = a.y;
    t.cw = a.w;
    t.ch = a.h;
    t.visible = t.y + t.h > a.y && t.y < a.y + a.h;
    items_.push_back(t);
  }
  areas_.push_back(a);
}

const AppMenu::Item *AppMenu::find(const Ref &r) const {
  if (r.kind == K_NONE) return nullptr;
  for (auto &it : items_)
    if (it.kind == r.kind && it.index == r.index && it.group == r.group) return &it;
  return nullptr;
}

const AppMenu::Item *AppMenu::item_at(int x, int y) const {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it)
    if (it->kind != K_NONE && it->kind != K_HEADING && it->contains(x, y)) return &*it;
  return nullptr;
}

int AppMenu::edge_at(int x, int y) const {
  int e = S(5), edge = 0;
  if (x >= w() - e && x < w()) edge |= 1;
  if (theme_.panel_top ? (y >= h() - e && y < h()) : (y >= 0 && y < e)) edge |= 2;
  return edge;
}

void AppMenu::apply_size(int nw, int nh) {
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  nw = std::max(S(440), std::min(nw, mw - S(16)));
  nh = std::max(S(360), std::min(nh, mh - Panel::instance->h() - S(24)));
  if (nw == w() && nh == h()) return;
  size(nw, nh);
  reposition();
  redraw();
}

// ---------------------------------------------------------------------------
// Actions

void AppMenu::activate(const Item &it) {
  switch (it.kind) {
    case K_RAIL: rail_action(it.index); return;
    case K_FILTER: filter_menu(); return;
    case K_LETTER:
      letters_open_ = true;
      selected_ = {};
      break;
    case K_JUMP: {
      letters_open_ = false;
      build_items();
      for (auto &h : items_)
        if (h.kind == K_LETTER && h.label == it.label) {
          list_scroll_ += h.y - list_top_;
          if (keyboard_) selected_ = {K_LETTER, h.index, -1};
          break;
        }
      break;
    }
    case K_GROUP: start_rename(it.index); return;
    case K_ROW:
    case K_TILE: launch(it); return;
    default: return;
  }
  redraw();
}

void AppMenu::launch(const Item &it) {
  if (!it.entry) return;
  DesktopEntry e = *it.entry;
  panel_search::record_launch(e.id);
  // The program starts at once; the menu closes when the effect has played.
  launch_entry(e);
  effects_ = effects_from_config(Panel::instance->config());
  if (effects_.launch == "none" || !effects_.on) {
    close();
    return;
  }
  bool big = it.kind == K_TILE;
  launch_size_ = big ? std::min(it.w, it.h) * 0.45 : S(24);
  launch_cx_ = big ? it.x + it.w / 2.0 : it.x + S(12) + launch_size_ / 2;
  launch_cy_ = it.y + it.h / 2.0;
  launch_icon_ = e.icon;
  launching_ = true;
  launch_effect_ = effects_.launch;
  launch_start_ = now();
  Fl::remove_timeout(animate, this);
  Fl::add_timeout(1.0 / 60, animate, this);
}

void AppMenu::rail_action(int index) {
  switch (index) {
    case 0:
      rail_open_ = !rail_open_;
      redraw();
      return;
    case 1: {
      std::string name = full_name_.empty() ? std::string(_("Account")) : full_name_;
      Fl_Menu_Item items[] = {
          {name.c_str(), 0, nullptr, nullptr, FL_MENU_INACTIVE | FL_MENU_DIVIDER, 0, 0, 0, 0},
          {_("Account and system information"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
          {_("Lock"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
          {_("Log Out"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
          {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
      const Item *it = find({K_RAIL, 1, -1});
      menu_busy_ = true;
      const Fl_Menu_Item *m = items->popup(it ? it->x + it->w : 0, it ? it->y : 0);
      menu_busy_ = false;
      if (!m) return;
      int i = int(m - items);
      close();
      if (i == 1) spawn_detached({"sde-settings", "--page", "about"});
      else if (i == 2) spawn_detached({"sde-logout", "--action", "lock"});
      else if (i == 3) spawn_detached({"sde-logout", "--action", "logout"});
      return;
    }
    case 2:
      close();
      open_folder(user_folder("DOCUMENTS", "Documents"));
      return;
    case 3:
      close();
      open_folder(user_folder("PICTURES", "Pictures"));
      return;
    case 4:
      close();
      spawn_detached({"sde-settings"});
      return;
    case 5: {
      Fl_Menu_Item items[] = {{_("Sleep"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                              {_("Shut Down"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                              {_("Restart"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                              {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
      const Item *it = find({K_RAIL, 5, -1});
      menu_busy_ = true;
      const Fl_Menu_Item *m = items->popup(it ? it->x + it->w : 0, it ? it->y - S(60) : 0);
      menu_busy_ = false;
      if (!m) return;
      static const char *const actions[] = {"suspend", "poweroff", "reboot"};
      close();
      spawn_detached({"sde-logout", "--action", actions[m - items]});
      return;
    }
  }
}

// The category filter: a short menu instead of a row of chips.
void AppMenu::filter_menu() {
  std::vector<std::string> labels;
  labels.push_back(format("%s (%d)", _("All apps"), int(sorted_.size())));
  for (auto &c : categories_) {
    int n = 0;
    for (auto *a : sorted_) n += a->menu_category() == c;
    labels.push_back(format("%s (%d)", category_label(c).c_str(), n));
  }
  std::vector<Fl_Menu_Item> items;
  for (size_t i = 0; i < labels.size(); ++i)
    items.push_back({labels[i].c_str(), 0, nullptr, nullptr,
                     FL_MENU_RADIO | (int(i) - 1 == category_ ? FL_MENU_VALUE : 0) |
                         (i == 0 ? FL_MENU_DIVIDER : 0),
                     0, 0, 0, 0});
  items.push_back({nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0});
  const Item *f = find({K_FILTER, -1, -1});
  menu_busy_ = true;
  const Fl_Menu_Item *m = items.data()->popup(f ? f->x : 0, f ? f->y + f->h : 0);
  menu_busy_ = false;
  if (!m) return;
  category_ = int(m - items.data()) - 1;
  list_scroll_ = 0;
  letters_open_ = false;
  selected_ = {};
  filter();
  redraw();
}

void AppMenu::tile_menu(const Item &it, int px, int py) {
  if (it.group < 0 || it.group >= int(groups_.size())) return;
  TileGroup &g = groups_[size_t(it.group)];
  if (it.index < 0 || it.index >= int(g.tiles.size())) return;
  Tile tile = g.tiles[size_t(it.index)];
  bool pinned = Panel::instance->is_pinned(tile.id);
  std::vector<std::string> texts;
  texts.reserve(groups_.size() + 2);
  enum { OPEN = 1, UNPIN, SMALL, MEDIUM, WIDE, LARGE, PANEL, NEW_GROUP, GROUP0 = 100 };
  std::vector<Fl_Menu_Item> items;
  auto add = [&](const char *text, long code, int flags = 0) {
    items.push_back({text, 0, nullptr, reinterpret_cast<void *>(code), flags, 0, 0, 0, 0});
  };
  add(_("Open"), OPEN, FL_MENU_DIVIDER);
  add(_("Unpin from Start"), UNPIN);
  add(_("Resize"), 0, FL_SUBMENU);
  for (TileSize s : {TileSize::Small, TileSize::Medium, TileSize::Wide, TileSize::Large}) {
    texts.push_back(size_label(s));
    add(texts.back().c_str(), SMALL + int(s), FL_MENU_RADIO | (tile.size == s ? FL_MENU_VALUE : 0));
  }
  add(nullptr, 0);
  add(_("Move to Group"), 0, FL_SUBMENU);
  for (size_t i = 0; i < groups_.size(); ++i) {
    if (int(i) == it.group) continue;
    texts.push_back(groups_[i].name.empty() ? std::string(_("Unnamed group")) : groups_[i].name);
    add(texts.back().c_str(), GROUP0 + long(i));
  }
  add(_("New Group"), NEW_GROUP);
  add(nullptr, 0);
  add(pinned ? _("Unpin from Panel") : _("Pin to Panel"), PANEL);
  add(nullptr, 0);
  menu_busy_ = true;
  const Fl_Menu_Item *m = items.data()->popup(px, py);
  menu_busy_ = false;
  if (!m) return;
  long code = reinterpret_cast<long>(m->user_data());
  auto &tiles = g.tiles;
  switch (code) {
    case OPEN: launch(it); return;
    case UNPIN: tiles.erase(tiles.begin() + it.index); break;
    case SMALL:
    case MEDIUM:
    case WIDE:
    case LARGE: tiles[size_t(it.index)].size = TileSize(code - SMALL); break;
    case PANEL:
      if (pinned) Panel::instance->unpin(tile.id);
      else Panel::instance->pin(tile.id);
      return;
    case NEW_GROUP:
      tiles.erase(tiles.begin() + it.index);
      groups_.push_back(TileGroup{"", {tile}});
      break;
    default:
      if (code >= GROUP0 && code - GROUP0 < long(groups_.size())) {
        tiles.erase(tiles.begin() + it.index);
        groups_[size_t(code - GROUP0)].tiles.push_back(tile);
      } else {
        return;
      }
  }
  save_tiles();
  redraw();
}

void AppMenu::group_menu(int group, int px, int py) {
  if (group < 0 || group >= int(groups_.size())) return;
  Fl_Menu_Item items[] = {
      {_("Rename Group"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Move Group Up"), 0, nullptr, nullptr, group == 0 ? FL_MENU_INACTIVE : 0, 0, 0, 0, 0},
      {_("Move Group Down"), 0, nullptr, nullptr,
       (group + 1 >= int(groups_.size()) ? FL_MENU_INACTIVE : 0) | FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Unpin Group from Start"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  menu_busy_ = true;
  const Fl_Menu_Item *m = items->popup(px, py);
  menu_busy_ = false;
  if (!m) return;
  switch (int(m - items)) {
    case 0: start_rename(group); return;
    case 1: std::swap(groups_[size_t(group)], groups_[size_t(group - 1)]); break;
    case 2: std::swap(groups_[size_t(group)], groups_[size_t(group + 1)]); break;
    case 3: groups_.erase(groups_.begin() + group); break;
    default: return;
  }
  save_tiles();
  redraw();
}

void AppMenu::row_menu(const Item &it, int px, int py) {
  if (!it.entry) return;
  DesktopEntry e = *it.entry;
  bool tile = start_tiles::pinned(e.id);
  bool used = std::find_if(usage_.begin(), usage_.end(),
                           [&](const menu::Usage &u) { return u.id == e.id; }) != usage_.end();
  bool pinned = Panel::instance->is_pinned(e.id);
  enum { OPEN = 1, START, PANEL, DESKTOP, FORGET };
  std::vector<Fl_Menu_Item> items;
  auto add = [&](const char *text, long code, int flags = 0) {
    items.push_back({text, 0, nullptr, reinterpret_cast<void *>(code), flags, 0, 0, 0, 0});
  };
  add(_("Open"), OPEN, FL_MENU_DIVIDER);
  add(tile ? _("Unpin from Start") : _("Pin to Start"), START);
  add(pinned ? _("Unpin from Panel") : _("Pin to Panel"), PANEL);
  add(_("Add to Desktop"), DESKTOP, used ? 0 : FL_MENU_DIVIDER);
  if (used) add(_("Remove from Most Used"), FORGET);
  add(nullptr, 0);
  menu_busy_ = true;
  const Fl_Menu_Item *m = items.data()->popup(px, py);
  menu_busy_ = false;
  if (!m) return;
  switch (reinterpret_cast<long>(m->user_data())) {
    case OPEN: launch(it); return;
    case START:
      if (tile) {
        for (auto &g : groups_)
          g.tiles.erase(std::remove_if(g.tiles.begin(), g.tiles.end(),
                                       [&](const Tile &t) { return t.id == e.id; }),
                        g.tiles.end());
      } else {
        if (groups_.empty()) groups_.push_back(TileGroup{_("Productivity"), {}});
        groups_.front().tiles.push_back(Tile{e.id, TileSize::Medium});
      }
      save_tiles();
      break;
    case PANEL:
      if (pinned) Panel::instance->unpin(e.id);
      else Panel::instance->pin(e.id);
      break;
    case DESKTOP: {
      std::string desk = user_dir("DESKTOP");
      make_dirs(desk);
      std::string text;
      if (read_file(e.path, text)) {
        std::string target = desk + "/" + e.id;
        write_file_atomic(target, text);
        // Launchers created by the user are trusted (executable).
        chmod(target.c_str(), 0755);
      }
      break;
    }
    case FORGET:
      menu::forget(usage_, e.id);
      panel_search::save_usage(usage_);
      break;
  }
  filter();
  redraw();
}

// Right-click on the menu background: arrange it ([menu] in sde.conf).
void AppMenu::background_menu(int px, int py) {
  auto toggle = [](bool on) { return FL_MENU_TOGGLE | (on ? FL_MENU_VALUE : 0); };
  Fl_Menu_Item items[] = {
      {_("Show Recently Added Apps"), 0, nullptr, nullptr, toggle(show_added_), 0, 0, 0, 0},
      {_("Show Most Used Apps"), 0, nullptr, nullptr, toggle(show_most_used_) | FL_MENU_DIVIDER,
       0, 0, 0, 0},
      {_("New Group"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Restore Default Tiles"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Restore Default Menu Size"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Effects and Animations..."), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  menu_busy_ = true;
  const Fl_Menu_Item *m = items->popup(px, py);
  menu_busy_ = false;
  if (!m) return;
  Panel *p = Panel::instance;
  switch (int(m - items)) {
    case 0: p->set_setting("menu", "show_recently_added", show_added_ ? "false" : "true"); break;
    case 1: p->set_setting("menu", "show_most_used", show_most_used_ ? "false" : "true"); break;
    case 2:
      groups_.push_back(TileGroup{"", {}});
      // An empty group is kept until it is named or gets tiles.
      start_tiles::save(groups_);
      start_rename(int(groups_.size()) - 1);
      return;
    case 3:
      p->set_setting("menu", "tiles", "");
      p->set_setting("menu", "favorites", "");
      break;
    case 4:
      p->set_setting("menu", "width", "");
      p->set_setting("menu", "height", "");
      p->set_setting("menu", "size", "");
      break;
    case 5:
      close();
      spawn_detached({"sde-settings", "--page", "effects"});
      return;
    default: return;
  }
  // Apply at once: the size may change, so place the menu again.
  prepare();
  reposition();
  redraw();
}

void AppMenu::start_rename(int group) {
  if (group < 0 || group >= int(groups_.size())) return;
  build_items();
  const Item *hd = find({K_GROUP, group, -1});
  if (!hd || !hd->visible) return;
  rename_group_ = group;
  rename_->resize(hd->x, hd->y + S(4), hd->w, hd->h - S(8));
  rename_->value(groups_[size_t(group)].name.c_str());
  rename_->show();
  // FLTK 1.3 ignores Fl::focus() while a window holds the grab, which the
  // popup always does: lift the grab for the moment the focus moves.
  Fl_Window *grab = Fl::grab_;
  Fl::grab_ = nullptr;
  rename_->take_focus();
  Fl::grab_ = grab;
  rename_->position(rename_->size(), 0);
  redraw();
}

void AppMenu::finish_rename(bool commit) {
  int g = rename_group_;
  rename_group_ = -1;
  rename_->hide();
  take_focus();
  if (commit && g >= 0 && g < int(groups_.size())) {
    groups_[size_t(g)].name = trim(rename_->value());
    start_tiles::save(groups_);
    tiles_default_ = false;
  }
  redraw();
}

void AppMenu::redirect_to_search(const std::string &text) {
  close();
  Panel::instance->open_search(text);
}

// ---------------------------------------------------------------------------
// Drag and drop of tiles

// Where a drop at (x, y) goes: |group| (groups_.size() for a new group, -1
// for nowhere) and the tile index in it.
void AppMenu::drop_target(int x, int y, int &group, int &index) const {
  group = -1;
  index = 0;
  const Area *a = area(K_TILE);
  if (!a || x < a->x || x >= a->x + a->w || y < a->y || y >= a->y + a->h) return;
  for (auto &it : items_)
    if (it.kind == K_TILE && it.contains(x, y)) {
      group = it.group;
      index = it.index + (x > it.x + it.w / 2 ? 1 : 0);
      return;
    }
  for (size_t g = 0; g < group_rects_.size(); ++g)
    if (group_rects_[g].contains(x, y)) {
      group = int(g);
      index = int(groups_[g].tiles.size());
      return;
    }
  group = int(groups_.size());
}

void AppMenu::drop(int x, int y) {
  int group, index;
  build_items();
  drop_target(x, y, group, index);
  if (group < 0) return;
  Tile tile{drag_id_, drag_size_};
  // Take the tile out of its place first (a tile moved, or an application
  // from the list that was already pinned).
  for (size_t g = 0; g < groups_.size(); ++g) {
    auto &tiles = groups_[g].tiles;
    for (size_t i = 0; i < tiles.size(); ++i)
      if (tiles[i].id == drag_id_) {
        tile.size = tiles[i].size;
        tiles.erase(tiles.begin() + long(i));
        if (int(g) == group && int(i) < index) --index;
        break;
      }
  }
  if (group >= int(groups_.size())) {
    groups_.push_back(TileGroup{"", {tile}});
  } else {
    auto &tiles = groups_[size_t(group)].tiles;
    index = std::max(0, std::min(index, int(tiles.size())));
    tiles.insert(tiles.begin() + index, tile);
  }
  save_tiles();
}

// ---------------------------------------------------------------------------
// Keyboard and scrolling

void AppMenu::ensure_visible(const Item &it) {
  auto fit = [&](int &scroll) {
    if (!it.clipped()) return;
    if (it.y < it.cy) scroll -= it.cy - it.y;
    else if (it.y + it.h > it.cy + it.ch) scroll += it.y + it.h - (it.cy + it.ch);
    scroll = std::max(0, scroll);
  };
  if (it.kind == K_ROW || it.kind == K_LETTER) fit(list_scroll_);
  else if (it.kind == K_TILE || it.kind == K_GROUP) fit(tiles_scroll_);
}

const AppMenu::Area *AppMenu::area(Kind kind) const {
  for (auto &a : areas_)
    if (a.kind == kind) return &a;
  return nullptr;
}

const AppMenu::Area *AppMenu::area_at(int x, int y) const {
  for (auto &a : areas_)
    if (x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h) return &a;
  return nullptr;
}

void AppMenu::scroll_by(Kind kind, int dy) {
  const Area *a = area(kind);
  if (!a || !a->scrolls()) return;
  *a->scroll = std::max(0, std::min(*a->scroll + dy, a->content - a->h));
  hover_ = {};
  redraw();
}

// Scrollbar thumb dragged to window y |y| (see FL_DRAG).
void AppMenu::drag_scrollbar(int y) {
  const Area *a = area(drag_area_);
  if (!a || !a->scrolls()) return;
  double track = a->h - S(8);
  double len = std::max(Sd(24), track * a->h / a->content);
  double per_px = (a->content - a->h) / std::max(1.0, track - len);
  *a->scroll = std::max(0, std::min(a->content - a->h,
                                    drag_scroll_ + int(std::lround((y - drag_y_) * per_px))));
  redraw();
}

// Moves the selection to the nearest item in the direction of |key|, across
// the rail, the list and the tiles (arrow keys) or by one screen (Page keys).
void AppMenu::move_selection(int key) {
  auto selectable = [](Kind k) {
    return k == K_RAIL || k == K_FILTER || k == K_ROW || k == K_LETTER || k == K_JUMP ||
           k == K_GROUP || k == K_TILE;
  };
  build_items();
  const Item *cur = find(selected_);
  keyboard_ = true;
  if (key == FL_Page_Down || key == FL_Page_Up) {
    int dir = key == FL_Page_Down ? 1 : -1;
    Kind list = cur && (cur->kind == K_TILE || cur->kind == K_GROUP) ? K_TILE : K_ROW;
    const Area *a = area(list);
    if (!a) return;
    bool keep = cur && (cur->kind == K_ROW || cur->kind == K_TILE) && cur->visible;
    int ref_x = keep ? cur->x : a->x, ref_y = keep ? cur->y : a->y;
    int before = *a->scroll;
    if (a->scrolls())
      *a->scroll = std::max(0, std::min(a->content - a->h, *a->scroll + dir * (a->h - S(36))));
    bool moved = *a->scroll != before;
    build_items();
    Kind kind = list == K_TILE ? K_TILE : K_ROW;
    const Item *best = nullptr;
    double best_d = 1e18;
    for (auto &it : items_) {
      if (it.kind != kind || !it.visible) continue;
      double d = std::fabs(double(it.y - ref_y)) + std::fabs(double(it.x - ref_x));
      if (d < best_d) best_d = d, best = &it;
    }
    if (!moved && keep)
      for (auto &it : items_)
        if (it.kind == kind) {
          best = &it;
          if (dir < 0) break;
        }
    if (best) {
      selected_ = {best->kind, best->index, best->group};
      ensure_visible(*best);
    }
    redraw();
    return;
  }
  if (!cur) {
    // The first key selects the first tile, else the first application.
    const Item *first = nullptr;
    for (auto &it : items_)
      if (it.kind == K_TILE && it.visible) {
        first = &it;
        break;
      }
    if (!first)
      for (auto &it : items_)
        if ((it.kind == K_ROW || it.kind == K_JUMP) && it.visible) {
          first = &it;
          break;
        }
    if (first) selected_ = {first->kind, first->index, first->group};
    redraw();
    return;
  }
  double cx = cur->x + cur->w / 2.0, cy = cur->y + cur->h / 2.0;
  const Item *best = nullptr;
  double best_score = 1e18;
  for (auto &it : items_) {
    if (&it == cur || !selectable(it.kind) || !it.visible) continue;
    // The expanded rail covers the list.
    double ix = it.x + it.w / 2.0, iy = it.y + it.h / 2.0;
    double along, across;
    if (key == FL_Down) along = iy - cy, across = ix - cx;
    else if (key == FL_Up) along = cy - iy, across = ix - cx;
    else if (key == FL_Right) along = ix - cx, across = iy - cy;
    else along = cx - ix, across = iy - cy;
    if (along < 2) continue;
    bool overlaps = key == FL_Down || key == FL_Up
                        ? it.x < cur->x + cur->w && it.x + it.w > cur->x
                        : it.y < cur->y + cur->h && it.y + it.h > cur->y;
    double score = along + std::fabs(across) * (overlaps ? 0.5 : 3.0);
    if (score < best_score) best_score = score, best = &it;
  }
  if (best) {
    selected_ = {best->kind, best->index, best->group};
    ensure_visible(*best);
  }
  redraw();
}

// ---------------------------------------------------------------------------
// Animations

void AppMenu::start_hover(const Ref &r) {
  anim_ = r;
  anim_hover_ = 0;
  anim_start_ = now();
  if (!effects_.on || effects_.hover == "none") return;
  Fl::remove_timeout(animate, this);
  Fl::add_timeout(1.0 / 60, animate, this);
}

void AppMenu::animate(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  double t = now();
  bool more = false;
  if (self->launching_) {
    if (t - self->launch_start_ >= self->effects_.duration) {
      self->launching_ = false;
      self->close();
      return;
    }
    more = true;
  }
  if (self->anim_.kind != K_NONE && self->hover_ == self->anim_) {
    double hover_time = 0.18 * (self->effects_.duration / 0.32);
    self->anim_hover_ = std::min(1.0, (t - self->anim_start_) / hover_time);
    const std::string &h = self->effects_.hover;
    more = more || self->anim_hover_ < 1.0 || h == "glow" ||
           (h == "bounce" && t - self->anim_start_ < 1.0);
  }
  self->redraw();
  if (more) Fl::repeat_timeout(1.0 / 60, animate, data);
}

// ---------------------------------------------------------------------------
// Events

int AppMenu::handle(int event) {
  if (launching_ && (event == FL_PUSH || event == FL_KEYBOARD || event == FL_SHORTCUT)) return 1;
  switch (event) {
    case FL_PUSH: {
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (ex < 0 || ey < 0 || ex >= w() || ey >= h()) {
        if (rename_group_ >= 0) finish_rename(true);
        return PanelPopup::handle(event);
      }
      if (rename_group_ >= 0) {
        if (Fl::event_inside(rename_)) return PanelPopup::handle(event);
        finish_rename(true);
      }
      build_items();
      bool left = Fl::event_button() == FL_LEFT_MOUSE;
      // Resizing by the outer edges.
      if (left && (resize_edge_ = edge_at(ex, ey))) {
        resize_rx_ = Fl::event_x_root();
        resize_ry_ = Fl::event_y_root();
        resize_w_ = w();
        resize_h_ = h();
        return 1;
      }
      const Item *hit = item_at(ex, ey);
      Item it = hit ? *hit : Item();
      // The expanded rail closes on a click next to it.
      if (rail_open_ && it.kind != K_RAIL) {
        rail_open_ = false;
        redraw();
        return 1;
      }
      if (left)
        for (auto &a : areas_) {
          // Scrollbars: drag the thumb, or click the track to jump there.
          if (!a.scrolls() || ex < a.bar_x() - S(5) || ex >= a.x + a.w || ey < a.y ||
              ey >= a.y + a.h)
            continue;
          double track = a.h - S(8);
          double len = std::max(Sd(24), track * a.h / a.content);
          double pos = a.y + S(4) + (track - len) * *a.scroll / std::max(1, a.content - a.h);
          drag_area_ = a.kind;
          drag_y_ = ey;
          drag_scroll_ = *a.scroll;
          if (ey < pos || ey > pos + len) {
            drag_y_ = int(pos + len / 2);
            drag_scrollbar(ey);
            drag_y_ = ey;
            drag_scroll_ = *a.scroll;
          }
          redraw();
          return 1;
        }
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (it.kind == K_TILE || it.kind == K_ROW) {
          selected_ = {it.kind, it.index, it.group};
          redraw();
        }
        if (it.kind == K_TILE) tile_menu(it, ex, ey);
        else if (it.kind == K_ROW) row_menu(it, ex, ey);
        else if (it.kind == K_GROUP) group_menu(it.index, ex, ey);
        else background_menu(ex, ey);
        return 1;
      }
      if (letters_open_ && it.kind != K_JUMP && ex >= list_x_ && ex < list_x_ + list_w_) {
        letters_open_ = false;
        redraw();
        return 1;
      }
      if (left && (it.kind == K_TILE || it.kind == K_ROW)) {
        // Opens on release, unless the press becomes a drag.
        press_ = {it.kind, it.index, it.group};
        press_x_ = drag_x_ = ex;
        press_y_ = drag_y2_ = ey;
        dragging_ = false;
        drag_id_ = it.entry ? it.entry->id : "";
        drag_size_ = it.kind == K_TILE ? it.size : TileSize::Medium;
        return 1;
      }
      if (left && it.kind != K_NONE) activate(it);
      return 1;
    }
    case FL_DRAG: {
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (resize_edge_) {
        int dx = Fl::event_x_root() - resize_rx_, dy = Fl::event_y_root() - resize_ry_;
        int nw = resize_edge_ & 1 ? resize_w_ + dx : w();
        int nh = resize_edge_ & 2 ? resize_h_ + (theme_.panel_top ? dy : -dy) : h();
        apply_size(nw, nh);
        return 1;
      }
      if (drag_area_ != K_NONE) {
        drag_scrollbar(ey);
        return 1;
      }
      if (press_.kind != K_NONE) {
        if (!dragging_ && std::abs(ex - press_x_) + std::abs(ey - press_y_) > S(8)) {
          dragging_ = true;
          rail_open_ = letters_open_ = false;
        }
        drag_x_ = ex;
        drag_y2_ = ey;
        if (dragging_) {
          // Near the top or bottom of the tiles: scroll them.
          const Area *a = area(K_TILE);
          if (a && ex >= a->x) {
            if (ey < a->y + S(24)) scroll_by(K_TILE, -S(12));
            else if (ey > a->y + a->h - S(24)) scroll_by(K_TILE, S(12));
          }
          redraw();
        }
        return 1;
      }
      return PanelPopup::handle(event);
    }
    case FL_RELEASE: {
      if (resize_edge_) {
        resize_edge_ = 0;
        double sc = ui_scale();
        Panel::instance->set_setting("menu", "width", std::to_string(int(std::lround(w() / sc))));
        Panel::instance->set_setting("menu", "height", std::to_string(int(std::lround(h() / sc))));
        return 1;
      }
      if (drag_area_ != K_NONE) {
        drag_area_ = K_NONE;
        redraw();
        return 1;
      }
      if (press_.kind != K_NONE) {
        Ref pressed = press_;
        press_ = {};
        if (dragging_) {
          dragging_ = false;
          drop(Fl::event_x(), Fl::event_y());
          redraw();
          return 1;
        }
        build_items();
        const Item *it = find(pressed);
        if (it && it->contains(Fl::event_x(), Fl::event_y())) activate(*it);
        return 1;
      }
      return PanelPopup::handle(event);
    }
    case FL_MOVE: {
      int ex = Fl::event_x(), ey = Fl::event_y();
      int edge = rename_group_ < 0 ? edge_at(ex, ey) : 0;
      if (edge != cursor_edge_) {
        cursor_edge_ = edge;
        cursor(edge == 3 ? (theme_.panel_top ? FL_CURSOR_SE : FL_CURSOR_NE)
               : edge == 1 ? FL_CURSOR_WE
               : edge == 2 ? FL_CURSOR_NS
                           : FL_CURSOR_DEFAULT);
      }
      build_items();
      const Item *hit = item_at(ex, ey);
      Ref r = hit ? Ref{hit->kind, hit->index, hit->group} : Ref{};
      if (!(r == hover_)) {
        hover_ = r;
        if (hit && hit->entry) {
          selected_ = r;
          keyboard_ = false;
          start_hover(r);
        }
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      if (cursor_edge_) {
        cursor_edge_ = 0;
        cursor(FL_CURSOR_DEFAULT);
      }
      if (hover_.kind != K_NONE) {
        hover_ = {};
        redraw();
      }
      return PanelPopup::handle(event);
    case FL_MOUSEWHEEL: {
      // The wheel scrolls the area under the pointer. The menu holds the
      // pointer grab, so take the position from root coordinates.
      int ex = Fl::event_x_root() - x_root(), ey = Fl::event_y_root() - y_root();
      int dy = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
      if (!dy || menu_busy_) return 1;
      build_items();
      const Area *a = area_at(ex, ey);
      if (!a) a = area(ex >= tiles_x_ ? K_TILE : K_ROW);
      if (a) scroll_by(a->kind, dy * (compact_ ? S(64) : S(72)));
      return 1;
    }
    case FL_SHORTCUT:
    case FL_KEYBOARD: {
      if (rename_group_ >= 0) return rename_->handle(FL_KEYBOARD);
      int k = Fl::event_key();
      if (k == FL_Escape) {
        if (dragging_) {
          dragging_ = false;
          press_ = {};
        } else if (letters_open_) {
          letters_open_ = false;
        } else if (rail_open_) {
          rail_open_ = false;
        } else if (category_ >= 0) {
          category_ = -1;
          list_scroll_ = 0;
          filter();
        } else {
          close();
          return 1;
        }
        redraw();
        return 1;
      }
      build_items();
      const Item *sel = find(selected_);
      if (k == FL_Menu) {
        if (sel) {
          int px = sel->x + sel->w / 2, py = sel->y + sel->h / 2;
          if (sel->kind == K_TILE) tile_menu(*sel, px, py);
          else if (sel->kind == K_ROW) row_menu(*sel, px, py);
          else if (sel->kind == K_GROUP) group_menu(sel->index, px, py);
        }
        return 1;
      }
      if (k == FL_Enter || k == FL_KP_Enter || (k == ' ' && keyboard_)) {
        if (sel) activate(*sel);
        return 1;
      }
      if (k == FL_Down || k == FL_Up || k == FL_Left || k == FL_Right || k == FL_Page_Down ||
          k == FL_Page_Up) {
        move_selection(k);
        return 1;
      }
      // Typing goes to the search box of the panel.
      const char *text = Fl::event_text();
      if (!(Fl::event_state() & (FL_CTRL | FL_ALT | FL_META)) && text && *text &&
          (unsigned char)text[0] >= 0x21 && text[0] != 0x7F) {
        redirect_to_search(text);
        return 1;
      }
      return 1;
    }
    default:
      return PanelPopup::handle(event);
  }
}

// ---------------------------------------------------------------------------
// Drawing

void AppMenu::draw() {
  const Theme &t = theme_;
  int W = w(), H = h();
  double tnow = now();
  build_items();
  const Item *hov = find(hover_);
  const Item *sel = find(selected_);
  auto is_sel = [&](const Item &it) { return sel == &it && (keyboard_ || it.entry); };
  auto is_hov = [&](const Item &it) { return hov == &it; };
  int drop_group = -1, drop_index = 0;
  if (dragging_) drop_target(drag_x_, drag_y2_, drop_group, drop_index);

  auto hover_of = [&](const Item &it, double &lift, double &scale) {
    lift = 0;
    scale = 1;
    if (!effects_.on || !(Ref{it.kind, it.index, it.group} == anim_) || !(anim_ == hover_)) return;
    double p = ease_out(anim_hover_), k = effects_.size;
    const std::string &h = effects_.hover;
    if (h == "wave") lift = Sd(4) * p * k;
    else if (h == "pop") scale = 1 + 0.12 * p * k;
    else if (h == "bounce") {
      double e = tnow - anim_start_;
      lift = e < 1.0 ? Sd(7) * k * std::fabs(std::sin(e * M_PI * 3)) * (1 - e) : 0;
    }
  };
  // Tiles: Fluent-like surfaces tinted with the accent.
  Color tile_bg = t.surface_alt.mix(t.accent, t.dark ? 0.16 : 0.10);
  Color tile_hover = tile_bg.mix(t.dark ? Color::hex(0xFFFFFF) : Color::hex(0x000000), 0.07);
  auto icon_size = [&](const Item &it) {
    switch (it.size) {
      case TileSize::Small: return std::min(it.w, it.h) * 0.62;
      case TileSize::Large: return std::min(it.w, it.h) * 0.38;
      default: return std::min(it.w, it.h) * 0.42;
    }
  };

  {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface);
    // The rail is a shade apart from the list; the tiles sit on the surface.
    p.fill_rect(0, 0, rail_w_, H, t.surface_alt.alpha(t.dark ? 0.35 : 0.55));
    p.stroke_rounded(0.5, 0.5, W - 1, H - 1, 0, t.border);

    for (auto &it : items_) {
      if (!it.visible || it.kind == K_RAIL) continue;
      cairo_save(p.cr());
      if (it.clipped()) {
        cairo_rectangle(p.cr(), it.cx, it.cy, it.cw, it.ch);
        cairo_clip(p.cr());
      }
      bool s = is_sel(it), hv = is_hov(it);
      switch (it.kind) {
        case K_FILTER: {
          p.fill_rounded(it.x, it.y, it.w, it.h, S(6), hv ? t.hover : t.surface_alt.alpha(0.7));
          if (s && keyboard_) p.stroke_rounded(it.x + 1, it.y + 1, it.w - 2, it.h - 2, S(6), t.accent, Sd(2));
          int g = S(18);
          if (category_ >= 0)
            p.image(IconLoader::get({category_icon(categories_[size_t(category_)]),
                                     "applications-other"}, g),
                    it.x + S(10), it.y + (it.h - g) / 2.0, g);
          chevron_down(p, it.x + it.w - S(28), it.y + (it.h - S(18)) / 2.0, S(18), t.text_dim);
          break;
        }
        case K_LETTER:
          if (hv) p.fill_rounded(it.x, it.y + S(2), it.w, it.h - S(4), S(6), t.hover);
          if (s) p.stroke_rounded(it.x + 1, it.y + S(2), it.w - 2, it.h - S(4), S(6), t.accent, Sd(2));
          break;
        case K_JUMP:
          p.fill_rounded(it.x, it.y, it.w, it.h, S(6), hv || s ? t.accent : t.surface_alt);
          break;
        case K_GROUP:
          if (hv && rename_group_ != it.index) {
            p.fill_rounded(it.x, it.y + S(4), it.w, it.h - S(8), S(4), t.hover);
            // Rename handle (two short lines) at the right, as on hover.
            double hx = it.x + it.w - S(22), hy = it.y + it.h / 2.0;
            p.line(hx, hy - S(3), hx + S(12), hy - S(3), t.text_dim, Sd(1.5));
            p.line(hx, hy + S(3), hx + S(12), hy + S(3), t.text_dim, Sd(1.5));
          }
          if (s && keyboard_)
            p.stroke_rounded(it.x + 1, it.y + S(4), it.w - 2, it.h - S(8), S(4), t.accent, Sd(2));
          if (drop_group == it.index && dragging_)
            p.stroke_rounded(group_rects_[size_t(it.index)].x - S(3),
                             group_rects_[size_t(it.index)].y - S(1),
                             group_rects_[size_t(it.index)].w + S(6),
                             group_rects_[size_t(it.index)].h + S(2), S(6), t.accent.alpha(0.5),
                             Sd(1.5));
          break;
        case K_ROW: {
          double lift, scale;
          hover_of(it, lift, scale);
          bool pressed = dragging_ && press_ == Ref{it.kind, it.index, it.group};
          if (s) p.fill_rounded(it.x, it.y, it.w, it.h, S(6), t.accent.alpha(t.dark ? 0.28 : 0.16));
          else if (hv) p.fill_rounded(it.x, it.y, it.w, it.h, S(6), t.hover);
          if (s && keyboard_)
            p.stroke_rounded(it.x + 1, it.y + 1, it.w - 2, it.h - 2, S(6), t.accent, Sd(1.5));
          double is = (compact_ ? S(22) : S(26)) * scale;
          p.image(IconLoader::get({it.entry->icon, "application-x-executable"}, int(std::lround(is))),
                  it.x + S(10) + (S(26) - is) / 2, it.y + (it.h - is) / 2.0 - lift, is,
                  pressed ? 0.4 : 1.0);
          break;
        }
        case K_TILE: {
          bool pressed = dragging_ && press_ == Ref{it.kind, it.index, it.group};
          double lift, scale;
          hover_of(it, lift, scale);
          double r = S(4);
          Color bg = hv ? tile_hover : tile_bg;
          if (pressed) bg = bg.alpha(0.35);
          p.fill_rounded(it.x, it.y, it.w, it.h, r, bg);
          // Reveal border on hover; accent ring for the keyboard selection.
          if (hv && !pressed) p.stroke_rounded(it.x + 0.5, it.y + 0.5, it.w - 1, it.h - 1, r, t.text.alpha(0.28));
          if (s && keyboard_)
            p.stroke_rounded(it.x + 1, it.y + 1, it.w - 2, it.h - 2, r, t.accent, Sd(2));
          double is = icon_size(it) * scale;
          double ix = it.x + (it.w - is) / 2.0;
          double iy = it.size == TileSize::Small ? it.y + (it.h - is) / 2.0
                                                 : it.y + (it.h - is) / 2.0 - it.h * 0.07;
          p.image(IconLoader::get({it.entry->icon, "application-x-executable"}, int(std::lround(is))),
                  ix, iy - lift, is, pressed ? 0.35 : 1.0);
          // Drop position: a bar before or after this tile.
          if (dragging_ && drop_group == it.group && (drop_index == it.index || drop_index == it.index + 1)) {
            double bx = drop_index == it.index ? it.x - gap_ / 2.0 - Sd(1.5) : it.x + it.w + gap_ / 2.0 - Sd(1.5);
            if (drop_index == it.index || !find({K_TILE, it.index + 1, it.group}))
              p.fill_rounded(bx, it.y, Sd(3), it.h, Sd(1.5), t.accent);
          }
          break;
        }
        default: break;
      }
      cairo_restore(p.cr());
    }
    // New group drop zone.
    if (dragging_ && drop_group == int(groups_.size()))
      p.stroke_rounded(new_group_.x, new_group_.y, new_group_.w, new_group_.h, S(6),
                       t.accent.alpha(0.7), Sd(1.5));

    // Scrollbars: thin, wider while hovered or dragged.
    int mx = Fl::event_x_root() - x_root(), my = Fl::event_y_root() - y_root();
    for (auto &a : areas_) {
      if (!a.scrolls()) continue;
      double track = a.h - S(8);
      double len = std::max(Sd(24), track * a.h / a.content);
      double pos = (track - len) * *a.scroll / std::max(1, a.content - a.h);
      bool active = drag_area_ == a.kind ||
                    (mx >= a.bar_x() - S(5) && mx < a.x + a.w && my >= a.y && my < a.y + a.h);
      double bw = active ? Sd(6) : Sd(3);
      double bx = a.bar_x() + Sd(6) - bw;
      if (active) p.fill_rounded(bx, a.y + S(4), bw, track, bw / 2, t.text_dim.alpha(0.12));
      p.fill_rounded(bx, a.y + S(4) + pos, bw, len, bw / 2, t.text_dim.alpha(active ? 0.6 : 0.3));
    }

    // The rail (expanded: over the list, with a shadow).
    int rail_right = rail_w_;
    for (auto &it : items_)
      if (it.kind == K_RAIL) rail_right = std::max(rail_right, it.x + it.w);
    if (rail_open_) {
      p.shadow(0, 0, rail_right, H, 0, t.shadow, S(10));
      p.fill_rect(0, 0, rail_right, H, t.surface.mix(t.surface_alt, 0.5));
      p.line(rail_right - 0.5, 0, rail_right - 0.5, H, t.border);
    }
    for (auto &it : items_) {
      if (it.kind != K_RAIL) continue;
      bool hv = is_hov(it), s = is_sel(it) || (sel == &it && keyboard_);
      if (hv) p.fill_rect(it.x, it.y, it.w, it.h, t.hover);
      if (s && keyboard_) p.stroke_rounded(it.x + S(3), it.y + S(2), it.w - S(6), it.h - S(4), S(4), t.accent, Sd(2));
      double g = S(20), gx = (rail_w_ - g) / 2.0, gy = it.y + (it.h - g) / 2.0;
      switch (it.index) {
        case 0: hamburger_glyph(p, gx, gy, g, t.text); break;
        case 1: {
          double ad = S(26), ax = (rail_w_ - ad) / 2.0, ay = it.y + (it.h - ad) / 2.0;
          if (avatar_) {
            cairo_t *cr = p.cr();
            cairo_save(cr);
            cairo_new_path(cr);
            cairo_arc(cr, ax + ad / 2, ay + ad / 2, ad / 2, 0, 2 * M_PI);
            cairo_clip(cr);
            int sw = cairo_image_surface_get_width(avatar_), sh = cairo_image_surface_get_height(avatar_);
            double sc = ad / std::max(1, std::min(sw, sh));
            cairo_translate(cr, ax + (ad - sw * sc) / 2, ay + (ad - sh * sc) / 2);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, avatar_, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
          } else {
            p.circle(ax + ad / 2, ay + ad / 2, ad / 2, t.accent);
            person_glyph(p, ax + ad * 0.15, ay + ad * 0.12, ad * 0.7, t.on_accent);
          }
          break;
        }
        case 2: document_glyph(p, gx, gy, g, t.text); break;
        case 3: picture_glyph(p, gx, gy, g, t.text); break;
        case 4: glyph::gear(p, gx, gy, g, t.text); break;
        case 5: glyph::power(p, gx, gy, g, t.text); break;
      }
    }

    // The tile being dragged follows the pointer.
    if (dragging_ && !drag_id_.empty()) {
      const DesktopEntry *e = app(drag_id_);
      int tw = 2 * cell_ + gap_;
      double dx = drag_x_ - tw / 2.0, dy = drag_y2_ - tw / 2.0;
      p.shadow(dx, dy, tw, tw, S(4), t.shadow, S(8));
      p.fill_rounded(dx, dy, tw, tw, S(4), tile_hover.alpha(0.95));
      p.stroke_rounded(dx + 0.5, dy + 0.5, tw - 1, tw - 1, S(4), t.accent, Sd(1.5));
      if (e) {
        double is = tw * 0.42;
        p.image(IconLoader::get({e->icon, "application-x-executable"}, int(is)),
                dx + (tw - is) / 2, dy + (tw - is) / 2 - tw * 0.07, is);
      }
    }

    // Launch effect, drawn over the content.
    if (launching_) {
      double q = std::min(1.0, (tnow - launch_start_) / std::max(0.05, effects_.duration));
      double cx = launch_cx_, cy = launch_cy_, ic = launch_size_, k = effects_.size;
      auto *icon = IconLoader::get({launch_icon_, "application-x-executable"}, int(ic * 3));
      const std::string &fx = launch_effect_;
      if (fx == "zoom" || fx == "zoom-out") {
        double s = fx == "zoom" ? ic * (1 + 2.2 * k * ease_out(q)) : ic * (1 - ease_out(q));
        if (s > 1) p.image(icon, cx - s / 2, cy - s / 2, s, 1 - q * (fx == "zoom" ? 1 : 0.3));
      } else if (fx == "ripple") {
        for (int r = 0; r < 3; ++r) {
          double qq = q - r * 0.18;
          if (qq <= 0) continue;
          cairo_new_sub_path(p.cr());
          cairo_arc(p.cr(), cx, cy, ic * (0.6 + 3.0 * k * ease_out(qq)), 0, 2 * M_PI);
          p.set(t.accent.alpha(0.55 * (1 - qq)));
          cairo_set_line_width(p.cr(), Sd(3));
          cairo_stroke(p.cr());
        }
      } else if (fx == "bounce") {
        double jump = Sd(22) * k * std::fabs(std::sin(q * M_PI * 2)) * (1 - q * 0.5);
        p.image(icon, cx - ic / 2.0, cy - ic / 2.0 - jump, ic);
      } else if (fx == "bubbles" || fx == "confetti") {
        static const unsigned colors[] = {0x2672BE, 0xE0525C, 0x2E9E6E, 0xE39A2B, 0x7A4FD1, 0x1C9DB8};
        int n = fx == "bubbles" ? 12 : 26;
        for (int b = 0; b < n; ++b) {
          double angle = (b * 137.5) * M_PI / 180, spread = (0.35 + (b % 5) * 0.16) * k;
          if (fx == "bubbles") {
            double px = cx + std::cos(angle) * ic * spread * 1.4;
            double py = cy - ease_out(q) * Sd(120) * spread * 1.5;
            p.circle(px, py, Sd(3 + b % 4) * k, t.accent.alpha(0.45 * (1 - q)));
          } else {
            double px = cx + std::cos(angle) * Sd(140) * spread * ease_out(q);
            double py = cy + std::sin(angle) * Sd(110) * spread * ease_out(q) + Sd(40) * q * q;
            p.fill_rounded(px, py, Sd(6) * k, Sd(3) * k, Sd(1), Color::hex(colors[b % 6], 1 - q));
          }
        }
      }
    }
  }

  // Text on top of the cairo layer.
  for (auto &it : items_) {
    if (!it.visible || it.kind == K_RAIL) continue;
    bool clipped = it.clipped();
    if (clipped) fl_push_clip(it.cx, it.cy, it.cw, it.ch);
    switch (it.kind) {
      case K_FILTER: {
        text_font(t.font_size, true);
        int tx = it.x + (category_ >= 0 ? S(36) : S(12));
        draw_text(it.label, tx, it.y, it.w - (tx - it.x) - S(32), it.h, t.text);
        break;
      }
      case K_HEADING:
        if (it.index == 4) {
          text_font(t.font_size + 2, true);
          draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim.mix(t.surface, 0.6), 1);
        } else if (it.index == 2) {
          text_font(t.font_size);
          draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim, 1);
        } else {
          text_font(t.font_size - 1, true);
          draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim);
        }
        break;
      case K_LETTER:
        text_font(t.font_size + 1, true);
        draw_text(it.label, it.x + S(10), it.y, it.w - S(20), it.h, t.accent_text);
        break;
      case K_JUMP: {
        bool hv = is_hov(it) || is_sel(it);
        text_font(t.font_size + 2, true);
        draw_text(it.label, it.x, it.y, it.w, it.h, hv ? t.on_accent : t.text, 1);
        break;
      }
      case K_GROUP:
        if (rename_group_ == it.index) break;
        text_font(t.font_size + 1, true);
        draw_text(it.label.empty() ? std::string(_("Name group")) : it.label, it.x + S(4), it.y,
                  it.w - S(36), it.h, it.label.empty() ? t.text_dim : t.text);
        break;
      case K_ROW: {
        text_font(t.font_size);
        int tx = it.x + S(46);
        draw_text(it.entry->name, tx, it.y, it.w - (tx - it.x) - S(6), it.h, t.text);
        break;
      }
      case K_TILE: {
        if (it.size == TileSize::Small) break;
        text_font(t.font_size - 1);
        draw_text(it.entry->name, it.x + S(7), it.y + it.h - S(24), it.w - S(12), S(20),
                  t.text.alpha(dragging_ && press_ == Ref{it.kind, it.index, it.group} ? 0.4 : 1));
        break;
      }
      default: break;
    }
    if (clipped) fl_pop_clip();
  }
  if (rail_open_) {
    for (auto &it : items_) {
      if (it.kind != K_RAIL) continue;
      text_font(t.font_size, it.index == 0);
      draw_text(it.index == 0 ? std::string(_("START")) : it.label, rail_w_ + S(2), it.y,
                it.w - rail_w_ - S(10), it.h, t.text);
    }
  }
  if (!avatar_) {
    // The avatar's initial would hide the glyph; nothing to draw.
  }
  // Tooltips of the collapsed rail.
  if (hov && hov->kind == K_RAIL && !rail_open_) {
    std::string tip = hov->index == 0 ? std::string(_("Expand")) : hov->label;
    text_font(t.font_size - 1);
    int tw = text_width(tip) + S(16), th = S(26);
    int tx = rail_w_ + S(4), ty = std::min(H - th - S(4), hov->y + (hov->h - th) / 2);
    {
      Painter p(W, H);
      p.fill_rounded(tx, ty, tw, th, S(4), t.text.alpha(0.9));
    }
    draw_text(tip, tx, ty, tw, th, t.surface, 1);
  }
  if (launching_ && launch_effect_ == "fade") {
    double q = std::min(1.0, (now() - launch_start_) / std::max(0.05, effects_.duration));
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface.alpha(ease_out(q)));
  }
  draw_children();
  if (qa_dump_enabled()) qa_dump_menu();
}

// UI test hook (see qa_dump in popup.h): the menu draws its own controls, so
// it lists them itself. Lines starting with "@" describe areas: "@menu",
// "@list" and "@tiles" (scrolled areas) and "@list-scroll"/"@tiles-scroll"
// with the offset and its maximum as x and y; "@rail-open" while the rail is
// expanded.
void AppMenu::qa_dump_menu() {
  std::string text;
  auto line = [&](const std::string &name, int x, int y, int w, int h, int active, int scrolled) {
    std::string n = name;
    for (char &c : n)
      if (c == '\t' || c == '\n') c = ' ';
    text += n + "\t" + std::to_string(x) + "\t" + std::to_string(y) + "\t" + std::to_string(w) +
            "\t" + std::to_string(h) + "\t" + std::to_string(active) + "\t" +
            std::to_string(scrolled) + "\n";
  };
  line("@menu", 0, 0, w(), h(), 1, 0);
  if (rail_open_) line("@rail-open", 0, 0, 0, 0, 1, 0);
  for (auto &a : areas_) {
    std::string n = a.kind == K_ROW ? "list" : "tiles";
    line("@" + n, a.x, a.y, a.w, a.h, 1, 0);
    line("@" + n + "-scroll", *a.scroll, std::max(0, a.content - a.h), 0, 0, 1, 0);
  }
  static const char *const rail[] = {"menu", "account", "documents", "pictures", "settings", "power"};
  for (auto &it : items_) {
    if (!it.visible) continue;
    int cx = it.x + it.w / 2, cy = it.y + it.h / 2;
    if (it.clipped() && !(cx >= it.cx && cx < it.cx + it.cw && cy >= it.cy && cy < it.cy + it.ch))
      continue;
    std::string name;
    switch (it.kind) {
      case K_RAIL: name = std::string("rail:") + rail[it.index]; break;
      case K_FILTER: name = "filter:" + it.label; break;
      case K_ROW:
        name = (it.group == 1 ? "added:" : it.group == 2 ? "most-used:" : "app:") + it.entry->name;
        break;
      case K_LETTER: name = "letter:" + it.label; break;
      case K_JUMP: name = "jump:" + it.label; break;
      case K_GROUP: name = "group:" + it.label; break;
      case K_TILE:
        name = "tile:" + it.entry->name;
        line("@tile-size:" + it.entry->name, int(menu::tile_size_code(it.size)), it.group, 0, 0, 1, 0);
        break;
      case K_HEADING:
        if (it.index == 0) name = "heading:" + it.label;
        else continue;
        break;
      default: continue;
    }
    line(name, it.x, it.y, it.w, it.h, 1, it.clipped() ? 1 : 0);
  }
  qa_dump(this, text);
}
