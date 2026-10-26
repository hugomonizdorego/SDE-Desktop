// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Merkadu's pages. The application pages follow the panes of Ubuntu
// Software Center: Home (the highlight, highlights, recommendations, "New
// & Updated", popular apps and the departments), a department with its
// sub-departments, search results with the source of every app, the
// details page (screenshots, description, "What's New" and the version,
// size, license and source), the installed software grouped by department,
// the app updates (the system's are Software Updater's), the history of
// changes grouped by day, the changes in progress with the animated SDE
// logo, and the sources. All Software (every package) is packages_ui.cpp.
//
// No page is ever empty without saying why: while the catalog loads it
// shows placeholders, and an empty catalog says what is missing (the
// appstream package, a refresh of the lists, PackageKit) with the button
// that fixes it.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <time.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "i18n.h"
#include "icons.h"
#include "logo_anim.h"
#include "packages_ui.h"
#include "spawn.h"
#include "store.h"
#include "ui.h"
#include "util.h"
#include "window.h"
#include "../settings/ui.h"

namespace sde {
namespace merkadu {

namespace {

// Filters and limits that outlive a rebuild of the page.
int g_search_source = -1;   // -1 all, else Source
int g_installed_source = -1;
int g_history_filter = -1;  // -1 all, else Change
std::map<std::string, size_t> g_limits;  // per view: how many items are shown
bool g_snap_enabled_now = false;          // Snap support was installed in this session

size_t limit_for(const std::string &key, size_t fallback) {
  auto it = g_limits.find(key);
  return it == g_limits.end() ? fallback : it->second;
}

int gap() { return S(14); }

double now_seconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// A caption above a group of tiles or rows.
void caption(Page *page, const std::string &id, const std::string &title, const std::string &sub = "") {
  int y = page->cursor();
  new Label(page->left() + S(2), y, page->width(), S(30), title, Label::TITLE);
  qa_line("section:" + id, page->left(), y, page->width(), S(30));
  page->advance(S(34));
  if (!sub.empty()) {
    new Label(page->left() + S(2), page->cursor() - S(4), page->width(), S(20), sub, Label::DIM);
    page->advance(S(22));
  }
  page->advance(S(4));
}

Button *button(int x, int y, const std::string &text, Button::Kind kind, std::function<void()> fn, int w = 0) {
  auto *b = new Button(x, y, w ? w : Button::fit(text), S(34), text, kind);
  b->on_click = std::move(fn);
  return b;
}

void open_app(const std::string &id) { navigate(View{View::Details, id}); }

int tile_cols(int width) { return std::max(1, std::min(4, (width + gap()) / (S(260) + gap()))); }

// App tiles in a grid; |rows| limits the rows (0: all).
void tile_grid(Page *page, const std::vector<const App *> &apps, int rows = 0) {
  int w = page->width(), cols = tile_cols(w), tw = (w - (cols - 1) * gap()) / cols, th = AppTile::height();
  size_t n = apps.size();
  if (rows > 0) n = std::min(n, size_t(rows * cols));
  int y = page->cursor();
  for (size_t i = 0; i < n; ++i) {
    int col = int(i % size_t(cols)), row = int(i / size_t(cols));
    auto *t = new AppTile(page->left() + col * (tw + gap()), y + row * (th + gap()), tw, th, apps[i]->id);
    t->on_open = open_app;
    qa_line("app:" + apps[i]->id, t->x(), t->y(), t->w(), t->h());
  }
  int used = int((n + size_t(cols) - 1) / size_t(cols));
  page->advance(used * (th + gap()));
}

// Placeholders where tiles will be (the catalog is loading).
void placeholder_grid(Page *page, int rows) {
  int w = page->width(), cols = tile_cols(w), tw = (w - (cols - 1) * gap()) / cols, th = AppTile::height();
  for (int i = 0; i < rows * cols; ++i)
    new Placeholder(page->left() + (i % cols) * (tw + gap()), page->cursor() + (i / cols) * (th + gap()), tw, th);
  page->advance(rows * (th + gap()));
}

void row_list(Page *page, const std::vector<const App *> &apps, AppRow::Mode mode, size_t max) {
  int h = S(76);
  for (size_t i = 0; i < apps.size() && i < max; ++i) {
    auto *r = new AppRow(page->left(), page->cursor(), page->width(), h, apps[i]->id, mode);
    r->on_open = open_app;
    r->on_action = [mode](const std::string &id) {
      const App *a = Store::get().app(id);
      if (!a) return;
      if (mode == AppRow::Installed) {
        if (const Origin *o = a->installed_origin()) request_remove(id, o->source);
      } else if (mode == AppRow::Updates) {
        Store::get().update(id);
      } else {
        request_install(id, chosen_source(id));
      }
    };
    qa_line("app:" + apps[i]->id, r->x(), r->y(), r->w(), r->h());
    int bx, by, bw, bh;
    r->action_box(&bx, &by, &bw, &bh);
    std::string act = r->action_text();
    if (!act.empty()) qa_line("action:" + apps[i]->id + ":" + act, bx, by, bw, bh);
    page->advance(h + S(8));
  }
}

// "Show more" for long lists.
void more_button(Page *page, const std::string &key, size_t shown, size_t total, size_t step) {
  if (shown >= total) return;
  std::string text = format(_("Show %d more"), int(std::min(step, total - shown)));
  button(page->left(), page->cursor(), text, Button::Normal, [key, shown, step] {
    g_limits[key] = shown + step;
    rebuild_page();
  });
  page->advance(S(34) + S(16));
}

// A row of filter chips.
struct ChipDef {
  std::string text;
  bool on;
  std::function<void()> fn;
  Color dot = Color::hex(0, 0);
};
void chips(Page *page, const std::string &id, const std::vector<ChipDef> &defs) {
  int x = page->left(), y = page->cursor(), h = S(32);
  for (auto &d : defs) {
    int w = Chip::fit(d.text) + (d.dot.a > 0 ? S(10) : 0);
    if (x + w > page->left() + page->width() && x > page->left()) {
      x = page->left();
      y += h + S(8);
    }
    auto *c = new Chip(x, y, w, h, d.text, d.on);
    c->dot = d.dot;
    c->on_click = d.fn;
    qa_line("chip:" + id + ":" + d.text, x, y, w, h);
    x += w + S(8);
  }
  page->advance(y - page->cursor() + h + S(16));
}

// A wrapped paragraph.
void paragraph(Page *page, const std::string &text, bool dim = false, int delta = 0, int indent = 0) {
  int w = page->width() - indent;
  int h = TextBlock::measure(text, w, delta) + S(4);
  new TextBlock(page->left() + indent, page->cursor(), w, h, text, dim, delta);
  page->advance(h + S(6));
}

// The animated logo with a line of text, while something loads.
void loading(Page *page, const std::string &text) {
  int size = S(96);
  auto *logo = new LogoAnimation(page->left() + (page->width() - size) / 2, page->cursor() + S(40), size, size);
  logo->running(true);
  logo->tooltip(_("Loading"));
  page->advance(S(40) + size + S(16));
  text_font(active_theme().font_size - 1);
  int tw = std::min(page->width(), text_width(text) + S(8));
  new Label(page->left() + (page->width() - tw) / 2, page->cursor(), tw, S(24), text, Label::DIM);
  page->advance(S(30));
}

// A label/value line of the details card.
void info_row(Page *page, const std::string &label, const std::string &value) {
  if (value.empty()) return;
  int x = page->left() + S(20), w = page->width() - S(40), lw = std::min(S(170), w / 3);
  int vh = std::max(S(24), TextBlock::measure(value, w - lw, 0) + S(2));
  new Label(x, page->cursor(), lw, S(24), label, Label::DIM);
  new TextBlock(x + lw, page->cursor() + S(3), w - lw, vh, value);
  qa_line("info:" + label, x, page->cursor(), w, vh);
  page->advance(vh + S(6));
}

// A notice with up to two buttons on its right.
struct NoticeButton {
  std::string text;
  std::function<void()> fn;
  bool primary = false;
};
void notice(Page *page, const std::string &id, const std::string &text, int level, bool busy = false,
            const std::vector<NoticeButton> &buttons = {}) {
  int bw = 0;
  for (auto &b : buttons) bw += Button::fit(b.text) + S(8);
  int w = page->width(), h = Notice::measure(w, text, bw);
  auto *n = new Notice(page->left(), page->cursor(), w, h, text, level, busy);
  n->text_right = bw;
  qa_line("notice:" + id, n->x(), n->y(), n->w(), n->h());
  int bx = page->left() + w - S(14) - bw + S(8);
  for (auto &b : buttons) {
    Button *btn = button(bx, page->cursor() + (h - S(34)) / 2, b.text, b.primary ? Button::Primary : Button::Normal,
                         b.fn);
    bx += btn->w() + S(8);
  }
  page->advance(h + S(12));
}

std::vector<const App *> resolve_all(const std::vector<std::string> &refs) {
  std::vector<const App *> out;
  for (auto &r : refs) {
    const App *a = resolve_reference(Store::get().catalog(), r);
    if (a && a->is_app() && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
  }
  return out;
}

std::string release_date(std::time_t t) {
  if (t <= 0) return "";
  struct tm tm = {};
  localtime_r(&t, &tm);
  char buf[64];
  // TRANSLATORS: the date of a release (strftime format).
  strftime(buf, sizeof buf, _("%B %e, %Y"), &tm);
  std::string out = buf;
  size_t dbl = out.find("  ");
  if (dbl != std::string::npos) out.erase(dbl, 1);
  return out;
}

// "today at 10:15", "October 20 at 10:15".
std::string when_label(std::time_t t) {
  if (t <= 0) return _("never");
  std::string day = day_label(t, std::time(nullptr));
  std::string lower = day;
  if (day == _("Today")) lower = _("today");
  else if (day == _("Yesterday")) lower = _("yesterday");
  return format(_("%s at %s"), lower.c_str(), time_label(t).c_str());
}

// Installs Debian packages for a source or the catalog (flatpak, snapd,
// appstream), then shows the progress.
void install_support(const std::string &title, const std::vector<std::string> &packages) {
  Job j;
  j.action = Action::Install;
  j.source = Source::Debian;
  j.title = title;
  j.icon = "system-software-install";
  j.targets = packages;
  if (Store::get().enqueue(j)) navigate(View{View::Progress, ""});
}

void setup_flathub() {
  Store::get().backend(Source::Flatpak)->setup("flathub", [](bool ok, bool, std::string error) {
    Store::get().probe_sources();
    if (ok) {
      // Its catalog: downloaded now, read when it is there.
      Store::get().refresh(true);
    } else {
      fprintf(stderr, "merkadu: adding Flathub failed: %s\n", error.c_str());
      if (!error.empty()) fl_alert(_("Flathub could not be set up:\n\n%s"), error.c_str());
    }
  });
}

// Snap support, as the Debian tutorial does it: the snapd package, then
// the snapd snap (snapd's own updates); polkit asks, no terminal.
void enable_snap() {
  int c = fl_choice(_("Enable Snap support?\n\nMerkadu installs snapd from %s and then the snapd snap from the Snap "
                      "Store, like the Debian instructions describe. Your password is asked for.\n\nWhen it is done, "
                      "restart the computer (or log out and back in): then snap apps start and their commands are "
                      "found in /snap/bin."),
                    _("Cancel"), _("Enable Snap Support"), nullptr, source_label(Source::Debian).c_str());
  if (c != 1) return;
  Store &st = Store::get();
  Job deb;
  deb.action = Action::Install;
  deb.source = Source::Debian;
  deb.title = _("Snap support (snapd)");
  deb.icon = "system-software-install";
  deb.targets = {"snapd"};
  int first = st.enqueue(deb);
  if (!first) return;
  Job snap;
  snap.action = Action::Install;
  snap.source = Source::Snap;
  snap.title = _("The snapd snap");
  snap.icon = "system-software-install";
  snap.targets = {"snapd"};
  snap.remote = "stable";
  snap.needs = first;
  st.enqueue(snap);
  g_snap_enabled_now = true;
  navigate(View{View::Progress, ""});
}

// ---------------------------------------------------------------- widgets

// An app icon.
class IconView : public Fl_Widget {
 public:
  IconView(int x, int y, int s, const std::string &id) : Fl_Widget(x, y, s, s), id_(id) {}
  void draw() override {
    const App *a = Store::get().app(id_);
    if (!a) return;
    Painter p(window()->w(), window()->h());
    paint_app_icon(p, *a, x(), y(), w());
  }

 private:
  std::string id_;
};

// The progress of a job: status, percentage and a bar, read live.
class JobProgress : public Fl_Widget {
 public:
  JobProgress(int x, int y, int w, int h, int job) : Fl_Widget(x, y, w, h), job_(job) {}
  void draw() override {
    const Theme &t = active_theme();
    const Job *j = Store::get().job(job_);
    if (!j) return;
    {
      Painter p(window()->w(), window()->h());
      int pct = j->state == Job::Done ? 100 : j->state == Job::Running ? j->percent : -1;
      if (j->state == Job::Queued || j->state == Job::Running)
        paint_progress(p, x(), y() + h() - S(8), w(), S(8), pct, std::fmod(now_seconds(), 1.6) / 1.6);
    }
    std::string text;
    switch (j->state) {
      case Job::Queued: text = _("Waiting for the other changes to finish"); break;
      case Job::Running:
        text = j->status.empty() ? action_progress_label(j->action) : j->status;
        if (j->percent >= 0) text += format("  %d%%", j->percent);
        break;
      case Job::Done: text = j->action == Action::Remove ? _("Removed") : j->action == Action::Update ? _("Updated")
                                                                                                      : _("Installed");
        break;
      case Job::Cancelled: text = j->error.empty() ? std::string(_("Cancelled")) : j->error; break;
      case Job::Failed: text = j->error.empty() ? std::string(_("Failed")) : j->error; break;
    }
    text_font(t.font_size);
    Color c = j->state == Job::Failed ? readable(t.error, t.background) : t.text_dim;
    draw_text(text, x(), y(), w(), h() - S(12), c);
  }
  int job() const { return job_; }

 private:
  int job_;
};

// One entry of the history: what changed, when, by whom.
class HistoryRow : public Fl_Widget {
 public:
  HistoryRow(int x, int y, int w, int h, const HistoryEntry &e) : Fl_Widget(x, y, w, h), e_(e) {
    title_ = title_of(e);
    copy_tooltip(title_.c_str());
  }
  static std::string title_of(const HistoryEntry &e) {
    Store &st = Store::get();
    std::string what;
    if (e.items.size() == 1) {
      what = st.item_title(e.source, e.items[0]);
    } else if (e.items.empty()) {
      what = e.automatic == 1 ? std::string(_("1 package")) : format(_("%d packages"), e.automatic);
    } else {
      what = format(_("%d packages"), int(e.items.size()));
    }
    switch (e.change) {
      case Change::Remove: return format(_("Removed %s"), what.c_str());
      case Change::Update: return format(_("Updated %s"), what.c_str());
      default: return format(_("Installed %s"), what.c_str());
    }
  }
  void draw() override {
    const Theme &t = active_theme();
    Color c = e_.change == Change::Remove ? Color::hex(0xC9564E)
              : e_.change == Change::Update ? Color::hex(0x3876B9)
                                            : Color::hex(0x4A9157);
    if (e_.failed) c = t.warning;
    std::string badge = source_label(e_.source);
    int bw = badge_width(badge), bh = S(20);
    int bx = x() + w() - S(14) - bw, by = y() + (h() - bh) / 2;
    text_font(std::max(S(9), t.font_size - S(1)));
    std::string when = time_label(e_.when);
    int ww = text_width(when) + S(4);
    {
      Painter p(window()->w(), window()->h());
      double r = std::min(double(t.radius), Sd(12));
      p.fill_rounded(x(), y(), w(), h(), r, t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.border);
      double cs = S(32);
      p.circle(x() + S(14) + cs / 2, y() + h() / 2.0, cs / 2, c.alpha(t.dark ? 0.3 : 0.16));
      Symbol s = e_.failed ? Symbol::Warning
                 : e_.change == Change::Remove ? Symbol::Minus
                 : e_.change == Change::Update ? Symbol::Refresh
                                               : Symbol::Plus;
      paint_symbol(p, s, x() + S(14) + S(6), y() + h() / 2.0 - S(10), S(20), readable(c, t.surface));
      paint_badge(p, bx, by, bw, bh, source_color(e_.source));
    }
    int tx = x() + S(60), tw = bx - ww - S(16) - tx;
    text_font(t.font_size + 1, true);
    draw_text(title_, tx, y() + h() / 2 - S(21), tw, S(22), t.text);
    std::string detail;
    if (e_.failed) {
      detail = e_.error.empty() ? std::string(_("Failed")) : format(_("Failed: %s"), e_.error.c_str());
    } else {
      std::vector<std::string> names;
      for (size_t i = 0; i < e_.items.size() && i < 6; ++i) {
        std::string n = e_.items[i];
        if (e_.source == Source::Flatpak) {
          Origin o;
          o.id = n;
          n = o.flatpak_app_id();
        }
        if (i < e_.versions.size() && !e_.versions[i].empty()) n += " " + e_.versions[i];
        names.push_back(n);
      }
      for (size_t i = 0; i < names.size(); ++i) detail += (i ? ", " : "") + names[i];
      if (e_.items.size() > names.size()) detail += format(_(" and %d more"), int(e_.items.size() - names.size()));
      if (e_.automatic > 0 && !e_.items.empty())
        detail += e_.automatic == 1 ? std::string(_(", with 1 dependency"))
                                    : format(_(", with %d dependencies"), e_.automatic);
      if (!e_.requested_by.empty()) detail += "  \xC2\xB7  " + format(_("by %s"), e_.requested_by.c_str());
    }
    text_font(std::max(S(9), t.font_size - S(1)));
    draw_text(detail, tx, y() + h() / 2 + S(1), tw, S(20), e_.failed ? readable(t.warning, t.surface) : t.text_dim);
    draw_text(when, bx - ww - S(10), y(), ww, h(), t.text_dim, 2);
    draw_badge_text(badge, bx, by, bw, bh, source_color(e_.source));
  }

 private:
  HistoryEntry e_;
  std::string title_;
};

// A card that leads somewhere: a symbol, a title, a text and a button.
void promo_card(Page *page, const std::string &id, Symbol s, const std::string &title, const std::string &text,
                const std::string &action, std::function<void()> fn) {
  int x = page->left(), w = page->width(), pad = S(20);
  int bw = Button::fit(action), tx = x + pad + S(56) + S(18), tw = w - (tx - x) - pad - bw - S(16);
  int th = TextBlock::measure(text, tw, 0);
  int h = std::max(S(96), S(16) + S(28) + th + S(18));
  int y = page->cursor();
  new Card(x, y, w, h);
  struct Badge : Fl_Widget {
    Symbol s;
    Badge(int x, int y, int sz, Symbol sym) : Fl_Widget(x, y, sz, sz), s(sym) {}
    void draw() override {
      const Theme &t = active_theme();
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), w() * 0.25, t.accent.alpha(t.dark ? 0.3 : 0.15));
      double ss = w() * 0.55;
      paint_symbol(p, s, x() + (w() - ss) / 2, y() + (h() - ss) / 2, ss, t.accent_text);
    }
  };
  new Badge(x + pad, y + (h - S(56)) / 2, S(56), s);
  new Label(tx, y + S(14), tw, S(28), title, Label::BOLD);
  new TextBlock(tx, y + S(42), tw, th + S(2), text, true);
  Button *b = button(x + w - pad - bw, y + (h - S(34)) / 2, action, Button::Primary, std::move(fn), bw);
  qa_line("promo:" + id, x, y, w, h);
  qa_line("action:promo:" + id, b->x(), b->y(), b->w(), b->h());
  page->advance(h + S(24));
}

// ------------------------------------------------------- source problems

// What the user should know about the sources before the apps: lists that
// could not be refreshed, a missing catalog or Flathub, PackageKit down.
void source_notices(Page *page, bool full) {
  Store &st = Store::get();
  const RefreshState &r = st.refresh_state();
  for (auto &e : r.errors)
    notice(page, "refresh-error", e, 2, false,
           {{_("Try Again"), [] { Store::get().refresh(true); }, false}, {_("Sources"), [] {
                                                                            navigate(View{View::Sources, ""});
                                                                          }}});
  if (full)
    for (auto &n : r.notes) notice(page, "refresh-note", n, 0);
  const SourceStatus &deb = st.status(Source::Debian);
  if (st.probed(Source::Debian) && !deb.available && !fake_mode())
    notice(page, "packagekit",
           format(_("Apps from %s cannot be installed or removed: %s Merkadu needs the PackageKit service "
                    "(package packagekit); start it or reinstall it with Software Updater or a terminal."),
                  source_label(Source::Debian).c_str(), deb.message.c_str()),
           2);
  const CatalogSources &cs = st.catalog_sources();
  if (st.catalog_loaded() && !fake_mode() && !r.running) {
    if (!cs.appstream_apt)
      notice(page, "appstream",
             format(_("The catalog of the apps of %s is not downloaded: the appstream package is missing."),
                    source_label(Source::Debian).c_str()),
             1, false,
             {{_("Install It"), [] { install_support(_("The software catalog (appstream)"), {"appstream"}); }, true}});
    else if (cs.dep11 == 0)
      notice(page, "dep11",
             format(_("The catalog of the apps of %s has not been downloaded yet: refresh the software lists."),
                    source_label(Source::Debian).c_str()),
             1, false, {{_("Refresh"), [] { Store::get().refresh(true); }, true}});
  }
  const SourceStatus &fp = st.status(Source::Flatpak);
  if (st.probed(Source::Flatpak) && st.settings().setup_flathub) {
    if (!fp.installed)
      notice(page, "flatpak", _("Flatpak is not installed: the apps of Flathub are not shown."), 1, false,
             {{_("Install Flatpak"), [] { install_support(_("Flatpak support"), {"flatpak"}); }, true}});
    else if (fp.available && !fp.has_flathub)
      notice(page, "flathub", _("Flathub is not set up: thousands of apps are not shown."), 1, false,
             {{_("Set Up Flathub"), setup_flathub, true}});
  }
}

// The page without any app: why, and what fixes it.
void empty_catalog(Page *page) {
  Store &st = Store::get();
  const CatalogSources &cs = st.catalog_sources();
  const RefreshState &r = st.refresh_state();
  std::string distro = source_label(Source::Debian);
  std::string title, text;
  std::vector<NoticeButton> buttons;
  bool refresh_button = true;
  if (r.running) {
    title = _("Getting the software lists\xE2\x80\xA6");
    text = _("Merkadu is downloading the lists of the software sources. The apps appear when they are there.");
    refresh_button = false;
  } else if (!cs.appstream_apt && !fake_mode()) {
    title = _("The software catalog is not installed");
    text = format(_("Merkadu shows the apps of %s with the AppStream catalog, which APT downloads once the appstream "
                    "package is installed. Install it, then the lists are refreshed."),
                  distro.c_str());
    buttons.push_back({_("Install the Catalog"), [] { install_support(_("The software catalog (appstream)"), {"appstream"}); },
                       true});
  } else if (cs.dep11 == 0 && cs.flatpak_remotes.empty()) {
    title = _("The software lists have not been downloaded yet");
    text = format(_("The catalog of %s comes with the package lists, and Flathub's with its own lists. They have not "
                    "been downloaded since they were set up: refresh them (this needs a network connection)."),
                  distro.c_str());
  } else {
    title = _("No apps were found");
    text = format(_("The software catalogs of this computer have no apps (%d catalogs of %s, %zu Flatpak remotes "
                    "with apps). Refresh the software lists, or see the Sources page."),
                  cs.dep11, distro.c_str(), cs.flatpak_remotes.size());
  }
  if (!r.errors.empty()) {
    text += "\n\n";
    for (auto &e : r.errors) text += e + "\n";
  }
  if (refresh_button) buttons.push_back({_("Refresh Software Lists"), [] { Store::get().refresh(true); }, buttons.empty()});
  if (st.packages_available() && !st.packages().empty())
    buttons.push_back({_("All Software"), [] { navigate(View{View::Packages, ""}); }, false});
  buttons.push_back({_("Sources"), [] { navigate(View{View::Sources, ""}); }, false});
  page->advance(S(24));
  auto *es = new EmptyState(page->left(), page->cursor(), page->width(), r.running ? Symbol::Download : Symbol::Package,
                            title, text, !r.running);
  qa_line("empty:" + std::string(r.running ? "loading" : !cs.appstream_apt ? "appstream" : cs.dep11 == 0 ? "lists"
                                                                                                         : "none"),
          es->x(), es->y(), es->w(), es->h());
  page->advance(es->h() + S(16));
  int bw = 0;
  for (auto &b : buttons) bw += Button::fit(b.text) + S(10);
  int bx = page->left() + (page->width() - bw + S(10)) / 2;
  for (auto &b : buttons) {
    Button *btn = button(bx, page->cursor(), b.text, b.primary ? Button::Primary : Button::Normal, b.fn);
    qa_line("action:empty:" + b.text, btn->x(), btn->y(), btn->w(), btn->h());
    bx += btn->w() + S(10);
  }
  page->advance(S(34) + S(24));
}

// ------------------------------------------------------------- explore

std::map<std::string, int> g_category_counts;
int g_counts_generation = -1;

int category_count(const Category &c) {
  Store &st = Store::get();
  if (g_counts_generation != st.generation()) {
    g_category_counts.clear();
    g_counts_generation = st.generation();
  }
  auto it = g_category_counts.find(c.id);
  if (it != g_category_counts.end()) return it->second;
  int n = int(st.categories().apps_in(c, st.catalog()).size());
  g_category_counts[c.id] = n;
  return n;
}

void departments(Page *page) {
  Store &st = Store::get();
  int w = page->width(), cols = std::max(2, std::min(4, (w + gap()) / (S(200) + gap())));
  bool compact = w / cols < S(210);
  int tw = (w - (cols - 1) * gap()) / cols, th = compact ? S(60) : S(68);
  int y = page->cursor(), i = 0;
  for (const Category *c : st.categories().sorted()) {
    int col = i % cols, row = i / cols;
    std::string id = c->id;
    auto *tile = new CategoryTile(page->left() + col * (tw + gap()), y + row * (th + gap()), tw, th, c->title(),
                                  c->icon, category_count(*c), compact);
    tile->on_open = [id] { navigate(View{View::Category, id}); };
    qa_line("category:" + id, tile->x(), tile->y(), tile->w(), tile->h());
    ++i;
  }
  int rows = (i + cols - 1) / cols;
  page->advance(rows * (th + gap()));
}

// Well-known apps of the departments the user has apps of: the more apps
// of a department are installed, the more of it are recommended (local
// data only, nothing is sent anywhere). |why| names the departments.
std::vector<const App *> recommendations(std::string *why) {
  Store &st = Store::get();
  const Settings &cfg = st.settings();
  std::map<std::string, int> weight;
  for (const App *a : st.installed_apps()) {
    if (!a->is_app()) continue;
    const Origin *o = a->installed_origin();
    if (o && o->source == Source::Debian && cfg.protected_packages.count(o->id)) continue;  // SDE's own
    if (const Category *c = st.categories().category_of(*a)) ++weight[c->id];
  }
  std::vector<std::pair<int, std::string>> order;
  for (auto &w : weight)
    if (cfg.recommended.count(w.first)) order.push_back({w.second, w.first});
  std::sort(order.begin(), order.end(), [](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b) {
    return a.first != b.first ? a.first > b.first : a.second < b.second;
  });
  std::vector<std::string> named;
  std::vector<const App *> out;
  auto take = [&](const std::string &dept, size_t per) {
    auto it = cfg.recommended.find(dept);
    if (it == cfg.recommended.end()) return;
    size_t n = 0;
    for (const App *a : resolve_all(it->second)) {
      if (n >= per || out.size() >= 8) break;
      if (a->installed() || std::find(out.begin(), out.end(), a) != out.end()) continue;
      out.push_back(a);
      ++n;
    }
    if (n) {
      const Category *c = st.categories().find(dept);
      if (c) named.push_back(c->title());
    }
  };
  for (auto &o : order) take(o.second, 3);
  if (out.size() < 4)
    for (auto &kv : cfg.recommended) take(kv.first, 2);
  if (why) {
    std::string list;
    for (size_t i = 0; i < named.size() && i < 3; ++i) list += (i ? ", " : "") + named[i];
    *why = order.empty() || named.empty() ? std::string(_("Well-known apps that are not installed yet"))
                                          : format(_("Because you use apps of %s"), list.c_str());
  }
  return out;
}

void build_explore(Page *page) {
  Store &st = Store::get();
  source_notices(page, false);
  if (!st.catalog_loaded() || (st.catalog().size() == 0 && st.catalog_loading())) {
    // The catalog is being read: the shape of the page while it loads.
    int h = page->h() < S(470) ? S(180) : S(230);
    new Placeholder(page->left(), page->cursor(), page->width(), h, true);
    qa_line("loading", page->left(), page->cursor(), page->width(), h);
    page->advance(h + S(28));
    caption(page, "loading", _("Loading the software catalog\xE2\x80\xA6"));
    placeholder_grid(page, 2);
    page->finish();
    return;
  }
  if (st.catalog().size() == 0) {
    empty_catalog(page);
    page->finish();
    return;
  }
  std::vector<const App *> highlights = resolve_all(st.settings().featured);
  bool short_window = page->h() < S(470);
  if (!highlights.empty()) {
    int h = short_window ? S(190) : S(240);
    auto *b = new Banner(page->left(), page->cursor(), page->width(), h, highlights[0]->id);
    b->on_open = open_app;
    qa_line("banner:" + highlights[0]->id, b->x(), b->y(), b->w(), b->h());
    page->advance(h + S(28));
  }
  if (highlights.size() > 1) {
    caption(page, "featured", _("Highlights"), _("Chosen by your distribution"));
    tile_grid(page, std::vector<const App *>(highlights.begin() + 1, highlights.end()), short_window ? 1 : 2);
    page->advance(S(14));
  }
  std::string why;
  std::vector<const App *> recommended = recommendations(&why);
  if (!recommended.empty()) {
    caption(page, "recommended", _("Recommended for You"), why);
    tile_grid(page, recommended, 1);
    page->advance(S(14));
  }
  // New & Updated: the newest releases of apps that are not installed.
  if (st.settings().new_count > 0) {
    std::vector<const App *> fresh;
    for (auto &a : st.catalog().apps())
      if (a.is_app() && !a.installed() && a.newest_release() > 0 && a.newest_release() <= std::time(nullptr) + 86400)
        fresh.push_back(&a);
    std::stable_sort(fresh.begin(), fresh.end(),
                     [](const App *x, const App *y) { return x->newest_release() > y->newest_release(); });
    if (fresh.size() > size_t(st.settings().new_count)) fresh.resize(size_t(st.settings().new_count));
    if (!fresh.empty()) {
      caption(page, "new", _("New & Updated"), _("The newest releases"));
      tile_grid(page, fresh, short_window ? 1 : 2);
      page->advance(S(14));
    }
  }
  std::vector<const App *> top = resolve_all(st.settings().top);
  if (!top.empty()) {
    caption(page, "top", _("Popular Apps"));
    tile_grid(page, top, 2);
    page->advance(S(14));
  }
  caption(page, "departments", _("Categories"));
  departments(page);
  page->advance(S(18));
  if (st.packages_available()) {
    size_t installed = 0;
    for (auto &p : st.packages()) installed += p.installed;
    std::string text =
        st.packages_loaded() && !st.packages().empty()
            ? format(_("Every package of the repositories, %s of them (%s installed), by section and status, the way "
                       "Synaptic shows them: for libraries, tools and everything that is not an app."),
                     format_count(long(st.packages().size())).c_str(), format_count(long(installed)).c_str())
            : std::string(_("Every package of the repositories by section and status, the way Synaptic shows "
                            "them: for libraries, tools and everything that is not an app."));
    promo_card(page, "packages", Symbol::Package, _("All Software"), text, _("Open All Software"),
               [] { navigate(View{View::Packages, ""}); });
  }
  // Where the catalog comes from, and when the lists were refreshed.
  std::vector<std::string> parts;
  for (int s = 0; s < kSourceCount; ++s)
    if (st.catalog_count(Source(s)))
      parts.push_back(format(_("%s from %s"), apps_count(size_t(st.catalog_count(Source(s)))).c_str(),
                             source_label(Source(s)).c_str()));
  std::string from;
  for (size_t i = 0; i < parts.size(); ++i) from += (i ? ", " : "") + parts[i];
  std::time_t refreshed = std::max(st.refresh_state().finished, st.catalog_sources().lists_time);
  page->note(format(_("%s. Snaps are found when you search. The software lists were refreshed %s."), from.c_str(),
                    when_label(refreshed).c_str()));
  if (!st.catalog_error().empty())
    page->note(format(_("Some software data could not be read: %s"), st.catalog_error().c_str()));
  page->finish();
}

// ------------------------------------------------------------ category

void build_category(Page *page, const std::string &id) {
  Store &st = Store::get();
  const Category *c = st.categories().find(id);
  if (!c) {
    page->note(_("This department does not exist."));
    page->finish();
    return;
  }
  const Category *parent = st.categories().parent_of(c);
  const Category *top = parent ? parent : c;
  st.browse_store(top->id);
  std::vector<const App *> apps = st.categories().apps_in(*c, st.catalog());
  page->heading(c->title(), parent ? format(_("%s in %s"), apps_count(apps.size()).c_str(), parent->title().c_str())
                                   : apps_count(apps.size()));
  if (!top->subcategories.empty()) {
    std::vector<ChipDef> defs;
    std::string top_id = top->id;
    defs.push_back({_("All"), c == top, [top_id] { replace_view(View{View::Category, top_id}); }});
    for (auto &sub : top->subcategories) {
      std::string sid = sub.id;
      defs.push_back({sub.title(), &sub == c, [sid] { replace_view(View{View::Category, sid}); }});
    }
    chips(page, "sub", defs);
  }
  if (!st.catalog_loaded()) {
    placeholder_grid(page, 3);
  } else if (apps.empty()) {
    auto *es = new EmptyState(page->left(), page->cursor(), page->width(), Symbol::Grid,
                              _("There are no apps in this department yet"),
                              st.catalog().size() ? _("Other departments have apps; All Software lists every package.")
                                                  : _("The software catalog is empty: see Home."),
                              false);
    page->advance(es->h() + S(12));
  } else {
    size_t lim = limit_for("category:" + id, 48);
    std::vector<const App *> shown(apps.begin(), apps.begin() + long(std::min(lim, apps.size())));
    tile_grid(page, shown);
    page->advance(S(8));
    more_button(page, "category:" + id, shown.size(), apps.size(), 48);
  }
  page->finish();
}

// -------------------------------------------------------------- search

void build_search(Page *page, const std::string &query) {
  Store &st = Store::get();
  std::vector<const App *> all = search(st.catalog(), query);
  int counts[kSourceCount] = {0, 0, 0};
  for (const App *a : all)
    for (int s = 0; s < kSourceCount; ++s)
      if (a->has_source(Source(s))) ++counts[s];
  std::vector<const App *> results;
  for (const App *a : all)
    if (g_search_source < 0 || a->has_source(Source(g_search_source))) results.push_back(a);
  page->heading(format(_("Results for \xE2\x80\x9C%s\xE2\x80\x9D"), query.c_str()), apps_count(results.size()));
  std::vector<ChipDef> defs;
  defs.push_back({format(_("All %d"), int(all.size())), g_search_source < 0, [] {
                    g_search_source = -1;
                    rebuild_page();
                  }});
  for (int s = 0; s < kSourceCount; ++s) {
    if (!counts[s] && g_search_source != s) continue;
    defs.push_back({format("%s %d", source_label(Source(s)).c_str(), counts[s]), g_search_source == s,
                    [s] {
                      g_search_source = s;
                      rebuild_page();
                    },
                    source_color(Source(s))});
  }
  chips(page, "source", defs);
  if (!st.catalog_loaded()) loading(page, _("Loading the software catalog\xE2\x80\xA6"));
  if (st.store_searching() && st.store_query() == query) page->note(_("Searching the Snap Store\xE2\x80\xA6"));
  if (results.empty() && st.catalog_loaded() && !(st.store_searching() && st.store_query() == query)) {
    page->note(format(_("No apps match \xE2\x80\x9C%s\xE2\x80\x9D."), query.c_str()));
    page->note(_("Try other words, or look in All Software, which has every package."));
  }
  size_t lim = limit_for("search", 40);
  row_list(page, results, AppRow::Browse, lim);
  more_button(page, "search", std::min(lim, results.size()), results.size(), 40);
  // The packages that match, for what is not an app.
  if (st.packages_available() && st.packages_loaded()) {
    PackageFilter f;
    f.query = query;
    size_t n = filter_packages(st.packages(), f, st.marks()).size();
    if (n) {
      page->advance(S(8));
      std::string q = query;
      promo_card(page, "search-packages", Symbol::Package,
                 format(n == 1 ? _("%s package matches") : _("%s packages match"), format_count(long(n)).c_str()),
                 _("All Software searches the names and descriptions of every package, also libraries and tools."),
                 _("Show Packages"), [q] {
                   PackageFilter pf;
                   pf.query = q;
                   navigate(View{View::Packages, packages_view_arg(pf)});
                 });
    }
  }
  page->finish();
}

// ------------------------------------------------------------- details

void details_header(Page *page, const App &a) {
  Store &st = Store::get();
  Source chosen = chosen_source(a.id);
  const Origin *o = a.origin(chosen);
  const Job *job = st.active_job(a.id);
  int x = page->left(), w = page->width(), y = page->cursor();
  int pad = S(28), is = w < S(560) ? S(88) : S(128);
  int tx = x + pad + is + S(28), tw = w - (tx - x) - pad;
  // The height of the text column decides the card's height.
  int summary_h = TextBlock::measure(a.summary, tw, 1);
  auto *card = new Card(x, y, w, S(100));
  new IconView(x + pad, y + pad, is, a.id);
  int cy = y + pad - S(4);
  new Label(tx, cy, tw, S(36), a.name, Label::TITLE);
  cy += S(38);
  std::string by = a.developer.empty() ? "" : format(_("by %s"), a.developer.c_str());
  if (o && o->source == Source::Snap && o->verified) by += by.empty() ? _("verified publisher") : _(" (verified)");
  if (!by.empty()) {
    new Label(tx, cy, tw, S(20), by, Label::DIM);
    cy += S(24);
  }
  if (!a.summary.empty()) {
    new TextBlock(tx, cy, tw, summary_h, a.summary, false, 1);
    cy += summary_h + S(10);
  }
  // Where to get it: one chip per source.
  if (a.origins.size() > 1 || !a.installed()) {
    int cx = tx;
    new Label(cx, cy + S(4), S(70), S(24), _("Source"), Label::DIM);
    text_font(active_theme().font_size, false);
    cx += std::max(S(70), text_width(_("Source")) + S(14));
    for (auto &org : a.origins) {
      std::string text = source_label(org.source);
      std::string ver = org.installed ? org.installed_version : org.version;
      if (!ver.empty()) text += " " + ver;
      if (org.source == Source::Flatpak && org.installation == "user") text += " " + std::string(_("(yours)"));
      int cw = Chip::fit(text) + S(10);
      if (cx + cw > tx + tw && cx > tx + S(80)) {
        cx = tx + S(80);
        cy += S(40);
      }
      auto *chip = new Chip(cx, cy, cw, S(32), text, org.source == chosen);
      chip->dot = source_color(org.source);
      std::string id = a.id;
      Source src = org.source;
      chip->on_click = [id, src] { choose_source(id, src); };
      qa_line(std::string("source:") + source_id(org.source), cx, cy, cw, S(32));
      cx += cw + S(8);
    }
    cy += S(44);
  }
  // Actions, or the progress of the change.
  if (job) {
    int ls = S(56);
    auto *logo = new LogoAnimation(tx, cy, ls, ls);
    logo->running(job->state == Job::Running);
    logo->tooltip(_("Working"));
    int jid = job->id;
    new JobProgress(tx + ls + S(16), cy + S(6), std::max(S(120), tw - ls - S(16) - S(120)), S(40), jid);
    if (job->cancellable || job->state == Job::Queued)
      button(tx + tw - S(110), cy + S(10), _("Cancel"), Button::Normal, [jid] { Store::get().cancel(jid); }, S(110));
    qa_line("job:" + std::to_string(jid), tx, cy, tw, ls);
    cy += ls + S(12);
  } else if (o) {
    int bx = tx;
    auto add = [&](const std::string &text, Button::Kind kind, std::function<void()> fn) {
      Button *b = button(bx, cy, text, kind, std::move(fn));
      qa_line("action:" + a.id + ":" + text, b->x(), b->y(), b->w(), b->h());
      bx += b->w() + S(10);
    };
    std::string id = a.id;
    if (o->installed) {
      if (!a.desktop_ids.empty() || o->source != Source::Debian)
        add(_("Open"), Button::Primary, [id] { Store::get().launch(id); });
      if (o->update) add(_("Update"), Button::Primary, [id] { Store::get().update(id); });
      if (o->source == Source::Debian && st.settings().protected_packages.count(o->id))
        new Label(bx, cy, tw - (bx - tx), S(34), _("Part of the Selviana Desktop Environment"), Label::DIM);
      else if (o->source == Source::Debian && st.system_packages().count(o->id))
        new Label(bx, cy, tw - (bx - tx), S(34), _("Part of the base system"), Label::DIM);
      else
        add(_("Remove"), Button::Danger, [id, chosen] { request_remove(id, chosen); });
    } else {
      bool available = st.status(chosen).available || fake_mode();
      if (!available) {
        new Label(bx, cy, tw, S(34), format(_("%s is not set up on this computer (see Sources)."),
                                              source_label(chosen).c_str()), Label::DIM);
      } else {
        add(_("Install"), Button::Primary, [id, chosen] { request_install(id, chosen); });
        long long size = o->download_size;
        if (size > 0) new Label(bx, cy, tw - (bx - tx), S(34), format(_("%s to download"), format_size(size).c_str()),
                                Label::DIM);
      }
    }
    cy += S(34) + S(16);
  }
  // A failed change stays visible until the next one.
  for (auto it = st.jobs().rbegin(); it != st.jobs().rend(); ++it)
    if (it->app_id == a.id) {
      if (it->state == Job::Failed) {
        std::string msg = format(_("The last change failed: %s"), it->error.c_str());
        int h = TextBlock::measure(msg, tw) + S(4);
        new TextBlock(tx, cy - S(6), tw, h, msg, false);
        cy += h + S(8);
      }
      break;
    }
  int bottom = std::max(cy, y + pad + is + pad);
  card->size(w, bottom - y);
  page->advance(bottom - y + S(24));
}

void build_details(Page *page, const std::string &id) {
  Store &st = Store::get();
  const App *a = st.app(id);
  if (!a) {
    if (!st.catalog_loaded()) {
      loading(page, _("Loading the software catalog\xE2\x80\xA6"));
    } else {
      auto *es = new EmptyState(page->left(), page->cursor(), page->width(), Symbol::Search,
                                _("This app is not in the catalog"),
                                _("It may come from a source that is not set up, or its name changed. Search for it, "
                                  "or look in All Software."),
                                false);
      page->advance(es->h() + S(12));
    }
    page->finish();
    return;
  }
  details_header(page, *a);
  if (!a->screenshots.empty()) {
    int h = ScreenshotStrip::preferred_height(page->width());
    auto *strip = new ScreenshotStrip(page->left(), page->cursor(), page->width(), h, a->screenshots);
    std::vector<Screenshot> shots = a->screenshots;
    std::string name = a->name;
    strip->on_open = [shots, name](int i) { show_screenshot(shots, i, name); };
    qa_line("screenshots", strip->x(), strip->y(), strip->w(), strip->h());
    page->advance(h + S(24));
  }
  if (!a->description.empty()) {
    caption(page, "description", _("Description"));
    paragraph(page, a->description);
    page->advance(S(14));
  }
  if (!a->releases.empty() && !a->releases[0].version.empty()) {
    const Release &r = a->releases[0];
    caption(page, "whats-new", _("What\xE2\x80\x99s New"),
            r.timestamp > 0 ? format(_("Version %s, %s"), r.version.c_str(), release_date(r.timestamp).c_str())
                            : format(_("Version %s"), r.version.c_str()));
    if (!r.description.empty()) paragraph(page, r.description);
    page->advance(S(14));
  }
  // Details (USC: Version, Total size, License, Updates).
  Source chosen = chosen_source(id);
  const Origin *o = a->origin(chosen);
  page->begin_card(_("Details"));
  if (o) {
    std::string version = o->installed ? o->installed_version : o->version;
    if (o->installed && o->update && !o->version.empty())
      version += " " + format(_("(version %s is available)"), o->version.c_str());
    info_row(page, _("Version"), version.empty() ? std::string(_("Unknown")) : version);
    std::string size;
    if (o->installed) {
      if (o->installed_size > 0) size = format(_("%s on disk"), format_size(o->installed_size).c_str());
    } else {
      if (o->download_size > 0) size = format(_("%s to download"), format_size(o->download_size).c_str());
      if (o->installed_size > 0)
        size += (size.empty() ? "" : ", ") + format(_("%s when installed"), format_size(o->installed_size).c_str());
    }
    info_row(page, _("Size"), size.empty() ? std::string(_("Unknown")) : size);
    std::string license = !o->license.empty() ? o->license : a->license;
    info_row(page, _("License"), license.empty() ? std::string(_("Unknown")) : license);
    info_row(page, _("Source"), origin_line(*o));
    if (o->source == Source::Snap)
      info_row(page, _("Confinement"), o->classic ? std::string(_("Classic: it has access to your whole system"))
                                                  : std::string(_("Strict: it runs isolated from the system")));
    if (o->source == Source::Flatpak) info_row(page, _("Isolation"), _("It runs in a sandbox with the permissions "
                                                                       "it asks for"));
    if (o->source == Source::Debian) {
      info_row(page, _("Isolation"), _("Not isolated: like the rest of the system"));
      info_row(page, _("Updates"), o->installed ? _("With the system, by Software Updater") : "");
    }
    if (o->installed) info_row(page, _("Status"), _("Installed"));
  }
  info_row(page, _("Developer"), a->developer);
  if (!a->categories.empty()) {
    std::string dept;
    if (const Category *c = st.categories().category_of(*a)) dept = c->title();
    info_row(page, _("Department"), dept);
  }
  page->end_card();
  // Links.
  int bx = page->left(), by = page->cursor();
  auto link = [&](const std::string &label, const std::string &url) {
    if (!starts_with(url, "https://") && !starts_with(url, "http://")) return;
    Button *b = button(bx, by, label, Button::Normal, [url] { open_uri(url); });
    bx += b->w() + S(10);
  };
  link(_("Website"), a->homepage);
  link(_("Help"), a->help);
  link(_("Report a Problem"), a->bugtracker);
  link(_("Donate"), a->donation);
  // The package behind a Debian app, in All Software.
  if (o && o->source == Source::Debian && st.packages_available() && st.package(o->id)) {
    std::string pkg = o->id;
    Button *b = button(bx, by, format(_("Package %s"), pkg.c_str()), Button::Ghost,
                       [pkg] { navigate(View{View::Package, pkg}); });
    qa_line("action:package:" + pkg, b->x(), b->y(), b->w(), b->h());
    bx += b->w() + S(10);
  }
  if (bx > page->left()) page->advance(S(34) + S(16));
  page->finish();
  // Sizes, versions and the Snap Store's version arrive later.
  st.fetch_details(id);
}

// ----------------------------------------------------------- installed

void build_installed(Page *page) {
  Store &st = Store::get();
  std::vector<const App *> all = st.installed_apps(), apps;
  int counts[kSourceCount] = {0, 0, 0};
  for (const App *a : all) {
    ++counts[int(a->installed_origin()->source)];
    if (g_installed_source < 0 || a->installed_origin()->source == Source(g_installed_source)) apps.push_back(a);
  }
  page->heading(_("Installed"), format(_("%s on this computer"), apps_count(all.size()).c_str()));
  std::vector<ChipDef> defs;
  defs.push_back({format(_("All %d"), int(all.size())), g_installed_source < 0, [] {
                    g_installed_source = -1;
                    rebuild_page();
                  }});
  for (int s = 0; s < kSourceCount; ++s)
    if (counts[s] || g_installed_source == s)
      defs.push_back({format("%s %d", source_label(Source(s)).c_str(), counts[s]), g_installed_source == s,
                      [s] {
                        g_installed_source = s;
                        rebuild_page();
                      },
                      source_color(Source(s))});
  chips(page, "source", defs);
  if (!st.all_installed_loaded() || !st.catalog_loaded()) loading(page, _("Looking for installed software\xE2\x80\xA6"));
  // Grouped by department, like Ubuntu Software Center.
  std::map<const Category *, std::vector<const App *>> groups;
  std::vector<const App *> other;
  for (const App *a : apps) {
    const Category *c = st.categories().category_of(*a);
    if (c)
      groups[c].push_back(a);
    else
      other.push_back(a);
  }
  for (const Category *c : st.categories().sorted()) {
    auto it = groups.find(c);
    if (it == groups.end()) continue;
    caption(page, c->id, c->title(), "");
    row_list(page, it->second, AppRow::Installed, 1000);
    page->advance(S(10));
  }
  if (!other.empty()) {
    caption(page, "other", _("Other"));
    row_list(page, other, AppRow::Installed, 1000);
  }
  if (apps.empty() && st.all_installed_loaded()) page->note(_("No apps from this source are installed."));
  page->note(_("Apps that came with the system (a web browser, an office suite, an email program) can be removed "
               "like any other; what goes with them is listed first. Only the parts the system or the Selviana "
               "Desktop Environment need stay. Everything else that is installed is in All Software."));
  page->finish();
}

// ------------------------------------------------------------- updates

void build_updates(Page *page) {
  Store &st = Store::get();
  // Merkadu updates the apps of Flatpak and Snap; the system (Debian's
  // packages, its apps too) is Software Updater's.
  std::vector<const App *> apps, system_apps;
  for (const App *a : st.updatable_apps()) {
    bool own = false, debian = false;
    for (auto &o : a->origins)
      if (o.installed && o.update) (o.source == Source::Debian ? debian : own) = true;
    if (own) apps.push_back(a);
    else if (debian) system_apps.push_back(a);
  }
  page->heading(_("Updates"), _("Apps from Flathub and the Snap Store"));
  if (st.refresh_state().running)
    notice(page, "refreshing", _("Looking for updates\xE2\x80\xA6"), 0, true);
  if (!apps.empty()) {
    Button *b = button(page->left(), page->cursor(), format(_("Update All (%d)"), int(apps.size())), Button::Primary,
                       [] { Store::get().update_all(); });
    qa_line("action:update-all", b->x(), b->y(), b->w(), b->h());
    page->advance(S(34) + S(18));
    row_list(page, apps, AppRow::Updates, 1000);
  } else if (!st.all_installed_loaded()) {
    loading(page, _("Looking for updates\xE2\x80\xA6"));
  } else {
    auto *es = new EmptyState(page->left(), page->cursor(), page->width(), Symbol::Check,
                              _("Your apps are up to date"),
                              _("Apps from Flathub and snaps are updated here. Snaps also update themselves in the "
                                "background."),
                              false);
    qa_line("empty:updates", es->x(), es->y(), es->w(), es->h());
    page->advance(es->h() + S(12));
  }
  page->advance(S(8));
  // The system's updates: counted here, installed by Software Updater.
  size_t packages = 0;
  for (auto &p : st.packages()) packages += p.upgradable;
  page->begin_card(_("System Updates"));
  std::string text;
  if (packages > 0 || !system_apps.empty()) {
    std::string names;
    for (size_t i = 0; i < system_apps.size() && i < 4; ++i) names += (i ? ", " : "") + system_apps[i]->name;
    if (system_apps.size() > 4) names += format(_(" and %d more"), int(system_apps.size() - 4));
    if (packages == 1)
      text = format(_("1 package of %s can be updated."), source_label(Source::Debian).c_str());
    else if (packages > 1)
      text = format(_("%s packages of %s can be updated."), format_count(long(packages)).c_str(),
                    source_label(Source::Debian).c_str());
    if (!names.empty()) text += (text.empty() ? "" : " ") + format(_("Among them: %s."), names.c_str());
    text += " ";
  }
  text += format(_("The operating system and the apps that come with %s (the kernel, libraries, security fixes) are "
                   "updated together by Software Updater, not here; an app page can still update its app alone."),
                 source_label(Source::Debian).c_str());
  page->note(text);
  if (!find_program("sde-update").empty() || fake_mode()) {
    page->advance(S(4));
    Button *b = button(page->left() + S(20), page->cursor(), _("Open Software Updater"), Button::Normal,
                       [] { spawn_detached({"sde-update"}); });
    qa_line("action:software-updater", b->x(), b->y(), b->w(), b->h());
    page->advance(S(34) + S(6));
  }
  page->end_card();
  page->finish();
}

// ------------------------------------------------------------- history

void build_history(Page *page) {
  Store &st = Store::get();
  page->heading(_("History"), _("Software installed, updated and removed on this computer"));
  struct F {
    const char *label;
    int change;
  };
  const F filters[] = {{N_("All Changes"), -1}, {N_("Installations"), int(Change::Install)},
                       {N_("Updates"), int(Change::Update)}, {N_("Removals"), int(Change::Remove)}};
  std::vector<ChipDef> defs;
  for (auto &f : filters) {
    int ch = f.change;
    defs.push_back({_(f.label), g_history_filter == ch, [ch] {
                      g_history_filter = ch;
                      rebuild_page();
                    }});
  }
  chips(page, "filter", defs);
  if (!st.history_loaded()) {
    st.load_history();
    loading(page, _("Reading the history\xE2\x80\xA6"));
    page->finish();
    return;
  }
  std::vector<const HistoryEntry *> shown;
  for (auto &e : st.history())
    if (g_history_filter < 0 || int(e.change) == g_history_filter) shown.push_back(&e);
  if (shown.empty()) page->note(_("Nothing has changed yet."));
  size_t lim = limit_for("history", 120);
  std::time_t now = std::time(nullptr);
  long day = -1;
  for (size_t i = 0; i < shown.size() && i < lim; ++i) {
    const HistoryEntry &e = *shown[i];
    if (local_day(e.when) != day) {
      day = local_day(e.when);
      page->advance(i ? S(8) : 0);
      caption(page, "day:" + std::to_string(day), day_label(e.when, now));
    }
    auto *row = new HistoryRow(page->left(), page->cursor(), page->width(), S(62), e);
    qa_line("history:" + std::to_string(i), row->x(), row->y(), row->w(), row->h());
    page->advance(S(62) + S(8));
  }
  more_button(page, "history", std::min(lim, shown.size()), shown.size(), 120);
  page->note(_("From the package system's log (/var/log/apt/history.log), Flatpak's history and snapd's changes."));
  page->finish();
}

// ------------------------------------------------------------ progress

void build_progress(Page *page) {
  Store &st = Store::get();
  const Job *current = nullptr;
  for (auto &j : st.jobs())
    if (j.state == Job::Running) current = &j;
  if (!current)
    for (auto &j : st.jobs())
      if (j.state == Job::Queued) {
        current = &j;
        break;
      }
  int active = st.active_jobs();
  page->heading(_("In Progress"), active == 1 ? std::string(_("1 change to make"))
                                  : active    ? format(_("%d changes to make"), active)
                                              : std::string(_("Nothing is being installed or removed.")));
  int x = page->left(), w = page->width();
  if (current) {
    int y = page->cursor(), size = std::min(S(150), page->h() / 3);
    int h = size + S(40) + S(130);
    new Card(x, y, w, h);
    auto *logo = new LogoAnimation(x + (w - size) / 2, y + S(20), size, size);
    logo->running(current->state == Job::Running);
    logo->tooltip(_("Working"));
    qa_line("logo", logo->x(), logo->y(), logo->w(), logo->h());
    int ty = y + S(20) + size + S(14);
    std::string title = action_progress_label(current->action) + " " + current->title;
    text_font(active_theme().font_size + 1, true);
    int tw = std::min(w - S(40), text_width(title) + S(8));
    new Label(x + (w - tw) / 2, ty, tw, S(30), title, Label::BOLD);
    int pw = std::min(w - S(80), S(520));
    new JobProgress(x + (w - pw) / 2, ty + S(34), pw, S(40), current->id);
    qa_line("job:" + std::to_string(current->id), x + (w - pw) / 2, ty + S(34), pw, S(40));
    int jid = current->id;
    if (current->cancellable || current->state == Job::Queued)
      button(x + (w - S(120)) / 2, ty + S(82), _("Cancel"), Button::Normal, [jid] { Store::get().cancel(jid); },
             S(120));
    page->advance(h + S(20));
  }
  bool any_finished = false;
  for (auto it = st.jobs().rbegin(); it != st.jobs().rend(); ++it) {
    const Job &j = *it;
    if (&j == current) continue;
    any_finished = any_finished || !j.active();
    int y = page->cursor(), h = S(66);
    new Card(x, y, w, h);
    if (!j.app_id.empty() && st.app(j.app_id)) new IconView(x + S(14), y + (h - S(40)) / 2, S(40), j.app_id);
    int tx = x + S(68);
    new Label(tx, y + S(8), w - S(88), S(24), action_label(j.action) + ": " + j.title, Label::BOLD);
    new JobProgress(tx, y + S(32), w - S(88), S(30), j.id);
    qa_line("job:" + std::to_string(j.id), x, y, w, h);
    page->advance(h + S(8));
  }
  if (any_finished) {
    page->advance(S(8));
    button(x, page->cursor(), _("Clear Finished"), Button::Normal, [] { Store::get().clear_finished(); });
    page->advance(S(34) + S(10));
  }
  page->finish();
}

// ------------------------------------------------------------- sources

void build_sources(Page *page) {
  Store &st = Store::get();
  page->heading(_("Sources"), _("Where Merkadu gets software from"));
  const RefreshState &r = st.refresh_state();
  for (auto &e : r.errors) notice(page, "refresh-error", e, 2);
  for (auto &n : r.notes) notice(page, "refresh-note", n, 0);
  // The refresh of every list.
  {
    std::string text = r.running ? r.status
                                 : format(_("The software lists were refreshed %s. Merkadu refreshes them when it "
                                            "starts (when they are older than %d minutes) and with Refresh."),
                                          when_label(std::max(r.finished, st.catalog_sources().lists_time)).c_str(),
                                          st.settings().refresh_minutes);
    notice(page, "refresh", text, 0, r.running,
           r.running ? std::vector<NoticeButton>{}
                     : std::vector<NoticeButton>{{_("Refresh Now"), [] { Store::get().refresh(true); }, true}});
  }
  // Debian.
  const SourceStatus &deb = st.status(Source::Debian);
  const CatalogSources &cs = st.catalog_sources();
  page->begin_card(format(_("%s packages"), source_label(Source::Debian).c_str()));
  page->note(_("Software from the distribution's archive: tested together with the system and updated with it by "
               "Software Updater. Merkadu installs and removes it with PackageKit; the administrator password is "
               "asked when needed."));
  info_row(page, _("Status"), deb.available ? (deb.message.empty() ? std::string(_("Ready")) : deb.message)
                                            : (deb.message.empty() ? std::string(_("PackageKit is not available"))
                                                                   : deb.message));
  std::string catalog;
  if (!cs.appstream_apt)
    catalog = _("Not downloaded: the appstream package is missing");
  else if (cs.dep11 == 0)
    catalog = _("Not downloaded yet: refresh the software lists");
  else
    catalog = format(_("%s from %d catalogs; icons: %s"), apps_count(size_t(st.catalog_count(Source::Debian))).c_str(),
                     cs.dep11, cs.dep11_icons ? _("downloaded") : _("from the catalog's website"));
  info_row(page, _("App catalog"), catalog);
  if (st.packages_available() && st.packages_loaded())
    info_row(page, _("Packages"), format(_("%s in the package lists (All Software)"),
                                         format_count(long(st.packages().size())).c_str()));
  page->advance(S(4));
  int bx = page->left() + S(20);
  if (!cs.appstream_apt && !fake_mode()) {
    Button *b = button(bx, page->cursor(), _("Install the Catalog"), Button::Primary,
                       [] { install_support(_("The software catalog (appstream)"), {"appstream"}); });
    bx += b->w() + S(10);
  }
  if (!find_program("sde-update").empty()) {
    button(bx, page->cursor(), _("Package Sources and Updates"), Button::Normal,
           [] { spawn_detached({"sde-update", "--page", "sources"}); });
  }
  if (bx != page->left() + S(20) || !find_program("sde-update").empty()) page->advance(S(34) + S(6));
  page->end_card();
  // Flatpak.
  const SourceStatus &fp = st.status(Source::Flatpak);
  page->begin_card("Flatpak");
  page->note(_("Apps from Flathub and other Flatpak remotes run isolated from the system with the permissions they "
               "ask for, and bring the libraries they need. They are installed for every user and updated here."));
  if (!fp.installed) {
    info_row(page, _("Status"), fp.message.empty() ? std::string(_("Flatpak is not installed")) : fp.message);
    page->advance(S(4));
    button(page->left() + S(20), page->cursor(), _("Install Flatpak Support"), Button::Primary,
           [] { install_support(_("Flatpak support"), {"flatpak"}); });
    page->advance(S(34) + S(6));
  } else {
    std::string remotes;
    for (auto &rm : fp.remotes) remotes += (remotes.empty() ? "" : ", ") + rm;
    info_row(page, _("Remotes"), remotes.empty() ? std::string(_("None")) : remotes);
    info_row(page, _("App catalog"), st.catalog_count(Source::Flatpak)
                                         ? apps_count(size_t(st.catalog_count(Source::Flatpak)))
                                         : std::string(_("Not downloaded yet: refresh the software lists")));
    if (!fp.has_flathub) {
      page->advance(S(4));
      Button *b = button(page->left() + S(20), page->cursor(), _("Set Up Flathub"), Button::Primary, setup_flathub);
      qa_line("action:flathub", b->x(), b->y(), b->w(), b->h());
      page->advance(S(34) + S(6));
    }
  }
  page->end_card();
  // Snap.
  const SourceStatus &sn = st.status(Source::Snap);
  page->begin_card("Snap");
  page->note(_("Snaps come from the Snap Store through snapd, which you install yourself if you want it. They "
               "update themselves in the background."));
  if (!sn.installed) {
    info_row(page, _("Status"), _("Not installed"));
    page->advance(S(4));
    Button *b = button(page->left() + S(20), page->cursor(), _("Enable Snap Support"), Button::Normal, enable_snap);
    qa_line("action:enable-snap", b->x(), b->y(), b->w(), b->h());
    page->advance(S(34) + S(10));
    page->note(_("Merkadu installs snapd and the snapd snap (as Debian's instructions do, with your password). "
                 "Restart the computer or log out and back in afterwards."));
  } else {
    info_row(page, _("Status"), sn.available ? std::string(_("snapd is running")) : sn.message);
    if (g_snap_enabled_now || (!sn.available && sn.installed))
      page->note(_("Restart the computer (or log out and back in) to finish: then snap apps start, appear in the "
                   "Start menu and their commands are found in /snap/bin."));
    std::string folder = sn.hidden_snap_folder == 1   ? std::string(_("~/.snap/data (hidden)"))
                         : sn.hidden_snap_folder == 0 ? std::string(_("~/snap"))
                                                      : std::string(_("Unknown"));
    info_row(page, _("Snap data"), folder);
    if (sn.hidden_snap_folder == 0 && sn.available) {
      page->advance(S(4));
      button(page->left() + S(20), page->cursor(), _("Hide the Snap Folder"), Button::Normal, [] {
        Store::get().backend(Source::Snap)->setup("hide-snap-folder", [](bool ok, bool, std::string error) {
          Store::get().probe_sources();
          if (!ok) fprintf(stderr, "merkadu: %s\n", error.c_str());
        });
      });
      page->advance(S(34) + S(6));
    }
    page->note(_("snapd keeps each user's snap data in their home folder: snaps may only write there, and the data "
                 "belongs to that user. It cannot be moved to a system folder such as /usr/share. Merkadu asks snapd "
                 "to use the hidden folder ~/.snap/data instead of ~/snap before it installs the first snap."));
  }
  page->end_card();
  page->finish();
}

}  // namespace

std::string view_title(const View &v) {
  switch (v.kind) {
    case View::Category: {
      const Category *c = Store::get().categories().find(v.arg);
      return c ? c->title() : std::string(_("Departments"));
    }
    case View::Search: return _("Search");
    case View::Details: {
      const App *a = Store::get().app(v.arg);
      return a ? a->name : std::string(_("Details"));
    }
    case View::Installed: return _("Installed");
    case View::Updates: return _("Updates");
    case View::History: return _("History");
    case View::Progress: return _("In Progress");
    case View::Sources: return _("Sources");
    case View::Packages: return packages_title(v.arg);
    case View::Package: return v.arg;
    case View::Changes: return _("Review Changes");
    default: return _("Home");
  }
}

void build_page(Page *page, const View &v) {
  switch (v.kind) {
    case View::Category: build_category(page, v.arg); break;
    case View::Search: build_search(page, v.arg); break;
    case View::Details: build_details(page, v.arg); break;
    case View::Installed: build_installed(page); break;
    case View::Updates: build_updates(page); break;
    case View::History: build_history(page); break;
    case View::Progress: build_progress(page); break;
    case View::Sources: build_sources(page); break;
    case View::Packages: build_packages_page(page, v.arg); break;
    case View::Package: build_package_page(page, v.arg); break;
    case View::Changes: build_changes_page(page); break;
    default: build_explore(page); break;
  }
}

}  // namespace merkadu
}  // namespace sde
