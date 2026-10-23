// SPDX-License-Identifier: GPL-3.0-or-later
// Software Updater (sde-update): native window over the mintupdate-derived APT model and
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
#include "legal.h"
#include "model.h"
#include "paint.h"
#include "process.h"
#include "spawn.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "x11.h"

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
  if (action == "flatpak") return _("Updating Flatpak applications");
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

// One line of the "Details of updates" tree.
struct Row {
  enum Type { GroupRow, UpdateRow, PackageRow } type = UpdateRow;
  Group group = Group::Main;
  int index = -1;       // into App::list.updates (UpdateRow, PackageRow)
  std::string package;  // PackageRow: a binary package of the update
};

// What the updater window shows.
enum class View { Checking, Available, UpToDate, Error, Installing, Failed, Restart };

class Page;
class UpdaterPage;
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
  int updater_w = 0, updater_h = 0;  // window size of the updater view

  // Updates (APT from check.py, Flatpak from flatpak.py).
  UpdateList list;
  bool loading = false, loaded_once = false, focused_once = false;
  int pending_loads = 0;
  std::string apt_output, flatpak_output;
  int apt_status = -1;
  std::string load_error;
  std::time_t refreshed = 0;
  std::vector<Row> rows;
  bool group_open[kGroupCount] = {true, true, true, true};
  std::set<std::string> open_updates;  // update rows that show their packages
  int current = -1;                    // update in the technical description
  int current_group = -1;              // or a group header
  bool details_open = true, tech_open = false, log_open = true;
  bool reboot = false;
  std::vector<std::string> reboot_packages;
  std::map<std::string, cairo_surface_t *> icons;  // icon name -> surface (or null)

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

  // Install Now: the transactions still to run, and the log of earlier ones.
  std::vector<std::vector<std::string>> queue;
  std::vector<std::string> earlier_log;
  bool installing = false;
  bool reboot_expected = false;
  bool show_failure = false, show_restart = false;
  int install_steps = 0;

  void show_page(int index);
  void layout();
  bool busy() const { return job != 0; }
  void changed();  // redraw everything that shows update state
  View view() const;

  void load_updates();
  void merge_updates();
  void rebuild_rows();
  void select_changelog(int index);
  void load_history(bool force = false);
  void load_sources(bool force = false);
  void load_maintenance();
  void start(const std::string &action, const std::vector<std::string> &packages = {});
  void run_next();
  void cancel();
  void finished();
  void install_selected();
  void remind_later();
  void restart_now();
  void open_terminal_transaction();
  void maybe_auto_refresh();
  void request_close();
  void apply_theme();
  std::string full_log() const;
  cairo_surface_t *icon(const std::string &name, int size);

  static void tick(void *);
};

App &app() {
  static App a;
  return a;
}

std::string update_key(const Update &u) { return u.flatpak ? u.flatpak_item() : u.name; }

Color group_color(Group g) {
  const Theme &t = active_theme();
  switch (g) {
    case Group::Critical: return danger_color();
    case Group::Security: return warning_color();
    case Group::Flatpak: return t.accent.mix(success_color(), 0.5);
    default: return t.accent;
  }
}

View App::view() const {
  if (installing || (busy() && (progress.action == "upgrade" || progress.action == "flatpak")))
    return View::Installing;
  if (show_restart) return View::Restart;
  if (show_failure) return View::Failed;
  if ((busy() && progress.action == "refresh") || (loading && !loaded_once)) return View::Checking;
  if (!load_error.empty()) return View::Error;
  if (list.updates.empty()) return loading ? View::Checking : View::UpToDate;
  return View::Available;
}

std::string App::full_log() const {
  std::vector<std::string> all = earlier_log;
  all.insert(all.end(), progress.log.begin(), progress.log.end());
  return join(all, "\n");
}

cairo_surface_t *App::icon(const std::string &name, int size) {
  if (name.empty()) return nullptr;
  std::string key = name + "@" + std::to_string(size);
  auto it = icons.find(key);
  if (it != icons.end()) return it->second;
  cairo_surface_t *s = nullptr;
  if (!IconLoader::lookup(name, size).empty()) s = IconLoader::get({name}, size);
  icons[key] = s;
  return s;
}

// ------------------------------------------------------------------ pages

void relayout_pages();

class Page : public Fl_Group {
 public:
  Page() : Fl_Group(0, 0, 100, 100) { box(FL_NO_BOX); }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    layout();
  }
  virtual void layout() {}
  virtual void activated() {}
  // Height the page needs, or 0 when it fits any height (lists stretch).
  // Taller pages scroll with the mouse wheel on small screens.
  virtual int content_height() const { return 0; }
  int scroll = 0, view_h = 0;
  int max_scroll() const { return std::max(0, h() - view_h); }
  void draw() override {
    paint();
    draw_children();
    if (max_scroll() > 0) {
      // A thin indicator of the visible part (the page moves, not the bar).
      const Theme &t = active_theme();
      Painter p(window()->w(), window()->h());
      double track = view_h - S(16);
      double th = std::max(double(S(28)), track * view_h / h());
      double ty = scroll + S(8) + (track - th) * scroll / double(max_scroll());
      p.fill_rounded(x() + w() - S(8), y() + ty, S(4), th, S(2), t.text_dim.alpha(0.35));
    }
  }
  int handle(int e) override {
    int r = Fl_Group::handle(e);
    if (!r && e == FL_MOUSEWHEEL && max_scroll() > 0) {
      int next = std::max(0, std::min(max_scroll(), scroll + Fl::event_dy() * S(48)));
      if (next != scroll) {
        scroll = next;
        relayout_pages();
      }
      return 1;
    }
    return r;
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

// "▸ Details of updates": a disclosure line that opens a section.
class Expander : public Fl_Widget {
 public:
  Expander(const std::string &label, bool *state) : Fl_Widget(0, 0, 10, 10), label_(label), state_(state) {}
  std::function<void()> changed;
  int preferred_width() const {
    text_font(active_theme().font_size);
    return text_width(label_) + S(30);
  }
  void draw() override {
    const Theme &t = active_theme();
    bool hot = hover_ || Fl::focus() == this;
    {
      Painter p(window()->w(), window()->h());
      if (Fl::focus() == this) focus_ring(p, x(), y() + S(2), w(), h() - S(4), look().control_radius(h() - S(4)));
      paint_disclosure(p, x() + S(4), y() + (h() - S(12)) / 2.0, S(12), *state_, hot ? t.text : t.text_dim);
    }
    text_font(t.font_size);
    draw_text(label_, x() + S(22), y(), w() - S(24), h(), hot ? t.accent_text : t.text);
  }
  void toggle() {
    *state_ = !*state_;
    if (changed) changed();
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        if (Fl::visible_focus()) take_focus();
        toggle();
        return 1;
      case FL_ENTER:
      case FL_LEAVE:
        hover_ = e == FL_ENTER;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k == ' ' || k == FL_Enter || k == FL_KP_Enter || (k == FL_Right && !*state_) ||
            (k == FL_Left && *state_)) {
          toggle();
          return 1;
        }
        return 0;
      }
    }
    return Fl_Widget::handle(e);
  }

 private:
  std::string label_;
  bool *state_;
  bool hover_ = false;
};

// "Technical description": versions, origin, size and the changelog of the
// selected update (or a summary of the selected group).
class TechPane : public Fl_Group {
 public:
  TechPane() : Fl_Group(0, 0, 10, 10) {
    box(FL_NO_BOX);
    text = new TextView(0, 0, 10, 10, false);
    end();
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    layout();
  }
  const Update *current() const {
    App &a = app();
    if (a.current < 0 || a.current >= int(a.list.updates.size())) return nullptr;
    return &a.list.updates[size_t(a.current)];
  }
  std::vector<std::pair<std::string, std::string>> facts() const {
    App &a = app();
    std::vector<std::pair<std::string, std::string>> f;
    const Update *u = current();
    if (!u && a.current_group >= 0) {
      Group g = Group(a.current_group);
      int n = 0, on = 0;
      long long sel = 0;
      for (auto &x : a.list.updates)
        if (x.group == g) {
          ++n;
          if (x.selected) {
            ++on;
            sel += x.size;
          }
        }
      f.push_back({_("Updates"), format(_("%d selected of %d"), on, n)});
      f.push_back({_("Download"), format(_("%s of %s"), format_size(sel).c_str(), format_size(a.list.size(g)).c_str())});
      return f;
    }
    if (!u) return f;
    std::string versions = u->old_version.empty() ? u->new_version : u->old_version + "  →  " + u->new_version;
    if (u->flatpak) {
      f.push_back({_("Reference"), u->ref});
      f.push_back({_("Version"), versions.empty() ? u->branch : versions});
      f.push_back({_("Remote"), u->origin + "  ·  " + (u->installation == "user" ? _("user installation")
                                                                                  : _("system installation"))});
      f.push_back({_("Download"), format_size(u->size)});
      return f;
    }
    f.push_back({_("Package"), u->packages.size() > 1
                                   ? format(_("%s and %d more"), u->main_package.c_str(), int(u->packages.size()) - 1)
                                   : u->main_package});
    f.push_back({_("Version"), versions});
    std::string origin = u->origin;
    if (!u->archive.empty()) origin += (origin.empty() ? "" : " · ") + u->archive;
    if (!u->site.empty()) origin += (origin.empty() ? "" : " · ") + u->site;
    if (!origin.empty()) f.push_back({_("Origin"), origin});
    std::string size = format_size(u->size);
    if (u->reboot) size += "  ·  " + std::string(_("needs a restart"));
    f.push_back({_("Download"), size});
    return f;
  }
  // On short screens the facts go into the scrolling text with the changelog.
  int full_info_height() const { return S(34) + int(facts().size()) * S(20) + S(6); }
  int info_height() const { return compact_ && current() ? S(34) : full_info_height(); }
  void layout() {
    bool compact = h() < full_info_height() + S(84);
    if (compact != compact_) {
      compact_ = compact;
      set_text();
    }
    int top = y() + info_height();
    int room = y() + h() - top - S(8);
    text->resize(x() + S(10), top, w() - S(20), std::max(S(30), room));
    if (!current() || room < S(36)) text->hide();
    else text->show();
  }
  void set_text() {
    App &a = app();
    const Update *u = current();
    std::string facts_text;
    if (u && compact_)
      for (auto &f : facts()) facts_text += f.first + ":  " + f.second + "\n";
    if (!facts_text.empty()) facts_text += "\n";
    if (!u) {
      text->set("");
    } else if (u->flatpak) {
      std::string body = u->summary.empty() ? std::string(_("No description is available.")) : u->summary;
      text->set(facts_text + body + "\n\n" +
                _("Flatpak applications are updated from their remote; their changes are "
                  "published by the application developers."));
    } else {
      auto it = a.changelogs.find(u->main_package);
      // The long description, unless it only repeats the summary.
      std::string d = trim(u->description);
      if (!d.empty() && d.back() == '.') d.pop_back();
      std::string head = d.empty() || lower(d) == lower(trim(u->summary)) ? "" : u->description + "\n\n";
      text->set(facts_text + head +
                (it != a.changelogs.end() ? it->second : std::string(_("Loading the changelog…"))));
    }
  }
  void refresh() {
    set_text();
    layout();
    redraw();
  }
  bool compact_ = false;
  void draw() override {
    const Theme &t = active_theme();
    App &a = app();
    const Update *u = current();
    int px = x() + S(12), pw = w() - S(24);
    std::string title;
    std::vector<std::pair<std::string, Color>> badges;
    if (u) {
      title = u->title();
      badges.push_back({group_badge(u->group), group_color(u->group)});
      if (!u->archive.empty()) badges.push_back({u->archive, t.text_dim});
    } else if (a.current_group >= 0) {
      title = group_title(Group(a.current_group));
    }
    fl_push_clip(x(), y(), w(), h());
    {
      Painter p(window()->w(), window()->h());
      paint_card(p, x(), y(), w(), h());
      int bx = x() + w() - S(12);
      for (auto it = badges.rbegin(); it != badges.rend(); ++it) {
        int bw = badge_width(it->first);
        bx -= bw;
        paint_badge(p, bx, y() + S(8), bw, S(20), it->second);
        bx -= S(6);
      }
      if (text->visible()) p.fill_rect(text->x(), text->y(), text->w(), text->h(), t.surface);
    }
    if (title.empty()) {
      text_font(t.font_size - S(1));
      draw_text(_("Select an update to see its technical description."), px, y(), pw, std::min(h(), S(40)), t.text_dim);
      fl_pop_clip();
      return;
    }
    int bx = x() + w() - S(12), bw_total = 0;
    for (auto it = badges.rbegin(); it != badges.rend(); ++it) {
      int bw = badge_width(it->first);
      bx -= bw;
      draw_badge_text(it->first, bx, y() + S(8), bw, S(20), it->second);
      bx -= S(6);
      bw_total += bw + S(6);
    }
    text_font(t.font_size, true);
    draw_text(title, px, y() + S(6), pw - bw_total - S(8), S(24), t.text);
    auto f = facts();
    if (compact_ && u) f.clear();
    int lw = std::min(S(110), pw / 3), fy = y() + S(32);
    text_font(t.font_size - S(1));
    for (auto &row : f) {
      draw_text(row.first, px, fy, lw, S(20), t.text_dim);
      draw_text(row.second, px + lw, fy, pw - lw, S(20), t.text);
      fy += S(20);
    }
    draw_children();
    fl_pop_clip();
  }
  TextView *text;
};

// The Software Updater view: the question, the update tree, the technical
// description, the download size and the buttons; progress and results of
// Install Now inline.
class UpdaterPage : public Page {
 public:
  UpdaterPage() {
    details = new Expander(_("Details of updates"), &app().details_open);
    // On short screens the two sections take turns (an accordion).
    details->changed = [] {
      App &a = app();
      if (a.details_open && updater()->short_screen()) a.tech_open = false;
      a.changed();
    };
    list = new ListView(0, 0, 10, 10);
    list->framed = false;
    setup_list();
    tech_exp = new Expander(_("Technical description"), &app().tech_open);
    tech_exp->changed = [] {
      App &a = app();
      if (a.tech_open && updater()->short_screen()) a.details_open = false;
      if (a.tech_open && a.current >= 0) a.select_changelog(a.current);
      a.changed();
    };
    tech = new TechPane();
    log_exp = new Expander(_("Details"), &app().log_open);
    log_exp->changed = [] { app().changed(); };
    log = new TextView(0, 0, 10, 10);
    settings = new Button(0, 0, 10, 10, _("Settings…"));
    settings->callback([](Fl_Widget *, void *) { app().show_page(P_SETTINGS); });
    remind = new Button(0, 0, 10, 10, _("Remind Me Later"));
    remind->callback([](Fl_Widget *, void *) { app().remind_later(); });
    install = new Button(0, 0, 10, 10, _("Install Now"), Button::Primary);
    install->callback([](Fl_Widget *, void *) { app().install_selected(); });
    check = new Button(0, 0, 10, 10, _("Check Again"));
    check->callback([](Fl_Widget *, void *) {
      App &a = app();
      a.show_failure = false;
      a.start("refresh");
    });
    close = new Button(0, 0, 10, 10, _("OK"), Button::Primary);
    close->callback([](Fl_Widget *, void *) { app().request_close(); });
    cancel = new Button(0, 0, 10, 10, _("Cancel"));
    cancel->callback([](Fl_Widget *, void *) { app().cancel(); });
    back = new Button(0, 0, 10, 10, _("Back to Updates"));
    back->callback([](Fl_Widget *, void *) {
      App &a = app();
      a.show_failure = false;
      a.show_restart = false;
      a.changed();
    });
    retry = new Button(0, 0, 10, 10, _("Try Again"), Button::Primary);
    retry->callback([](Fl_Widget *, void *) {
      App &a = app();
      a.show_failure = false;
      a.install_selected();
    });
    later = new Button(0, 0, 10, 10, _("Later"));
    later->callback([](Fl_Widget *, void *) {
      App &a = app();
      a.show_restart = false;
      a.changed();
    });
    restart_now = new Button(0, 0, 10, 10, _("Restart Now"), Button::Primary);
    restart_now->callback([](Fl_Widget *, void *) { app().restart_now(); });
    restart_small = new Button(0, 0, 10, 10, _("Restart…"), Button::Ghost);
    restart_small->callback([](Fl_Widget *, void *) { app().restart_now(); });
    end();
  }

  static int group_state(Group g) {
    int on = 0, total = 0;
    for (auto &u : app().list.updates)
      if (u.group == g) {
        ++total;
        on += u.selected;
      }
    return on == 0 ? 0 : on == total ? 1 : 2;
  }
  static void toggle_group(Group g) {
    App &a = app();
    bool any_off = group_state(g) != 1;
    for (auto &u : a.list.updates)
      if (u.group == g) u.selected = any_off;
    a.changed();
  }
  static void toggle_open(const Row &r, int force = -1) {
    App &a = app();
    if (r.type == Row::GroupRow) {
      bool &open = a.group_open[int(r.group)];
      open = force < 0 ? !open : force == 1;
    } else if (r.type == Row::UpdateRow) {
      const Update &u = a.list.updates[size_t(r.index)];
      if (u.packages.size() < 2) return;
      std::string key = update_key(u);
      bool open = a.open_updates.count(key);
      bool want = force < 0 ? !open : force == 1;
      if (want) a.open_updates.insert(key);
      else a.open_updates.erase(key);
    }
    a.rebuild_rows();
  }

  void setup_list() {
    list->count = [] { return int(app().rows.size()); };
    list->row_height = [](int i) { return app().rows[size_t(i)].type == Row::PackageRow ? S(24) : S(30); };
    list->paint_row = [](Painter &p, int i, int x, int y, int w, int h, bool sel, bool hover) {
      const Theme &t = active_theme();
      App &a = app();
      const Row &r = a.rows[size_t(i)];
      double rr = std::min<double>(look().control_radius(h - S(2)), S(8));
      if (sel) p.fill_rounded(x + S(2), y + S(1), w - S(4), h - S(2), rr, t.selection_bg.alpha(t.dark ? 0.55 : 0.35));
      else if (hover) p.fill_rounded(x + S(2), y + S(1), w - S(4), h - S(2), rr, t.hover.alpha(0.8));
      if (r.type == Row::GroupRow) {
        paint_disclosure(p, x + S(8), y + (h - S(12)) / 2.0, S(12), a.group_open[int(r.group)], t.text_dim);
        paint_checkbox(p, x + S(26), y + (h - S(16)) / 2.0, S(16), group_state(r.group), false);
        return;
      }
      if (r.type == Row::PackageRow) {
        p.circle(x + S(104), y + h / 2.0, S(2), t.text_dim.alpha(0.7));
        return;
      }
      const Update &u = a.list.updates[size_t(r.index)];
      if (u.packages.size() > 1)
        paint_disclosure(p, x + S(30), y + (h - S(10)) / 2.0, S(10), a.open_updates.count(update_key(u)) != 0,
                         t.text_dim);
      paint_checkbox(p, x + S(48), y + (h - S(16)) / 2.0, S(16), u.selected ? 1 : 0, hover);
      int is = S(20);
      cairo_surface_t *img = a.icon(u.icon, is);
      if (!img && u.flatpak) img = a.icon(u.application, is);
      if (!img) img = a.icon(u.flatpak ? "application-x-executable" : "package-x-generic", is);
      if (img) p.image(img, x + S(72), y + (h - is) / 2.0, is);
      else paint_icon(p, Icon::Package, x + S(72), y + (h - is) / 2.0, is, t.text_dim);
    };
    list->text_row = [](int i, int x, int y, int w, int h, bool sel, bool) {
      const Theme &t = active_theme();
      App &a = app();
      const Row &r = a.rows[size_t(i)];
      Color fg = sel ? t.text : t.text;
      int right = S(86);
      if (r.type == Row::GroupRow) {
        int n = a.list.count(r.group);
        text_font(t.font_size, true);
        std::string title = group_title(r.group);
        int tw = std::min(text_width(title), w - S(50) - right - S(40));
        draw_text(title, x + S(50), y, tw, h, fg);
        text_font(t.font_size - S(1));
        draw_text(format("%d", n), x + S(58) + tw, y, S(40), h, t.text_dim);
        text_font(t.font_size - S(1), true);
        draw_text(format_size(a.list.size(r.group)), x + w - right - S(8), y, right, h, fg, 2);
        return;
      }
      if (r.type == Row::PackageRow) {
        text_font(t.font_size - S(2));
        draw_text(r.package, x + S(112), y, w - S(112) - right - S(8), h, t.text_dim);
        return;
      }
      const Update &u = a.list.updates[size_t(r.index)];
      text_font(t.font_size);
      draw_text(u.title(), x + S(100), y, w - S(100) - right - S(14), h, fg);
      text_font(t.font_size - S(1));
      draw_text(format_size(u.size), x + w - right - S(8), y, right, h, t.text_dim, 2);
    };
    list->clicked = [](int i, int rx, int clicks) {
      App &a = app();
      if (i < 0 || i >= int(a.rows.size())) return;
      Row r = a.rows[size_t(i)];
      if (r.type == Row::GroupRow) {
        if (rx >= S(22) && rx < S(46)) toggle_group(r.group);
        else if (rx < S(22) || clicks == 2) toggle_open(r);
      } else if (r.type == Row::UpdateRow) {
        auto &u = a.list.updates[size_t(r.index)];
        if (rx >= S(26) && rx < S(44) && u.packages.size() > 1) {
          toggle_open(r);
        } else if ((rx >= S(44) && rx < S(68)) || clicks == 2) {
          u.selected = !u.selected;
          a.changed();
        }
      }
    };
    list->toggled = [](int i) {
      App &a = app();
      const Row &r = a.rows[size_t(i)];
      if (r.type == Row::GroupRow) {
        toggle_group(r.group);
      } else if (r.type == Row::UpdateRow) {
        auto &u = a.list.updates[size_t(r.index)];
        u.selected = !u.selected;
        a.changed();
      }
    };
    list->key_pressed = [this](int i, int key) {
      App &a = app();
      Row r = a.rows[size_t(i)];
      if (key == FL_Right && r.type != Row::PackageRow) {
        toggle_open(r, 1);
        return true;
      }
      if (key == FL_Left) {
        if (r.type == Row::PackageRow || (r.type == Row::UpdateRow && !a.open_updates.count(update_key(a.list.updates[size_t(r.index)])))) {
          // Up to the parent row.
          for (int j = i - 1; j >= 0; --j) {
            const Row &p = a.rows[size_t(j)];
            if ((r.type == Row::PackageRow && p.type == Row::UpdateRow) ||
                (r.type == Row::UpdateRow && p.type == Row::GroupRow)) {
              list->select(j);
              break;
            }
          }
          return true;
        }
        toggle_open(r, 0);
        return true;
      }
      return false;
    };
    list->selection_changed = [](int i) {
      App &a = app();
      a.current = -1;
      a.current_group = -1;
      if (i >= 0 && i < int(a.rows.size())) {
        const Row &r = a.rows[size_t(i)];
        if (r.type == Row::GroupRow) a.current_group = int(r.group);
        else a.current = r.index;
      }
      if (a.tech_open && a.current >= 0) a.select_changelog(a.current);
      else updater()->tech->refresh();
    };
    list->empty_icon = Icon::Shield;
  }
  static UpdaterPage *updater() { return static_cast<UpdaterPage *>(app().pages[P_UPDATES]); }

  int P() const { return h() < S(460) ? S(10) : S(16); }
  bool short_screen() const { return h() < S(480); }

  // Header text for the current view.
  void header_text(View v, std::string &title, std::string &sub) const {
    App &a = app();
    std::string age = format(_("Last checked %s"), format_age(a.refreshed, std::time(nullptr)).c_str());
    switch (v) {
      case View::Checking:
        title = _("Checking for updates…");
        sub = a.busy() ? status_text(a.progress.status) : std::string(_("Reading the package information"));
        break;
      case View::Error:
        title = _("Software information could not be read.");
        sub = a.load_error;
        break;
      case View::UpToDate:
        title = _("The software on this computer is up to date.");
        sub = age;
        if (!a.list.held.empty())
          sub += "  ·  " + format(_("%d held back by the administrator"), int(a.list.held.size()));
        break;
      case View::Installing:
        title = _("Installing updates…");
        sub = a.install_steps > 1 ? format(_("Step %d of %d: %s"), a.install_steps - int(a.queue.size()),
                                           a.install_steps, action_title(a.progress.action).c_str())
                                  : action_title(a.progress.action);
        break;
      case View::Failed: {
        const Progress &p = a.progress;
        if (p.outcome == "cancelled") title = _("Installing the updates was cancelled.");
        else if (p.outcome == "denied") title = _("The updates were not installed: authentication failed.");
        else title = _("The updates could not be installed.");
        if (!p.errors.empty()) sub = p.errors.back();
        else if (p.outcome == "denied") sub = _("Authentication was cancelled or refused.");
        else if (p.outcome == "cancelled") sub = _("Some updates may have been installed already.");
        else sub = format(_("Exit code %d. Open Details for the full log."), p.exit_code);
        break;
      }
      case View::Restart:
        title = _("The computer needs to restart to finish installing updates.");
        sub = _("Save your work before restarting.");
        if (!a.reboot_packages.empty()) sub += "  " + format(_("Requested by %s."), join(a.reboot_packages, ", ").c_str());
        break;
      case View::Available: {
        title = _("Updated software is available for this computer. Do you want to install it now?");
        std::vector<std::string> parts;
        int n = int(a.list.updates.size());
        parts.push_back(n == 1 ? std::string(_("1 update")) : format(_("%d updates"), n));
        if (int c = a.list.count(Group::Critical)) parts.push_back(format(_("%d critical"), c));
        if (int s = a.list.count(Group::Security)) parts.push_back(format(_("%d security"), s));
        if (int f = a.list.count(Group::Flatpak)) parts.push_back(format(_("%d Flatpak"), f));
        if (!a.list.held.empty()) parts.push_back(format(_("%d held back"), int(a.list.held.size())));
        sub = join(parts, "  ·  ") + "  ·  " + age;
        break;
      }
    }
  }
  int header_height(View v) const {
    const Theme &t = active_theme();
    std::string title, sub;
    header_text(v, title, sub);
    int tw = w() - 2 * P() - S(62);
    text_font(t.font_size + S(2), true);
    int th = std::max(S(22), std::min(S(60), wrapped_height(title, tw)));
    return std::max(S(48), th + S(4) + S(20));
  }

  void place(Button *b, int x, int y, int min_w) {
    b->size(std::max(min_w, b->preferred_width()), S(34));
    b->position(x, y);
    want_.insert(b);
  }

  void layout() override {
    App &a = app();
    View v = a.view();
    int p = P(), X = x() + p, W = w() - 2 * p;
    int by = y() + h() - p - S(34);
    all_.clear();
    for (Fl_Widget *wd : std::initializer_list<Fl_Widget *>{
             details, list, tech_exp, tech, log_exp, log, settings, remind, install, check, close, cancel, back, retry,
             later, restart_now, restart_small})
      all_.push_back(wd);
    want_.clear();
    header_y = y() + p;
    header_h = header_height(v);
    int cy = header_y + header_h + S(8);
    banner = a.reboot && v != View::Restart && v != View::Installing;
    if (banner) {
      banner_y = cy;
      restart_small->size(restart_small->preferred_width(), S(28));
      restart_small->position(X + W - S(6) - restart_small->w(), cy + S(3));
      want_.insert(restart_small);
      cy += S(34) + S(8);
    }
    progress_y = -1;
    download_y = -1;
    tree_y = tree_h = 0;
    log_h = 0;
    // Bottom buttons.
    auto right_to_left = [&](std::initializer_list<Button *> buttons) {
      int bx = X + W;
      for (Button *b : buttons) {
        b->size(std::max(S(96), b->preferred_width()), S(34));
        bx -= b->w();
        b->position(bx, by);
        want_.insert(b);
        bx -= S(8);
      }
    };
    if (v == View::Available) {
      place(settings, X, by, S(96));
      right_to_left({install, remind});
      int line_h = S(26);
      // On short screens the download size goes between the buttons when it
      // fits, and the tree has no column header: more rows stay visible.
      int free_x = settings->x() + settings->w() + S(12), free_w = remind->x() - S(12) - free_x;
      text_font(active_theme().font_size);
      download_inline = short_screen() && text_width(download_text(true)) + S(30) <= free_w;
      download_x = download_inline ? free_x : X;
      download_w = download_inline ? free_w : W;
      download_y = download_inline ? by + (S(34) - line_h) / 2 : by - S(8) - line_h;
      int exp_h = S(26);
      int bottom = download_inline ? by - S(8) : download_y - S(4);
      int avail = bottom - exp_h - S(4) - cy - exp_h - S(4);
      tree_head = short_screen() ? 0 : S(26);
      int tech_h = 0;
      if (a.details_open && a.tech_open) {
        tree_h = std::max(S(84), avail * 55 / 100);
        tech_h = std::max(S(60), avail - tree_h - S(6));
        if (tree_h + tech_h + S(6) > avail) tree_h = std::max(S(60), avail - tech_h - S(6));
      } else if (a.details_open) {
        tree_h = std::max(S(60), avail);
      } else if (a.tech_open) {
        tech_h = std::max(S(60), avail);
      }
      details->resize(X, cy, std::min(W, details->preferred_width()), exp_h);
      want_.insert(details);
      cy += exp_h + S(4);
      if (a.details_open) {
        tree_y = cy;
        list->resize(X + S(3), cy + (tree_head ? tree_head : S(3)), W - S(6), tree_h - tree_head - S(6) + (tree_head ? S(3) : 0));
        want_.insert(list);
        cy += tree_h + S(6);
      }
      tech_exp->resize(X, cy, std::min(W, tech_exp->preferred_width()), exp_h);
      want_.insert(tech_exp);
      cy += exp_h + S(4);
      if (a.tech_open) {
        tech->resize(X, cy, W, tech_h);
        want_.insert(tech);
      }
    } else if (v == View::Checking || v == View::Installing || v == View::Failed || v == View::Restart) {
      if (v == View::Checking || v == View::Installing) {
        progress_y = cy;
        cy += S(70);
      }
      bool has_log = !a.progress.log.empty() || !a.earlier_log.empty() || v == View::Installing;
      if (has_log) {
        log_exp->resize(X, cy, std::min(W, log_exp->preferred_width()), S(26));
        want_.insert(log_exp);
        cy += S(30);
        if (a.log_open) {
          log_y = cy;
          log_h = std::max(S(40), by - S(10) - cy);
          log->resize(X + S(6), cy + S(6), W - S(12), log_h - S(12));
          want_.insert(log);
        }
      }
      if (v == View::Checking) {
        place(settings, X, by, S(96));
        if (a.busy() && a.progress.cancellable) right_to_left({cancel});
      } else if (v == View::Installing) {
        right_to_left({cancel});
        if (a.progress.cancellable && !a.progress.cancel_requested) cancel->activate();
        else cancel->deactivate();
      } else if (v == View::Failed) {
        place(settings, X, by, S(96));
        right_to_left({retry, back});
      } else {
        right_to_left({restart_now, later});
      }
    } else {  // UpToDate, Error
      place(settings, X, by, S(96));
      right_to_left({close, check});
    }
    if (a.busy() && v != View::Installing) check->deactivate();
    else check->activate();
    if (a.busy() || a.list.selected_count() == 0) install->deactivate();
    else install->activate();
    // Only change what changes, so the focused control keeps the focus.
    for (Fl_Widget *wd : all_) {
      if (!want_.count(wd)) wd->hide();
      else if (!wd->visible()) wd->show();
    }
  }
  std::vector<Fl_Widget *> all_;
  std::set<Fl_Widget *> want_;

  void sync() {
    App &a = app();
    log->set(a.full_log());
    log->scroll_to_end();
    tech->refresh();
    layout();
    redraw();
  }

  void paint() override {
    const Theme &t = active_theme();
    App &a = app();
    View v = a.view();
    int p = P(), X = x() + p, W = w() - 2 * p;
    std::string title, sub;
    header_text(v, title, sub);
    int is = S(48), tx = X + is + S(14), tw = W - is - S(14);
    {
      Painter pp(window()->w(), window()->h());
      bool problem = v == View::Error || v == View::Failed;
      if (problem || v == View::Restart) {
        Color c = problem ? danger_color() : warning_color();
        pp.fill_rounded(X, header_y, is, is, look().card_radius(), c.alpha(t.dark ? 0.22 : 0.14));
        paint_icon(pp, problem ? Icon::Warning : Icon::Restart, X + S(10), header_y + S(10), S(28),
                   readable_on(c, t.background));
      } else {
        pp.image(IconLoader::get({"system-software-update", "software-update-available", "sde-logo"}, is), X,
                 header_y, is);
      }
      if (banner) {
        double r = look().card_radius();
        Color wc = warning_color();
        pp.fill_rounded(X, banner_y, W, S(34), r, wc.alpha(t.dark ? 0.18 : 0.12));
        pp.stroke_rounded(X, banner_y, W, S(34), r, wc.alpha(0.45));
        paint_icon(pp, Icon::Restart, X + S(10), banner_y + S(8), S(18), readable_on(wc, t.background));
      }
      if (tree_h > 0) {
        paint_card(pp, X, tree_y, W, tree_h);
        if (tree_head) pp.line(X + S(8), tree_y + tree_head + 0.5, X + W - S(8), tree_y + tree_head + 0.5, t.border);
      }
      if (log_h > 0 && a.log_open) paint_card(pp, X, log_y, W, log_h);
      if (progress_y >= 0) {
        paint_progress(pp, X, progress_y + S(4), W, S(8), a.busy() ? a.progress.percent : -1, a.phase);
      }
      if (download_y >= 0)
        paint_icon(pp, Icon::Package, download_x + S(2), download_y + S(4), S(18), t.accent_text);
    }
    text_font(t.font_size + S(2), true);
    int th = std::max(S(22), std::min(S(60), wrapped_height(title, tw)));
    draw_wrapped(title, tx, header_y, tw, th, t.text);
    text_font(t.font_size - S(1));
    draw_text(sub, tx, header_y + th + S(4), tw, S(20), v == View::Failed ? danger_color() : t.text_dim);
    if (banner) {
      std::string msg = _("Restart to finish installing earlier updates");
      if (!a.reboot_packages.empty()) msg += "  ·  " + join(a.reboot_packages, ", ");
      text_font(t.font_size - S(1), true);
      draw_text(msg, X + S(36), banner_y, restart_small->x() - X - S(44), S(34), t.text);
    }
    if (tree_h > 0 && tree_head) {
      text_font(t.font_size - S(1));
      draw_text(_("Install"), X + S(12), tree_y, W / 2, S(26), t.text_dim);
      draw_text(_("Download"), X + W / 2, tree_y, W / 2 - S(12), S(26), t.text_dim, 2);
    }
    if (progress_y >= 0) {
      const Progress &pr = a.progress;
      std::string status = a.busy() ? status_text(pr.status) : std::string(_("Reading the package information"));
      if (a.busy() && pr.percent >= 0) status += format("  ·  %d%%", pr.percent);
      if (pr.cancel_requested) status = _("Cancelling…");
      text_font(t.font_size);
      draw_text(status, X, progress_y + S(16), W, S(24), t.text);
      std::string item;
      if (a.busy() && !pr.item_name.empty())
        item = pr.item_info + " " + pr.item_name + (pr.item_version.empty() ? "" : " " + pr.item_version);
      if (a.busy() && pr.items_seen > 1) item += (item.empty() ? "" : "  ·  ") + format(_("%d packages"), pr.items_seen);
      text_font(t.font_size - S(1));
      draw_text(item, X, progress_y + S(40), W, S(22), t.text_dim);
    }
    if (download_y >= 0) {
      text_font(t.font_size);
      draw_text(download_text(download_inline), download_x + S(28), download_y, download_w - S(28), S(26), t.text);
    }
  }

  // "80.0 MB will be downloaded." (|brief| leaves out the selection and restart).
  std::string download_text(bool brief) const {
    App &a = app();
    long long size = a.list.selected_size();
    int n = a.list.selected_count();
    std::string line;
    if (!n) line = _("No updates are selected.");
    else if (!size) line = _("Nothing needs to be downloaded.");
    else line = format(_("%s will be downloaded."), format_size(size).c_str());
    if (brief) return line;
    if (n && n < int(a.list.updates.size()))
      line += "  " + format(_("(%d of %d updates selected)"), n, int(a.list.updates.size()));
    if (n && a.list.needs_reboot()) line += "  ·  " + std::string(_("A restart will be required."));
    return line;
  }

  void activated() override {
    if (list->visible()) Fl::focus(list);
  }

  Expander *details, *tech_exp, *log_exp;
  ListView *list;
  TechPane *tech;
  TextView *log;
  Button *settings, *remind, *install, *check, *close, *cancel, *back, *retry, *later, *restart_now, *restart_small;
  int header_y = 0, header_h = 0, banner_y = 0, tree_y = 0, tree_h = 0, progress_y = -1, download_y = -1;
  int log_y = 0, log_h = 0, tree_head = 0, download_x = 0, download_w = 0;
  bool download_inline = false;
  bool banner = false;
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
  int content_height() const override {
    return heading_bottom() - y() + int(actions.size()) * (S(78) + S(10)) + pad();
  }
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
    remind = new Fl_Choice(0, 0, 10, 10);
    style_choice(remind);
    int rh = a.cfg.get_int("update", "remind_hours", 24), rindex = 2;
    for (size_t i = 0; i < remind_hours().size(); ++i) {
      remind->add(menu_escape(remind_label(remind_hours()[i])).c_str());
      if (remind_hours()[i] == rh) rindex = int(i);
    }
    remind->value(rindex);
    remind->callback([](Fl_Widget *w, void *) {
      int v = static_cast<Fl_Choice *>(w)->value();
      if (v >= 0) save("remind_hours", std::to_string(remind_hours()[size_t(v)]));
    });
    end();
  }
  int content_height() const override {
    return heading_bottom() - y() + S(16) + int(rows().size()) * row_h() + S(16) + note_h() + pad();
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
        {_("Only for security updates"), _("Notify only when at least one critical or security update is available.")},
        {_("Remind Me Later"), _("When to notify again after you choose Remind Me Later.")},
    };
  }
  int row_h() const { return S(62); }
  void layout() override {
    int p = pad(), X = x() + p, W = std::min(w() - 2 * p, S(760));
    card_x = X;
    card_w = W;
    card_y = heading_bottom();
    Fl_Widget *controls[] = {interval, refresh, notify, security, remind};
    int cw = std::min(S(200), W / 3);
    for (int i = 0; i < 5; ++i) {
      int ry = card_y + S(8) + i * row_h();
      if (i == 0 || i == 4) controls[i]->resize(X + W - S(20) - cw, ry + (row_h() - S(32)) / 2, cw, S(32));
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
      paint_card(pp, card_x, card_y + ch + S(16), card_w, note_h());
      paint_icon(pp, Icon::Info, card_x + S(20), card_y + ch + S(16) + S(20), S(22), t.accent_text);
    }
    for (size_t i = 0; i < r.size(); ++i) {
      int ry = card_y + S(8) + int(i) * row_h();
      int tw = card_w - S(40) - (i == 0 || i == 4 ? std::min(S(200), card_w / 3) + S(20) : S(68));
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
    draw_wrapped(note(), card_x + S(54), ny + S(42), card_w - S(74), note_h() - S(48), t.text_dim);
  }
  static const char *note() {
    return _("Automatic checks only read package information. Installing always asks for "
             "administrator rights through PackageKit or pkexec. Settings are saved "
             "automatically in the [update] section of sde.conf.");
  }
  int note_h() const {
    text_font(active_theme().font_size - S(1));
    int cw = std::min(w() - 2 * pad(), S(760));
    return S(54) + wrapped_height(note(), cw - S(74));
  }
  Fl_Choice *interval;
  Fl_Choice *remind;
  Switch *refresh, *notify, *security;
  int card_x = 0, card_y = 0, card_w = 0;
};

// ------------------------------------------------------------------ about

AppInfo about_info() {
  AppInfo info;
  info.name = _("Software Updater");
  info.program = "sde-update";
  info.version = SDE_UPDATE_VERSION;
  info.icon = "sde-software-updater";
  info.icon_fallbacks = {"system-software-update"};
  info.description = _("Keeps the operating system and its applications up to date: critical, security, "
                       "main and Flatpak updates from the configured sources.");
  // mintupdate (GPL-3.0-or-later, Clement Lefebvre) is credited from the
  // legal data; PackageKit and APT are Debian's.
  info.credits = {_("Transactions by PackageKit or APT from Debian"), toolkit_credit()};
  return info;
}

class AboutPage : public Page {
 public:
  AboutPage() {
    upstream = new Button(0, 0, 10, 10, _("mintupdate on GitHub"));
    upstream->callback([](Fl_Widget *, void *) { open_uri("https://github.com/linuxmint/mintupdate"); });
    copyright = new Button(0, 0, 10, 10, _("Copyright and License"));
    copyright->callback([](Fl_Widget *, void *) { show_license(about_info()); });
    about = new Button(0, 0, 10, 10, _("About Software Updater"));
    about->callback([](Fl_Widget *, void *) { show_about(about_info()); });
    end();
  }
  // Copyright and the trademark notices of the names on this page (SDE,
  // Linux in "Linux Mint", Debian), below the Components card.
  std::string legal_text() const {
    std::string text = std::string(kCopyright) + " \xC2\xB7 " + license_name("GPL-3.0-or-later");
    for (const auto &line : legal::trademarks_for(legal::find("sde-update"))) text += "\n" + line;
    return text;
  }
  int legal_height() const {
    text_font(active_theme().font_size - S(1));
    int W = std::min(w() - 2 * pad(), S(760)) - S(4);
    return wrapped_height(legal_text(), W) + S(8);
  }
  int content_height() const override {
    return S(186) + S(150) + S(14) + S(120) + S(14) + legal_height() + pad();
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
      pp.image(IconLoader::get({"sde-software-updater", "system-software-update", "software-update-available"},
                               S(72)),
               X, y() + S(28), S(72));
      paint_card(pp, X, y() + S(126), W, S(210) - S(10));
      paint_card(pp, X, y() + S(186) + S(150) + S(14), W, S(120));
    }
    text_font(t.font_size + S(10), true);
    draw_text(_("Software Updater"), X + S(92), y() + S(32), W - S(92), S(38), t.text);
    text_font(t.font_size);
    draw_text(format(_("Version %s · Critical, security, main and Flatpak updates"), SDE_UPDATE_VERSION), X + S(92),
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
    text_font(t.font_size - S(1));
    int ly = sy + S(120) + S(14);
    draw_wrapped(legal_text(), X + S(2), ly, W - S(4), legal_height(), t.text_dim);
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
  // Compact on short screens (1024x600 at 150%).
  bool compact() const { return h() < S(470); }
  int row_h() const { return compact() ? S(34) : S(42); }
  int row_y(int i) const {
    return y() + (compact() ? S(66) : S(86)) + i * row_h() + (i >= P_SETTINGS ? (compact() ? S(16) : S(20)) : 0);
  }
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
    if (text_width(_("Software Updater")) > w() - S(70)) text_font(t.font_size, true);
    draw_text(_("Software Updater"), x() + S(64), y() + S(24), w() - S(70), S(34), t.text);
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
    // The footer only where it does not cover the last page.
    if (row_y(P_COUNT - 1) + row_h() + S(46) > y() + h() - inset) return;
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
  MainWindow(int w, int h) : BufferedWindow(w, h, _("Software Updater")) {}
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
      if (k == 'q' || k == 'w') {
        app().request_close();
        return 1;
      }
      if (k >= '1' && k < '1' + P_COUNT) {
        app().show_page(k - '1');
        return 1;
      }
    }
    // Escape on a settings page goes back to the updater (searches clear
    // themselves first).
    if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && Fl::event_key() == FL_Escape && app().page != P_UPDATES &&
        !(Fl::event_state() & (FL_CTRL | FL_ALT))) {
      if (BufferedWindow::handle(e)) return 1;
      app().show_page(P_UPDATES);
      return 1;
    }
    return BufferedWindow::handle(e);
  }
};

void relayout_pages() { app().layout(); }

UpdaterPage *updater_page() { return static_cast<UpdaterPage *>(app().pages[P_UPDATES]); }

// ----------------------------------------------------------- App methods

void App::layout() {
  if (!window) return;
  int W = window->w(), H = window->h();
  // The updater is a compact window of its own; the settings, history,
  // sources, maintenance and about pages get the sidebar.
  bool updater = page == P_UPDATES;
  int side = updater ? 0 : W < S(860) ? S(188) : S(224);
  if (updater) {
    sidebar->hide();
  } else {
    sidebar->resize(0, 0, side, H);
    sidebar->show();
  }
  bool strip_on = strip_visible && !updater;
  int strip_h = strip_on ? S(64) : 0;
  int cx = side, cw = W - side;
  int ph = H - (strip_on ? strip_h + S(16) : 0);
  for (auto *p : pages) {
    p->view_h = ph;
    int ch = std::max(ph, p->content_height());
    p->scroll = std::max(0, std::min(p->scroll, ch - ph));
    p->resize(cx, -p->scroll, cw, ch);
  }
  int sp = cw < S(620) ? S(16) : S(28);
  if (strip_on) {
    strip->resize(cx + sp, H - strip_h - S(16), cw - 2 * sp, strip_h);
    strip->show();
  } else {
    strip->hide();
  }
  window->redraw();
}

void App::show_page(int index) {
  if (index < 0 || index >= P_COUNT) return;
  bool was_updater = page == P_UPDATES, updater = index == P_UPDATES;
  page = index;
  for (int i = 0; i < P_COUNT; ++i) {
    if (i == index) pages[i]->show();
    else pages[i]->hide();
  }
  if (window && window->shown() && was_updater != updater) {
    // The settings pages need more room than the compact updater.
    int sx, sy, sw, sh;
    Fl::screen_work_area(sx, sy, sw, sh, window->x(), window->y());
    int W = window->w(), H = window->h();
    if (!updater) {
      updater_w = W;
      updater_h = H;
      W = std::max(W, std::min(S(940), sw - S(24)));
      H = std::max(H, std::min(S(640), sh - S(60)));
    } else if (updater_w > 0) {
      W = updater_w;
      H = updater_h;
    }
    int X = std::max(sx, std::min(window->x() - (W - window->w()) / 2, sx + sw - W));
    int Y = std::max(sy, std::min(window->y(), sy + sh - H));
    window->resize(X, Y, W, H);
  }
  layout();
  pages[index]->activated();
  if (window) window->redraw();
}

void App::changed() {
  updater_page()->sync();
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
  updater_page()->list->phase = a.phase;
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
  UpdaterPage *pg = updater_page();
  // Keep the selected row (by what it shows) across rebuilds.
  int sel = pg->list->selected();
  Row keep;
  bool had = sel >= 0 && sel < int(rows.size());
  std::string keep_key;
  if (had) {
    keep = rows[size_t(sel)];
    if (keep.index >= 0) keep_key = update_key(list.updates[size_t(keep.index)]);
  }
  rows.clear();
  for (int g = 0; g < kGroupCount; ++g) {
    std::vector<int> items;
    for (size_t i = 0; i < list.updates.size(); ++i)
      if (int(list.updates[i].group) == g) items.push_back(int(i));
    if (items.empty()) continue;
    rows.push_back({Row::GroupRow, Group(g), -1, ""});
    if (!group_open[g]) continue;
    for (int i : items) {
      const Update &u = list.updates[size_t(i)];
      rows.push_back({Row::UpdateRow, Group(g), i, ""});
      if (u.packages.size() > 1 && open_updates.count(update_key(u)))
        for (auto &p : u.packages) rows.push_back({Row::PackageRow, Group(g), i, p});
    }
  }
  int new_sel = -1;
  if (had) {
    for (size_t r = 0; r < rows.size(); ++r) {
      const Row &row = rows[r];
      bool same = row.type == keep.type && row.group == keep.group &&
                  (row.index < 0 || update_key(list.updates[size_t(row.index)]) == keep_key) &&
                  row.package == keep.package;
      if (same) {
        new_sel = int(r);
        break;
      }
    }
    // A collapsed child: select its group or update instead.
    if (new_sel < 0)
      for (size_t r = 0; r < rows.size(); ++r)
        if ((rows[r].type == Row::UpdateRow && !keep_key.empty() &&
             update_key(list.updates[size_t(rows[r].index)]) == keep_key) ||
            (rows[r].type == Row::GroupRow && rows[r].group == keep.group && new_sel < 0)) {
          new_sel = int(r);
          if (rows[r].type == Row::UpdateRow) break;
        }
  }
  if (new_sel < 0)
    for (size_t r = 0; r < rows.size(); ++r)
      if (rows[r].type == Row::UpdateRow) {
        new_sel = int(r);
        break;
      }
  // Size the tree for the current view first: the selection scrolls into
  // view against the real height.
  pg->layout();
  pg->list->select(-1, false);
  if (!had) pg->list->reset_scroll();
  pg->list->select(new_sel);
  if (new_sel < 0) {
    current = -1;
    current_group = -1;
  }
  changed();
}

void App::select_changelog(int index) {
  TechPane *d = updater_page()->tech;
  d->refresh();
  if (index < 0 || index >= int(list.updates.size())) return;
  const Update &u = list.updates[size_t(index)];
  if (u.flatpak) return;
  std::string pkg = u.main_package;
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
      updater_page()->tech->refresh();
    });
    if (!ok) {
      a.changelogs[pkg] = _("Python 3 is missing; changelogs cannot be shown.");
      a.changelog_pending.clear();
      updater_page()->tech->refresh();
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
  // APT (check.py) and Flatpak (flatpak.py) are read in parallel.
  pending_loads = 2;
  apt_output.clear();
  flatpak_output.clear();
  apt_status = -1;
  bool started = run_async({kPython, helper("check.py")}, [](const std::string &output, int status) {
    App &a = app();
    a.apt_output = output;
    a.apt_status = status;
    if (--a.pending_loads == 0) a.merge_updates();
  });
  if (!started) {
    loading = false;
    pending_loads = 0;
    load_error = _("Python 3 is missing. Reinstall the sde package and its dependencies.");
    changed();
    return;
  }
  bool flatpak = run_async({kPython, helper("flatpak.py")}, [](const std::string &output, int) {
    App &a = app();
    a.flatpak_output = output;
    if (--a.pending_loads == 0) a.merge_updates();
  });
  if (!flatpak) --pending_loads;
}

void App::merge_updates() {
  loading = false;
  refreshed = lists_mtime();
  UpdateList fresh = parse_updates(apt_output);
  if (apt_status != 0 || !fresh.valid) {
    load_error = _("Cannot read the APT package information. Make sure python3-apt is installed and "
                   "your package sources are valid.");
    list = UpdateList{};
  } else {
    parse_flatpaks(flatpak_output, fresh);
    // Keep the user's choices for updates that are still offered.
    std::set<std::string> off;
    for (auto &u : list.updates)
      if (!u.selected) off.insert(update_key(u) + "=" + u.new_version);
    for (auto &u : fresh.updates)
      if (off.count(update_key(u) + "=" + u.new_version)) u.selected = false;
    list = std::move(fresh);
  }
  icons.clear();
  loaded_once = true;
  bool first = !focused_once;
  rebuild_rows();
  // The tree takes the keyboard focus once it first appears.
  if (first && page == P_UPDATES && updater_page()->list->visible()) {
    focused_once = true;
    Fl::focus(updater_page()->list);
  }
  maybe_auto_refresh();
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
        UpdaterPage *pg = updater_page();
        if (pg->log->visible()) {
          pg->log->set(a.full_log());
          pg->log->scroll_to_end();
        }
        pg->layout();
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
  if (!job) finished();
}

void App::run_next() {
  if (queue.empty()) return;
  std::vector<std::string> next = queue.front();
  queue.erase(queue.begin());
  std::string action = next.front();
  next.erase(next.begin());
  start(action, next);
}

void App::cancel() {
  if (!busy() || !progress.cancellable) return;
  progress.cancel_requested = true;
  queue.clear();
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
  if (installing) {
    if (progress.ok() && !queue.empty()) {
      // The next part of Install Now (e.g. Flatpak after the APT packages).
      earlier_log.insert(earlier_log.end(), progress.log.begin(), progress.log.end());
      earlier_log.push_back("");
      run_next();
      return;
    }
    installing = false;
    queue.clear();
    if (progress.ok()) {
      reboot = reboot_required(&reboot_packages);
      show_restart = reboot || reboot_expected;
    } else {
      show_failure = true;
      log_open = true;
    }
    strip_visible = false;
  }
  if (action == "refresh" || action == "upgrade" || action == "flatpak" || action == "autoremove" ||
      action == "repair") {
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
  queue = install_commands(list.updates);
  if (queue.empty()) return;
  install_steps = int(queue.size());
  earlier_log.clear();
  installing = true;
  show_failure = show_restart = false;
  reboot_expected = list.needs_reboot();
  run_next();
}

void App::remind_later() {
  // The notifier stays quiet until then, and notifies again afterwards.
  int hours = std::max(1, cfg.get_int("update", "remind_hours", 24));
  NotifierState s = load_notifier_state();
  s.remind_until = std::time(nullptr) + std::time_t(hours) * 3600;
  s.notified.clear();
  save_notifier_state(s);
  request_close();
}

void App::restart_now() {
  if (!find_program("sde-logout").empty()) spawn_detached({"sde-logout"});
  else if (confirm(window, _("Restart the computer?"), _("Save your work first. Open applications will be closed."),
                   "", _("Restart Now"), true))
    spawn_detached({"systemctl", "reboot"});
}

void App::open_terminal_transaction() {
  if (busy()) return;
  if (find_program("xterm").empty()) {
    inform(window, _("No terminal available"), _("Install xterm to run update transactions in a terminal."));
    return;
  }
  run_async({"xterm", "-T", _("Software Updater"), "-e", "sde-update", "--transaction", "upgrade"},
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

void App::apply_theme() {
  cfg.load();
  Theme theme = Theme::from_config(cfg);
  theme.apply_to_fltk();
  set_look(Look::from_config(cfg));
  IconLoader::set_theme(cfg.get("appearance", "icon_theme", "auto"));
  icons.clear();
  for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w)) {
    restyle_tree(w);
    w->redraw();
  }
  layout();
  changed();
}

// The SDE settings bump _SDE_CONFIG_SERIAL on the root window whenever the
// appearance changes: follow the new theme without a restart.
int config_watch(int) {
  if (!fl_xevent || !app().window) return 0;
  const XEvent &e = *fl_xevent;
  if (e.type == PropertyNotify && e.xproperty.window == RootWindow(fl_display, fl_screen) &&
      e.xproperty.atom == x11::atom("_SDE_CONFIG_SERIAL"))
    app().apply_theme();
  return 0;
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

std::string notifier_state_path() { return join_path(state_home(), "sde/update-notifier"); }

NotifierState load_notifier_state() {
  NotifierState s;
  std::string text;
  if (!read_file(notifier_state_path(), text)) return s;
  for (auto &line : split(text, '\n')) {
    if (starts_with(line, "last_check=")) s.last_check = std::time_t(atoll(line.c_str() + 11));
    if (starts_with(line, "notified=")) s.notified = line.substr(9);
    if (starts_with(line, "remind_until=")) s.remind_until = std::time_t(atoll(line.c_str() + 13));
  }
  return s;
}

void save_notifier_state(const NotifierState &s) {
  make_dirs(join_path(state_home(), "sde"));
  write_file_atomic(notifier_state_path(),
                    format("last_check=%lld\nnotified=%s\nremind_until=%lld\n", (long long)s.last_check,
                           s.notified.c_str(), (long long)s.remind_until));
}

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
  // Compact, like a dialog; the settings pages widen it (see show_page()).
  int W = std::min(S(700), sw - S(24)), H = std::min(S(640), sh - S(60));
  auto *win = new MainWindow(W, H);
  a.window = win;
  a.sidebar = new Sidebar(0, 0, S(224), H);
  a.pages[P_UPDATES] = new UpdaterPage();
  a.pages[P_HISTORY] = new HistoryPage();
  a.pages[P_SOURCES] = new SourcesPage();
  a.pages[P_MAINTENANCE] = new MaintenancePage();
  a.pages[P_SETTINGS] = new SettingsPage();
  a.pages[P_ABOUT] = new AboutPage();
  a.strip = new ActivityStrip();
  win->end();
  win->resizable(nullptr);
  win->size_range(std::min(W, S(460)), std::min(H, S(340)));
  win->xclass("sde-update");
  win->callback([](Fl_Widget *, void *) {
    // Escape clears searches; it should not close the whole window.
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
    app().request_close();
  });
  int index = P_UPDATES;
  for (int i = 0; i < P_COUNT; ++i)
    if (page_name == kPages[i].id) index = i;
  a.page = index;
  if (index != P_UPDATES) {
    // Opened on a settings page: start with the wide window.
    a.updater_w = W;
    a.updater_h = H;
    win->size(std::max(W, std::min(S(940), sw - S(24))), std::max(H, std::min(S(640), sh - S(60))));
  }
  a.show_page(index);
  win->position(sx + (sw - win->w()) / 2, sy + (sh - win->h()) / 2);
  Fl::add_handler(config_watch);
  win->show();
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  a.load_updates();
  a.load_maintenance();
  Fl::add_timeout(60, minute_tick);
  return Fl::run();
}

}  // namespace update
}  // namespace sde
