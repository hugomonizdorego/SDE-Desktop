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

#include "buffered_window.h"
#include "build_config.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "menu_perf.h"
#include "panel.h"
#include "search.h"
#include "spawn.h"
#include "suite.h"
#include "util.h"
#include "widgets.h"

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

// Menu item text: "&" would mark a shortcut letter.
std::string menu_text(const std::string &s) {
  std::string out;
  for (char c : s) {
    out += c;
    if (c == '&') out += '&';
  }
  return out;
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

// The question before Power off: a small dialog in the middle of the
// screen. Enter (or the Power off button) confirms, Escape cancels.
class PowerOffDialog : public BufferedWindow {
 public:
  bool confirmed = false;
  PowerOffDialog(int w, int h) : BufferedWindow(w, h) {}
  void draw() override {
    const Theme &t = Panel::instance->theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      p.stroke_rounded(0.5, 0.5, w() - 1, h() - 1, 0, t.border);
      p.circle(S(40), S(44), S(22), t.accent.alpha(0.14));
      glyph::power(p, S(28), S(32), S(24), t.accent_text);
    }
    text_font(t.font_size + 3, true);
    draw_text(_("Power off the computer?"), S(76), S(20), w() - S(92), S(26), t.text);
    text_font(t.font_size);
    draw_text(_("Unsaved work in open applications may be lost."), S(76), S(48), w() - S(92),
              S(22), t.text_dim);
    draw_children();
  }
  int handle(int event) override {
    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
      confirmed = true;
      hide();
      return 1;
    }
    return BufferedWindow::handle(event);
  }
};

PowerOffDialog *g_power_dialog = nullptr;

bool confirm_power_off() {
  // One question at a time: Power off chosen again (the menu reopened
  // meanwhile) brings the open question back instead of a second one.
  if (g_power_dialog) {
    g_power_dialog->show();
    if (fl_xid(g_power_dialog))
      XSetInputFocus(fl_display, fl_xid(g_power_dialog), RevertToParent, CurrentTime);
    return false;
  }
  int W = S(440), H = S(140), bw = S(112), bh = S(34);
  auto *win = new PowerOffDialog(W, H);
  g_power_dialog = win;
  win->copy_label(_("Power off"));
  win->xclass("sde-panel");
  auto *cancel = new FlatButton(W - S(16) - 2 * bw - S(10), H - S(16) - bh, bw, bh, _("Cancel"));
  cancel->copy_tooltip(_("Cancel"));
  cancel->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  auto *ok = new FlatButton(W - S(16) - bw, H - S(16) - bh, bw, bh, _("Power off"), true);
  ok->copy_tooltip(_("Power off"));
  ok->callback([](Fl_Widget *b, void *) {
    static_cast<PowerOffDialog *>(b->window())->confirmed = true;
    b->window()->hide();
  });
  win->end();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, Fl::event_x_root(), Fl::event_y_root());
  win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  win->set_modal();
  win->show();
  // Keys go to the question even without a window manager to focus it.
  win->wait_for_expose();
  if (fl_xid(win)) XSetInputFocus(fl_display, fl_xid(win), RevertToParent, CurrentTime);
  ok->take_focus();
  while (win->shown()) Fl::wait();
  bool yes = win->confirmed;
  g_power_dialog = nullptr;
  Fl::delete_widget(win);
  return yes;
}

// Gallery: two stacked photos.
void gallery_glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  double w = std::max(1.1, s * 0.07);
  cairo_new_path(cr);
  cairo_move_to(cr, x + s * 0.26, y + s * 0.2);
  cairo_line_to(cr, x + s * 0.9, y + s * 0.2);
  cairo_line_to(cr, x + s * 0.9, y + s * 0.66);
  stroke(p, c, w);
  cairo_new_path(cr);
  cairo_rectangle(cr, x + s * 0.1, y + s * 0.32, s * 0.66, s * 0.5);
  cairo_move_to(cr, x + s * 0.1, y + s * 0.76);
  cairo_line_to(cr, x + s * 0.32, y + s * 0.54);
  cairo_line_to(cr, x + s * 0.46, y + s * 0.68);
  cairo_line_to(cr, x + s * 0.56, y + s * 0.6);
  cairo_line_to(cr, x + s * 0.76, y + s * 0.78);
  stroke(p, c, w);
  p.circle(x + s * 0.58, y + s * 0.44, s * 0.05, c);
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

// "Open file location" of an application: the folder of its .desktop file.
void open_file_location(const DesktopEntry &e) {
  if (e.path.empty()) return;
  std::string dir = e.path.substr(0, e.path.rfind('/'));
  if (!dir.empty()) open_folder(dir);
}

void close_app_info();

// "App info...": what the menu knows about an application, with Open and
// Open file location. One at a time; Escape or Close closes it.
class AppInfoDialog : public BufferedWindow {
 public:
  DesktopEntry entry;
  AppInfoDialog(int w, int h, const DesktopEntry &e) : BufferedWindow(w, h), entry(e) {}
  void draw() override {
    const Theme &t = Panel::instance->theme();
    int is = S(48);
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.surface);
      p.stroke_rounded(0.5, 0.5, w() - 1, h() - 1, 0, t.border);
      p.image(IconLoader::get({entry.icon, "application-x-executable"}, is), S(20), S(20), is);
    }
    int tx = S(84), tw = w() - tx - S(20);
    text_font(t.font_size + 3, true);
    draw_text(entry.name, tx, S(20), tw, S(26), t.text);
    text_font(t.font_size);
    draw_text(entry.generic_name.empty() ? std::string(_("Application")) : entry.generic_name, tx,
              S(46), tw, S(22), t.text_dim);
    struct Row {
      const char *label;
      std::string value;
    } rows[] = {{N_("Description"), entry.comment},
                {N_("Category"), category_label(entry.menu_category())},
                {N_("Command"), entry.exec},
                {N_("Desktop file"), entry.path}};
    int y = S(84), lw = S(112);
    std::string qa = "@app-info:" + entry.id + "\t0\t0\t0\t0\t1\t0\n";
    for (auto &r : rows) {
      if (r.value.empty()) continue;
      text_font(t.font_size - 1, true);
      draw_text(_(r.label), S(20), y, lw, S(22), t.text_dim);
      text_font(t.font_size - 1);
      draw_text(r.value, S(20) + lw, y, w() - lw - S(40), S(22), t.text);
      qa += std::string("info:") + r.label + "\t" + std::to_string(S(20)) + "\t" + std::to_string(y) +
            "\t" + std::to_string(w() - S(40)) + "\t" + std::to_string(S(22)) + "\t1\t0\n";
      y += S(24);
    }
    draw_children();
    if (qa_dump_enabled()) qa_dump(this, qa);
  }
  int handle(int event) override {
    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
      close_app_info();
      return 1;
    }
    return BufferedWindow::handle(event);
  }
};

AppInfoDialog *g_app_info = nullptr;

void close_app_info() {
  if (!g_app_info) return;
  g_app_info->hide();
  Fl::delete_widget(g_app_info);
  g_app_info = nullptr;
}

void show_app_info(const DesktopEntry &e) {
  close_app_info();
  int W = S(520), H = S(250), bw = S(150), bh = S(34);
  auto *win = new AppInfoDialog(W, H, e);
  g_app_info = win;
  win->copy_label(_("App info"));
  win->xclass("sde-panel");
  auto *open = new FlatButton(S(20), H - S(16) - bh, S(100), bh, _("Open"), true);
  open->copy_tooltip(_("Open"));
  open->callback([](Fl_Widget *b, void *) {
    DesktopEntry entry = static_cast<AppInfoDialog *>(b->window())->entry;
    close_app_info();
    panel_search::record_launch(entry.id);
    launch_entry(entry);
  });
  auto *where = new FlatButton(S(130), H - S(16) - bh, bw, bh, _("Open file location"));
  where->copy_tooltip(_("Open file location"));
  where->callback([](Fl_Widget *b, void *) {
    DesktopEntry entry = static_cast<AppInfoDialog *>(b->window())->entry;
    close_app_info();
    open_file_location(entry);
  });
  auto *close = new FlatButton(W - S(20) - S(100), H - S(16) - bh, S(100), bh, _("Close"));
  close->copy_tooltip(_("Close"));
  close->callback([](Fl_Widget *, void *) { close_app_info(); });
  win->callback([](Fl_Widget *, void *) { close_app_info(); });
  win->end();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, Fl::event_x_root(), Fl::event_y_root());
  win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  win->show();
  win->wait_for_expose();
  if (fl_xid(win)) XSetInputFocus(fl_display, fl_xid(win), RevertToParent, CurrentTime);
  close->take_focus();
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
  std::string fav = v == sentinel ? cfg.get("menu", "favorites", sentinel) : std::string();
  // The search asks for every keystroke and every result it shows: parse
  // once per change of the setting (the default tiles: of the installed
  // applications).
  bool defaults = v == sentinel && fav == sentinel;
  app_catalog::SnapshotPtr apps = defaults ? app_catalog::current() : nullptr;
  std::string key = v + '\x02' + fav + '\x02' + (apps ? std::to_string(apps->generation) : "");
  static std::string cached_key;
  static std::vector<TileGroup> cached;
  if (key == cached_key) return cached;
  std::vector<TileGroup> groups;
  if (v != sentinel) {
    groups = menu::parse_tile_groups(v);
  } else {
    // alpha15 and older: up to 12 favorites. They become the first group.
    bool user = fav != sentinel;
    auto ids = user ? menu::parse_id_list(fav) : menu::default_favorites(apps->apps, 8);
    groups = menu::groups_from_favorites(ids, user, _("Productivity"), _("Explore"));
  }
  cached_key = key;
  cached = groups;
  return groups;
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
  app_catalog::subscribe(catalog_changed, this);
}

AppMenu::~AppMenu() {
  app_catalog::unsubscribe(this);
  Fl::remove_timeout(poll_applications, this);
  Fl::remove_timeout(animate, this);
  Fl::remove_timeout(glide, this);
  Fl::remove_timeout(preload, this);
  Fl::remove_timeout(qa_dump_later, this);
  if (avatar_) cairo_surface_destroy(avatar_);
}

// ---------------------------------------------------------------------------
// Data

void AppMenu::reload() {
  apps_ = app_catalog::current();
  apps_stale_ = false;
  sorted_.clear();
  sorted_.reserve(apps_->apps.size());
  for (auto &a : apps_->apps) sorted_.push_back(&a);
  categories_ = apps_->categories;
  invalidate();
}

const DesktopEntry *AppMenu::app(const std::string &id) const {
  return apps_ ? apps_->find(id) : nullptr;
}

void AppMenu::load_tiles() {
  tiles_default_ = Panel::instance->config().get("menu", "tiles", "\x01") == "\x01" &&
                   Panel::instance->config().get("menu", "favorites", "\x01") == "\x01";
  groups_ = start_tiles::load();
  usage_ = panel_search::load_usage();
  invalidate();
}

void AppMenu::save_tiles() {
  // Groups emptied by a drag or by unpinning disappear.
  groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                               [](const TileGroup &g) { return g.tiles.empty(); }),
                groups_.end());
  tiles_default_ = false;
  start_tiles::save(groups_);
  invalidate();
}

// Without inotify (app_catalog::watching()): compare the applications
// directories every 2 s while the menu is open, as before 0.2.
void AppMenu::poll_applications(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  Fl::repeat_timeout(2, poll_applications, data);
  app_catalog::poll();
}

// Applications were installed or removed while the panel runs. An open menu
// shows them at once, keeping its scroll positions and category; while a
// context menu, a drag or a rename runs it keeps the snapshot it shows
// (its items point into it) and takes the new one afterwards.
void AppMenu::catalog_changed(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  self->schedule_preload(0.5);
  if (!self->is_open()) return;  // the next opening takes them
  if (self->busy()) {
    self->apps_stale_ = true;
    return;
  }
  self->apply_catalog();
}

bool AppMenu::busy() const {
  return menu_busy_ || launching_ || dragging_ || press_.kind != K_NONE || rename_group_ >= 0 ||
         resize_edge_ || drag_area_ != K_NONE;
}

void AppMenu::apply_catalog() {
  std::string category = category_ >= 0 && category_ < int(categories_.size())
                             ? categories_[size_t(category_)]
                             : "";
  reload();
  if (tiles_default_) groups_ = start_tiles::load();
  auto cat = std::find(categories_.begin(), categories_.end(), category);
  category_ = cat != categories_.end() ? int(cat - categories_.begin()) : -1;
  filter();
  hover_ = anim_ = {};
  redraw();
}

void AppMenu::filter() {
  invalidate();
  list_apps_.clear();
  const std::string *cat =
      category_ >= 0 && category_ < int(categories_.size()) ? &categories_[size_t(category_)] : nullptr;
  for (size_t i = 0; i < apps_->apps.size(); ++i)
    if (category_ < 0 || (cat && apps_->category[i] == *cat)) list_apps_.push_back(&apps_->apps[i]);
  most_used_.clear();
  if (show_most_used_)
    for (auto &id : menu::rank_frecent(usage_, epoch_now(), 32))
      if (const DesktopEntry *e = app(id))
        if (most_used_.size() < kMaxMostUsed) most_used_.push_back(e);
  added_.clear();
  if (show_added_) {
    // The catalog keeps the files' times: no stat() of every entry here.
    std::vector<std::pair<long long, const DesktopEntry *>> fresh;
    long long limit = epoch_now() - kRecentlyAdded;
    for (size_t i = 0; i < apps_->apps.size(); ++i)
      if (apps_->mtime[i] > limit) fresh.push_back({apps_->mtime[i], &apps_->apps[i]});
    std::stable_sort(fresh.begin(), fresh.end(),
                     [](auto &x, auto &y) { return x.first > y.first; });
    for (auto &f : fresh)
      if (added_.size() < kMaxAdded) added_.push_back(f.second);
  }
}

// ---------------------------------------------------------------------------
// Opening

void AppMenu::preferred_size(int &w, int &h) {
  Panel *panel = Panel::instance;
  const Config &cfg = panel->config();
  int mx, my, mw, mh;
  panel->monitor(mx, my, mw, mh);
  // [menu] width/height (dragged by the user, in unscaled pixels), else the
  // size preset.
  int lw = cfg.get_int("menu", "width", 0), lh = cfg.get_int("menu", "height", 0);
  if (lw <= 0 || lh <= 0) {
    std::string sz = cfg.get("menu", "size", "medium");
    lw = sz == "small" ? 600 : sz == "large" ? 940 : 680;
    lh = sz == "small" ? 560 : sz == "large" ? 700 : 620;
  }
  w = std::max(S(360), std::min(S(lw), mw - S(16)));
  h = std::max(S(300), std::min(S(lh), mh - panel->h() - S(24)));
  // PanelPopup::reposition() keeps it between the panel and the screen edge.
  int available = panel->theme().panel_top ? my + mh - panel->y() - panel->h() : panel->y() - my;
  h = std::min(h, std::max(1, available - 2 * S(8)));
}

int start_row_icon_size() {
  int w, h;
  AppMenu::preferred_size(w, h);
  return h < S(470) ? S(22) : S(26);  // compact_ in AppMenu::metrics()
}

void AppMenu::prepare() {
  menu_perf::Step step("prepare");
  perf_layout_ = menu_perf::enabled();
  handing_to_search_ = false;
  theme_ = Panel::instance->theme();
  effects_ = effects_from_config(Panel::instance->config());
  const Config &cfg = Panel::instance->config();
  smooth_ = !cfg.get_bool("appearance", "low_resource", false);
  show_added_ = cfg.get_bool("menu", "show_recently_added", true);
  show_most_used_ = cfg.get_bool("menu", "show_most_used", cfg.get_bool("menu", "show_recent", true));
  int W, H;
  preferred_size(W, H);
  size(W, H);
  rename_->color(theme_.surface_alt.fl());
  rename_->textcolor(theme_.text.fl());
  rename_->cursor_color(theme_.accent.fl());
  rename_->selection_color(theme_.accent.fl());
  rename_->textsize(theme_.font_size + 1);
  rename_->hide();
  rename_group_ = -1;
  category_ = -1;
  list_scroll_ = tiles_scroll_ = 0;
  list_glide_ = tiles_glide_ = Glide();
  Fl::remove_timeout(glide, this);
  drag_area_ = K_NONE;
  letters_open_ = rail_open_ = false;
  hover_ = selected_ = anim_ = press_ = {};
  dragging_ = false;
  resize_edge_ = cursor_edge_ = 0;
  keyboard_ = false;
  launching_ = false;
  bar_hot_ = 0;
  super_down_ = super_alone_ = false;
  if (!app_catalog::watching()) app_catalog::poll();
  {
    menu_perf::Step s("reload");
    reload();
  }
  {
    menu_perf::Step s("tiles");
    load_tiles();
  }
  {
    menu_perf::Step s("filter");
    filter();
  }
  menu_perf::Step avatar_step("avatar");

  full_name_ = user_name();
  // The picture is decoded again only when its file changed.
  int ad = S(28);
  bool found = false;
  for (const std::string &f : {home_dir() + "/.face", home_dir() + "/.face.icon",
                               "/var/lib/AccountsService/icons/" + env("USER")}) {
    struct stat st;
    if (stat(f.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    long long mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    std::string key = f + "@" + std::to_string(ad);
    if (avatar_ && key == avatar_file_ && mtime == avatar_mtime_) {
      found = true;
      break;
    }
    if (cairo_surface_t *img = load_image(f, ad * 2, ad * 2)) {
      if (avatar_) cairo_surface_destroy(avatar_);
      avatar_ = img;
      avatar_file_ = key;
      avatar_mtime_ = mtime;
      found = true;
      break;
    }
  }
  if (!found && avatar_) {
    cairo_surface_destroy(avatar_);
    avatar_ = nullptr;
    avatar_file_.clear();
  }
  Fl::remove_timeout(poll_applications, this);
  if (!app_catalog::watching()) Fl::add_timeout(2, poll_applications, this);
}

void AppMenu::closed() {
  Fl::remove_timeout(animate, this);
  Fl::remove_timeout(glide, this);
  Fl::remove_timeout(poll_applications, this);
  list_glide_.on = tiles_glide_.on = false;
  launching_ = false;
  letters_open_ = rail_open_ = false;
  dragging_ = false;
  press_ = {};
  rename_->hide();
  rename_group_ = -1;
  super_down_ = false;
  cursor(FL_CURSOR_DEFAULT);
}

// sde.conf changed while the menu is open (Settings, dark mode from the
// Action Center, another accent): follow it at once.
void AppMenu::config_changed() {
  schedule_preload(0.8);  // the icon theme may have changed
  if (!is_open()) return;
  theme_ = Panel::instance->theme();
  effects_ = effects_from_config(Panel::instance->config());
  const Config &cfg = Panel::instance->config();
  smooth_ = !cfg.get_bool("appearance", "low_resource", false);
  bool added = cfg.get_bool("menu", "show_recently_added", true);
  bool used = cfg.get_bool("menu", "show_most_used", cfg.get_bool("menu", "show_recent", true));
  rename_->color(theme_.surface_alt.fl());
  rename_->textcolor(theme_.text.fl());
  rename_->cursor_color(theme_.accent.fl());
  rename_->selection_color(theme_.accent.fl());
  show_added_ = added;
  show_most_used_ = used;
  if (!busy()) {
    // Tiles pinned from the search or Settings; the menu itself saves
    // every change at once, so this is what it shows anyway.
    load_tiles();
    filter();
  }
  invalidate();
  redraw();
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
  // Every event and every frame asks for the layout; it only changes with
  // the size, the scroll positions, the views and the data (invalidate()).
  if (built_rev_ == layout_rev_ && built_w_ == w() && built_h_ == h() &&
      built_list_ == list_scroll_ && built_tiles_ == tiles_scroll_ &&
      built_category_ == category_ && built_letters_ == letters_open_ &&
      built_rail_ == rail_open_ && built_dragging_ == dragging_)
    return;
  items_.clear();
  areas_.clear();
  metrics();
  layout_list();
  layout_tiles();
  layout_rail();  // on top: the expanded rail covers the list
  built_rev_ = layout_rev_;
  built_w_ = w();
  built_h_ = h();
  built_list_ = list_scroll_;
  built_tiles_ = tiles_scroll_;
  built_category_ = category_;
  built_letters_ = letters_open_;
  built_rail_ = rail_open_;
  built_dragging_ = dragging_;
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
  // From the bottom: Power off, a small gap, then Settings, Gallery,
  // Pictures, Documents and the account (read top to bottom: account,
  // Documents, Pictures, Gallery, Settings ... Power off).
  int y = H - S(4) - bh;
  add(5, y, _("Power off"));
  y -= bh + S(10);
  const char *names[] = {nullptr, nullptr, N_("Documents"), N_("Pictures"), N_("Control Center"),
                         nullptr, N_("Gallery")};
  for (int i : {4, 6, 3, 2, 1}) {
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
          scroll_to(K_ROW, list_scroll_ + h.y - list_top_, false);
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
      // The user's name and their account page; locking and logging out
      // live in the Action Center's footer.
      std::string name = menu_text(full_name_.empty() ? std::string(_("Account")) : full_name_);
      Fl_Menu_Item items[] = {
          {name.c_str(), 0, nullptr, nullptr, FL_MENU_INACTIVE | FL_MENU_DIVIDER, 0, 0, 0, 0},
          {_("Account and system information"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
          {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
      const Item *it = find({K_RAIL, 1, -1});
      menu_busy_ = true;
      const Fl_Menu_Item *m = items->popup(it ? it->x + it->w : 0, it ? it->y : 0);
      menu_busy_ = false;
      if (!m) return;
      int i = int(m - items);
      close();
      if (i == 1) spawn_detached({"sde-settings", "--page", "about"});
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
    case 6:
      close();
      spawn_detached({"sde-gallery"});
      return;
    case 5:
      // The one session button of the menu: Power off, after a question
      // (sleep, restart and log out are in the Action Center).
      close();
      Fl::add_timeout(0, [](void *) {
        if (confirm_power_off()) spawn_detached({"sde-logout", "--action", "poweroff"});
      });
      return;
  }
}

// The category filter: a short menu instead of a row of chips.
void AppMenu::filter_menu() {
  std::vector<std::string> labels;
  labels.push_back(format("%s (%d)", _("All apps"), int(sorted_.size())));
  for (auto &c : categories_) {
    int n = int(std::count(apps_->category.begin(), apps_->category.end(), c));
    labels.push_back(format("%s (%d)", menu_text(category_label(c)).c_str(), n));
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
  scroll_to(K_ROW, 0, false);
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
  DesktopEntry entry = it.entry ? *it.entry : DesktopEntry();
  bool pinned = Panel::instance->is_pinned(tile.id);
  std::vector<std::string> texts;
  texts.reserve(groups_.size() + 2);
  enum { OPEN = 1, UNPIN, SMALL, MEDIUM, WIDE, LARGE, PANEL, NEW_GROUP, LOCATION, INFO, GROUP0 = 100 };
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
    texts.push_back(groups_[i].name.empty() ? std::string(_("Unnamed group"))
                                            : menu_text(groups_[i].name));
    add(texts.back().c_str(), GROUP0 + long(i));
  }
  add(_("New Group"), NEW_GROUP);
  add(nullptr, 0);
  add(pinned ? _("Unpin from Panel") : _("Pin to Panel"), PANEL, FL_MENU_DIVIDER);
  add(_("Open file location"), LOCATION, entry.path.empty() ? FL_MENU_INACTIVE : 0);
  add(_("App info..."), INFO);
  add(nullptr, 0);
  menu_busy_ = true;
  const Fl_Menu_Item *m = items.data()->popup(px, py);
  menu_busy_ = false;
  if (!m) return;
  long code = reinterpret_cast<long>(m->user_data());
  auto &tiles = g.tiles;
  switch (code) {
    case OPEN: launch(it); return;
    case LOCATION:
      close();
      open_file_location(entry);
      return;
    case INFO:
      close();
      show_app_info(entry);
      return;
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
  enum { OPEN = 1, START, PANEL, DESKTOP, FORGET, LOCATION, INFO };
  std::vector<Fl_Menu_Item> items;
  auto add = [&](const char *text, long code, int flags = 0) {
    items.push_back({text, 0, nullptr, reinterpret_cast<void *>(code), flags, 0, 0, 0, 0});
  };
  add(_("Open"), OPEN, FL_MENU_DIVIDER);
  add(tile ? _("Unpin from Start") : _("Pin to Start"), START);
  add(pinned ? _("Unpin from Panel") : _("Pin to Panel"), PANEL);
  add(_("Add to Desktop"), DESKTOP, used ? 0 : FL_MENU_DIVIDER);
  if (used) add(_("Remove from Most Used"), FORGET, FL_MENU_DIVIDER);
  add(_("Open file location"), LOCATION, e.path.empty() ? FL_MENU_INACTIVE : 0);
  add(_("App info..."), INFO);
  add(nullptr, 0);
  menu_busy_ = true;
  const Fl_Menu_Item *m = items.data()->popup(px, py);
  menu_busy_ = false;
  if (!m) return;
  switch (reinterpret_cast<long>(m->user_data())) {
    case OPEN: launch(it); return;
    case LOCATION:
      close();
      open_file_location(e);
      return;
    case INFO:
      close();
      show_app_info(e);
      return;
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
      invalidate();
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
    invalidate();
  }
  redraw();
}

void AppMenu::redirect_to_search(const std::string &text) {
  // Open the search first so it takes the keyboard straight from the menu,
  // and keep the menu mapped a moment: keys typed quickly after the first
  // one are still queued for this window and are forwarded from here.
  bool first = !handing_to_search_;
  handing_to_search_ = true;
  menu_perf::begin(first ? "key-open" : "key", "search");
  Panel::instance->open_search(text, true);
  if (first)
    Fl::add_timeout(0.25, [](void *d) {
      auto *menu = static_cast<AppMenu *>(d);
      if (menu->handing_to_search_) {
        menu->handing_to_search_ = false;
        menu->close();
      }
    }, this);
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
  if (!it.clipped()) return;
  Kind kind = it.kind == K_ROW || it.kind == K_LETTER   ? K_ROW
              : it.kind == K_TILE || it.kind == K_GROUP ? K_TILE
                                                        : K_NONE;
  if (kind == K_NONE) return;
  int scroll = kind == K_TILE ? tiles_scroll_ : list_scroll_;
  if (it.y < it.cy) scroll -= it.cy - it.y;
  else if (it.y + it.h > it.cy + it.ch) scroll += it.y + it.h - (it.cy + it.ch);
  else return;
  scroll_to(kind, scroll, false);
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

namespace {
const double kGlideTime = 0.13;  // seconds from one scroll position to the next
}

// Wheel and Page keys glide from where the area is now; a wheel turned
// on while it glides goes on from where it was heading.
void AppMenu::scroll_by(Kind kind, int dy) {
  build_items();
  const Area *a = area(kind);
  if (!a || !a->scrolls()) return;
  const Glide &g = kind == K_TILE ? tiles_glide_ : list_glide_;
  scroll_to(kind, (g.on ? g.to : *a->scroll) + dy, true);
}

void AppMenu::scroll_to(Kind kind, int offset, bool smooth) {
  int &scroll = kind == K_TILE ? tiles_scroll_ : list_scroll_;
  Glide &g = kind == K_TILE ? tiles_glide_ : list_glide_;
  build_items();
  const Area *a = area(kind);
  offset = std::max(0, std::min(offset, a ? std::max(0, a->content - a->h) : offset));
  if (!smooth || !smooth_ || !shown() || std::abs(offset - scroll) <= 1) {
    g.on = false;
    if (scroll != offset) {
      scroll = offset;
      hover_ = {};
      damage_area(kind);
    }
    return;
  }
  g.from = scroll;
  g.to = offset;
  g.start = now();
  g.on = true;
  // The items slide away under the pointer; rehover() follows at the end.
  if (hover_.kind != K_NONE) damage_item(hover_, S(12));
  hover_ = {};
  if (!Fl::has_timeout(glide, this)) Fl::add_timeout(1.0 / 60, glide, this);
}

// One frame of smooth scrolling: only the scrolled area is repainted.
void AppMenu::glide(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  double t = now();
  bool more = false;
  for (Kind kind : {K_ROW, K_TILE}) {
    Glide &g = kind == K_TILE ? self->tiles_glide_ : self->list_glide_;
    if (!g.on) continue;
    int &scroll = kind == K_TILE ? self->tiles_scroll_ : self->list_scroll_;
    double p = std::min(1.0, (t - g.start) / kGlideTime);
    int pos = int(std::lround(g.from + (g.to - g.from) * ease_out(p)));
    if (p >= 1.0) {
      pos = g.to;
      g.on = false;
    } else {
      more = true;
    }
    if (pos != scroll) {
      scroll = pos;
      self->damage_area(kind);
    }
  }
  if (more) Fl::repeat_timeout(1.0 / 60, glide, data);
  else self->rehover();
}

// The item under a pointer that did not move (after scrolling).
void AppMenu::rehover() {
  if (!shown() || busy()) return;
  int mx = Fl::event_x_root() - x_root(), my = Fl::event_y_root() - y_root();
  build_items();
  const Item *hit = mx >= 0 && my >= 0 && mx < w() && my < h() ? item_at(mx, my) : nullptr;
  Ref r = hit ? Ref{hit->kind, hit->index, hit->group} : Ref{};
  if (r == hover_) return;
  damage_item(hover_, S(12));
  hover_ = r;
  if (hit && hit->entry && !keyboard_) {
    damage_item(selected_, S(4));
    selected_ = r;
  }
  damage_item(hover_, S(12));
}

// Scrollbar thumb dragged to window y |y| (see FL_DRAG).
void AppMenu::drag_scrollbar(int y) {
  const Area *a = area(drag_area_);
  if (!a || !a->scrolls()) return;
  double track = a->h - S(8);
  double len = std::max(Sd(24), track * a->h / a->content);
  double per_px = (a->content - a->h) / std::max(1.0, track - len);
  scroll_to(drag_area_, drag_scroll_ + int(std::lround((y - drag_y_) * per_px)), false);
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
  Ref old = selected_;
  keyboard_ = true;
  if (key == FL_Page_Down || key == FL_Page_Up) {
    int dir = key == FL_Page_Down ? 1 : -1;
    Kind list = cur && (cur->kind == K_TILE || cur->kind == K_GROUP) ? K_TILE : K_ROW;
    const Area *a = area(list);
    if (!a) return;
    bool keep = cur && (cur->kind == K_ROW || cur->kind == K_TILE) && cur->visible;
    int ref_x = keep ? cur->x : a->x, ref_y = keep ? cur->y : a->y;
    int &scroll = list == K_TILE ? tiles_scroll_ : list_scroll_;
    const Glide &g = list == K_TILE ? tiles_glide_ : list_glide_;
    int from = scroll, base = g.on ? g.to : scroll;
    int target = a->scrolls() ? std::max(0, std::min(a->content - a->h, base + dir * (a->h - S(36))))
                              : base;
    bool moved = target != base;
    // Choose the item where the area will be, then glide there. ref_y is
    // where the selection is on screen now.
    ref_y += base - from;
    scroll = target;
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
      if (best->clipped()) {
        if (best->y < best->cy) target -= best->cy - best->y;
        else if (best->y + best->h > best->cy + best->ch)
          target += best->y + best->h - (best->cy + best->ch);
      }
    }
    scroll = from;
    damage_item(old, S(4));
    scroll_to(list, target, true);
    damage_item(selected_, S(4));
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
    damage_item(selected_, S(4));
    return;
  }
  double cx = cur->x + cur->w / 2.0, cy = cur->y + cur->h / 2.0;
  const Item *best = nullptr;
  double best_score = 1e18;
  for (auto &it : items_) {
    if (&it == cur || !selectable(it.kind)) continue;
    // Items scrolled out of view count in the area of the selection (Down
    // on the last visible row goes to the next row, not to the rail).
    bool same_area = it.clipped() && cur->clipped() && it.cx == cur->cx && it.cy == cur->cy;
    if (!it.visible && !same_area) continue;
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
    Item copy = *best;
    damage_item(old, S(4));
    ensure_visible(copy);
    damage_item(selected_, S(4));
  }
}

namespace {
// Keyboard zones of the menu, in Tab order.
enum Zone { Z_NONE = -1, Z_START, Z_FILTER, Z_LIST, Z_TILES, Z_RAIL, Z_COUNT };
}  // namespace

// Tab and Shift+Tab: the Start button, the category filter, the list, the
// tiles and the places of the rail, in turn.
void AppMenu::move_zone(bool back) {
  build_items();
  auto zone_of = [](const Item &it) {
    switch (it.kind) {
      case K_RAIL: return it.index == 0 ? Z_START : Z_RAIL;
      case K_FILTER: return Z_FILTER;
      case K_ROW:
      case K_LETTER:
      case K_JUMP: return Z_LIST;
      case K_GROUP:
      case K_TILE: return Z_TILES;
      default: return Z_NONE;
    }
  };
  // The top left item of a zone that is in view, applications and tiles
  // before headings (the rail from its top: the account).
  auto first_of = [&](int zone) -> const Item * {
    const Item *best = nullptr;
    for (int pass = 0; pass < 2 && !best; ++pass)
      for (auto &it : items_) {
        if (zone_of(it) != zone || !it.visible) continue;
        if (pass == 0 && (it.kind == K_LETTER || it.kind == K_GROUP)) continue;
        if (!best || it.y < best->y || (it.y == best->y && it.x < best->x)) best = &it;
      }
    return best;
  };
  const Item *cur = find(selected_);
  int zone = cur ? zone_of(*cur) : Z_NONE;
  Ref old = selected_;
  for (int step = 1; step <= Z_COUNT; ++step) {
    int next = zone == Z_NONE ? (back ? Z_COUNT - step : step - 1) : zone + (back ? -step : step);
    next = (next % Z_COUNT + Z_COUNT) % Z_COUNT;
    if (const Item *it = first_of(next)) {
      keyboard_ = true;
      selected_ = {it->kind, it->index, it->group};
      break;
    }
  }
  damage_item(old, S(4));
  damage_item(selected_, S(4));
}

// Home and End: the first or last item of the list (or of the tiles when a
// tile is selected), scrolled into view.
void AppMenu::move_end(bool end) {
  build_items();
  const Item *cur = find(selected_);
  bool tiles = cur && (cur->kind == K_TILE || cur->kind == K_GROUP);
  Kind kind = tiles ? K_TILE : letters_open_ ? K_JUMP : K_ROW;
  const Item *pick = nullptr;
  for (auto &it : items_)
    if (it.kind == kind) {
      pick = &it;
      if (!end) break;
    }
  if (!pick) return;
  Ref old = selected_;
  keyboard_ = true;
  selected_ = {pick->kind, pick->index, pick->group};
  damage_item(old, S(4));
  if (kind != K_JUMP) {
    const Area *a = area(tiles ? K_TILE : K_ROW);
    if (a) scroll_to(a->kind, end ? a->content : 0, true);
  }
  damage_item(selected_, S(4));
}

// ---------------------------------------------------------------------------
// Animations

namespace {
// The glow of a hovered item pulses this long, then it stays still: a
// pointer resting on a tile costs no frames (it pulsed forever before 0.2).
const double kGlowPulse = 2.0;
}  // namespace

void AppMenu::start_hover(const Ref &r) {
  anim_ = r;
  anim_hover_ = 0;
  anim_start_ = now();
  if (!effects_.on || effects_.hover == "none") return;
  if (!Fl::has_timeout(animate, this)) Fl::add_timeout(1.0 / 60, animate, this);
}

// One frame of the hover effect (only the hovered item is repainted) or of
// the launch effect (the menu until it closes).
void AppMenu::animate(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  double t = now();
  if (self->launching_) {
    if (t - self->launch_start_ >= self->effects_.duration) {
      self->launching_ = false;
      self->close();
      return;
    }
    self->redraw();
    Fl::repeat_timeout(1.0 / 60, animate, data);
    return;
  }
  bool more = false;
  if (self->anim_.kind != K_NONE && self->hover_ == self->anim_) {
    double hover_time = 0.18 * (self->effects_.duration / 0.32);
    double e = t - self->anim_start_;
    self->anim_hover_ = std::min(1.0, e / hover_time);
    const std::string &h = self->effects_.hover;
    more = self->anim_hover_ < 1.0 || (h == "glow" && e < kGlowPulse) || (h == "bounce" && e < 1.0);
    self->damage_item(self->anim_, S(14));
  }
  if (more) Fl::repeat_timeout(1.0 / 60, animate, data);
}

// ---------------------------------------------------------------------------
// Partial repaints

void AppMenu::damage_item(const Ref &r, int grow) {
  if (r.kind == K_NONE || !shown()) return;
  build_items();
  const Item *it = find(r);
  if (!it || !it->visible) return;
  int x0 = it->x - grow, y0 = it->y - grow, x1 = it->x + it->w + grow, y1 = it->y + it->h + grow;
  if (it->clipped()) {
    x0 = std::max(x0, it->cx);
    y0 = std::max(y0, it->cy);
    x1 = std::min(x1, it->cx + it->cw);
    y1 = std::min(y1, it->cy + it->ch);
  }
  if (x1 > x0 && y1 > y0) damage_rect(x0, y0, x1 - x0, y1 - y0);
}

void AppMenu::damage_area(Kind kind) {
  build_items();
  if (const Area *a = area(kind)) damage_rect(a->x, a->y, a->w, a->h);
  else redraw();
}

// The tooltip of a collapsed rail button, drawn beside the rail.
void AppMenu::damage_tooltip(const Ref &r) {
  if (r.kind != K_RAIL || rail_open_) return;
  const Item *it = find(r);
  if (!it) return;
  int th = S(26), ty = std::min(h() - th - S(4), it->y + (it->h - th) / 2);
  damage_rect(rail_w_, ty - S(2), w() - rail_w_, th + S(4));
}

// ---------------------------------------------------------------------------
// Icons on idle

// The icons the menu shows (tiles, the list rows, the search's top apps),
// loaded a few at a time when the panel has nothing else to do: the first
// opening and scrolling to new rows no longer wait for SVG files (a cold
// first opening took 680 ms on Xvfb, 650 of them for 18 icons).
void AppMenu::schedule_preload(double delay) {
  if (!app_catalog::ready()) return;
  ++preload_rev_;
  preload_.clear();
  preload_next_ = 0;
  Fl::remove_timeout(preload, this);
  Fl::add_timeout(delay, preload, this);
}

void AppMenu::preload(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (self->preload_.empty() && self->preload_next_ == 0) {
    // Lay the menu out as it will open (hidden: only the item list).
    if (!self->is_open()) {
      int W, H;
      preferred_size(W, H);
      self->size(W, H);
      self->reload();
      self->load_tiles();
      self->filter();
    }
    self->build_items();
    auto add = [&](const std::string &icon, int size) {
      if (size > 0) self->preload_.push_back({icon, size});
    };
    int row = start_row_icon_size();
    for (auto &it : self->items_)
      if (it.kind == K_TILE && it.entry) {
        double is = std::min(it.w, it.h) *
                    (it.size == TileSize::Small ? 0.62 : it.size == TileSize::Large ? 0.38 : 0.42);
        add(it.entry->icon, int(std::lround(is)));
      }
    for (auto *e : self->most_used_) add(e->icon, row);
    for (auto *e : self->added_) add(e->icon, row);
    for (auto &a : self->apps_->apps) add(a.icon, row);
    // The search's start page: the top apps.
    for (auto *e : self->most_used_) add(e->icon, S(36));
    self->preload_next_ = 0;
    if (menu_perf::enabled())
      menu_perf::log("preload icons=" + std::to_string(self->preload_.size()));
  }
  // A few milliseconds per turn of the event loop, so that input and
  // frames always come first.
  double end = menu_perf::now_ms() + 4.0;
  while (self->preload_next_ < self->preload_.size() && menu_perf::now_ms() < end) {
    auto &p = self->preload_[self->preload_next_++];
    IconLoader::get({p.first, "application-x-executable"}, p.second);
  }
  if (self->preload_next_ < self->preload_.size()) {
    Fl::repeat_timeout(self->is_open() ? 0.05 : 0.01, preload, data);
    return;
  }
  if (menu_perf::enabled()) menu_perf::log("preload done");
  self->preload_.clear();
  self->preload_next_ = 0;
}

// ---------------------------------------------------------------------------
// Events

int AppMenu::handle(int event) {
  if (launching_ && (event == FL_PUSH || event == FL_KEYBOARD || event == FL_SHORTCUT)) return 1;
  // Applications changed during a context menu or a drag: take them now.
  if (apps_stale_ && !busy() && event != FL_HIDE && event != FL_SHOW) apply_catalog();
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
          Kind kind = a.kind;
          scroll_to(kind, *a.scroll, false);  // stop a glide where it is
          drag_area_ = kind;
          drag_y_ = ey;
          drag_scroll_ = kind == K_TILE ? tiles_scroll_ : list_scroll_;
          if (ey < pos || ey > pos + len) {
            drag_y_ = int(pos + len / 2);
            drag_scrollbar(ey);
            drag_y_ = ey;
            drag_scroll_ = kind == K_TILE ? tiles_scroll_ : list_scroll_;
          }
          damage_area(kind);
          return 1;
        }
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (it.kind == K_TILE || it.kind == K_ROW) {
          damage_item(selected_, S(4));
          selected_ = {it.kind, it.index, it.group};
          damage_item(selected_, S(4));
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
        menu_perf::begin("drag", "menu");
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
            if (ey < a->y + S(24)) scroll_to(K_TILE, tiles_scroll_ - S(12), false);
            else if (ey > a->y + a->h - S(24)) scroll_to(K_TILE, tiles_scroll_ + S(12), false);
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
        Panel::instance->set_settings(
            {{"menu", "width", std::to_string(int(std::lround(w() / sc)))},
             {"menu", "height", std::to_string(int(std::lround(h() / sc)))}});
        return 1;
      }
      if (drag_area_ != K_NONE) {
        Kind kind = drag_area_;
        drag_area_ = K_NONE;
        damage_area(kind);
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
        if (it && it->contains(Fl::event_x(), Fl::event_y())) {
          Item copy = *it;
          activate(copy);
        }
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
        // Only the item left and the item entered are repainted.
        menu_perf::begin("hover", "menu");
        Ref old_hover = hover_, old_sel = selected_;
        bool was_keyboard = keyboard_;
        hover_ = r;
        if (hit && hit->entry) {
          selected_ = r;
          keyboard_ = false;
          start_hover(r);
        }
        damage_item(old_hover, S(14));
        damage_tooltip(old_hover);
        damage_item(hover_, S(14));
        damage_tooltip(hover_);
        if (!(old_sel == selected_) || was_keyboard != keyboard_) damage_item(old_sel, S(4));
      }
      int hot = 0;
      for (auto &a : areas_)
        if (a.scrolls() && ex >= a.bar_x() - S(5) && ex < a.x + a.w && ey >= a.y && ey < a.y + a.h)
          hot = a.kind == K_TILE ? 2 : 1;
      if (hot != bar_hot_) {
        if (bar_hot_) damage_area(bar_hot_ == 2 ? K_TILE : K_ROW);
        if (hot) damage_area(hot == 2 ? K_TILE : K_ROW);
        bar_hot_ = hot;
      }
      return 1;
    }
    case FL_LEAVE:
      if (cursor_edge_) {
        cursor_edge_ = 0;
        cursor(FL_CURSOR_DEFAULT);
      }
      if (hover_.kind != K_NONE) {
        damage_item(hover_, S(14));
        damage_tooltip(hover_);
        hover_ = {};
      }
      if (bar_hot_) {
        damage_area(bar_hot_ == 2 ? K_TILE : K_ROW);
        bar_hot_ = 0;
      }
      return PanelPopup::handle(event);
    case FL_MOUSEWHEEL: {
      // The wheel scrolls the area under the pointer. The menu holds the
      // pointer grab, so take the position from root coordinates.
      int ex = Fl::event_x_root() - x_root(), ey = Fl::event_y_root() - y_root();
      int dy = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
      if (!dy || menu_busy_) return 1;
      menu_perf::begin("wheel", "menu");
      build_items();
      const Area *a = area_at(ex, ey);
      if (!a) a = area(ex >= tiles_x_ ? K_TILE : K_ROW);
      if (a) scroll_by(a->kind, dy * (compact_ ? S(64) : S(72)));
      return 1;
    }
    case FL_KEYUP: {
      // Super pressed and released alone closes the menu, as Super opened
      // it (the window manager does not see the key while the menu holds
      // the keyboard).
      int k = Fl::event_key();
      if ((k == FL_Meta_L || k == FL_Meta_R) && super_down_) {
        super_down_ = false;
        if (super_alone_ && rename_group_ < 0) {
          close();
          return 1;
        }
      }
      return 1;
    }
    case FL_SHORTCUT:
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      if (k == FL_Meta_L || k == FL_Meta_R) {
        super_down_ = true;
        super_alone_ = true;
        return 1;
      }
      super_alone_ = false;
      if (rename_group_ >= 0) return rename_->handle(FL_KEYBOARD);
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
          scroll_to(K_ROW, 0, false);
          filter();
        } else {
          close();
          return 1;
        }
        redraw();
        return 1;
      }
      build_items();
      const Item *found = find(selected_);
      // A copy: the items are laid out again while a context menu is open.
      Item sel = found ? *found : Item();
      if (k == FL_Menu || (k == FL_F + 10 && (Fl::event_state() & FL_SHIFT))) {
        if (found) {
          int px = sel.x + sel.w / 2, py = sel.y + sel.h / 2;
          if (sel.kind == K_TILE) tile_menu(sel, px, py);
          else if (sel.kind == K_ROW) row_menu(sel, px, py);
          else if (sel.kind == K_GROUP) group_menu(sel.index, px, py);
        }
        return 1;
      }
      if (k == FL_Enter || k == FL_KP_Enter || (k == ' ' && keyboard_)) {
        if (found) activate(sel);
        return 1;
      }
      if (k == FL_Down || k == FL_Up || k == FL_Left || k == FL_Right || k == FL_Page_Down ||
          k == FL_Page_Up) {
        menu_perf::begin("nav", "menu");
        move_selection(k);
        return 1;
      }
      if (k == FL_Tab) {
        menu_perf::begin("nav", "menu");
        move_zone(Fl::event_state() & FL_SHIFT);
        return 1;
      }
      if (k == FL_Home || k == FL_End) {
        menu_perf::begin("nav", "menu");
        move_end(k == FL_End);
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

void AppMenu::flush() {
  if (!menu_perf::enabled()) {
    PanelPopup::flush();
    return;
  }
  double t0 = menu_perf::now_ms();
  IconLoader::Stats icons = IconLoader::stats();
  PanelPopup::flush();
  double t1 = menu_perf::now_ms();
  XSync(fl_display, False);
  IconLoader::Stats after = IconLoader::stats();
  if (after.loads != icons.loads) {
    menu_perf::note("icons", double(after.loads - icons.loads));
    menu_perf::note("icon_ms", after.ms - icons.ms);
  }
  menu_perf::frame("menu", t1 - t0, menu_perf::now_ms() - t1);
}

void AppMenu::draw() {
  const Theme &t = theme_;
  int W = w(), H = h();
  double tnow = now();
  build_items();
  if (perf_layout_) {
    perf_layout_ = false;
    std::string line = "layout menu window=" + std::to_string(x_root()) + "," +
                       std::to_string(y_root()) + "," + std::to_string(W) + "," + std::to_string(H);
    for (auto &a : areas_)
      line += std::string(a.kind == K_ROW ? " list=" : " tiles=") + std::to_string(a.x) + "," +
              std::to_string(a.y) + "," + std::to_string(a.w) + "," + std::to_string(a.h) + "," +
              std::to_string(a.content);
    int rows = 0, tiles = 0;
    for (auto &it : items_) rows += it.kind == K_ROW, tiles += it.kind == K_TILE;
    line += " rows=" + std::to_string(rows) + " ntiles=" + std::to_string(tiles) +
            " apps=" + std::to_string(apps_.size());
    menu_perf::log(line);
  }
  const Item *hov = find(hover_);
  const Item *sel = find(selected_);
  auto is_sel = [&](const Item &it) { return sel == &it && (keyboard_ || it.entry); };
  auto is_hov = [&](const Item &it) { return hov == &it; };
  int drop_group = -1, drop_index = 0;
  if (dragging_) drop_target(drag_x_, drag_y2_, drop_group, drop_index);

  // The hover effect of an item: lift (wave, bounce), scale (pop), slide
  // to the right (slide) and a glow behind the icon (glow; it pulses for a
  // moment and then stays).
  struct Fx {
    double lift = 0, scale = 1, slide = 0, glow = 0;
  };
  auto hover_of = [&](const Item &it) {
    Fx fx;
    if (!effects_.on || !(Ref{it.kind, it.index, it.group} == anim_) || !(anim_ == hover_)) return fx;
    double p = ease_out(anim_hover_), k = effects_.size, e = tnow - anim_start_;
    const std::string &h = effects_.hover;
    if (h == "wave") fx.lift = Sd(4) * p * k;
    else if (h == "pop") fx.scale = 1 + 0.12 * p * k;
    else if (h == "slide") fx.slide = Sd(6) * p * k;
    else if (h == "glow")
      fx.glow = p * (0.85 + 0.15 * std::sin(e * 5.0) * std::max(0.0, 1 - e / kGlowPulse));
    else if (h == "bounce")
      fx.lift = e < 1.0 ? Sd(7) * k * std::fabs(std::sin(e * M_PI * 3)) * (1 - e) : 0;
    return fx;
  };
  // An icon drawn at |size|; while it grows (pop) the largest size is used
  // and scaled down, instead of a new rendering for every frame.
  auto icon = [&](const std::string &name, double base, double scale) {
    int load = int(std::lround(scale > 1.0001 ? base * (1 + 0.12 * effects_.size) : base));
    return IconLoader::get({name, "application-x-executable"}, load);
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
    stroke_outline(p, t.border);

    for (auto &it : items_) {
      if (!it.visible || it.kind == K_RAIL) continue;
      // Outside the repainted part (a hover change, one scrolled area).
      if (!fl_not_clipped(it.x - S(14), it.y - S(14), it.w + S(28), it.h + S(28))) continue;
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
          Fx fx = hover_of(it);
          bool pressed = dragging_ && press_ == Ref{it.kind, it.index, it.group};
          if (s) p.fill_rounded(it.x, it.y, it.w, it.h, S(6), t.accent.alpha(t.dark ? 0.28 : 0.16));
          else if (hv) p.fill_rounded(it.x, it.y, it.w, it.h, S(6), t.hover);
          if (s && keyboard_)
            p.stroke_rounded(it.x + 1, it.y + 1, it.w - 2, it.h - 2, S(6), t.accent, Sd(1.5));
          double base = compact_ ? S(22) : S(26), is = base * fx.scale;
          double ix = it.x + S(10) + (S(26) - is) / 2 + fx.slide, iy = it.y + (it.h - is) / 2.0 - fx.lift;
          if (fx.glow > 0) p.circle(ix + is / 2, iy + is / 2, is * 0.72, t.accent.alpha(0.28 * fx.glow));
          p.image(icon(it.entry->icon, base, fx.scale), ix, iy, is, pressed ? 0.4 : 1.0);
          break;
        }
        case K_TILE: {
          bool pressed = dragging_ && press_ == Ref{it.kind, it.index, it.group};
          Fx fx = hover_of(it);
          double r = S(4);
          Color bg = hv ? tile_hover : tile_bg;
          if (pressed) bg = bg.alpha(0.35);
          p.fill_rounded(it.x, it.y, it.w, it.h, r, bg);
          // Reveal border on hover; accent ring for the keyboard selection.
          if (hv && !pressed) p.stroke_rounded(it.x + 0.5, it.y + 0.5, it.w - 1, it.h - 1, r, t.text.alpha(0.28));
          if (s && keyboard_)
            p.stroke_rounded(it.x + 1, it.y + 1, it.w - 2, it.h - 2, r, t.accent, Sd(2));
          double base = std::round(icon_size(it)), is = base * fx.scale;
          double ix = it.x + (it.w - is) / 2.0 + fx.slide;
          double iy = it.size == TileSize::Small ? it.y + (it.h - is) / 2.0
                                                 : it.y + (it.h - is) / 2.0 - it.h * 0.07;
          if (fx.glow > 0)
            p.circle(ix + is / 2, iy + is / 2 - fx.lift, is * 0.72, t.accent.alpha(0.28 * fx.glow));
          p.image(icon(it.entry->icon, base, fx.scale), ix, iy - fx.lift, is, pressed ? 0.35 : 1.0);
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
        case 5:
          glyph::power(p, gx, gy, g, t.text);
          // Apart from the places above it.
          p.line(S(12), it.y - S(5) + 0.5, rail_w_ - S(12), it.y - S(5) + 0.5, t.border);
          break;
        case 6: gallery_glyph(p, gx, gy, g, t.text); break;
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

  // Text on top of the cairo layer; the expanded rail covers the list.
  int rail_right = rail_w_;
  for (auto &it : items_)
    if (it.kind == K_RAIL) rail_right = std::max(rail_right, it.x + it.w);
  if (rail_open_) fl_push_clip(rail_right, 0, W - rail_right, H);
  for (auto &it : items_) {
    if (!it.visible || it.kind == K_RAIL) continue;
    if (!fl_not_clipped(it.x - S(14), it.y - S(14), it.w + S(28), it.h + S(28))) continue;
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
        int tx = it.x + S(46) + int(std::lround(hover_of(it).slide));
        draw_text(it.entry->name, tx, it.y, it.x + it.w - S(6) - tx, it.h, t.text);
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
    fl_pop_clip();
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
  if (qa_dump_enabled()) {
    // At most every 50 ms while frames follow each other (smooth scrolling,
    // effects); the last state is always written.
    if (tnow - qa_dumped_ >= 0.05) {
      qa_dumped_ = tnow;
      Fl::remove_timeout(qa_dump_later, this);
      qa_dump_menu();
    } else if (!Fl::has_timeout(qa_dump_later, this)) {
      Fl::add_timeout(0.06, qa_dump_later, this);
    }
  }
}

void AppMenu::qa_dump_later(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  self->qa_dumped_ = now();
  self->build_items();
  self->qa_dump_menu();
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
  static const char *const rail[] = {"menu",     "account", "documents", "pictures",
                                     "settings", "power",   "gallery"};
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
