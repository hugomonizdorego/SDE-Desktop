// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Preferences page, shown inside the window like Notepad's: rows grouped
// under headings (Appearance, Text formatting, View, Opening files,
// Spelling and the web, Editing, Files and recovery) and an About section.
// The same page lists the keyboard shortcuts (Help > Keyboard Shortcuts).
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

#include "about.h"
#include "config.h"
#include "effects.h"
#include "i18n.h"
#include "niny.h"
#include "process.h"
#include "textops.h"
#include "util.h"

using namespace sde;

namespace niny {

const std::vector<std::string> &font_families() {
  static std::vector<std::string> cache;
  if (!cache.empty()) return cache;
  std::set<std::string> names;
  for (const char *spacing : {":spacing=100", ":spacing=90"}) {
    int status = 0;
    std::string out = run_sync({"fc-list", spacing, "family"}, &status, 3000);
    for (auto &line : split(out, '\n')) {
      std::string f = trim(line.substr(0, line.find(',')));
      if (!f.empty()) names.insert(f);
    }
  }
  cache.push_back("Monospace");
  for (auto &n : names)
    if (n != "Monospace") cache.push_back(n);
  cache.push_back("Sans");
  cache.push_back("Serif");
  return cache;
}

namespace {

// The keys of a shortcut row as keycaps ending at |right|: a box for each
// key, a slash between alternatives ("Ctrl+Y, Ctrl+Shift+Z").
struct Keycap {
  std::string text;
  double x = 0, w = 0;
  bool slash = false;
};
std::vector<Keycap> keycaps(const std::string &keys, double right) {
  std::vector<std::string> alternatives;
  size_t from = 0;
  for (size_t at; (at = keys.find(", ", from)) != std::string::npos; from = at + 2)
    alternatives.push_back(keys.substr(from, at - from));
  alternatives.push_back(keys.substr(from));
  text_font(look().theme.font_size - S(1));
  std::vector<Keycap> caps;
  double x = 0;
  for (size_t a = 0; a < alternatives.size(); ++a) {
    if (a) {
      Keycap s;
      s.text = "/";
      s.slash = true;
      s.x = x + S(2);
      s.w = text_width("/") + S(8);
      caps.push_back(s);
      x += s.w + S(6);
    }
    for (const std::string &k : shortcut_keys(alternatives[a])) {
      Keycap c;
      c.text = k;
      c.x = x;
      c.w = std::max<double>(S(28), text_width(k) + S(16));
      caps.push_back(c);
      x += c.w + S(4);
    }
  }
  double shift = right - (x - S(4));
  for (Keycap &c : caps) c.x += shift;
  return caps;
}

Color hover_color() { return look().dark ? Color::hex(0xFFFFFF, 0.07) : Color::hex(0x000000, 0.045); }

const double kGlideTime = 0.13;  // seconds from one scroll position to the next, as in the Start Menu

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// On/off switch drawn with Niny's colors, with an "On"/"Off" caption.
class Toggle : public Fl_Widget {
 public:
  Toggle(std::function<bool()> get, std::function<void(bool)> set)
      : Fl_Widget(0, 0, S(110), S(32)), get_(std::move(get)), set_(std::move(set)) {}
  void draw() override {
    const Look &L = look();
    bool on = get_();
    double sw = Sd(40), sh = Sd(20);
    double sx = x() + w() - sw - S(2), sy = y() + (h() - sh) / 2;
    {
      Painter p(window()->w(), window()->h());
      if (on) {
        p.fill_rounded(sx, sy, sw, sh, sh / 2, active() ? L.accent : L.dim.alpha(0.4));
      } else {
        p.fill_rounded(sx, sy, sw, sh, sh / 2, L.card);
        p.stroke_rounded(sx + 0.5, sy + 0.5, sw - 1, sh - 1, sh / 2, L.text.mix(L.dim, 0.3), Sd(1.2));
      }
      double r = sh / 2 - Sd(on ? 4 : 5);
      p.circle(on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, r, on ? L.on_accent : L.text.mix(L.dim, 0.3));
      if (Fl::focus() == this) p.stroke_rounded(sx - S(3), sy - S(3), sw + S(6), sh + S(6), sh / 2 + S(3), L.text, Sd(1.5));
    }
    text_font(L.theme.font_size);
    draw_text(on ? _("On") : _("Off"), x(), y(), int(sx - x() - S(12)), h(), active() ? L.text : L.dim, 2);
  }
  int handle(int event) override {
    switch (event) {
      case FL_PUSH:
        if (!active()) return 1;
        take_focus();
        return 1;
      case FL_RELEASE:
        if (active() && Fl::event_inside(this)) flip();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          flip();
          return 1;
        }
        return 0;
    }
    return Fl_Widget::handle(event);
  }

 private:
  void flip() {
    set_(!get_());
    if (window()) window()->redraw();
  }
  std::function<bool()> get_;
  std::function<void(bool)> set_;
};

// Dropdown showing the current value; opens a themed menu of choices.
class Choice : public Fl_Widget {
 public:
  using Options = std::function<std::vector<std::string>()>;
  Choice(std::function<std::string()> current, Options options, std::function<void(const std::string &)> set, int width)
      : Fl_Widget(0, 0, S(width), S(34)), current_(std::move(current)), options_(std::move(options)),
        set_(std::move(set)) {}
  void draw() override {
    const Look &L = look();
    {
      Painter p(window()->w(), window()->h());
      bool hot = Fl::belowmouse() == this;
      p.fill_rounded(x(), y(), w(), h(), Sd(5), hot ? L.card.mix(L.text, 0.04) : L.card);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(5), L.border, 1);
      icon::draw(p, icon::ChevronDown, x() + w() - S(26), y() + (h() - S(14)) / 2, S(14), L.dim);
      if (Fl::focus() == this) p.stroke_rounded(x() - 1, y() - 1, w() + 2, h() + 2, Sd(6), L.text, Sd(1.5));
    }
    text_font(L.theme.font_size);
    draw_text(current_(), x() + S(12), y(), w() - S(42), h(), L.text);
  }
  int handle(int event) override {
    switch (event) {
      case FL_ENTER:
      case FL_LEAVE:
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_PUSH:
        take_focus();
        open();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter || Fl::event_key() == FL_Down) {
          open();
          return 1;
        }
        return 0;
    }
    return Fl_Widget::handle(event);
  }

 private:
  void open() {
    MenuSpec spec;
    std::string cur = current_();
    for (auto &o : options_()) {
      MenuItem m;
      m.label = o;
      m.radio = true;
      m.checked = o == cur;
      m.run = [this, o] {
        set_(o);
        if (window()) window()->redraw();
      };
      spec.items.push_back(m);
    }
    spec.min_width = int(w() / ui_scale());
    popup_menu(spec, window()->x_root() + x(), window()->y_root() + y() + h() + S(2), h() + S(4));
  }
  std::function<std::string()> current_;
  Options options_;
  std::function<void(const std::string &)> set_;
};

// Plain button.
class Action : public Fl_Widget {
 public:
  Action(const std::string &label, std::function<void()> run)
      : Fl_Widget(0, 0, 10, S(34)), label_(label), run_(std::move(run)) {
    text_font(look().theme.font_size);
    size(text_width(label_) + S(32), S(34));
  }
  void draw() override {
    const Look &L = look();
    {
      Painter p(window()->w(), window()->h());
      bool hot = Fl::belowmouse() == this;
      p.fill_rounded(x(), y(), w(), h(), Sd(5), hot ? L.card.mix(L.text, 0.05) : L.card.mix(L.text, 0.015));
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(5), L.border, 1);
      if (Fl::focus() == this) p.stroke_rounded(x() - 1, y() - 1, w() + 2, h() + 2, Sd(6), L.text, Sd(1.5));
    }
    text_font(L.theme.font_size);
    draw_text(label_, x(), y(), w(), h(), active() ? L.text : L.dim, 1);
  }
  int handle(int event) override {
    switch (event) {
      case FL_ENTER:
      case FL_LEAVE:
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_PUSH:
        take_focus();
        return 1;
      case FL_RELEASE:
        if (Fl::event_inside(this)) run_();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          run_();
          return 1;
        }
        return 0;
    }
    return Fl_Widget::handle(event);
  }

 private:
  std::string label_;
  std::function<void()> run_;
};

}  // namespace

struct SettingsPage::Row {
  enum Kind { Heading, Item, Preview, About, Shortcut } kind = Item;
  std::string title, desc;
  std::string keys;  // Shortcut rows: "Ctrl+S", or alternatives separated by ", "
  int icon = -1;
  std::vector<Fl_Widget *> controls;
  int y = 0, h = 0;  // content coordinates
  bool first = false, last = false;
};

SettingsPage::SettingsPage(App *app, int x, int y, int w, int h) : Fl_Group(x, y, w, h), app_(app) {
  box(FL_NO_BOX);
  end();
  Config cfg;
  cfg.load();
  smooth_ = !cfg.get_bool("appearance", "low_resource", false);
  build();
}

SettingsPage::~SettingsPage() {
  Fl::remove_timeout(glide_cb, this);
  for (Row *r : rows_) delete r;
}

void SettingsPage::rebuild() {
  build();
  relayout();
  redraw();
}

void SettingsPage::set_mode(Mode m) {
  if (m != mode_) scroll_ = 0;
  mode_ = m;
  rebuild();
}

void SettingsPage::build() {
  for (Row *r : rows_) delete r;
  rows_.clear();
  clear();  // deletes the old controls
  back_ = nullptr;
  // build() runs from the constructor, while App's constructor has the window
  // open with begin(), and later from rebuild(). Restore whatever group was
  // current: end() would make the window current (later popups must not join
  // it), and clearing it in the constructor left the widgets App creates after
  // this page (find bar, palette, question sheet) without a window, so the
  // first ask() (About Niny) dereferenced a null window().
  Fl_Group *outer = Fl_Group::current();
  begin();
  back_ = new DrawnButton(0, 0, S(40), S(40), [this](Painter &p, bool hot, bool down) {
    const Look &L = look();
    if (hot || down) p.fill_rounded(back_->x(), back_->y(), back_->w(), back_->h(), Sd(6), hover_color());
    icon::draw(p, icon::ChevronLeft, back_->x() + S(10), back_->y() + S(10), S(20), L.text);
  }, nullptr, Sd(6));
  back_->tooltip(_("Back (Esc)"));
  back_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->show_settings_page(false); }, app_);
  if (mode_ == Mode::Shortcuts) build_shortcuts();
  else build_settings();
  end();
  Fl_Group::current(outer);
  // Mark the first and last rows of each group for the rounded corners.
  for (size_t i = 0; i < rows_.size(); ++i) {
    Row *r = rows_[i];
    if (r->kind == Row::Heading) continue;
    r->first = i == 0 || rows_[i - 1]->kind == Row::Heading;
    r->last = i + 1 == rows_.size() || rows_[i + 1]->kind == Row::Heading;
  }
}

// Help > Keyboard Shortcuts: the commands that have keys, by the menu that
// has them, then the keys of the tabs and the menus.
void SettingsPage::build_shortcuts() {
  auto heading = [&](const std::string &t) {
    Row *r = new Row;
    r->kind = Row::Heading;
    r->title = t;
    rows_.push_back(r);
  };
  auto keys = [&](const std::string &title, const std::string &k) {
    Row *r = new Row;
    r->kind = Row::Shortcut;
    r->title = title;
    r->keys = k;
    rows_.push_back(r);
  };
  for (const char *group : {N_("File"), N_("Edit"), N_("Format"), N_("View"), N_("Search"), N_("Document"),
                            N_("Tools"), N_("Help")}) {
    bool any = false;
    for (const Command &c : app_->commands()) {
      if (c.group != _(group) || c.shortcut_text.empty()) continue;
      if (!any) heading(_(group));
      any = true;
      keys(c.label, c.shortcut_text);
    }
  }
  heading(_("Tabs and menus"));
  keys(_("Next tab, previous tab"), "Ctrl+Tab, Ctrl+Shift+Tab");
  keys(_("Go to a tab (Alt+9: the last one)"), "Alt+1, Alt+9");
  keys(_("Open the menu bar"), "F10");
  std::string letters;
  for (int id = Toolbar::File; id <= Toolbar::Help; ++id)
    if (int k = Toolbar::mnemonic(id)) letters += (letters.empty() ? "" : ", ") + std::string("Alt+") + char(toupper(k));
  if (!letters.empty()) keys(_("Open a menu by its underlined letter"), letters);
  keys(_("Show the menu bar when it is hidden"), "Alt");
  keys(_("Context menu"), "Menu, Shift+F10");
  heading(_("More keys"));
  keys(_("Redo"), "Ctrl+Y");
  keys(_("Delete the word before or after the cursor"), "Ctrl+Backspace, Ctrl+Delete");
  keys(_("Zoom with the mouse wheel"), "Ctrl+Wheel");
  keys(_("Close the find bar, the palette or this page"), "Escape");
}

void SettingsPage::build_settings() {
  App *app = app_;
  auto changed = [app] { app->apply_settings(true); };
  auto heading = [&](const std::string &t) {
    Row *r = new Row;
    r->kind = Row::Heading;
    r->title = t;
    rows_.push_back(r);
  };
  auto item = [&](int icon, const std::string &title, const std::string &desc, std::vector<Fl_Widget *> controls) {
    Row *r = new Row;
    r->icon = icon;
    r->title = title;
    r->desc = desc;
    r->controls = std::move(controls);
    rows_.push_back(r);
    return r;
  };
  auto toggle = [&](bool Settings::*field) {
    return new Toggle([app, field] { return app->settings().*field; },
                      [app, field, changed](bool v) {
                        app->settings().*field = v;
                        changed();
                      });
  };
  auto choice = [&](std::function<std::string()> cur, Choice::Options opts, std::function<void(const std::string &)> set,
                    int width = 240) {
    return new Choice(std::move(cur), std::move(opts),
                      [set, changed](const std::string &v) {
                        set(v);
                        changed();
                      },
                      width);
  };
  heading(_("Appearance"));
  item(icon::Theme, _("App theme"), _("Follow the desktop, or always use light or dark colors"),
       {choice(
           [app] {
             const std::string &c = app->settings().color_scheme;
             return std::string(c == "dark" ? _("Dark") : c == "light" ? _("Light") : _("Use system setting"));
           },
           [] { return std::vector<std::string>{_("Light"), _("Dark"), _("Use system setting")}; },
           [app](const std::string &v) {
             app->settings().color_scheme = v == _("Dark") ? "dark" : v == _("Light") ? "light" : "auto";
           })});

  heading(_("Text formatting"));
  item(icon::Font, _("Font"), _("Family and size of the text you edit"),
       {choice([app] { return app->settings().font; }, [] { return font_families(); },
               [app](const std::string &v) { app->settings().font = v; }, 210),
        choice([app] { return std::to_string(app->settings().font_size); },
               [] {
                 std::vector<std::string> v;
                 for (int i : {8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24, 28, 32}) v.push_back(std::to_string(i));
                 return v;
               },
               [app](const std::string &v) {
                 app->settings().font_size = atoi(v.c_str());
                 app->settings().zoom = 0;
               },
               84)});
  {
    Row *r = new Row;
    r->kind = Row::Preview;
    rows_.push_back(r);
  }
  item(icon::Wrap, _("Word wrap"), _("Fit lines to the window instead of scrolling sideways"),
       {toggle(&Settings::word_wrap)});
  item(icon::TextTools, _("Syntax colors"), _("Color code, Markdown and configuration files"),
       {toggle(&Settings::syntax_colors)});

  heading(_("View"));
  item(icon::Menu, _("Menu bar"), _("File, Edit, View and the other menus; Alt or F10 shows them when hidden"),
       {toggle(&Settings::menu_bar)});
  item(icon::Info, _("Status bar"), _("Line and column, characters, file type, zoom, line ending and encoding"),
       {toggle(&Settings::status_bar)});
  item(icon::ListNumbered, _("Line numbers"), "", {toggle(&Settings::line_numbers)});
  item(icon::Pilcrow, _("Show whitespace"), _("Dots for spaces, arrows for tabs, marks at line ends"),
       {toggle(&Settings::show_whitespace)});
  item(icon::Menu, _("Highlight the current line"), "", {toggle(&Settings::highlight_line)});
  item(icon::Sidebar, _("Side panel"), _("Open documents and a file browser (F9)"), {toggle(&Settings::show_sidebar)});

  heading(_("Opening files"));
  item(icon::Window, _("Opening files"), _("Where files opened from other apps appear"),
       {choice([app] {
                 return std::string(app->settings().open_in_new_window ? _("Open in a new window")
                                                                       : _("Open in a new tab"));
               },
               [] { return std::vector<std::string>{_("Open in a new tab"), _("Open in a new window")}; },
               [app](const std::string &v) { app->settings().open_in_new_window = v == _("Open in a new window"); },
               260)});
  item(icon::Documents, _("When Niny starts"), _("Reopen the files you had open last time"),
       {choice([app] {
                 return std::string(app->settings().restore_session ? _("Open content from the previous session")
                                                                    : _("Start new session"));
               },
               [] {
                 return std::vector<std::string>{_("Open content from the previous session"), _("Start new session")};
               },
               [app](const std::string &v) { app->settings().restore_session = v != _("Start new session"); }, 330)});
  item(icon::Recent, _("Recent files"), _("Show recently opened files in File > Recent"),
       {new Action(_("Clear history"),
                   [app] {
                     app->clear_recent();
                     app->toast(_("Recent files cleared"));
                   }),
        toggle(&Settings::remember_recent)});

  heading(_("Spelling and the web"));
  {
    bool have = app_->spell_available();
    Toggle *t = toggle(&Settings::spell_check);
    if (!have) t->deactivate();
    item(icon::Spelling, _("Spell check"),
         have ? _("Underline misspelled words in plain text and Markdown")
              : _("Install hunspell or aspell (and a dictionary) to check spelling"),
         {t});
  }
  item(icon::Globe, _("Search the web with"), _("Used by Search the Web (Ctrl+E)"),
       {choice(
           [app] {
             const std::string &u = app->settings().search_url;
             if (u.find("startpage") != std::string::npos) return std::string("Startpage");
             if (u.find("wiktionary") != std::string::npos) return std::string("Wiktionary");
             if (u.find("wikipedia") != std::string::npos) return std::string("Wikipedia");
             if (u.find("duckduckgo") != std::string::npos) return std::string("DuckDuckGo");
             return std::string(_("Custom"));
           },
           [] { return std::vector<std::string>{"DuckDuckGo", "Startpage", "Wikipedia", "Wiktionary"}; },
           [app](const std::string &v) {
             std::string &u = app->settings().search_url;
             if (v == "Startpage") u = "https://www.startpage.com/do/search?q=%s";
             else if (v == "Wikipedia") u = "https://en.wikipedia.org/w/index.php?search=%s";
             else if (v == "Wiktionary") u = "https://en.wiktionary.org/w/index.php?search=%s";
             else u = "https://duckduckgo.com/?q=%s";
           })});

  heading(_("Editing"));
  item(icon::Menu, _("Tab width"), "",
       {choice([app] { return std::to_string(app->settings().tab_width); },
               [] { return std::vector<std::string>{"2", "3", "4", "8"}; },
               [app](const std::string &v) { app->settings().tab_width = atoi(v.c_str()); }, 84)});
  item(-1, _("Insert spaces for Tab"), "", {toggle(&Settings::insert_spaces)});
  item(-1, _("Detect the indentation of files"), "", {toggle(&Settings::detect_indent)});
  item(-1, _("Automatic indentation"), _("New lines keep the indentation; Markdown lists continue"),
       {toggle(&Settings::auto_indent)});
  item(-1, _("Close brackets and quotes"), "", {toggle(&Settings::auto_close)});
  item(-1, _("Highlight matching brackets"), "", {toggle(&Settings::bracket_match)});
  item(-1, _("Margin line"), _("A guide at a column, for code and commit messages"),
       {choice([app] {
                 int m = app->settings().right_margin;
                 return m ? std::to_string(m) : std::string(_("Off"));
               },
               [] { return std::vector<std::string>{_("Off"), "72", "80", "100", "120"}; },
               [app](const std::string &v) { app->settings().right_margin = atoi(v.c_str()); }, 100)});

  heading(_("Files and recovery"));
  item(icon::Save, _("Back up unsaved changes"), _("Recover your work if Niny or the computer stops"),
       {toggle(&Settings::backups)});
  item(-1, _("Keep unsaved changes when closing"), _("Niny remembers them instead of asking to save"),
       {toggle(&Settings::hot_exit)});
  item(-1, _("Watch files for changes"), _("Reload files changed by other programs"), {toggle(&Settings::watch_files)});
  item(-1, _("Trim trailing spaces when saving"), "", {toggle(&Settings::trim_trailing)});
  item(-1, _("End files with a newline"), "", {toggle(&Settings::final_newline)});
  item(-1, _("Line ending for new files"), "",
       {choice([app] { return std::string(app->settings().default_eol == "CRLF" ? _("Windows (CRLF)") : _("Unix (LF)")); },
               [] { return std::vector<std::string>{_("Unix (LF)"), _("Windows (CRLF)")}; },
               [app](const std::string &v) { app->settings().default_eol = v == _("Windows (CRLF)") ? "CRLF" : "LF"; },
               170)});

  heading(_("About this app"));
  {
    Row *r = new Row;
    r->kind = Row::About;
    rows_.push_back(r);
  }
}

void SettingsPage::resize(int X, int Y, int W, int H) {
  Fl_Widget::resize(X, Y, W, H);
  relayout();
}

void SettingsPage::relayout() {
  int col_w = std::min(w() - 2 * S(20), S(980));
  int x0 = x() + (w() - col_w) / 2;
  int yy = S(76);
  for (Row *r : rows_) {
    switch (r->kind) {
      case Row::Heading:
        yy += S(14);
        r->y = yy;
        r->h = S(36);
        break;
      case Row::Item: r->h = r->desc.empty() ? S(54) : S(68); r->y = yy; break;
      case Row::Preview: r->h = S(72); r->y = yy; break;
      case Row::About: r->h = S(148); r->y = yy; break;
      case Row::Shortcut: r->h = S(44); r->y = yy; break;
    }
    yy += r->h + (r->kind == Row::Heading ? 0 : S(3));
  }
  content_h_ = yy + S(32);
  scroll_ = std::max(0, std::min(scroll_, max_scroll()));
  int oy = y() - scroll_;
  if (back_) back_->resize(x0 - S(4), oy + S(18), S(40), S(40));
  // Controls show while any part of them is on the page: draw() clips them
  // to the page, and clicks outside the page never reach it. The controls
  // that scrolled away are hidden so that they cannot take clicks or focus;
  // a focused one hands the focus to the page, which keeps the keys working
  // (alpha 2: it went to the window and Page Down stopped scrolling).
  auto place = [this](Fl_Widget *c, int cx, int cy) {
    c->resize(cx, cy, c->w(), c->h());
    bool on_page = cy < y() + h() && cy + c->h() > y();
    if (on_page) {
      c->show();
    } else if (c->visible()) {
      bool had_focus = c->contains(Fl::focus());
      c->hide();
      if (had_focus) take_focus();
    }
  };
  for (Row *r : rows_) {
    int xr = x0 + col_w - S(18);
    // Narrow windows: controls keep their width, the text gets the rest.
    for (int i = int(r->controls.size()) - 1; i >= 0; --i) {
      Fl_Widget *c = r->controls[size_t(i)];
      int cw = c->w();
      place(c, xr - cw, oy + r->y + (r->h - c->h()) / 2);
      xr -= cw + S(10);
    }
  }
  if (back_) place(back_, back_->x(), back_->y());
}

// Smooth scrolling like the Start Menu: the wheel and the Page keys glide
// from where the page is now; a wheel turned again while it glides goes on
// from where it was heading. Low-resource mode jumps.
void SettingsPage::scroll_to(int offset, bool smooth) {
  offset = std::max(0, std::min(offset, max_scroll()));
  if (!smooth || !smooth_ || !visible_r() || std::abs(offset - scroll_) <= 1) {
    gliding_ = false;
    Fl::remove_timeout(glide_cb, this);
    if (offset != scroll_) {
      scroll_ = offset;
      relayout();
      redraw();
    }
    return;
  }
  glide_from_ = scroll_;
  glide_to_ = offset;
  glide_start_ = now() - 1.0 / 60;  // the first step is drawn at once
  gliding_ = true;
  Fl::remove_timeout(glide_cb, this);
  Fl::add_timeout(0, glide_cb, this);
}

void SettingsPage::glide_cb(void *data) {
  auto *self = static_cast<SettingsPage *>(data);
  if (!self->gliding_) return;
  double p = std::min(1.0, (now() - self->glide_start_) / kGlideTime);
  int pos = int(std::lround(self->glide_from_ + (self->glide_to_ - self->glide_from_) * ease_out(p)));
  if (p >= 1.0) {
    pos = self->glide_to_;
    self->gliding_ = false;
  }
  if (pos != self->scroll_) {
    self->scroll_ = pos;
    self->relayout();
    self->redraw();
  }
  if (self->gliding_) Fl::repeat_timeout(1.0 / 60, glide_cb, data);
}

bool SettingsPage::thumb(int &ty, int &th) const {
  if (content_h_ <= h()) return false;
  int track = h() - S(8);
  th = std::max(S(30), int(double(track) * h() / content_h_));
  ty = y() + S(4) + int(double(track - th) * scroll_ / std::max(1, max_scroll()));
  return true;
}

// Tab, Shift+Tab and the arrow keys go through every control in reading
// order, also those scrolled out of the page, which is scrolled to show them.
void SettingsPage::move_focus(int dir) {
  std::vector<Fl_Widget *> order;
  if (back_) order.push_back(back_);
  for (Row *r : rows_)
    for (Fl_Widget *c : r->controls)
      if (c->active()) order.push_back(c);
  if (order.empty()) return;
  int cur = -1;
  for (size_t i = 0; i < order.size(); ++i)
    if (order[i]->contains(Fl::focus())) cur = int(i);
  int n = int(order.size());
  int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
  Fl_Widget *target = order[size_t(next)];
  ensure_visible(target);
  target->take_focus();
  redraw();
}

void SettingsPage::ensure_visible(Fl_Widget *control) {
  int top = 0, bottom = 0;
  if (control == back_) {
    bottom = S(64);
  } else {
    bool found = false;
    for (Row *r : rows_)
      for (Fl_Widget *c : r->controls)
        if (c == control) {
          top = r->y;
          bottom = r->y + r->h;
          found = true;
        }
    if (!found) return;
  }
  int margin = S(16), target = scroll_;
  if (top - margin < scroll_) target = std::max(0, top - margin - (control == back_ ? 0 : S(40)));
  else if (bottom + margin > scroll_ + h()) target = bottom + margin - h();
  if (control == back_) target = 0;
  scroll_to(target, false);
}

void SettingsPage::draw() {
  const Look &L = look();
  int col_w = std::min(w() - 2 * S(20), S(980));
  int x0 = x() + (w() - col_w) / 2;
  int oy = y() - scroll_;
  Color page = L.dark ? L.window : L.window.mix(L.bar, 0.3);
  Color card = L.dark ? L.card.mix(L.window, 0.25) : L.card;
  fl_push_clip(x(), y(), w(), h());
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), page);
    for (Row *r : rows_) {
      if (r->kind == Row::Heading) continue;
      double ry = oy + r->y;
      if (ry > y() + h() || ry + r->h < y()) continue;
      double rad = Sd(6);
      p.fill_rounded(x0, ry, col_w, r->h, rad, card);
      p.stroke_rounded(x0 + 0.5, ry + 0.5, col_w - 1, r->h - 1, rad, L.border.alpha(L.dark ? 0.5 : 0.7), 1);
      if (r->icon >= 0) icon::draw(p, icon::Kind(r->icon), x0 + S(20), ry + (r->h - S(20)) / 2.0, S(20), L.text);
      if (r->kind == Row::About) icon::logo(p, x0 + S(20), ry + S(22), S(40));
      if (r->kind == Row::Preview) {
        p.fill_rounded(x0 + S(54), ry + S(12), col_w - S(72), r->h - S(24), Sd(5), L.editor);
        p.stroke_rounded(x0 + S(54) + 0.5, ry + S(12) + 0.5, col_w - S(72) - 1, r->h - S(24) - 1, Sd(5), L.border, 1);
      }
      if (r->kind == Row::Shortcut)
        for (const Keycap &c : keycaps(r->keys, x0 + col_w - S(16)))
          if (!c.slash) {
            double ky = ry + (r->h - S(26)) / 2.0;
            p.fill_rounded(c.x, ky, c.w, S(26), Sd(5), L.field);
            p.stroke_rounded(c.x + 0.5, ky + 0.5, c.w - 1, S(26) - 1, Sd(5), L.border, 1);
          }
    }
    cairo_restore(cr);
  }
  text_font(L.theme.font_size + S(12), true);
  draw_text(mode_ == Mode::Shortcuts ? _("Keyboard Shortcuts") : _("Preferences"), x0 + S(46), oy + S(16),
            col_w - S(60), S(44), L.text);
  for (Row *r : rows_) {
    int ry = oy + r->y;
    if (ry > y() + h() || ry + r->h < y()) continue;
    int tx = x0 + (r->icon >= 0 || r->kind == Row::About ? S(56) : S(20));
    int right = r->controls.empty() ? x0 + col_w - S(16) : r->controls.front()->x() - S(16);
    switch (r->kind) {
      case Row::Heading:
        text_font(L.theme.font_size, true);
        draw_text(r->title, x0 + S(2), ry + S(6), col_w, S(28), L.text);
        break;
      case Row::Item:
        text_font(L.theme.font_size + S(1));
        if (r->desc.empty()) {
          draw_text(r->title, tx, ry, right - tx, r->h, L.text);
        } else {
          draw_text(r->title, tx, ry + S(12), right - tx, S(24), L.text);
          text_font(L.theme.font_size - S(1));
          draw_text(r->desc, tx, ry + S(36), right - tx, S(20), L.dim);
        }
        break;
      case Row::Preview: {
        const Settings &s = app_->settings();
        int px = std::max(8, int(std::lround(s.font_size * 4.0 / 3.0 * ui_scale())));
        fl_font(FL_FREE_FONT, px);
        fl_color(L.text.fl());
        fl_push_clip(x0 + S(66), ry + S(12), col_w - S(96), r->h - S(24));
        fl_draw(_("The quick brown fox jumps over the lazy dog. 0123456789 {}[]()"), x0 + S(66), ry + S(12),
                col_w - S(96), r->h - S(24), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        fl_pop_clip();
        break;
      }
      case Row::About: {
        text_font(L.theme.font_size + S(4), true);
        draw_text("Niny", tx + S(22), ry + S(16), col_w - S(100), S(28), L.text);
        text_font(L.theme.font_size);
        draw_text(format(_("Version %s"), NINY_VERSION), tx + S(22), ry + S(44), col_w - S(100), S(22), L.dim);
        draw_text(developer_line(), tx + S(22), ry + S(68), col_w - S(100), S(22), L.text);
        text_font(L.theme.font_size - S(1));
        draw_text(std::string(kCopyright) + " · " + license_name("GPL-3.0-or-later"), tx + S(22), ry + S(92),
                  col_w - S(100), S(20), L.dim);
        draw_text(format(_("Settings are kept in %s"), home_relative(settings_path()).c_str()), tx + S(22), ry + S(114),
                  col_w - S(100), S(20), L.dim);
        break;
      }
      case Row::Shortcut: {
        auto caps = keycaps(r->keys, x0 + col_w - S(16));
        int left = caps.empty() ? right : int(caps.front().x) - S(16);
        text_font(L.theme.font_size + S(1));
        draw_text(r->title, tx, ry, left - tx, r->h, L.text);
        text_font(L.theme.font_size - S(1));
        for (const Keycap &c : caps)
          draw_text(c.text, int(c.x), ry + (r->h - S(26)) / 2, int(c.w), S(26), c.slash ? L.dim : L.text, 1);
        break;
      }
    }
  }
  draw_children();
  // Scroll bar: a thin thumb that widens, with its track, under the pointer.
  int ty = 0, th = 0;
  if (thumb(ty, th)) {
    Painter p(window()->w(), window()->h());
    bool wide = bar_hot_ || drag_ >= 0;
    int bw = wide ? S(8) : S(5);
    int bx = x() + w() - S(4) - bw;
    if (wide) p.fill_rounded(bx - S(2), y() + S(2), bw + S(4), h() - S(4), (bw + S(4)) / 2.0, L.dim.alpha(0.12));
    p.fill_rounded(bx, ty, bw, th, bw / 2.0, L.dim.alpha(drag_ >= 0 ? 0.7 : wide ? 0.55 : 0.35));
  }
  fl_pop_clip();
}

int SettingsPage::handle(int event) {
  switch (event) {
    case FL_MOUSEWHEEL: {
      if (!Fl::event_dy()) return 0;
      int from = gliding_ ? glide_to_ : scroll_;
      scroll_to(from + Fl::event_dy() * S(56), true);
      return 1;
    }
    case FL_PUSH: {
      int ty = 0, th = 0;
      if (Fl::event_button() == FL_LEFT_MOUSE && in_scrollbar(Fl::event_x()) && thumb(ty, th)) {
        if (Fl::event_y() >= ty && Fl::event_y() < ty + th) {
          drag_ = Fl::event_y() - ty;
          redraw();
        } else {
          // The track pages towards the pointer, like other scroll bars.
          int page = h() - S(60);
          scroll_to((gliding_ ? glide_to_ : scroll_) + (Fl::event_y() < ty ? -page : page), true);
        }
        return 1;
      }
      int r = Fl_Group::handle(event);
      if (!r && !Fl::focus()) take_focus();
      return 1;
    }
    case FL_DRAG:
      if (drag_ >= 0) {
        int ty = 0, th = 0;
        if (thumb(ty, th)) {
          int track = h() - S(8) - th;
          int pos = Fl::event_y() - drag_ - y() - S(4);
          scroll_to(int(std::lround(double(pos) * max_scroll() / std::max(1, track))), false);
        }
        return 1;
      }
      break;
    case FL_RELEASE:
      if (drag_ >= 0) {
        drag_ = -1;
        bar_hot_ = Fl::event_inside(this) && in_scrollbar(Fl::event_x());
        redraw();
        return 1;
      }
      break;
    case FL_ENTER:
    case FL_MOVE: {
      bool hot = in_scrollbar(Fl::event_x()) && content_h_ > h();
      if (hot != bar_hot_) {
        bar_hot_ = hot;
        redraw();
      }
      Fl_Group::handle(event);
      return 1;
    }
    case FL_LEAVE:
      if (bar_hot_) {
        bar_hot_ = false;
        redraw();
      }
      break;
    case FL_FOCUS:
      return 1;
    case FL_KEYBOARD: {
      int k = Fl::event_key(), state = Fl::event_state() & (FL_SHIFT | FL_CTRL | FL_ALT | FL_META);
      bool on_page = Fl::focus() == this;
      int from = gliding_ ? glide_to_ : scroll_, page = h() - S(60);
      if (k == FL_Tab && (state & ~FL_SHIFT) == 0) {
        move_focus(state & FL_SHIFT ? -1 : 1);
        return 1;
      }
      if (state) break;
      switch (k) {
        case FL_Page_Down: scroll_to(from + page, true); return 1;
        case FL_Page_Up: scroll_to(from - page, true); return 1;
        case FL_Home: scroll_to(0, true); return 1;
        case FL_End: scroll_to(max_scroll(), true); return 1;
        case FL_Down:
        case FL_Up:
          if (on_page) scroll_to(from + (k == FL_Down ? S(56) : -S(56)), true);
          else move_focus(k == FL_Down ? 1 : -1);
          return 1;
        case ' ':
          if (on_page) {
            scroll_to(from + page, true);
            return 1;
          }
          break;
      }
      break;
    }
  }
  return Fl_Group::handle(event);
}

}  // namespace niny
