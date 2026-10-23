// SPDX-License-Identifier: GPL-3.0-or-later
// Control Center > Overview: the hub of every setting.
//
// The account at the top (picture and name, a click opens Users & Avatar),
// a search over every setting, then tiles by category: Personalization,
// System, Network & Internet, Devices, Administration and About. A tile
// opens a page of this window (scrolled to its card when it has an anchor)
// or starts a separate tool (Display Settings, Software Updater, Driver
// Manager, System Reports, System Monitor, printers, disks); a tool that is
// not installed falls back to the page, or shows what to install.
//
// The tiles keep to the width of the window: three, two or one per row, so
// the overview never scrolls sideways (1024 x 600 at 150 % has two).
#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "avatar.h"
#include "avatar_view.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "settings.h"
#include "spawn.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

namespace sde {

namespace {

// One way to open a tile: a program (when |argv|[0], and |needs| if set,
// are installed).
struct Tool {
  std::vector<std::string> argv;
  const char *needs = nullptr;
};

struct Tile {
  const char *id;      // symbol and QA name ("tile:<id>")
  const char *title;
  const char *hint;
  const char *page;    // a page of this window, or nullptr
  const char *anchor;  // ...and the card it scrolls to
  std::vector<Tool> tools;  // tried before the page: the first installed one
  const char *keywords;     // more words the search finds it by
  const char *missing;      // shown when nothing can open it
  bool search_only;         // a result of the search, not a tile of its category
};

struct Category {
  const char *id;
  const char *title;
  std::vector<Tile> tiles;
};

const std::vector<Category> &categories() {
  static const std::vector<Category> list = {
      {"personalization",
       N_("Personalization"),
       {
           {"appearance", N_("Appearance"), N_("Theme, colors and wallpaper"), "appearance", nullptr, {},
            "theme dark light accent color colour wallpaper background font text size scale icons "
            "transparency glass blur",
            nullptr, false},
           {"desktop", N_("Desktop"), N_("Icons on the desktop"), "desktop", nullptr, {},
            "icons home trash computer", nullptr, false},
           {"panel", N_("Panel"), N_("Position, size and pinned apps"), "panel", nullptr, {},
            "taskbar tray clock pinned autohide search bar", nullptr, false},
           {"start-menu", N_("Start Menu"), N_("Size, lists and your account"), "start-menu", nullptr,
            {}, "start menu applications recently added most used tiles", nullptr, false},
           {"action-center", N_("Action Center"), N_("Quick controls and greeting"), "action-center",
            nullptr, {}, "quick settings tiles greeting", nullptr, false},
           {"clock", N_("Clock & Weather"), N_("Clock style and weather units"), "clock", nullptr, {},
            "clock analog digital seconds weather temperature", nullptr, false},
           {"notifications", N_("Notifications"), N_("Alerts and Do Not Disturb"), "notifications",
            nullptr, {}, "notification alert disturb dnd", nullptr, false},
           {"effects", N_("Effects"), N_("Animations and compositing"), "effects", nullptr, {},
            "animation effect compositor shadow fade blur", nullptr, false},
           {"lock", N_("Lock Screen"), N_("When and how the screen locks"), "lock", nullptr, {},
            "lock screensaver password idle", nullptr, false},
       }},
      {"system",
       N_("System"),
       {
           {"display", N_("Display"), N_("Resolution, arrangement and scale"), "display", nullptr,
            {{{"sde-display-settings"}}}, "monitor screen resolution refresh rotation scale arrange",
            nullptr, false},
           {"sound", N_("Sound"), N_("Volume, speakers and microphones"), "sound", nullptr, {},
            "volume audio speaker headphones microphone output input", nullptr, false},
           {"power", N_("Power"), N_("Battery, sleep and brightness"), "power", nullptr, {},
            "battery sleep suspend lid brightness dim", nullptr, false},
           {"date-time", N_("Date & Time"), N_("Time zone and network time"), "date-time", nullptr, {},
            "date time zone timezone clock ntp", nullptr, false},
           {"users", N_("Users & Avatar"), N_("Your picture and your name"), "users", nullptr, {},
            "user account avatar picture photo face name password", nullptr, false},
           {"default-apps", N_("Default Applications"), N_("Browser, mail, music and more"),
            "default-apps", nullptr, {},
            "default browser mail email music video image viewer pdf terminal file types open with",
            nullptr, false},
           {"startup", N_("Startup Applications"), N_("Programs started at login"), nullptr, nullptr,
            {{{"sde-system-monitor", "--page", "startup"}}}, "startup autostart login boot",
            N_("Needs SDE System Monitor"), false},
           {"keyboard", N_("Keyboard"), N_("Layouts and key repeat"), "input", nullptr, {},
            "keyboard layout language repeat typing", nullptr, false},
           {"mouse", N_("Mouse & Touchpad"), N_("Pointer speed, tapping, scrolling"), "input", "mouse",
            {}, "mouse touchpad pointer tap scroll natural speed", nullptr, false},
           {"touchpad", N_("Touchpad"), N_("Tapping, scrolling and clicking"), "touchpad", nullptr, {},
            "touchpad tap scroll edge click palm", nullptr, true},
       }},
      {"network",
       N_("Network & Internet"),
       {
           {"network", N_("Network"), N_("Wi-Fi and wired connections"), "network", nullptr, {},
            "wifi wi-fi wlan lan ethernet internet vpn connection", nullptr, false},
           {"proxy", N_("Proxy"), N_("A proxy server for the internet"), "network", "proxy", {},
            "proxy http https socks pac", nullptr, false},
           {"bluetooth", N_("Bluetooth"), N_("Pair and connect devices"), "bluetooth", nullptr, {},
            "bluetooth pair headphones speaker", nullptr, false},
       }},
      {"devices",
       N_("Devices"),
       {
           {"drives", N_("Drives"), N_("Disks, USB sticks and cards"), nullptr, nullptr,
            {{{"gnome-disks"}}, {{"sde-panel", "--section", "drives"}}},
            "drive disk usb stick card eject format mount", N_("Needs the SDE panel"), false},
           {"printers", N_("Printers"), N_("Add and manage printers"), nullptr, nullptr,
            {{{"system-config-printer"}}, {{"xdg-open", "http://localhost:631/"}, "lpstat"}},
            "printer print cups scanner", N_("Install system-config-printer"), false},
       }},
      {"administration",
       N_("Administration"),
       {
           {"updates", N_("Software Updater"), N_("Security and software updates"), nullptr, nullptr,
            {{{"sde-update"}}}, "update upgrade security packages flatpak", N_("Needs Software Updater"),
            false},
           {"drivers", N_("Driver Manager"), N_("Graphics, Wi-Fi and firmware"), nullptr, nullptr,
            {{{"sde-drivers"}}}, "driver nvidia firmware graphics wifi", N_("Needs Driver Manager"),
            false},
           {"reports", N_("System Reports"), N_("Problems and system information"), nullptr, nullptr,
            {{{"sde-system-reports"}}}, "report crash problem bug information",
            N_("Needs System Reports"), false},
           {"sysmon", N_("System Monitor"), N_("Processes, performance and disks"), nullptr, nullptr,
            {{{"sde-system-monitor"}}}, "process task manager cpu memory performance disk",
            N_("Needs SDE System Monitor"), false},
           {"suite", N_("Applications"), N_("Included applications and credits"), "suite", nullptr, {},
            "applications apps credits license source", nullptr, false},
           {"system", N_("Power & Updates"), N_("Power mode and system tools"), "system", nullptr, {},
            "power mode performance updates tools", nullptr, true},
       }},
      {"about",
       N_("About"),
       {
           {"about", N_("About"), N_("This computer and SDE"), "about", nullptr, {},
            "about version system information graphics hardware", nullptr, false},
       }},
  };
  return list;
}

// What a tile opens, decided when the overview is built.
struct Target {
  std::vector<std::string> argv;  // a program, or
  std::string page, anchor;       // a page of this window
  bool tool() const { return !argv.empty(); }
  bool usable() const { return tool() || !page.empty(); }
};

Target target_of(const Tile &t) {
  Target out;
  for (const Tool &tool : t.tools)
    if (!tool.argv.empty() && !find_program(tool.argv[0]).empty() &&
        (!tool.needs || !find_program(tool.needs).empty())) {
      out.argv = tool.argv;
      return out;
    }
  if (t.page) {
    out.page = t.page;
    out.anchor = t.anchor ? t.anchor : "";
  }
  return out;
}

std::string g_query;           // the overview's search
Fl_Input *g_search = nullptr;  // its field in the page shown

// An icon from the theme for a few tools, else the native symbol.
const char *theme_icon(const std::string &id) {
  if (id == "updates") return "system-software-update";
  if (id == "sysmon") return "utilities-system-monitor";
  if (id == "printers") return "printer";
  return nullptr;
}

class HubTile : public Fl_Widget {
 public:
  // |compact|: narrow tiles (small screens, large interface sizes) with a
  // smaller symbol and the hint on up to two lines.
  HubTile(int x, int y, int w, int h, const Tile &tile, Target target, bool compact)
      : Fl_Widget(x, y, w, h), tile_(tile), target_(std::move(target)), compact_(compact) {
    title_ = _(tile.title);
    hint_ = target_.usable() ? std::string(_(tile.hint))
                             : std::string(tile.missing ? _(tile.missing) : _("Not installed"));
    tooltip(title_.c_str());
    if (!target_.usable()) deactivate();
  }
  void activate_tile() {
    if (target_.tool()) {
      spawn_detached(target_.argv);
      return;
    }
    if (!target_.page.empty()) open_settings_page(target_.page, target_.anchor);
  }
  void draw() override {
    const Theme &t = settings_theme();
    bool on = active_r();
    {
      Painter p(window()->w(), window()->h());
      double r = std::min(double(t.radius), Sd(12));
      p.fill_rounded(x(), y(), w(), h(), r, hot_ && on ? t.hover : t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, hot_ && on ? t.accent : t.border);
      int box = compact_ ? S(30) : S(36), bx = x() + (compact_ ? S(10) : S(12));
      int by = compact_ ? y() + S(10) : y() + (h() - box) / 2;
      p.fill_rounded(bx, by, box, box, Sd(compact_ ? 8 : 10), t.accent.alpha(on ? 0.12 : 0.06));
      Color ink = on ? t.accent_text : t.text_dim;
      int icon_size = compact_ ? S(20) : S(24), glyph_size = compact_ ? S(18) : S(20);
      const char *icon = theme_icon(tile_.id);
      std::string path = icon ? IconLoader::lookup(icon, icon_size) : std::string();
      if (!path.empty())
        p.image(IconLoader::get({icon}, icon_size), bx + (box - icon_size) / 2.0,
                by + (box - icon_size) / 2.0, icon_size, on ? 1.0 : 0.5);
      else
        draw_settings_glyph(p, tile_.id, bx + (box - glyph_size) / 2.0, by + (box - glyph_size) / 2.0,
                            glyph_size, ink);
      if (target_.tool()) {
        // A separate program: a small arrow out of the corner.
        double ax = x() + w() - Sd(15), ay = y() + Sd(9), z = Sd(6);
        p.line(ax, ay + z, ax + z, ay, t.text_dim, Sd(1.3));
        p.line(ax + z * 0.35, ay, ax + z, ay, t.text_dim, Sd(1.3));
        p.line(ax + z, ay, ax + z, ay + z * 0.65, t.text_dim, Sd(1.3));
      }
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
    }
    int tx = x() + (compact_ ? S(48) : S(60)), tw = w() - (tx - x()) - S(10);
    int title_w = tw - (target_.tool() ? S(12) : 0);
    text_font(t.font_size, true);
    if (compact_) {
      draw_text(title_, tx, y() + S(8), title_w, S(22), on ? t.text : t.text_dim);
      text_font(std::max(S(9), t.font_size - S(1)));
      fl_color(t.text_dim.fl());
      fl_draw(hint_.c_str(), tx, y() + S(30), tw, h() - S(34),
              Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
    } else {
      draw_text(title_, tx, y() + h() / 2 - S(21), title_w, S(22), on ? t.text : t.text_dim);
      text_font(std::max(S(9), t.font_size - S(1)));
      draw_text(hint_, tx, y() + h() / 2 + S(1), tw, S(20), t.text_dim);
    }
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        if (!active_r()) return 1;
        take_focus();
        return 1;
      case FL_RELEASE:
        if (active_r() && Fl::event_inside(this)) activate_tile();
        return 1;
      case FL_ENTER:
      case FL_LEAVE:
        hot_ = e == FL_ENTER;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
          activate_tile();
          return 1;
        }
        return 0;
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  const Tile &tile_;
  Target target_;
  bool compact_;
  std::string title_, hint_;
  bool hot_ = false;
};

// The account: picture, name and kind; opens Users & Avatar.
class AccountCard : public Fl_Widget {
 public:
  AccountCard(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    tooltip(_("Users & Avatar"));
  }
  void draw() override {
    const Theme &t = settings_theme();
    const avatar::Account &a = settings_account();
    int d = h() - S(20), ax = x() + S(14), ay = y() + S(10);
    cairo_surface_t *pic = settings_avatar().at(d);
    {
      Painter p(window()->w(), window()->h());
      double r = std::min(double(t.radius), Sd(14));
      p.fill_rounded(x(), y(), w(), h(), r, hot_ ? t.hover : t.surface);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, hot_ ? t.accent : t.border);
      avatar::paint_circle(p, pic, ax, ay, d, t.accent);
      glyph::chevron(p, x() + w() - S(30), y() + h() / 2.0 - S(8), S(16), true, t.text_dim);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), r);
    }
    if (!pic) avatar::draw_initials(a.display_name(), ax, ay, d, t.on_accent);
    int tx = ax + d + S(16), tw = w() - (tx - x()) - S(44);
    text_font(t.font_size + 2, true);
    draw_text(a.display_name(), tx, y() + h() / 2 - S(24), tw, S(26), t.text);
    text_font(t.font_size - 1);
    std::string kind = a.admin ? _("Administrator") : _("Standard account");
    draw_text(a.user + "  ·  " + kind, tx, y() + h() / 2 + S(2), tw, S(22), t.text_dim);
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        take_focus();
        return 1;
      case FL_RELEASE:
        if (Fl::event_inside(this)) open_settings_page("users");
        return 1;
      case FL_ENTER:
      case FL_LEAVE:
        hot_ = e == FL_ENTER;
        redraw();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          open_settings_page("users");
          return 1;
        }
        return 0;
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  bool hot_ = false;
};

void first_result(void *);

// The rounded field behind the search, with its magnifier.
class SearchField : public Fl_Widget {
 public:
  SearchField(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  Fl_Widget *input = nullptr;
  void draw() override {
    const Theme &t = settings_theme();
    bool focused = input && Fl::focus() == input;
    Painter p(window()->w(), window()->h());
    double r = h() / 2.0;
    p.fill_rounded(x(), y(), w(), h(), r, t.surface);
    p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, r, focused ? t.accent : t.border,
                     focused ? Sd(2) : Sd(1));
    cairo_t *cr = p.cr();
    double g = S(16), gx = x() + S(14), gy = y() + (h() - g) / 2.0;
    cairo_new_path(cr);
    cairo_arc(cr, gx + g * 0.42, gy + g * 0.42, g * 0.3, 0, 2 * M_PI);
    cairo_move_to(cr, gx + g * 0.64, gy + g * 0.64);
    cairo_line_to(cr, gx + g * 0.92, gy + g * 0.92);
    p.set(t.text_dim);
    cairo_set_line_width(cr, Sd(1.6));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
  }
  int handle(int e) override {
    // A click on the field (outside the text) types into it.
    if (e == FL_PUSH && input) {
      input->take_focus();
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

// The search text: the overview is rebuilt with the matching tiles as one
// types; Enter opens the first one, Escape clears it.
class HubSearch : public Fl_Input {
 public:
  HubSearch(SearchField *field)
      : Fl_Input(field->x() + S(38), field->y() + S(4), field->w() - S(54), field->h() - S(8)),
        field_(field) {
    const Theme &t = settings_theme();
    field->input = this;
    box(FL_FLAT_BOX);
    color(t.surface.fl());
    textcolor(t.text.fl());
    cursor_color(t.accent.fl());
    selection_color(t.accent.fl());
    textsize(t.font_size + 1);
    when(FL_WHEN_CHANGED);
    tooltip(_("Find a setting"));
    callback([](Fl_Widget *w, void *) {
      g_query = static_cast<Fl_Input *>(w)->value();
      Fl::remove_timeout(rebuild);
      Fl::add_timeout(0.0, rebuild);
    });
  }
  static void rebuild(void *) {
    refresh_page();
    if (g_search) {
      g_search->take_focus();
      g_search->position(g_search->size());
    }
  }
  void draw() override {
    Fl_Input::draw();
    if (size() == 0 && Fl::focus() != this) {
      const Theme &t = settings_theme();
      text_font(t.font_size + 1);
      draw_text(_("Find a setting"), x() + S(3), y(), w() - S(6), h(), t.text_dim);
    }
  }
  int handle(int e) override {
    if (e == FL_FOCUS || e == FL_UNFOCUS) field_->redraw();
    if (e == FL_KEYBOARD && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
      Fl::add_timeout(0.0, first_result);
      return 1;
    }
    if (e == FL_KEYBOARD && Fl::event_key() == FL_Escape && size() > 0) {
      value("");
      do_callback();
      return 1;
    }
    return Fl_Input::handle(e);
  }

 private:
  SearchField *field_;
};

std::vector<HubTile *> g_results;  // the tiles shown, in order

void first_result(void *) {
  for (HubTile *t : g_results)
    if (t->active_r()) {
      t->activate_tile();
      return;
    }
}

bool matches(const Category &c, const Tile &t, const std::vector<std::string> &words) {
  std::string text = lower(std::string(_(t.title)) + " " + _(t.hint) + " " + _(c.title) + " " +
                           t.title + " " + t.hint + " " + (t.keywords ? t.keywords : "") + " " + t.id);
  for (const std::string &w : words)
    if (text.find(w) == std::string::npos) return false;
  return true;
}

}  // namespace

void build_hub(Page *page) {
  g_results.clear();
  page->heading(_("Control Center"), _("Every setting of your computer, in one place"));
  int x = page->left(), w = page->width();
  new AccountCard(x, page->cursor(), w, S(78));
  page->advance(S(78) + S(14));
  g_search = new HubSearch(new SearchField(x, page->cursor(), w, S(38)));
  g_search->value(g_query.c_str());
  page->advance(S(38) + S(16));

  // Two or three tiles a row with their hint on one line; narrow pages (a
  // small screen or a large interface size) keep two compact tiles a row.
  int gap = S(10);
  int cols = std::max(1, std::min(3, (w + gap) / (S(250) + gap)));
  bool compact = false;
  if (cols == 1 && w >= S(360)) {
    cols = 2;
    compact = true;
  }
  int tw = (w - (cols - 1) * gap) / cols, th = compact ? S(72) : S(62);
  auto grid = [&](const std::vector<std::pair<const Tile *, Target>> &tiles) {
    int y = page->cursor();
    for (size_t i = 0; i < tiles.size(); ++i) {
      int col = int(i % size_t(cols)), row = int(i / size_t(cols));
      auto *tile = new HubTile(x + col * (tw + gap), y + row * (th + gap), tw, th, *tiles[i].first,
                               tiles[i].second, compact);
      g_results.push_back(tile);
      qa_line(std::string("tile:") + tiles[i].first->id, tile->x(), tile->y(), tile->w(), tile->h());
    }
    int rows = int((tiles.size() + size_t(cols) - 1) / size_t(cols));
    page->advance(rows * (th + gap) - gap);
  };
  auto caption = [&](const std::string &id, const std::string &title) {
    int y = page->cursor();
    new Label(x + S(2), y, w, S(28), title, Label::BOLD);
    qa_line("category:" + id, x, y, w, S(28));
    page->advance(S(32));
  };

  std::vector<std::string> words;
  for (auto &word : split(lower(trim(g_query)), ' '))
    if (!word.empty()) words.push_back(word);
  if (words.empty()) {
    for (const Category &c : categories()) {
      std::vector<std::pair<const Tile *, Target>> tiles;
      for (const Tile &t : c.tiles)
        if (!t.search_only) tiles.push_back({&t, target_of(t)});
      caption(c.id, _(c.title));
      grid(tiles);
      page->advance(S(20));
    }
  } else {
    std::vector<std::pair<const Tile *, Target>> tiles;
    for (const Category &c : categories())
      for (const Tile &t : c.tiles)
        if (matches(c, t, words)) tiles.push_back({&t, target_of(t)});
    if (tiles.empty()) {
      page->note(format(_("No setting matches \"%s\"."), trim(g_query).c_str()));
    } else {
      caption("results", tiles.size() == 1 ? std::string(_("1 setting"))
                                            : format(_("%d settings"), int(tiles.size())));
      grid(tiles);
      page->advance(S(20));
    }
  }
  page->note(_("Changes are saved automatically. The search on the panel finds these settings "
               "too."));
  page->finish();
}

bool hub_glyph(Painter &p, const std::string &id, double x, double y, double s, const Color &c) {
  cairo_t *cr = p.cr();
  double lw = std::max(1.2, s * 0.08);
  auto stroke = [&] {
    p.set(c);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
  };
  auto X = [&](double f) { return x + s * f; };
  auto Y = [&](double f) { return y + s * f; };
  cairo_new_path(cr);
  if (id == "users") {
    cairo_arc(cr, X(0.5), Y(0.32), s * 0.18, 0, 2 * M_PI);
    stroke();
    cairo_new_path(cr);
    cairo_arc(cr, X(0.5), Y(0.98), s * 0.36, M_PI * 1.08, M_PI * 1.92);
    stroke();
  } else if (id == "start-menu") {
    for (int i = 0; i < 4; ++i)
      p.fill_rounded(X(0.1 + (i % 2) * 0.44), Y(0.1 + (i / 2) * 0.44), s * 0.36, s * 0.36, s * 0.07, c);
  } else if (id == "date-time") {
    p.stroke_rounded(X(0.08), Y(0.16), s * 0.84, s * 0.76, s * 0.12, c, lw);
    p.line(X(0.08), Y(0.38), X(0.92), Y(0.38), c, lw);
    p.line(X(0.32), Y(0.06), X(0.32), Y(0.24), c, lw);
    p.line(X(0.68), Y(0.06), X(0.68), Y(0.24), c, lw);
    for (int i = 0; i < 3; ++i) p.circle(X(0.3 + i * 0.2), Y(0.6), s * 0.05, c);
    p.circle(X(0.3), Y(0.78), s * 0.05, c);
  } else if (id == "startup") {
    cairo_arc(cr, X(0.5), Y(0.5), s * 0.42, 0, 2 * M_PI);
    stroke();
    cairo_new_path(cr);
    cairo_move_to(cr, X(0.42), Y(0.32));
    cairo_line_to(cr, X(0.70), Y(0.5));
    cairo_line_to(cr, X(0.42), Y(0.68));
    cairo_close_path(cr);
    p.set(c);
    cairo_fill(cr);
  } else if (id == "keyboard") {
    glyph::keyboard(p, x, y, s, c);
  } else if (id == "mouse") {
    p.stroke_rounded(X(0.26), Y(0.06), s * 0.48, s * 0.88, s * 0.24, c, lw);
    p.line(X(0.5), Y(0.08), X(0.5), Y(0.36), c, lw);
  } else if (id == "proxy") {
    cairo_arc(cr, X(0.5), Y(0.5), s * 0.42, 0, 2 * M_PI);
    stroke();
    cairo_new_path(cr);
    cairo_save(cr);
    cairo_translate(cr, X(0.5), Y(0.5));
    cairo_scale(cr, 0.45, 1.0);
    cairo_arc(cr, 0, 0, s * 0.42, 0, 2 * M_PI);
    cairo_restore(cr);
    stroke();
    p.line(X(0.1), Y(0.5), X(0.9), Y(0.5), c, lw);
  } else if (id == "drives") {
    p.stroke_rounded(X(0.06), Y(0.28), s * 0.88, s * 0.44, s * 0.12, c, lw);
    p.circle(X(0.76), Y(0.5), s * 0.06, c);
    p.line(X(0.2), Y(0.5), X(0.52), Y(0.5), c, lw);
  } else if (id == "printers") {
    p.stroke_rounded(X(0.26), Y(0.08), s * 0.48, s * 0.26, s * 0.04, c, lw);
    p.stroke_rounded(X(0.06), Y(0.34), s * 0.88, s * 0.38, s * 0.1, c, lw);
    p.stroke_rounded(X(0.26), Y(0.6), s * 0.48, s * 0.32, s * 0.04, c, lw);
    p.circle(X(0.78), Y(0.47), s * 0.045, c);
  } else if (id == "updates") {
    p.line(X(0.5), Y(0.1), X(0.5), Y(0.62), c, lw);
    cairo_new_path(cr);
    cairo_move_to(cr, X(0.28), Y(0.42));
    cairo_line_to(cr, X(0.5), Y(0.64));
    cairo_line_to(cr, X(0.72), Y(0.42));
    stroke();
    p.line(X(0.16), Y(0.86), X(0.84), Y(0.86), c, lw);
  } else if (id == "drivers") {
    p.stroke_rounded(X(0.24), Y(0.24), s * 0.52, s * 0.52, s * 0.08, c, lw);
    for (int i = 0; i < 3; ++i) {
      double k = 0.36 + i * 0.14;
      p.line(X(k), Y(0.08), X(k), Y(0.22), c, lw * 0.8);
      p.line(X(k), Y(0.78), X(k), Y(0.92), c, lw * 0.8);
      p.line(X(0.08), Y(k), X(0.22), Y(k), c, lw * 0.8);
      p.line(X(0.78), Y(k), X(0.92), Y(k), c, lw * 0.8);
    }
  } else if (id == "reports") {
    p.stroke_rounded(X(0.18), Y(0.06), s * 0.64, s * 0.88, s * 0.1, c, lw);
    for (int i = 0; i < 3; ++i) p.line(X(0.32), Y(0.32 + i * 0.18), X(0.68 - (i == 2) * 0.14), Y(0.32 + i * 0.18), c, lw);
  } else if (id == "sysmon") {
    p.stroke_rounded(X(0.06), Y(0.14), s * 0.88, s * 0.72, s * 0.12, c, lw);
    cairo_new_path(cr);
    cairo_move_to(cr, X(0.16), Y(0.6));
    cairo_line_to(cr, X(0.36), Y(0.44));
    cairo_line_to(cr, X(0.52), Y(0.58));
    cairo_line_to(cr, X(0.68), Y(0.3));
    cairo_line_to(cr, X(0.84), Y(0.4));
    stroke();
  } else {
    return false;
  }
  return true;
}

}  // namespace sde
