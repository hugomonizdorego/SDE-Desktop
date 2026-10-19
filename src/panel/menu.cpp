// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "build_config.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "panel.h"
#include "spawn.h"
#include "suite.h"
#include "util.h"

using namespace sde;

namespace {

const char *kAll = N_("All applications");

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string category_icon(const std::string &cat) {
  if (cat == kAll) return "applications-all";
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

// Higher is better; 0 means "does not match".
int match_score(const DesktopEntry &e, const std::vector<std::string> &terms) {
  std::string name = lower(e.name), generic = lower(e.generic_name), comment = lower(e.comment);
  std::string exec = lower(basename_of(e.command().empty() ? "" : e.command()[0]));
  int total = 0;
  for (auto &t : terms) {
    if (t.empty()) continue;
    int best = 0;
    if (starts_with(name, t)) best = 100;
    else if (name.find(" " + t) != std::string::npos) best = 80;
    else if (name.find(t) != std::string::npos) best = 60;
    else if (generic.find(t) != std::string::npos) best = 40;
    else if (starts_with(exec, t)) best = 35;
    if (!best)
      for (auto &k : e.keywords)
        if (starts_with(lower(k), t)) best = 30;
    if (!best && comment.find(t) != std::string::npos) best = 15;
    if (!best) return 0;
    total += best;
  }
  return total;
}

DesktopEntry extra(const std::string &id, const std::string &name, const std::string &kind,
                   const std::string &icon, const std::string &exec,
                   const std::vector<std::string> &keywords) {
  DesktopEntry e;
  e.id = "sde-search:" + id;
  e.type = "Application";
  e.name = name;
  e.generic_name = kind;
  e.icon = icon;
  e.exec = exec;
  e.keywords = keywords;
  return e;
}

// Search field that hands navigation keys to the menu.
class SearchInput : public Fl_Input {
 public:
  SearchInput(int x, int y, int w, int h) : Fl_Input(x, y, w, h) {}
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key();
      if (k == FL_Up || k == FL_Down || k == FL_Enter || k == FL_KP_Enter || k == FL_Escape ||
          k == FL_Page_Up || k == FL_Page_Down ||
          ((!*value()) && (k == FL_Left || k == FL_Right)))
        return parent()->handle(FL_KEYBOARD);
    }
    if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) return parent()->handle(e);
    return Fl_Input::handle(e);
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
}

void AppMenu::search_changed(Fl_Widget *, void *data) {
  auto *m = static_cast<AppMenu *>(data);
  m->selected_ = 0;
  m->scroll_ = 0;
  m->filter();
  m->redraw();
}

void AppMenu::reload() {
  apps_ = load_applications("SDE");
  categories_.clear();
  categories_.push_back(kAll);
  for (auto &c : menu_categories()) {
    for (auto &a : apps_) {
      if (a.menu_category() == c) {
        categories_.push_back(c);
        break;
      }
    }
  }
  if (category_ >= int(categories_.size())) category_ = 0;

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
            "sde-settings --page input", {"keyboard", "mouse", "touchpad", "layout", "keyboard"}),
      extra("network", _("Network and Wi-Fi"), settings, "network-wireless",
            "sde-panel --section network", {"wifi", "wlan", "lan", "internet", "jaringan"}),
      extra("bluetooth", _("Bluetooth"), settings, "bluetooth",
            "sde-panel --section bluetooth", {"bluetooth", "pair", "headphones"}),
      extra("sound", _("Sound"), settings, "audio-volume-high", "sde-panel --section sound",
            {"volume", "audio", "microphone", "speaker", "suara"}),
      extra("system", _("Power and Updates"), settings, "preferences-system-power",
            "sde-settings --page system", {"power", "update", "battery", "compositor"}),
      extra("about", _("About This Computer"), settings, "help-about",
            "sde-settings --page about", {"about", "version", "system", "info"}),
      extra("lock", _("Lock Screen"), _("Session"), "system-lock-screen", "sde-lock",
            {"lock", "kunci"}),
      extra("logout", _("Log Out, Restart or Shut Down"), _("Session"), "system-shutdown",
            "sde-logout",
            {"logout", "log out", "restart", "reboot", "shutdown", "power off", "suspend",
             "matikan"}),
  };
}

// Refresh XDG application entries during package installs/removals without
// restarting the panel, preserving the current category, query and selection.
void AppMenu::poll_applications(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  std::string category = self->categories_[size_t(self->category_)];
  std::string id = self->rows_.empty() ? "" : self->rows_[size_t(self->selected_)].entry->id;
  self->reload();
  auto cat = std::find(self->categories_.begin(), self->categories_.end(), category);
  self->category_ = cat == self->categories_.end() ? 0 : int(cat - self->categories_.begin());
  self->filter();
  for (size_t i = 0; i < self->rows_.size(); ++i)
    if (self->rows_[i].entry->id == id) self->selected_ = int(i);
  self->scroll_ = std::min(self->scroll_,
                           std::max(0, int(self->rows_.size()) - self->list_rows_visible()));
  self->redraw();
  Fl::repeat_timeout(2, poll_applications, data);
}

void AppMenu::filter() {
  rows_.clear();
  std::string q = lower(trim(search_->value()));
  if (q.empty()) {
    const std::string &cat = categories_[size_t(category_)];
    for (auto &a : apps_)
      if (cat == kAll || a.menu_category() == cat) rows_.push_back({&a});
  } else {
    auto terms = split(q, ' ');
    std::vector<std::pair<int, Row>> scored;
    for (auto &a : apps_) {
      int s = match_score(a, terms);
      if (s) scored.push_back({s, {&a}});
    }
    // Settings pages and actions follow applications with the same score.
    for (auto &e : extras_) {
      int s = match_score(e, terms);
      if (s) scored.push_back({s - 1, {&e, true}});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](auto &a, auto &b) { return a.first > b.first; });
    for (auto &s : scored) rows_.push_back(s.second);
  }
  if (selected_ >= int(rows_.size())) selected_ = int(rows_.size()) - 1;
  if (selected_ < 0) selected_ = 0;
}

void AppMenu::layout() {
  const Config &cfg = Panel::instance->config();
  search_top_ = cfg.get("menu", "search_position", "top") != "bottom";
  cat_side_ = cfg.get("menu", "categories", "left");
  if (cat_side_ != "right" && cat_side_ != "hidden") cat_side_ = "left";
  grid_ = cfg.get("menu", "view", "list") == "grid";
  show_desc_ = cfg.get_bool("menu", "show_descriptions", true);
  pad_ = S(16);
  header_h_ = S(48);
  footer_h_ = S(60);
  cat_w_ = cat_side_ == "hidden" ? 0 : (w() < S(560) ? S(150) : S(176));
  row_h_ = grid_ ? S(104) : (show_desc_ ? S(48) : S(40));
  cat_row_h_ = S(36);
  icon_ = grid_ ? S(44) : S(32);
  int W = w(), H = h();
  if (search_top_) {
    search_y_ = S(58);
    body_top_ = S(112);
    body_bottom_ = H - footer_h_;
  } else {
    body_top_ = header_h_ + S(8);
    search_y_ = H - footer_h_ - S(52);
    body_bottom_ = search_y_ - S(8);
  }
  if (cat_side_ == "right") {
    cat_x_ = W - pad_ - cat_w_ + S(12);
    list_x_ = pad_;
  } else {
    cat_x_ = pad_;
    list_x_ = pad_ + cat_w_ + (cat_w_ ? S(4) : 0);
  }
  list_w_ = W - 2 * pad_ - cat_w_ - (cat_w_ ? S(4) : 0);
  search_->resize(pad_ + S(42), search_y_ + S(6), W - 2 * pad_ - S(54), S(30));
}

void AppMenu::prepare() {
  theme_ = Panel::instance->theme();
  effects_ = effects_from_config(Panel::instance->config());
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  std::string sz = Panel::instance->config().get("menu", "size", "medium");
  int bw = sz == "small" ? 640 : sz == "large" ? 900 : 760;
  int bh = sz == "small" ? 540 : sz == "large" ? 760 : 650;
  int mw2 = std::min(S(bw), mw - S(16));
  int mh2 = std::min(S(bh), mh - Panel::instance->h() - S(24));
  size(mw2, mh2);
  layout();
  search_->color(theme_.surface_alt.fl());
  search_->textcolor(theme_.text.fl());
  search_->cursor_color(theme_.accent.fl());
  search_->selection_color(theme_.accent.fl());
  search_->textsize(theme_.font_size + 1);
  search_->textfont(FL_HELVETICA);
  search_->value("");
  category_ = 0;
  category_scroll_ = 0;
  selected_ = 0;
  scroll_ = 0;
  hover_zone_ = Z_NONE;
  hover_index_ = anim_index_ = launch_index_ = -1;
  reload();
  filter();
  Fl::remove_timeout(poll_applications, this);
  Fl::add_timeout(2, poll_applications, this);
  Fl::add_timeout(0, [](void *d) { static_cast<AppMenu *>(d)->search_->take_focus(); }, this);
}

void AppMenu::closed() {
  Fl::remove_timeout(animate, this);
  launch_index_ = -1;
}

void AppMenu::item_menu(int row) {
  if (row < 0 || row >= int(rows_.size())) return;
  if (rows_[size_t(row)].extra) {
    launch(row);
    return;
  }
  DesktopEntry e = *rows_[size_t(row)].entry;
  bool pinned = Panel::instance->is_pinned(e.id);
  Fl_Menu_Item items[] = {
      {_("Open"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {pinned ? _("Unpin from Panel") : _("Pin to Panel"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Add to Desktop"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Arrange Menu..."), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y());
  if (m == &items[0]) {
    launch(row);
  } else if (m == &items[1]) {
    if (pinned) Panel::instance->unpin(e.id);
    else Panel::instance->pin(e.id);
    redraw();
  } else if (m == &items[2]) {
    std::string desk = user_dir("DESKTOP");
    make_dirs(desk);
    std::string text;
    if (read_file(e.path, text)) {
      std::string target = desk + "/" + e.id;
      write_file_atomic(target, text);
      // Launchers created by the user are trusted (executable).
      chmod(target.c_str(), 0755);
    }
  } else if (m == &items[3]) {
    layout_menu();
  }
}

// Right-click on the menu: arrange it. Every choice is saved in [menu].
void AppMenu::layout_menu() {
  const Config &cfg = Panel::instance->config();
  auto radio = [](bool on) { return FL_MENU_RADIO | (on ? FL_MENU_VALUE : 0); };
  std::string size = cfg.get("menu", "size", "medium");
  Fl_Menu_Item items[] = {
      {_("Search Bar"), 0, nullptr, nullptr, FL_SUBMENU, 0, 0, 0, 0},
      {_("At the Top"), 0, nullptr, nullptr, radio(search_top_), 0, 0, 0, 0},
      {_("At the Bottom"), 0, nullptr, nullptr, radio(!search_top_), 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Categories"), 0, nullptr, nullptr, FL_SUBMENU, 0, 0, 0, 0},
      {_("On the Left"), 0, nullptr, nullptr, radio(cat_side_ == "left"), 0, 0, 0, 0},
      {_("On the Right"), 0, nullptr, nullptr, radio(cat_side_ == "right"), 0, 0, 0, 0},
      {_("Hidden"), 0, nullptr, nullptr, radio(cat_side_ == "hidden"), 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Applications"), 0, nullptr, nullptr, FL_SUBMENU, 0, 0, 0, 0},
      {_("List"), 0, nullptr, nullptr, radio(!grid_), 0, 0, 0, 0},
      {_("Grid"), 0, nullptr, nullptr, radio(grid_) | FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Show Descriptions"), 0, nullptr, nullptr, FL_MENU_TOGGLE | (show_desc_ ? FL_MENU_VALUE : 0),
       0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Menu Size"), 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER, 0, 0, 0, 0},
      {_("Small"), 0, nullptr, nullptr, radio(size == "small"), 0, 0, 0, 0},
      {_("Medium"), 0, nullptr, nullptr, radio(size != "small" && size != "large"), 0, 0, 0, 0},
      {_("Large"), 0, nullptr, nullptr, radio(size == "large"), 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Effects and Animations..."), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Reset Menu Layout"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
  const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y());
  if (!m) return;
  Panel *p = Panel::instance;
  int i = int(m - items);
  switch (i) {
    case 1: p->set_setting("menu", "search_position", "top"); break;
    case 2: p->set_setting("menu", "search_position", "bottom"); break;
    case 5: p->set_setting("menu", "categories", "left"); break;
    case 6: p->set_setting("menu", "categories", "right"); break;
    case 7: p->set_setting("menu", "categories", "hidden"); break;
    case 10: p->set_setting("menu", "view", "list"); break;
    case 11: p->set_setting("menu", "view", "grid"); break;
    case 12: p->set_setting("menu", "show_descriptions", show_desc_ ? "false" : "true"); break;
    case 15: p->set_setting("menu", "size", "small"); break;
    case 16: p->set_setting("menu", "size", "medium"); break;
    case 17: p->set_setting("menu", "size", "large"); break;
    case 19:
      close();
      spawn_detached({"sde-settings", "--page", "effects"});
      return;
    case 20:
      for (const char *key : {"search_position", "categories", "view", "show_descriptions", "size"})
        p->set_setting("menu", key, "");
      break;
    default: return;
  }
  // Apply at once: the size may change, so place the menu again.
  prepare();
  reposition();
  redraw();
}

int AppMenu::columns() const {
  if (!grid_) return 1;
  return std::max(2, list_w_ / S(124));
}

int AppMenu::list_rows_visible() const {
  return std::max(1, (body_bottom_ - body_top_) / row_h_) * columns();
}

void AppMenu::tile_rect(int i, int &x, int &y, int &w, int &h) const {
  int col = (i - scroll_) % columns(), line = (i - scroll_) / columns();
  w = list_w_ / columns();
  h = row_h_;
  x = list_x_ + col * w;
  y = body_top_ + line * row_h_;
}

void AppMenu::ensure_visible(int row) {
  int vis = list_rows_visible();
  if (row < scroll_) scroll_ = row;
  if (row >= scroll_ + vis) scroll_ = ((row / columns()) - (vis / columns()) + 1) * columns();
  if (scroll_ < 0) scroll_ = 0;
  scroll_ = (scroll_ / columns()) * columns();
}

AppMenu::Zone AppMenu::zone_at(int x, int y, int &index) const {
  index = -1;
  if (y >= search_y_ && y < search_y_ + S(42) && x >= pad_ && x < w() - pad_) return Z_SEARCH;
  if (y >= body_top_ && y < body_bottom_) {
    if (cat_w_ && x >= cat_x_ && x < cat_x_ + cat_w_ - S(8)) {
      int i = category_scroll_ + (y - body_top_ - S(4)) / cat_row_h_;
      if (i >= 0 && i < int(categories_.size())) {
        index = i;
        return Z_CATEGORY;
      }
    } else if (x >= list_x_ && x < list_x_ + list_w_) {
      int column = std::min(columns() - 1, (x - list_x_) / std::max(1, list_w_ / columns()));
      int i = scroll_ + ((y - body_top_) / row_h_) * columns() + column;
      if (i >= 0 && i < int(rows_.size())) {
        index = i;
        return Z_APP;
      }
    }
  }
  if (y >= h() - footer_h_ + S(10) && y < h() - S(10)) {
    if (x >= w() - pad_ - S(40) && x < w() - pad_) return Z_POWER;
    if (x >= w() - pad_ - S(136) && x < w() - pad_ - S(96)) return Z_FILES;
    if (x >= w() - pad_ - S(88) && x < w() - pad_ - S(48)) return Z_SETTINGS;
  }
  return Z_NONE;
}

void AppMenu::launch(int row) {
  if (row < 0 || row >= int(rows_.size())) return;
  DesktopEntry e = *rows_[size_t(row)].entry;
  // The program starts at once; the menu closes when the effect has played.
  launch_entry(e);
  effects_ = effects_from_config(Panel::instance->config());
  if (effects_.launch == "none" || !effects_.on) {
    close();
    return;
  }
  launch_index_ = row;
  launch_effect_ = effects_.launch;
  launch_start_ = now();
  Fl::remove_timeout(animate, this);
  Fl::add_timeout(1.0 / 60, animate, this);
}

void AppMenu::start_hover(int index) {
  anim_index_ = index;
  anim_hover_ = 0;
  anim_start_ = now();
  if (!effects_.on || effects_.hover == "none") return;
  Fl::remove_timeout(animate, this);
  Fl::add_timeout(1.0 / 60, animate, this);
}

// One frame of the hover and launch animations.
void AppMenu::animate(void *data) {
  auto *self = static_cast<AppMenu *>(data);
  if (!self->shown()) return;
  double t = now();
  bool more = false;
  if (self->launch_index_ >= 0) {
    if (t - self->launch_start_ >= self->effects_.duration) {
      self->launch_index_ = -1;
      self->close();
      return;
    }
    more = true;
  }
  if (self->anim_index_ >= 0 && self->hover_zone_ == Z_APP) {
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

int AppMenu::handle(int event) {
  if (launch_index_ >= 0 && (event == FL_PUSH || event == FL_KEYBOARD)) return 1;
  switch (event) {
    case FL_PUSH: {
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (ex < 0 || ey < 0 || ex >= w() || ey >= h()) return PanelPopup::handle(event);
      int idx;
      Zone z = zone_at(ex, ey, idx);
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (z == Z_APP) {
          selected_ = idx;
          redraw();
          item_menu(idx);
        } else {
          layout_menu();
        }
        return 1;
      }
      if (z == Z_CATEGORY) {
        category_ = idx;
        search_->value("");
        selected_ = 0;
        scroll_ = 0;
        filter();
        redraw();
        return 1;
      }
      if (z == Z_APP) {
        launch(idx);
        return 1;
      }
      if (z == Z_FILES) {
        close();
        spawn_detached({"liria", "--no-desktop", home_dir()});
        return 1;
      }
      if (z == Z_SETTINGS) {
        close();
        spawn_detached({"sde-settings"});
        return 1;
      }
      if (z == Z_POWER) {
        close();
        spawn_detached({"sde-logout"});
        return 1;
      }
      if (z == Z_SEARCH) {
        search_->take_focus();
        return PanelPopup::handle(event);
      }
      return PanelPopup::handle(event);
    }
    case FL_MOVE:
    case FL_DRAG: {
      int idx;
      Zone z = zone_at(Fl::event_x(), Fl::event_y(), idx);
      if (z != hover_zone_ || idx != hover_index_) {
        hover_zone_ = z;
        hover_index_ = idx;
        if (z == Z_APP) {
          selected_ = idx;
          start_hover(idx);
        }
        redraw();
      }
      PanelPopup::handle(event);
      return 1;
    }
    case FL_MOUSEWHEEL: {
      int max_scroll = std::max(
          0, ((int(rows_.size()) + columns() - 1) / columns()) * columns() - list_rows_visible());
      int ex = Fl::event_x();
      if (cat_w_ && ex >= cat_x_ && ex < cat_x_ + cat_w_) {
        int visible = std::max(1, (body_bottom_ - body_top_ - S(4)) / cat_row_h_);
        category_scroll_ = std::max(0, std::min(std::max(0, int(categories_.size()) - visible),
                                                category_scroll_ + Fl::event_dy()));
      } else {
        scroll_ = std::max(0, std::min(max_scroll, scroll_ + Fl::event_dy() * columns() * 2));
        scroll_ = (scroll_ / columns()) * columns();
      }
      redraw();
      return 1;
    }
    case FL_SHORTCUT:
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      int n = int(rows_.size());
      if (k == FL_Escape) {
        close();
        return 1;
      }
      if (k == FL_Menu && n > 0) {
        item_menu(selected_);
        return 1;
      }
      if (k == FL_Enter || k == FL_KP_Enter) {
        launch(selected_);
        return 1;
      }
      bool navigation = k == FL_Down || k == FL_Up || k == FL_Page_Down || k == FL_Page_Up ||
                        (grid_mode() && (k == FL_Left || k == FL_Right));
      if (!navigation) {
        // Typing anywhere in the menu goes to the search field, even when
        // keyboard focus was not on it (for example after a click).
        if (Fl::focus() != search_) search_->take_focus();
        int done = search_->Fl_Input::handle(FL_KEYBOARD);
        if (done) search_changed(search_, this);
        return done;
      }
      if (n == 0) return 1;
      if (k == FL_Down) selected_ = std::min(n - 1, selected_ + columns());
      else if (k == FL_Up) selected_ = std::max(0, selected_ - columns());
      else if (k == FL_Right && grid_mode()) selected_ = std::min(n - 1, selected_ + 1);
      else if (k == FL_Left && grid_mode()) selected_ = std::max(0, selected_ - 1);
      else if (k == FL_Page_Down) selected_ = std::min(n - 1, selected_ + list_rows_visible());
      else if (k == FL_Page_Up) selected_ = std::max(0, selected_ - list_rows_visible());
      else return PanelPopup::handle(event);
      ensure_visible(selected_);
      redraw();
      return 1;
    }
    default:
      return PanelPopup::handle(event);
  }
}

void AppMenu::draw() {
  const Theme &t = theme_;
  int W = w(), H = h();
  int cat_icon = S(20), btn = S(40), gi = S(22);
  double tnow = now();
  bool searching = *search_->value() != 0;
  // Hover effect of the tile under the pointer.
  auto hover_of = [&](int i, double &lift, double &scale, double &slide, double &glow) {
    lift = slide = glow = 0;
    scale = 1;
    if (!effects_.on || i != anim_index_ || hover_zone_ != Z_APP) return;
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
  {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface);
    if (t.glass) {
      p.fill_gradient(0, 0, W, S(48), 0, t.accent.alpha(t.dark ? 0.22 : 0.16), t.accent.alpha(0.04));
      p.glass_sheen(0, 0, W, S(48), 0, t.dark);
    } else {
      p.fill_rect(0, 0, W, S(48), t.accent.alpha(.10));
    }
    p.stroke_rounded(0.5, 0.5, W - 1, H - 1, 0, t.border);

    // Search box.
    p.fill_rounded(pad_, search_y_, W - 2 * pad_, S(42), S(8), t.surface_alt);
    if (Fl::focus() == search_)
      p.stroke_rounded(pad_, search_y_, W - 2 * pad_, S(42), S(8), t.accent, Sd(1.5));
    p.image(IconLoader::get({"system-search", "edit-find"}, S(18)), pad_ + S(14),
            search_y_ + S(12), S(18), 0.7);

    // Categories.
    if (cat_w_) {
      for (size_t i = size_t(category_scroll_); i < categories_.size(); ++i) {
        int y = body_top_ + S(4) + (int(i) - category_scroll_) * cat_row_h_;
        if (y + cat_row_h_ > body_bottom_) break;
        bool sel = int(i) == category_ && !searching;
        bool hov = hover_zone_ == Z_CATEGORY && hover_index_ == int(i);
        if (sel) p.fill_rounded(cat_x_, y, cat_w_ - S(12), cat_row_h_ - S(4), S(8), t.accent.alpha(0.16));
        else if (hov) p.fill_rounded(cat_x_, y, cat_w_ - S(12), cat_row_h_ - S(4), S(8), t.hover);
        if (sel) p.fill_rounded(cat_x_, y + S(8), Sd(3), cat_row_h_ - S(20), Sd(1.5), t.accent);
        p.image(IconLoader::get({category_icon(categories_[i]), "applications-other"}, cat_icon),
                cat_x_ + S(12), y + (cat_row_h_ - S(4) - cat_icon) / 2.0, cat_icon);
      }
      double lx = cat_side_ == "right" ? cat_x_ - S(8) : cat_x_ + cat_w_ - S(4);
      p.line(lx, body_top_ + S(6), lx, body_bottom_ - S(6), t.border);
    }

    // Application tiles.
    cairo_save(p.cr());
    cairo_rectangle(p.cr(), list_x_, body_top_, list_w_, body_bottom_ - body_top_);
    cairo_clip(p.cr());
    for (int i = scroll_; i < int(rows_.size()); ++i) {
      int x, y, tw, th;
      tile_rect(i, x, y, tw, th);
      if (y > body_bottom_) break;
      double lift, scale, slide, glow;
      hover_of(i, lift, scale, slide, glow);
      if (i == selected_)
        p.fill_rounded(x, y + S(2), tw - S(6), th - S(6), S(8), t.accent.alpha(0.18));
      else if (grid_)
        p.fill_rounded(x, y + S(2), tw - S(6), th - S(6), S(8), t.surface_alt.alpha(0.6));
      const DesktopEntry *e = rows_[size_t(i)].entry;
      double size = icon_ * scale;
      double cx = grid_ ? x + (tw - S(6)) / 2.0 : x + S(10) + icon_ / 2.0 + slide;
      double cy = grid_ ? y + S(10) + icon_ / 2.0 - lift : y + th / 2.0 - lift;
      if (glow > 0) p.circle(cx, cy, icon_ * 0.72, t.accent.alpha(0.28 * glow));
      p.image(IconLoader::get({e->icon, "application-x-executable"}, int(std::lround(size))),
              cx - size / 2, cy - size / 2, size);
    }
    // Launch effect, drawn over the tiles.
    if (launch_index_ >= 0 && launch_index_ < int(rows_.size())) {
      int x, y, tw, th;
      tile_rect(launch_index_, x, y, tw, th);
      double q = std::min(1.0, (tnow - launch_start_) / std::max(0.05, effects_.duration));
      double cx = grid_ ? x + (tw - S(6)) / 2.0 : x + S(10) + icon_ / 2.0;
      double cy = grid_ ? y + S(10) + icon_ / 2.0 : y + th / 2.0;
      double k = effects_.size;
      auto *icon = IconLoader::get({rows_[size_t(launch_index_)].entry->icon, "application-x-executable"},
                                   icon_ * 3);
      const std::string &fx = launch_effect_;
      if (fx == "zoom" || fx == "zoom-out") {
        double s = fx == "zoom" ? icon_ * (1 + 2.2 * k * ease_out(q)) : icon_ * (1 - ease_out(q));
        if (s > 1) p.image(icon, cx - s / 2, cy - s / 2, s, 1 - q * (fx == "zoom" ? 1 : 0.3));
      } else if (fx == "ripple") {
        for (int r = 0; r < 3; ++r) {
          double qq = q - r * 0.18;
          if (qq <= 0) continue;
          cairo_new_sub_path(p.cr());
          cairo_arc(p.cr(), cx, cy, icon_ * (0.6 + 3.0 * k * ease_out(qq)), 0, 2 * M_PI);
          p.set(t.accent.alpha(0.55 * (1 - qq)));
          cairo_set_line_width(p.cr(), Sd(3));
          cairo_stroke(p.cr());
        }
      } else if (fx == "bounce") {
        double jump = Sd(22) * k * std::fabs(std::sin(q * M_PI * 2)) * (1 - q * 0.5);
        p.image(icon, cx - icon_ / 2.0, cy - icon_ / 2.0 - jump, icon_);
      } else if (fx == "bubbles" || fx == "confetti") {
        static const unsigned colors[] = {0x2672BE, 0xE0525C, 0x2E9E6E, 0xE39A2B, 0x7A4FD1, 0x1C9DB8};
        int n = fx == "bubbles" ? 12 : 26;
        for (int b = 0; b < n; ++b) {
          double angle = (b * 137.5) * M_PI / 180, spread = (0.35 + (b % 5) * 0.16) * k;
          double px, py;
          if (fx == "bubbles") {
            px = cx + std::cos(angle) * icon_ * spread * 1.4;
            py = cy - ease_out(q) * Sd(120) * spread * 1.5;
            p.circle(px, py, Sd(3 + b % 4) * k, t.accent.alpha(0.45 * (1 - q)));
          } else {
            px = cx + std::cos(angle) * Sd(140) * spread * ease_out(q);
            py = cy + std::sin(angle) * Sd(110) * spread * ease_out(q) + Sd(40) * q * q;
            p.fill_rounded(px, py, Sd(6) * k, Sd(3) * k, Sd(1),
                           Color::hex(colors[b % 6], 1 - q));
          }
        }
      }
    }
    cairo_restore(p.cr());
    if (launch_index_ >= 0 && launch_effect_ == "fade") {
      double q = std::min(1.0, (now() - launch_start_) / std::max(0.05, effects_.duration));
      p.fill_rect(0, 0, W, H, t.surface.alpha(ease_out(q)));
    }

    // Scroll indicator.
    int vis = list_rows_visible();
    if (int(rows_.size()) > vis) {
      double track = body_bottom_ - body_top_ - S(8);
      double len = std::max(Sd(24), track * vis / rows_.size());
      double pos = (track - len) * scroll_ / std::max(1, int(rows_.size()) - vis);
      double sx = cat_side_ == "right" ? list_x_ + list_w_ - S(4) : W - pad_ + S(6);
      p.fill_rounded(sx, body_top_ + S(4) + pos, Sd(4), len, Sd(2), t.text_dim.alpha(0.35));
    }

    // Footer.
    p.line(pad_, H - footer_h_ + 0.5, W - pad_, H - footer_h_ + 0.5, t.border);
    int cy = H - footer_h_ / 2;
    p.circle(pad_ + S(18), cy, S(18), t.accent);
    if (hover_zone_ == Z_FILES) p.fill_rounded(W - pad_ - S(136), cy - btn / 2, btn, btn, S(6), t.hover);
    p.image(IconLoader::get({"user-home", "system-file-manager"}, gi),
            W - pad_ - S(136) + (btn - gi) / 2.0, cy - gi / 2.0, gi);
    bool hs = hover_zone_ == Z_SETTINGS, hp = hover_zone_ == Z_POWER;
    if (hs) p.fill_rounded(W - pad_ - S(88), cy - btn / 2, btn, btn, S(10), t.hover);
    if (hp) p.fill_rounded(W - pad_ - btn, cy - btn / 2, btn, btn, S(10), t.hover);
    p.image(IconLoader::get({"preferences-system", "preferences-desktop", "configure"}, gi),
            W - pad_ - S(88) + (btn - gi) / 2.0, cy - gi / 2.0, gi);
    p.image(IconLoader::get({"system-shutdown", "system-log-out", "application-exit"}, gi),
            W - pad_ - btn + (btn - gi) / 2.0, cy - gi / 2.0, gi);
  }

  // Children (the search input) and text.
  draw_children();
  text_font(t.font_size + 3, true);
  draw_text("SDE", pad_, S(8), S(68), S(32), t.accent_text);
  text_font(t.font_size + 1);
  draw_text(_("Applications"), pad_ + S(68), S(8), W - S(200), S(32), t.text);
  text_font(t.font_size - 1);
  std::string count = rows_.size() == 1 ? std::string(_("1 result"))
                                         : format(_("%d results"), int(rows_.size()));
  draw_text(searching ? count : std::string("Alpha"),
            W - pad_ - S(120), S(8), S(120), S(32), t.text_dim, 2);
  if (!searching) {
    text_font(t.font_size + 1);
    draw_text(_("Search applications, settings and actions"), search_->x() + S(3), search_->y(),
              search_->w(), search_->h(), t.text_dim);
  }
  if (cat_w_) {
    for (size_t i = size_t(category_scroll_); i < categories_.size(); ++i) {
      int y = body_top_ + S(4) + (int(i) - category_scroll_) * cat_row_h_;
      if (y + cat_row_h_ > body_bottom_) break;
      bool sel = int(i) == category_ && !searching;
      text_font(t.font_size, sel);
      draw_text(_(categories_[i].c_str()), cat_x_ + S(42), y, cat_w_ - S(60), cat_row_h_ - S(4),
                sel ? t.accent_text : t.text);
    }
  }
  fl_push_clip(list_x_, body_top_, list_w_, body_bottom_ - body_top_);
  for (int i = scroll_; i < int(rows_.size()); ++i) {
    int x, y, tw, th;
    tile_rect(i, x, y, tw, th);
    if (y > body_bottom_) break;
    double lift, scale, slide, glow;
    hover_of(i, lift, scale, slide, glow);
    const DesktopEntry *e = rows_[size_t(i)].entry;
    if (grid_) {
      text_font(t.font_size);
      draw_text(e->name, x + S(4), y + S(16) + icon_ - int(lift), tw - S(14), S(36), t.text, 1);
      continue;
    }
    std::string sub = !e->generic_name.empty() && e->generic_name != e->name ? e->generic_name
                                                                             : e->comment;
    int tx = x + S(22) + icon_ + int(slide), avail = tw - (tx - x) - S(12);
    text_font(t.font_size + 1);
    if (sub.empty() || !show_desc_) {
      draw_text(e->name, tx, y, avail, th, t.text);
    } else {
      draw_text(e->name, tx, y + S(4), avail, th / 2 - S(2), t.text);
      text_font(t.font_size - 1);
      draw_text(sub, tx, y + th / 2, avail, th / 2 - S(6), t.text_dim);
    }
  }
  if (rows_.empty()) {
    text_font(t.font_size + 1);
    draw_text(_("Nothing found"), list_x_, body_top_ + S(20), list_w_, S(40), t.text_dim, 1);
  }
  fl_pop_clip();

  // Footer text.
  int cy = H - footer_h_ / 2;
  std::string name = user_name();
  std::string initial = name.empty() ? "?" : name.substr(0, 1);
  if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] = char(initial[0] - 'a' + 'A');
  text_font(t.font_size + 3, true);
  draw_text(initial, pad_, cy - S(18), S(36), S(36), t.on_accent, 1);
  text_font(t.font_size + 1, true);
  draw_text(name, pad_ + S(46), cy - S(12), W - pad_ * 2 - S(46) - S(144), S(24), t.text);
}
