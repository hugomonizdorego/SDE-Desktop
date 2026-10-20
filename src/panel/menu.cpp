// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <sys/stat.h>

#include "i18n.h"
#include "icons.h"
#include "panel.h"
#include "paint.h"
#include "spawn.h"
#include "util.h"
#include "suite.h"
#include "build_config.h"

using namespace sde;

namespace {

const char *kAll = N_("All applications");

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

// Search field that hands navigation keys to the menu.
class SearchInput : public Fl_Input {
 public:
  SearchInput(int x, int y, int w, int h) : Fl_Input(x, y, w, h) {}
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key();
      if (k == FL_Up || k == FL_Down || k == FL_Enter || k == FL_KP_Enter || k == FL_Escape ||
          k == FL_Page_Up || k == FL_Page_Down || ((!*value()) && (k == FL_Left || k == FL_Right)))
        return parent()->handle(FL_KEYBOARD);
    }
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

AppMenu::~AppMenu() { Fl::remove_timeout(poll_applications, this); }

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
  self->scroll_ = std::min(self->scroll_, std::max(0, int(self->rows_.size()) - self->list_rows_visible()));
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
    std::vector<std::pair<int, const DesktopEntry *>> scored;
    for (auto &a : apps_) {
      int s = match_score(a, terms);
      if (s) scored.push_back({s, &a});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](auto &a, auto &b) { return a.first > b.first; });
    for (auto &s : scored) rows_.push_back({s.second});
  }
  row_h_ = S(grid_mode() ? 72 : 48);
  if (selected_ >= int(rows_.size())) selected_ = int(rows_.size()) - 1;
  if (selected_ < 0) selected_ = 0;
}

void AppMenu::layout() {
  pad_ = S(16);
  header_h_ = S(112);
  footer_h_ = S(60);
  cat_w_ = S(176);
  row_h_ = S(46);
  cat_row_h_ = S(36);
  icon_ = S(32);
}

void AppMenu::prepare() {
  theme_ = Panel::instance->theme();
  layout();
  int mx, my, mw, mh;
  Panel::instance->monitor(mx, my, mw, mh);
  int mw2 = std::min(S(760), mw - S(16));
  int mh2 = std::min(S(650), mh - Panel::instance->h() - S(24));
  // Narrow screens: give the list the room and shrink the categories.
  if (mw2 < S(560)) cat_w_ = S(150);
  size(mw2, mh2);
  search_->resize(pad_ + S(42), S(64), mw2 - 2 * pad_ - S(54), S(30));
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
  reload();
  filter();
  Fl::remove_timeout(poll_applications, this);
  Fl::add_timeout(2, poll_applications, this);
  Fl::add_timeout(0, [](void *d) { static_cast<AppMenu *>(d)->search_->take_focus(); }, this);
}

void AppMenu::item_menu(int row) {
  if (row < 0 || row >= int(rows_.size())) return;
  DesktopEntry e = *rows_[size_t(row)].entry;
  bool pinned = Panel::instance->is_pinned(e.id);
  Fl_Menu_Item items[] = {
      {_("Open"), 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
      {pinned ? _("Unpin from Panel") : _("Pin to Panel"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
      {_("Add to Desktop"), 0, nullptr, nullptr, 0, 0, 0, 0, 0},
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
  }
}

bool AppMenu::grid_mode() const { return false; }
int AppMenu::columns() const { return grid_mode() ? 2 : 1; }
int AppMenu::list_rows_visible() const { return std::max(1, (h() - header_h_ - footer_h_) / row_h_) * columns(); }

void AppMenu::ensure_visible(int row) {
  int vis = list_rows_visible();
  if (row < scroll_) scroll_ = row;
  if (row >= scroll_ + vis) scroll_ = ((row / columns()) - (vis / columns()) + 1) * columns();
  if (scroll_ < 0) scroll_ = 0;
  scroll_ = (scroll_ / columns()) * columns();
}

AppMenu::Zone AppMenu::zone_at(int x, int y, int &index) const {
  index = -1;
  int body_top = header_h_, body_bottom = h() - footer_h_;
  if (y >= body_top && y < body_bottom) {
    if (x >= pad_ && x < pad_ + cat_w_ - S(8)) {
      int i = category_scroll_ + (y - body_top - S(4)) / cat_row_h_;
      if (i >= 0 && i < int(categories_.size())) {
        index = i;
        return Z_CATEGORY;
      }
    } else if (x >= pad_ + cat_w_ && x < w() - pad_) {
      int lw = w() - 2 * pad_ - cat_w_ - S(4);
      int column = std::min(columns() - 1, (x - pad_ - cat_w_ - S(4)) / std::max(1, lw / columns()));
      int i = scroll_ + ((y - body_top) / row_h_) * columns() + column;
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
  close();
  launch_entry(e);
}

int AppMenu::handle(int event) {
  switch (event) {
    case FL_PUSH: {
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (ex < 0 || ey < 0 || ex >= w() || ey >= h()) return PanelPopup::handle(event);
      int idx;
      Zone z = zone_at(ex, ey, idx);
      if (z == Z_APP && Fl::event_button() == FL_RIGHT_MOUSE) {
        selected_ = idx;
        redraw();
        item_menu(idx);
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
        close(); spawn_detached({"liria", "--no-desktop", home_dir()}); return 1;
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
      return PanelPopup::handle(event);
    }
    case FL_MOVE:
    case FL_DRAG: {
      int idx;
      Zone z = zone_at(Fl::event_x(), Fl::event_y(), idx);
      if (z != hover_zone_ || idx != hover_index_) {
        hover_zone_ = z;
        hover_index_ = idx;
        if (z == Z_APP) selected_ = idx;
        redraw();
      }
      PanelPopup::handle(event);
      return 1;
    }
    case FL_MOUSEWHEEL: {
      int max_scroll = std::max(0, ((int(rows_.size()) + columns() - 1) / columns()) * columns() - list_rows_visible());
      if (Fl::event_x() < pad_ + cat_w_) {
        int visible = std::max(1, (h() - header_h_ - footer_h_ - S(4)) / cat_row_h_);
        category_scroll_ = std::max(0, std::min(std::max(0, int(categories_.size()) - visible), category_scroll_ + Fl::event_dy()));
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
      if (!navigation) return search_->Fl_Input::handle(FL_KEYBOARD);
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
  {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface);
    p.stroke_rounded(0.5, 0.5, W - 1, H - 1, 0, t.border);

    // Search box.
    p.fill_rect(0, 0, W, S(48), t.accent.alpha(.10));
    p.fill_rounded(pad_, S(58), W - 2 * pad_, S(42), S(6), t.surface_alt);
    if (Fl::focus() == search_) p.stroke_rounded(pad_, S(58), W - 2 * pad_, S(42), S(6), t.accent, Sd(1.5));
    p.image(IconLoader::get({"system-search", "edit-find"}, S(18)), pad_ + S(14), S(70), S(18), 0.7);

    // Categories.
    int top = header_h_;
    for (size_t i = size_t(category_scroll_); i < categories_.size(); ++i) {
      int y = top + S(4) + (int(i) - category_scroll_) * cat_row_h_;
      if (y + cat_row_h_ > H - footer_h_) break;
      bool sel = int(i) == category_ && !*search_->value();
      bool hov = hover_zone_ == Z_CATEGORY && hover_index_ == int(i);
      if (sel) p.fill_rounded(pad_, y, cat_w_ - S(12), cat_row_h_ - S(4), S(8), t.accent.alpha(0.16));
      else if (hov) p.fill_rounded(pad_, y, cat_w_ - S(12), cat_row_h_ - S(4), S(8), t.hover);
      if (sel) p.fill_rounded(pad_, y + S(8), Sd(3), cat_row_h_ - S(20), Sd(1.5), t.accent);
      p.image(IconLoader::get({category_icon(categories_[i]), "applications-other"}, cat_icon),
              pad_ + S(12), y + (cat_row_h_ - S(4) - cat_icon) / 2.0, cat_icon);
    }
    p.line(pad_ + cat_w_ - S(4), top + S(6), pad_ + cat_w_ - S(4), H - footer_h_ - S(6), t.border);

    // Application rows.
    int lx = pad_ + cat_w_ + S(4), lw = W - pad_ - lx;
    cairo_save(p.cr());
    cairo_rectangle(p.cr(), lx, top, lw, H - footer_h_ - top);
    cairo_clip(p.cr());
    for (int i = scroll_; i < int(rows_.size()); ++i) {
      int y = top + ((i - scroll_) / columns()) * row_h_;
      int tile_w = lw / columns(), tile_x = lx + ((i - scroll_) % columns()) * tile_w;
      if (y > H - footer_h_) break;
      if (i == selected_) p.fill_rounded(tile_x, y + S(2), tile_w - S(6), row_h_ - S(6), S(6), t.accent.alpha(0.18));
      else if (grid_mode()) p.fill_rounded(tile_x, y + S(2), tile_w - S(6), row_h_ - S(6), S(6), t.surface_alt);
      const DesktopEntry *e = rows_[size_t(i)].entry;
      p.image(IconLoader::get({e->icon}, icon_), tile_x + S(10), y + (row_h_ - icon_) / 2.0, icon_);
    }
    cairo_restore(p.cr());

    // Scroll indicator.
    int vis = list_rows_visible();
    if (int(rows_.size()) > vis) {
      double track = H - footer_h_ - top - S(8);
      double len = std::max(Sd(24), track * vis / rows_.size());
      double pos = (track - len) * scroll_ / std::max(1, int(rows_.size()) - vis);
      p.fill_rounded(W - pad_ + S(6), top + S(4) + pos, Sd(4), len, Sd(2), t.text_dim.alpha(0.35));
    }

    // Footer.
    p.line(pad_, H - footer_h_ + 0.5, W - pad_, H - footer_h_ + 0.5, t.border);
    int cy = H - footer_h_ / 2;
    p.circle(pad_ + S(18), cy, S(18), t.accent);
    if (hover_zone_ == Z_FILES) p.fill_rounded(W - pad_ - S(136), cy - btn / 2, btn, btn, S(6), t.hover);
    p.image(IconLoader::get({"user-home", "system-file-manager"}, gi), W - pad_ - S(136) + (btn - gi) / 2.0, cy - gi / 2.0, gi);
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
  draw_text("Alpha", W - pad_ - S(80), S(8), S(80), S(32), t.text_dim, 2);
  int top = header_h_;
  if (!*search_->value()) {
    text_font(t.font_size + 1);
    draw_text(_("Search applications"), search_->x() + S(3), search_->y(), search_->w(), search_->h(),
              t.text_dim);
  }
  for (size_t i = size_t(category_scroll_); i < categories_.size(); ++i) {
    int y = top + S(4) + (int(i) - category_scroll_) * cat_row_h_;
    if (y + cat_row_h_ > H - footer_h_) break;
    bool sel = int(i) == category_ && !*search_->value();
    text_font(t.font_size, sel);
    draw_text(_(categories_[i].c_str()), pad_ + S(42), y, cat_w_ - S(60), cat_row_h_ - S(4),
              sel ? t.accent_text : t.text);
  }
  int lx = pad_ + cat_w_ + S(4), lw = W - pad_ - lx;
  fl_push_clip(lx, top, lw, H - footer_h_ - top);
  int tx = lx + S(22) + icon_;
  for (int i = scroll_; i < int(rows_.size()); ++i) {
    int y = top + ((i - scroll_) / columns()) * row_h_;
      int tile_w = lw / columns(), tile_x = lx + ((i - scroll_) % columns()) * tile_w;
    if (y > H - footer_h_) break;
    const DesktopEntry *e = rows_[size_t(i)].entry;
    std::string sub = !e->generic_name.empty() && e->generic_name != e->name ? e->generic_name
                                                                             : e->comment;
    text_font(t.font_size + 1);
    if (sub.empty()) {
      draw_text(e->name, tile_x + S(22) + icon_, y, tile_w - (tx - lx) - S(12), row_h_, t.text);
    } else {
      draw_text(e->name, tile_x + S(22) + icon_, y + S(4), tile_w - (tx - lx) - S(12), row_h_ / 2 - S(2), t.text);
      text_font(t.font_size - 1);
      draw_text(sub, tile_x + S(22) + icon_, y + row_h_ / 2, tile_w - (tx - lx) - S(12), row_h_ / 2 - S(6), t.text_dim);
    }
  }
  if (rows_.empty()) {
    text_font(t.font_size + 1);
    draw_text(_("No applications found"), lx, top + S(20), lw, S(40), t.text_dim, 1);
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
