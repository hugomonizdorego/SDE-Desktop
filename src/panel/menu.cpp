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
#include "spawn.h"
#include "suite.h"
#include "util.h"

using namespace sde;

namespace {

const char *kExtraPrefix = "sde-search:";
const char *kFilePrefix = "sde-file:";
// Favorites fit on one page of large tiles.
const size_t kMaxFavorites = 12;
// "Recently used" rows on the home view.
const size_t kMaxRecent = 6;

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

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

bool is_extra(const DesktopEntry *e) { return e && starts_with(e->id, kExtraPrefix); }
bool is_file(const DesktopEntry *e) { return e && starts_with(e->id, kFilePrefix); }
bool is_app(const DesktopEntry *e) { return e && !is_extra(e) && !is_file(e); }

DesktopEntry extra(const std::string &id, const std::string &name, const std::string &kind,
                   const std::string &icon, const std::string &exec,
                   const std::vector<std::string> &keywords) {
  DesktopEntry e;
  e.id = kExtraPrefix + id;
  e.type = "Application";
  e.name = name;
  e.generic_name = kind;
  e.icon = icon;
  e.exec = exec;
  e.keywords = keywords;
  return e;
}

// Older plain list of recently opened applications (read once to migrate).
std::string recent_path() { return state_home() + "/sde/menu-recent"; }
// Launch counts and times: "id<TAB>count<TAB>last" (see menu::parse_usage).
std::string usage_path() { return state_home() + "/sde/menu-usage"; }

long long epoch_now() { return (long long)time(nullptr); }

std::string tilde(const std::string &path) {
  std::string home = home_dir();
  if (path == home) return "~";
  if (starts_with(path, home + "/")) return "~" + path.substr(home.size());
  return path;
}

// Small folder outline in the style of the SDE status glyphs.
void folder_glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  double l = x + s * 0.14, r = x + s * 0.86, t = y + s * 0.24, b = y + s * 0.78;
  double tab = x + s * 0.44, rad = s * 0.06;
  cairo_new_path(cr);
  cairo_move_to(cr, l, b - rad);
  cairo_line_to(cr, l, t + rad);
  cairo_line_to(cr, l + rad, t);
  cairo_line_to(cr, tab - s * 0.06, t);
  cairo_line_to(cr, tab + s * 0.02, t + s * 0.09);
  cairo_line_to(cr, r - rad, t + s * 0.09);
  cairo_line_to(cr, r, t + s * 0.09 + rad);
  cairo_line_to(cr, r, b - rad);
  cairo_line_to(cr, r - rad, b);
  cairo_line_to(cr, l + rad, b);
  cairo_close_path(cr);
  cairo_move_to(cr, l, t + s * 0.2);
  cairo_line_to(cr, r, t + s * 0.2);
  p.set(c);
  cairo_set_line_width(cr, s * 0.08);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);
}

void search_glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_new_path(cr);
  cairo_arc(cr, x + s * 0.43, y + s * 0.43, s * 0.27, 0, 2 * M_PI);
  cairo_move_to(cr, x + s * 0.63, y + s * 0.63);
  cairo_line_to(cr, x + s * 0.86, y + s * 0.86);
  p.set(c);
  cairo_set_line_width(cr, s * 0.1);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_stroke(cr);
}

// Search field that hands navigation keys and right clicks to the menu.
class SearchInput : public Fl_Input {
 public:
  SearchInput(int x, int y, int w, int h) : Fl_Input(x, y, w, h) {}
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key();
      if (k == FL_Up || k == FL_Down || k == FL_Enter || k == FL_KP_Enter || k == FL_Escape ||
          k == FL_Page_Up || k == FL_Page_Down ||
          ((!*value()) && (k == FL_Left || k == FL_Right || k == FL_BackSpace)))
        return parent()->handle(FL_KEYBOARD);
    }
    if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) return parent()->handle(e);
    int r = Fl_Input::handle(e);
    if (e == FL_FOCUS || e == FL_UNFOCUS) parent()->redraw();
    return r;
  }
};

}  // namespace

AppMenu::AppMenu() : PanelPopup("SDE Menu") {
  search_ = new SearchInput(0, 0, 10, 10);
  search_->box(FL_FLAT_BOX);
  search_->when(FL_WHEN_CHANGED);
  search_->callback(search_changed, this);
  end();
  callback([](Fl_Widget *w, void *) { static_cast<AppMenu *>(w)->close(); });
}

AppMenu::~AppMenu() {
  Fl::remove_timeout(poll_applications, this);
  Fl::remove_timeout(animate, this);
  Fl::remove_timeout(blink, this);
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

  // Search also finds Settings pages and session actions.
  std::string settings = _("Settings");
  extras_ = {
      extra("appearance", _("Appearance"), settings, "preferences-desktop-theme",
            "sde-settings --page appearance",
            {"theme", "dark", "light", "aero", "glass", "liquid", "wallpaper", "accent", "color",
             "transparency", "blur", "font", "icons", "tema"}),
      extra("effects", _("Effects and Animations"), settings, "preferences-desktop-effects",
            "sde-settings --page effects",
            {"animation", "effect", "compositor", "shadow", "fade", "efek", "animasi"}),
      extra("panel", _("Panel"), settings, "preferences-desktop", "sde-settings --page panel",
            {"taskbar", "tray", "clock", "pinned", "autohide"}),
      extra("desktop", _("Desktop Icons"), settings, "user-desktop",
            "sde-settings --page desktop", {"icons", "home", "trash"}),
      extra("action-center", _("Action Center"), settings, "preferences-system",
            "sde-settings --page action-center", {"quick", "tiles", "media", "volume"}),
      extra("notifications", _("Notifications"), settings, "preferences-system-notifications",
            "sde-settings --page notifications", {"disturb", "dnd", "notifikasi"}),
      extra("display", _("Displays"), settings, "preferences-desktop-display",
            "sde-display-settings", {"monitor", "screen", "resolution", "scale", "layar"}),
      extra("input", _("Keyboard and Mouse"), settings, "preferences-desktop-keyboard",
            "sde-settings --page input", {"keyboard", "mouse", "touchpad", "layout"}),
      extra("network", _("Network and Wi-Fi"), settings, "network-wireless",
            "sde-panel --section network", {"wifi", "wlan", "lan", "internet", "jaringan"}),
      extra("bluetooth", _("Bluetooth"), settings, "bluetooth", "sde-panel --section bluetooth",
            {"bluetooth", "pair", "headphones"}),
      extra("sound", _("Sound"), settings, "multimedia-volume-control", "sde-panel --section sound",
            {"volume", "audio", "microphone", "speaker", "suara"}),
      extra("system", _("Power and Updates"), settings, "preferences-system-power",
            "sde-settings --page system", {"power", "update", "battery", "compositor"}),
      extra("about", _("About This Computer"), settings, "dialog-information",
            "sde-settings --page about", {"about", "version", "system", "info"}),
      extra("lock", _("Lock Screen"), _("Session"), "system-lock-screen", "sde-lock",
            {"lock", "kunci"}),
      extra("logout", _("Log Out, Restart or Shut Down"), _("Session"), "system-shutdown",
            "sde-logout",
            {"logout", "log out", "restart", "reboot", "shutdown", "power off", "suspend",
             "matikan"}),
  };
}

void AppMenu::load_favorites() {
  const std::string sentinel = "\x01";
  std::string v = Panel::instance->config().get("menu", "favorites", sentinel);
  favorites_default_ = v == sentinel;
  favorites_ = favorites_default_ ? menu::default_favorites(apps_) : menu::parse_id_list(v);
  if (favorites_.size() > kMaxFavorites) favorites_.resize(kMaxFavorites);
  std::string text;
  usage_.clear();
  if (read_file(usage_path(), text)) usage_ = menu::parse_usage(text);
  else if (read_file(recent_path(), text)) usage_ = menu::parse_usage(text, epoch_now());
  recent_ = menu::by_last_use(usage_, 12);
}

void AppMenu::save_favorites() {
  favorites_default_ = false;
  // An empty list is saved as ";" so that the defaults do not come back.
  std::string v = menu::join_id_list(favorites_);
  Panel::instance->set_setting("menu", "favorites", v.empty() ? ";" : v);
}

void AppMenu::save_usage() {
  make_dirs(state_home() + "/sde");
  write_file_atomic(usage_path(), menu::format_usage(usage_));
  recent_ = menu::by_last_use(usage_, 12);
}

void AppMenu::add_recent(const std::string &id) {
  menu::record_launch(usage_, id, epoch_now());
  save_usage();
}

// Refresh XDG application entries during package installs/removals without
// restarting the panel, preserving the view, query and selection.
void AppMenu::poll_applications(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  Fl::repeat_timeout(2, poll_applications, data);
  if (self->menu_busy_ || self->launching_) return;
  // Parsing every .desktop file every 2 s while the menu is open showed up
  // as periodic hitches; only reload when an applications directory changed.
  std::string stamp = applications_stamp();
  if (stamp == self->apps_stamp_) return;
  self->apps_stamp_ = stamp;
  auto fresh = load_applications("SDE");
  bool same = fresh.size() == self->apps_.size();
  for (size_t i = 0; same && i < fresh.size(); ++i) {
    const DesktopEntry &a = fresh[i], &b = self->apps_[i];
    same = a.id == b.id && a.name == b.name && a.icon == b.icon && a.exec == b.exec &&
           a.path == b.path && a.categories == b.categories;
  }
  if (same) return;
  std::string category = self->category_ >= 0 && self->category_ < int(self->categories_.size())
                             ? self->categories_[size_t(self->category_)]
                             : "";
  self->reload();
  if (self->favorites_default_) self->favorites_ = menu::default_favorites(self->apps_);
  auto cat = std::find(self->categories_.begin(), self->categories_.end(), category);
  if (cat != self->categories_.end()) {
    self->category_ = int(cat - self->categories_.begin());
  } else if (self->view_ == V_CATEGORY) {
    self->category_ = -1;
    self->view_ = V_HOME;
  }
  self->filter();
  self->redraw();
}

bool AppMenu::searching() const { return view_ == V_SEARCH; }

void AppMenu::filter() {
  std::map<std::string, const DesktopEntry *> by_id;
  for (auto &a : apps_) by_id[a.id] = &a;

  // Home: favorites, then the applications opened most often and lately.
  fav_entries_.clear();
  for (auto &id : favorites_)
    if (by_id.count(id) && fav_entries_.size() < kMaxFavorites) fav_entries_.push_back(by_id[id]);
  frecent_.clear();
  if (show_recent_)
    for (auto &id : menu::rank_frecent(usage_, epoch_now(), 64, favorites_))
      if (by_id.count(id) && frecent_.size() < kMaxRecent) frecent_.push_back(by_id[id]);

  category_apps_.clear();
  if (category_ >= 0 && category_ < int(categories_.size()))
    for (auto *a : sorted_)
      if (a->menu_category() == categories_[size_t(category_)]) category_apps_.push_back(a);

  results_.clear();
  files_.clear();
  std::string q = lower(trim(search_->value()));
  if (q.empty()) return;
  auto terms = split(q, ' ');
  std::vector<std::pair<int, const DesktopEntry *>> scored;
  for (auto &a : apps_) {
    int s = menu::match_score(a, terms);
    if (s) scored.push_back({s, &a});
  }
  // Settings pages and actions follow applications with the same score.
  for (auto &e : extras_) {
    int s = menu::match_score(e, terms);
    if (s) scored.push_back({s - 1, &e});
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](auto &a, auto &b) { return a.first > b.first; });
  for (auto &s : scored) results_.push_back(s.second);

  // Files and folders of the home folder, after everything else.
  if (!file_index_.built()) {
    std::vector<std::string> roots = {home_dir()};
    static const char *const dirs[][2] = {{"DESKTOP", "Desktop"},   {"DOCUMENTS", "Documents"},
                                          {"DOWNLOAD", "Downloads"}, {"MUSIC", "Music"},
                                          {"PICTURES", "Pictures"},  {"VIDEOS", "Videos"}};
    for (auto &d : dirs) {
      std::string p = user_dir(d[0]);
      if (p.empty() || p == home_dir()) p = home_dir() + "/" + d[1];
      roots.push_back(p);
    }
    file_index_.build(roots);
  }
  for (auto &path : file_index_.search(terms, 8)) {
    DesktopEntry e;
    bool dir = file_index_.is_dir(path);
    e.id = kFilePrefix + path;
    e.type = "Link";
    e.url = path;
    e.path = path;
    e.name = basename_of(path);
    e.generic_name = tilde(path.substr(0, path.rfind('/')));
    e.icon = menu::file_icon(path, dir);
    files_.push_back(e);
  }
  for (auto &f : files_) results_.push_back(&f);
}

void AppMenu::search_changed(Fl_Widget *, void *data) {
  auto *m = static_cast<AppMenu *>(data);
  std::string q = trim(m->search_->value());
  if (q == m->last_query_) return;
  m->last_query_ = q;
  m->caret_on_ = true;
  m->letters_open_ = false;
  if (!q.empty()) {
    if (m->view_ != V_SEARCH) {
      m->before_search_ = m->view_ == V_SEARCH_HOME ? V_HOME : m->view_;
      m->view_ = V_SEARCH;
    }
    m->result_scroll_ = 0;
    m->selected_ = {K_RESULT, 0};
  } else if (m->view_ == V_SEARCH) {
    m->view_ = Fl::focus() == m->search_ && m->before_search_ == V_HOME ? V_SEARCH_HOME
                                                                         : m->before_search_;
    m->selected_ = {};
  }
  m->filter();
  m->redraw();
}

// ---------------------------------------------------------------------------
// Opening and focus

void AppMenu::prepare() {
  theme_ = Panel::instance->theme();
  effects_ = effects_from_config(Panel::instance->config());
  const Config &cfg = Panel::instance->config();
  search_top_ = cfg.get("menu", "search_position", "top") != "bottom";
  show_desc_ = cfg.get_bool("menu", "show_descriptions", true);
  show_recent_ = cfg.get_bool("menu", "show_recent", true);
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  std::string sz = cfg.get("menu", "size", "medium");
  // Slim, balanced proportions; small screens get what is there.
  int bw = sz == "small" ? 520 : sz == "large" ? 660 : 580;
  int bh = sz == "small" ? 580 : sz == "large" ? 720 : 650;
  int mw2 = std::min(S(bw), mw - S(16));
  int mh2 = std::min(S(bh), mh - Panel::instance->h() - S(24));
  size(mw2, mh2);
  search_->color(theme_.surface_alt.fl());
  search_->textcolor(theme_.text.fl());
  search_->cursor_color(theme_.accent.fl());
  search_->selection_color(theme_.accent.fl());
  search_->textsize(theme_.font_size + 1);
  search_->textfont(FL_HELVETICA);
  search_->value("");
  last_query_.clear();
  view_ = before_search_ = V_HOME;
  category_ = -1;
  all_scroll_ = cat_scroll_ = result_scroll_ = home_scroll_ = 0;
  drag_area_ = K_NONE;
  letters_open_ = false;
  hover_ = selected_ = anim_ = {};
  keyboard_ = false;
  launching_ = false;
  caret_on_ = true;
  reload();
  load_favorites();
  file_index_.clear();
  filter();
  metrics();

  full_name_ = user_name();
  if (avatar_) cairo_surface_destroy(avatar_);
  avatar_ = nullptr;
  int ad = S(36);
  for (const std::string &f : {home_dir() + "/.face", home_dir() + "/.face.icon",
                               "/var/lib/AccountsService/icons/" + env("USER")}) {
    if (file_exists(f) && (avatar_ = load_image(f, ad * 2, ad * 2))) break;
  }

  Fl::remove_timeout(poll_applications, this);
  Fl::add_timeout(2, poll_applications, this);
  Fl::remove_timeout(blink, this);
  Fl::add_timeout(0.55, blink, this);
  // The popup grabs the pointer and keyboard right after it maps; give the
  // search field focus then, and again once the popup has claimed X focus.
  for (double delay : {0.0, 0.1, 0.3})
    Fl::add_timeout(delay, [](void *d) { static_cast<AppMenu *>(d)->give_focus(); }, this);
}

void AppMenu::closed() {
  Fl::remove_timeout(animate, this);
  Fl::remove_timeout(blink, this);
  launching_ = false;
  letters_open_ = false;
}

// FLTK 1.3 ignores Fl::focus() while a window holds the grab, which the
// popup always does: the search field then never showed its caret until the
// first key. Lift the grab for the moment the focus moves.
void AppMenu::give_focus() {
  if (!shown() || launching_) return;
  if (Fl::focus() != search_) {
    Fl_Window *grab = Fl::grab_;
    Fl::grab_ = nullptr;
    search_->take_focus();
    Fl::grab_ = grab;
  }
  caret_on_ = true;
  search_->cursor_color(theme_.accent.fl());
  redraw();
}

void AppMenu::blink(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  Fl::repeat_timeout(0.55, blink, data);
  if (Fl::focus() != self->search_) return;
  self->caret_on_ = !self->caret_on_;
  self->search_->cursor_color(self->caret_on_ ? self->theme_.accent.fl()
                                              : self->theme_.surface_alt.fl());
  self->search_->redraw();
}

void AppMenu::set_view(View v) {
  view_ = v;
  letters_open_ = false;
  selected_ = {};
  hover_ = {};
  redraw();
}

void AppMenu::open_category(int index) {
  if (index < 0 || index >= int(categories_.size())) return;
  if (searching()) {
    search_->value("");
    last_query_.clear();
  }
  category_ = index;
  cat_scroll_ = 0;
  filter();
  set_view(V_CATEGORY);
}

void AppMenu::go_back() {
  category_ = -1;
  filter();
  set_view(V_HOME);
}

// ---------------------------------------------------------------------------
// Layout

int AppMenu::Area::bar_x() const { return x + w - S(7); }

// Category chips that always fit the width: labeled chips in one or two
// justified rows when they fit, otherwise equal icon cells (one or two rows)
// whose names show on hover. Sets cats_h_ and chips_.
void AppMenu::layout_chips() {
  chips_.clear();
  int avail = w() - 2 * pad_, gap = S(6), row_gap = S(6);
  int chip_h = compact_ ? S(28) : S(30);
  size_t n = categories_.size();
  if (!n) {
    cats_h_ = 0;
    return;
  }
  text_font(theme_.font_size - 1, true);  // the selected chip is bold
  std::vector<int> natural;
  int total = 0;
  for (auto &c : categories_) {
    natural.push_back(S(40) + text_width(category_label(c)));
    total += natural.back();
  }
  // Splits the chips into |rows| rows of about equal width; empty when one
  // of them does not fit.
  auto split_rows = [&](int rows) {
    std::vector<std::vector<size_t>> out(1);
    int target = (total + gap * int(n)) / rows, row_w = 0;
    for (size_t i = 0; i < n; ++i) {
      int add = natural[i] + (out.back().empty() ? 0 : gap);
      if (!out.back().empty() && int(out.size()) < rows &&
          (row_w + add > avail || row_w + add / 2 > target)) {
        out.emplace_back();
        row_w = 0;
        add = natural[i];
      }
      out.back().push_back(i);
      row_w += add;
      if (row_w > avail) return std::vector<std::vector<size_t>>();
    }
    return out;
  };
  std::vector<std::vector<size_t>> rows;
  // Rows of labeled chips: one on short screens, up to three on tall ones.
  int max_rows = compact_ ? 1 : h() >= S(620) ? 3 : 2;
  for (int r = 1; r <= max_rows && rows.empty(); ++r) rows = split_rows(r);
  chips_icons_ = rows.empty();
  if (chips_icons_) {
    // Icon cells: as many per row as fit, rows balanced.
    int per_row = std::max(1, std::min(int(n), (avail + gap) / (S(34) + gap)));
    int nrows = (int(n) + per_row - 1) / per_row;
    per_row = (int(n) + nrows - 1) / nrows;
    rows.assign(size_t(nrows), {});
    for (size_t i = 0; i < n; ++i) rows[i / size_t(per_row)].push_back(i);
  }
  int y = cats_y_;
  int cols = int(rows[0].size());
  for (auto &row : rows) {
    int count = int(row.size()), x = pad_;
    int sum = 0;
    for (size_t i : row) sum += natural[i];
    // Labeled rows are justified to the full width; icon cells share it.
    int extra = chips_icons_ ? 0 : avail - sum - gap * (count - 1);
    for (int k = 0; k < count; ++k) {
      size_t i = row[size_t(k)];
      Item c;
      c.kind = K_CATEGORY;
      c.index = int(i);
      c.label = category_label(categories_[i]);
      if (chips_icons_) c.w = (avail - gap * (cols - 1)) / cols;
      else c.w = natural[i] + extra / count + (k < extra % count ? 1 : 0);
      c.h = chip_h;
      c.x = x;
      c.y = y;
      x += c.w + gap;
      chips_.push_back(c);
    }
    y += chip_h + row_gap;
  }
  cats_h_ = y - row_gap - cats_y_;
}

void AppMenu::metrics() {
  int W = w(), H = h();
  compact_ = H < S(560);
  pad_ = S(12);
  header_h_ = compact_ ? S(46) : S(56);
  search_h_ = compact_ ? S(36) : S(40);
  int gap = compact_ ? S(6) : S(10);
  if (search_top_) {
    search_y_ = header_h_;
    cats_y_ = search_y_ + search_h_ + gap;
    layout_chips();
    body_top_ = cats_y_ + cats_h_ + gap;
    body_bottom_ = H - pad_;
  } else {
    cats_y_ = header_h_;
    layout_chips();
    body_top_ = cats_y_ + cats_h_ + gap;
    search_y_ = H - pad_ - search_h_;
    body_bottom_ = search_y_ - gap;
  }
  split_x_ = pad_ + int((W - 2 * pad_) * 0.58);
  int sx = pad_ + S(40), sy = search_y_ + S(8), sw = W - 2 * pad_ - S(52), sh = search_h_ - S(16);
  if (search_->x() != sx || search_->y() != sy || search_->w() != sw || search_->h() != sh)
    search_->resize(sx, sy, sw, sh);
}

void AppMenu::build_items() {
  items_.clear();
  areas_.clear();
  metrics();
  int W = w();
  Item it;

  // Header: Files (power and Settings are in the Action Center).
  int b = S(36);
  it.kind = K_HEADER_BUTTON;
  it.index = 0;
  it.w = it.h = b;
  it.x = W - pad_ - b;
  it.y = (header_h_ - b) / 2;
  items_.push_back(it);

  it = Item();
  it.kind = K_SEARCH;
  it.x = pad_;
  it.y = search_y_;
  it.w = W - 2 * pad_;
  it.h = search_h_;
  items_.push_back(it);

  for (auto &c : chips_) items_.push_back(c);

  switch (view_) {
    case V_HOME: layout_home(body_top_, body_bottom_); break;
    case V_CATEGORY: layout_category(body_top_, body_bottom_); break;
    case V_SEARCH: layout_search(body_top_, body_bottom_); break;
    case V_SEARCH_HOME: layout_search_home(body_top_, body_bottom_); break;
  }
}

void AppMenu::layout_home(int top, int bottom) {
  int W = w();
  Item it;
  // Left pane: favorites (one page of up to 12 tiles), then recently used.
  int lx = pad_, lw = split_x_ - S(12) - pad_;
  int title_h = compact_ ? S(24) : S(28);
  std::vector<Item> left;
  int y = 0;
  it.kind = K_NONE;
  it.index = 0;  // pane title
  it.label = _("Favorites");
  it.x = lx + S(4);
  it.y = y;
  it.w = lw - S(8);
  it.h = title_h;
  left.push_back(it);
  it.index = 3;  // count, right aligned
  it.label = format("%d/%d", int(fav_entries_.size()), int(kMaxFavorites));
  left.push_back(it);
  y += title_h;

  int cols = std::max(3, std::min(6, lw / S(74)));
  int tile_w = lw / cols, tile_h = compact_ ? S(58) : S(78);
  if (fav_entries_.empty()) {
    Item h;
    h.kind = K_NONE;
    h.index = 2;  // hint
    h.label = _("Right-click an app and choose \"Add to Favorites\".");
    h.x = lx + S(6);
    h.y = y;
    h.w = lw - S(12);
    h.h = S(40);
    left.push_back(h);
    y += S(44);
  }
  for (size_t i = 0; i < fav_entries_.size(); ++i) {
    Item t;
    t.kind = K_TILE;
    t.index = int(i);
    t.entry = fav_entries_[i];
    t.x = lx + int(i % size_t(cols)) * tile_w;
    t.y = y + int(i / size_t(cols)) * tile_h;
    t.w = tile_w;
    t.h = tile_h;
    left.push_back(t);
  }
  y += int((fav_entries_.size() + size_t(cols) - 1) / size_t(cols)) * tile_h;

  if (show_recent_) {
    y += compact_ ? S(4) : S(10);
    it = Item();
    it.kind = K_NONE;
    it.index = 0;
    it.label = _("Recently Used");
    it.x = lx + S(4);
    it.y = y;
    it.w = lw - S(8);
    it.h = title_h;
    left.push_back(it);
    y += title_h;
    if (frecent_.empty()) {
      Item h;
      h.kind = K_NONE;
      h.index = 2;
      h.label = _("Apps you open often show up here.");
      h.x = lx + S(6);
      h.y = y;
      h.w = lw - S(12);
      h.h = S(32);
      left.push_back(h);
      y += S(32);
    }
    int rcols = lw >= S(300) ? 2 : 1, row_h = compact_ ? S(36) : S(42);
    int rw = lw / rcols;
    for (size_t i = 0; i < frecent_.size(); ++i) {
      Item r;
      r.kind = K_RECENT;
      r.index = int(i);
      r.entry = frecent_[i];
      r.x = lx + int(i % size_t(rcols)) * rw;
      r.y = y + int(i / size_t(rcols)) * row_h;
      r.w = rw - S(4);
      r.h = row_h;
      left.push_back(r);
    }
    y += int((frecent_.size() + size_t(rcols) - 1) / size_t(rcols)) * row_h;
  }
  // The left pane scrolls only when the screen is too small for it.
  {
    Area a;
    a.kind = K_TILE;
    a.x = lx;
    a.y = top;
    a.w = split_x_ - S(4) - lx;
    a.h = bottom - top;
    a.content = y;
    a.scroll = &home_scroll_;
    home_scroll_ = std::max(0, std::min(home_scroll_, a.content - a.h));
    for (auto &t : left) {
      t.y += top - home_scroll_;
      t.cx = a.x;
      t.cy = a.y;
      t.cw = a.w;
      t.ch = a.h;
      t.visible = t.y + t.h > a.y && t.y < a.y + a.h;
      items_.push_back(t);
    }
    areas_.push_back(a);
  }

  // Right pane: all programs, A-Z.
  int rx = split_x_, rw = W - pad_ - rx;
  it = Item();
  it.kind = K_NONE;
  it.index = 0;
  it.label = _("All Programs");
  it.x = rx + S(12);
  it.y = top;
  it.w = rw - S(24);
  it.h = title_h;
  items_.push_back(it);
  it.index = 3;  // count, right aligned
  it.label = format("%d", int(sorted_.size()));
  items_.push_back(it);
  all_top_ = top + title_h + S(2);
  all_bottom_ = bottom - S(6);
  int area_h = all_bottom_ - all_top_;
  if (letters_open_) {
    // Jump grid: every letter, the ones with applications enabled.
    std::vector<std::string> letters = {"#"};
    for (char c = 'A'; c <= 'Z'; ++c) letters.push_back(std::string(1, c));
    std::vector<std::string> present;
    for (auto *a : sorted_) {
      std::string l = menu::index_letter(a->name);
      if (std::find(present.begin(), present.end(), l) == present.end()) present.push_back(l);
    }
    for (auto &l : present)
      if (std::find(letters.begin(), letters.end(), l) == letters.end()) letters.push_back(l);
    int gc = 5, cell = std::min((rw - S(16)) / gc, S(46));
    int grows = (int(letters.size()) + gc - 1) / gc;
    cell = std::min(cell, std::max(S(20), area_h / std::max(1, grows)));
    int gx = rx + (rw - cell * gc) / 2, gy = all_top_ + std::max(0, (area_h - cell * grows) / 2);
    for (size_t i = 0; i < letters.size(); ++i) {
      Item l;
      bool has = std::find(present.begin(), present.end(), letters[i]) != present.end();
      l.kind = has ? K_LETTER_JUMP : K_NONE;
      l.index = has ? int(i) : 4;  // 4: disabled letter
      l.label = letters[i];
      l.x = gx + int(i % size_t(gc)) * cell + S(2);
      l.y = gy + int(i / size_t(gc)) * cell + S(2);
      l.w = l.h = cell - S(4);
      items_.push_back(l);
    }
    return;
  }
  int row_h = compact_ ? S(32) : S(36), letter_h = compact_ ? S(26) : S(28), ly = 0, idx = 0;
  std::string last;
  std::vector<Item> rows;
  for (size_t i = 0; i < sorted_.size(); ++i) {
    std::string l = menu::index_letter(sorted_[i]->name);
    if (l != last) {
      Item h;
      h.kind = K_ALL_LETTER;
      h.index = int(rows.size());
      h.label = l;
      h.x = rx + S(6);
      h.y = ly;
      h.w = rw - S(20);
      h.h = letter_h;
      rows.push_back(h);
      ly += letter_h;
      last = l;
    }
    Item r;
    r.kind = K_ALL_ROW;
    r.index = idx++;
    r.entry = sorted_[i];
    r.x = rx + S(6);
    r.y = ly;
    r.w = rw - S(20);
    r.h = row_h;
    rows.push_back(r);
    ly += row_h;
  }
  Area a;
  a.kind = K_ALL_ROW;
  a.x = rx;
  a.y = all_top_;
  a.w = rw;
  a.h = area_h;
  a.content = ly;
  a.scroll = &all_scroll_;
  areas_.push_back(a);
  all_scroll_ = std::max(0, std::min(all_scroll_, ly - area_h));
  for (auto &r : rows) {
    r.y += all_top_ - all_scroll_;
    r.cx = rx;
    r.cy = all_top_;
    r.cw = rw;
    r.ch = area_h;
    r.visible = r.y + r.h > all_top_ && r.y < all_bottom_;
    items_.push_back(r);
  }
}

void AppMenu::layout_category(int top, int bottom) {
  int W = w();
  int title_h = S(40);
  Item b;
  b.kind = K_BACK;
  b.x = pad_;
  b.y = top + S(2);
  b.w = b.h = S(34);
  items_.push_back(b);
  Item t;
  t.kind = K_NONE;
  t.index = 5;  // view title with the category icon
  t.label = category_ >= 0 ? category_label(categories_[size_t(category_)]) : "";
  t.x = pad_ + S(44);
  t.y = top;
  t.w = W - 2 * pad_ - S(44);
  t.h = title_h;
  items_.push_back(t);
  t.index = 3;
  t.label = category_apps_.size() == 1 ? std::string(_("1 app"))
                                       : format(_("%d apps"), int(category_apps_.size()));
  items_.push_back(t);

  view_top_ = top + title_h + S(6);
  view_bottom_ = bottom;
  int area_w = W - 2 * pad_, area_h = view_bottom_ - view_top_;
  int cols = std::max(3, area_w / S(100));
  int tile_w = area_w / cols, tile_h = compact_ ? S(84) : S(96);
  int rows = (int(category_apps_.size()) + cols - 1) / cols;
  Area a;
  a.kind = K_CAT_TILE;
  a.x = pad_;
  a.y = view_top_;
  a.w = area_w + S(8);
  a.h = area_h;
  a.content = rows * tile_h;
  a.scroll = &cat_scroll_;
  areas_.push_back(a);
  cat_scroll_ = std::max(0, std::min(cat_scroll_, a.content - area_h));
  for (size_t i = 0; i < category_apps_.size(); ++i) {
    Item c;
    c.kind = K_CAT_TILE;
    c.index = int(i);
    c.entry = category_apps_[i];
    c.x = pad_ + int(i % size_t(cols)) * tile_w;
    c.y = view_top_ + int(i / size_t(cols)) * tile_h - cat_scroll_;
    c.w = tile_w;
    c.h = tile_h;
    c.cx = pad_;
    c.cy = view_top_;
    c.cw = area_w;
    c.ch = area_h;
    c.visible = c.y + c.h > view_top_ && c.y < view_bottom_;
    items_.push_back(c);
  }
}

void AppMenu::layout_search(int top, int bottom) {
  int W = w();
  int title_h = S(32);
  Item t;
  t.kind = K_NONE;
  t.index = 0;
  t.label = results_.empty() ? _("No results") : _("Best matches");
  t.x = pad_ + S(4);
  t.y = top;
  t.w = W - 2 * pad_ - S(8);
  t.h = title_h;
  items_.push_back(t);
  t.index = 3;
  t.label = results_.size() == 1 ? std::string(_("1 result"))
                                 : format(_("%d results"), int(results_.size()));
  items_.push_back(t);
  view_top_ = top + title_h + S(2);
  view_bottom_ = bottom;
  int area_h = view_bottom_ - view_top_;
  if (results_.empty()) {
    t.index = 2;
    t.group = 1;
    t.label = _("Try another word, or check the spelling.");
    t.y = view_top_ + S(20);
    t.h = S(40);
    items_.push_back(t);
    return;
  }
  int row_h = show_desc_ ? S(50) : S(40);
  Area a;
  a.kind = K_RESULT;
  a.x = pad_;
  a.y = view_top_;
  a.w = W - 2 * pad_;
  a.h = area_h;
  a.content = int(results_.size()) * row_h;
  a.scroll = &result_scroll_;
  areas_.push_back(a);
  result_scroll_ = std::max(0, std::min(result_scroll_, a.content - area_h));
  for (size_t i = 0; i < results_.size(); ++i) {
    Item r;
    r.kind = K_RESULT;
    r.index = int(i);
    r.entry = results_[i];
    r.x = pad_;
    r.y = view_top_ + int(i) * row_h - result_scroll_;
    r.w = W - 2 * pad_ - S(12);
    r.h = row_h;
    r.cx = pad_;
    r.cy = view_top_;
    r.cw = W - 2 * pad_;
    r.ch = area_h;
    r.visible = r.y + r.h > view_top_ && r.y < view_bottom_;
    items_.push_back(r);
  }
}

// The empty, focused search field: recent applications, quick settings and
// what search can find.
void AppMenu::layout_search_home(int top, int bottom) {
  int W = w(), area_w = W - 2 * pad_;
  int y = top;
  Item t;
  t.kind = K_NONE;
  t.index = 1;
  t.x = pad_ + S(4);
  t.w = area_w - S(8);
  t.h = S(26);
  std::vector<const DesktopEntry *> recent;
  std::map<std::string, const DesktopEntry *> by_id;
  for (auto &a : apps_) by_id[a.id] = &a;
  for (auto &id : recent_)
    if (by_id.count(id)) recent.push_back(by_id[id]);
  t.label = recent.empty() ? _("Suggested") : _("Recent");
  if (recent.empty())
    for (auto &id : favorites_)
      if (by_id.count(id)) recent.push_back(by_id[id]);
  t.y = y;
  items_.push_back(t);
  y += t.h;
  int cols = std::max(3, area_w / S(96));
  int tile_w = area_w / cols, tile_h = S(88);
  for (size_t i = 0; i < recent.size() && int(i) < cols; ++i) {
    Item c;
    c.kind = K_TILE;
    c.index = int(i);
    c.entry = recent[i];
    c.x = pad_ + int(i) * tile_w;
    c.y = y;
    c.w = tile_w;
    c.h = tile_h;
    items_.push_back(c);
  }
  y += tile_h + S(10);
  t.label = _("Quick Settings");
  t.y = y;
  items_.push_back(t);
  y += t.h + S(2);
  int x = pad_, chip_h = S(34);
  text_font(theme_.font_size);
  static const char *const quick[] = {"appearance", "display", "network", "bluetooth",
                                      "sound", "notifications", "input", "system"};
  for (size_t i = 0; i < extras_.size(); ++i) {
    bool wanted = false;
    for (const char *q : quick) wanted = wanted || extras_[i].id == std::string(kExtraPrefix) + q;
    if (!wanted) continue;
    Item c;
    c.kind = K_CHIP;
    c.index = int(i);
    c.entry = &extras_[i];
    c.w = S(48) + text_width(extras_[i].name);
    c.h = chip_h;
    if (x + c.w > pad_ + area_w) {
      x = pad_;
      y += chip_h + S(8);
    }
    if (y + chip_h > bottom - S(40)) break;
    c.x = x;
    c.y = y;
    items_.push_back(c);
    x += c.w + S(8);
  }
  y += chip_h + S(16);
  t.index = 2;
  t.group = 0;
  t.label = _("Search finds applications, settings pages, session actions and files in your "
              "home folder.");
  t.y = std::min(y, bottom - S(30));
  t.h = S(28);
  items_.push_back(t);
}

const AppMenu::Item *AppMenu::find(const Ref &r) const {
  if (r.kind == K_NONE) return nullptr;
  for (auto &it : items_)
    if (it.kind == r.kind && it.index == r.index) return &it;
  return nullptr;
}

const AppMenu::Item *AppMenu::item_at(int x, int y) const {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it)
    if (it->kind != K_NONE && it->contains(x, y)) return &*it;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Actions

void AppMenu::activate(Item it) {
  switch (it.kind) {
    case K_HEADER_BUTTON:
      close();
      if (!find_program("liria").empty()) spawn_detached({"liria", "--no-desktop", home_dir()});
      else open_uri(home_dir());
      return;
    case K_SEARCH:
      give_focus();
      if (!*search_->value() && view_ != V_SEARCH_HOME) {
        before_search_ = view_;
        set_view(V_SEARCH_HOME);
      }
      return;
    case K_CATEGORY: open_category(it.index); return;
    case K_ALL_LETTER:
      letters_open_ = true;
      selected_ = {};
      break;
    case K_LETTER_JUMP: {
      letters_open_ = false;
      build_items();
      for (auto &h : items_)
        if (h.kind == K_ALL_LETTER && h.label == it.label) {
          all_scroll_ += h.y - all_top_;
          if (keyboard_) selected_ = {K_ALL_LETTER, h.index};
          break;
        }
      break;
    }
    case K_BACK: go_back(); return;
    case K_TILE:
    case K_RECENT:
    case K_ALL_ROW:
    case K_CAT_TILE:
    case K_RESULT:
    case K_CHIP: launch(it); return;
    default: return;
  }
  redraw();
}

void AppMenu::launch(Item it) {
  if (!it.entry) return;
  DesktopEntry e = *it.entry;
  if (is_app(&e)) add_recent(e.id);
  // The program starts at once; the menu closes when the effect has played.
  launch_entry(e);
  effects_ = effects_from_config(Panel::instance->config());
  if (effects_.launch == "none" || !effects_.on) {
    close();
    return;
  }
  bool big = it.kind == K_TILE || it.kind == K_CAT_TILE;
  launch_size_ = big ? S(48) : it.kind == K_RESULT ? S(32) : S(24);
  launch_cx_ = big ? it.x + it.w / 2.0 : it.x + S(10) + launch_size_ / 2;
  launch_cy_ = big ? it.y + S(8) + launch_size_ / 2 : it.y + it.h / 2.0;
  launch_icon_ = e.icon;
  launching_ = true;
  launch_effect_ = effects_.launch;
  launch_start_ = now();
  Fl::remove_timeout(animate, this);
  Fl::add_timeout(1.0 / 60, animate, this);
}

void AppMenu::item_menu(Item it, int px, int py) {
  if (!it.entry) {
    layout_menu();
    return;
  }
  DesktopEntry e = *it.entry;
  bool app = is_app(&e), file = is_file(&e);
  bool fav = std::find(favorites_.begin(), favorites_.end(), e.id) != favorites_.end();
  bool recent = std::find_if(usage_.begin(), usage_.end(), [&](const menu::Usage &u) {
                  return u.id == e.id;
                }) != usage_.end();
  bool pinned = app && Panel::instance->is_pinned(e.id);
  // Favorites hold 12 applications. When they are full, adding one replaces
  // a favorite that is no longer installed, or else the oldest one; the menu
  // says which.
  bool full = !fav && favorites_.size() >= kMaxFavorites;
  std::string replaced;
  if (full) {
    std::map<std::string, const DesktopEntry *> by_id;
    for (auto &a : apps_) by_id[a.id] = &a;
    auto gone = std::find_if(favorites_.begin(), favorites_.end(),
                             [&](const std::string &id) { return !by_id.count(id); });
    replaced = gone != favorites_.end() ? *gone : favorites_.front();
  }
  enum { OPEN = 1, FAV, FAV_LEFT, FAV_RIGHT, PIN, DESKTOP, FORGET, FOLDER, ARRANGE, NOTE };
  std::vector<Fl_Menu_Item> items;
  std::vector<std::string> texts;  // labels made here; kept alive while the menu runs
  texts.reserve(4);
  auto add = [&](const char *text, long code, int flags = 0) {
    Fl_Menu_Item m = {text, 0, nullptr, reinterpret_cast<void *>(code), flags, 0, 0, 0, 0};
    items.push_back(m);
  };
  add(_("Open"), OPEN, FL_MENU_DIVIDER);
  if (file) add(_("Open Containing Folder"), FOLDER, FL_MENU_DIVIDER);
  if (app) {
    if (full) {
      std::string name = replaced;
      for (auto &a : apps_)
        if (a.id == replaced) name = a.name;
      texts.push_back(format(_("Favorites are full (%d of %d)"), int(kMaxFavorites),
                             int(kMaxFavorites)));
      add(texts.back().c_str(), NOTE, FL_MENU_INACTIVE);
      texts.push_back(format(_("Add to Favorites, Replacing \"%s\""), name.c_str()));
      add(texts.back().c_str(), FAV);
    } else {
      add(fav ? _("Remove from Favorites") : _("Add to Favorites"), FAV);
    }
    if (fav && it.kind == K_TILE && view_ == V_HOME) {
      add(_("Move Left"), FAV_LEFT);
      add(_("Move Right"), FAV_RIGHT);
    }
    add(pinned ? _("Unpin from Panel") : _("Pin to Panel"), PIN);
    add(_("Add to Desktop"), DESKTOP, recent ? 0 : FL_MENU_DIVIDER);
    if (recent) add(_("Remove from Recent"), FORGET, FL_MENU_DIVIDER);
  }
  add(_("Arrange Menu..."), ARRANGE);
  add(nullptr, 0);
  menu_busy_ = true;
  const Fl_Menu_Item *m = items.data()->popup(px, py);
  menu_busy_ = false;
  if (!m) return;
  long code = reinterpret_cast<long>(m->user_data());
  auto pos = std::find(favorites_.begin(), favorites_.end(), e.id);
  switch (code) {
    case OPEN: launch(it); return;
    case FOLDER:
      close();
      open_uri(e.path.substr(0, e.path.rfind('/')));
      return;
    case FAV:
      if (fav) {
        favorites_.erase(pos);
      } else {
        if (full) favorites_.erase(std::find(favorites_.begin(), favorites_.end(), replaced));
        favorites_.push_back(e.id);
      }
      save_favorites();
      break;
    case FAV_LEFT:
    case FAV_RIGHT: {
      if (pos == favorites_.end()) break;
      size_t i = size_t(pos - favorites_.begin());
      size_t j = code == FAV_LEFT ? (i ? i - 1 : i) : std::min(favorites_.size() - 1, i + 1);
      std::swap(favorites_[i], favorites_[j]);
      save_favorites();
      break;
    }
    case PIN:
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
      save_usage();
      break;
    case ARRANGE: layout_menu(); return;
  }
  filter();
  redraw();
}

// Right-click on the menu: arrange it. Every choice is saved in [menu]. Older
// keys (categories, view) are no longer used and are left as they are.
void AppMenu::layout_menu() {
  const Config &cfg = Panel::instance->config();
  auto radio = [](bool on) { return FL_MENU_RADIO | (on ? FL_MENU_VALUE : 0); };
  auto toggle = [](bool on) { return FL_MENU_TOGGLE | (on ? FL_MENU_VALUE : 0); };
  std::string size = cfg.get("menu", "size", "medium");
  Fl_Menu_Item items[] = {
      {_("Search Bar"), 0, nullptr, nullptr, FL_SUBMENU, 0, 0, 0, 0},
      {_("At the Top"), 0, nullptr, nullptr, radio(search_top_), 0, 0, 0, 0},
      {_("At the Bottom"), 0, nullptr, nullptr, radio(!search_top_), 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Menu Size"), 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Small"), 0, nullptr, nullptr, radio(size == "small"), 0, 0, 0, 0},
      {_("Medium"), 0, nullptr, nullptr, radio(size != "small" && size != "large"), 0, 0, 0, 0},
      {_("Large"), 0, nullptr, nullptr, radio(size == "large"), 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Show Recently Used Apps"), 0, nullptr, nullptr, toggle(show_recent_), 0, 0, 0, 0},
      {_("Show Descriptions in Results"), 0, nullptr, nullptr, toggle(show_desc_) | FL_MENU_DIVIDER,
       0, 0, 0, 0},
      {_("Restore Default Favorites"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Effects and Animations..."), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Reset Menu Layout"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  menu_busy_ = true;
  const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y());
  menu_busy_ = false;
  if (!m) return;
  Panel *p = Panel::instance;
  switch (int(m - items)) {
    case 1: p->set_setting("menu", "search_position", "top"); break;
    case 2: p->set_setting("menu", "search_position", "bottom"); break;
    case 5: p->set_setting("menu", "size", "small"); break;
    case 6: p->set_setting("menu", "size", "medium"); break;
    case 7: p->set_setting("menu", "size", "large"); break;
    case 9: p->set_setting("menu", "show_recent", show_recent_ ? "false" : "true"); break;
    case 10: p->set_setting("menu", "show_descriptions", show_desc_ ? "false" : "true"); break;
    case 11: p->set_setting("menu", "favorites", ""); break;
    case 12:
      close();
      spawn_detached({"sde-settings", "--page", "effects"});
      return;
    case 13:
      for (const char *key : {"search_position", "categories", "view", "show_descriptions",
                              "show_recent", "size"})
        p->set_setting("menu", key, "");
      break;
    default: return;
  }
  // Apply at once: the size may change, so place the menu again.
  prepare();
  reposition();
  redraw();
}

// ---------------------------------------------------------------------------
// Keyboard and scrolling

void AppMenu::ensure_visible(const Item &it) {
  auto fit = [&](int &scroll, int top, int bottom) {
    if (it.y < top) scroll -= top - it.y;
    else if (it.y + it.h > bottom) scroll += it.y + it.h - bottom;
    scroll = std::max(0, scroll);
  };
  switch (it.kind) {
    case K_ALL_ROW:
    case K_ALL_LETTER: fit(all_scroll_, all_top_, all_bottom_); break;
    case K_CAT_TILE: fit(cat_scroll_, view_top_, view_bottom_); break;
    case K_RESULT: fit(result_scroll_, view_top_, view_bottom_); break;
    case K_TILE:
    case K_RECENT:
      if (view_ == V_HOME && it.ch < (1 << 19)) fit(home_scroll_, it.cy, it.cy + it.ch);
      break;
    default: break;
  }
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
  // The item under the pointer changed: hover follows the content.
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
// panes (arrow keys) or by one screen (Page Up/Down).
void AppMenu::move_selection(int key) {
  auto selectable = [](Kind k) {
    return k == K_HEADER_BUTTON || k == K_CATEGORY || k == K_TILE || k == K_RECENT || k == K_ALL_ROW ||
           k == K_LETTER_JUMP || k == K_BACK || k == K_CAT_TILE ||
           k == K_RESULT || k == K_CHIP;
  };
  build_items();
  const Item *cur = find(selected_);
  keyboard_ = true;
  if (key == FL_Page_Down || key == FL_Page_Up) {
    // Page Up/Down scroll the view's list by a page; the selection keeps its
    // place on screen (or starts at the top of the list).
    int dir = key == FL_Page_Down ? 1 : -1;
    Kind list = view_ == V_HOME ? (letters_open_ ? K_NONE : K_ALL_ROW)
                : view_ == V_CATEGORY ? K_CAT_TILE : view_ == V_SEARCH ? K_RESULT : K_NONE;
    if (cur && cur->kind == K_TILE && view_ == V_HOME) list = K_TILE;
    const Area *a = area(list);
    if (a) {
      bool keep = cur && cur->kind == (list == K_TILE ? K_TILE : list) && cur->visible;
      int ref_x = keep ? cur->x : a->x, ref_y = keep ? cur->y : a->y;
      int before = *a->scroll;
      if (a->scrolls())
        *a->scroll = std::max(0, std::min(a->content - a->h, *a->scroll + dir * (a->h - S(36))));
      bool moved = *a->scroll != before;
      build_items();
      Kind kind = list == K_TILE ? K_TILE : list;
      const Item *best = nullptr;
      double best_d = 1e18;
      for (auto &it : items_) {
        if (it.kind != kind || !it.visible) continue;
        double d = std::fabs(double(it.y - ref_y)) + std::fabs(double(it.x - ref_x));
        if (d < best_d) best_d = d, best = &it;
      }
      // Already at either end of the list: the first or last item.
      if (!moved && keep) {
        for (auto &it : items_)
          if (it.kind == kind) {
            if (dir < 0) {
              best = &it;
              break;
            }
            best = &it;
          }
      }
      if (best) {
        selected_ = {best->kind, best->index};
        ensure_visible(*best);
      }
      redraw();
      return;
    }
    return;
  }
  if (!cur) {
    // The first key shows the selection on the first item of the content.
    const Item *first = nullptr;
    for (auto &it : items_)
      if ((it.entry || it.kind == K_LETTER_JUMP) && it.visible) {
        first = &it;
        break;
      }
    if (first) selected_ = {first->kind, first->index};
    redraw();
    return;
  }
  double cx = cur->x + cur->w / 2.0, cy = cur->y + cur->h / 2.0;
  const Item *best = nullptr;
  double best_score = 1e18;
  for (auto &it : items_) {
    if (&it == cur || !selectable(it.kind)) continue;
    double ix = it.x + it.w / 2.0, iy = it.y + it.h / 2.0;
    double along, across;
    if (key == FL_Down) along = iy - cy, across = ix - cx;
    else if (key == FL_Up) along = cy - iy, across = ix - cx;
    else if (key == FL_Right) along = ix - cx, across = iy - cy;
    else along = cx - ix, across = iy - cy;
    if (along < 2) continue;
    // Within a row or column the neighbor wins; otherwise the closest item.
    bool overlaps = key == FL_Down || key == FL_Up
                        ? it.x < cur->x + cur->w && it.x + it.w > cur->x
                        : it.y < cur->y + cur->h && it.y + it.h > cur->y;
    double score = along + std::fabs(across) * (overlaps ? 0.5 : 3.0);
    if (score < best_score) best_score = score, best = &it;
  }
  if (best) {
    selected_ = {best->kind, best->index};
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
    // Glow keeps pulsing and bounce plays for a second while hovered.
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
      if (ex < 0 || ey < 0 || ex >= w() || ey >= h()) return PanelPopup::handle(event);
      build_items();
      const Item *hit = item_at(ex, ey);
      Item it = hit ? *hit : Item();
      if (Fl::event_button() == FL_LEFT_MOUSE) {
        // Scrollbars: drag the thumb, or click the track to jump there.
        for (auto &a : areas_) {
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
            // Center the thumb on the click, then keep dragging from there.
            drag_y_ = int(pos + len / 2);
            drag_scrollbar(ey);
            drag_y_ = ey;
            drag_scroll_ = *a.scroll;
          }
          redraw();
          return 1;
        }
      }
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (it.entry) {
          selected_ = {it.kind, it.index};
          redraw();
        }
        item_menu(it, ex, ey);
        return 1;
      }
      if (it.kind == K_SEARCH) {
        activate(it);
        // Let the field place the caret where it was clicked.
        PanelPopup::handle(event);
        return 1;
      }
      if (letters_open_ && it.kind != K_LETTER_JUMP && ex >= split_x_) {
        letters_open_ = false;
        redraw();
        return 1;
      }
      if (it.kind != K_NONE) {
        activate(it);
        return 1;
      }
      return PanelPopup::handle(event);
    }
    case FL_MOVE: {
      build_items();
      const Item *hit = item_at(Fl::event_x(), Fl::event_y());
      Ref r = hit ? Ref{hit->kind, hit->index} : Ref{};
      bool on_bar = false;
      for (auto &a : areas_)
        on_bar = on_bar || (a.scrolls() && Fl::event_x() >= a.bar_x() - S(5) &&
                            Fl::event_x() < a.x + a.w && Fl::event_y() >= a.y &&
                            Fl::event_y() < a.y + a.h);
      if (on_bar != bar_hover_) {
        bar_hover_ = on_bar;
        redraw();
      }
      if (!(r == hover_)) {
        hover_ = r;
        if (hit && hit->entry) {
          selected_ = r;
          keyboard_ = false;
          start_hover(r);
        }
        redraw();
      }
      PanelPopup::handle(event);
      return 1;
    }
    case FL_LEAVE:
      if (hover_.kind != K_NONE) {
        hover_ = {};
        redraw();
      }
      return PanelPopup::handle(event);
    case FL_DRAG:
      if (drag_area_ != K_NONE) {
        drag_scrollbar(Fl::event_y());
        return 1;
      }
      return PanelPopup::handle(event);
    case FL_RELEASE:
      if (drag_area_ != K_NONE) {
        drag_area_ = K_NONE;
        redraw();
        return 1;
      }
      return PanelPopup::handle(event);
    case FL_MOUSEWHEEL: {
      // The wheel scrolls the area under the pointer. The menu holds the
      // pointer grab, so take the position from root coordinates: they are
      // right whichever of the panel's windows X reported the event to.
      int ex = Fl::event_x_root() - x_root(), ey = Fl::event_y_root() - y_root();
      int dy = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
      if (!dy || menu_busy_) return 1;
      build_items();
      const Area *a = area_at(ex, ey);
      // Elsewhere (headings, chips, or a pane that fits): the main list.
      if (!a || !a->scrolls()) {
        Kind main = view_ == V_HOME ? K_ALL_ROW : view_ == V_CATEGORY ? K_CAT_TILE
                    : view_ == V_SEARCH ? K_RESULT : K_NONE;
        a = area(main);
      }
      if (a) scroll_by(a->kind, dy * (compact_ ? S(64) : S(72)));
      return 1;
    }
    case FL_SHORTCUT:
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      bool empty = !*search_->value();
      caret_on_ = true;
      if (k == FL_Escape) {
        if (letters_open_) {
          letters_open_ = false;
          redraw();
        } else if (!empty) {
          search_->value("");
          search_changed(search_, this);
        } else if (view_ != V_HOME) {
          go_back();
        } else {
          close();
        }
        return 1;
      }
      build_items();
      const Item *sel = find(selected_);
      if (k == FL_Menu) {
        if (sel) item_menu(*sel, sel->x + sel->w / 2, sel->y + sel->h / 2);
        return 1;
      }
      if (k == FL_Enter || k == FL_KP_Enter) {
        if (sel) activate(*sel);
        else if (view_ == V_SEARCH && !results_.empty()) {
          const Item *first = find({K_RESULT, 0});
          if (first) activate(*first);
        }
        return 1;
      }
      if (k == FL_BackSpace && empty && view_ != V_HOME) {
        go_back();
        return 1;
      }
      if (k == FL_Tab) return 1;
      bool navigation = k == FL_Down || k == FL_Up || k == FL_Page_Down || k == FL_Page_Up ||
                        (empty && (k == FL_Left || k == FL_Right));
      if (navigation) {
        move_selection(k);
        return 1;
      }
      // Typing anywhere in the menu goes to the search field.
      if (Fl::focus() != search_) give_focus();
      int done = search_->Fl_Input::handle(FL_KEYBOARD);
      search_changed(search_, this);
      search_->cursor_color(theme_.accent.fl());
      Fl::remove_timeout(blink, this);
      Fl::add_timeout(0.55, blink, this);
      return done;
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
  bool focused = Fl::focus() == search_;
  const Item *hov = find(hover_);
  const Item *sel = find(selected_);
  auto is_sel = [&](const Item &it) {
    return sel == &it && (keyboard_ || it.entry || it.kind == K_RESULT);
  };
  auto is_hov = [&](const Item &it) { return hov == &it; };

  // Hover effect of the item under the pointer.
  auto hover_of = [&](const Item &it, double &lift, double &scale, double &slide, double &glow) {
    lift = slide = glow = 0;
    scale = 1;
    if (!effects_.on || !(Ref{it.kind, it.index} == anim_) || !(anim_ == hover_)) return;
    double p = ease_out(anim_hover_), k = effects_.size;
    const std::string &h = effects_.hover;
    if (h == "wave") lift = Sd(5) * p * k;
    else if (h == "pop") scale = 1 + 0.16 * p * k;
    else if (h == "slide") slide = Sd(8) * p * k;
    else if (h == "glow") glow = (0.55 + 0.45 * std::sin((tnow - anim_start_) * 5.0)) * p;
    else if (h == "bounce") {
      double e = tnow - anim_start_;
      lift = e < 1.0 ? Sd(9) * k * std::fabs(std::sin(e * M_PI * 3)) * (1 - e) : 0;
    }
  };
  // Geometry of the icon of an item: center and size.
  auto icon_geom = [&](const Item &it, double &cx, double &cy, double &size) {
    switch (it.kind) {
      case K_TILE:
      case K_CAT_TILE:
        size = compact_ ? S(30) : it.kind == K_CAT_TILE ? S(44) : S(40);
        cx = it.x + it.w / 2.0;
        cy = it.y + (compact_ ? S(4) : S(8)) + size / 2;
        break;
      case K_RECENT:
        size = compact_ ? S(24) : S(28);
        cx = it.x + S(8) + size / 2;
        cy = it.y + it.h / 2.0;
        break;
      case K_RESULT: size = S(32); cx = it.x + S(12) + size / 2; cy = it.y + it.h / 2.0; break;
      case K_CHIP: size = S(18); cx = it.x + S(12) + size / 2; cy = it.y + it.h / 2.0; break;
      default: size = S(24); cx = it.x + S(10) + size / 2; cy = it.y + it.h / 2.0; break;
    }
  };

  {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface);
    // Header band.
    if (t.glass) {
      p.fill_gradient(0, 0, W, header_h_, 0, t.accent.alpha(t.dark ? 0.24 : 0.16),
                      t.accent.alpha(0.02));
      p.glass_sheen(0, 0, W, header_h_, 0, t.dark);
    } else {
      p.fill_rect(0, 0, W, header_h_, t.accent.alpha(0.08));
    }
    p.stroke_rounded(0.5, 0.5, W - 1, H - 1, 0, t.border);

    // Avatar.
    int ad = S(36), ax = pad_, ay = (header_h_ - ad) / 2;
    if (avatar_) {
      cairo_t *cr = p.cr();
      cairo_save(cr);
      cairo_new_path(cr);
      cairo_arc(cr, ax + ad / 2.0, ay + ad / 2.0, ad / 2.0, 0, 2 * M_PI);
      cairo_clip(cr);
      int sw = cairo_image_surface_get_width(avatar_), sh = cairo_image_surface_get_height(avatar_);
      double sc = double(ad) / std::max(1, std::min(sw, sh));
      cairo_translate(cr, ax + (ad - sw * sc) / 2, ay + (ad - sh * sc) / 2);
      cairo_scale(cr, sc, sc);
      cairo_set_source_surface(cr, avatar_, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    } else {
      p.circle(ax + ad / 2.0, ay + ad / 2.0, ad / 2.0, t.accent);
    }

    // Right pane card behind "All Programs".
    if (view_ == V_HOME)
      p.fill_rounded(split_x_, body_top_, W - pad_ - split_x_, body_bottom_ - body_top_, S(10),
                     t.surface_alt.alpha(t.dark ? 0.55 : 0.7));

    for (auto &it : items_) {
      if (!it.visible) continue;
      cairo_save(p.cr());
      if (it.ch < (1 << 19)) {
        cairo_rectangle(p.cr(), it.cx, it.cy, it.cw, it.ch);
        cairo_clip(p.cr());
      }
      bool s = is_sel(it), hv = is_hov(it);
      Color hl = t.accent.alpha(0.18);
      switch (it.kind) {
        case K_HEADER_BUTTON: {
          if (hv) p.circle(it.x + it.w / 2.0, it.y + it.h / 2.0, it.w / 2.0, t.hover);
          if (s && keyboard_) p.stroke_rounded(it.x, it.y, it.w, it.h, it.w / 2.0, t.accent, Sd(2));
          double g = S(20), gx = it.x + (it.w - g) / 2.0, gy = it.y + (it.h - g) / 2.0;
          folder_glyph(p, gx, gy, g, t.text);
          break;
        }
        case K_SEARCH: {
          double r = it.h / 2.0;
          if (focused) {
            // Focus: a soft accent halo and ring, the field slightly lifted.
            p.fill_rounded(it.x - Sd(3), it.y - Sd(3), it.w + Sd(6), it.h + Sd(6), r + Sd(3),
                           t.accent.alpha(0.16));
            p.fill_rounded(it.x, it.y, it.w, it.h, r, t.surface_alt);
            p.stroke_rounded(it.x + 1, it.y + 1, it.w - 2, it.h - 2, r - 1, t.accent, Sd(2));
          } else {
            p.fill_rounded(it.x, it.y, it.w, it.h, r, t.surface_alt);
            p.stroke_rounded(it.x + 0.5, it.y + 0.5, it.w - 1, it.h - 1, r, t.border);
          }
          double g = S(18);
          search_glyph(p, it.x + S(14), it.y + (it.h - g) / 2.0, g,
                       focused ? t.accent : t.text_dim);
          break;
        }
        case K_CATEGORY: {
          bool on = view_ == V_CATEGORY && category_ == it.index;
          double r = it.h / 2.0;
          if (on) p.fill_rounded(it.x, it.y, it.w, it.h, r, t.accent);
          else p.fill_rounded(it.x, it.y, it.w, it.h, r, hv ? t.hover : t.surface_alt.alpha(0.8));
          if (s && keyboard_)
            p.stroke_rounded(it.x + 1, it.y + 1, it.w - 2, it.h - 2, r - 1, t.accent, Sd(2));
          int g = S(18);
          // Labeled chips: icon on the left; icon cells: icon centered.
          double gx = it.x + (it.w - g) / 2.0;
          if (!chips_icons_) {
            // Icon and name centered together in the (justified) chip.
            text_font(t.font_size - 1, on);
            int group = g + S(6) + text_width(it.label);
            gx = it.x + std::max(S(9), (it.w - group) / 2);
          }
          p.image(IconLoader::get({category_icon(categories_[size_t(it.index)]),
                                   "applications-other"},
                                  g),
                  gx, it.y + (it.h - g) / 2.0, g);
          break;
        }
        case K_BACK:
          p.circle(it.x + it.w / 2.0, it.y + it.h / 2.0, it.w / 2.0, hv ? t.hover : t.surface_alt);
          glyph::chevron(p, it.x + (it.w - S(20)) / 2.0, it.y + (it.h - S(20)) / 2.0, S(20), false,
                         t.text);
          break;
        case K_ALL_LETTER:
          if (hv) p.fill_rounded(it.x, it.y + S(2), S(30), it.h - S(4), S(6), t.hover);
          if (s) p.stroke_rounded(it.x + 1, it.y + S(2), S(30), it.h - S(4), S(6), t.accent, Sd(2));
          p.line(it.x + S(36), it.y + it.h / 2.0 + 0.5, it.x + it.w - S(4), it.y + it.h / 2.0 + 0.5,
                 t.border);
          break;
        case K_LETTER_JUMP:
          p.fill_rounded(it.x, it.y, it.w, it.h, S(8), hv || s ? t.accent : t.surface);
          break;
        case K_TILE:
        case K_RECENT:
        case K_ALL_ROW:
        case K_CAT_TILE:
        case K_RESULT:
        case K_CHIP: {
          double lift, scale, slide, glow;
          hover_of(it, lift, scale, slide, glow);
          double r = it.kind == K_CHIP ? it.h / 2.0 : S(8);
          double inset = it.kind == K_TILE || it.kind == K_CAT_TILE ? S(3) : 0;
          if (it.kind == K_CHIP) p.fill_rounded(it.x, it.y, it.w, it.h, r, t.surface_alt);
          if (s) p.fill_rounded(it.x + inset, it.y + inset, it.w - 2 * inset, it.h - 2 * inset, r, hl);
          else if (hv) p.fill_rounded(it.x + inset, it.y + inset, it.w - 2 * inset, it.h - 2 * inset, r, t.hover);
          if (s && keyboard_)
            p.stroke_rounded(it.x + inset + 1, it.y + inset + 1, it.w - 2 * inset - 2,
                             it.h - 2 * inset - 2, r, t.accent, Sd(1.5));
          double cx, cy, size;
          icon_geom(it, cx, cy, size);
          cx += slide;
          cy -= lift;
          size *= scale;
          if (glow > 0) p.circle(cx, cy, size * 0.72, t.accent.alpha(0.28 * glow));
          p.image(IconLoader::get({it.entry->icon, "application-x-executable"}, int(std::lround(size))),
                  cx - size / 2, cy - size / 2, size);
          break;
        }
        default: break;
      }
      cairo_restore(p.cr());
    }

    // Scrollbars: thin, wider while hovered or dragged (they can be dragged).
    int mx = Fl::event_x_root() - x_root(), my = Fl::event_y_root() - y_root();
    for (auto &a : areas_) {
      if (!a.scrolls()) continue;
      double track = a.h - S(8);
      double len = std::max(Sd(24), track * a.h / a.content);
      double pos = (track - len) * *a.scroll / std::max(1, a.content - a.h);
      bool active = drag_area_ == a.kind ||
                    (mx >= a.bar_x() - S(5) && mx < a.x + a.w && my >= a.y && my < a.y + a.h);
      double bw = active ? Sd(6) : Sd(4);
      double bx = a.bar_x() + Sd(6) - bw;
      if (active) p.fill_rounded(bx, a.y + S(4), bw, track, bw / 2, t.text_dim.alpha(0.12));
      p.fill_rounded(bx, a.y + S(4) + pos, bw, len, bw / 2,
                     t.text_dim.alpha(active ? 0.6 : 0.35));
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

  // The search field, then text.
  draw_children();

  int ad = S(36), nx = pad_ + ad + S(12);
  int buttons_x = W - pad_ - S(36);
  if (!avatar_) {
    std::string initial = full_name_.empty() ? "?" : full_name_.substr(0, 1);
    if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] = char(initial[0] - 'a' + 'A');
    text_font(t.font_size + 3, true);
    draw_text(initial, pad_, (header_h_ - ad) / 2, ad, ad, t.on_accent, 1);
  }
  static const std::string os = os_release("PRETTY_NAME");
  text_font(t.font_size + 1, true);
  draw_text(full_name_, nx, header_h_ / 2 - S(19), buttons_x - nx - S(8), S(22), t.text);
  text_font(t.font_size - 1);
  draw_text(os.empty() ? std::string("SDE") : os, nx, header_h_ / 2 + S(1), buttons_x - nx - S(8),
            S(18), t.text_dim);

  if (!*search_->value()) {
    text_font(t.font_size + 1);
    draw_text(focused ? _("Type to search apps, settings and files\xE2\x80\xA6")
                      : _("Search apps, settings and files"),
              search_->x() + S(6), search_->y(), search_->w() - S(6), search_->h(),
              focused ? t.text_dim : t.text_dim.mix(t.surface_alt, 0.15));
  }

  for (auto &it : items_) {
    if (!it.visible) continue;
    bool clipped = it.ch < (1 << 19);
    if (clipped) fl_push_clip(it.cx, it.cy, it.cw, it.ch);
    bool s = is_sel(it);
    switch (it.kind) {
      case K_NONE:
        switch (it.index) {
          case 0: text_font(t.font_size + 1, true); draw_text(it.label, it.x, it.y, it.w, it.h, t.text); break;
          case 1: text_font(t.font_size - 1, true); draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim); break;
          case 2: text_font(t.font_size); draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim, it.group == 1 ? 1 : 0); break;
          case 3: text_font(t.font_size - 1); draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim, 2); break;
          case 4: text_font(t.font_size + 2, true); draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim.mix(t.surface, 0.6), 1); break;
          case 5: {
            {
              Painter p(W, H);
              int g = S(26);
              p.image(IconLoader::get({category_icon(categories_[size_t(std::max(0, category_))]),
                                       "applications-other"},
                                      g),
                      it.x, it.y + (it.h - g) / 2.0, g);
            }
            text_font(t.font_size + 4, true);
            draw_text(it.label, it.x + S(34), it.y, it.w - S(120), it.h, t.text);
            break;
          }
        }
        break;
      case K_CATEGORY: {
        if (chips_icons_) break;  // the name shows on hover
        bool on = view_ == V_CATEGORY && category_ == it.index;
        text_font(t.font_size - 1, on);
        int g = S(18), group = g + S(6) + text_width(it.label);
        int tx = it.x + std::max(S(9), (it.w - group) / 2) + g + S(6);
        draw_text(it.label, tx, it.y, it.w - (tx - it.x) - S(8), it.h, on ? t.on_accent : t.text);
        break;
      }
      case K_ALL_LETTER:
        text_font(t.font_size + 1, true);
        draw_text(it.label, it.x, it.y, S(30), it.h, t.accent_text, 1);
        break;
      case K_LETTER_JUMP: {
        bool hv = is_hov(it) || s;
        text_font(t.font_size + 2, true);
        draw_text(it.label, it.x, it.y, it.w, it.h, hv ? t.on_accent : t.text, 1);
        break;
      }
      case K_TILE:
      case K_CAT_TILE: {
        double lift, scale, slide, glow;
        hover_of(it, lift, scale, slide, glow);
        int icon = compact_ ? S(30) : it.kind == K_CAT_TILE ? S(44) : S(40);
        int top = compact_ ? S(4) : S(8);
        text_font(t.font_size - (it.kind == K_TILE ? 1 : 0));
        draw_text(it.entry->name, it.x + S(4), it.y + top + icon + S(2) - int(lift), it.w - S(8),
                  S(24), t.text, 1);
        break;
      }
      case K_RECENT: {
        double lift, scale, slide, glow;
        hover_of(it, lift, scale, slide, glow);
        int tx = it.x + (compact_ ? S(40) : S(46)) + int(slide), tw = it.w - (tx - it.x) - S(6);
        const DesktopEntry *e = it.entry;
        std::string sub = !e->generic_name.empty() && e->generic_name != e->name ? e->generic_name
                                                                                 : e->comment;
        text_font(t.font_size);
        if (sub.empty() || compact_) {
          draw_text(e->name, tx, it.y, tw, it.h, t.text);
        } else {
          draw_text(e->name, tx, it.y + S(3), tw, it.h / 2 - S(1), t.text);
          text_font(t.font_size - 2);
          draw_text(sub, tx, it.y + it.h / 2, tw, it.h / 2 - S(4), t.text_dim);
        }
        break;
      }
      case K_ALL_ROW: {
        double lift, scale, slide, glow;
        hover_of(it, lift, scale, slide, glow);
        text_font(t.font_size);
        int tx = it.x + S(44) + int(slide);
        draw_text(it.entry->name, tx, it.y, it.w - (tx - it.x) - S(8), it.h, t.text);
        break;
      }
      case K_RESULT: {
        double lift, scale, slide, glow;
        hover_of(it, lift, scale, slide, glow);
        const DesktopEntry *e = it.entry;
        std::string sub = is_file(e) ? e->generic_name
                          : is_extra(e) ? e->generic_name
                          : !e->generic_name.empty() && e->generic_name != e->name ? e->generic_name
                                                                                   : e->comment;
        std::string kind = is_file(e) ? (e->icon == "folder" ? _("Folder") : _("File"))
                           : is_extra(e) ? e->generic_name
                                         : _("App");
        int tx = it.x + S(56) + int(slide), avail = it.w - (tx - it.x) - S(90);
        text_font(t.font_size + 1);
        if (sub.empty() || !show_desc_ || is_extra(e)) {
          draw_text(e->name, tx, it.y, avail, it.h, t.text);
        } else {
          draw_text(e->name, tx, it.y + S(5), avail, it.h / 2 - S(3), t.text);
          text_font(t.font_size - 1);
          draw_text(sub, tx, it.y + it.h / 2, avail, it.h / 2 - S(6), t.text_dim);
        }
        text_font(t.font_size - 1);
        draw_text(kind, it.x + it.w - S(90), it.y, S(80), it.h, t.text_dim, 2);
        break;
      }
      case K_CHIP:
        text_font(t.font_size);
        draw_text(it.entry->name, it.x + S(36), it.y, it.w - S(40), it.h, t.text);
        break;
      default: break;
    }
    if (clipped) fl_pop_clip();
  }

  // Tooltips: the Files button and icon-only category chips.
  if (hov && (hov->kind == K_HEADER_BUTTON || (hov->kind == K_CATEGORY && chips_icons_))) {
    std::string tip = hov->kind == K_HEADER_BUTTON ? std::string(_("Files")) : hov->label;
    text_font(t.font_size - 1);
    int tw = text_width(tip) + S(16), th = S(24);
    int tx = std::min(W - S(4) - tw, hov->x + hov->w / 2 - tw / 2), ty = hov->y + hov->h + S(4);
    {
      Painter p(W, H);
      p.fill_rounded(tx, ty, tw, th, S(6), t.text.alpha(0.88));
    }
    draw_text(tip, tx, ty, tw, th, t.surface, 1);
  }
  if (launching_ && launch_effect_ == "fade") {
    double q = std::min(1.0, (now() - launch_start_) / std::max(0.05, effects_.duration));
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface.alpha(ease_out(q)));
  }
  if (qa_dump_enabled()) qa_dump_menu();
}

// UI test hook (see qa_dump in popup.h): the menu draws its own controls, so
// it lists them itself. Lines starting with "@" describe areas: "@chips"
// (and "@chips-icons" for icon-only chips), "@all"/"@home" scrolled areas and
// "@all-scroll"/"@home-scroll" with the offset and its maximum as x and y.
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
  line("@chips", pad_, cats_y_, w() - 2 * pad_, cats_h_, 1, 0);
  if (chips_icons_) line("@chips-icons", pad_, cats_y_, w() - 2 * pad_, cats_h_, 1, 0);
  for (auto &a : areas_) {
    std::string n = a.kind == K_ALL_ROW ? "all" : a.kind == K_TILE ? "home"
                    : a.kind == K_CAT_TILE ? "category" : "results";
    line("@" + n, a.x, a.y, a.w, a.h, 1, 0);
    line("@" + n + "-scroll", *a.scroll, std::max(0, a.content - a.h), 0, 0, 1, 0);
  }
  for (auto &it : items_) {
    if (!it.visible) continue;
    int cx = it.x + it.w / 2, cy = it.y + it.h / 2;
    bool clipped = it.ch < (1 << 19);
    if (clipped && !(cx >= it.cx && cx < it.cx + it.cw && cy >= it.cy && cy < it.cy + it.ch))
      continue;
    std::string name;
    switch (it.kind) {
      case K_HEADER_BUTTON: name = "files"; break;
      case K_SEARCH: name = "search"; break;
      case K_CATEGORY: name = "category:" + it.label; break;
      case K_TILE: name = (view_ == V_HOME ? "favorite:" : "tile:") + it.entry->name; break;
      case K_RECENT: name = "recent:" + it.entry->name; break;
      case K_ALL_ROW: name = "app:" + it.entry->name; break;
      case K_ALL_LETTER: name = "letter:" + it.label; break;
      case K_LETTER_JUMP: name = "jump:" + it.label; break;
      case K_BACK: name = "back"; break;
      case K_CAT_TILE: name = "tile:" + it.entry->name; break;
      case K_RESULT: name = "result:" + it.entry->name; break;
      case K_CHIP: name = "chip:" + it.entry->name; break;
      default: continue;
    }
    line(name, it.x, it.y, it.w, it.h, 1, clipped ? 1 : 0);
  }
  qa_dump(this, text);
}
