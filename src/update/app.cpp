// SPDX-License-Identifier: GPL-3.0-or-later
// SDE Update: native window over the mintupdate-derived APT model and
// Debian PackageKit transactions. All package work runs in helper processes
// (data/update/*.py) so the window never blocks.
#include "app.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/fl_draw.H>
#include <signal.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <set>

#include "about.h"
#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "model.h"
#include "paint.h"
#include "process.h"
#include "spawn.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {
namespace update {

std::string helper_dir() {
  std::string dir = env("SDE_UPDATE_HELPERS");
  return dir.empty() ? std::string(SDE_DATADIR) + "/update" : dir;
}

std::string helper(const std::string &name) { return join_path(helper_dir(), name); }

namespace {

const char *kPython = "/usr/bin/python3";

enum PageId { P_UPDATES, P_HISTORY, P_SOURCES, P_MAINTENANCE, P_SETTINGS, P_ABOUT, P_COUNT };

struct PageInfo {
  const char *id;
  const char *title;
  Icon icon;
};

const PageInfo kPages[P_COUNT] = {
    {"updates", N_("Updates"), Icon::Updates},
    {"history", N_("History"), Icon::History},
    {"sources", N_("Sources"), Icon::Sources},
    {"maintenance", N_("Maintenance"), Icon::Maintenance},
    {"settings", N_("Settings"), Icon::Settings},
    {"about", N_("About"), Icon::About},
};

// Last time the package lists were downloaded, by anyone.
std::time_t lists_mtime() {
  std::time_t newest = 0;
  const std::string dir = "/var/lib/apt/lists";
  for (auto &name : list_dir(dir)) {
    if (name.find("Release") == std::string::npos) continue;
    struct stat st;
    if (stat(join_path(dir, name).c_str(), &st) == 0) newest = std::max(newest, st.st_mtime);
  }
  if (!newest) {
    struct stat st;
    if (stat("/var/cache/apt/pkgcache.bin", &st) == 0) newest = st.st_mtime;
  }
  return newest;
}

bool reboot_required(std::vector<std::string> *packages) {
  packages->clear();
  if (!file_exists("/run/reboot-required")) return false;
  std::string text;
  if (read_file("/run/reboot-required.pkgs", text))
    for (auto &line : split(text, '\n'))
      if (!trim(line).empty() &&
          std::find(packages->begin(), packages->end(), trim(line)) == packages->end())
        packages->push_back(trim(line));
  return true;
}

void cache_usage(int *files, long long *bytes) {
  *files = 0;
  *bytes = 0;
  const std::string dir = "/var/cache/apt/archives";
  for (auto &name : list_dir(dir)) {
    if (!ends_with(name, ".deb")) continue;
    struct stat st;
    if (stat(join_path(dir, name).c_str(), &st) == 0) {
      ++*files;
      *bytes += st.st_size;
    }
  }
}

std::string first_program(const std::vector<std::string> &names) {
  for (auto &n : names)
    if (!find_program(n).empty()) return n;
  return "";
}

std::string join(const std::vector<std::string> &items, const std::string &sep) {
  std::string out;
  for (auto &i : items) {
    if (!out.empty()) out += sep;
    out += i;
  }
  return out;
}

std::string action_title(const std::string &action) {
  if (action == "refresh") return _("Refreshing package information");
  if (action == "upgrade") return _("Installing updates");
  if (action == "autoremove") return _("Removing unused packages");
  if (action == "clean") return _("Cleaning the package cache");
  if (action == "repair") return _("Repairing interrupted installations");
  return action;
}

void draw_wrapped(const std::string &text, int x, int y, int w, int h, const Color &c) {
  fl_color(c.fl());
  fl_push_clip(x, y, w, h);
  fl_draw(text.c_str(), x, y, w, h, Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE));
  fl_pop_clip();
}

int wrapped_height(const std::string &text, int w) {
  int ww = w, hh = 0;
  fl_measure(text.c_str(), ww, hh, 0);
  return hh;
}

// ------------------------------------------------------------------ state

struct Row {
  bool header = false;
  Kind kind = Kind::Software;
  int index = -1;  // into App::list.updates
};

class Page;
class UpdatesPage;
class HistoryPage;
class SourcesPage;
class MaintenancePage;
class SettingsPage;
class AboutPage;
class ActivityStrip;
class Sidebar;

class App {
 public:
  Config cfg;
  Fl_Window *window = nullptr;
  Sidebar *sidebar = nullptr;
  Page *pages[P_COUNT] = {};
  ActivityStrip *strip = nullptr;
  int page = P_UPDATES;

  // Updates.
  UpdateList list;
  bool loading = false, loaded_once = false;
  std::string load_error;
  std::time_t refreshed = 0;
  int filter = -1;  // -1 = all, else Kind
  std::string query;
  std::vector<Row> rows;
  bool reboot = false;
  std::vector<std::string> reboot_packages;

  // Changelogs, cached by package.
  std::map<std::string, std::string> changelogs;
  std::string changelog_pending;
  int changelog_request = 0;

  // History and sources.
  std::vector<HistoryEntry> history;
  bool history_loaded = false, history_loading = false;
  std::vector<Source> sources;
  bool sources_loaded = false, sources_loading = false;

  // Maintenance.
  int cache_files = 0;
  long long cache_bytes = 0;
  std::vector<std::string> autoremove;
  bool autoremove_known = false, autoremove_loading = false;

  // Transactions.
  Progress progress;
  int job = 0;
  bool strip_visible = false;
  double phase = 0;
  std::string packagekit_client;  // pkcon, or "" (APT through pkexec)

  void show_page(int index);
  void layout();
  void relayout_pages();
  bool busy() const { return job != 0; }
  void changed();  // redraw everything that shows update state

  void load_updates();
  void rebuild_rows();
  void select_changelog(int index);
  void load_history(bool force = false);
  void load_sources(bool force = false);
  void load_maintenance();
  void start(const std::string &action, const std::vector<std::string> &packages = {});
  void cancel();
  void finished();
  void install_selected();
  void open_terminal_transaction();
  void maybe_auto_refresh();
  void request_close();

  static void tick(void *);
};

App &app() {
  static App a;
  return a;
}

// ------------------------------------------------------------------ pages

class Page : public Fl_Group {
 public:
  Page() : Fl_Group(0, 0, 100, 100) { box(FL_NO_BOX); }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    layout();
  }
  virtual void layout() {}
  virtual void activated() {}
  void draw() override {
    paint();
    draw_children();
  }
  virtual void paint() {}

 protected:
  int pad() const { return w() < S(620) ? S(16) : S(28); }
  // Title and subtitle at the top of the page; returns the y below them.
  int heading_bottom() const { return y() + S(86); }
  void heading(const std::string &title, const std::string &subtitle) {
    const Theme &t = active_theme();
    text_font(t.font_size + S(8), true);
    draw_text(title, x() + pad(), y() + S(18), w() - 2 * pad(), S(36), t.text);
    text_font(t.font_size);
    draw_text(subtitle, x() + pad(), y() + S(54), w() - 2 * pad(), S(22), t.text_dim);
  }
};

// Filter chips: All, Security, Kernel, Software, Backports with counts.
class Chips : public Fl_Widget {
 public:
  Chips(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  std::function<void(int)> changed;
  struct Item {
    int id;
    std::string label;
    int x = 0, w = 0;
  };
  std::vector<Item> items;
  int value = -1;
  void set_counts(const UpdateList &list) {
    items.clear();
    items.push_back({-1, format("%s  %d", _("All"), int(list.updates.size()))});
    for (int k = 0; k < kKindCount; ++k) {
      int n = list.count(Kind(k));
      static const char *names[] = {N_("Security"), N_("Kernel"), N_("Software"), N_("Backports")};
      if (n) items.push_back({k, format("%s  %d", gettext(names[k]), n)});
    }
    bool found = false;
    for (auto &i : items) found |= i.id == value;
    if (!found) value = -1;
    redraw();
  }
  void measure() {
    const Theme &t = active_theme();
    int cx = x();
    for (auto &i : items) {
      text_font(t.font_size - S(1), true);
      i.w = text_width(i.label) + (i.id >= 0 ? S(28) : S(20));
      i.x = cx;
      cx += i.w + S(4);
    }
  }
  void draw() override {
    const Theme &t = active_theme();
    measure();
    {
      Painter p(window()->w(), window()->h());
      for (auto &i : items) {
        if (i.x + i.w > x() + w()) break;
        bool on = i.id == value, hot = i.id == hover_;
        Color bg = on ? t.accent : hot ? t.hover : t.surface_alt;
        if (look().liquid() && !on) bg = bg.alpha(0.75);
        p.fill_rounded(i.x, y(), i.w, h(), h() / 2.0, bg);
        if (!on) p.stroke_rounded(i.x, y(), i.w, h(), h() / 2.0, t.border);
        if (i.id >= 0 && !on)
          p.circle(i.x + S(11), y() + h() / 2.0, S(3.5), kind_color(Kind(i.id)));
      }
      if (Fl::focus() == this)
        for (auto &i : items)
          if (i.id == value) focus_ring(p, i.x, y(), i.w, h(), h() / 2.0);
    }
    for (auto &i : items) {
      if (i.x + i.w > x() + w()) break;
      bool on = i.id == value;
      text_font(t.font_size - S(1), on);
      if (i.id >= 0 && !on)
        draw_text(i.label, i.x + S(19), y(), i.w - S(21), h(), t.text);
      else
        draw_text(i.label, i.x, y(), i.w, h(), on ? t.on_accent : t.text, 1);
    }
  }
  int at(int ex) const {
    for (auto &i : items)
      if (ex >= i.x && ex < i.x + i.w) return i.id;
    return -2;
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH: {
        int id = at(Fl::event_x());
        if (id != -2) choose(id);
        return 1;
      }
      case FL_ENTER:
      case FL_MOVE: {
        int id = at(Fl::event_x());
        if (id != hover_) {
          hover_ = id;
          redraw();
        }
        return 1;
      }
      case FL_LEAVE:
        hover_ = -2;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k != FL_Left && k != FL_Right) return 0;
        for (size_t i = 0; i < items.size(); ++i)
          if (items[i].id == value) {
            int j = int(i) + (k == FL_Right ? 1 : -1);
            if (j >= 0 && j < int(items.size())) choose(items[size_t(j)].id);
            break;
          }
        return 1;
      }
    }
    return Fl_Widget::handle(e);
  }
  void choose(int id) {
    value = id;
    redraw();
    if (changed) changed(id);
  }

 private:
  int hover_ = -2;
};

// Details of the selected update: versions, size, origin and changelog.
class DetailsPane : public Fl_Group {
 public:
  DetailsPane(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    box(FL_NO_BOX);
    text = new TextView(x, y, w, h);
    end();
  }
  int index = -1;
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    layout();
  }
  void layout() {
    int top = y() + info_height();
    int room = y() + h() - top - S(14);
    text->resize(x() + S(14), top, w() - S(28), std::max(S(40), room));
    // The changelog needs a few lines to be useful.
    if (index < 0 || room < S(60)) text->hide();
    else text->show();
  }
  void show_update(int i) {
    index = i;
    refresh_text();
    layout();
    redraw();
  }
  void refresh_text() {
    App &a = app();
    if (index < 0 || index >= int(a.list.updates.size())) {
      text->set("");
      return;
    }
    const Update &u = a.list.updates[size_t(index)];
    auto it = a.changelogs.find(u.main_package);
    if (it != a.changelogs.end()) text->set(it->second);
    else text->set(_("Loading the changelog…"));
  }
  const Update *current() const {
    App &a = app();
    if (index < 0 || index >= int(a.list.updates.size())) return nullptr;
    return &a.list.updates[size_t(index)];
  }
  std::vector<std::pair<std::string, std::string>> facts(const Update &u) const {
    std::vector<std::pair<std::string, std::string>> f;
    f.push_back({_("Installed"), u.old_version.empty() ? _("Not installed") : u.old_version});
    f.push_back({_("Available"), u.new_version});
    f.push_back({_("Download"), format_size(u.size)});
    f.push_back({format(_("Packages (%d)"), int(u.packages.size())), join(u.packages, ", ")});
    std::string origin = u.origin;
    if (!u.archive.empty()) origin += (origin.empty() ? "" : " · ") + u.archive;
    if (!u.site.empty()) origin += (origin.empty() ? "" : " · ") + u.site;
    if (!origin.empty()) f.push_back({_("Origin"), origin});
    return f;
  }
  int description_height() const {
    const Update *u = current();
    if (!u || u->description.empty()) return 0;
    text_font(active_theme().font_size - S(1));
    return std::min(S(66), wrapped_height(u->description, w() - S(36)));
  }
  int info_height() const {
    const Update *u = current();
    if (!u) return h();
    return S(96) + description_height() + S(8) + int(facts(*u).size()) * S(24) + S(44);
  }
  void draw() override {
    fl_push_clip(x(), y(), w(), h());
    draw_inner();
    fl_pop_clip();
  }
  void draw_inner() {
    const Theme &t = active_theme();
    const Update *u = current();
    int px = x() + S(18), pw = w() - S(36);
    {
      Painter p(window()->w(), window()->h());
      paint_card(p, x(), y(), w(), h());
      if (!u) {
        paint_icon(p, Icon::Package, x() + w() / 2.0 - S(20), y() + h() / 2.0 - S(50), S(40),
                   t.text_dim.alpha(0.7));
      } else {
        p.fill_rounded(px, y() + S(18), S(40), S(40), look().liquid() ? S(20) : S(10),
                       kind_color(u->kind).alpha(0.14));
        paint_icon(p, u->kind == Kind::Security ? Icon::Shield : Icon::Package, px + S(8),
                   y() + S(26), S(24), kind_text(u->kind));
        int bx = px;
        std::vector<std::pair<std::string, Color>> badges = {
            {kind_badge(u->kind), kind_color(u->kind)}};
        if (!u->archive.empty()) badges.push_back({u->archive, t.text_dim});
        for (auto &b : badges) {
          int bw = badge_width(b.first);
          paint_badge(p, bx, y() + S(68), bw, S(20), b.second);
          bx += bw + S(6);
        }
        int fy = y() + S(96) + description_height() + S(8);
        for (size_t i = 0; i < facts(*u).size(); ++i)
          if (i) p.line(px, fy + i * S(24) + 0.5, px + pw, fy + i * S(24) + 0.5, t.border.alpha(0.6));
        if (text->visible()) {
          int ty = text->y();
          p.line(px, ty - S(36) + 0.5, px + pw, ty - S(36) + 0.5, t.border);
          p.fill_rect(text->x(), text->y(), text->w(), text->h(), t.surface);
        }
      }
    }
    if (!u) {
      text_font(t.font_size);
      draw_text(_("Select an update to see its details."), x() + S(16), y() + h() / 2 + S(2),
                w() - S(32), S(24), t.text_dim, 1);
      return;
    }
    text_font(t.font_size + S(3), true);
    draw_text(u->name, px + S(52), y() + S(16), pw - S(52), S(24), t.text);
    text_font(t.font_size - S(1));
    draw_text(u->summary.empty() ? u->main_package : u->summary, px + S(52), y() + S(40),
              pw - S(52), S(20), t.text_dim);
    int bx = px;
    std::vector<std::pair<std::string, Color>> badges = {{kind_badge(u->kind), kind_color(u->kind)}};
    if (!u->archive.empty()) badges.push_back({u->archive, t.text_dim});
    for (auto &b : badges) {
      int bw = badge_width(b.first);
      draw_badge_text(b.first, bx, y() + S(68), bw, S(20), b.second);
      bx += bw + S(6);
    }
    int dh = description_height();
    if (dh) {
      text_font(t.font_size - S(1));
      draw_wrapped(u->description, px, y() + S(96), pw, dh, t.text);
    }
    int fy = y() + S(96) + dh + S(8);
    auto f = facts(*u);
    int lw = std::min(S(130), pw / 3);
    for (size_t i = 0; i < f.size(); ++i) {
      text_font(t.font_size - S(1));
      draw_text(f[i].first, px, fy + int(i) * S(24), lw, S(24), t.text_dim);
      draw_text(f[i].second, px + lw, fy + int(i) * S(24), pw - lw, S(24), t.text);
    }
    if (!text->visible()) return;
    text_font(t.font_size, true);
    int ty = text->y();
    draw_text(_("Changelog"), px, ty - S(32), pw / 2, S(26), t.text);
    App &a = app();
    std::string state;
    if (a.changelog_pending == u->main_package) state = _("Loading…");
    else if (a.changelogs.count(u->main_package)) state = u->main_package;
    text_font(t.font_size - S(2));
    draw_text(state, px + pw / 2, ty - S(32), pw / 2, S(26), t.text_dim, 2);
    draw_children();
  }
  TextView *text;
};

class UpdatesPage : public Page {
 public:
  UpdatesPage() {
    refresh = new Button(0, 0, 10, 10, _("Refresh"));
    refresh->icon(Icon::Restart);
    refresh->callback([](Fl_Widget *, void *) { app().start("refresh"); });
    install = new Button(0, 0, 10, 10, _("Install Updates"), Button::Primary);
    install->icon(Icon::Download);
    install->callback([](Fl_Widget *, void *) { app().install_selected(); });
    restart = new Button(0, 0, 10, 10, _("Restart…"));
    restart->callback([](Fl_Widget *, void *) {
      if (!find_program("sde-logout").empty()) spawn_detached({"sde-logout"});
      else if (confirm(app().window, _("Restart the computer?"),
                       _("Save your work first. Open applications will be closed."), "",
                       _("Restart"), true))
        spawn_detached({"systemctl", "reboot"});
    });
    search = new SearchField(0, 0, 10, 10, _("Search updates"));
    search->callback(
        [](Fl_Widget *w, void *) {
          app().query = static_cast<SearchField *>(w)->value();
          app().rebuild_rows();
        },
        nullptr);
    chips = new Chips(0, 0, 10, 10);
    chips->changed = [](int id) {
      app().filter = id;
      app().rebuild_rows();
    };
    all = new Button(0, 0, 10, 10, _("Select All"), Button::Ghost);
    all->callback([](Fl_Widget *, void *) { select_all(!all_selected()); });
    none = new Button(0, 0, 10, 10, _("Select None"), Button::Ghost);
    none->callback([](Fl_Widget *, void *) { select_all(false); });
    list = new ListView(0, 0, 10, 10);
    setup_list();
    details = new DetailsPane(0, 0, 10, 10);
    end();
  }

  // Whether every update shown with the current filter and search is selected.
  static bool all_selected() {
    App &a = app();
    for (auto &r : a.rows)
      if (!r.header && !a.list.updates[size_t(r.index)].selected) return false;
    return true;
  }
  static void select_all(bool on) {
    App &a = app();
    // Only the updates that are visible with the current filter and search.
    for (auto &r : a.rows)
      if (!r.header) a.list.updates[size_t(r.index)].selected = on;
    a.changed();
  }

  void setup_list() {
    list->count = [] { return int(app().rows.size()); };
    list->row_height = [](int i) { return app().rows[size_t(i)].header ? S(36) : S(56); };
    list->selectable = [](int i) { return !app().rows[size_t(i)].header; };
    list->paint_row = [](Painter &p, int i, int x, int y, int w, int h, bool sel, bool hover) {
      const Theme &t = active_theme();
      App &a = app();
      const Row &r = a.rows[size_t(i)];
      if (r.header) {
        int on = 0, total = 0;
        for (auto &u : a.list.updates)
          if (u.kind == r.kind) {
            ++total;
            on += u.selected;
          }
        paint_checkbox(p, x + S(14), y + (h - S(18)) / 2.0 + S(3), S(18),
                       on == 0 ? 0 : on == total ? 1 : 2, false);
        p.circle(x + S(48), y + h / 2.0 + S(3), S(4), kind_color(r.kind));
        if (i) p.line(x + S(10), y + 0.5, x + w - S(10), y + 0.5, t.border.alpha(0.7));
        return;
      }
      double rr = look().liquid() ? S(14) : S(8);
      if (sel) p.fill_rounded(x + S(4), y + S(2), w - S(8), h - S(4), rr, t.accent.alpha(t.dark ? 0.25 : 0.13));
      else if (hover) p.fill_rounded(x + S(4), y + S(2), w - S(8), h - S(4), rr, t.hover.alpha(0.8));
      const Update &u = a.list.updates[size_t(r.index)];
      paint_checkbox(p, x + S(14), y + (h - S(18)) / 2.0, S(18), u.selected ? 1 : 0, hover);
    };
    list->text_row = [](int i, int x, int y, int w, int h, bool sel, bool) {
      const Theme &t = active_theme();
      App &a = app();
      const Row &r = a.rows[size_t(i)];
      if (r.header) {
        int n = 0;
        for (auto &row : a.rows) n += !row.header && row.kind == r.kind;
        text_font(t.font_size - S(1), true);
        std::string title = kind_title(r.kind);
        draw_text(title, x + S(60), y + S(6), w - S(140), h - S(6), t.text);
        int tw = text_width(title);
        text_font(t.font_size - S(1));
        draw_text(format("%d", n), x + S(68) + tw, y + S(6), S(60), h - S(6), t.text_dim);
        return;
      }
      const Update &u = a.list.updates[size_t(r.index)];
      int right = std::min(S(220), w * 2 / 5);
      int tx = x + S(44), tw = w - S(44) - right - S(16);
      text_font(t.font_size, true);
      draw_text(u.name, tx, y + S(8), tw, S(22), t.text);
      text_font(t.font_size - S(1));
      draw_text(u.summary.empty() ? join(u.packages, ", ") : u.summary, tx, y + S(28), tw, S(20),
                t.text_dim);
      std::string versions = u.old_version.empty() ? u.new_version : u.old_version + "  →  " + u.new_version;
      text_font(t.font_size - S(2));
      draw_text(versions, x + w - right - S(12), y + S(8), right, S(22), sel ? t.text : t.text_dim, 2);
      draw_text(format_size(u.size), x + w - right - S(12), y + S(28), right, S(20), t.text_dim, 2);
    };
    list->clicked = [](int i, int rx, int) {
      App &a = app();
      const Row &r = a.rows[size_t(i)];
      if (rx >= S(40)) return;
      if (r.header) {
        bool any_off = false;
        for (auto &u : a.list.updates)
          if (u.kind == r.kind && !u.selected) any_off = true;
        for (auto &u : a.list.updates)
          if (u.kind == r.kind) u.selected = any_off;
      } else {
        auto &u = a.list.updates[size_t(r.index)];
        u.selected = !u.selected;
      }
      a.changed();
    };
    list->toggled = [](int i) {
      App &a = app();
      const Row &r = a.rows[size_t(i)];
      if (!r.header) {
        auto &u = a.list.updates[size_t(r.index)];
        u.selected = !u.selected;
        a.changed();
      }
    };
    list->selection_changed = [](int i) {
      App &a = app();
      int index = i >= 0 && !a.rows[size_t(i)].header ? a.rows[size_t(i)].index : -1;
      a.select_changelog(index);
    };
    list->empty_icon = Icon::Shield;
  }

  void layout() override {
    App &a = app();
    int p = pad(), X = x() + p, W = w() - 2 * p;
    hero_y = y() + S(20);
    hero_h = S(108);
    int bh = S(38);
    install->size(std::max(S(150), install->preferred_width()), bh);
    refresh->size(std::max(S(116), refresh->preferred_width()), bh);
    install->position(X + W - S(20) - install->w(), hero_y + (hero_h - bh) / 2);
    refresh->position(install->x() - S(10) - refresh->w(), install->y());
    int cy = hero_y + hero_h + S(14);
    banner_y = cy;
    if (a.reboot) {
      restart->resize(X + W - S(12) - S(120), cy + S(8), S(120), S(32));
      restart->show();
      cy += S(48) + S(12);
    } else {
      restart->hide();
    }
    int th = S(34);
    search->resize(X, cy, std::min(S(190), W / 4), th);
    // One button toggles between Select All and Select None.
    none->hide();
    none->size(none->preferred_width() - S(12), th);
    all->size(none->w(), th);
    all->position(X + W - all->w(), cy);
    chips->resize(search->x() + search->w() + S(12), cy + S(2), all->x() - search->x() - search->w() - S(24), th - S(4));
    int by = cy + th + S(12), bhh = y() + h() - p + S(8) - by;
    int lw = W >= S(760) ? W * 56 / 100 : W;
    list->resize(X, by, lw, bhh);
    if (W >= S(760)) {
      details->resize(X + lw + S(14), by, W - lw - S(14), bhh);
      details->show();
    } else {
      details->hide();
    }
  }

  void sync() {
    App &a = app();
    chips->set_counts(a.list);
    bool every = false;
    auto pkgs = selected_packages(a.list.updates, &every);
    int n = int(std::count_if(a.list.updates.begin(), a.list.updates.end(),
                              [](const Update &u) { return u.selected; }));
    all->text(!a.rows.empty() && all_selected() ? _("Select None") : _("Select All"));
    install->text(every || !n ? std::string(_("Install Updates")) : format(_("Install %d Selected"), n));
    if (n && !a.busy() && a.list.valid) install->activate();
    else install->deactivate();
    if (a.busy() || a.loading) refresh->deactivate();
    else refresh->activate();
    if (a.list.updates.empty()) {
      all->deactivate();
    } else {
      all->activate();
    }
    list->loading = a.loading && a.list.updates.empty();
    if (a.loading) {
      list->empty_title = "";
    } else if (!a.load_error.empty()) {
      list->empty_icon = Icon::Warning;
      list->empty_title = _("Updates could not be read");
      list->empty_text = a.load_error;
    } else if (a.list.updates.empty()) {
      list->empty_icon = Icon::Shield;
      list->empty_title = _("Your system is up to date");
      list->empty_text = _("Refresh to check the package sources for new updates.");
    } else {
      list->empty_icon = Icon::Search;
      list->empty_title = _("No matching updates");
      list->empty_text = _("Try another search or filter.");
    }
    layout();
    redraw();
  }

  void hero_text(std::string &title, std::string &subtitle, Icon &icon, Color &color) const {
    App &a = app();
    const Theme &t = active_theme();
    std::string age = format(_("Last refreshed %s"), format_age(a.refreshed, std::time(nullptr)).c_str());
    if (a.busy() && a.progress.action == "refresh") {
      title = _("Checking for updates…");
      subtitle = status_text(a.progress.status);
      icon = Icon::Restart;
      color = t.accent;
    } else if (a.loading && !a.loaded_once) {
      title = _("Checking for updates…");
      subtitle = _("Reading the package lists");
      icon = Icon::Restart;
      color = t.accent;
    } else if (!a.load_error.empty()) {
      title = _("Updates could not be read");
      subtitle = a.load_error;
      icon = Icon::Warning;
      color = warning_color();
    } else if (a.list.updates.empty()) {
      title = _("Your system is up to date");
      subtitle = age;
      icon = Icon::Shield;
      color = success_color();
    } else {
      int n = int(a.list.updates.size());
      title = n == 1 ? std::string(_("1 update available")) : format(_("%d updates available"), n);
      std::vector<std::string> parts;
      int sec = a.list.count(Kind::Security);
      if (sec) parts.push_back(sec == 1 ? std::string(_("1 security update")) : format(_("%d security updates"), sec));
      parts.push_back(format(_("%s to download"), format_size(a.list.total_size()).c_str()));
      subtitle = join(parts, "  ·  ");
      std::vector<std::string> second = {age};
      if (!a.list.held.empty()) second.push_back(format(_("%d held back by the administrator"), int(a.list.held.size())));
      if (a.packagekit_client.empty()) second.push_back(_("Installs through APT and pkexec"));
      subtitle += "\n" + join(second, "  ·  ");
      icon = sec ? Icon::Shield : Icon::Download;
      color = sec ? danger_color() : t.accent;
    }
  }

  void paint() override {
    const Theme &t = active_theme();
    App &a = app();
    int p = pad(), X = x() + p, W = w() - 2 * p;
    std::string title, subtitle;
    Icon icon;
    Color color;
    hero_text(title, subtitle, icon, color);
    int text_x = X + S(100), text_w = refresh->x() - text_x - S(12);
    {
      Painter pp(window()->w(), window()->h());
      paint_card(pp, X, hero_y, W, hero_h, true);
      double cs = S(60), cx = X + S(22), cy = hero_y + (hero_h - cs) / 2;
      pp.fill_rounded(cx, cy, cs, cs, look().liquid() ? cs / 2 : S(16), color.alpha(t.dark ? 0.22 : 0.14));
      if (a.loading && !a.loaded_once)
        paint_spinner(pp, cx + cs / 2, cy + cs / 2, S(15), a.phase, color);
      else
        paint_icon(pp, icon, cx + S(14), cy + S(14), S(32), readable_on(color, t.surface));
      if (a.reboot) {
        double r = look().card_radius();
        Color wc = warning_color();
        pp.fill_rounded(X, banner_y, W, S(48), r, wc.alpha(t.dark ? 0.18 : 0.12));
        pp.stroke_rounded(X, banner_y, W, S(48), r, wc.alpha(0.45));
        paint_icon(pp, Icon::Restart, X + S(14), banner_y + S(13), S(22), readable_on(wc, t.background));
      }
    }
    auto lines = split(subtitle, '\n');
    int ty = hero_y + (lines.size() > 1 ? S(14) : S(24));
    text_font(t.font_size + S(7), true);
    draw_text(title, text_x, ty, text_w, S(32), t.text);
    text_font(t.font_size);
    for (size_t i = 0; i < lines.size(); ++i)
      draw_text(lines[i], text_x, ty + S(34) + int(i) * S(22), text_w, S(22), t.text_dim);
    if (a.reboot) {
      std::string msg = _("Restart to finish installing updates");
      if (!a.reboot_packages.empty()) msg += "  ·  " + join(a.reboot_packages, ", ");
      text_font(t.font_size, true);
      draw_text(msg, X + S(46), banner_y, W - S(46) - S(144), S(48), t.text);
    }
  }

  void activated() override { Fl::focus(list); }

  Button *refresh, *install, *restart, *all, *none;
  SearchField *search;
  Chips *chips;
  ListView *list;
  DetailsPane *details;
  int hero_y = 0, hero_h = 0, banner_y = 0;
};

// ---------------------------------------------------------------- history

class HistoryPage : public Page {
 public:
  HistoryPage() {
    search = new SearchField(0, 0, 10, 10, _("Search packages"));
    search->callback([](Fl_Widget *, void *p) { static_cast<HistoryPage *>(p)->rebuild(); }, this);
    list = new ListView(0, 0, 10, 10);
    changes = new ListView(0, 0, 10, 10);
    changes->framed = false;
    end();
    list->count = [this] { return int(rows.size()); };
    list->row_height = [](int) { return S(62); };
    list->paint_row = [](Painter &p, int, int x, int y, int w, int h, bool sel, bool hover) {
      const Theme &t = active_theme();
      double rr = look().liquid() ? S(14) : S(8);
      if (sel) p.fill_rounded(x + S(4), y + S(2), w - S(8), h - S(4), rr, t.accent.alpha(t.dark ? 0.25 : 0.13));
      else if (hover) p.fill_rounded(x + S(4), y + S(2), w - S(8), h - S(4), rr, t.hover.alpha(0.8));
    };
    list->text_row = [this](int i, int x, int y, int w, int, bool, bool) {
      const Theme &t = active_theme();
      const HistoryEntry &e = app().history[size_t(rows[size_t(i)])];
      text_font(t.font_size, true);
      draw_text(format_date(e.start), x + S(16), y + S(6), w / 2, S(22), t.text);
      text_font(t.font_size - S(1));
      draw_text(e.summary(), x + w / 2, y + S(6), w / 2 - S(14), S(22),
                e.error.empty() ? t.text_dim : danger_color(), 2);
      text_font(t.font_size - S(2));
      draw_text(e.commandline, x + S(16), y + S(30), w - S(30), S(22), t.text_dim);
    };
    list->selection_changed = [this](int) {
      changes->select(-1, false);
      changes->reset_scroll();
      redraw();
    };
    changes->count = [this] {
      const HistoryEntry *e = current();
      return e ? int(e->changes.size()) : 0;
    };
    changes->row_height = [](int) { return S(44); };
    changes->paint_row = [this](Painter &p, int i, int x, int y, int w, int h, bool, bool) {
      const Change &c = current()->changes[size_t(i)];
      std::string label = action_label(c.action);
      paint_badge(p, x + S(14), y + (h - S(20)) / 2.0, badge_width(label), S(20), action_color(c.action));
      if (i) p.line(x + S(14), y + 0.5, x + w - S(14), y + 0.5, active_theme().border.alpha(0.6));
    };
    changes->text_row = [this](int i, int x, int y, int w, int h, bool, bool) {
      const Theme &t = active_theme();
      const Change &c = current()->changes[size_t(i)];
      std::string label = action_label(c.action);
      int bw = badge_width(label);
      draw_badge_text(label, x + S(14), y + (h - S(20)) / 2, bw, S(20), action_color(c.action));
      int nx = x + S(14) + std::max(bw, S(92)) + S(10);
      std::string v = c.old_version.empty() ? c.new_version
                      : c.new_version.empty() ? c.old_version
                                              : c.old_version + "  →  " + c.new_version;
      text_font(t.font_size - S(2));
      int vw = std::min(text_width(v), w * 2 / 5);
      draw_text(v, x + w - S(14) - vw, y, vw, h, t.text_dim, 2);
      int name_w = x + w - S(14) - vw - S(12) - nx;
      text_font(t.font_size, true);
      draw_text(c.package, nx, y + S(2), name_w, c.arch.empty() && !c.automatic ? h - S(4) : S(24), t.text);
      std::string meta = c.arch;
      if (c.automatic) meta += (meta.empty() ? "" : "  ·  ") + std::string(_("automatic"));
      if (!meta.empty()) {
        text_font(t.font_size - S(3));
        draw_text(meta, nx, y + S(22), name_w, S(18), t.text_dim);
      }
    };
    changes->empty_icon = Icon::Package;
  }
  static std::string action_label(const std::string &a) {
    if (a == "Install") return _("Installed");
    if (a == "Upgrade") return _("Upgraded");
    if (a == "Remove") return _("Removed");
    if (a == "Purge") return _("Purged");
    if (a == "Downgrade") return _("Downgraded");
    if (a == "Reinstall") return _("Reinstalled");
    return a;
  }
  static Color action_color(const std::string &a) {
    if (a == "Install") return success_color();
    if (a == "Remove" || a == "Purge") return danger_color();
    if (a == "Downgrade") return warning_color();
    return active_theme().accent;
  }
  const HistoryEntry *current() const {
    int s = list->selected();
    if (s < 0 || s >= int(rows.size())) return nullptr;
    return &app().history[size_t(rows[size_t(s)])];
  }
  void rebuild() {
    rows.clear();
    auto &h = app().history;
    for (size_t i = 0; i < h.size(); ++i)
      if (matches(h[i], search->value())) rows.push_back(int(i));
    list->select(-1, false);
    list->reset_scroll();
    if (!rows.empty()) list->select(0);
    App &a = app();
    list->loading = a.history_loading;
    list->empty_icon = Icon::History;
    list->empty_title = a.history_loading ? "" : rows.empty() && !h.empty() ? _("No matching transactions") : _("No history yet");
    list->empty_text = a.history_loading ? "" : _("Package changes recorded in /var/log/apt/history.log appear here.");
    changes->empty_title = "";
    redraw();
  }
  void layout() override {
    int p = pad(), X = x() + p, W = w() - 2 * p;
    int top = heading_bottom();
    search->resize(X + W - std::min(S(260), W / 3), y() + S(30), std::min(S(260), W / 3), S(34));
    int lw = W * 46 / 100;
    list->resize(X, top, lw, y() + h() - p + S(8) - top);
    detail_x = X + lw + S(14);
    detail_w = W - lw - S(14);
    detail_y = top;
    detail_h = list->h();
    changes->resize(detail_x + S(6), detail_y + S(112), detail_w - S(12), detail_h - S(118));
  }
  void paint() override {
    const Theme &t = active_theme();
    App &a = app();
    heading(_("History"),
            a.history_loading ? _("Reading the APT history…")
                              : format(_("%d transactions recorded by APT"), int(a.history.size())));
    const HistoryEntry *e = current();
    {
      Painter p(window()->w(), window()->h());
      paint_card(p, detail_x, detail_y, detail_w, detail_h);
      if (e) p.line(detail_x + S(18), detail_y + S(106) + 0.5, detail_x + detail_w - S(18), detail_y + S(106) + 0.5, t.border);
    }
    if (!e) return;
    int px = detail_x + S(20), pw = detail_w - S(40);
    text_font(t.font_size + S(3), true);
    draw_text(format_date(e->start), px, detail_y + S(14), pw, S(26), t.text);
    text_font(t.font_size - S(1));
    draw_text(e->commandline.empty() ? _("Unknown command") : e->commandline, px, detail_y + S(42), pw, S(20), t.text_dim);
    std::string who = e->requested_by.empty() ? e->summary() : format(_("By %s"), e->requested_by.c_str()) + "  ·  " + e->summary();
    draw_text(who, px, detail_y + S(62), pw, S(20), t.text_dim);
    if (!e->error.empty()) {
      text_font(t.font_size - S(1), true);
      draw_text(e->error, px, detail_y + S(82), pw, S(20), danger_color());
    }
  }
  void activated() override {
    app().load_history();
    Fl::focus(list);
  }
  SearchField *search;
  ListView *list, *changes;
  std::vector<int> rows;
  int detail_x = 0, detail_y = 0, detail_w = 0, detail_h = 0;
};

// ---------------------------------------------------------------- sources

class SourcesPage : public Page {
 public:
  SourcesPage() {
    tool = first_program({"software-properties-gtk", "software-properties-qt", "software-properties-kde"});
    open = new Button(0, 0, 10, 10, _("Software Sources…"));
    open->icon(Icon::Sources);
    open->callback([](Fl_Widget *, void *p) {
      spawn_detached({static_cast<SourcesPage *>(p)->tool});
    }, this);
    if (tool.empty()) open->hide();
    reload = new Button(0, 0, 10, 10, _("Reload"));
    reload->icon(Icon::Restart);
    reload->callback([](Fl_Widget *, void *) { app().load_sources(true); });
    list = new ListView(0, 0, 10, 10);
    end();
    list->count = [] { return int(app().sources.size()); };
    list->row_height = [](int) { return S(70); };
    list->paint_row = [](Painter &p, int i, int x, int y, int w, int h, bool sel, bool hover) {
      const Theme &t = active_theme();
      const Source &s = app().sources[size_t(i)];
      double rr = look().liquid() ? S(14) : S(8);
      if (sel) p.fill_rounded(x + S(4), y + S(2), w - S(8), h - S(4), rr, t.accent.alpha(t.dark ? 0.25 : 0.13));
      else if (hover) p.fill_rounded(x + S(4), y + S(2), w - S(8), h - S(4), rr, t.hover.alpha(0.8));
      p.circle(x + S(22), y + S(22), S(5), s.enabled ? success_color() : t.text_dim.alpha(0.5));
      int bx = x + w - S(14);
      for (auto &type : split(s.types, ' ')) {
        int bw = badge_width(type);
        bx -= bw;
        paint_badge(p, bx, y + S(12), bw, S(20), type == "deb-src" ? warning_color() : t.accent);
        bx -= S(6);
      }
    };
    list->text_row = [](int i, int x, int y, int w, int, bool, bool) {
      const Theme &t = active_theme();
      const Source &s = app().sources[size_t(i)];
      int bx = x + w - S(14), bw_total = 0;
      for (auto &type : split(s.types, ' ')) {
        int bw = badge_width(type);
        bx -= bw;
        draw_badge_text(type, bx, y + S(12), bw, S(20), type == "deb-src" ? warning_color() : t.accent);
        bx -= S(6);
        bw_total += bw + S(6);
      }
      text_font(t.font_size, true);
      draw_text(s.uris, x + S(40), y + S(10), w - S(60) - bw_total, S(24), s.enabled ? t.text : t.text_dim);
      text_font(t.font_size - S(1));
      std::string where = s.suites + (s.components.empty() ? "" : "  ·  " + s.components);
      draw_text(where, x + S(40), y + S(32), w - S(56), S(18), t.text_dim);
      text_font(t.font_size - S(2));
      std::string file = s.file + ":" + std::to_string(s.line) + "  ·  " +
                         (s.format == "deb822" ? _("deb822 format") : _("one-line format"));
      if (!s.enabled) file = std::string(_("Disabled")) + "  ·  " + file;
      draw_text(file, x + S(40), y + S(50), w - S(56), S(16), t.text_dim);
    };
    list->empty_icon = Icon::Sources;
  }
  void layout() override {
    int p = pad(), X = x() + p, W = w() - 2 * p;
    int bx = X + W;
    reload->size(reload->preferred_width(), S(34));
    reload->position(bx - reload->w(), y() + S(30));
    bx = reload->x() - S(10);
    open->size(open->preferred_width(), S(34));
    open->position(bx - open->w(), y() + S(30));
    int top = heading_bottom() + S(28);
    list->resize(X, top, W, y() + h() - p + S(8) - top);
  }
  void sync() {
    App &a = app();
    list->loading = a.sources_loading;
    list->empty_title = a.sources_loading ? "" : _("No package sources found");
    list->empty_text = a.sources_loading ? "" : _("APT reads sources from /etc/apt/sources.list and /etc/apt/sources.list.d.");
    redraw();
  }
  void paint() override {
    const Theme &t = active_theme();
    App &a = app();
    int enabled = int(std::count_if(a.sources.begin(), a.sources.end(), [](const Source &s) { return s.enabled; }));
    heading(_("Sources"), a.sources_loading ? _("Reading the package sources…")
                                            : format(_("%d package sources, %d enabled"), int(a.sources.size()), enabled));
    int p = pad();
    text_font(t.font_size - S(1));
    std::string note = tool.empty()
                           ? _("Shown read-only. Editing the sources needs administrator rights; install software-properties-gtk for a graphical editor.")
                           : _("Shown read-only. Use Software Sources to change them; it asks for administrator rights.");
    draw_text(note, x() + p, heading_bottom() - S(4), w() - 2 * p, S(24), t.text_dim);
  }
  void activated() override {
    app().load_sources();
    Fl::focus(list);
  }
  std::string tool;
  Button *open, *reload;
  ListView *list;
};

// ------------------------------------------------------------ maintenance

class MaintenancePage : public Page {
 public:
  struct Action {
    Icon icon;
    std::string title;
    Button *button;
    std::function<std::string()> text;
  };
  MaintenancePage() {
    add(Icon::Restart, _("Refresh package information"), _("Refresh"),
        [] { return format(_("Download the latest package lists from your sources. Last refreshed %s."),
                           format_age(app().refreshed, std::time(nullptr)).c_str()); },
        [] { app().start("refresh"); });
    add(Icon::Broom, _("Clean the package cache"), _("Clean…"),
        [] {
          App &a = app();
          if (!a.cache_files) return std::string(_("No downloaded package files are stored in /var/cache/apt/archives."));
          return format(_("%d downloaded package files use %s in /var/cache/apt/archives."), a.cache_files,
                        format_size(a.cache_bytes).c_str());
        },
        [] {
          App &a = app();
          if (confirm(a.window, _("Clean the package cache?"),
                      format(_("Delete %d downloaded package files (%s). Installed software is not affected."),
                             a.cache_files, format_size(a.cache_bytes).c_str()),
                      "", _("Clean")))
            a.start("clean");
        });
    add(Icon::Trash, _("Remove unused packages"), _("Review…"),
        [] {
          App &a = app();
          if (a.autoremove_loading) return std::string(_("Looking for packages that are no longer needed…"));
          if (!a.autoremove_known) return std::string(_("Packages installed automatically that nothing needs any more."));
          if (a.autoremove.empty()) return std::string(_("No unused packages. Nothing needs to be removed."));
          return format(_("%d automatically installed packages are no longer needed."), int(a.autoremove.size()));
        },
        [] {
          App &a = app();
          if (a.autoremove.empty()) return;
          if (confirm(a.window, _("Remove unused packages?"),
                      format(_("These %d packages were installed automatically and are no longer needed. "
                               "Removing them does not delete your files."),
                             int(a.autoremove.size())),
                      join(a.autoremove, "\n"), _("Remove"), true))
            a.start("autoremove");
        });
    add(Icon::Wrench, _("Repair interrupted installations"), _("Repair…"),
        [] { return std::string(_("Finish configuring packages after an update was interrupted (dpkg --configure -a).")); },
        [] {
          if (confirm(app().window, _("Repair interrupted installations?"),
                      _("Configures all packages that were unpacked but not set up. This needs administrator rights."),
                      "", _("Repair")))
            app().start("repair");
        });
    add(Icon::Terminal, _("Update in a terminal"), _("Open Terminal"),
        [] { return std::string(_("Run PackageKit's interactive update to review every change before confirming it.")); },
        [] { app().open_terminal_transaction(); });
    end();
  }
  void add(Icon icon, const std::string &title, const std::string &label, std::function<std::string()> text,
           std::function<void()> run) {
    auto *fn = new std::function<void()>(std::move(run));
    auto *b = new Button(0, 0, 10, 10, label);
    b->callback([](Fl_Widget *, void *d) { (*static_cast<std::function<void()> *>(d))(); }, fn);
    actions.push_back({icon, title, b, std::move(text)});
  }
  void layout() override {
    int p = pad(), X = x() + p, W = w() - 2 * p;
    int cy = heading_bottom();
    for (auto &a : actions) {
      a.button->size(std::max(S(120), a.button->preferred_width()), S(36));
      a.button->position(X + W - S(18) - a.button->w(), cy + (S(78) - S(36)) / 2);
      cy += S(78) + S(10);
    }
  }
  void sync() {
    App &a = app();
    for (size_t i = 0; i < actions.size(); ++i) {
      bool on = !a.busy();
      if (i == 1) on = on && a.cache_files > 0;
      if (i == 2) on = on && !a.autoremove.empty();
      if (i == 4) on = on && !find_program("xterm").empty() && !a.packagekit_client.empty();
      if (on) actions[i].button->activate();
      else actions[i].button->deactivate();
    }
    redraw();
  }
  void paint() override {
    const Theme &t = active_theme();
    heading(_("Maintenance"), _("Keep the package system clean and healthy"));
    int p = pad(), X = x() + p, W = w() - 2 * p;
    int cy = heading_bottom();
    {
      Painter pp(window()->w(), window()->h());
      int y2 = cy;
      for (auto &a : actions) {
        paint_card(pp, X, y2, W, S(78));
        pp.fill_rounded(X + S(18), y2 + S(19), S(40), S(40), look().liquid() ? S(20) : S(10), t.accent.alpha(t.dark ? 0.2 : 0.12));
        paint_icon(pp, a.icon, X + S(26), y2 + S(27), S(24), t.accent_text);
        y2 += S(78) + S(10);
      }
    }
    for (auto &a : actions) {
      int tw = a.button->x() - (X + S(74)) - S(16);
      text_font(t.font_size + S(1), true);
      draw_text(a.title, X + S(74), cy + S(14), tw, S(24), t.text);
      text_font(t.font_size - S(1));
      draw_text(a.text(), X + S(74), cy + S(40), tw, S(22), t.text_dim);
      cy += S(78) + S(10);
    }
  }
  void activated() override { app().load_maintenance(); }
  std::vector<Action> actions;
};

// --------------------------------------------------------------- settings

class SettingsPage : public Page {
 public:
  SettingsPage() {
    App &a = app();
    interval = new Fl_Choice(0, 0, 10, 10);
    style_choice(interval);
    int hours = a.cfg.get_int("update", "check_interval", 24), index = 4;
    for (size_t i = 0; i < interval_hours().size(); ++i) {
      interval->add(menu_escape(interval_label(interval_hours()[i])).c_str());
      if (interval_hours()[i] == hours) index = int(i);
    }
    interval->value(index);
    interval->callback([](Fl_Widget *w, void *) {
      int v = static_cast<Fl_Choice *>(w)->value();
      if (v >= 0) save("check_interval", std::to_string(interval_hours()[size_t(v)]));
    });
    refresh = make_switch("auto_refresh", true);
    notify = make_switch("notify", true);
    security = make_switch("notify_security_only", false);
    end();
  }
  Switch *make_switch(const char *key, bool fallback) {
    auto *s = new Switch(0, 0, 10, 10);
    s->value(app().cfg.get_bool("update", key, fallback));
    s->callback([](Fl_Widget *w, void *k) {
      save(static_cast<const char *>(k), static_cast<Switch *>(w)->value() ? "true" : "false");
    }, const_cast<char *>(key));
    return s;
  }
  static void save(const std::string &key, const std::string &value) {
    App &a = app();
    a.cfg.set("update", key, value);
    a.cfg.save();
    if (a.pages[P_SETTINGS]) a.pages[P_SETTINGS]->redraw();
  }
  struct RowText {
    std::string title, hint;
  };
  std::vector<RowText> rows() const {
    return {
        {_("Check for updates"), _("How often SDE looks for new updates while you are signed in.")},
        {_("Refresh package sources automatically"),
         _("Download new package lists through PackageKit before each automatic check.")},
        {_("Show a notification"), _("Tell me when updates are available.")},
        {_("Only for security updates"), _("Notify only when at least one security update is available.")},
    };
  }
  int row_h() const { return S(66); }
  void layout() override {
    int p = pad(), X = x() + p, W = std::min(w() - 2 * p, S(760));
    card_x = X;
    card_w = W;
    card_y = heading_bottom();
    Fl_Widget *controls[] = {interval, refresh, notify, security};
    for (int i = 0; i < 4; ++i) {
      int ry = card_y + S(8) + i * row_h();
      if (i == 0) controls[i]->resize(X + W - S(20) - S(200), ry + (row_h() - S(32)) / 2, S(200), S(32));
      else controls[i]->resize(X + W - S(20) - S(48), ry + (row_h() - S(28)) / 2, S(48), S(28));
    }
  }
  void paint() override {
    const Theme &t = active_theme();
    heading(_("Settings"), _("Automatic checks and notifications"));
    auto r = rows();
    int ch = S(16) + int(r.size()) * row_h();
    {
      Painter pp(window()->w(), window()->h());
      paint_card(pp, card_x, card_y, card_w, ch);
      for (size_t i = 1; i < r.size(); ++i) {
        double ly = card_y + S(8) + i * row_h() + 0.5;
        pp.line(card_x + S(20), ly, card_x + card_w - S(20), ly, t.border.alpha(0.7));
      }
      paint_card(pp, card_x, card_y + ch + S(16), card_w, S(92));
      paint_icon(pp, Icon::Info, card_x + S(20), card_y + ch + S(16) + S(20), S(22), t.accent_text);
    }
    for (size_t i = 0; i < r.size(); ++i) {
      int ry = card_y + S(8) + int(i) * row_h();
      int tw = card_w - S(40) - (i == 0 ? S(220) : S(68));
      bool dim = i == 3 && !notify->value();
      text_font(t.font_size + S(1));
      draw_text(r[i].title, card_x + S(20), ry + S(10), tw, S(24), dim ? t.text_dim : t.text);
      text_font(t.font_size - S(1));
      draw_text(r[i].hint, card_x + S(20), ry + S(34), tw, S(20), t.text_dim);
    }
    int ny = card_y + ch + S(16);
    text_font(t.font_size, true);
    draw_text(_("Updates are never installed automatically"), card_x + S(54), ny + S(16), card_w - S(74), S(24), t.text);
    text_font(t.font_size - S(1));
    draw_wrapped(_("Automatic checks only read package information. Installing always asks for "
                   "administrator rights through PackageKit or pkexec. Settings are saved "
                   "automatically in the [update] section of sde.conf."),
                 card_x + S(54), ny + S(42), card_w - S(74), S(44), t.text_dim);
  }
  Fl_Choice *interval;
  Switch *refresh, *notify, *security;
  int card_x = 0, card_y = 0, card_w = 0;
};

// ------------------------------------------------------------------ about

class AboutPage : public Page {
 public:
  AboutPage() {
    upstream = new Button(0, 0, 10, 10, _("mintupdate on GitHub"));
    upstream->callback([](Fl_Widget *, void *) { open_uri("https://github.com/linuxmint/mintupdate"); });
    copyright = new Button(0, 0, 10, 10, _("Copyright and License"));
    copyright->callback([](Fl_Widget *, void *) {
      std::string path = helper("COPYRIGHT");
      open_uri(file_exists("/usr/share/doc/sde/copyright") ? "/usr/share/doc/sde/copyright" : path);
    });
    about = new Button(0, 0, 10, 10, _("About SDE Update"));
    about->callback([](Fl_Widget *, void *) {
      AppInfo info;
      info.name = _("SDE Update");
      info.program = "sde-update";
      info.version = SDE_UPDATE_VERSION;
      info.icon = "system-software-update";
      info.description = _("Keeps DebION GNU/Linux up to date: security, software and kernel updates "
                           "from the configured sources.");
      info.credits = {_("Update model adapted from Linux Mint's Update Manager (mintupdate), "
                        "© 2007–2015 Clement Lefebvre and the Linux Mint team, GPL-3.0-or-later"),
                      toolkit_credit()};
      show_about(info);
    });
    end();
  }
  void layout() override {
    int p = pad(), X = x() + p;
    int by = y() + S(186) + S(150) - S(52);
    upstream->size(upstream->preferred_width(), S(34));
    upstream->position(X + S(20), by);
    copyright->size(copyright->preferred_width(), S(34));
    copyright->position(upstream->x() + upstream->w() + S(10), by);
    about->size(about->preferred_width(), S(34));
    about->position(copyright->x() + copyright->w() + S(10), by);
  }
  void paint() override {
    const Theme &t = active_theme();
    App &a = app();
    int p = pad(), X = x() + p, W = std::min(w() - 2 * p, S(760));
    {
      Painter pp(window()->w(), window()->h());
      pp.image(IconLoader::get({"system-software-update", "software-update-available", "sde-logo"}, S(72)),
               X, y() + S(28), S(72));
      paint_card(pp, X, y() + S(126), W, S(210) - S(10));
      paint_card(pp, X, y() + S(186) + S(150) + S(14), W, S(120));
    }
    text_font(t.font_size + S(10), true);
    draw_text(_("SDE Update"), X + S(92), y() + S(32), W - S(92), S(38), t.text);
    text_font(t.font_size);
    draw_text(format(_("Version %s · System updates for DebION GNU/Linux"), SDE_UPDATE_VERSION), X + S(92),
              y() + S(70), W - S(92), S(24), t.text_dim);
    text_font(t.font_size - S(1));
    draw_text(developer_line(), X + S(92), y() + S(94), W - S(92), S(22), t.text_dim);
    int cy = y() + S(126);
    text_font(t.font_size + S(1), true);
    draw_text(_("Credits"), X + S(20), cy + S(14), W - S(40), S(24), t.text);
    text_font(t.font_size);
    draw_wrapped(_("The update model (grouping by source package, update types and the choice of the "
                   "package that represents a group) is adapted from Linux Mint's Update Manager, "
                   "mintupdate. Copyright © 2007–2015 Clement Lefebvre and the Linux Mint team, "
                   "GPL-3.0-or-later. SDE uses Debian's PackageKit for transactions and does not "
                   "import Mint repositories or policies."),
                 X + S(20), cy + S(42), W - S(40), S(100), t.text_dim);
    int sy = y() + S(186) + S(150) + S(14);
    text_font(t.font_size + S(1), true);
    draw_text(_("Components"), X + S(20), sy + S(14), W - S(40), S(24), t.text);
    text_font(t.font_size - S(1));
    std::string pk = a.packagekit_client.empty() ? std::string(_("not installed; APT is used through pkexec"))
                                                 : format(_("available (%s)"), a.packagekit_client.c_str());
    std::string polkit = find_program("pkexec").empty() ? _("not installed") : _("available");
    const std::pair<std::string, std::string> rows[] = {
        {_("PackageKit"), pk},
        {_("pkexec"), polkit},
        {_("Helpers"), helper_dir()},
    };
    int ry = sy + S(42);
    for (auto &r : rows) {
      draw_text(r.first, X + S(20), ry, S(130), S(22), t.text_dim);
      draw_text(r.second, X + S(150), ry, W - S(170), S(22), t.text);
      ry += S(22);
    }
  }
  Button *upstream, *copyright, *about;
};

// ---------------------------------------------------------- activity strip

class ActivityStrip : public Fl_Group {
 public:
  ActivityStrip() : Fl_Group(0, 0, 100, 100) {
    box(FL_NO_BOX);
    details = new Button(0, 0, 10, 10, _("Details"));
    details->callback([](Fl_Widget *, void *) {
      App &a = app();
      show_log(a.window, action_title(a.progress.action), join(a.progress.log, "\n"));
    });
    cancel = new Button(0, 0, 10, 10, _("Cancel"));
    cancel->callback([](Fl_Widget *, void *) { app().cancel(); });
    dismiss = new Button(0, 0, 10, 10, _("Dismiss"), Button::Ghost);
    dismiss->callback([](Fl_Widget *, void *) {
      App &a = app();
      a.strip_visible = false;
      a.layout();
    });
    end();
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    int bh = S(34), by = Y + (H - bh) / 2;
    int bx = X + W - S(16);
    for (Button *b : {dismiss, cancel, details}) {
      b->size(std::max(S(92), b->preferred_width()), bh);
      bx -= b->w();
      b->position(bx, by);
      bx -= S(8);
    }
  }
  void sync() {
    App &a = app();
    bool running = a.busy();
    if (running) {
      dismiss->hide();
      cancel->show();
      if (a.progress.cancellable && !a.progress.cancel_requested) cancel->activate();
      else cancel->deactivate();
    } else {
      cancel->hide();
      dismiss->show();
    }
    redraw();
  }
  void draw() override {
    const Theme &t = active_theme();
    App &a = app();
    const Progress &p = a.progress;
    bool running = a.busy();
    int left = x() + S(16);
    int text_right = (running ? cancel : dismiss)->x();
    text_right = std::min(text_right, details->x()) - S(16);
    Color c = running ? t.accent : p.ok() ? success_color() : p.outcome == "cancelled" ? t.text_dim : danger_color();
    {
      Painter pp(window()->w(), window()->h());
      paint_card(pp, x(), y(), w(), h(), running);
      if (running)
        paint_spinner(pp, left + S(14), y() + h() / 2.0, S(11), a.phase, t.accent);
      else
        paint_icon(pp, p.ok() ? Icon::Check : Icon::Warning, left + S(2), y() + (h() - S(24)) / 2.0, S(24),
                   readable_on(c, t.surface));
      if (running)
        paint_progress(pp, left + S(40), y() + h() - S(20), text_right - left - S(40), S(6), p.percent, a.phase);
    }
    std::string title = action_title(p.action), line;
    if (running) {
      if (p.percent >= 0) title += format("  ·  %d%%", p.percent);
      line = status_text(p.status);
      if (!p.item_name.empty())
        line += "  ·  " + p.item_info + " " + p.item_name + (p.item_version.empty() ? "" : " " + p.item_version);
      if (p.items_seen > 1) line += "  ·  " + format(_("%d packages"), p.items_seen);
      if (p.cancel_requested) line = _("Cancelling…");
    } else {
      if (p.outcome == "ok") title = _("Done");
      else if (p.outcome == "nothing") title = _("Nothing to do");
      else if (p.outcome == "cancelled") title = _("Cancelled");
      else if (p.outcome == "denied") title = _("Not authorized");
      else title = _("The transaction failed");
      title += "  ·  " + action_title(p.action);
      if (!p.errors.empty()) line = p.errors.back();
      else if (p.outcome == "denied") line = _("Authentication was cancelled or refused.");
      else if (p.ok()) line = p.backend == "packagekit" ? _("Completed through PackageKit.") : _("Completed through APT.");
      else line = format(_("Exit code %d. Open Details for the full log."), p.exit_code);
    }
    text_font(t.font_size, true);
    draw_text(title, left + S(40), y() + S(8), text_right - left - S(40), S(22), t.text);
    text_font(t.font_size - S(1));
    draw_text(line, left + S(40), y() + S(28), text_right - left - S(40), S(20), t.text_dim);
    draw_children();
  }
  Button *details, *cancel, *dismiss;
};

// ---------------------------------------------------------------- sidebar

class Sidebar : public Fl_Widget {
 public:
  Sidebar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int row_h() const { return S(42); }
  int row_y(int i) const { return y() + S(86) + i * row_h() + (i >= P_SETTINGS ? S(20) : 0); }
  void draw() override {
    const Theme &t = active_theme();
    App &a = app();
    int inset = look().liquid() ? S(10) : 0;
    {
      Painter p(window()->w(), window()->h());
      if (look().liquid()) {
        paint_card(p, x() + inset, y() + inset, w() - 2 * inset, h() - 2 * inset);
      } else {
        p.fill_rect(x(), y(), w(), h(), t.surface);
        if (look().aero()) p.fill_gradient(x(), y(), w(), h(), 0, t.accent.alpha(t.dark ? 0.10 : 0.06), t.surface.alpha(0));
        p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), t.border);
      }
      p.image(IconLoader::get({"system-software-update", "software-update-available", "sde-logo"}, S(34)),
              x() + S(20), y() + S(24), S(34));
      for (int i = 0; i < P_COUNT; ++i) {
        int ry = row_y(i);
        double rx = x() + S(10) + inset / 2, rw = w() - S(20) - inset;
        double rr = look().liquid() ? (row_h() - S(6)) / 2.0 : S(9);
        if (i == a.page) {
          p.fill_rounded(rx, ry, rw, row_h() - S(6), rr, t.accent.alpha(t.dark ? 0.24 : 0.14));
          if (!look().liquid())
            p.fill_rounded(rx, ry + S(10), Sd(3), row_h() - S(26), Sd(1.5), t.accent);
        } else if (i == hover_) {
          p.fill_rounded(rx, ry, rw, row_h() - S(6), rr, t.hover);
        }
        if (Fl::focus() == this && i == a.page) focus_ring(p, rx, ry, rw, row_h() - S(6), rr);
        paint_icon(p, kPages[i].icon, x() + S(24) + inset / 2, ry + (row_h() - S(6) - S(20)) / 2.0, S(20),
                   i == a.page ? t.accent_text : t.text_dim);
        if (i == P_UPDATES && !a.list.updates.empty()) {
          std::string n = std::to_string(a.list.updates.size());
          int bw = std::max(S(22), badge_width(n) - S(4));
          p.fill_rounded(x() + w() - S(18) - inset - bw, ry + (row_h() - S(6) - S(20)) / 2.0, bw, S(20), S(10),
                         a.list.count(Kind::Security) ? danger_color() : t.accent);
        }
      }
    }
    text_font(t.font_size + S(2), true);
    draw_text(_("Update"), x() + S(64), y() + S(24), w() - S(70), S(34), t.text);
    text_font(t.font_size - S(2), true);
    draw_text(_("SYSTEM"), x() + S(22) + inset / 2, row_y(P_SETTINGS) - S(22), w() - S(30), S(18), t.text_dim);
    for (int i = 0; i < P_COUNT; ++i) {
      int ry = row_y(i);
      bool cur = i == a.page;
      text_font(t.font_size, cur);
      draw_text(_(kPages[i].title), x() + S(56) + inset / 2, ry, w() - S(100), row_h() - S(6),
                cur ? t.accent_text : t.text);
      if (i == P_UPDATES && !a.list.updates.empty()) {
        std::string n = std::to_string(a.list.updates.size());
        int bw = std::max(S(22), badge_width(n) - S(4));
        text_font(std::max(S(9), t.font_size - S(2)), true);
        draw_text(n, x() + w() - S(18) - inset - bw, ry + (row_h() - S(6) - S(20)) / 2, bw, S(20),
                  a.list.count(Kind::Security) ? Color::hex(0xFFFFFF) : t.on_accent, 1);
      }
    }
    text_font(t.font_size - S(2));
    std::string backend = a.packagekit_client.empty() ? _("APT via pkexec") : _("Debian PackageKit");
    draw_text(backend, x() + S(22) + inset / 2, y() + h() - S(40) - inset, w() - S(36), S(18), t.text_dim);
    draw_text(_("Based on mintupdate"), x() + S(22) + inset / 2, y() + h() - S(22) - inset, w() - S(36), S(18),
              t.text_dim);
  }
  int index_at(int ey) const {
    for (int i = 0; i < P_COUNT; ++i)
      if (ey >= row_y(i) && ey < row_y(i) + row_h() - S(6)) return i;
    return -1;
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH: {
        int i = index_at(Fl::event_y());
        if (i >= 0) app().show_page(i);
        return 1;
      }
      case FL_ENTER:
      case FL_MOVE: {
        int i = index_at(Fl::event_y());
        if (i != hover_) {
          hover_ = i;
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
        int k = Fl::event_key(), cur = app().page;
        if (k == FL_Up && cur > 0) app().show_page(cur - 1);
        else if (k == FL_Down && cur + 1 < P_COUNT) app().show_page(cur + 1);
        else return 0;
        take_focus();
        return 1;
      }
    }
    return Fl_Widget::handle(e);
  }

 private:
  int hover_ = -1;
};

// ----------------------------------------------------------------- window

class MainWindow : public BufferedWindow {
 public:
  MainWindow(int w, int h) : BufferedWindow(w, h, _("SDE Update")) {}
  void draw() override {
    {
      Painter p(w(), h());
      paint_window_background(p, w(), h());
    }
    draw_children();
  }
  void resize(int X, int Y, int W, int H) override {
    bool changed = W != w() || H != h();
    BufferedWindow::resize(X, Y, W, H);
    if (changed) app().layout();
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD && (Fl::event_state() & FL_CTRL)) {
      int k = Fl::event_key();
      if (k == 'r') {
        app().start("refresh");
        return 1;
      }
      if (k == 'f' && app().page == P_UPDATES) {
        Fl::focus(static_cast<UpdatesPage *>(app().pages[P_UPDATES])->search);
        return 1;
      }
      if (k == 'q' || k == 'w') {
        app().request_close();
        return 1;
      }
      if (k >= '1' && k < '1' + P_COUNT) {
        app().show_page(k - '1');
        return 1;
      }
    }
    return BufferedWindow::handle(e);
  }
};

UpdatesPage *updates_page() { return static_cast<UpdatesPage *>(app().pages[P_UPDATES]); }

// ----------------------------------------------------------- App methods

void App::layout() {
  if (!window) return;
  int W = window->w(), H = window->h();
  int side = W < S(860) ? S(188) : S(224);
  sidebar->resize(0, 0, side, H);
  int strip_h = strip_visible ? S(64) : 0;
  int cx = side, cw = W - side;
  int ph = H - (strip_visible ? strip_h + S(16) : 0);
  for (auto *p : pages) p->resize(cx, 0, cw, ph);
  int sp = cw < S(620) ? S(16) : S(28);
  if (strip_visible) {
    strip->resize(cx + sp, H - strip_h - S(16), cw - 2 * sp, strip_h);
    strip->show();
  } else {
    strip->hide();
  }
  window->redraw();
}

void App::show_page(int index) {
  if (index < 0 || index >= P_COUNT) return;
  page = index;
  for (int i = 0; i < P_COUNT; ++i) {
    if (i == index) pages[i]->show();
    else pages[i]->hide();
  }
  pages[index]->activated();
  window->redraw();
}

void App::changed() {
  updates_page()->sync();
  static_cast<MaintenancePage *>(pages[P_MAINTENANCE])->sync();
  strip->sync();
  window->redraw();
}

void App::tick(void *) {
  App &a = app();
  bool animate = a.busy() || a.loading || a.history_loading || a.sources_loading;
  if (!animate) return;
  a.phase += 1.0 / 22;
  if (a.phase > 1) a.phase -= 1;
  updates_page()->list->phase = a.phase;
  static_cast<HistoryPage *>(a.pages[P_HISTORY])->list->phase = a.phase;
  static_cast<SourcesPage *>(a.pages[P_SOURCES])->list->phase = a.phase;
  a.window->redraw();
  Fl::repeat_timeout(1.0 / 15, tick);
}

void start_ticking() {
  Fl::remove_timeout(App::tick);
  Fl::add_timeout(1.0 / 15, App::tick);
}

void App::rebuild_rows() {
  int keep = -1;
  UpdatesPage *pg = updates_page();
  int sel = pg->list->selected();
  if (sel >= 0 && sel < int(rows.size()) && !rows[size_t(sel)].header) keep = rows[size_t(sel)].index;
  rows.clear();
  for (int k = 0; k < kKindCount; ++k) {
    if (filter >= 0 && filter != k) continue;
    std::vector<int> items;
    for (size_t i = 0; i < list.updates.size(); ++i)
      if (int(list.updates[i].kind) == k && matches(list.updates[i], query)) items.push_back(int(i));
    if (items.empty()) continue;
    rows.push_back({true, Kind(k), -1});
    for (int i : items) rows.push_back({false, Kind(k), i});
  }
  int new_sel = -1;
  for (size_t r = 0; r < rows.size(); ++r)
    if (!rows[r].header && (keep < 0 || rows[r].index == keep)) {
      if (keep >= 0 || new_sel < 0) new_sel = int(r);
      if (keep >= 0) break;
    }
  pg->list->select(-1, false);
  if (keep < 0) pg->list->reset_scroll();
  pg->list->select(new_sel);
  if (new_sel < 0) select_changelog(-1);
  changed();
}

void App::select_changelog(int index) {
  DetailsPane *d = updates_page()->details;
  d->show_update(index);
  if (index < 0) return;
  std::string pkg = list.updates[size_t(index)].main_package;
  if (changelogs.count(pkg) || !valid_package_name(pkg)) return;
  // Wait a moment so that arrowing through the list does not start a
  // download for every row.
  changelog_pending = pkg;
  int request = ++changelog_request;
  struct Req {
    int id;
    std::string pkg;
  };
  Fl::add_timeout(0.35, [](void *data) {
    std::unique_ptr<Req> req(static_cast<Req *>(data));
    App &a = app();
    if (req->id != a.changelog_request) return;
    std::vector<std::string> argv = {kPython, helper("changelog.py"), req->pkg};
    if (!find_program("timeout").empty()) argv.insert(argv.begin(), {"timeout", "45"});
    std::string pkg = req->pkg;
    bool ok = run_async(argv, [pkg](const std::string &out, int status) {
      App &a = app();
      std::string text = out;
      std::string source;
      size_t nl = text.find('\n');
      std::string first = nl == std::string::npos ? text : text.substr(0, nl);
      if (starts_with(first, "SOURCE\t")) {
        source = first.substr(7);
        text = nl == std::string::npos ? "" : text.substr(nl + 1);
      }
      if (status != 0 && status != -1 && text.empty())
        text = _("The changelog could not be downloaded in time. Check your network connection.");
      else if (source == "none" || trim(text).empty())
        text = _("No changelog is available for this update.");
      else if (source == "installed")
        text = std::string(_("The new changelog could not be downloaded. This is the changelog of the "
                             "installed version:")) + "\n\n" + text;
      a.changelogs[pkg] = text;
      if (a.changelog_pending == pkg) a.changelog_pending.clear();
      DetailsPane *d = updates_page()->details;
      if (d->current() && d->current()->main_package == pkg) d->refresh_text();
      d->redraw();
    });
    if (!ok) {
      a.changelogs[pkg] = _("Python 3 is missing; changelogs cannot be shown.");
      a.changelog_pending.clear();
      updates_page()->details->refresh_text();
    }
  }, new Req{request, pkg});
}

void App::load_updates() {
  if (loading) return;
  loading = true;
  load_error.clear();
  reboot = reboot_required(&reboot_packages);
  start_ticking();
  changed();
  bool started = run_async({kPython, helper("check.py")}, [](const std::string &output, int status) {
    App &a = app();
    a.loading = false;
    a.refreshed = lists_mtime();
    UpdateList fresh = parse_updates(output);
    if (status != 0 || !fresh.valid) {
      a.load_error = _("Cannot read the APT package information. Make sure python3-apt is installed and "
                       "your package sources are valid.");
      a.list = UpdateList{};
    } else {
      // Keep the user's selection for updates that are still offered.
      std::set<std::string> off;
      for (auto &u : a.list.updates)
        if (!u.selected) off.insert(u.name + "=" + u.new_version);
      for (auto &u : fresh.updates)
        if (off.count(u.name + "=" + u.new_version)) u.selected = false;
      a.list = std::move(fresh);
    }
    a.loaded_once = true;
    a.rebuild_rows();
    a.maybe_auto_refresh();
  });
  if (!started) {
    loading = false;
    load_error = _("Python 3 is missing. Reinstall the sde package and its dependencies.");
    changed();
  }
}

void App::maybe_auto_refresh() {
  static bool done = false;
  if (done || busy()) return;
  done = true;
  int hours = cfg.get_int("update", "check_interval", 24);
  if (hours <= 0 || !cfg.get_bool("update", "auto_refresh", true)) return;
  // Only PackageKit refreshes without asking for a password.
  if (packagekit_client.empty()) return;
  if (refreshed && std::time(nullptr) - refreshed < std::time_t(hours) * 3600) return;
  start("refresh");
}

void App::load_history(bool force) {
  if (history_loading || (history_loaded && !force)) return;
  history_loading = true;
  auto *page = static_cast<HistoryPage *>(pages[P_HISTORY]);
  page->rebuild();
  start_ticking();
  bool ok = run_async({kPython, helper("history.py")}, [](const std::string &out, int) {
    App &a = app();
    a.history_loading = false;
    a.history_loaded = true;
    parse_history(out, a.history);
    static_cast<HistoryPage *>(a.pages[P_HISTORY])->rebuild();
  });
  if (!ok) {
    history_loading = false;
    page->rebuild();
  }
}

void App::load_sources(bool force) {
  if (sources_loading || (sources_loaded && !force)) return;
  sources_loading = true;
  auto *page = static_cast<SourcesPage *>(pages[P_SOURCES]);
  page->sync();
  start_ticking();
  bool ok = run_async({kPython, helper("sources.py")}, [](const std::string &out, int) {
    App &a = app();
    a.sources_loading = false;
    a.sources_loaded = true;
    parse_sources(out, a.sources);
    static_cast<SourcesPage *>(a.pages[P_SOURCES])->sync();
  });
  if (!ok) {
    sources_loading = false;
    page->sync();
  }
}

void App::load_maintenance() {
  cache_usage(&cache_files, &cache_bytes);
  if (!autoremove_loading && !find_program("apt-get").empty()) {
    autoremove_loading = true;
    run_async({"apt-get", "-s", "-q", "autoremove"}, [](const std::string &out, int status) {
      App &a = app();
      a.autoremove_loading = false;
      a.autoremove_known = status == 0;
      a.autoremove = status == 0 ? parse_autoremove(out) : std::vector<std::string>{};
      a.changed();
    });
  }
  changed();
}

void App::start(const std::string &action, const std::vector<std::string> &packages) {
  if (busy()) return;
  progress = Progress{};
  progress.action = action;
  progress.status = "Starting";
  std::vector<std::string> argv = {kPython, helper("transaction.py"), action};
  argv.insert(argv.end(), packages.begin(), packages.end());
  job = stream_lines(
      argv, [](const std::string &line) {
        App &a = app();
        apply_transaction_line(a.progress, line);
        a.strip->sync();
        a.window->redraw();
      },
      [] { app().finished(); });
  if (!job) {
    progress.finished = true;
    progress.outcome = "failed";
    progress.errors.push_back(_("The transaction helper could not be started. Python 3 is missing."));
  }
  strip_visible = true;
  layout();
  start_ticking();
  changed();
}

void App::cancel() {
  if (!busy() || !progress.cancellable) return;
  progress.cancel_requested = true;
  signal_stream(job, SIGINT);
  changed();
}

void App::finished() {
  job = 0;
  if (!progress.finished) {
    progress.finished = true;
    if (progress.outcome.empty()) progress.outcome = "failed";
  }
  std::string action = progress.action;
  if (action == "refresh" || action == "upgrade" || action == "autoremove" || action == "repair") {
    changelogs.clear();
    load_updates();
  }
  if (action != "refresh") history_loaded = false;
  if (page == P_HISTORY) load_history();
  load_maintenance();
  changed();
}

void App::install_selected() {
  if (busy()) return;
  bool all = false;
  auto packages = selected_packages(list.updates, &all);
  if (packages.empty()) return;
  if (all) packages.clear();  // the whole system: lets PackageKit add new dependencies
  start("upgrade", packages);
}

void App::open_terminal_transaction() {
  if (busy()) return;
  if (find_program("xterm").empty()) {
    inform(window, _("No terminal available"), _("Install xterm to run update transactions in a terminal."));
    return;
  }
  run_async({"xterm", "-T", _("SDE Update"), "-e", "sde-update", "--transaction", "upgrade"},
            [](const std::string &, int) { app().load_updates(); });
}

void App::request_close() {
  if (busy()) {
    inform(window, _("An update is still running"),
           progress.cancellable
               ? _("Wait until it finishes, or cancel it first. Closing now could leave packages half installed.")
               : _("Wait until it finishes. Closing now could leave packages half installed."));
    return;
  }
  window->hide();
}

void minute_tick(void *) {
  App &a = app();
  if (a.window && a.window->shown()) {
    bool reboot = reboot_required(&a.reboot_packages);
    if (reboot != a.reboot) {
      a.reboot = reboot;
      a.changed();
    }
    a.window->redraw();
  }
  Fl::repeat_timeout(60, minute_tick);
}

}  // namespace

int run_app(const std::string &page_name) {
  App &a = app();
  a.cfg.load();
  fl_open_display();
  Theme theme = Theme::from_config(a.cfg);
  theme.apply_to_fltk();
  set_look(Look::from_config(a.cfg));
  IconLoader::set_theme(a.cfg.get("appearance", "icon_theme", "auto"));
  Fl::visible_focus(1);
  a.packagekit_client = is_executable("/usr/bin/pkcon") ? "pkcon" : "";

  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int W = std::min(S(1080), sw - S(24)), H = std::min(S(720), sh - S(60));
  auto *win = new MainWindow(W, H);
  a.window = win;
  a.sidebar = new Sidebar(0, 0, S(224), H);
  a.pages[P_UPDATES] = new UpdatesPage();
  a.pages[P_HISTORY] = new HistoryPage();
  a.pages[P_SOURCES] = new SourcesPage();
  a.pages[P_MAINTENANCE] = new MaintenancePage();
  a.pages[P_SETTINGS] = new SettingsPage();
  a.pages[P_ABOUT] = new AboutPage();
  a.strip = new ActivityStrip();
  win->end();
  win->resizable(nullptr);
  win->size_range(std::min(W, S(720)), std::min(H, S(520)));
  win->xclass("sde-update");
  win->callback([](Fl_Widget *, void *) {
    // Escape clears searches; it should not close the whole window.
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
    app().request_close();
  });
  a.layout();
  int index = P_UPDATES;
  for (int i = 0; i < P_COUNT; ++i)
    if (page_name == kPages[i].id) index = i;
  a.show_page(index);
  win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  win->show();
  a.load_updates();
  a.load_maintenance();
  Fl::add_timeout(60, minute_tick);
  return Fl::run();
}

}  // namespace update
}  // namespace sde
