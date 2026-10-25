// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "window.h"

#include <FL/Fl.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <map>

#include "build_config.h"
#include "buffered_window.h"
#include "i18n.h"
#include "icons.h"
#include "image_cache.h"
#include "store.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "wm.h"
#include "../settings/ui.h"

namespace sde {
namespace merkadu {

namespace {

class MainWindow;
class Sidebar;

MainWindow *g_win = nullptr;
Sidebar *g_sidebar = nullptr;
Fl_Scroll *g_sidebar_scroll = nullptr;
Page *g_page = nullptr;
DrawnButton *g_back = nullptr;
SearchField *g_search = nullptr;
View g_view;
View g_built;  // the view of g_page
std::vector<std::pair<View, int>> g_back_stack;  // view and how far it was scrolled
std::map<std::string, Source> g_chosen;
// Named parts of the page for the QA dump, where they were built (page
// content, unscrolled): name, x, y, w, h.
struct QaPart {
  std::string name;
  int x, y, w, h;
};
std::vector<QaPart> g_qa_lines;
int g_content_x = 0;
bool g_rebuild_pending = false;

int topbar_h() { return S(64); }
int sidebar_w(int win_w) { return win_w < S(760) ? S(176) : S(220); }

// ------------------------------------------------------------- sidebar

struct NavItem {
  View view;
  std::string title;
  Symbol symbol = Symbol::Explore;
  std::string icon;     // departments: a theme icon instead of a symbol
  std::string caption;  // a caption above it ("DEPARTMENTS")
  std::string count;    // a small count on the right
};

std::vector<NavItem> nav_items() {
  Store &st = Store::get();
  std::vector<NavItem> out;
  auto add = [&](View::Kind k, const std::string &title, Symbol s, const std::string &count = "") {
    NavItem n;
    n.view.kind = k;
    n.title = title;
    n.symbol = s;
    n.count = count;
    out.push_back(n);
  };
  add(View::Explore, _("Explore"), Symbol::Explore);
  add(View::Installed, _("Installed"), Symbol::Installed);
  size_t updates = st.updatable_apps().size();
  add(View::Updates, _("Updates"), Symbol::Updates, updates ? std::to_string(updates) : "");
  add(View::History, _("History"), Symbol::History);
  int active = st.active_jobs();
  if (!st.jobs().empty())
    add(View::Progress, _("In Progress"), Symbol::Progress, active ? std::to_string(active) : "");
  add(View::Sources, _("Sources"), Symbol::Sources);
  bool first = true;
  for (const Category *c : st.categories().sorted()) {
    NavItem n;
    n.view.kind = View::Category;
    n.view.arg = c->id;
    n.title = c->title();
    n.icon = c->icon;
    if (first) n.caption = _("DEPARTMENTS");
    first = false;
    out.push_back(n);
  }
  return out;
}

bool nav_selected(const NavItem &n) {
  if (n.view.kind == View::Category && g_view.kind == View::Category)
    return n.view.arg == g_view.arg.substr(0, g_view.arg.find('/'));
  return n.view.kind == g_view.kind && n.view.kind != View::Category;
}

class Sidebar : public Fl_Widget {
 public:
  Sidebar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int row_h() const { return S(32); }
  int top() const { return y() + S(80); }
  // The rows' y positions (captions make room above them).
  std::vector<int> rows(const std::vector<NavItem> &items) const {
    std::vector<int> ys;
    int cy = top();
    for (auto &n : items) {
      if (!n.caption.empty()) cy += S(30);
      ys.push_back(cy);
      cy += row_h();
    }
    return ys;
  }
  int content_h() const {
    auto items = nav_items();
    auto ys = rows(items);
    return (ys.empty() ? top() : ys.back() + row_h()) - y() + S(24);
  }
  void draw() override {
    const Theme &t = active_theme();
    auto items = nav_items();
    auto ys = rows(items);
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), t.surface);
      p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), t.border);
      p.image(IconLoader::get({"merkadu", "system-software-install", "sde-logo"}, S(36)), x() + S(20), y() + S(20),
              S(36));
      for (size_t i = 0; i < items.size(); ++i) {
        int ry = ys[i];
        bool cur = nav_selected(items[i]);
        if (cur) {
          p.fill_rounded(x() + S(10), ry, w() - S(20), row_h() - S(4), S(9), t.accent.alpha(0.15));
          p.fill_rounded(x() + S(10), ry + S(10), Sd(3), std::max(S(6), row_h() - S(24)), Sd(1.5), t.accent);
        } else if (int(i) == hover_) {
          p.fill_rounded(x() + S(10), ry, w() - S(20), row_h() - S(4), S(9), t.hover);
        }
        if (Fl::focus() == this && cur) focus_ring(p, x() + S(10), ry, w() - S(20), row_h() - S(4), S(9));
        int is = S(20), iy = ry + (row_h() - S(4) - is) / 2;
        if (!items[i].icon.empty())
          p.image(IconLoader::get({items[i].icon, "applications-other"}, is), x() + S(24), iy, is);
        else
          paint_symbol(p, items[i].symbol, x() + S(24), iy, is, t.accent_text);
        if (!items[i].count.empty()) {
          text_font(std::max(S(9), t.font_size - S(2)), true);
          int cw = std::max(S(22), text_width(items[i].count) + S(12));
          p.fill_rounded(x() + w() - S(18) - cw, ry + (row_h() - S(4) - S(20)) / 2.0, cw, S(20), S(10),
                         items[i].view.kind == View::Updates ? t.accent : t.border);
        }
      }
    }
    std::string title = _("Merkadu");
    text_font(t.font_size + 2, true);
    draw_text(title, x() + S(64), y() + S(22), w() - S(74), S(32), t.text);
    for (size_t i = 0; i < items.size(); ++i) {
      int ry = ys[i];
      if (!items[i].caption.empty()) {
        text_font(t.font_size - 2, true);
        draw_text(items[i].caption, x() + S(22), ry - S(26), w() - S(30), S(20), t.text_dim);
      }
      bool cur = nav_selected(items[i]);
      int cw = 0;
      if (!items[i].count.empty()) {
        text_font(std::max(S(9), t.font_size - S(2)), true);
        cw = std::max(S(22), text_width(items[i].count) + S(12));
        draw_text(items[i].count, x() + w() - S(18) - cw, ry + (row_h() - S(4) - S(20)) / 2, cw, S(20),
                  items[i].view.kind == View::Updates ? t.on_accent : t.text, 1);
      }
      text_font(t.font_size, cur);
      int tw = w() - S(66) - (cw ? cw + S(8) : 0);
      if (text_width(items[i].title) > tw) text_font(std::max(S(9), t.font_size - S(2)), cur);
      draw_text(items[i].title, x() + S(56), ry, tw, row_h() - S(4), cur ? t.accent_text : t.text);
    }
  }
  int index_at(int ey) const {
    auto items = nav_items();
    auto ys = rows(items);
    for (size_t i = 0; i < ys.size(); ++i)
      if (ey >= ys[i] && ey < ys[i] + row_h() - S(4)) return int(i);
    return -1;
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH: {
        if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
        int i = index_at(Fl::event_y());
        if (i >= 0) navigate(nav_items()[size_t(i)].view);
        return 1;
      }
      case FL_MOVE:
      case FL_ENTER: {
        int hv = index_at(Fl::event_y());
        if (hv != hover_) {
          hover_ = hv;
          redraw();
        }
        return 1;
      }
      case FL_LEAVE:
        hover_ = -1;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD: {
        auto items = nav_items();
        int cur = -1;
        for (size_t i = 0; i < items.size(); ++i)
          if (nav_selected(items[i])) cur = int(i);
        int k = Fl::event_key(), n = int(items.size());
        if (k == FL_Up && cur > 0)
          navigate(items[size_t(cur - 1)].view);
        else if (k == FL_Down && cur + 1 < n)
          navigate(items[size_t(cur + 1)].view);
        else
          return 0;
        take_focus();
        return 1;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }
  // The window-relative boxes of the rows, for the QA dump.
  std::vector<std::pair<std::string, std::vector<int>>> qa_rows() const {
    std::vector<std::pair<std::string, std::vector<int>>> out;
    auto items = nav_items();
    auto ys = rows(items);
    for (size_t i = 0; i < items.size(); ++i) {
      std::string id = std::string("nav:") + view_id(items[i].view.kind);
      if (items[i].view.kind == View::Category) id = "nav:category:" + items[i].view.arg;
      out.push_back({id, {x() + S(10), ys[i], w() - S(20), row_h() - S(4)}});
    }
    return out;
  }

 private:
  int hover_ = -1;
};

// --------------------------------------------------------------- window

void qa_collect(Fl_Widget *w, const Fl_Widget *view, std::string &out) {
  if (!w->visible()) return;
  const char *name = w->tooltip() ? w->tooltip() : w->label();
  if (name && *name && w != w->window()) {
    std::string n = name;
    for (char &c : n)
      if (c == '\t' || c == '\n') c = ' ';
    bool shown = !view || (w->y() >= view->y() && w->y() + w->h() <= view->y() + view->h() &&
                           w->x() >= view->x() && w->x() + w->w() <= view->x() + view->w());
    out += n + "\t" + std::to_string(w->x()) + "\t" + std::to_string(w->y()) + "\t" + std::to_string(w->w()) +
           "\t" + std::to_string(w->h()) + "\t" + std::to_string(w->active_r() && shown ? 1 : 0) + "\t" +
           (view ? "1" : "0") + "\n";
  }
  if (auto *g = w->as_group()) {
    const Fl_Widget *inner = dynamic_cast<Fl_Scroll *>(w) ? w : view;
    for (int i = 0; i < g->children(); ++i) qa_collect(g->child(i), inner, out);
  }
}

void qa_dump();
int scrolled();

class MainWindow : public BufferedWindow {
 public:
  MainWindow(int w, int h) : BufferedWindow(w, h, _("Merkadu")) {}
  void draw() override {
    const Theme &t = active_theme();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
      p.line(g_content_x, topbar_h() - 0.5, w(), topbar_h() - 0.5, t.border.alpha(0.6));
    }
    // The page title in the bar.
    int tx = g_content_x + (g_back && g_back->visible() ? S(72) : S(28));
    int tw = (g_search ? g_search->frame->x() : w()) - tx - S(16);
    text_font(t.font_size + 5, true);
    draw_text(view_title(g_view), tx, 0, tw, topbar_h(), t.text);
    draw_children();
    qa_dump();
  }
  void resize(int x, int y, int w, int h) override {
    bool changed = w != this->w() || h != this->h();
    BufferedWindow::resize(x, y, w, h);
    if (changed) layout();
  }
  void layout();
  int handle(int e) override {
    if (e == FL_KEYBOARD || e == FL_SHORTCUT) {
      int k = Fl::event_key();
      bool ctrl = Fl::event_state() & FL_CTRL, alt = Fl::event_state() & FL_ALT;
      if (ctrl && (k == 'f' || k == 'k')) {
        if (g_search) g_search->take_focus();
        return 1;
      }
      if (ctrl && k == 'q') {
        hide();
        return 1;
      }
      if ((alt && k == FL_Left) || (k == FL_BackSpace && Fl::focus() != g_search) ||
          (k == FL_Escape && Fl::focus() != g_search)) {
        if (!g_back_stack.empty()) {
          go_back();
          return 1;
        }
        if (k == FL_Escape) return 1;  // Escape does not close the window
      }
      if (k == FL_F + 5) {
        Store::get().reload_all();
        Store::get().probe_sources();
        return 1;
      }
    }
    int r = BufferedWindow::handle(e);
    if (!r && handle_about_event(e, about_info())) return 1;
    return r;
  }
};

void MainWindow::layout() {
  int W = w(), H = h();
  int side = sidebar_w(W);
  g_content_x = side;
  if (g_sidebar_scroll) {
    g_sidebar_scroll->resize(0, 0, side, H);
    g_sidebar->resize(0, 0, side - S(10), std::max(H, g_sidebar->content_h()));
  }
  int cw = W - side;
  int search_w = std::max(S(170), std::min(S(340), int(cw * 0.42)));
  int sh = S(38), sy = (topbar_h() - sh) / 2;
  if (g_search) {
    g_search->frame->resize(W - S(24) - search_w, sy, search_w, sh);
    g_search->resize(W - S(24) - search_w + S(36), sy + S(4), search_w - S(50), sh - S(8));
  }
  if (g_back) g_back->resize(side + S(20), (topbar_h() - S(36)) / 2, S(40), S(36));
  rebuild_page();
}

void qa_dump() {
  const char *dir = getenv("SDE_PANEL_QA_DIR");
  if (!dir || !*dir || !g_win) return;
  std::string text = std::string("@page:") + view_id(g_view.kind) + (g_view.arg.empty() ? "" : ":" + g_view.arg) +
                     "\t0\t0\t0\t0\t1\t0\n";
  text += "@state:catalog=" + std::string(Store::get().catalog_loaded() ? "1" : "0") +
          ",apps=" + std::to_string(Store::get().catalog().size()) + ",jobs=" +
          std::to_string(Store::get().active_jobs()) + "\t0\t0\t0\t0\t1\t0\n";
  // Where the parts are now: the page may be scrolled.
  int dy = scrolled();
  for (const QaPart &q : g_qa_lines) {
    int y = q.y - dy;
    bool shown = g_page && y >= g_page->y() && y + q.h <= g_page->y() + g_page->h();
    text += q.name + "\t" + std::to_string(q.x) + "\t" + std::to_string(y) + "\t" + std::to_string(q.w) + "\t" +
            std::to_string(q.h) + "\t" + (shown ? "1" : "0") + "\t1\n";
  }
  if (g_sidebar && g_sidebar_scroll)
    for (auto &row : g_sidebar->qa_rows()) {
      const auto &b = row.second;
      bool shown = b[1] >= g_sidebar_scroll->y() && b[1] + b[3] <= g_sidebar_scroll->y() + g_sidebar_scroll->h();
      text += row.first + "\t" + std::to_string(b[0]) + "\t" + std::to_string(b[1]) + "\t" + std::to_string(b[2]) +
              "\t" + std::to_string(b[3]) + "\t" + (shown ? "1" : "0") + "\t1\n";
    }
  for (int i = 0; i < g_win->children(); ++i)
    if (g_win->child(i) != g_sidebar_scroll) qa_collect(g_win->child(i), nullptr, text);
  std::string path = std::string(dir) + "/Merkadu.txt", tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}

// Where the page's content starts unscrolled (below its padding), and how
// far it is scrolled now. Measured on the children, not with yposition():
// Fl_Scroll only counts that from the content's top once it has drawn, and
// a page rebuilt before its first draw would scroll by the padding.
int g_page_top0 = 0;

int content_top() {
  int top = INT_MAX;
  for (int i = 0; i < g_page->children(); ++i) {
    Fl_Widget *c = g_page->child(i);
    if (c == &g_page->scrollbar || c == &g_page->hscrollbar) continue;
    top = std::min(top, c->y() - g_page->y());
  }
  return top == INT_MAX ? g_page_top0 : top;
}

int scrolled() { return g_page ? std::max(0, g_page_top0 - content_top()) : 0; }

// Scrolls the page down by |amount| (as far as its content allows).
void restore_scroll(int amount) {
  if (!g_page || amount <= 0) return;
  int top = INT_MAX, bottom = INT_MIN;
  for (int i = 0; i < g_page->children(); ++i) {
    Fl_Widget *c = g_page->child(i);
    if (c == &g_page->scrollbar || c == &g_page->hscrollbar) continue;
    top = std::min(top, c->y());
    bottom = std::max(bottom, c->y() + c->h());
  }
  if (top == INT_MAX) return;
  int target = std::min(amount, bottom - top + g_page_top0 - g_page->h());
  int now = scrolled();
  if (target > now) g_page->scroll_to(0, g_page->yposition() + (target - now));
}

void do_rebuild(void *) {
  g_rebuild_pending = false;
  if (!g_win) return;
  // The same view keeps its place; a new one starts at the top.
  int scroll = g_page && g_built == g_view ? scrolled() : 0;
  if (g_page) {
    g_win->remove(g_page);
    Fl::delete_widget(g_page);
    g_page = nullptr;
  }
  g_win->begin();
  g_qa_lines.clear();
  g_page = new Page(g_content_x, topbar_h(), g_win->w() - g_content_x, g_win->h() - topbar_h());
  // A new Fl_Scroll starts with both scrollbars "visible" until its first
  // draw finds out; drawing around a horizontal one that never shows left
  // a strip of stale text at the bottom of the page.
  g_page->hscrollbar.clear_visible();
  // Up to four columns of tiles on wide windows; the content is centred
  // beyond that.
  g_page->max_width(S(1100));
  build_page(g_page, g_view);
  g_built = g_view;
  g_win->end();
  g_page_top0 = 0;
  g_page_top0 = content_top();
  restore_scroll(scroll);
  if (g_back) {
    if (g_back_stack.empty())
      g_back->hide();
    else
      g_back->show();
  }
  if (g_sidebar) {
    g_sidebar->size(g_sidebar->w(), std::max(g_sidebar_scroll->h(), g_sidebar->content_h()));
    g_sidebar_scroll->redraw();
  }
  g_win->redraw();
}

void schedule_rebuild() {
  if (g_rebuild_pending) return;
  g_rebuild_pending = true;
  Fl::add_timeout(0.0, do_rebuild);
}

void on_store_event(Event e) {
  if (!g_win) return;
  switch (e) {
    case Event::Progress:
      // Widgets read the jobs when they draw: a repaint is enough.
      if (g_page) g_page->redraw();
      if (g_sidebar) g_sidebar->redraw();
      break;
    case Event::Sources:
      if (g_view.kind == View::Sources || g_view.kind == View::Details) schedule_rebuild();
      break;
    case Event::History:
      if (g_view.kind == View::History) schedule_rebuild();
      break;
    case Event::Search:
      if (g_view.kind == View::Search || g_view.kind == View::Category || g_view.kind == View::Details)
        schedule_rebuild();
      break;
    default:
      schedule_rebuild();
      if (g_sidebar) g_sidebar->redraw();
      break;
  }
}

std::string g_store_query;

void store_search(void *) {
  if (g_search && trim(g_search->value()) == g_store_query) Store::get().search_store(g_store_query);
}

void search_changed(Fl_Widget *, void *) {
  std::string q = trim(g_search->value());
  if (q.empty()) {
    if (g_view.kind == View::Search) go_back();
    return;
  }
  if (g_view.kind == View::Search) {
    g_view.arg = q;  // new results start at the top
    schedule_rebuild();
  } else {
    navigate(View{View::Search, q});
  }
  // The Snap Store is searched once typing pauses.
  g_store_query = q;
  Fl::remove_timeout(store_search);
  Fl::add_timeout(0.5, store_search);
}

}  // namespace

const char *view_id(View::Kind k) {
  switch (k) {
    case View::Category: return "category";
    case View::Search: return "search";
    case View::Details: return "details";
    case View::Installed: return "installed";
    case View::Updates: return "updates";
    case View::History: return "history";
    case View::Progress: return "progress";
    case View::Sources: return "sources";
    default: return "explore";
  }
}

bool view_from_id(const std::string &id, View::Kind *out) {
  for (int k = View::Explore; k <= View::Sources; ++k)
    if (id == view_id(View::Kind(k))) {
      *out = View::Kind(k);
      return true;
    }
  return false;
}

const View &current_view() { return g_view; }

void navigate(const View &v) {
  if (v == g_view) return;
  if (g_page) g_back_stack.push_back({g_view, scrolled()});
  if (g_back_stack.size() > 40) g_back_stack.erase(g_back_stack.begin());
  g_view = v;
  if (v.kind != View::Search && g_search && *g_search->value()) g_search->value("");
  schedule_rebuild();
  if (g_sidebar) g_sidebar->redraw();
}

void go_back() {
  if (g_back_stack.empty()) return;
  auto prev = g_back_stack.back();
  g_back_stack.pop_back();
  g_view = prev.first;
  if (g_search) g_search->value(g_view.kind == View::Search ? g_view.arg.c_str() : "");
  do_rebuild(nullptr);
  if (g_page) restore_scroll(prev.second);
  if (g_sidebar) g_sidebar->redraw();
}

void rebuild_page() { schedule_rebuild(); }

void set_search_text(const std::string &text) {
  if (g_search) g_search->value(text.c_str());
}

Source chosen_source(const std::string &app_id) {
  const App *a = Store::get().app(app_id);
  auto it = g_chosen.find(app_id);
  if (a && it != g_chosen.end() && a->has_source(it->second)) return it->second;
  if (a && a->main_origin()) return a->main_origin()->source;
  return Source::Debian;
}

void choose_source(const std::string &app_id, Source s) {
  g_chosen[app_id] = s;
  schedule_rebuild();
}

void qa_line(const std::string &name, int x, int y, int w, int h) {
  if (!getenv("SDE_PANEL_QA_DIR")) return;
  g_qa_lines.push_back({name, x, y, w, h});
}

AppInfo about_info() {
  AppInfo info;
  info.name = _("Merkadu");
  info.program = "merkadu";
  info.version = SDE_VERSION;
  info.icon = "merkadu";
  info.description = _("Find, install, update and remove software from the Debian archive, Flatpak and Snap.");
  info.credits = {toolkit_credit(),
                  _("Departments, lobby and history after Ubuntu Software Center"),
                  _("Software catalog read with libappstream; Flatpak with libflatpak"),
                  _("Debian packages installed with PackageKit; snaps with snapd")};
  return info;
}

namespace {

void confirm_and_install(const std::string &app_id, Source s) {
  if (!Store::get().install(app_id, s))
    fl_alert("%s", _("This app cannot be installed from this source now."));
  else if (current_view().kind == View::Details)
    rebuild_page();
}

}  // namespace

void request_install(const std::string &app_id, Source s) {
  Store &st = Store::get();
  const App *a = st.app(app_id);
  if (!a) return;
  const Origin *o = a->origin(s);
  if (!o) return;
  if (s == Source::Snap && o->classic) {
    int c = fl_choice(_("%s is a snap with classic confinement: unlike other snaps it is not isolated from "
                        "the system and can read and change all your files.\n\nInstall it anyway?"),
                      _("Cancel"), _("Install"), nullptr, a->name.c_str());
    if (c != 1) return;
  }
  // Snap data goes to ~/.snap/data instead of ~/snap (see docs/MERKADU.md).
  if (s == Source::Snap && st.settings().hidden_snap_folder && st.status(Source::Snap).hidden_snap_folder == 0) {
    std::string id = app_id;
    st.backend(Source::Snap)->setup("hide-snap-folder", [id, s](bool ok, bool, std::string error) {
      Store::get().probe_sources();
      if (!ok)
        fprintf(stderr, "merkadu: could not enable snapd's hidden snap folder: %s\n", error.c_str());
      confirm_and_install(id, s);
    });
    return;
  }
  confirm_and_install(app_id, s);
}

void request_remove(const std::string &app_id, Source s) {
  Store &st = Store::get();
  const App *a = st.app(app_id);
  if (!a) return;
  const Origin *o = nullptr;
  for (auto &x : a->origins)
    if (x.source == s && x.installed) o = &x;
  if (!o) return;
  std::string name = a->name, pkg = o->id;
  if (s != Source::Debian) {
    if (fl_choice(_("Remove %s?\n\nIt is removed for every user of this computer."), _("Cancel"), _("Remove"),
                  nullptr, name.c_str()) == 1)
      st.remove(app_id, s);
    return;
  }
  std::string id = app_id;
  st.backend(Source::Debian)->plan_remove({pkg}, [id, name, pkg](std::vector<std::string> removed, long long freed,
                                                                 std::string error) {
    Store &st2 = Store::get();
    RemovalPlan plan = plan_removal(pkg, removed, st2.settings().protected_packages);
    if (!error.empty()) {
      fl_alert(_("%s cannot be removed:\n\n%s"), name.c_str(), error.c_str());
      return;
    }
    if (!plan.allowed) {
      if (plan.protected_package == pkg)
        fl_alert(_("%s is part of the Selviana Desktop Environment (package %s) and cannot be removed on its own."),
                 name.c_str(), pkg.c_str());
      else
        fl_alert(_("Removing %s would also remove %s, which the desktop needs. It cannot be removed."),
                 name.c_str(), plan.protected_package.c_str());
      return;
    }
    std::string text = format(_("Remove %s?"), name.c_str());
    if (freed > 0) text += "\n" + format(_("This frees %s of disk space."), format_size(freed).c_str());
    if (!plan.also_removed.empty()) {
      text += "\n\n" + std::string(_("These packages are removed with it:")) + "\n";
      size_t shown = std::min<size_t>(plan.also_removed.size(), 12);
      for (size_t i = 0; i < shown; ++i) text += plan.also_removed[i] + (i + 1 < shown ? ", " : "");
      if (plan.also_removed.size() > shown)
        text += format(_(" and %d more"), int(plan.also_removed.size() - shown));
    }
    if (fl_choice("%s", _("Cancel"), _("Remove"), nullptr, text.c_str()) == 1) st2.remove(id, Source::Debian);
  });
}

int run_window(const View &start) {
  Store &st = Store::get();
  // Centred in the work area with room for the title bar (fit_to_work_area).
  int wx = 0, wy = 0, W = S(1100), H = S(720);
  fit_to_work_area(wx, wy, W, H);
  auto *win = new MainWindow(W, H);
  g_win = win;
  int side = sidebar_w(W);
  g_content_x = side;
  const Theme &t = active_theme();
  g_sidebar_scroll = new Fl_Scroll(0, 0, side, H);
  g_sidebar_scroll->type(Fl_Scroll::VERTICAL);
  g_sidebar_scroll->box(FL_FLAT_BOX);
  g_sidebar_scroll->color(t.surface.fl());
  g_sidebar_scroll->scrollbar_size(S(10));
  g_sidebar_scroll->scrollbar.color(t.surface.fl());
  g_sidebar_scroll->scrollbar.selection_color(t.border.fl());
  g_sidebar = new Sidebar(0, 0, side - S(10), H);
  g_sidebar_scroll->end();
  g_back = new DrawnButton(side + S(20), (topbar_h() - S(36)) / 2, S(40), S(36), [](Painter &p, bool hot, bool down) {
    const Theme &th = active_theme();
    if (hot || down)
      p.fill_rounded(g_back->x(), g_back->y(), g_back->w(), g_back->h(), th.radius_for(g_back->h()),
                     down ? th.hover.mix(th.text, 0.1) : th.hover);
    double s = S(20);
    paint_symbol(p, Symbol::Back, g_back->x() + (g_back->w() - s) / 2, g_back->y() + (g_back->h() - s) / 2, s,
                 th.text);
  }, nullptr, -1);
  g_back->tooltip(_("Back"));
  g_back->callback([](Fl_Widget *, void *) { go_back(); });
  g_back->hide();
  g_search = make_search_field(W - S(24) - S(340), (topbar_h() - S(38)) / 2, S(340), S(38),
                               _("Search apps"));
  g_search->callback(search_changed);
  g_search->on_enter = [] {
    std::string q = trim(g_search->value());
    if (!q.empty()) Store::get().search_store(q);
  };
  g_search->on_escape = [] { go_back(); };
  win->end();
  win->size_range(std::min(W, S(640)), std::min(H, S(440)));
  win->resizable(win);
  win->xclass("merkadu");
  g_view = start;
  if (start.kind == View::Search) g_search->value(start.arg.c_str());
  st.listen(on_store_event);
  images::on_ready([] {
    for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w)) w->redraw();
  });
  win->layout();
  do_rebuild(nullptr);
  win->position(wx, wy);
  win->show();
  return Fl::run();
}

}  // namespace merkadu
}  // namespace sde
