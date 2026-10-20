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

std::string recent_path() { return state_home() + "/sde/menu-recent"; }

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
  std::string text;
  recent_.clear();
  if (read_file(recent_path(), text))
    for (auto &line : split(text, '\n'))
      if (!trim(line).empty()) recent_.push_back(trim(line));
  if (recent_.size() > 12) recent_.resize(12);
}

void AppMenu::save_favorites() {
  favorites_default_ = false;
  // An empty list is saved as ";" so that the defaults do not come back.
  std::string v = menu::join_id_list(favorites_);
  Panel::instance->set_setting("menu", "favorites", v.empty() ? ";" : v);
}

void AppMenu::add_recent(const std::string &id) {
  menu::remember(recent_, id, 12);
  make_dirs(state_home() + "/sde");
  std::string text;
  for (auto &r : recent_) text += r + "\n";
  write_file_atomic(recent_path(), text);
}

// Refresh XDG application entries during package installs/removals without
// restarting the panel, preserving the view, query and selection.
void AppMenu::poll_applications(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  Fl::repeat_timeout(2, poll_applications, data);
  if (self->menu_busy_ || self->launching_) return;
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

  // Home: favorites, recent applications and quick settings.
  groups_.clear();
  Group fav{_("Pinned Apps"), {}};
  for (auto &id : favorites_)
    if (by_id.count(id)) fav.entries.push_back(by_id[id]);
  groups_.push_back(fav);
  if (show_recent_) {
    Group rec{_("Recently Used"), {}};
    for (auto &id : recent_)
      if (by_id.count(id) && std::find(favorites_.begin(), favorites_.end(), id) == favorites_.end())
        rec.entries.push_back(by_id[id]);
    if (!rec.entries.empty()) groups_.push_back(rec);
  }
  Group quick{_("Quick Settings"), {}};
  for (auto &e : extras_)
    for (const char *id : {"appearance", "display", "network", "sound", "bluetooth", "about"})
      if (e.id == std::string(kExtraPrefix) + id) quick.entries.push_back(&e);
  groups_.push_back(quick);

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
  int bw = sz == "small" ? 600 : sz == "large" ? 820 : 700;
  int bh = sz == "small" ? 520 : sz == "large" ? 720 : 610;
  int mw2 = std::min(S(bw), mw - S(16));
  int mh2 = std::min(S(bh), mh - Panel::instance->h() - S(24));
  size(mw2, mh2);
  metrics();
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
  page_ = 0;
  all_scroll_ = cat_scroll_ = result_scroll_ = cats_scroll_ = 0;
  letters_open_ = false;
  hover_ = selected_ = anim_ = {};
  keyboard_ = false;
  launching_ = false;
  caret_on_ = true;
  reload();
  load_favorites();
  file_index_.clear();
  filter();

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

void AppMenu::metrics() {
  int W = w(), H = h();
  pad_ = S(14);
  header_h_ = S(60);
  search_h_ = S(40);
  cats_h_ = S(38);
  if (search_top_) {
    search_y_ = header_h_;
    cats_y_ = search_y_ + search_h_ + S(8);
    body_top_ = cats_y_ + cats_h_ + S(8);
    body_bottom_ = H - pad_;
  } else {
    cats_y_ = header_h_;
    body_top_ = cats_y_ + cats_h_ + S(8);
    search_y_ = H - pad_ - search_h_;
    body_bottom_ = search_y_ - S(10);
  }
  split_x_ = pad_ + int((W - 2 * pad_) * 0.56);
  search_->resize(pad_ + S(40), search_y_ + S(8), W - 2 * pad_ - S(52), search_h_ - S(16));
}

void AppMenu::build_items() {
  items_.clear();
  int W = w();
  Item it;

  // Header buttons: Files, Settings, Lock, Power.
  int b = S(36), gap = S(4);
  for (int i = 0; i < 4; ++i) {
    it = Item();
    it.kind = K_HEADER_BUTTON;
    it.index = i;
    it.w = it.h = b;
    it.x = W - pad_ - (4 - i) * b - (3 - i) * gap;
    it.y = (header_h_ - b) / 2;
    items_.push_back(it);
  }

  it = Item();
  it.kind = K_SEARCH;
  it.x = pad_;
  it.y = search_y_;
  it.w = W - 2 * pad_;
  it.h = search_h_;
  items_.push_back(it);

  // Category strip: chips with icon and label, scrolled sideways when they
  // do not fit.
  {
    int x = 0, chip_h = cats_h_ - S(6);
    text_font(theme_.font_size, true);  // the selected chip is bold
    std::vector<Item> chips;
    for (size_t i = 0; i < categories_.size(); ++i) {
      Item c;
      c.kind = K_CATEGORY;
      c.index = int(i);
      c.label = category_label(categories_[i]);
      c.w = S(42) + text_width(c.label);
      c.h = chip_h;
      c.x = x;
      c.y = cats_y_ + S(3);
      x += c.w + S(5);
      chips.push_back(c);
    }
    cats_content_w_ = std::max(0, x - S(5));
    int full = W - 2 * pad_, arrow = S(28), reserve = arrow + S(4);
    bool overflow = cats_content_w_ > full;
    // Arrows take room only on the sides where more chips are hidden.
    cats_scroll_ = overflow ? std::max(0, std::min(cats_scroll_, cats_content_w_ - (full - 2 * reserve)))
                            : 0;
    bool left = overflow && cats_scroll_ > 0;
    bool right = overflow && cats_scroll_ < cats_content_w_ - (full - 2 * reserve);
    int area_x = pad_ + (left ? reserve : 0);
    int area_w = full - (left ? reserve : 0) - (right ? reserve : 0);
    for (auto &c : chips) {
      c.x += area_x - cats_scroll_;
      c.cx = area_x;
      c.cw = area_w;
      c.cy = cats_y_;
      c.ch = cats_h_;
      c.visible = c.x + c.w > area_x && c.x < area_x + area_w;
      items_.push_back(c);
    }
    if (overflow) {
      for (int side = 0; side < 2; ++side) {
        if (!(side ? right : left)) continue;
        Item a;
        a.kind = side ? K_CATS_RIGHT : K_CATS_LEFT;
        a.w = arrow;
        a.h = chip_h;
        a.x = side ? W - pad_ - arrow : pad_;
        a.y = cats_y_ + S(3);
        items_.push_back(a);
      }
    }
  }

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
  // Left pane: favorites groups on pages.
  int lx = pad_, lw = split_x_ - S(12) - pad_;
  int title_h = S(30);
  it = Item();
  it.kind = K_NONE;
  it.index = 0;  // pane title
  it.label = _("Favorites");
  it.x = lx + S(4);
  it.y = top;
  it.w = lw / 2;
  it.h = title_h;
  items_.push_back(it);

  int cols = std::max(3, lw / S(84));
  int tile_w = lw / cols, tile_h = S(84), label_h = S(24);
  int content_top = top + title_h + S(2);
  int page = 0, y = content_top, n = 0;
  std::vector<Item> tiles;
  auto heading = [&](const std::string &text) {
    Item h;
    h.kind = K_NONE;
    h.index = 1;  // group heading
    h.label = text;
    h.x = lx + S(6);
    h.y = y;
    h.w = lw - S(12);
    h.h = label_h;
    h.group = page;
    tiles.push_back(h);
    y += label_h;
  };
  for (size_t g = 0; g < groups_.size(); ++g) {
    const Group &grp = groups_[g];
    size_t count = grp.entries.size();
    // Recent applications: one row is enough.
    if (grp.title == _("Recently Used")) count = std::min(count, size_t(cols));
    int rows = std::max(1, int((count + size_t(cols) - 1) / size_t(cols)));
    if (y != content_top && y + label_h + tile_h > bottom) {
      ++page;
      y = content_top;
    }
    heading(grp.title);
    if (count == 0) {
      Item h;
      h.kind = K_NONE;
      h.index = 2;  // hint
      h.label = _("Right-click an app and choose \"Add to Favorites\".");
      h.x = lx + S(6);
      h.y = y;
      h.w = lw - S(12);
      h.h = S(40);
      h.group = page;
      tiles.push_back(h);
      y += S(48);
      continue;
    }
    for (int r = 0; r < rows; ++r) {
      if (y + tile_h > bottom) {
        ++page;
        y = content_top;
        heading(grp.title);
      }
      for (int c = 0; c < cols; ++c) {
        size_t i = size_t(r * cols + c);
        if (i >= count) break;
        Item t;
        t.kind = K_TILE;
        t.index = n++;
        t.entry = grp.entries[i];
        t.group = page;
        t.x = lx + c * tile_w;
        t.y = y;
        t.w = tile_w;
        t.h = tile_h;
        tiles.push_back(t);
      }
      y += tile_h;
    }
    y += S(6);
  }
  page_count_ = size_t(page + 1);
  page_ = std::max(0, std::min(page_, page));
  for (auto &t : tiles) {
    if (t.group != page_) continue;
    if (t.kind == K_NONE) t.group = 0;
    items_.push_back(t);
  }
  if (page_count_ > 1) {
    int dot = S(14), arrow = S(26);
    int total = arrow * 2 + dot * int(page_count_) + S(8);
    int x = lx + lw - total, cy = top + title_h / 2;
    Item a;
    a.kind = K_PAGE_PREV;
    a.x = x;
    a.y = cy - arrow / 2;
    a.w = a.h = arrow;
    items_.push_back(a);
    x += arrow + S(4);
    for (int p = 0; p < int(page_count_); ++p) {
      Item d;
      d.kind = K_PAGE_DOT;
      d.index = p;
      d.x = x;
      d.y = cy - dot / 2;
      d.w = d.h = dot;
      items_.push_back(d);
      x += dot;
    }
    a.kind = K_PAGE_NEXT;
    a.x = x + S(4);
    items_.push_back(a);
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
    int gc = 5, cell = std::min((rw - S(24)) / gc, S(48));
    int grows = (int(letters.size()) + gc - 1) / gc;
    cell = std::min(cell, std::max(S(24), area_h / std::max(1, grows)));
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
  int row_h = S(36), letter_h = S(28), ly = 0, idx = 0;
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
      h.w = rw - S(18);
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
    r.w = rw - S(18);
    r.h = row_h;
    rows.push_back(r);
    ly += row_h;
  }
  all_content_h_ = ly;
  all_scroll_ = std::max(0, std::min(all_scroll_, all_content_h_ - area_h));
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
  int cols = std::max(3, area_w / S(104));
  int tile_w = area_w / cols, tile_h = S(100);
  int rows = (int(category_apps_.size()) + cols - 1) / cols;
  cat_content_h_ = rows * tile_h;
  cat_scroll_ = std::max(0, std::min(cat_scroll_, cat_content_h_ - area_h));
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
  result_content_h_ = int(results_.size()) * row_h;
  result_scroll_ = std::max(0, std::min(result_scroll_, result_content_h_ - area_h));
  for (size_t i = 0; i < results_.size(); ++i) {
    Item r;
    r.kind = K_RESULT;
    r.index = int(i);
    r.entry = results_[i];
    r.x = pad_;
    r.y = view_top_ + int(i) * row_h - result_scroll_;
    r.w = W - 2 * pad_ - S(8);
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
      if (it.index == 0) {
        if (!find_program("liria").empty()) spawn_detached({"liria", "--no-desktop", home_dir()});
        else open_uri(home_dir());
      } else if (it.index == 1) {
        spawn_detached({"sde-settings"});
      } else if (it.index == 2) {
        spawn_detached({"sde-lock"});
      } else {
        spawn_detached({"sde-logout"});
      }
      return;
    case K_SEARCH:
      give_focus();
      if (!*search_->value() && view_ != V_SEARCH_HOME) {
        before_search_ = view_;
        set_view(V_SEARCH_HOME);
      }
      return;
    case K_CATEGORY: open_category(it.index); return;
    case K_CATS_LEFT:
    case K_CATS_RIGHT:
      cats_scroll_ += (it.kind == K_CATS_LEFT ? -1 : 1) * (w() / 2);
      redraw();
      return;
    case K_PAGE_PREV: page_ = std::max(0, page_ - 1); break;
    case K_PAGE_NEXT: page_ = std::min(int(page_count_) - 1, page_ + 1); break;
    case K_PAGE_DOT: page_ = it.index; break;
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
  bool recent = std::find(recent_.begin(), recent_.end(), e.id) != recent_.end();
  bool pinned = app && Panel::instance->is_pinned(e.id);
  enum { OPEN = 1, FAV, FAV_LEFT, FAV_RIGHT, PIN, DESKTOP, FORGET, FOLDER, ARRANGE };
  std::vector<Fl_Menu_Item> items;
  auto add = [&](const char *text, long code, int flags = 0) {
    Fl_Menu_Item m = {text, 0, nullptr, reinterpret_cast<void *>(code), flags, 0, 0, 0, 0};
    items.push_back(m);
  };
  add(_("Open"), OPEN, FL_MENU_DIVIDER);
  if (file) add(_("Open Containing Folder"), FOLDER, FL_MENU_DIVIDER);
  if (app) {
    add(fav ? _("Remove from Favorites") : _("Add to Favorites"), FAV);
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
      if (fav) favorites_.erase(pos);
      else favorites_.push_back(e.id);
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
      recent_.erase(std::remove(recent_.begin(), recent_.end(), e.id), recent_.end());
      add_recent("");  // saves the list
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
    case K_CATEGORY:
      if (it.x < it.cx) cats_scroll_ -= it.cx - it.x;
      else if (it.x + it.w > it.cx + it.cw) cats_scroll_ += it.x + it.w - it.cx - it.cw;
      break;
    default: break;
  }
}

void AppMenu::scroll_by(Kind area, int dy) {
  if (area == K_ALL_ROW) all_scroll_ = std::max(0, all_scroll_ + dy);
  else if (area == K_CAT_TILE) cat_scroll_ = std::max(0, cat_scroll_ + dy);
  else if (area == K_RESULT) result_scroll_ = std::max(0, result_scroll_ + dy);
  else if (area == K_CATEGORY) cats_scroll_ = std::max(0, cats_scroll_ + dy);
  redraw();
}

// Moves the selection to the nearest item in the direction of |key|, across
// panes (arrow keys) or by one screen (Page Up/Down).
void AppMenu::move_selection(int key) {
  auto selectable = [](Kind k) {
    return k == K_HEADER_BUTTON || k == K_CATEGORY || k == K_TILE || k == K_ALL_ROW ||
           k == K_LETTER_JUMP || k == K_BACK || k == K_CAT_TILE ||
           k == K_RESULT || k == K_CHIP;
  };
  build_items();
  const Item *cur = find(selected_);
  keyboard_ = true;
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
  if (key == FL_Page_Down || key == FL_Page_Up) {
    int dir = key == FL_Page_Down ? 1 : -1;
    if (cur->kind == K_TILE && view_ == V_HOME) {
      page_ = std::max(0, std::min(int(page_count_) - 1, page_ + dir));
      selected_ = {K_TILE, -1};
      build_items();
      for (auto &it : items_)
        if (it.kind == K_TILE) {
          selected_ = {K_TILE, it.index};
          break;
        }
      redraw();
      return;
    }
    int span = cur->ch < (1 << 19) ? cur->ch - S(36) : S(200);
    double target = cur->y + cur->h / 2.0 + dir * span;
    const Item *best = nullptr;
    double best_d = 1e9;
    for (auto &it : items_) {
      if (it.kind != cur->kind) continue;
      double d = std::fabs(it.y + it.h / 2.0 - target) + std::fabs(double(it.x - cur->x));
      if (d < best_d) best_d = d, best = &it;
    }
    if (best) {
      selected_ = {best->kind, best->index};
      ensure_visible(*best);
    }
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
    case FL_MOUSEWHEEL: {
      int ex = Fl::event_x(), ey = Fl::event_y(), dy = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
      if (ey >= cats_y_ && ey < cats_y_ + cats_h_) {
        scroll_by(K_CATEGORY, dy * S(60));
      } else if (view_ == V_HOME && ex < split_x_ && ey >= body_top_) {
        page_ = std::max(0, std::min(int(page_count_) - 1, page_ + (dy > 0 ? 1 : -1)));
        redraw();
      } else if (view_ == V_HOME && !letters_open_) {
        scroll_by(K_ALL_ROW, dy * S(72));
      } else if (view_ == V_CATEGORY) {
        scroll_by(K_CAT_TILE, dy * S(72));
      } else if (view_ == V_SEARCH) {
        scroll_by(K_RESULT, dy * S(72));
      }
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
        size = it.kind == K_CAT_TILE ? S(52) : S(48);
        cx = it.x + it.w / 2.0;
        cy = it.y + S(8) + size / 2;
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
          if (it.index == 0) folder_glyph(p, gx, gy, g, t.text);
          else if (it.index == 1) glyph::gear(p, gx, gy, g, t.text);
          else if (it.index == 2) glyph::lock(p, gx, gy, g, t.text);
          else glyph::power(p, gx, gy, g, hv ? Color::hex(0xE0525C) : t.text);
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
          p.image(IconLoader::get({category_icon(categories_[size_t(it.index)]),
                                   "applications-other"},
                                  g),
                  it.x + S(10), it.y + (it.h - g) / 2.0, g);
          break;
        }
        case K_CATS_LEFT:
        case K_CATS_RIGHT:
          p.circle(it.x + it.w / 2.0, it.y + it.h / 2.0, it.w / 2.0,
                   hv ? t.hover : t.surface_alt);
          glyph::chevron(p, it.x + (it.w - S(18)) / 2.0, it.y + (it.h - S(18)) / 2.0, S(18),
                         it.kind == K_CATS_RIGHT, t.text);
          break;
        case K_PAGE_PREV:
        case K_PAGE_NEXT: {
          bool enabled = it.kind == K_PAGE_PREV ? page_ > 0 : page_ + 1 < int(page_count_);
          if (hv && enabled) p.circle(it.x + it.w / 2.0, it.y + it.h / 2.0, it.w / 2.0, t.hover);
          glyph::chevron(p, it.x + (it.w - S(18)) / 2.0, it.y + (it.h - S(18)) / 2.0, S(18),
                         it.kind == K_PAGE_NEXT, enabled ? t.text : t.text_dim.alpha(0.4));
          break;
        }
        case K_PAGE_DOT: {
          bool on = it.index == page_;
          if (on) p.fill_rounded(it.x + Sd(1), it.y + it.h / 2.0 - Sd(3), it.w - Sd(2), Sd(6), Sd(3), t.accent);
          else p.circle(it.x + it.w / 2.0, it.y + it.h / 2.0, Sd(3), t.text_dim.alpha(hv ? 0.8 : 0.4));
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

    // Scroll indicators.
    auto scrollbar = [&](int x, int top, int bottom, int content, int scroll) {
      int view = bottom - top;
      if (content <= view) return;
      double track = view - S(8);
      double len = std::max(Sd(24), track * view / content);
      double pos = (track - len) * scroll / std::max(1, content - view);
      p.fill_rounded(x, top + S(4) + pos, Sd(4), len, Sd(2), t.text_dim.alpha(0.35));
    };
    if (view_ == V_HOME && !letters_open_)
      scrollbar(W - pad_ - S(8), all_top_, all_bottom_, all_content_h_, all_scroll_);
    else if (view_ == V_CATEGORY)
      scrollbar(W - pad_ + S(4), view_top_, view_bottom_, cat_content_h_, cat_scroll_);
    else if (view_ == V_SEARCH && !results_.empty())
      scrollbar(W - pad_ - S(2), view_top_, view_bottom_, result_content_h_, result_scroll_);

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
  int buttons_x = W - pad_ - 4 * S(36) - 3 * S(4);
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
        bool on = view_ == V_CATEGORY && category_ == it.index;
        text_font(t.font_size, on);
        draw_text(it.label, it.x + S(34), it.y, it.w - S(38), it.h, on ? t.on_accent : t.text);
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
        int icon = it.kind == K_CAT_TILE ? S(52) : S(48);
        text_font(t.font_size - (it.kind == K_TILE ? 1 : 0));
        draw_text(it.entry->name, it.x + S(4), it.y + S(12) + icon - int(lift), it.w - S(8), S(26),
                  t.text, 1);
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

  // Tooltips for the header buttons.
  if (hov && hov->kind == K_HEADER_BUTTON) {
    static const char *tips[] = {N_("Files"), N_("Settings"), N_("Lock"), N_("Power")};
    std::string tip = _(tips[hov->index]);
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
}
