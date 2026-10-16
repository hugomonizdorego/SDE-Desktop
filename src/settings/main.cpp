// SPDX-License-Identifier: GPL-3.0-or-later
// sde-settings: SDE settings center (Appearance, Panel, Desktop, About).
// Every change is saved immediately to ~/.config/sde/sde.conf and announced to
// the running panel and desktop.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include "buffered_window.h"
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Group.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <sys/utsname.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "build_config.h"
#include "config.h"
#include "icons.h"
#include "paint.h"
#include "theme.h"
#include "util.h"
#include "wm.h"
#include "x11.h"

using namespace sde;

namespace {

Config g_cfg;
Theme g_theme;
sde::BufferedWindow *g_window = nullptr;

void apply_change() {
  g_cfg.save();
  g_theme = Theme::from_config(g_cfg);
  g_theme.apply_to_fltk();
  x11::notify_config_changed();
  if (g_window) g_window->redraw();
}

// ----------------------------------------------------------------- widgets

class Heading : public Fl_Widget {
 public:
  Heading(int x, int y, int w, int h, const char *title, const char *subtitle = nullptr)
      : Fl_Widget(x, y, w, h), title_(title), subtitle_(subtitle ? subtitle : "") {}
  void draw() override {
    text_font(g_theme.font_size + 7, true);
    draw_text(title_, x(), y(), w(), 32, g_theme.text);
    if (!subtitle_.empty()) {
      text_font(g_theme.font_size);
      draw_text(subtitle_, x(), y() + 32, w(), 20, g_theme.text_dim);
    }
  }

 private:
  std::string title_, subtitle_;
};

class Label : public Fl_Widget {
 public:
  Label(int x, int y, int w, int h, const char *text, bool bold = true)
      : Fl_Widget(x, y, w, h), text_(text), bold_(bold) {}
  void draw() override {
    text_font(g_theme.font_size + (bold_ ? 1 : 0), bold_);
    draw_text(text_, x(), y(), w(), h(), bold_ ? g_theme.text : g_theme.text_dim);
  }
  void set(const std::string &t) {
    text_ = t;
    redraw();
  }

 private:
  std::string text_;
  bool bold_;
};

// Rounded card behind a group of options.
class Card : public Fl_Widget {
 public:
  Card(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    Painter p(window()->w(), window()->h());
    p.fill_rounded(x(), y(), w(), h(), 12, g_theme.surface);
    p.stroke_rounded(x(), y(), w(), h(), 12, g_theme.border);
  }
};

// Light/Dark style preview that acts as a radio button.
class StyleCard : public Fl_Widget {
 public:
  StyleCard(int x, int y, int w, int h, bool dark) : Fl_Widget(x, y, w, h), dark_(dark) {}
  void draw() override {
    bool active = g_theme.dark == dark_;
    {
      Painter p(window()->w(), window()->h());
      Color bg = dark_ ? Color::hex(0x161B22) : Color::hex(0xF5F7FA);
      Color surf = dark_ ? Color::hex(0x1F2630) : Color::hex(0xFFFFFF);
      Color line = dark_ ? Color::hex(0x384252) : Color::hex(0xD5DBE3);
      p.fill_rounded(x(), y(), w(), h() - 26, 12, bg);
      p.stroke_rounded(x(), y(), w(), h() - 26, 12, active ? g_theme.accent : g_theme.border,
                       active ? 2.5 : 1);
      p.fill_rounded(x() + 14, y() + 14, w() - 28, h() - 78, 8, surf);
      p.fill_rounded(x() + 24, y() + 26, w() * 0.4, 8, 4, line);
      p.fill_rounded(x() + 24, y() + 42, w() * 0.55, 8, 4, line);
      p.fill_rounded(x() + 24, y() + 58, 36, 12, 6, g_theme.accent);
      p.fill_rect(x() + 1, y() + h() - 26 - 18, w() - 2, 17, surf);
      p.circle(x() + 16, y() + h() - 35, 4, g_theme.accent);
    }
    text_font(g_theme.font_size, active);
    draw_text(dark_ ? "Dark" : "Light", x(), y() + h() - 24, w(), 22,
              active ? g_theme.accent : g_theme.text, 1);
  }
  int handle(int e) override {
    if (e == FL_PUSH) return 1;
    if (e == FL_RELEASE && Fl::event_inside(this)) {
      g_cfg.set("appearance", "theme", dark_ ? "dark" : "light");
      set_openbox_theme(dark_);
      reconfigure_window_manager();
      apply_change();
      return 1;
    }
    return Fl_Widget::handle(e);
  }

 private:
  bool dark_;
};

class AccentPicker : public Fl_Widget {
 public:
  static constexpr int kStep = 30;  // seven swatches fit next to the style cards
  AccentPicker(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    Painter p(window()->w(), window()->h());
    for (int i = 0; i < kAccentChoiceCount; ++i) {
      Color c;
      Color::parse(kAccentChoices[i].hex, c);
      double cx = x() + 15 + i * kStep, cy = y() + h() / 2.0;
      bool active = lower(c.to_hex()) == lower(g_theme.accent.to_hex());
      if (active) p.circle(cx, cy, 14, g_theme.text.alpha(0.85));
      p.circle(cx, cy, active ? 12 : 13, g_theme.surface);
      p.circle(cx, cy, 11, c);
    }
  }
  int handle(int e) override {
    if (e == FL_PUSH) return 1;
    if (e == FL_RELEASE) {
      int i = (Fl::event_x() - x()) / kStep;
      if (i >= 0 && i < kAccentChoiceCount) {
        g_cfg.set("appearance", "accent", kAccentChoices[i].hex);
        apply_change();
      }
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

class WallpaperGrid : public Fl_Widget {
 public:
  WallpaperGrid(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { scan(); }
  ~WallpaperGrid() override {
    for (auto &kv : thumbs_)
      if (kv.second) cairo_surface_destroy(kv.second);
  }
  void scan() {
    files_.clear();
    std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
    std::vector<std::string> dirs{base + "/backgrounds/sde", base + "/backgrounds",
                                  home_dir() + "/Pictures/Wallpapers"};
    for (auto &d : dirs) {
      for (auto &f : list_dir(d)) {
        std::string l = lower(f);
        if (!ends_with(l, ".svg") && !ends_with(l, ".png") && !ends_with(l, ".jpg") &&
            !ends_with(l, ".jpeg"))
          continue;
        std::string full = d + "/" + f;
        if (std::find(files_.begin(), files_.end(), full) == files_.end()) files_.push_back(full);
        if (files_.size() >= 8) return;
      }
    }
  }
  void add(const std::string &path) {
    if (std::find(files_.begin(), files_.end(), path) == files_.end()) {
      files_.insert(files_.begin(), path);
      if (files_.size() > 8) files_.pop_back();
    }
  }
  std::string current() const {
    std::string wp = g_cfg.get("appearance", "wallpaper", "auto");
    if (wp == "auto" || wp.empty()) {
      std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
      return base + "/backgrounds/sde/" + (g_theme.dark ? "selviana-dark.svg" : "selviana-light.svg");
    }
    return wp;
  }
  void draw() override {
    Painter p(window()->w(), window()->h());
    std::string cur = current();
    for (size_t i = 0; i < files_.size(); ++i) {
      int tx = x() + int(i % 4) * (tw_ + 12), ty = y() + int(i / 4) * (th_ + 12);
      cairo_surface_t *&thumb = thumbs_[files_[i]];
      if (!thumb) thumb = load_wallpaper(files_[i], tw_, th_, "zoom", 0.5, 0.5, 0.5);
      cairo_save(p.cr());
      p.rounded_rect(tx, ty, tw_, th_, 8);
      cairo_clip(p.cr());
      cairo_set_source_surface(p.cr(), thumb, tx, ty);
      cairo_paint(p.cr());
      cairo_restore(p.cr());
      if (files_[i] == cur) p.stroke_rounded(tx - 3, ty - 3, tw_ + 6, th_ + 6, 10, g_theme.accent, 3);
    }
  }
  int handle(int e) override {
    if (e == FL_PUSH) return 1;
    if (e == FL_RELEASE) {
      int col = (Fl::event_x() - x()) / (tw_ + 12), row = (Fl::event_y() - y()) / (th_ + 12);
      size_t i = size_t(row * 4 + col);
      if (col >= 0 && col < 4 && row >= 0 && i < files_.size()) {
        g_cfg.set("appearance", "wallpaper", files_[i]);
        apply_change();
      }
      return 1;
    }
    return Fl_Widget::handle(e);
  }

 private:
  std::vector<std::string> files_;
  std::map<std::string, cairo_surface_t *> thumbs_;
  int tw_ = 132, th_ = 80;
};

// Sidebar with page names.
class Sidebar : public Fl_Widget {
 public:
  Sidebar(int x, int y, int w, int h, std::vector<Fl_Group *> pages)
      : Fl_Widget(x, y, w, h), pages_(std::move(pages)) {}
  void select(int i) {
    current_ = i;
    for (size_t p = 0; p < pages_.size(); ++p) {
      if (int(p) == i) pages_[p]->show();
      else pages_[p]->hide();
    }
    window()->redraw();
  }
  void draw() override {
    static const char *icons[][3] = {{"preferences-desktop-theme", "preferences-desktop-wallpaper", "preferences-desktop"},
                                     {"preferences-system-windows", "preferences-desktop", "configure"},
                                     {"user-desktop", "preferences-desktop-wallpaper", "folder"},
                                     {"help-about", "dialog-information", "system-help"}};
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), g_theme.surface);
      p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), g_theme.border);
      p.image(IconLoader::get({"sde-logo", "start-here"}, 36), x() + 20, y() + 20, 36);
      for (size_t i = 0; i < names_.size(); ++i) {
        int ry = y() + 84 + int(i) * 44;
        if (int(i) == current_) {
          p.fill_rounded(x() + 10, ry, w() - 20, 38, 9, g_theme.accent.alpha(0.15));
          p.fill_rounded(x() + 10, ry + 10, 3, 18, 1.5, g_theme.accent);
        } else if (int(i) == hover_) {
          p.fill_rounded(x() + 10, ry, w() - 20, 38, 9, g_theme.hover);
        }
        p.image(IconLoader::get({icons[i][0], icons[i][1], icons[i][2]}, 20), x() + 24, ry + 9, 20);
      }
    }
    text_font(g_theme.font_size + 2, true);
    draw_text("Settings", x() + 64, y() + 22, w() - 70, 32, g_theme.text);
    for (size_t i = 0; i < names_.size(); ++i) {
      int ry = y() + 84 + int(i) * 44;
      text_font(g_theme.font_size + 1, int(i) == current_);
      draw_text(names_[i], x() + 56, ry, w() - 66, 38, int(i) == current_ ? g_theme.accent : g_theme.text);
    }
    text_font(g_theme.font_size - 1);
    draw_text("SDE " SDE_VERSION_LABEL, x() + 20, y() + h() - 36, w() - 30, 20, g_theme.text_dim);
  }
  int handle(int e) override {
    int i = (Fl::event_y() - y() - 84) / 44;
    bool valid = Fl::event_y() >= y() + 84 && i >= 0 && i < int(names_.size());
    if (e == FL_PUSH) {
      if (valid) select(i);
      return 1;
    }
    if (e == FL_MOVE || e == FL_ENTER) {
      int h2 = valid ? i : -1;
      if (h2 != hover_) {
        hover_ = h2;
        redraw();
      }
      return 1;
    }
    if (e == FL_LEAVE) {
      hover_ = -1;
      redraw();
      return 1;
    }
    return Fl_Widget::handle(e);
  }

 private:
  std::vector<Fl_Group *> pages_;
  std::vector<std::string> names_{"Appearance", "Panel", "Desktop", "About"};
  int current_ = 0, hover_ = -1;
};

// The window background follows the theme.
class SettingsWindow : public sde::BufferedWindow {
 public:
  SettingsWindow(int w, int h) : sde::BufferedWindow(w, h, "Settings") {}
  void draw() override {
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), g_theme.background);
    }
    draw_children();
  }
};

// ------------------------------------------------------------------ helpers

Fl_Check_Button *check(int x, int y, int w, const char *label, const char *section, const char *key,
                       bool def) {
  auto *b = new Fl_Check_Button(x, y, w, 28, label);
  b->value(g_cfg.get_bool(section, key, def));
  b->labelsize(g_theme.font_size + 1);
  b->selection_color(g_theme.accent.fl());
  struct Key {
    const char *section, *key;
  };
  b->callback(
      [](Fl_Widget *w, void *d) {
        auto *k = static_cast<Key *>(d);
        g_cfg.set(k->section, k->key, static_cast<Fl_Check_Button *>(w)->value() ? "true" : "false");
        apply_change();
      },
      new Key{section, key});
  return b;
}

std::string read_first(const std::string &path, const std::string &prefix) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
    if (starts_with(line, prefix)) return trim(line.substr(line.find(':') + 1));
  return "";
}

std::string wm_name() {
  Window root = RootWindow(fl_display, fl_screen);
  Window check = x11::get_window(root, "_NET_SUPPORTING_WM_CHECK");
  if (!check) return "none";
  std::string n = x11::get_utf8(check, "_NET_WM_NAME");
  return n.empty() ? "unknown" : n;
}

// ------------------------------------------------------------------- pages

const int kSide = 220, kW = 860, kH = 600;

Fl_Group *appearance_page() {
  int x0 = kSide + 32, cw = kW - kSide - 64;
  auto *g = new Fl_Group(kSide, 0, kW - kSide, kH);
  new Heading(x0, 24, cw, 56, "Appearance", "Style, colors, icons and wallpaper");
  new Card(x0, 92, cw, 182);
  new Label(x0 + 20, 102, 200, 28, "Style");
  new StyleCard(x0 + 20, 134, 150, 124, false);
  new StyleCard(x0 + 186, 134, 150, 124, true);
  new Label(x0 + 360, 102, 200, 28, "Accent color");
  new AccentPicker(x0 + 356, 132, AccentPicker::kStep * kAccentChoiceCount, 40);
  new Label(x0 + 360, 186, 120, 28, "Icon theme");
  auto *icons = new Fl_Choice(x0 + 360, 216, cw - 380, 30);
  icons->add("Automatic");
  auto themes = IconLoader::installed_themes();
  std::string cur = g_cfg.get("appearance", "icon_theme", "auto");
  icons->value(0);
  for (size_t i = 0; i < themes.size(); ++i) {
    // Escape '/' and '&' which have a meaning in FLTK menu labels.
    std::string label;
    for (char c : themes[i]) {
      if (c == '/' || c == '&' || c == '_' || c == '\\') label += '\\';
      label += c;
    }
    icons->add(label.c_str());
    if (themes[i] == cur) icons->value(int(i) + 1);
  }
  static std::vector<std::string> s_themes;
  s_themes = themes;
  icons->callback([](Fl_Widget *w, void *) {
    int v = static_cast<Fl_Choice *>(w)->value();
    g_cfg.set("appearance", "icon_theme", v <= 0 ? "auto" : s_themes[size_t(v - 1)]);
    IconLoader::set_theme(g_cfg.get("appearance", "icon_theme"));
    apply_change();
  });

  new Card(x0, 290, cw, 290);
  new Label(x0 + 20, 300, 200, 28, "Wallpaper");
  auto *grid = new WallpaperGrid(x0 + 20, 336, cw - 40, 184);
  auto *mode = new Fl_Choice(x0 + 120, 536, 150, 30, "Placement ");
  mode->labelsize(g_theme.font_size);
  for (const char *m : {"Zoom", "Fit", "Stretch", "Center", "Tile"}) mode->add(m);
  std::string cm = g_cfg.get("appearance", "wallpaper_mode", "zoom");
  const char *modes[] = {"zoom", "fit", "stretch", "center", "tile"};
  for (int i = 0; i < 5; ++i)
    if (cm == modes[i]) mode->value(i);
  mode->callback([](Fl_Widget *w, void *) {
    const char *modes[] = {"zoom", "fit", "stretch", "center", "tile"};
    g_cfg.set("appearance", "wallpaper_mode", modes[static_cast<Fl_Choice *>(w)->value()]);
    apply_change();
  });
  auto *browse = new Fl_Button(x0 + cw - 150, 536, 130, 30, "Browse...");
  browse->callback(
      [](Fl_Widget *, void *d) {
        const char *f = fl_file_chooser("Choose a wallpaper", "Images (*.{png,jpg,jpeg,svg})",
                                        (home_dir() + "/Pictures").c_str());
        if (!f) return;
        static_cast<WallpaperGrid *>(d)->add(f);
        g_cfg.set("appearance", "wallpaper", f);
        apply_change();
      },
      grid);
  g->end();
  return g;
}

Fl_Group *panel_page() {
  int x0 = kSide + 32, cw = kW - kSide - 64;
  auto *g = new Fl_Group(kSide, 0, kW - kSide, kH);
  new Heading(x0, 24, cw, 56, "Panel", "Size, clock and status area");
  new Card(x0, 92, cw, 96);
  new Label(x0 + 20, 102, 200, 28, "Panel size");
  auto *size = new Fl_Choice(x0 + 20, 138, 220, 30);
  size->add("Small (32 px)");
  size->add("Medium (40 px)");
  size->add("Large (48 px)");
  int h = g_cfg.get_int("panel", "height", 40);
  size->value(h == 32 ? 0 : h == 48 ? 2 : 1);
  size->callback([](Fl_Widget *w, void *) {
    static const char *v[] = {"32", "40", "48"};
    g_cfg.set("panel", "height", v[static_cast<Fl_Choice *>(w)->value()]);
    apply_change();
  });
  new Card(x0, 204, cw, 150);
  new Label(x0 + 20, 214, 200, 28, "Clock and status");
  check(x0 + 20, 246, cw - 40, " Use 24-hour clock", "panel", "clock_24h", true);
  check(x0 + 20, 278, cw - 40, " Show the date", "panel", "show_date", true);
  check(x0 + 20, 310, cw - 40, " Show battery status (laptops)", "panel", "show_battery", true);
  new Card(x0, 370, cw, 92);
  new Label(x0 + 20, 380, cw - 40, 28, "Pinned applications");
  new Label(x0 + 20, 408, cw - 40, 40,
            "Edit the \"pinned\" line in ~/.config/sde/sde.conf (desktop file names).", false);
  g->end();
  return g;
}

Fl_Group *desktop_page() {
  int x0 = kSide + 32, cw = kW - kSide - 64;
  auto *g = new Fl_Group(kSide, 0, kW - kSide, kH);
  new Heading(x0, 24, cw, 56, "Desktop", "Icons shown on the desktop");
  new Card(x0, 92, cw, 150);
  new Label(x0 + 20, 102, 200, 28, "Desktop icons");
  check(x0 + 20, 134, cw - 40, " Show files from the Desktop folder", "desktop", "show_icons", true);
  check(x0 + 20, 166, cw - 40, " Show Home", "desktop", "show_home", true);
  check(x0 + 20, 198, cw - 40, " Show Trash", "desktop", "show_trash", false);
  g->end();
  return g;
}

class AboutLogo : public Fl_Widget {
 public:
  AboutLogo(int x, int y, int s) : Fl_Widget(x, y, s, s) {}
  void draw() override {
    Painter p(window()->w(), window()->h());
    p.image(IconLoader::get({"sde-logo", "start-here"}, w()), x(), y(), w());
  }
};

Fl_Group *about_page() {
  int x0 = kSide + 32, cw = kW - kSide - 64;
  auto *g = new Fl_Group(kSide, 0, kW - kSide, kH);
  new Card(x0, 24, cw, 150);
  new AboutLogo(x0 + 24, 51, 96);
  auto *title = new Heading(x0 + 144, 54, cw - 160, 60, "Selviana Desktop Environment",
                            "Small and fast, with a modern desktop experience.");
  (void)title;
  new Label(x0 + 144, 120, cw - 160, 24, "Version " SDE_VERSION_LABEL " (" SDE_VERSION ")", false);

  struct utsname u;
  uname(&u);
  std::string mem = read_first("/proc/meminfo", "MemTotal");
  if (!mem.empty()) {
    double kb = atof(mem.c_str());
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f GiB", kb / 1024.0 / 1024.0);
    mem = buf;
  }
  std::string cpu = read_first("/proc/cpuinfo", "model name");
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, 0);
  std::vector<std::pair<std::string, std::string>> rows = {
      {"Operating system", os_release("PRETTY_NAME")},
      {"Kernel", std::string(u.sysname) + " " + u.release},
      {"Processor", cpu.empty() ? u.machine : cpu},
      {"Memory", mem},
      {"Display", std::to_string(sw) + " x " + std::to_string(sh) + " (" +
                      std::to_string(Fl::screen_count()) + " screen" + (Fl::screen_count() > 1 ? "s)" : ")")},
      {"Window manager", wm_name()},
      {"Toolkit", "FLTK " SDE_FLTK_VERSION},
  };
  new Card(x0, 190, cw, 36 + int(rows.size()) * 30);
  int y = 206;
  for (auto &r : rows) {
    auto *k = new Label(x0 + 24, y, 170, 30, strdup(r.first.c_str()), false);
    (void)k;
    new Label(x0 + 200, y, cw - 224, 30, strdup(r.second.c_str()), true);
    y += 30;
  }
  new Label(x0, y + 34, cw, 24,
            "Free software under the GNU General Public License, version 3 or later.", false);
  new Label(x0, y + 58, cw, 24,
            "Copyright (C) 2026 Hugo Moniz do Rego and the SDE contributors.", false);
  g->end();
  return g;
}

}  // namespace

int main(int argc, char **argv) {
  std::string page = "appearance";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--page" && i + 1 < argc) {
      page = argv[++i];
    } else if (a == "--version") {
      printf("sde-settings %s (%s)\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    } else {
      printf("Usage: sde-settings [--page appearance|panel|desktop|about] [--version]\n");
      return a == "--help" ? 0 : 2;
    }
  }
  fl_open_display();
  g_cfg.load();
  g_theme = Theme::from_config(g_cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(g_cfg.get("appearance", "icon_theme", "auto"));

  auto *win = new SettingsWindow(kW, kH);
  g_window = win;
  std::vector<Fl_Group *> pages{appearance_page(), panel_page(), desktop_page(), about_page()};
  auto *side = new Sidebar(0, 0, kSide, kH, pages);
  win->end();
  win->size_range(kW, kH, kW, kH);
  win->xclass("sde-settings");
  static const std::map<std::string, int> index = {{"appearance", 0}, {"panel", 1}, {"desktop", 2}, {"about", 3}};
  auto it = index.find(page);
  side->select(it == index.end() ? 0 : it->second);
  win->show();
  return Fl::run();
}
