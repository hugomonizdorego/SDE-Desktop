// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>

#include "icons.h"
#include "paint.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

namespace {

const char *kAll = "All applications";

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
          k == FL_Page_Up || k == FL_Page_Down)
        return parent()->handle(FL_KEYBOARD);
    }
    return Fl_Input::handle(e);
  }
};

}  // namespace

AppMenu::AppMenu() : sde::BufferedWindow(0, 0, 640, 580, "SDE Menu") {
  search_ = new SearchInput(0, 0, 10, 10);
  search_->box(FL_FLAT_BOX);
  search_->when(FL_WHEN_CHANGED);
  search_->callback(search_changed, this);
  end();
  set_override();
  border(0);
  callback([](Fl_Widget *w, void *) { static_cast<AppMenu *>(w)->close(); });
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
  if (selected_ >= int(rows_.size())) selected_ = int(rows_.size()) - 1;
  if (selected_ < 0) selected_ = 0;
}

void AppMenu::popup(const Theme &theme, int anchor_x, int panel_y, int screen_x, int screen_w) {
  theme_ = theme;
  int mh = std::min(580, panel_y - 24);
  int mw = std::min(640, screen_w - 16);
  int mx = std::max(screen_x + 8, std::min(anchor_x, screen_x + screen_w - mw - 8));
  int my = panel_y - mh - 8;
  resize(mx, my, mw, mh);
  search_->resize(pad_ + 42, pad_ + 6, mw - 2 * pad_ - 54, 30);
  search_->color(theme_.surface_alt.fl());
  search_->textcolor(theme_.text.fl());
  search_->cursor_color(theme_.accent.fl());
  search_->selection_color(theme_.accent.fl());
  search_->textsize(theme_.font_size + 1);
  search_->textfont(FL_HELVETICA);
  search_->value("");
  category_ = 0;
  selected_ = 0;
  scroll_ = 0;
  reload();
  filter();
  show();
  Fl::grab(this);
  search_->take_focus();
  redraw();
}

void AppMenu::close() {
  if (Fl::grab() == this) Fl::grab(nullptr);
  hide();
}

int AppMenu::list_rows_visible() const { return std::max(1, (h() - header_h_ - footer_h_) / row_h_); }

void AppMenu::ensure_visible(int row) {
  int vis = list_rows_visible();
  if (row < scroll_) scroll_ = row;
  if (row >= scroll_ + vis) scroll_ = row - vis + 1;
  if (scroll_ < 0) scroll_ = 0;
}

AppMenu::Zone AppMenu::zone_at(int x, int y, int &index) const {
  index = -1;
  int body_top = header_h_, body_bottom = h() - footer_h_;
  if (y >= body_top && y < body_bottom) {
    if (x >= pad_ && x < pad_ + cat_w_ - 8) {
      int i = (y - body_top - 4) / cat_row_h_;
      if (i >= 0 && i < int(categories_.size())) {
        index = i;
        return Z_CATEGORY;
      }
    } else if (x >= pad_ + cat_w_ && x < w() - pad_) {
      int i = scroll_ + (y - body_top) / row_h_;
      if (i >= 0 && i < int(rows_.size())) {
        index = i;
        return Z_APP;
      }
    }
  }
  if (y >= h() - footer_h_ + 10 && y < h() - 10) {
    if (x >= w() - pad_ - 40 && x < w() - pad_) return Z_POWER;
    if (x >= w() - pad_ - 88 && x < w() - pad_ - 48) return Z_SETTINGS;
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
      if (ex < 0 || ey < 0 || ex >= w() || ey >= h()) {
        close();
        return 1;
      }
      int idx;
      Zone z = zone_at(ex, ey, idx);
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
      return sde::BufferedWindow::handle(event);
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
      sde::BufferedWindow::handle(event);
      return 1;
    }
    case FL_MOUSEWHEEL: {
      int max_scroll = std::max(0, int(rows_.size()) - list_rows_visible());
      scroll_ = std::max(0, std::min(max_scroll, scroll_ + Fl::event_dy() * 3));
      redraw();
      return 1;
    }
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      int n = int(rows_.size());
      if (k == FL_Escape) {
        close();
        return 1;
      }
      if (k == FL_Enter || k == FL_KP_Enter) {
        launch(selected_);
        return 1;
      }
      if (n == 0) return 1;
      if (k == FL_Down) selected_ = std::min(n - 1, selected_ + 1);
      else if (k == FL_Up) selected_ = std::max(0, selected_ - 1);
      else if (k == FL_Page_Down) selected_ = std::min(n - 1, selected_ + list_rows_visible());
      else if (k == FL_Page_Up) selected_ = std::max(0, selected_ - list_rows_visible());
      else return sde::BufferedWindow::handle(event);
      ensure_visible(selected_);
      redraw();
      return 1;
    }
    default:
      return sde::BufferedWindow::handle(event);
  }
}

void AppMenu::draw() {
  const Theme &t = theme_;
  int W = w(), H = h();
  {
    Painter p(W, H);
    p.fill_rect(0, 0, W, H, t.surface);
    p.stroke_rounded(0, 0, W, H, 0, t.border);

    // Search box.
    p.fill_rounded(pad_, pad_, W - 2 * pad_, 42, 12, t.surface_alt);
    p.image(IconLoader::get({"system-search", "edit-find"}, 18), pad_ + 14, pad_ + 12, 18, 0.7);

    // Categories.
    int top = header_h_;
    for (size_t i = 0; i < categories_.size(); ++i) {
      int y = top + 4 + int(i) * cat_row_h_;
      if (y + cat_row_h_ > H - footer_h_) break;
      bool sel = int(i) == category_ && !*search_->value();
      bool hov = hover_zone_ == Z_CATEGORY && hover_index_ == int(i);
      if (sel) p.fill_rounded(pad_, y, cat_w_ - 12, cat_row_h_ - 4, 8, t.accent.alpha(0.16));
      else if (hov) p.fill_rounded(pad_, y, cat_w_ - 12, cat_row_h_ - 4, 8, t.hover);
      if (sel) p.fill_rounded(pad_, y + 8, 3, cat_row_h_ - 20, 1.5, t.accent);
      p.image(IconLoader::get({category_icon(categories_[i]), "applications-other"}, 20),
              pad_ + 12, y + (cat_row_h_ - 4 - 20) / 2.0, 20);
    }
    p.line(pad_ + cat_w_ - 4, top + 6, pad_ + cat_w_ - 4, H - footer_h_ - 6, t.border);

    // Application rows.
    int lx = pad_ + cat_w_ + 4, lw = W - pad_ - lx;
    cairo_save(p.cr());
    cairo_rectangle(p.cr(), lx, top, lw, H - footer_h_ - top);
    cairo_clip(p.cr());
    for (int i = scroll_; i < int(rows_.size()); ++i) {
      int y = top + (i - scroll_) * row_h_;
      if (y > H - footer_h_) break;
      if (i == selected_) p.fill_rounded(lx, y + 2, lw, row_h_ - 4, 10, t.accent.alpha(0.14));
      const DesktopEntry *e = rows_[size_t(i)].entry;
      p.image(IconLoader::get({e->icon}, 32), lx + 10, y + (row_h_ - 32) / 2.0, 32);
    }
    cairo_restore(p.cr());

    // Scroll indicator.
    int vis = list_rows_visible();
    if (int(rows_.size()) > vis) {
      double track = H - footer_h_ - top - 8;
      double len = std::max(24.0, track * vis / rows_.size());
      double pos = (track - len) * scroll_ / std::max(1, int(rows_.size()) - vis);
      p.fill_rounded(W - pad_ + 6, top + 4 + pos, 4, len, 2, t.text_dim.alpha(0.35));
    }

    // Footer.
    p.line(pad_, H - footer_h_ + 0.5, W - pad_, H - footer_h_ + 0.5, t.border);
    int cy = H - footer_h_ / 2;
    p.circle(pad_ + 18, cy, 18, t.accent);
    bool hs = hover_zone_ == Z_SETTINGS, hp = hover_zone_ == Z_POWER;
    if (hs) p.fill_rounded(W - pad_ - 88, cy - 20, 40, 40, 10, t.hover);
    if (hp) p.fill_rounded(W - pad_ - 40, cy - 20, 40, 40, 10, t.hover);
    p.image(IconLoader::get({"preferences-system", "preferences-desktop", "configure"}, 22),
            W - pad_ - 79, cy - 11, 22);
    p.image(IconLoader::get({"system-shutdown", "system-log-out", "application-exit"}, 22),
            W - pad_ - 31, cy - 11, 22);
  }

  // Children (the search input) and text.
  draw_children();
  int top = header_h_;
  if (!*search_->value()) {
    text_font(t.font_size + 1);
    fl_color(t.text_dim.fl());
    draw_text("Search applications", search_->x() + 3, search_->y(), search_->w(), search_->h(),
              t.text_dim);
  }
  for (size_t i = 0; i < categories_.size(); ++i) {
    int y = top + 4 + int(i) * cat_row_h_;
    if (y + cat_row_h_ > H - footer_h_) break;
    bool sel = int(i) == category_ && !*search_->value();
    text_font(t.font_size, sel);
    draw_text(categories_[i], pad_ + 42, y, cat_w_ - 60, cat_row_h_ - 4, sel ? t.accent : t.text);
  }
  int lx = pad_ + cat_w_ + 4, lw = W - pad_ - lx;
  fl_push_clip(lx, top, lw, H - footer_h_ - top);
  for (int i = scroll_; i < int(rows_.size()); ++i) {
    int y = top + (i - scroll_) * row_h_;
    if (y > H - footer_h_) break;
    const DesktopEntry *e = rows_[size_t(i)].entry;
    std::string sub = !e->generic_name.empty() && e->generic_name != e->name ? e->generic_name
                                                                             : e->comment;
    text_font(t.font_size + 1);
    if (sub.empty()) {
      draw_text(e->name, lx + 54, y, lw - 64, row_h_, t.text);
    } else {
      draw_text(e->name, lx + 54, y + 4, lw - 64, row_h_ / 2 - 2, t.text);
      text_font(t.font_size - 1);
      draw_text(sub, lx + 54, y + row_h_ / 2, lw - 64, row_h_ / 2 - 6, t.text_dim);
    }
  }
  if (rows_.empty()) {
    text_font(t.font_size + 1);
    draw_text("No applications found", lx, top + 20, lw, 40, t.text_dim, 1);
  }
  fl_pop_clip();

  // Footer text.
  int cy = H - footer_h_ / 2;
  std::string name = user_name();
  std::string initial = name.empty() ? "?" : name.substr(0, 1);
  if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] = char(initial[0] - 'a' + 'A');
  text_font(t.font_size + 3, true);
  draw_text(initial, pad_, cy - 18, 36, 36, t.on_accent, 1);
  text_font(t.font_size + 1, true);
  draw_text(name, pad_ + 46, cy - 12, W / 2, 24, t.text);
}
