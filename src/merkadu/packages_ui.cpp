// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "packages_ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <map>
#include <set>

#include "i18n.h"
#include "icons.h"
#include "menu.h"
#include "store.h"
#include "ui.h"
#include "util.h"
#include "watchdog.h"
#include "window.h"
#include "../settings/ui.h"

namespace sde {
namespace merkadu {

// ---------------------------------------------------------------- views

std::string packages_view_arg(const PackageFilter &f) {
  std::string q = f.query;
  std::replace(q.begin(), q.end(), '|', ' ');
  std::replace(q.begin(), q.end(), '\t', ' ');
  bool other = f.source != Source::Debian || f.runtimes;
  if (f.status == PackageStatus::All && f.section.empty() && q.empty() && !other) return "";
  std::string arg = std::string(package_status_id(f.status)) + "|" + f.section + "|" + q;
  // STATUS|SECTION|QUERY|SOURCE|runtimes (Debian without the last two).
  if (other) arg += std::string("|") + source_id(f.source) + (f.runtimes ? "|runtimes" : "");
  return arg;
}

PackageFilter packages_filter(const std::string &arg) {
  PackageFilter f;
  auto parts = split(arg, '|', true);
  if (parts.size() > 0) package_status_from_id(parts[0], &f.status);
  if (parts.size() > 1) f.section = parts[1];
  if (parts.size() > 2) f.query = parts[2];
  if (parts.size() > 3) source_from_id(parts[3], &f.source);
  f.runtimes = parts.size() > 4 && parts[4] == "runtimes" && f.source == Source::Flatpak;
  return f;
}

namespace {

// The name of a section of |source|'s rows: Debian's sections, the
// departments for Flatpak and Snap, the runtimes.
std::string row_section_title(Source source, const std::string &id) {
  if (source == Source::Debian) return section_title(id);
  if (id == "runtimes") return _("Runtimes and Extensions");
  if (id.empty()) return _("Other");
  if (const Category *c = Store::get().categories().find(id)) return c->title();
  return id;
}

std::vector<std::string> row_section_icons(Source source, const std::string &id) {
  if (source == Source::Debian) return section_icon_names(id);
  if (id == "runtimes") return {"application-x-sharedlib", "package-x-generic"};
  if (const Category *c = Store::get().categories().find(id))
    if (!c->icon.empty()) return {c->icon, "applications-other"};
  return {"applications-other", "package-x-generic"};
}

// "Debian packages", "Flatpak", "Snap".
std::string source_switch_label(Source s) {
  return s == Source::Debian ? format(_("%s packages"), source_label(s).c_str()) : source_label(s);
}

}  // namespace

std::string packages_title(const std::string &arg) {
  PackageFilter f = packages_filter(arg);
  return f.section.empty() ? std::string(_("All Software")) : row_section_title(f.source, f.section);
}

namespace {

std::string status_title(PackageStatus s) {
  switch (s) {
    case PackageStatus::Installed: return _("Installed");
    case PackageStatus::Upgradable: return _("Updates");
    case PackageStatus::NotInstalled: return _("Not Installed");
    case PackageStatus::Marked: return _("Marked");
    case PackageStatus::ConfigOnly: return _("Settings Left");
    default: return _("All");
  }
}

void go_filter(const PackageFilter &f) { replace_view(View{View::Packages, packages_view_arg(f)}); }

Button *button(int x, int y, const std::string &text, Button::Kind kind, std::function<void()> fn, int w = 0) {
  auto *b = new Button(x, y, w ? w : Button::fit(text), S(34), text, kind);
  b->on_click = std::move(fn);
  return b;
}

// What the page keeps across rebuilds.
struct BrowserState {
  std::string selected;  // package name
  int top = 0;           // first row shown
  std::string arg;       // the filter the two are for
  bool focused = false;  // the list had the keyboard
};
BrowserState g_state;

// The filtered list, kept while the packages, the filter and the marks stay.
struct ListCache {
  int generation = -1;
  std::string arg;
  std::string marks;
  std::vector<size_t> rows;
  size_t counts[kPackageStatusCount] = {};
};
ListCache g_list;

std::string marks_signature(const ChangeSet &m) {
  std::string s;
  for (auto &kv : m.all()) s += kv.first + char('0' + int(kv.second)) + ";";
  return s;
}

const ListCache &filtered(const std::string &arg) {
  Store &st = Store::get();
  PackageFilter f = packages_filter(arg);
  const std::vector<Package> &rows = st.rows(f.source, f.runtimes);
  // Marks are Debian's (APT's simulation); the other sources act at once.
  std::string sig = f.source == Source::Debian ? marks_signature(st.marks()) : std::string();
  int gen = st.rows_generation(f.source);
  if (g_list.generation == gen && g_list.arg == arg && g_list.marks == sig) return g_list;
  g_list.generation = gen;
  g_list.arg = arg;
  g_list.marks = sig;
  watchdog::Busy busy("filtering the packages", long(rows.size()));
  g_list.rows = filter_packages(rows, f, st.marks());
  for (auto &c : g_list.counts) c = 0;
  for (auto &p : rows) {
    if (p.runtime && !f.runtimes) continue;
    if (!f.section.empty() && section_base(p.section) != f.section) continue;
    for (int s = 0; s < kPackageStatusCount; ++s) g_list.counts[s] += status_matches(p, PackageStatus(s), st.marks());
  }
  return g_list;
}

// The marks that make sense for a package.
std::vector<std::pair<Mark, std::string>> mark_choices(const Package &p, Mark current) {
  std::vector<std::pair<Mark, std::string>> out;
  if (current != Mark::Unmarked) out.push_back({Mark::Unmarked, _("Unmark")});
  if (!p.installed && !p.candidate_version.empty() && current != Mark::Install)
    out.push_back({Mark::Install, _("Mark for Installation")});
  if (p.installed && p.upgradable && current != Mark::Upgrade) out.push_back({Mark::Upgrade, _("Mark for Update")});
  if ((p.installed || p.config_only) && current != Mark::Remove) out.push_back({Mark::Remove, _("Mark for Removal")});
  return out;
}

// Why a package cannot be marked for removal ("" when it can).
std::string removal_block(const Package &p) {
  Store &st = Store::get();
  if (st.settings().protected_packages.count(p.name.substr(0, p.name.find(':'))))
    return _("Part of the Selviana Desktop Environment: removed together with the desktop, not on its own.");
  if (st.system_packages().count(p.name))
    return p.essential ? _("Essential for the system: the computer may not start or work without it.")
                       : _("Part of the base system: the computer may not start or work without it.");
  return "";
}

void set_mark(const std::string &name, Mark m) {
  Store &st = Store::get();
  const Package *p = st.package(name);
  if (!p) return;
  // Only the changes the package allows (the keys mark without a menu).
  if ((m == Mark::Remove && !p->installed && !p->config_only) || (m == Mark::Upgrade && !p->upgradable) ||
      (m == Mark::Install && (p->installed || p->candidate_version.empty())))
    return;
  if (m == Mark::Remove) {
    std::string why = removal_block(*p);
    if (!why.empty()) {
      fl_alert("%s\n\n%s", name.c_str(), why.c_str());
      return;
    }
  }
  st.mark(name, m);
}

// Space and double click: the most likely mark, or unmark.
void toggle_mark(const std::string &name) {
  Store &st = Store::get();
  const Package *p = st.package(name);
  if (!p) return;
  Mark cur = st.marks().get(name);
  if (cur != Mark::Unmarked) set_mark(name, Mark::Unmarked);
  else if (!p->installed) set_mark(name, Mark::Install);
  else if (p->upgradable) set_mark(name, Mark::Upgrade);
  else set_mark(name, Mark::Remove);
}

void mark_menu(const std::string &name, bool details) {
  Store &st = Store::get();
  const Package *p = st.package(name);
  if (!p) return;
  ContextMenu menu;
  for (auto &c : mark_choices(*p, st.marks().get(name))) {
    Mark m = c.first;
    std::string n = name;
    menu.item(c.second, [n, m] { set_mark(n, m); });
  }
  if (details) {
    menu.separator();
    std::string n = name;
    menu.item(_("Details"), [n] { navigate(View{View::Package, n}); });
    if (const App *a = st.app_of_package(name)) {
      std::string id = a->id;
      menu.item(_("App Page"), [id] { navigate(View{View::Details, id}); });
    }
  }
  menu.popup();
}

// ------------------------------------------------- Flatpak and Snap rows

// A row's page: the package page (Debian) or the app page.
void open_row(Source source, const std::string &name) {
  if (source == Source::Debian) {
    navigate(View{View::Package, name});
    return;
  }
  const Package *p = Store::get().row(source, name);
  if (p && !p->app_id.empty()) navigate(View{View::Details, p->app_id});
}

// Removes a Flatpak runtime or extension (libflatpak refuses while an
// installed app needs it, and says which).
void remove_runtime(const Package &p) {
  if (fl_choice(_("Remove %s?\n\nApps that need it stop working until it is installed again; Flatpak does not "
                  "remove it while an installed app needs it."),
                _("Cancel"), _("Remove"), nullptr, p.label().c_str()) != 1)
    return;
  Job j;
  j.action = Action::Remove;
  j.source = Source::Flatpak;
  j.title = p.label();
  j.icon = "application-x-sharedlib";
  j.targets = {(p.installation.empty() ? std::string("system") : p.installation) + ":" + p.ref};
  if (Store::get().enqueue(j)) navigate(View{View::Progress, ""});
}

using RowAction = std::pair<std::string, std::function<void()>>;

// What can be done with a row of Flatpak or Snap: at once, there is no
// marking (that is APT's simulation of Debian packages).
std::vector<RowAction> row_actions(const Package &p) {
  std::vector<RowAction> out;
  if (p.runtime) {
    if (p.installed) {
      Package copy = p;
      out.push_back({_("Remove"), [copy] { remove_runtime(copy); }});
    }
    return out;
  }
  Source s = p.source;
  std::string app = p.app_id;
  if (Store::get().active_job(app)) return out;
  if (!p.installed) out.push_back({_("Install"), [app, s] { request_install(app, s); }});
  if (p.upgradable) out.push_back({_("Update"), [app] { Store::get().update(app); }});
  if (p.installed) out.push_back({_("Remove"), [app, s] { request_remove(app, s); }});
  return out;
}

void row_menu(Source source, const std::string &name) {
  const Package *p = Store::get().row(source, name);
  if (!p) return;
  ContextMenu menu;
  for (auto &a : row_actions(*p)) menu.item(a.first, a.second);
  if (!p->app_id.empty()) {
    menu.separator();
    std::string id = p->app_id;
    menu.item(_("App Page"), [id] { navigate(View{View::Details, id}); });
  }
  menu.popup();
}

// The state of a package as a short text and a color.
std::string state_text(const Package &p, Mark m) {
  switch (m) {
    case Mark::Install: return _("Marked for installation");
    case Mark::Remove: return _("Marked for removal");
    case Mark::Upgrade: return _("Marked for update");
    default: break;
  }
  if (p.broken) return _("Installed, broken dependencies");
  if (p.config_only) return _("Removed, its settings are left");
  if (p.installed && p.upgradable) return _("Installed, an update is available");
  if (p.installed) return p.automatic ? _("Installed as a dependency") : _("Installed");
  return _("Not installed");
}

void paint_status(Painter &p, const Package &pkg, Mark m, double x, double y, double s) {
  const Theme &t = active_theme();
  Color white = Color::hex(0xFFFFFF);
  double r = s / 2, cx = x + r, cy = y + r, g = s * 0.62;
  auto glyph = [&](Symbol sym, const Color &c) { paint_symbol(p, sym, cx - g / 2, cy - g / 2, g, c); };
  if (m == Mark::Install) {
    p.circle(cx, cy, r, t.accent);
    glyph(Symbol::Plus, t.on_accent);
  } else if (m == Mark::Remove) {
    p.circle(cx, cy, r, t.error);
    glyph(Symbol::Minus, white);
  } else if (m == Mark::Upgrade) {
    p.circle(cx, cy, r, t.accent);
    glyph(Symbol::Updates, t.on_accent);
  } else if (pkg.broken) {
    glyph(Symbol::Warning, readable(t.warning, t.surface));
  } else if (pkg.installed && pkg.upgradable) {
    p.circle(cx, cy, r, t.accent.alpha(0.18));
    glyph(Symbol::Updates, t.accent_text);
  } else if (pkg.installed) {
    p.circle(cx, cy, r, t.success.alpha(0.9));
    glyph(Symbol::Check, white);
  } else {
    cairo_t *cr = p.cr();
    p.set(pkg.config_only ? t.text_dim.alpha(0.6) : t.border);
    cairo_set_line_width(cr, Sd(1.5));
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r - Sd(1), 0, 2 * M_PI);
    cairo_stroke(cr);
  }
}

// --------------------------------------------------------------- table

// The packages, one row each, drawn as they scroll into view: 90,000
// rows cost no more than the 15 shown.
class PackageTable : public Fl_Group {
 public:
  PackageTable(int x, int y, int w, int h, std::vector<size_t> rows, Source source = Source::Debian,
               bool runtimes = false)
      : Fl_Group(x, y, w, h), rows_(std::move(rows)), source_(source), runtimes_(runtimes) {
    bar_ = new Fl_Scrollbar(x + w - S(12), y + header_h(), S(12), h - header_h());
    bar_->type(FL_VERTICAL);
    bar_->callback([](Fl_Widget *w, void *data) {
      auto *t = static_cast<PackageTable *>(data);
      t->top(int(static_cast<Fl_Scrollbar *>(w)->value()));
    }, this);
    end();
    tooltip(_("Packages"));
    for (size_t i = 0; i < rows_.size(); ++i)
      if (name(int(i)) == g_state.selected) sel_ = int(i);
    top(g_state.arg == current_view().arg ? g_state.top : 0);
    if (sel_ >= 0 && (sel_ < top_ || sel_ >= top_ + visible())) top(sel_ - visible() / 2);
  }
  int row_h() const { return S(46); }
  int header_h() const { return S(30); }
  int visible() const { return std::max(1, (h() - header_h()) / row_h()); }
  int count() const { return int(rows_.size()); }
  int first() const { return top_; }

  const Package *pkg(int i) const {
    const auto &all = Store::get().rows(source_, runtimes_);
    if (i < 0 || i >= count() || rows_[size_t(i)] >= all.size()) return nullptr;
    return &all[rows_[size_t(i)]];
  }
  Source source() const { return source_; }
  // The app of a row (its app page), nullptr for packages without one.
  const App *app_of(const Package &p) const {
    return source_ == Source::Debian ? Store::get().app_of_package(p.name)
                                     : p.app_id.empty() ? nullptr : Store::get().app(p.app_id);
  }
  std::string name(int i) const {
    const Package *p = pkg(i);
    return p ? p->name : std::string();
  }

  void top(int t) {
    top_ = std::max(0, std::min(t, count() - visible()));
    g_state.top = top_;
    g_state.arg = current_view().arg;
    bar_->value(top_, visible(), 0, std::max(visible(), count()));
    // A thumb that can be grabbed, also for 90,000 rows.
    bar_->slider_size(std::max(double(bar_->slider_size()), double(S(36)) / std::max(1, bar_->h())));
    if (count() <= visible()) bar_->hide();
    else bar_->show();
    redraw();
  }

  void select(int i, bool rebuild = true) {
    if (count() == 0) return;
    i = std::max(0, std::min(i, count() - 1));
    sel_ = i;
    if (sel_ < top_) top(sel_);
    else if (sel_ >= top_ + visible()) top(sel_ - visible() + 1);
    g_state.selected = name(sel_);
    g_state.arg = current_view().arg;
    redraw();
    if (rebuild) rebuild_page();  // the details of the selected package
  }

  // Columns: they appear as the table gets wider.
  struct Cols {
    int status, icon, name, installed, latest, size, end;
  };
  Cols cols() const {
    Cols c;
    c.end = x() + w() - (bar_->visible() ? S(16) : S(4));
    c.status = x() + S(12);
    c.icon = c.status + S(30);
    c.name = c.icon + S(36);
    int right = c.end;
    c.size = w() >= S(860) ? (right -= S(90)) : -1;
    c.latest = w() >= S(620) ? (right -= S(160)) : -1;
    c.installed = w() >= S(760) ? (right -= S(160)) : -1;
    return c;
  }

  void draw() override {
    const Theme &t = active_theme();
    Cols c = cols();
    Store &st = Store::get();
    int n = std::min(count() - top_, visible() + 1);
    {
      Painter p(window()->w(), window()->h());
      double r = std::min(double(t.radius), Sd(12));
      p.fill_rounded(x(), y(), w(), h(), r, t.surface);
      p.fill_rounded(x(), y(), w(), header_h(), r, t.surface_alt);
      p.fill_rect(x(), y() + header_h() - r, w(), r, t.surface_alt);
      p.line(x(), y() + header_h() - 0.5, x() + w(), y() + header_h() - 0.5, t.border);
      cairo_save(p.cr());
      cairo_rectangle(p.cr(), x(), y() + header_h(), w(), h() - header_h());
      cairo_clip(p.cr());
      for (int k = 0; k < n; ++k) {
        int i = top_ + k, ry = y() + header_h() + k * row_h();
        const Package *pk = pkg(i);
        if (!pk) continue;
        if (i == sel_) {
          p.fill_rounded(x() + S(4), ry + S(2), w() - S(8) - (bar_->visible() ? S(12) : 0), row_h() - S(4), S(8),
                         t.accent.alpha(Fl::focus() == this ? 0.22 : 0.13));
          p.fill_rounded(x() + S(5), ry + S(12), Sd(3), row_h() - S(24), Sd(1.5), t.accent);
        } else if (i == hover_) {
          p.fill_rounded(x() + S(4), ry + S(2), w() - S(8) - (bar_->visible() ? S(12) : 0), row_h() - S(4), S(8),
                         t.hover);
        }
        Mark m = source_ == Source::Debian ? st.marks().get(pk->name) : Mark::Unmarked;
        paint_status(p, *pk, m, c.status, ry + (row_h() - S(20)) / 2.0, S(20));
        paint_package_icon(p, *pk, app_of(*pk), c.icon, ry + (row_h() - S(28)) / 2.0, S(28));
        if (source_ == Source::Debian && pk->system()) {
          int bx = c.name + system_badge_x(*pk, c), bw = system_badge_w();
          p.fill_rounded(bx, ry + S(6), bw, S(16), S(8), t.warning.alpha(t.dark ? 0.3 : 0.18));
        }
      }
      cairo_restore(p.cr());
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, Fl::focus() == this ? t.accent : t.border);
    }
    // Header.
    text_font(std::max(S(9), t.font_size - S(1)), true);
    draw_text(_("Package"), c.name, y(), name_w(c), header_h(), t.text_dim);
    if (c.installed >= 0) draw_text(_("Installed"), c.installed, y(), S(150), header_h(), t.text_dim);
    if (c.latest >= 0) draw_text(_("Latest"), c.latest, y(), S(150), header_h(), t.text_dim);
    if (c.size >= 0) draw_text(_("Size"), c.size, y(), S(84), header_h(), t.text_dim, 2);
    fl_push_clip(x(), y() + header_h(), w(), h() - header_h());
    for (int k = 0; k < n; ++k) {
      int i = top_ + k, ry = y() + header_h() + k * row_h();
      const Package *pk = pkg(i);
      if (!pk) continue;
      int nw = name_w(c);
      bool system = source_ == Source::Debian && pk->system();
      // Packages of the base system: the name stops before the badge (its
      // width is measured first: that sets another font).
      int name_room = system ? system_badge_x(*pk, c) - S(8) : nw;
      text_font(t.font_size, true);
      draw_text(pk->label(), c.name, ry + S(4), name_room, S(20), t.text);
      if (system) {
        int bx = c.name + system_badge_x(*pk, c), bw = system_badge_w();
        text_font(std::max(S(9), t.font_size - S(3)), true);
        draw_text(_("SYSTEM"), bx, ry + S(6), bw, S(16), readable(t.warning, t.surface), 1);
      }
      text_font(std::max(S(9), t.font_size - S(1)));
      draw_text(pk->summary, c.name, ry + S(23), nw, S(18), t.text_dim);
      text_font(std::max(S(9), t.font_size - S(1)));
      if (c.installed >= 0) draw_text(pk->installed_version.empty() ? "\xE2\x80\x94" : pk->installed_version, c.installed,
                                      ry, S(150), row_h(), pk->installed ? t.text : t.text_dim);
      if (c.latest >= 0) {
        std::string v = pk->candidate_version.empty() ? std::string("\xE2\x80\x94") : pk->candidate_version;
        draw_text(v, c.latest, ry, S(150), row_h(), pk->upgradable ? t.accent_text : t.text);
      }
      if (c.size >= 0) {
        long long size = pk->installed ? pk->current_size : pk->installed_size;
        draw_text(size > 0 ? format_size(size) : std::string(), c.size, ry, S(84), row_h(), t.text_dim, 2);
      }
    }
    fl_pop_clip();
    if (count() == 0) {
      text_font(t.font_size);
      draw_text(_("No packages match."), x(), y() + header_h(), w(), S(80), t.text_dim, 1);
    }
    draw_child(*bar_);
  }

  int index_at(int ey) const {
    if (ey < y() + header_h()) return -1;
    int i = top_ + (ey - y() - header_h()) / row_h();
    return i < count() ? i : -1;
  }

  int handle(int e) override {
    if (Fl::event_inside(bar_) && (e == FL_PUSH || e == FL_DRAG || e == FL_RELEASE)) return Fl_Group::handle(e);
    switch (e) {
      case FL_FOCUS:
      case FL_UNFOCUS:
        g_state.focused = e == FL_FOCUS;
        redraw();
        return 1;
      case FL_ENTER:
      case FL_MOVE: {
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
      case FL_MOUSEWHEEL:
        top(top_ + Fl::event_dy() * 3);
        return 1;
      case FL_SCROLL_GESTURE: {  // a finger, a row at a time (docs/TOUCH.md)
        int first = top_, can = sde::touch_scroll_rows(first, count() - visible(), row_h());
        if (first != top_) top(first);
        return can;
      }
      case FL_PUSH: {
        int i = index_at(Fl::event_y());
        take_focus();
        if (i < 0) return 1;
        bool on_status = Fl::event_x() < cols().icon;
        bool debian = source_ == Source::Debian;
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          select(i, false);
          if (debian)
            mark_menu(name(i), true);
          else
            row_menu(source_, name(i));
          rebuild_page();
        } else if (on_status && debian) {
          select(i, false);
          mark_menu(name(i), false);
          rebuild_page();
        } else if (Fl::event_clicks()) {
          open_row(source_, name(i));
        } else {
          select(i);
        }
        return 1;
      }
      case FL_KEYBOARD: {
        int k = Fl::event_key(), page = visible() - 1;
        bool debian = source_ == Source::Debian;
        if (k == FL_Down) select(sel_ + 1);
        else if (k == FL_Up) select(sel_ < 0 ? 0 : sel_ - 1);
        else if (k == FL_Page_Down) select(sel_ + page);
        else if (k == FL_Page_Up) select(sel_ - page);
        else if (k == FL_Home) select(0);
        else if (k == FL_End) select(count() - 1);
        else if ((k == FL_Enter || k == FL_KP_Enter) && sel_ >= 0) open_row(source_, name(sel_));
        else if (k == FL_Menu && sel_ >= 0 && !debian) row_menu(source_, name(sel_));
        else if (!debian) return 0;  // marking is for Debian packages (APT's simulation)
        else if (k == ' ' && sel_ >= 0) toggle_mark(name(sel_));
        else if ((k == '+' || k == FL_KP + '+' || k == FL_Insert) && sel_ >= 0) {
          // Install, or update what is installed (like Synaptic's "+").
          const Package *p = Store::get().package(name(sel_));
          if (p && !p->installed) set_mark(p->name, Mark::Install);
          else if (p && p->upgradable) set_mark(p->name, Mark::Upgrade);
        } else if ((k == '-' || k == FL_KP + '-' || k == FL_Delete) && sel_ >= 0) {
          set_mark(name(sel_), Mark::Remove);
        } else if (k == FL_Menu && sel_ >= 0) {
          mark_menu(name(sel_), true);
        } else {
          return 0;
        }
        return 1;
      }
      default:
        return Fl_Group::handle(e);
    }
  }

  // The rows shown, for the QA dump.
  void qa(void (*line)(const std::string &, int, int, int, int)) const {
    for (int k = 0; k < std::min(count() - top_, visible()); ++k) {
      int i = top_ + k, ry = y() + header_h() + k * row_h();
      line("pkg:" + name(i), x(), ry, w(), row_h());
    }
  }

 private:
  // Where the SYSTEM badge of a package of the base system goes: at the
  // end of the name column, the same place in every row.
  int system_badge_x(const Package &, const Cols &c) const { return name_w(c) - system_badge_w(); }
  static int system_badge_w() {
    text_font(std::max(S(9), active_theme().font_size - S(3)), true);
    return text_width(_("SYSTEM")) + S(12);
  }
  int name_w(const Cols &c) const {
    int right = c.installed >= 0 ? c.installed : c.latest >= 0 ? c.latest : c.size >= 0 ? c.size : c.end;
    return right - c.name - S(12);
  }
  std::vector<size_t> rows_;
  Source source_;
  bool runtimes_;
  Fl_Scrollbar *bar_;
  int top_ = 0, sel_ = -1, hover_ = -1;
};

// --------------------------------------------------------- section list

class SectionList : public Fl_Widget {
 public:
  SectionList(int x, int y, int w, int h, std::vector<SectionCount> sections, size_t total, const std::string &current,
              Source source = Source::Debian)
      : Fl_Widget(x, y, w, h), sections_(std::move(sections)), total_(total), current_(current), source_(source) {
    tooltip(_("Sections"));
  }
  int row_h() const { return S(32); }
  int rows() const { return int(sections_.size()) + 1; }  // "All sections" first
  int visible() const { return std::max(1, (h() - S(8)) / row_h()); }
  void draw() override {
    const Theme &t = active_theme();
    int n = std::min(rows() - top_, visible() + 1);
    {
      Painter p(window()->w(), window()->h());
      double r = std::min(double(t.radius), Sd(12));
      p.fill_rounded(x(), y(), w(), h(), r, t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.border);
      cairo_save(p.cr());
      cairo_rectangle(p.cr(), x(), y() + S(4), w(), h() - S(8));
      cairo_clip(p.cr());
      for (int k = 0; k < n; ++k) {
        int i = top_ + k, ry = y() + S(4) + k * row_h();
        bool cur = i == 0 ? current_.empty() : sections_[size_t(i - 1)].id == current_;
        if (cur) p.fill_rounded(x() + S(4), ry, w() - S(8), row_h() - S(2), S(8), t.accent.alpha(0.15));
        else if (i == hover_) p.fill_rounded(x() + S(4), ry, w() - S(8), row_h() - S(2), S(8), t.hover);
        std::vector<std::string> names = i == 0 ? std::vector<std::string>{"view-grid", "applications-other"}
                                                : row_section_icons(source_, sections_[size_t(i - 1)].id);
        names.push_back("package-x-generic");
        cairo_surface_t *icon = nullptr;
        for (auto &nm : names)
          if (!IconLoader::lookup(nm, S(18)).empty()) {
            icon = IconLoader::get({nm}, S(18));
            break;
          }
        if (icon) p.image(icon, x() + S(12), ry + (row_h() - S(2) - S(18)) / 2.0, S(18));
        else paint_symbol(p, Symbol::Package, x() + S(12), ry + (row_h() - S(2) - S(18)) / 2.0, S(18), t.text_dim);
      }
      cairo_restore(p.cr());
    }
    fl_push_clip(x(), y() + S(4), w(), h() - S(8));
    for (int k = 0; k < n; ++k) {
      int i = top_ + k, ry = y() + S(4) + k * row_h();
      bool cur = i == 0 ? current_.empty() : sections_[size_t(i - 1)].id == current_;
      std::string title = i == 0 ? std::string(_("All Sections")) : sections_[size_t(i - 1)].title;
      size_t cnt = i == 0 ? total_ : sections_[size_t(i - 1)].count;
      std::string num = format_count(long(cnt));
      text_font(std::max(S(9), t.font_size - S(2)));
      int cw = text_width(num) + S(6);
      draw_text(num, x() + w() - S(10) - cw, ry, cw, row_h() - S(2), t.text_dim, 2);
      text_font(t.font_size - 1, cur);
      draw_text(title, x() + S(38), ry, w() - S(52) - cw, row_h() - S(2), cur ? t.accent_text : t.text);
    }
    fl_pop_clip();
  }
  int index_at(int ey) const {
    int i = top_ + (ey - y() - S(4)) / row_h();
    return ey >= y() + S(4) && i < rows() ? i : -1;
  }
  void open(int i) {
    PackageFilter f = packages_filter(current_view().arg);
    f.section = i <= 0 ? std::string() : sections_[size_t(i - 1)].id;
    go_filter(f);
  }
  int handle(int e) override {
    switch (e) {
      case FL_ENTER:
      case FL_MOVE: {
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
      case FL_MOUSEWHEEL:
        top_ = std::max(0, std::min(top_ + Fl::event_dy() * 3, rows() - visible()));
        g_top = top_;
        redraw();
        return 1;
      case FL_SCROLL_GESTURE: {  // a finger, a row at a time (docs/TOUCH.md)
        int old = top_, can = sde::touch_scroll_rows(top_, rows() - visible(), row_h());
        if (top_ != old) {
          g_top = top_;
          redraw();
        }
        return can;
      }
      case FL_PUSH: {
        int i = index_at(Fl::event_y());
        if (i >= 0) open(i);
        return 1;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }
  // Keeps the list where it was across rebuilds.
  static int g_top;
  void restore() { top_ = std::max(0, std::min(g_top, rows() - visible())); }
  void qa(void (*line)(const std::string &, int, int, int, int)) const {
    for (int k = 0; k < std::min(rows() - top_, visible()); ++k) {
      int i = top_ + k;
      line("section:" + (i == 0 ? std::string("all") : sections_[size_t(i - 1)].id), x(), y() + S(4) + k * row_h(), w(),
           row_h());
    }
  }

 private:
  std::vector<SectionCount> sections_;
  size_t total_;
  std::string current_;
  Source source_;
  int top_ = 0, hover_ = -1;
};
int SectionList::g_top = 0;

void qa_hook(const std::string &name, int x, int y, int w, int h) { qa_line(name, x, y, w, h); }

// The selected Flatpak app, runtime or snap: what it is, where it comes
// from, and Install, Update, Remove at once.
void row_info_pane(int x, int y, int w, int h, const Package &pk) {
  Store &st = Store::get();
  struct Icon : Fl_Widget {
    Source source;
    std::string name;
    Icon(int x, int y, int s, Source src, const std::string &n) : Fl_Widget(x, y, s, s), source(src), name(n) {}
    void draw() override {
      const Package *p = Store::get().row(source, name);
      if (!p) return;
      Painter pt(window()->w(), window()->h());
      paint_package_icon(pt, *p, p->app_id.empty() ? nullptr : Store::get().app(p->app_id), x(), y(), w());
    }
  };
  int pad = S(18), is = S(48);
  new Icon(x + pad, y + pad, is, pk.source, pk.name);
  int tx = x + pad + is + S(16), tw = w - (tx - x) - pad;
  new Label(tx, y + pad - S(4), tw, S(26), pk.label() + "  (" + (pk.runtime ? pk.ref : pk.app_id) + ")", Label::BOLD);
  std::string line = state_text(pk, Mark::Unmarked);
  std::string version = pk.installed ? pk.installed_version : pk.candidate_version;
  if (pk.installed && pk.upgradable && !pk.candidate_version.empty())
    line += "  \xC2\xB7  " + format(_("%s, update to %s"), pk.installed_version.c_str(), pk.candidate_version.c_str());
  else if (!version.empty())
    line += "  \xC2\xB7  " + format(_("version %s"), version.c_str());
  line += "  \xC2\xB7  " + source_label(pk.source) + (pk.origin.empty() ? "" : " (" + pk.origin + ")");
  long long size = pk.installed ? pk.current_size : pk.download_size > 0 ? pk.download_size : pk.installed_size;
  if (size > 0) line += "  \xC2\xB7  " + format_size(size);
  new Label(tx, y + pad + S(22), tw, S(20), line, Label::DIM);
  int cy = y + pad + S(44);
  if (!pk.summary.empty()) {
    new Label(tx, cy, tw, S(22), pk.summary);
    cy += S(26);
  }
  int bx = tx, by = std::min(cy + S(4), y + h - pad - S(34));
  if (!pk.app_id.empty() && st.active_job(pk.app_id)) {
    Button *b = button(bx, by, _("In Progress"), Button::Normal, [] { navigate(View{View::Progress, ""}); });
    bx += b->w() + S(8);
  } else {
    for (auto &a : row_actions(pk)) {
      Button::Kind kind = a.first == _("Remove") ? Button::Danger : Button::Primary;
      Button *b = button(bx, by, a.first, kind, a.second);
      qa_line("action:row:" + a.first, b->x(), b->y(), b->w(), b->h());
      bx += b->w() + S(8);
    }
  }
  if (!pk.app_id.empty()) {
    std::string id = pk.app_id;
    Button *b = button(bx, by, _("App Page"), Button::Ghost, [id] { navigate(View{View::Details, id}); });
    qa_line("action:row:App Page", b->x(), b->y(), b->w(), b->h());
  }
}

// The details of the selected package below (or beside) the list.
void info_pane(int x, int y, int w, int h, const Package *pk, Source source = Source::Debian) {
  const Theme &t = active_theme();
  new Card(x, y, w, h);
  if (pk && pk->source != Source::Debian) {
    row_info_pane(x, y, w, h, *pk);
    return;
  }
  if (!pk) {
    new Label(x + S(20), y, w - S(40), h,
              source == Source::Debian ? _("Select a package to see what it is and to mark a change.")
                                       : _("Select an app to see where it comes from and to install, update or "
                                           "remove it."),
              Label::DIM);
    return;
  }
  Store &st = Store::get();
  const App *app = st.app_of_package(pk->name);
  Mark m = st.marks().get(pk->name);
  struct Icon : Fl_Widget {
    std::string name;
    Icon(int x, int y, int s, const std::string &n) : Fl_Widget(x, y, s, s), name(n) {}
    void draw() override {
      const Package *p = Store::get().package(name);
      if (!p) return;
      Painter pt(window()->w(), window()->h());
      paint_package_icon(pt, *p, Store::get().app_of_package(name), x(), y(), w());
    }
  };
  int pad = S(18), is = S(48);
  new Icon(x + pad, y + pad, is, pk->name);
  int tx = x + pad + is + S(16), tw = w - (tx - x) - pad;
  new Label(tx, y + pad - S(4), tw, S(26), app ? app->name + "  (" + pk->name + ")" : pk->name, Label::BOLD);
  std::string line = state_text(*pk, m);
  if (!pk->candidate_version.empty()) {
    if (pk->installed && pk->upgradable)
      line += "  \xC2\xB7  " + format(_("%s, update to %s"), pk->installed_version.c_str(), pk->candidate_version.c_str());
    else
      line += "  \xC2\xB7  " + format(_("version %s"), (pk->installed ? pk->installed_version : pk->candidate_version).c_str());
  }
  line += "  \xC2\xB7  " + section_title(section_base(pk->section));
  if (!pk->origin.empty()) line += "  \xC2\xB7  " + pk->origin;
  new Label(tx, y + pad + S(22), tw, S(20), line, Label::DIM);
  int cy = y + pad + S(44);
  if (!pk->summary.empty()) {
    new Label(tx, cy, tw, S(22), pk->summary);
    cy += S(26);
  }
  std::string block = pk->installed ? removal_block(*pk) : "";
  // The buttons.
  int bx = tx, by = std::min(cy + S(4), y + h - pad - S(34));
  for (auto &c : mark_choices(*pk, m)) {
    if (c.first == Mark::Remove && !block.empty()) continue;
    Mark mk = c.first;
    std::string n = pk->name;
    Button::Kind kind = mk == Mark::Unmarked ? Button::Normal : mk == Mark::Remove ? Button::Danger : Button::Primary;
    Button *b = button(bx, by, c.second, kind, [n, mk] { set_mark(n, mk); });
    qa_line("action:mark:" + c.second, b->x(), b->y(), b->w(), b->h());
    bx += b->w() + S(8);
  }
  std::string n = pk->name;
  Button *d = button(bx, by, _("Details"), Button::Normal, [n] { navigate(View{View::Package, n}); });
  qa_line("action:package-details", d->x(), d->y(), d->w(), d->h());
  bx += d->w() + S(8);
  if (app) {
    std::string id = app->id;
    Button *a = button(bx, by, _("App Page"), Button::Ghost, [id] { navigate(View{View::Details, id}); });
    bx += a->w() + S(8);
  }
  if (!block.empty() && bx < x + w - S(120)) new Label(bx + S(4), by, x + w - pad - bx - S(4), S(34), block, Label::DIM);
  if (!block.empty()) qa_line("protected:" + pk->name, bx, by, S(1), S(1));
  (void)t;
}

// The bar of the marked changes.
void marks_bar(int x, int y, int w, int h) {
  Store &st = Store::get();
  const ChangeSet &m = st.marks();
  std::vector<std::string> parts;
  if (m.count(Mark::Install)) parts.push_back(format(_("%d to install"), int(m.count(Mark::Install))));
  if (m.count(Mark::Upgrade)) parts.push_back(format(_("%d to update"), int(m.count(Mark::Upgrade))));
  if (m.count(Mark::Remove)) parts.push_back(format(_("%d to remove"), int(m.count(Mark::Remove))));
  std::string what;
  for (size_t i = 0; i < parts.size(); ++i) what += (i ? ", " : "") + parts[i];
  struct Bar : Fl_Widget {
    Bar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
    void draw() override {
      const Theme &t = active_theme();
      Painter p(window()->w(), window()->h());
      double r = std::min(double(t.radius), Sd(12));
      p.fill_rounded(x(), y(), w(), h(), r, t.accent.alpha(t.dark ? 0.22 : 0.12).mix(t.surface, 0.2));
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, t.accent.alpha(0.5));
    }
  };
  new Bar(x, y, w, h);
  std::string text = m.size() == 1 ? format(_("1 change marked: %s"), what.c_str())
                                   : format(_("%d changes marked: %s"), int(m.size()), what.c_str());
  int bw2 = Button::fit(_("Review and Apply\xE2\x80\xA6")), bw1 = Button::fit(_("Unmark All"));
  new Label(x + S(18), y, w - bw1 - bw2 - S(60), h, text, Label::BOLD);
  Button *clear = button(x + w - S(14) - bw2 - S(10) - bw1, y + (h - S(34)) / 2, _("Unmark All"), Button::Normal,
                         [] { Store::get().clear_marks(); });
  Button *apply = button(x + w - S(14) - bw2, y + (h - S(34)) / 2, _("Review and Apply\xE2\x80\xA6"), Button::Primary,
                         [] { navigate(View{View::Changes, ""}); });
  qa_line("action:unmark-all", clear->x(), clear->y(), clear->w(), clear->h());
  qa_line("action:review", apply->x(), apply->y(), apply->w(), apply->h());
}

}  // namespace

// ------------------------------------------------------------- the page

namespace {

// The sections of the rows of |f|'s source with their names.
std::vector<SectionCount> row_sections(const PackageFilter &f) {
  Store &st = Store::get();
  std::vector<SectionCount> out = section_counts(st.rows(f.source, f.runtimes), f.status, st.marks());
  if (f.source != Source::Debian) {
    for (auto &sc : out) sc.title = row_section_title(f.source, sc.id);
    std::sort(out.begin(), out.end(), [](const SectionCount &a, const SectionCount &b) {
      // The runtimes and what has no department last.
      int ra = a.id == "runtimes" ? 2 : a.id.empty() ? 1 : 0, rb = b.id == "runtimes" ? 2 : b.id.empty() ? 1 : 0;
      if (ra != rb) return ra < rb;
      return lower(a.title) < lower(b.title);
    });
  }
  return out;
}

// A thin line between two groups of chips.
struct Divider : Fl_Widget {
  Divider(int x, int y, int h) : Fl_Widget(x, y, S(1) + 1, h) {}
  void draw() override {
    Painter p(window()->w(), window()->h());
    p.fill_rect(x(), y(), S(1), h(), active_theme().border);
  }
};

// The switch between the sources at the top of All Software, with how
// much each has: Debian packages, Flatpak, Snap. Returns where it ends.
int source_switch(int x, int y, int w, const PackageFilter &f) {
  Store &st = Store::get();
  int h = S(32), cx = x;
  for (Source s : {Source::Debian, Source::Flatpak, Source::Snap}) {
    std::string count;
    if (s == Source::Debian)
      count = st.packages_loaded() ? format_count(long(st.packages().size())) : std::string("\xE2\x80\xA6");
    else
      count = st.catalog_loaded() ? format_count(long(st.rows(s).size())) : std::string("\xE2\x80\xA6");
    std::string text = source_switch_label(s) + "  " + count;
    int cw = Chip::fit(text) + S(10);
    if (cx + cw > x + w && cx > x) break;
    auto *chip = new Chip(cx, y, cw, h, text, f.source == s);
    chip->dot = source_color(s);
    PackageFilter g;
    g.source = s;
    g.query = f.query;
    chip->on_click = [g] { go_filter(g); };
    qa_line(std::string("chip:source:") + source_id(s), cx, y, cw, h);
    cx += cw + S(8);
  }
  return cx - S(8);
}

// Why a source has no rows, and what fixes it; false when it has some.
// |before| runs first when there is a problem to show.
bool source_problem(Page *page, const PackageFilter &f, const std::function<void()> &before) {
  Store &st = Store::get();
  int x = page->left(), w = page->width();
  std::string title, text, action;
  std::function<void()> fix;
  if (f.source == Source::Flatpak) {
    const SourceStatus &fp = st.status(Source::Flatpak);
    if (st.probed(Source::Flatpak) && !fp.installed && !fake_mode()) {
      title = _("Flatpak is not installed");
      text = _("Flatpak apps come from Flathub and other remotes and run isolated from the system.");
      action = _("Install Flatpak");
      fix = install_flatpak_support;
    } else if (st.probed(Source::Flatpak) && fp.available && fp.remotes.empty() && st.settings().setup_flathub) {
      title = _("No Flatpak remote is set up");
      text = _("Flathub has thousands of apps. Set it up, then its catalog is downloaded.");
      action = _("Set Up Flathub");
      fix = set_up_flathub;
    } else if (st.catalog_loaded() && st.rows(Source::Flatpak, f.runtimes).empty()) {
      title = _("No Flatpak apps yet");
      text = _("The catalogs of the Flatpak remotes have not been downloaded: refresh the software lists (this "
               "needs a network connection).");
      action = _("Refresh Software Lists");
      fix = [] { Store::get().refresh(true); };
    }
  } else if (f.source == Source::Snap) {
    const SourceStatus &sn = st.status(Source::Snap);
    if (st.probed(Source::Snap) && !sn.available && !fake_source(Source::Snap)) {
      title = sn.installed ? _("snapd is not running") : _("Snap support is not enabled");
      text = sn.installed ? _("snapd was installed but does not answer yet. Restart the computer (or log out and back "
                              "in) if it does not start by itself.")
                          : _("Snaps come from the Snap Store through snapd, which Merkadu installs with your "
                              "password.");
      if (!sn.installed) {
        action = _("Enable Snap Support");
        fix = enable_snap_support;
      }
    }
  }
  if (title.empty()) return false;
  before();
  auto *es = new EmptyState(x, page->cursor(), w, f.source == Source::Snap ? Symbol::Snap : Symbol::Package, title,
                            text, false);
  qa_line(std::string("empty:source:") + source_id(f.source), es->x(), es->y(), es->w(), es->h());
  page->advance(es->h() + S(16));
  if (fix) {
    Button *b = button(x + (w - Button::fit(action)) / 2, page->cursor(), action, Button::Primary, fix);
    qa_line("action:source-fix:" + action, b->x(), b->y(), b->w(), b->h());
    page->advance(S(34) + S(16));
  }
  return true;
}

}  // namespace

void build_packages_page(Page *page, const std::string &arg) {
  Store &st = Store::get();
  page->max_width(1 << 20);  // the list takes the whole width
  PackageFilter f = packages_filter(arg);
  int x = page->left(), w = page->width();
  // Debian packages, Flatpak or Snap: each source its own list. The status
  // filters follow on the same line when there is room.
  int top = page->cursor() - S(8);
  int switch_end = source_switch(x, top, w, f);
  bool one_line = w >= S(820);
  page->advance(one_line ? 0 : S(32) + S(8));
  // Pages without the list start below the switch.
  bool below = false;
  auto below_switch = [&] {
    if (one_line && !below) page->advance(S(32) + S(8));
    below = true;
  };
  if (f.source == Source::Debian) {
    if (!st.packages_available() || !st.packages_loaded() || st.packages().empty()) below_switch();
    if (!st.packages_available()) {
      page->note(_("This build of Merkadu cannot read the package lists (libapt-pkg)."));
      page->finish();
      return;
    }
    if (!st.packages_loaded() || (st.packages().empty() && st.packages_loading())) {
      if (!st.packages_loading()) st.load_packages();
      int y = page->cursor();
      for (int i = 0; i < 6; ++i) new Placeholder(x, y + i * S(56), w, S(48));
      new Label(x, y + 6 * S(56) + S(8), w, S(24), _("Reading the package lists\xE2\x80\xA6"), Label::DIM);
      qa_line("loading", x, y, w, S(48));
      page->advance(6 * S(56) + S(40));
      page->finish();
      return;
    }
    if (st.packages().empty()) {
      auto *es = new EmptyState(x, page->cursor(), w, Symbol::Warning, _("The package lists could not be read"),
                                st.packages_error(), true);
      page->advance(es->h() + S(16));
      button(x + (w - Button::fit(_("Refresh Software Lists"))) / 2, page->cursor(), _("Refresh Software Lists"),
             Button::Primary, [] { Store::get().refresh(true); });
      page->advance(S(34) + S(16));
      page->finish();
      return;
    }
  } else {
    if (f.runtimes && !st.runtimes_loaded()) st.load_runtimes();
    if (!st.catalog_loaded()) {
      below_switch();
      int y = page->cursor();
      for (int i = 0; i < 6; ++i) new Placeholder(x, y + i * S(56), w, S(48));
      qa_line("loading", x, y, w, S(48));
      page->advance(6 * S(56) + S(16));
      page->finish();
      return;
    }
    if (source_problem(page, f, below_switch)) {
      page->finish();
      return;
    }
  }
  const std::vector<Package> &rows = st.rows(f.source, f.runtimes);
  const ListCache &list = filtered(arg);
  if (g_state.arg != arg) {
    // Another filter: the list starts at the top with nothing selected,
    // unless the selection is in it.
    g_state.top = 0;
    if (std::none_of(list.rows.begin(), list.rows.end(),
                     [&rows](size_t i) { return i < rows.size() && rows[i].name == g_state.selected; }))
      g_state.selected.clear();
    g_state.arg = arg;
  }
  bool wide = w >= S(820), debian = f.source == Source::Debian;
  // The status filters (and the sections on narrow windows).
  int cx = x, cy = page->cursor() - (one_line ? S(8) : 0), chip_h = S(32);
  if (one_line) {
    new Divider(switch_end + S(12), cy + S(4), chip_h - S(8));
    cx = switch_end + S(26);
  }
  if (!wide) {
    std::string label = f.section.empty() ? std::string(_("All Sections")) : row_section_title(f.source, f.section);
    int bw = Chip::fit(label) + S(18);
    auto *c = new Chip(cx, cy, bw, chip_h, label, !f.section.empty());
    c->menu = true;
    c->on_click = [f] {
      ContextMenu menu;
      PackageFilter all = f;
      all.section.clear();
      menu.radio(_("All Sections"), f.section.empty(), [all] { go_filter(all); });
      for (auto &sc : row_sections(f)) {
        PackageFilter g = f;
        g.section = sc.id;
        menu.radio(format("%s (%s)", sc.title.c_str(), format_count(long(sc.count)).c_str()), sc.id == f.section,
                   [g] { go_filter(g); });
      }
      menu.popup();
    };
    qa_line("chip:section", cx, cy, bw, chip_h);
    cx += bw + S(14);
  }
  for (int s = 0; s < kPackageStatusCount; ++s) {
    PackageStatus ps = PackageStatus(s);
    size_t n = list.counts[s];
    // Marks and left settings are Debian's.
    if (!debian && (ps == PackageStatus::Marked || ps == PackageStatus::ConfigOnly)) continue;
    if (ps == PackageStatus::ConfigOnly && !n) continue;
    if (ps == PackageStatus::Marked && !n && f.status != ps) continue;
    std::string text = status_title(ps) + "  " + format_count(long(n));
    int cw = Chip::fit(text);
    if (cx + cw > x + w && cx > x) {
      cx = x;
      cy += chip_h + S(8);
    }
    auto *chip = new Chip(cx, cy, cw, chip_h, text, f.status == ps);
    PackageFilter g = f;
    g.status = ps;
    chip->on_click = [g] { go_filter(g); };
    qa_line(std::string("chip:status:") + package_status_id(ps), cx, cy, cw, chip_h);
    cx += cw + S(8);
  }
  if (f.source == Source::Flatpak) {
    // Runtimes and extensions come with the apps: shown on request.
    std::string text = _("Runtimes and Extensions");
    int cw = Chip::fit(text);
    if (cx + S(10) + cw > x + w && cx > x) {
      cx = x;
      cy += chip_h + S(8);
    } else {
      cx += S(10);
    }
    auto *chip = new Chip(cx, cy, cw, chip_h, text, f.runtimes);
    PackageFilter g = f;
    g.runtimes = !f.runtimes;
    if (!g.runtimes && g.section == "runtimes") g.section.clear();
    chip->on_click = [g] { go_filter(g); };
    qa_line("chip:runtimes", cx, cy, cw, chip_h);
  }
  int body_y = cy + chip_h + S(14);
  if (f.source == Source::Snap) {
    // snapd has no list of the whole store.
    std::string note = _("The Snap Store has no complete list: these are the installed snaps and those the store "
                         "showed (featured, departments and searches). Search to find more.");
    int nh = TextBlock::measure(note, w, -1) + S(4);
    new TextBlock(x, body_y - S(6), w, nh, note, true, -1);
    body_y += nh + S(6);
  }
  bool marks = debian && !st.marks().empty();
  int bar_h = marks ? S(56) : 0;
  int info_h = S(148);
  int bottom = page->y() + page->h() - S(16);
  // At least four rows: on short screens the page scrolls to the details.
  int list_h = std::max(S(224), bottom - body_y - info_h - S(12) - (marks ? bar_h + S(12) : 0));
  int lx = x, lw = w;
  if (wide) {
    int sw = S(220);
    std::vector<SectionCount> sections = row_sections(f);
    size_t total = 0;
    for (auto &sc : sections) total += sc.count;
    auto *sl = new SectionList(x, body_y, sw, list_h + S(12) + info_h, sections, total, f.section, f.source);
    sl->restore();
    sl->qa(qa_hook);
    lx = x + sw + S(14);
    lw = w - sw - S(14);
  }
  auto *table = new PackageTable(lx, body_y, lw, list_h, list.rows, f.source, f.runtimes);
  table->qa(qa_hook);
  qa_line("table", table->x(), table->y(), table->w(), table->h());
  const Package *sel = g_state.selected.empty() ? nullptr : st.row(f.source, g_state.selected);
  info_pane(lx, body_y + list_h + S(12), lw, info_h, sel, f.source);
  if (marks) marks_bar(x, body_y + list_h + S(12) + info_h + S(12), w, bar_h);
  if (g_state.focused) table->take_focus();
  page->advance(bottom - page->cursor() - S(24));
  page->end();
}

// ------------------------------------------------------- one package

namespace {

std::map<std::string, apt::Details> g_details;
std::set<std::string> g_details_asked;
int g_details_generation = -1;

void dep_group(Page *page, const std::string &title, const std::vector<std::string> &items, size_t max = 60) {
  if (items.empty()) return;
  page->begin_card(title);
  std::string text;
  for (size_t i = 0; i < items.size() && i < max; ++i) text += (i ? "\n" : "") + items[i];
  if (items.size() > max)
    text += "\n" + format(ngettext("and %d more", "and %d more", int(items.size() - max)), int(items.size() - max));
  int h = TextBlock::measure(text, page->width() - S(40), 0) + S(4);
  new TextBlock(page->left() + S(20), page->cursor(), page->width() - S(40), h, text);
  page->advance(h + S(6));
  page->end_card();
}

}  // namespace

void build_package_page(Page *page, const std::string &name) {
  Store &st = Store::get();
  if (!st.packages_loaded()) {
    if (!st.packages_loading()) st.load_packages();
    page->note(_("Reading the package lists\xE2\x80\xA6"));
    page->finish();
    return;
  }
  const Package *pk = st.package(name);
  if (!pk) {
    auto *es = new EmptyState(page->left(), page->cursor(), page->width(), Symbol::Search,
                              format(_("There is no package %s"), name.c_str()),
                              _("It is not in the package lists of the configured repositories."), false);
    page->advance(es->h() + S(12));
    page->finish();
    return;
  }
  if (g_details_generation != st.packages_generation()) {
    g_details.clear();
    g_details_asked.clear();
    g_details_generation = st.packages_generation();
  }
  if (!g_details.count(name) && g_details_asked.insert(name).second) {
    std::string n = name;
    st.package_details(n, [n](const apt::Details &d) {
      g_details[n] = d;
      const View &v = current_view();
      if (v.kind == View::Package && v.arg == n) rebuild_page();
    });
  }
  const App *app = st.app_of_package(name);
  Mark m = st.marks().get(name);
  // Header.
  int x = page->left(), w = page->width(), y = page->cursor(), pad = S(24), is = S(80);
  int tx = x + pad + is + S(22), tw = w - (tx - x) - pad;
  auto *card = new Card(x, y, w, S(100));
  struct Icon : Fl_Widget {
    std::string name;
    Icon(int x, int y, int s, const std::string &n) : Fl_Widget(x, y, s, s), name(n) {}
    void draw() override {
      const Package *p = Store::get().package(name);
      if (!p) return;
      Painter pt(window()->w(), window()->h());
      paint_package_icon(pt, *p, Store::get().app_of_package(name), x(), y(), w());
    }
  };
  new Icon(x + pad, y + pad, is, name);
  int cy = y + pad - S(4);
  new Label(tx, cy, tw, S(36), name, Label::TITLE);
  cy += S(38);
  new Label(tx, cy, tw, S(22), pk->summary, Label::SUBTITLE);
  cy += S(28);
  new Label(tx, cy, tw, S(20), state_text(*pk, m), Label::DIM);
  cy += S(30);
  std::string block = pk->installed ? removal_block(*pk) : "";
  int bx = tx;
  for (auto &c : mark_choices(*pk, m)) {
    if (c.first == Mark::Remove && !block.empty()) continue;
    Mark mk = c.first;
    std::string n = name;
    Button::Kind kind = mk == Mark::Unmarked ? Button::Normal : mk == Mark::Remove ? Button::Danger : Button::Primary;
    Button *b = button(bx, cy, c.second, kind, [n, mk] { set_mark(n, mk); });
    qa_line("action:mark:" + c.second, b->x(), b->y(), b->w(), b->h());
    bx += b->w() + S(10);
  }
  if (app) {
    std::string id = app->id;
    button(bx, cy, _("App Page"), Button::Normal, [id] { navigate(View{View::Details, id}); });
  }
  cy += S(34) + S(12);
  if (!block.empty()) {
    int h = TextBlock::measure(block, tw, 0) + S(4);
    new TextBlock(tx, cy - S(4), tw, h, block, true);
    qa_line("protected:" + name, tx, cy - S(4), tw, h);
    cy += h + S(6);
  }
  if (!st.marks().empty()) {
    Button *b = button(tx, cy, format(_("Review %d Marked Changes"), int(st.marks().size())), Button::Ghost,
                       [] { navigate(View{View::Changes, ""}); });
    qa_line("action:review", b->x(), b->y(), b->w(), b->h());
    cy += S(34) + S(8);
  }
  int bottom = std::max(cy, y + pad + is + pad);
  card->size(w, bottom - y);
  page->advance(bottom - y + S(20));
  // Facts.
  auto it = g_details.find(name);
  const apt::Details *d = it == g_details.end() ? nullptr : &it->second;
  auto row = [&](const std::string &label, const std::string &value) {
    if (value.empty()) return;
    int lx = page->left() + S(20), lw = std::min(S(170), (page->width() - S(40)) / 3);
    int vw = page->width() - S(40) - lw;
    int vh = std::max(S(24), TextBlock::measure(value, vw, 0) + S(2));
    new Label(lx, page->cursor(), lw, S(24), label, Label::DIM);
    new TextBlock(lx + lw, page->cursor() + S(3), vw, vh, value);
    qa_line("info:" + label, lx, page->cursor(), vw + lw, vh);
    page->advance(vh + S(6));
  };
  page->begin_card(_("Package"));
  row(_("Installed version"), pk->installed_version.empty() ? std::string(_("Not installed")) : pk->installed_version);
  row(_("Available version"), pk->candidate_version.empty() ? std::string(_("None")) : pk->candidate_version);
  row(_("Section"), section_title(section_base(pk->section)) + "  (" + pk->section + ")");
  static const char *priorities[] = {"", N_("required"), N_("important"), N_("standard"), N_("optional"), N_("extra")};
  if (pk->priority > 0 && pk->priority <= 5)
    row(_("Priority"), std::string(_(priorities[pk->priority])) + (pk->essential ? std::string(", ") + _("essential") : ""));
  row(_("Source"), pk->origin);
  if (pk->download_size > 0) row(_("Download size"), format_size(pk->download_size));
  long long size = pk->installed ? pk->current_size : pk->installed_size;
  if (size > 0) row(_("Installed size"), format_size(size));
  if (d) {
    row(_("Maintainer"), d->maintainer);
    row(_("Source package"), d->source_package);
    row(_("Homepage"), d->homepage);
  }
  page->end_card();
  if (!d) {
    page->note(_("Reading the details\xE2\x80\xA6"));
    page->finish();
    return;
  }
  if (!d->description.empty()) {
    new Label(page->left() + S(2), page->cursor(), page->width(), S(30), _("Description"), Label::TITLE);
    page->advance(S(36));
    int h = TextBlock::measure(d->description, page->width(), 0) + S(4);
    new TextBlock(page->left(), page->cursor(), page->width(), h, d->description);
    page->advance(h + S(18));
  }
  // Dependencies by kind, like Synaptic's Dependencies tab.
  std::map<std::string, std::vector<std::string>> by_type;
  for (auto &dep : d->dependencies) by_type[dep.type].push_back(dep.text);
  struct Kind {
    const char *type, *title;
  };
  const Kind kinds[] = {{"Pre-Depends", N_("Needs before installation")}, {"Depends", N_("Depends on")},
                        {"Recommends", N_("Recommends")},                 {"Suggests", N_("Suggests")},
                        {"Conflicts", N_("Conflicts with")},              {"Breaks", N_("Breaks")},
                        {"Replaces", N_("Replaces")},                     {"Enhances", N_("Enhances")}};
  for (auto &k : kinds) dep_group(page, _(k.title), by_type[k.type]);
  dep_group(page, _("Needed by these installed packages"), d->required_by);
  dep_group(page, _("Provides"), d->provides);
  // Versions and where they come from.
  if (!d->versions.empty()) {
    std::vector<std::string> lines;
    for (auto &v : d->versions)
      lines.push_back(v.version + "  \xC2\xB7  " + v.origin + "  \xC2\xB7  " + format(_("priority %d"), v.pin) +
                      (v.installed ? "  \xC2\xB7  " + std::string(_("installed")) : "") +
                      (v.candidate ? "  \xC2\xB7  " + std::string(_("candidate")) : ""));
    dep_group(page, _("Versions"), lines);
  }
  page->finish();
}

// ------------------------------------------------------- the changes

namespace {

ChangePlan g_plan;
std::string g_plan_marks = "-";
bool g_planning = false;

std::vector<std::string> labelled(const std::vector<std::string> &names, const std::set<std::string> &marked) {
  std::vector<std::string> out;
  for (auto &n : names) {
    const App *a = Store::get().app_of_package(n);
    std::string line = n;
    if (a) line += "  (" + a->name + ")";
    if (!marked.count(n)) line += "  \xC2\xB7  " + std::string(_("with the others"));
    out.push_back(line);
  }
  return out;
}

}  // namespace

void build_changes_page(Page *page) {
  Store &st = Store::get();
  page->heading(_("Review Changes"), _("Everything the marked changes do, with what comes with them"));
  if (st.marks().empty()) {
    auto *es = new EmptyState(page->left(), page->cursor(), page->width(), Symbol::Check, _("No changes are marked"),
                              _("Mark packages in All Software: they are installed, updated and removed together."),
                              false);
    page->advance(es->h() + S(16));
    button(page->left() + (page->width() - Button::fit(_("All Software"))) / 2, page->cursor(), _("All Software"),
           Button::Primary, [] { navigate(View{View::Packages, ""}); });
    page->advance(S(34) + S(16));
    page->finish();
    return;
  }
  std::string sig = marks_signature(st.marks());
  if (sig != g_plan_marks && !g_planning) {
    g_planning = true;
    st.plan_marks([sig](const ChangePlan &plan) {
      g_planning = false;
      g_plan = plan;
      g_plan_marks = sig;
      if (current_view().kind == View::Changes) rebuild_page();
    });
  }
  if (sig != g_plan_marks) {
    new Placeholder(page->left(), page->cursor(), page->width(), S(120));
    page->advance(S(132));
    page->note(_("Working out what the changes need\xE2\x80\xA6"));
    page->finish();
    return;
  }
  const ChangePlan &plan = g_plan;
  auto refused = refused_removals(plan, st.system_packages(), st.settings().protected_packages);
  bool can_apply = plan.valid && plan.error.empty() && refused.empty() && plan.changes() > 0;
  // The plan for the QA dump (tests/test_merkadu_ui.py).
  for (auto *group : {&plan.install, &plan.upgrade, &plan.downgrade, &plan.remove})
    for (auto &n : *group)
      qa_line(std::string("plan:") +
                  (group == &plan.install ? "install:" : group == &plan.upgrade ? "update:"
                                                       : group == &plan.downgrade ? "downgrade:"
                                                                                  : "remove:") +
                  n,
              page->left(), page->cursor(), 1, 1);
  for (auto &r : refused) qa_line("plan:refused:" + r.first, page->left(), page->cursor(), 1, 1);
  // The summary.
  std::vector<std::string> facts;
  std::string counts = plan_counts(plan);
  if (!counts.empty()) facts.push_back(counts);
  if (plan.download > 0) facts.push_back(format(_("%s to download"), format_size(plan.download).c_str()));
  if (plan.disk > 0) facts.push_back(format(_("%s of disk space used"), format_size(plan.disk).c_str()));
  if (plan.disk < 0) facts.push_back(format(_("%s of disk space freed"), format_size(-plan.disk).c_str()));
  std::string summary;
  for (size_t i = 0; i < facts.size(); ++i) summary += (i ? "  \xC2\xB7  " : "") + facts[i];
  page->begin_card(_("Summary"));
  page->note(summary.empty() ? std::string(_("Nothing changes.")) : summary);
  if (!plan.error.empty()) page->note(format(_("These changes cannot be made: %s"), plan.error.c_str()));
  for (auto &r : refused) {
    RemovalPlan rp;
    rp.protected_package = r.first;
    rp.system = r.second;
    std::string marked;
    for (auto &n : plan.marked)
      if (st.marks().get(n) == Mark::Remove) marked = n;
    page->note(refusal_text(rp, marked.empty() ? r.first : marked, marked.empty() ? r.first : marked));
  }
  page->advance(S(6));
  int bx = page->left() + S(20);
  Button *apply = button(bx, page->cursor(), _("Apply Changes"), Button::Primary, [] {
    int n = Store::get().apply_marks();
    if (n) navigate(View{View::Progress, ""});
  });
  if (!can_apply) apply->deactivate();
  qa_line("action:apply", apply->x(), apply->y(), apply->w(), apply->h());
  bx += apply->w() + S(10);
  Button *back = button(bx, page->cursor(), _("Back to the List"), Button::Normal, [] { go_back(); });
  bx += back->w() + S(10);
  button(bx, page->cursor(), _("Unmark All"), Button::Normal, [] {
    Store::get().clear_marks();
    go_back();
  });
  page->advance(S(34) + S(10));
  page->end_card();
  dep_group(page, format(_("To remove (%d)"), int(plan.remove.size())), labelled(plan.remove, plan.marked), 200);
  dep_group(page, format(_("To install (%d)"), int(plan.install.size())), labelled(plan.install, plan.marked), 200);
  dep_group(page, format(_("To update (%d)"), int(plan.upgrade.size())), labelled(plan.upgrade, plan.marked), 200);
  dep_group(page, format(_("To downgrade (%d)"), int(plan.downgrade.size())), labelled(plan.downgrade, plan.marked),
            200);
  page->note(_("The changes are made with PackageKit, removals first; the administrator password is asked for. "
               "Removing a package also removes the packages that need it, as listed above."));
  page->finish();
}

}  // namespace merkadu
}  // namespace sde
