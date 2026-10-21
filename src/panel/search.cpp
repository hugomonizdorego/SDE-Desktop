// SPDX-License-Identifier: GPL-3.0-or-later
#include "search.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>

#include "i18n.h"
#include "icons.h"
#include "menu.h"
#include "panel.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

namespace panel_search {

namespace {
const char *kExtraPrefix = "sde-search:";
const char *kFilePrefix = "sde-file:";

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
}  // namespace

bool is_extra(const DesktopEntry *e) { return e && starts_with(e->id, kExtraPrefix); }
bool is_file(const DesktopEntry *e) { return e && starts_with(e->id, kFilePrefix); }
bool is_app(const DesktopEntry *e) { return e && !is_extra(e) && !is_file(e); }

std::vector<DesktopEntry> extras() {
  std::string settings = _("Settings");
  return {
      extra("appearance", _("Appearance"), settings, "preferences-desktop-theme",
            "sde-settings --page appearance",
            {"theme", "dark", "light", "aero", "glass", "liquid", "wallpaper", "accent", "color",
             "transparency", "blur", "font", "icons", "tema"}),
      extra("effects", _("Effects and Animations"), settings, "preferences-desktop-effects",
            "sde-settings --page effects",
            {"animation", "effect", "compositor", "shadow", "fade", "efek", "animasi"}),
      extra("panel", _("Panel"), settings, "preferences-desktop", "sde-settings --page panel",
            {"taskbar", "tray", "clock", "pinned", "autohide", "search"}),
      extra("desktop", _("Desktop Icons"), settings, "user-desktop",
            "sde-settings --page desktop", {"icons", "home", "trash"}),
      extra("action-center", _("Action Center"), settings, "preferences-system",
            "sde-settings --page action-center", {"quick", "tiles", "media", "volume"}),
      extra("notifications", _("Notifications"), settings, "preferences-system-notifications",
            "sde-settings --page notifications", {"disturb", "dnd", "notifikasi"}),
      extra("display", _("Displays"), settings, "preferences-desktop-display",
            "sde-display-settings",
            {"monitor", "screen", "resolution", "scale", "brightness", "layar"}),
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
            {"logout", "log out", "restart", "reboot", "shutdown", "shut down", "power off",
             "suspend", "matikan"}),
  };
}

std::vector<menu::Usage> load_usage() {
  std::string text;
  if (read_file(usage_path(), text)) return menu::parse_usage(text);
  if (read_file(recent_path(), text)) return menu::parse_usage(text, (long long)time(nullptr));
  return {};
}

void save_usage(const std::vector<menu::Usage> &usage) {
  make_dirs(state_home() + "/sde");
  write_file_atomic(usage_path(), menu::format_usage(usage));
}

void record_launch(const std::string &id) {
  auto usage = load_usage();
  menu::record_launch(usage, id, (long long)time(nullptr));
  save_usage(usage);
}

void glyph(Painter &p, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  cairo_new_path(cr);
  cairo_arc(cr, x + s * 0.43, y + s * 0.43, s * 0.27, 0, 2 * M_PI);
  cairo_move_to(cr, x + s * 0.63, y + s * 0.63);
  cairo_line_to(cr, x + s * 0.86, y + s * 0.86);
  p.set(c);
  cairo_set_line_width(cr, std::max(1.2, s * 0.1));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_stroke(cr);
}

void draw_field_back(Painter &p, const Theme &t, int x, int y, int w, int h, bool focused,
                     bool hover, double r) {
  if (focused) {
    p.fill_rounded(x, y, w, h, r, t.surface_alt.mix(t.dark ? Color::hex(0) : Color::hex(0xFFFFFF),
                                                     t.dark ? 0.15 : 0.5));
    p.stroke_rounded(x + 1, y + 1, w - 2, h - 2, std::max(0.0, r - 1), t.accent, Sd(2));
  } else {
    p.fill_rounded(x, y, w, h, r, hover ? t.hover.mix(t.surface_alt, 0.4) : t.surface_alt);
    p.stroke_rounded(x + 0.5, y + 0.5, w - 1, h - 1, r, hover ? t.text_dim.alpha(0.5) : t.border);
  }
  double g = std::min(S(18), h - S(10));
  glyph(p, x + S(10), y + (h - g) / 2.0, g, focused ? t.accent : t.text_dim);
}

void draw_field_text(const Theme &t, int x, int y, int w, int h, const std::string &text,
                     size_t caret, bool caret_on, bool focused, bool selected,
                     const std::string &placeholder) {
  int tx = x + S(36), tw = w - S(36) - S(8);
  if (tw <= 0) return;
  text_font(t.font_size + (h >= S(34) ? 1 : 0));
  if (text.empty()) {
    if (!placeholder.empty())
      draw_text(placeholder, tx, y, tw, h, focused ? t.text_dim : t.text_dim.mix(t.surface_alt, 0.1));
  } else {
    // Long queries scroll so that the caret stays in view.
    int before = text_width(text.substr(0, std::min(caret, text.size())));
    int shift = std::max(0, before - tw + S(4));
    fl_push_clip(tx, y, tw, h);
    if (selected) {
      int sw = text_width(text);
      fl_color(t.accent.alpha(1).mix(t.surface_alt, 0.55).fl());
      fl_rectf(tx - shift, y + S(6), std::min(sw, tw + shift), h - S(12));
    }
    fl_color(t.text.fl());
    fl_draw(text.c_str(), tx - shift, y + (h + fl_height()) / 2 - fl_descent());
    fl_pop_clip();
    tx += before - shift;
  }
  if (focused && caret_on && !selected) {
    int cx = text.empty() ? tx : tx;
    fl_color(t.text.fl());
    fl_rectf(cx, y + S(9), std::max(1, S(1.5)), h - S(18));
  }
}

}  // namespace panel_search

using namespace panel_search;

// ===================================================================== flyout

SearchFlyout::SearchFlyout() : PanelPopup("SDE Search") {
  end();
  callback([](Fl_Widget *w, void *) { static_cast<SearchFlyout *>(w)->close(); });
}

SearchFlyout::~SearchFlyout() { Fl::remove_timeout(blink, this); }

bool SearchFlyout::own_field() const { return !Panel::instance->search_box_shown(); }

void SearchFlyout::open_with(const std::string &text, int anchor_x) {
  if (is_open()) {
    if (!text.empty()) type(text);
    return;
  }
  query_ = text;
  caret_ = query_.size();
  select_all_ = false;
  open_at(anchor_x, false);
}

void SearchFlyout::prepare() {
  theme_ = Panel::instance->theme();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  int W = std::min(S(620), mw - S(16));
  int H = std::min(S(540), mh - Panel::instance->h() - S(24));
  size(W, H);
  std::string stamp = applications_stamp();
  if (stamp != apps_stamp_ || apps_.empty()) {
    apps_stamp_ = stamp;
    apps_ = load_applications("SDE");
  }
  extras_ = extras();
  usage_ = load_usage();
  file_index_.clear();
  tab_ = 0;
  scroll_ = 0;
  hover_ = -1;
  sel_kind_ = K_NONE;
  sel_index_ = -1;
  caret_on_ = true;
  filter();
  Fl::remove_timeout(blink, this);
  Fl::add_timeout(0.55, blink, this);
}

void SearchFlyout::closed() {
  Fl::remove_timeout(blink, this);
  query_.clear();
  caret_ = 0;
  select_all_ = false;
  if (Panel::instance) Panel::instance->search_changed();
}

void SearchFlyout::blink(void *data) {
  auto *self = static_cast<SearchFlyout *>(data);
  if (!self->is_open()) return;
  Fl::repeat_timeout(0.55, blink, data);
  self->caret_on_ = !self->caret_on_;
  if (self->own_field()) self->damage_rect(0, self->field_y_, self->w(), self->field_h_);
  else Panel::instance->search_changed();
}

void SearchFlyout::filter() {
  std::map<std::string, const DesktopEntry *> by_id;
  for (auto &a : apps_) by_id[a.id] = &a;
  long long now = (long long)time(nullptr);
  // Start page: the most used applications (or the Start tiles) and the
  // ones opened last.
  top_.clear();
  for (auto &id : menu::rank_frecent(usage_, now, 32))
    if (by_id.count(id) && top_.size() < 6) top_.push_back(by_id[id]);
  for (auto &id : start_tiles::ids())
    if (by_id.count(id) && top_.size() < 6 &&
        std::find(top_.begin(), top_.end(), by_id[id]) == top_.end())
      top_.push_back(by_id[id]);
  recent_.clear();
  for (auto &id : menu::by_last_use(usage_, 12))
    if (by_id.count(id) && recent_.size() < 5) recent_.push_back(by_id[id]);

  results_.clear();
  files_.clear();
  std::string q = lower(trim(query_));
  if (q.empty()) return;
  auto terms = split(q, ' ');
  std::vector<std::pair<int, const DesktopEntry *>> scored;
  if (tab_ == 0 || tab_ == 1)
    for (auto &a : apps_) {
      int s = menu::match_score(a, terms);
      if (s) scored.push_back({s, &a});
    }
  // Settings pages and actions follow applications with the same score.
  if (tab_ == 0 || tab_ == 2)
    for (auto &e : extras_) {
      int s = menu::match_score(e, terms);
      if (s) scored.push_back({s - 1, &e});
    }
  std::stable_sort(scored.begin(), scored.end(),
                   [](auto &a, auto &b) { return a.first > b.first; });
  for (auto &s : scored) results_.push_back(s.second);
  // The best match first, then applications before Settings pages, each
  // kind in one block under its heading.
  if (results_.size() > 2)
    std::stable_partition(results_.begin() + 1, results_.end(),
                          [](const DesktopEntry *e) { return is_app(e); });
  if (tab_ != 0 && tab_ != 3) return;
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
  std::string home = home_dir();
  for (auto &path : file_index_.search(terms, tab_ == 3 ? 40 : 8)) {
    DesktopEntry e;
    bool dir = file_index_.is_dir(path);
    e.id = kFilePrefix + path;
    e.type = "Link";
    e.url = path;
    e.path = path;
    e.name = basename_of(path);
    std::string parent = path.substr(0, path.rfind('/'));
    if (parent == home) parent = "~";
    else if (starts_with(parent, home + "/")) parent = "~" + parent.substr(home.size());
    e.generic_name = parent;
    e.icon = menu::file_icon(path, dir);
    files_.push_back(e);
  }
  for (auto &f : files_) results_.push_back(&f);
}

void SearchFlyout::type(const std::string &text) {
  if (text.empty()) return;
  if (select_all_) {
    query_.clear();
    caret_ = 0;
    select_all_ = false;
  }
  std::string clean;
  for (char c : text)
    if (c != '\n' && c != '\r' && c != '\t') clean += c;
  query_.insert(std::min(caret_, query_.size()), clean);
  caret_ += clean.size();
  changed();
}

void SearchFlyout::changed() {
  caret_on_ = true;
  Fl::remove_timeout(blink, this);
  if (is_open()) Fl::add_timeout(0.55, blink, this);
  scroll_ = 0;
  filter();
  sel_kind_ = results_.empty() ? K_NONE : K_RESULT;
  sel_index_ = results_.empty() ? -1 : 0;
  redraw();
  Panel::instance->search_changed();
}

// ---------------------------------------------------------------- layout

void SearchFlyout::build() {
  items_.clear();
  const Theme &t = theme_;
  int W = w(), H = h(), pad = S(12);
  bool top_panel = t.panel_top;
  int top = pad, bottom = H - pad;
  field_h_ = 0;
  if (own_field()) {
    field_h_ = S(40);
    field_y_ = top_panel ? pad : H - pad - field_h_;
    if (top_panel) top = field_y_ + field_h_ + S(10);
    else bottom = field_y_ - S(10);
    Item f;
    f.kind = K_FIELD;
    f.x = pad;
    f.y = field_y_;
    f.w = W - 2 * pad;
    f.h = field_h_;
    items_.push_back(f);
  }
  // Tabs.
  static const char *const tabs[] = {N_("All"), N_("Apps"), N_("Settings"), N_("Files")};
  text_font(t.font_size, true);
  int tx = pad;
  for (int i = 0; i < 4; ++i) {
    Item it;
    it.kind = K_TAB;
    it.index = i;
    it.label = _(tabs[i]);
    it.x = tx;
    it.y = top;
    it.w = text_width(it.label) + S(24);
    it.h = S(30);
    tx += it.w + S(4);
    items_.push_back(it);
  }
  top += S(30) + S(10);
  list_top_ = top + S(1);
  list_bottom_ = bottom;

  auto heading = [&](const std::string &label, int x, int y, int w) {
    Item h;
    h.kind = K_HEADING;
    h.label = label;
    h.x = x;
    h.y = y;
    h.w = w;
    h.h = S(26);
    items_.push_back(h);
  };

  if (trim(query_).empty()) {
    // Start page.
    int y = list_top_ + S(4), area = W - 2 * pad;
    if (!top_.empty()) {
      heading(_("Top apps"), pad + S(4), y, area);
      y += S(28);
      int n = 6, cw = area / n, th = S(86);
      if (cw < S(80)) n = std::max(3, area / S(80)), cw = area / n;
      for (size_t i = 0; i < top_.size() && int(i) < n; ++i) {
        Item it;
        it.kind = K_TOP;
        it.index = int(i);
        it.entry = top_[i];
        it.x = pad + int(i) * cw;
        it.y = y;
        it.w = cw - S(4);
        it.h = th;
        items_.push_back(it);
      }
      y += th + S(10);
    }
    bool two = area >= S(460);
    int cw = two ? (area - S(12)) / 2 : area;
    int ry = y, qy = y;
    if (!recent_.empty()) {
      heading(_("Recent"), pad + S(4), ry, cw);
      ry += S(28);
      for (size_t i = 0; i < recent_.size(); ++i) {
        if (ry + S(36) > bottom) break;
        Item it;
        it.kind = K_RECENT;
        it.index = int(i);
        it.entry = recent_[i];
        it.x = pad;
        it.y = ry;
        it.w = cw;
        it.h = S(36);
        items_.push_back(it);
        ry += S(36);
      }
    }
    // Quick settings beside the recent applications, or in two columns
    // of their own when nothing was opened yet.
    int qcols = two && recent_.empty() ? 2 : 1;
    int qx = two && !recent_.empty() ? pad + cw + S(12) : pad;
    if (!two && !recent_.empty()) qy = ry + S(10);
    heading(_("Quick settings"), qx + S(4), qy, cw);
    qy += S(28);
    int qy0 = qy;
    static const char *const quick[] = {"appearance", "display", "network", "sound",
                                        "notifications", "system"};
    int n = 0;
    for (size_t i = 0; i < extras_.size(); ++i) {
      bool wanted = false;
      for (const char *q : quick) wanted = wanted || extras_[i].id == std::string(kExtraPrefix) + q;
      if (!wanted) continue;
      int col = qcols == 2 ? n % 2 : 0;
      int y = qcols == 2 ? qy0 + (n / 2) * S(36) : qy;
      if (y + S(36) > bottom) break;
      Item it;
      it.kind = K_CHIP;
      it.index = int(i);
      it.entry = &extras_[i];
      it.x = qx + col * (cw + S(12));
      it.y = y;
      it.w = cw;
      it.h = S(36);
      items_.push_back(it);
      qy = y + S(36);
      ++n;
    }
    content_h_ = 0;
    return;
  }

  // Results: the list on the left, the selected result on the right.
  int area = W - 2 * pad;
  bool preview = area >= S(500);
  int lw = preview ? int(area * 0.56) : area;
  if (results_.empty()) {
    Item m;
    m.kind = K_HEADING;
    m.index = 1;  // centered message
    m.label = format(_("No results for \xE2\x80\x9C%s\xE2\x80\x9D"), trim(query_).c_str());
    m.x = pad;
    m.y = list_top_ + S(30);
    m.w = area;
    m.h = S(30);
    items_.push_back(m);
    m.index = 2;
    m.label = _("Search finds applications, settings, session actions and files in your home folder.");
    m.y += S(30);
    items_.push_back(m);
    content_h_ = 0;
    return;
  }
  std::vector<Item> list;
  int y = 0;
  auto add_heading = [&](const std::string &label) {
    Item h;
    h.kind = K_HEADING;
    h.label = label;
    h.x = pad + S(4);
    h.y = y;
    h.w = lw - S(8);
    h.h = S(26);
    list.push_back(h);
    y += S(26);
  };
  add_heading(_("Best match"));
  for (size_t i = 0; i < results_.size(); ++i) {
    const DesktopEntry *e = results_[i];
    if (i == 1 || (i > 1 && (is_app(e) != is_app(results_[i - 1]) ||
                             is_file(e) != is_file(results_[i - 1])))) {
      y += S(4);
      add_heading(is_file(e) ? _("Files and folders") : is_extra(e) ? _("Settings") : _("Apps"));
    }
    Item r;
    r.kind = K_RESULT;
    r.index = int(i);
    r.entry = e;
    r.x = pad;
    r.y = y;
    r.w = lw - S(10);
    r.h = i == 0 ? S(58) : S(38);
    list.push_back(r);
    y += r.h;
  }
  content_h_ = y;
  int view_h = list_bottom_ - list_top_;
  scroll_ = std::max(0, std::min(scroll_, content_h_ - view_h));
  for (auto &it : list) {
    it.y += list_top_ - scroll_;
    it.visible = it.y + it.h > list_top_ && it.y < list_bottom_;
    items_.push_back(it);
  }
  if (!preview) return;
  // Preview actions of the selected result.
  const DesktopEntry *e = selected_entry();
  if (!e) e = results_.front();
  int px = pad + lw + S(8), pw = area - lw - S(8);
  int ay = list_top_ + S(150);
  std::vector<std::pair<int, std::string>> actions = {{0, _("Open")}};
  if (is_file(e)) actions.push_back({1, _("Open file location")});
  if (is_app(e)) {
    actions.push_back({2, start_tiles::pinned(e->id) ? _("Unpin from Start") : _("Pin to Start")});
    actions.push_back(
        {3, Panel::instance->is_pinned(e->id) ? _("Unpin from panel") : _("Pin to panel")});
  }
  for (auto &a : actions) {
    Item it;
    it.kind = K_ACTION;
    it.index = a.first;
    it.label = a.second;
    it.entry = e;
    it.x = px + S(10);
    it.y = ay;
    it.w = pw - S(20);
    it.h = S(34);
    if (ay + it.h <= list_bottom_) items_.push_back(it);
    ay += S(36);
  }
}

const SearchFlyout::Item *SearchFlyout::item_at(int x, int y) const {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it)
    if (it->kind != K_NONE && it->kind != K_HEADING && it->contains(x, y)) return &*it;
  return nullptr;
}

const SearchFlyout::Item *SearchFlyout::selected_item() const {
  for (auto &it : items_)
    if (it.kind == sel_kind_ && it.index == sel_index_ && sel_kind_ != K_NONE) return &it;
  return nullptr;
}

const DesktopEntry *SearchFlyout::selected_entry() const {
  if (sel_kind_ == K_RESULT && sel_index_ >= 0 && sel_index_ < int(results_.size()))
    return results_[size_t(sel_index_)];
  return nullptr;
}

// ---------------------------------------------------------------- actions

void SearchFlyout::launch(const DesktopEntry *e) {
  if (!e) return;
  DesktopEntry copy = *e;
  if (is_app(&copy)) record_launch(copy.id);
  close();
  launch_entry(copy);
}

void SearchFlyout::activate(const Item &it) {
  switch (it.kind) {
    case K_TAB:
      tab_ = it.index;
      changed();
      return;
    case K_RESULT:
    case K_TOP:
    case K_RECENT:
    case K_CHIP: launch(it.entry); return;
    case K_ACTION: {
      const DesktopEntry *e = it.entry;
      if (!e) return;
      if (it.index == 0) {
        launch(e);
      } else if (it.index == 1) {
        std::string dir = e->path.substr(0, e->path.rfind('/'));
        close();
        open_uri(dir);
      } else if (it.index == 2) {
        if (start_tiles::pinned(e->id)) start_tiles::unpin(e->id);
        else start_tiles::pin(e->id);
        redraw();
      } else if (it.index == 3) {
        if (Panel::instance->is_pinned(e->id)) Panel::instance->unpin(e->id);
        else Panel::instance->pin(e->id);
        redraw();
      }
      return;
    }
    default: return;
  }
}

void SearchFlyout::item_menu(const Item &it, int px, int py) {
  const DesktopEntry *e = it.entry;
  if (!e || it.kind == K_ACTION) return;
  bool app = is_app(e), file = is_file(e);
  bool start = app && start_tiles::pinned(e->id), panel = app && Panel::instance->is_pinned(e->id);
  std::vector<Fl_Menu_Item> items;
  auto add = [&](const char *text, long code, int flags = 0) {
    items.push_back({text, 0, nullptr, reinterpret_cast<void *>(code), flags, 0, 0, 0, 0});
  };
  add(_("Open"), 1, (app || file) ? FL_MENU_DIVIDER : 0);
  if (file) add(_("Open file location"), 2);
  if (app) {
    add(start ? _("Unpin from Start") : _("Pin to Start"), 3);
    add(panel ? _("Unpin from panel") : _("Pin to panel"), 4);
  }
  add(nullptr, 0);
  menu_busy_ = true;
  const Fl_Menu_Item *m = items.data()->popup(px, py);
  menu_busy_ = false;
  if (!m) return;
  Item action = it;
  action.kind = K_ACTION;
  action.index = int(reinterpret_cast<long>(m->user_data())) - 1;
  activate(action);
}

void SearchFlyout::move(int dir) {
  build();
  std::vector<const Item *> order;
  for (auto &it : items_)
    if (it.kind == K_RESULT || it.kind == K_TOP || it.kind == K_RECENT || it.kind == K_CHIP)
      order.push_back(&it);
  // Results scrolled out of view still count.
  if (order.empty()) return;
  int cur = -1;
  for (size_t i = 0; i < order.size(); ++i)
    if (order[i]->kind == sel_kind_ && order[i]->index == sel_index_) cur = int(i);
  int next = cur < 0 ? (dir > 0 ? 0 : int(order.size()) - 1)
                     : std::max(0, std::min(int(order.size()) - 1, cur + dir));
  sel_kind_ = order[size_t(next)]->kind;
  sel_index_ = order[size_t(next)]->index;
  ensure_visible();
  redraw();
}

void SearchFlyout::ensure_visible() {
  build();
  const Item *it = selected_item();
  if (!it || it->kind != K_RESULT) return;
  // Keep the heading above the first result in view as well.
  int top = it->index == 0 ? it->y - S(26) : it->y;
  if (top < list_top_) scroll_ -= list_top_ - top;
  else if (it->y + it->h > list_bottom_) scroll_ += it->y + it->h - list_bottom_;
  scroll_ = std::max(0, scroll_);
}

// ---------------------------------------------------------------- events

int SearchFlyout::handle(int event) {
  switch (event) {
    case FL_PUSH: {
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (ex < 0 || ey < 0 || ex >= w() || ey >= h()) {
        // A click on the panel's own search box keeps the flyout open.
        if (Panel::instance->search_box_contains(Fl::event_x_root(), Fl::event_y_root()))
          return 1;
        return PanelPopup::handle(event);
      }
      build();
      const Item *hit = item_at(ex, ey);
      if (!hit) return 1;
      Item it = *hit;
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (it.kind == K_RESULT) {
          sel_kind_ = it.kind;
          sel_index_ = it.index;
          redraw();
        }
        item_menu(it, ex, ey);
        return 1;
      }
      if (it.kind == K_FIELD) {
        caret_ = query_.size();
        select_all_ = false;
        redraw();
        return 1;
      }
      activate(it);
      return 1;
    }
    case FL_MOVE: {
      build();
      const Item *hit = item_at(Fl::event_x(), Fl::event_y());
      int h = hit ? int(hit - items_.data()) : -1;
      if (h != hover_) {
        hover_ = h;
        // Hovering a result shows it in the preview.
        if (hit && hit->kind == K_RESULT) {
          sel_kind_ = K_RESULT;
          sel_index_ = hit->index;
        }
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      if (hover_ >= 0) {
        hover_ = -1;
        redraw();
      }
      return PanelPopup::handle(event);
    case FL_MOUSEWHEEL: {
      if (menu_busy_) return 1;
      build();
      int view_h = list_bottom_ - list_top_;
      if (content_h_ > view_h) {
        scroll_ = std::max(0, std::min(content_h_ - view_h, scroll_ + Fl::event_dy() * S(48)));
        hover_ = -1;
        redraw();
      }
      return 1;
    }
    case FL_PASTE: {
      std::string text = Fl::event_text() ? Fl::event_text() : "";
      type(text);
      return 1;
    }
    case FL_SHORTCUT:
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      bool ctrl = Fl::event_state() & FL_CTRL;
      caret_on_ = true;
      if (k == FL_Escape) {
        close();
        return 1;
      }
      if (k == FL_Enter || k == FL_KP_Enter) {
        build();
        const Item *sel = selected_item();
        if (sel) activate(*sel);
        else if (!results_.empty()) launch(results_.front());
        return 1;
      }
      if (k == FL_Down || k == FL_Up) {
        move(k == FL_Down ? 1 : -1);
        return 1;
      }
      if (k == FL_Page_Down || k == FL_Page_Up) {
        move(k == FL_Page_Down ? 6 : -6);
        return 1;
      }
      if (k == FL_Tab) {
        tab_ = (tab_ + ((Fl::event_state() & FL_SHIFT) ? 3 : 1)) % 4;
        changed();
        return 1;
      }
      if (k == FL_Menu) {
        build();
        const Item *sel = selected_item();
        if (sel) item_menu(*sel, sel->x + sel->w / 2, sel->y + sel->h / 2);
        return 1;
      }
      // Editing the query.
      auto prev_char = [&](size_t i) {
        if (!i) return size_t(0);
        --i;
        while (i > 0 && (query_[i] & 0xC0) == 0x80) --i;
        return i;
      };
      auto next_char = [&](size_t i) {
        if (i >= query_.size()) return query_.size();
        ++i;
        while (i < query_.size() && (query_[i] & 0xC0) == 0x80) ++i;
        return i;
      };
      if (ctrl && (k == 'a')) {
        select_all_ = !query_.empty();
        redraw();
        Panel::instance->search_changed();
        return 1;
      }
      if (ctrl && (k == 'v')) {
        Fl::paste(*this, 1);
        return 1;
      }
      if (ctrl && (k == 'u')) {
        query_.clear();
        caret_ = 0;
        select_all_ = false;
        changed();
        return 1;
      }
      if (k == FL_BackSpace || k == FL_Delete) {
        if (select_all_) {
          query_.clear();
          caret_ = 0;
          select_all_ = false;
        } else if (k == FL_BackSpace && caret_ > 0) {
          size_t from = prev_char(caret_);
          if (ctrl)  // a whole word
            while (from > 0 && query_[from - 1] != ' ') from = prev_char(from);
          query_.erase(from, caret_ - from);
          caret_ = from;
        } else if (k == FL_Delete && caret_ < query_.size()) {
          query_.erase(caret_, next_char(caret_) - caret_);
        } else {
          return 1;
        }
        changed();
        return 1;
      }
      if (k == FL_Left || k == FL_Right || k == FL_Home || k == FL_End) {
        select_all_ = false;
        if (k == FL_Left) caret_ = prev_char(caret_);
        else if (k == FL_Right) caret_ = next_char(caret_);
        else caret_ = k == FL_Home ? 0 : query_.size();
        redraw();
        Panel::instance->search_changed();
        return 1;
      }
      const char *text = Fl::event_text();
      if (!ctrl && text && *text && (unsigned char)text[0] >= 0x20 && text[0] != 0x7F) {
        type(text);
        return 1;
      }
      return 1;
    }
    default: return PanelPopup::handle(event);
  }
}

// ---------------------------------------------------------------- drawing

void SearchFlyout::draw() {
  const Theme &t = theme_;
  int W = w(), H = h(), pad = S(12);
  build();
  const Item *sel = selected_item();
  bool query = !trim(query_).empty();
  int area = W - 2 * pad;
  bool preview = query && !results_.empty() && area >= S(500);
  int lw = int(area * 0.56);
  const DesktopEntry *shown = query && !results_.empty()
                                  ? (selected_entry() ? selected_entry() : results_.front())
                                  : nullptr;
  {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface);
    p.stroke_rounded(0.5, 0.5, W - 1, H - 1, 0, t.border);
    p.line(pad, list_top_ - S(5) + 0.5, W - pad, list_top_ - S(5) + 0.5, t.border);
    if (preview)
      p.fill_rounded(pad + lw + S(8), list_top_, area - lw - S(8), list_bottom_ - list_top_,
                     S(8), t.surface_alt.alpha(t.dark ? 0.6 : 0.8));
    for (size_t i = 0; i < items_.size(); ++i) {
      const Item &it = items_[i];
      if (!it.visible) continue;
      bool hv = int(i) == hover_, s = sel == &it;
      cairo_save(p.cr());
      if (it.kind == K_RESULT) {
        cairo_rectangle(p.cr(), 0, list_top_, W, list_bottom_ - list_top_);
        cairo_clip(p.cr());
      }
      switch (it.kind) {
        case K_FIELD:
          draw_field_back(p, t, it.x, it.y, it.w, it.h, true, false, S(6));
          break;
        case K_TAB: {
          bool on = it.index == tab_;
          if (on) p.fill_rounded(it.x, it.y, it.w, it.h, it.h / 2.0, t.accent.alpha(t.dark ? 0.3 : 0.16));
          else if (hv) p.fill_rounded(it.x, it.y, it.w, it.h, it.h / 2.0, t.hover);
          if (on)
            p.fill_rounded(it.x + it.w / 2.0 - S(8), it.y + it.h - S(3), S(16), S(3), S(1.5),
                           t.accent);
          break;
        }
        case K_TOP:
        case K_RECENT:
        case K_CHIP:
        case K_RESULT:
        case K_ACTION: {
          double r = S(6);
          if (it.kind == K_ACTION) {
            p.fill_rounded(it.x, it.y, it.w, it.h, r, hv ? t.hover : t.surface.alpha(0.6));
            break;
          }
          if (s) p.fill_rounded(it.x, it.y, it.w, it.h, r, t.accent.alpha(t.dark ? 0.32 : 0.18));
          else if (hv) p.fill_rounded(it.x, it.y, it.w, it.h, r, t.hover);
          if (s && it.kind == K_RESULT)
            p.fill_rounded(it.x + S(2), it.y + it.h / 2.0 - S(8), S(3), S(16), S(1.5), t.accent);
          int is = it.kind == K_TOP ? S(36) : it.kind == K_RESULT && it.index == 0 ? S(36) : S(24);
          double ix = it.kind == K_TOP ? it.x + (it.w - is) / 2.0 : it.x + S(12);
          double iy = it.kind == K_TOP ? it.y + S(10) : it.y + (it.h - is) / 2.0;
          p.image(IconLoader::get({it.entry->icon, "application-x-executable"}, is), ix, iy, is);
          break;
        }
        default: break;
      }
      cairo_restore(p.cr());
    }
    if (preview && shown) {
      int px = pad + lw + S(8), pw = area - lw - S(8), is = S(64);
      p.image(IconLoader::get({shown->icon, "application-x-executable"}, is), px + (pw - is) / 2.0,
              list_top_ + S(18), is);
      p.line(px + S(16), list_top_ + S(140) + 0.5, px + pw - S(16), list_top_ + S(140) + 0.5,
             t.border);
    }
    // Scrollbar of the results.
    int view_h = list_bottom_ - list_top_;
    if (query && content_h_ > view_h) {
      double track = view_h - S(8), len = std::max(Sd(24), track * view_h / content_h_);
      double pos = (track - len) * scroll_ / std::max(1, content_h_ - view_h);
      double bx = pad + (preview ? lw : area) - Sd(4);
      p.fill_rounded(bx, list_top_ + S(4) + pos, Sd(4), len, Sd(2), t.text_dim.alpha(0.4));
    }
  }

  // Text.
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item &it = items_[i];
    if (!it.visible) continue;
    bool clip = it.kind == K_RESULT;
    if (clip) fl_push_clip(0, list_top_, W, list_bottom_ - list_top_);
    switch (it.kind) {
      case K_FIELD:
        draw_field_text(t, it.x, it.y, it.w, it.h, query_, caret_, caret_on_, true, select_all_,
                        _("Type here to search"));
        break;
      case K_TAB:
        text_font(t.font_size, it.index == tab_);
        draw_text(it.label, it.x, it.y, it.w, it.h - S(2), it.index == tab_ ? t.text : t.text_dim, 1);
        break;
      case K_HEADING:
        if (it.index == 1) {
          text_font(t.font_size + 2, true);
          draw_text(it.label, it.x, it.y, it.w, it.h, t.text, 1);
        } else if (it.index == 2) {
          text_font(t.font_size);
          draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim, 1);
        } else {
          text_font(t.font_size - 1, true);
          draw_text(it.label, it.x, it.y, it.w, it.h, t.text_dim);
        }
        break;
      case K_TOP:
        text_font(t.font_size - 1);
        draw_text(it.entry->name, it.x + S(4), it.y + S(50), it.w - S(8), S(28), t.text, 1);
        break;
      case K_RECENT:
      case K_CHIP:
        text_font(t.font_size);
        draw_text(it.entry->name, it.x + S(46), it.y, it.w - S(52), it.h, t.text);
        break;
      case K_RESULT: {
        const DesktopEntry *e = it.entry;
        std::string kind = is_file(e) ? (e->icon == "folder" ? _("Folder") : _("File"))
                           : is_extra(e) ? e->generic_name
                                         : _("App");
        std::string sub = is_file(e) ? e->generic_name : kind;
        int tx = it.x + (it.index == 0 ? S(58) : S(46)), tw = it.w - (tx - it.x) - S(8);
        if (it.index == 0) {
          text_font(t.font_size + 1, true);
          draw_text(e->name, tx, it.y + S(8), tw, S(22), t.text);
          text_font(t.font_size - 1);
          draw_text(sub, tx, it.y + S(30), tw, S(20), t.text_dim);
        } else if (is_file(e)) {
          text_font(t.font_size);
          draw_text(e->name, tx, it.y + S(2), tw, S(20), t.text);
          text_font(t.font_size - 2);
          draw_text(sub, tx, it.y + S(19), tw, S(17), t.text_dim);
        } else {
          text_font(t.font_size);
          draw_text(e->name, tx, it.y, tw, it.h, t.text);
        }
        break;
      }
      case K_ACTION:
        text_font(t.font_size);
        draw_text(it.label, it.x + S(12), it.y, it.w - S(24), it.h, t.text);
        break;
      default: break;
    }
    if (clip) fl_pop_clip();
  }
  if (preview && shown) {
    int px = pad + lw + S(8), pw = area - lw - S(8);
    text_font(t.font_size + 2, true);
    draw_text(shown->name, px + S(8), list_top_ + S(88), pw - S(16), S(26), t.text, 1);
    std::string kind = is_file(shown) ? (shown->icon == "folder" ? _("Folder") : _("File"))
                       : is_extra(shown) ? shown->generic_name
                                         : _("App");
    text_font(t.font_size - 1);
    draw_text(is_file(shown) ? shown->path : kind, px + S(8), list_top_ + S(112), pw - S(16), S(20),
              t.text_dim, 1);
  }
  if (qa_dump_enabled()) qa_dump_search();
}

// UI test hook (see qa_dump in popup.h): the flyout's own controls, plus
// "query:<text>" and "selected:<name>" lines.
void SearchFlyout::qa_dump_search() {
  std::string text;
  auto line = [&](const std::string &name, int x, int y, int w, int h, int scrolled) {
    std::string n = name;
    for (char &c : n)
      if (c == '\t' || c == '\n') c = ' ';
    text += n + "\t" + std::to_string(x) + "\t" + std::to_string(y) + "\t" + std::to_string(w) +
            "\t" + std::to_string(h) + "\t1\t" + std::to_string(scrolled) + "\n";
  };
  line("@search", 0, 0, w(), h(), 0);
  line("query:" + query_, int(caret_), caret_on_ ? 1 : 0, 0, 0, 0);
  if (const Item *s = selected_item())
    if (s->entry) line("selected:" + s->entry->name, s->x, s->y, s->w, s->h, 0);
  for (auto &it : items_) {
    if (!it.visible) continue;
    std::string name;
    switch (it.kind) {
      case K_FIELD: name = "field"; break;
      case K_TAB: name = "tab:" + it.label; break;
      case K_RESULT: name = "result:" + it.entry->name; break;
      case K_TOP: name = "top:" + it.entry->name; break;
      case K_RECENT: name = "recent:" + it.entry->name; break;
      case K_CHIP: name = "chip:" + it.entry->name; break;
      case K_ACTION: name = "action:" + it.label; break;
      default: continue;
    }
    int cy = it.y + it.h / 2;
    if (it.kind == K_RESULT && (cy < list_top_ || cy >= list_bottom_)) continue;
    line(name, it.x, it.y, it.w, it.h, it.kind == K_RESULT ? 1 : 0);
  }
  qa_dump(this, text);
}
