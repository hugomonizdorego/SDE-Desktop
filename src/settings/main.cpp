// SPDX-License-Identifier: GPL-3.0-or-later
// sde-settings: the SDE settings center.
//
// Pages: Appearance, Panel, Desktop, Notifications, Displays, Keyboard &
// Mouse and About. Every change is saved immediately to
// ~/.config/sde/sde.conf and announced to the running desktop.
//
// "sde-settings --apply" applies the saved display, keyboard and pointer
// settings without opening a window (run by sde-session at login).
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "dbus.h"
#include "effects.h"
#include "desktop_entry.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "integration.h"
#include "paint.h"
#include "settings.h"
#include "spawn.h"
#include "suite.h"
#include "system.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "wm.h"
#include "x11.h"

namespace sde {

namespace {

Config g_cfg;
Theme g_theme;
BufferedWindow *g_window = nullptr;
std::string g_argv0;
std::string g_restart_page = "appearance";

}  // namespace

Config &settings_config() { return g_cfg; }
const Theme &settings_theme() { return g_theme; }

void apply_change() {
  double old_scale = g_theme.scale;
  g_cfg.save();
  sync_gtk_settings(g_cfg);
  g_theme = Theme::from_config(g_cfg);
  g_theme.apply_to_fltk();
  if (sync_openbox(g_cfg)) reconfigure_window_manager();
  x11::notify_config_changed();
  if (std::fabs(g_theme.scale - old_scale) > 0.01) {
    // Every size depends on the scale: start again with the new one.
    Fl::add_timeout(0.2, [](void *) {
      execl("/proc/self/exe", g_argv0.c_str(), "--page", g_restart_page.c_str(),
            static_cast<char *>(nullptr));
    });
  }
  if (g_window) g_window->redraw();
}

namespace {

// ------------------------------------------------------- custom widgets

void aero_changed();

// Theme preview that acts as a radio button: SDE Light, SDE Dark, SDE Aero
// and SDE Liquid Glass (Aero and Liquid Glass follow the dark-mode switch).
class ThemeCard : public Fl_Widget {
 public:
  ThemeCard(int x, int y, int w, int h, std::string id) : Fl_Widget(x, y, w, h), id_(std::move(id)) {}
  bool active() const {
    std::string style = theme_style(g_cfg);
    if (id_ == "light") return style == "sde" && !g_theme.dark;
    if (id_ == "dark") return style == "sde" && g_theme.dark;
    return style == id_;
  }
  void draw() override {
    bool on = active();
    bool dark = id_ == "dark" || ((id_ == "aero" || id_ == "liquid-glass") && g_theme.dark);
    int pv = h() - S(26);
    {
      Painter p(window()->w(), window()->h());
      Color bg = dark ? Color::hex(0x161B22) : Color::hex(0xF5F7FA);
      Color surf = dark ? Color::hex(0x1F2630) : Color::hex(0xFFFFFF);
      Color line = dark ? Color::hex(0x384252) : Color::hex(0xD5DBE3);
      if (id_ == "liquid-glass") {
        // A colorful backdrop seen through frosted layers.
        p.fill_gradient(x(), y(), w(), pv, Sd(12), Color::hex(dark ? 0x23305A : 0x8EC5FC),
                        Color::hex(dark ? 0x4A2B55 : 0xE0C3FC), true);
        surf = dark ? Color::hex(0xFFFFFF, 0.10) : Color::hex(0xFFFFFF, 0.55);
        line = dark ? Color::hex(0xFFFFFF, 0.25) : Color::hex(0xFFFFFF, 0.9);
      } else {
        p.fill_rounded(x(), y(), w(), pv, Sd(12), bg);
      }
      p.stroke_rounded(x(), y(), w(), pv, Sd(12), on ? g_theme.accent : g_theme.border,
                       on ? Sd(2.5) : Sd(1));
      double wx = x() + Sd(12), wy = y() + Sd(12), ww = w() - Sd(24), wh = pv - Sd(24);
      double r = id_ == "liquid-glass" ? Sd(12) : Sd(6);
      p.fill_rounded(wx, wy, ww, wh, r, surf);
      if (id_ == "aero") {
        p.fill_gradient(wx, wy, ww, Sd(18), Sd(6), dark ? Color::hex(0x354152) : Color::hex(0xFBFDFF),
                        dark ? Color::hex(0x232B38) : Color::hex(0xD2E2F3));
        p.glass_sheen(wx, wy, ww, Sd(18), Sd(6), dark);
      } else if (id_ == "liquid-glass") {
        p.stroke_rounded(wx, wy, ww, wh, r, line, Sd(1));
      } else {
        p.line(wx, wy + Sd(18), wx + ww, wy + Sd(18), line);
      }
      p.fill_rounded(wx + Sd(10), wy + Sd(28), ww * 0.5, Sd(7), Sd(3.5), line);
      p.fill_rounded(wx + Sd(10), wy + Sd(42), ww * 0.65, Sd(7), Sd(3.5), line);
      double br = id_ == "liquid-glass" ? Sd(7) : Sd(4);
      p.fill_rounded(wx + Sd(10), wy + wh - Sd(22), Sd(40), Sd(14), br, g_theme.accent);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), pv, Sd(12));
    }
    static const std::pair<const char *, const char *> names[] = {
        {"light", N_("Light")}, {"dark", N_("Dark")}, {"aero", N_("Aero")},
        {"liquid-glass", N_("Liquid Glass")}};
    const char *label = "";
    for (auto &n : names)
      if (id_ == n.first) label = _(n.second);
    text_font(g_theme.font_size, on);
    draw_text(label, x(), y() + pv + S(2), w(), S(22), on ? g_theme.accent_text : g_theme.text, 1);
  }
  void choose() {
    if (id_ == "light" || id_ == "dark") {
      g_cfg.set("appearance", "style", "sde");
      g_cfg.set("appearance", "theme", id_);
    } else {
      g_cfg.set("appearance", "style", id_);
    }
    g_theme = Theme::from_config(g_cfg);
    set_openbox_theme(g_theme.dark, theme_style(g_cfg));
    reconfigure_window_manager();
    aero_changed();
    if (window()) window()->redraw();
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        return 1;
      case FL_RELEASE:
        if (Fl::event_inside(this)) choose();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          choose();
          return 1;
        }
        return 0;
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  std::string id_;
};

class AccentPicker : public Fl_Widget {
 public:
  AccentPicker(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int step() const { return std::min(S(34), w() / kAccentChoiceCount); }
  void draw() override {
    Painter p(window()->w(), window()->h());
    int st = step();
    for (int i = 0; i < kAccentChoiceCount; ++i) {
      Color c;
      Color::parse(kAccentChoices[i].hex, c);
      double cx = x() + st / 2.0 + i * st, cy = y() + h() / 2.0;
      bool active = lower(c.to_hex()) == lower(g_theme.accent.to_hex());
      double r = st * 0.36;
      if (active) p.circle(cx, cy, r + Sd(3), g_theme.text.alpha(0.85));
      p.circle(cx, cy, active ? r + Sd(1) : r + Sd(2), g_theme.surface);
      p.circle(cx, cy, r, c);
      if (Fl::focus() == this && i == focus_) {
        p.set(g_theme.accent);
        cairo_set_line_width(p.cr(), Sd(2));
        cairo_new_sub_path(p.cr());
        cairo_arc(p.cr(), cx, cy, r + Sd(6), 0, 2 * M_PI);
        cairo_stroke(p.cr());
      }
    }
  }
  void pick(int i) {
    if (i < 0 || i >= kAccentChoiceCount) return;
    g_cfg.set("appearance", "accent", kAccentChoices[i].hex);
    apply_change();
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        return 1;
      case FL_RELEASE:
        pick((Fl::event_x() - x()) / step());
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k == FL_Left && focus_ > 0)
          --focus_;
        else if (k == FL_Right && focus_ + 1 < kAccentChoiceCount)
          ++focus_;
        else if (k == ' ' || k == FL_Enter)
          pick(focus_);
        else
          return 0;
        redraw();
        return 1;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  int focus_ = 0;
};

std::string default_wallpaper() {
  std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
  return base + "/backgrounds/sde/" + (g_theme.dark ? "selviana-dark.svg" : "selviana-light.svg");
}

class WallpaperGrid : public Fl_Widget {
 public:
  WallpaperGrid(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    tw_ = S(132);
    th_ = S(80);
    cols_ = std::max(1, (w + S(12)) / (tw_ + S(12)));
    scan();
    size(w, rows() * (th_ + S(12)));
  }
  ~WallpaperGrid() override {
    for (auto &kv : thumbs_)
      if (kv.second) cairo_surface_destroy(kv.second);
  }
  int rows() const { return std::max(1, (int(files_.size()) + cols_ - 1) / cols_); }
  void scan() {
    files_.clear();
    std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
    std::vector<std::string> dirs{base + "/backgrounds/sde", base + "/backgrounds",
                                  home_dir() + "/Pictures/Wallpapers"};
    std::string current = g_cfg.get("appearance", "wallpaper", "auto");
    if (current != "auto" && file_exists(current)) files_.push_back(current);
    for (auto &d : dirs) {
      for (auto &f : list_dir(d)) {
        std::string l = lower(f);
        if (!ends_with(l, ".svg") && !ends_with(l, ".png") && !ends_with(l, ".jpg") &&
            !ends_with(l, ".jpeg"))
          continue;
        std::string full = d + "/" + f;
        if (std::find(files_.begin(), files_.end(), full) == files_.end()) files_.push_back(full);
        if (int(files_.size()) >= cols_ * 2) return;
      }
    }
  }
  std::string current() const {
    std::string wp = g_cfg.get("appearance", "wallpaper", "auto");
    return wp == "auto" || wp.empty() ? default_wallpaper() : wp;
  }
  void draw() override {
    Painter p(window()->w(), window()->h());
    std::string cur = current();
    for (size_t i = 0; i < files_.size(); ++i) {
      int tx = x() + int(i % size_t(cols_)) * (tw_ + S(12)),
          ty = y() + int(i / size_t(cols_)) * (th_ + S(12));
      cairo_surface_t *&thumb = thumbs_[files_[i]];
      if (!thumb) thumb = load_wallpaper(files_[i], tw_, th_, "zoom", 0.5, 0.5, 0.5);
      cairo_save(p.cr());
      p.rounded_rect(tx, ty, tw_, th_, Sd(8));
      cairo_clip(p.cr());
      if (thumb) {
        cairo_set_source_surface(p.cr(), thumb, tx, ty);
        cairo_paint(p.cr());
      }
      cairo_restore(p.cr());
      if (files_[i] == cur)
        p.stroke_rounded(tx - Sd(3), ty - Sd(3), tw_ + Sd(6), th_ + Sd(6), Sd(10), g_theme.accent,
                         Sd(3));
      if (Fl::focus() == this && int(i) == focus_) focus_ring(p, tx, ty, tw_, th_, Sd(8));
    }
  }
  void pick(int i) {
    if (i < 0 || i >= int(files_.size())) return;
    g_cfg.set("appearance", "wallpaper", files_[size_t(i)]);
    apply_change();
  }
  void add(const std::string &path) {
    if (std::find(files_.begin(), files_.end(), path) == files_.end()) {
      files_.insert(files_.begin(), path);
      if (int(files_.size()) > cols_ * 2) files_.pop_back();
    }
    redraw();
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        return 1;
      case FL_RELEASE: {
        int col = (Fl::event_x() - x()) / (tw_ + S(12)),
            row = (Fl::event_y() - y()) / (th_ + S(12));
        if (col >= 0 && col < cols_ && row >= 0) pick(row * cols_ + col);
        return 1;
      }
      case FL_FOCUS:
      case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key(), n = int(files_.size());
        if (k == FL_Left && focus_ > 0)
          --focus_;
        else if (k == FL_Right && focus_ + 1 < n)
          ++focus_;
        else if (k == ' ' || k == FL_Enter)
          pick(focus_);
        else
          return 0;
        redraw();
        return 1;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  std::vector<std::string> files_;
  std::map<std::string, cairo_surface_t *> thumbs_;
  int tw_, th_, cols_, focus_ = 0;
};

class ImageBox : public Fl_Widget {
 public:
  ImageBox(int x, int y, int s, std::vector<std::string> names)
      : Fl_Widget(x, y, s, s), names_(std::move(names)) {}
  void draw() override {
    Painter p(window()->w(), window()->h());
    p.image(IconLoader::get(names_, w()), x(), y(), w());
  }

 private:
  std::vector<std::string> names_;
};

// ------------------------------------------------------------------ pages

struct PageInfo {
  const char *id;
  const char *title;
  std::vector<std::string> icons;
  void (*build)(Page *);
};

const std::vector<PageInfo> &pages();
void show_page(int index);
void open_page(const char *id) {
  for (size_t i = 0; i < pages().size(); ++i)
    if (std::string(pages()[i].id) == id) {
      show_page(int(i));
      return;
    }
}

// Native category symbols remain distinct even without an optional icon theme.
void settings_glyph(Painter &p, const std::string &id, double x, double y, double size,
                    const Color &c) {
  if (id == "appearance") {
    glyph::sun(p, x, y, size, c);
    return;
  }
  if (id == "action-center") {
    glyph::gear(p, x, y, size, c);
    return;
  }
  if (id == "notifications") {
    glyph::bell(p, x, y, size, false, c);
    return;
  }
  if (id == "system") {
    glyph::power(p, x, y, size, c);
    return;
  }
  if (id == "effects") {
    // Four-point sparkle.
    cairo_t *cr = p.cr();
    double cx = x + size * .45, cy = y + size * .5, r = size * .42, k = size * .1;
    cairo_move_to(cr, cx, cy - r);
    cairo_curve_to(cr, cx + k, cy - k, cx + k, cy - k, cx + r, cy);
    cairo_curve_to(cr, cx + k, cy + k, cx + k, cy + k, cx, cy + r);
    cairo_curve_to(cr, cx - k, cy + k, cx - k, cy + k, cx - r, cy);
    cairo_curve_to(cr, cx - k, cy - k, cx - k, cy - k, cx, cy - r);
    p.set(c);
    cairo_fill(cr);
    p.circle(x + size * .86, y + size * .16, size * .08, c);
    return;
  }
  if (id == "about") {
    p.stroke_rounded(x, y, size, size, size / 2, c, size * .08);
    p.circle(x + size * .5, y + size * .3, size * .055, c);
    p.line(x + size * .5, y + size * .46, x + size * .5, y + size * .74, c, size * .09);
    return;
  }
  if (id == "input") {
    p.stroke_rounded(x, y + size * .18, size, size * .65, size * .1, c, size * .07);
    for (int row = 0; row < 2; ++row)
      for (int col = 0; col < 4; ++col)
        p.fill_rounded(x + size * (.14 + col * .19), y + size * (.32 + row * .18), size * .09,
                       size * .08, size * .02, c);
    p.line(x + size * .3, y + size * .72, x + size * .7, y + size * .72, c, size * .06);
    return;
  }
  p.stroke_rounded(x, y + size * .1, size, size * .65, size * .08, c, size * .075);
  if (id == "panel") {
    p.fill_rounded(x + size * .1, y + size * .57, size * .8, size * .1, size * .03, c);
  } else if (id == "home" || id == "suite") {
    for (int i = 0; i < 4; ++i)
      p.fill_rounded(x + size * (.17 + (i % 2) * .38), y + size * (.23 + (i / 2) * .24), size * .25,
                     size * .13, size * .03, c);
  } else {
    p.line(x + size * .5, y + size * .75, x + size * .5, y + size * .96, c, size * .075);
    p.line(x + size * .28, y + size * .96, x + size * .72, y + size * .96, c, size * .075);
  }
}

class SettingsLink : public DrawnButton {
 public:
  SettingsLink(int x, int y, int w, const std::string &title, const std::string &hint,
               std::vector<std::string> icons, const char *page)
      : DrawnButton(x, y, w, S(100), nullptr),
        title_(title),
        hint_(hint),
        icons_(std::move(icons)),
        page_(page) {
    callback([](Fl_Widget *w, void *) { open_page(static_cast<SettingsLink *>(w)->page_); });
    copy_tooltip(title.c_str());
  }
  void draw() override {
    bool hot = Fl::belowmouse() == this;
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(14), hot ? g_theme.hover : g_theme.surface);
      p.stroke_rounded(x(), y(), w(), h(), S(14), hot ? g_theme.accent : g_theme.border);
      p.fill_rounded(x() + S(14), y() + S(16), S(36), S(36), S(10), g_theme.accent.alpha(.12));
      settings_glyph(p, page_, x() + S(20), y() + S(22), S(24), g_theme.accent_text);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), S(14));
    }
    text_font(g_theme.font_size + 1, true);
    draw_text(title_, x() + S(62), y() + S(15), w() - S(76), S(28), g_theme.text);
    text_font(g_theme.font_size - 1);
    draw_text(hint_, x() + S(16), y() + S(64), w() - S(32), S(22), g_theme.text_dim);
  }

 private:
  std::string title_, hint_;
  std::vector<std::string> icons_;
  const char *page_;
};

class PanelStyleCard : public Fl_Widget {
 public:
  PanelStyleCard(int x, int y, int w, int h, std::string style, std::string title)
      : Fl_Widget(x, y, w, h), style_(std::move(style)), title_(std::move(title)) {}
  void draw() override {
    bool selected = g_cfg.get("panel", "style", "full") == style_;
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h() - S(25), S(10), g_theme.background);
      p.stroke_rounded(x(), y(), w(), h() - S(25), S(10),
                       selected ? g_theme.accent : g_theme.border, selected ? S(2) : S(1));
      p.fill_rounded(x() + S(12), y() + S(12), w() * .5, S(20), S(4), g_theme.surface);
      int length = style_ == "full" ? w() - S(4) : style_ == "short" ? w() * 2 / 3 : w() - S(22);
      int bx = x() + (w() - length) / 2, by = y() + h() - S(47);
      p.fill_rounded(bx, by, length, S(14), style_ == "full" ? 0 : S(5), g_theme.surface);
      p.fill_rounded(bx + S(4), by + S(3), S(18), S(8), S(3), g_theme.accent);
      for (int i = 0; i < 3; ++i) p.circle(bx + S(30 + i * 10), by + S(7), S(2), g_theme.text_dim);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h() - S(25), S(10));
    }
    text_font(g_theme.font_size, selected);
    draw_text(title_, x(), y() + h() - S(24), w(), S(22),
              selected ? g_theme.accent_text : g_theme.text, 1);
  }
  void choose() {
    g_cfg.set("panel", "style", style_);
    apply_change();
    Fl::add_timeout(0, [](void *) { refresh_page(); });
  }
  int handle(int e) override {
    if (e == FL_PUSH) {
      take_focus();
      return 1;
    }
    if (e == FL_RELEASE) {
      if (Fl::event_inside(this)) choose();
      return 1;
    }
    if (e == FL_FOCUS || e == FL_UNFOCUS) {
      redraw();
      return 1;
    }
    if (e == FL_KEYBOARD && (Fl::event_key() == FL_Enter || Fl::event_key() == ' ')) {
      choose();
      return 1;
    }
    return Fl_Widget::handle(e);
  }

 private:
  std::string style_, title_;
};

void build_home(Page *page) {
  page->heading(_("System Settings"), _("Personalize your desktop and manage your devices"));
  page->begin_card();
  int x = page->left() + S(20), y = page->cursor();
  new ImageBox(x, y + S(8), S(46), {"computer", "sde-logo"});
  new Label(x + S(62), y, page->width() - S(102), S(30), user_name(), Label::BOLD);
  new Label(x + S(62), y + S(30), page->width() - S(102), S(24), os_release("PRETTY_NAME"),
            Label::DIM);
  page->advance(S(66));
  page->end_card();
  int gap = S(12), width = (page->width() - gap) / 2;
  struct Link {
    const char *id, *title, *hint, *icon;
  };
  const Link links[] = {
      {"appearance", N_("Appearance"), N_("Themes, colors and wallpaper"),
       "preferences-desktop-theme"},
      {"panel", N_("Panel"), N_("Full, floating or short"), "preferences-system-windows"},
      {"action-center", N_("Action Center"), N_("Clock, greeting and quick controls"),
       "preferences-system"},
      {"display", N_("Displays"), N_("Resolution and arrangement"), "video-display"},
      {"input", N_("Keyboard & Mouse"), N_("Layouts, pointer and touchpad"), "input-keyboard"},
      {"notifications", N_("Notifications"), N_("Alerts and Do Not Disturb"),
       "preferences-desktop-notification"},
      {"suite", N_("Applications"), N_("Applications and source credits"), "applications-other"},
      {"system", N_("Power & Updates"), N_("Power, packages and processes"),
       "utilities-system-monitor"},
  };
  y = page->cursor();
  for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); ++i)
    new SettingsLink(page->left() + int(i % 2) * (width + gap), y + int(i / 2) * S(112), width,
                     _(links[i].title), _(links[i].hint), {links[i].icon, "preferences-system"},
                     links[i].id);
  page->advance(S(448));
  page->note(_("Changes are saved automatically."));
  page->finish();
}

void reload_compositor_later(void *) {
  write_compositor_config(g_cfg);
  write_compositor_config(g_cfg, true);
  reload_compositor();
}
void aero_changed();

void build_appearance(Page *page) {
  page->heading(_("Appearance"), _("Theme, colors, glass, size and wallpaper"));
  page->begin_card(_("Theme"));
  int gap = S(12), cw = std::min(S(150), (page->width() - S(40) - 3 * gap) / 4);
  int y = page->cursor();
  const char *ids[] = {"light", "dark", "aero", "liquid-glass"};
  for (int i = 0; i < 4; ++i) new ThemeCard(page->left() + S(20) + i * (cw + gap), y, cw, S(118), ids[i]);
  page->advance(S(126));
  page->note(_("Themes are installed in /usr/share/themes (SDE, SDE-Dark, SDE-Aero, SDE-Liquid-Glass "
               "and their dark versions) and work in GTK 3 and GTK 4 applications."));
  page->switch_row(
      _("Dark mode"), g_theme.dark,
      [](bool on) {
        g_cfg.set("appearance", "theme", on ? "dark" : "light");
        g_theme = Theme::from_config(g_cfg);
        set_openbox_theme(on, theme_style(g_cfg));
        reconfigure_window_manager();
        aero_changed();
      },
      _("Aero and Liquid Glass have light and dark versions."));
  page->row(_("Accent color"), S(34) * kAccentChoiceCount, S(36),
            [](int x, int yy, int w, int h) { return new AccentPicker(x, yy, w, h); });
  page->end_card();

  page->begin_card(_("Glass effects"));
  page->switch_row(
      _("Glass effects"), g_cfg.get_bool("aero", "enabled", true),
      [](bool on) {
        g_cfg.set("aero", "enabled", on ? "true" : "false");
        aero_changed();
      },
      _("Translucent panel, menus and window frames with Aero and Liquid Glass. SDE Light and "
        "Dark stay opaque."));
  page->slider_row(
      _("Transparency"), 0, 60, g_cfg.get_int("aero", "transparency", 18),
      [](double v) {
        g_cfg.set("aero", "transparency", std::to_string(int(std::lround(v))));
        aero_changed();
      },
      _("0% is opaque. Text stays readable: menus use half of this value."));
  page->switch_row(
      _("Blur behind glass"), g_cfg.get_bool("aero", "blur", true),
      [](bool on) {
        g_cfg.set("aero", "blur", on ? "true" : "false");
        aero_changed();
      },
      _("Uses OpenGL when available; older graphics fall back to a lighter blur."));
  page->slider_row(
      _("Blur strength"), 1, 12, g_cfg.get_int("aero", "blur_strength", 6),
      [](double v) {
        g_cfg.set("aero", "blur_strength", std::to_string(int(std::lround(v))));
        aero_changed();
      });
  page->switch_row(
      _("Rounded window corners"), g_cfg.get_bool("aero", "rounded_corners", true),
      [](bool on) {
        g_cfg.set("aero", "rounded_corners", on ? "true" : "false");
        aero_changed();
      });
  page->switch_row(
      _("Smooth animations"), g_cfg.get_bool("aero", "animations", true),
      [](bool on) {
        g_cfg.set("aero", "animations", on ? "true" : "false");
        aero_changed();
      },
      _("Fade windows and menus in and out."));
  if (g_cfg.get("compositor", "mode", "auto") == "off")
    page->note(_("Compositing is off (Power & Updates), so glass effects are not shown."));
  else if (file_exists(config_home() + "/sde/compositor.conf"))
    page->note(_("~/.config/sde/compositor.conf exists, so it is used instead of these settings."));
  page->end_card();

  page->begin_card(_("Size and text"));
  std::string sc = g_cfg.get("appearance", "scale", "auto");
  std::vector<std::string> scales = {_("Automatic"), "100 %", "125 %", "150 %", "175 %", "200 %"};
  int sc_i = sc == "auto" ? 0 : std::max(0, std::min(5, (atoi(sc.c_str()) - 100) / 25 + 1));
  page->choice_row(
      _("Interface size"), scales, sc_i,
      [](int v) {
        static const char *vals[] = {"auto", "100", "125", "150", "175", "200"};
        if (v >= 0 && v < 6) g_cfg.set("appearance", "scale", vals[v]);
        apply_change();
      },
      _("Larger sizes help on high-resolution screens. Automatic follows the screen's DPI."));
  int pt = g_cfg.get_int("appearance", "font_size", 10);
  std::vector<std::string> sizes;
  for (int s = 8; s <= 14; ++s) sizes.push_back(format(_("%d pt"), s));
  page->choice_row(_("Text size"), sizes, std::max(0, std::min(6, pt - 8)), [](int v) {
    g_cfg.set("appearance", "font_size", std::to_string(v + 8));
    apply_change();
  });
  static std::vector<std::string> s_themes;
  s_themes = IconLoader::installed_themes();
  std::vector<std::string> theme_names{_("Automatic")};
  std::string cur_theme = g_cfg.get("appearance", "icon_theme", "auto");
  int theme_i = 0;
  for (size_t i = 0; i < s_themes.size(); ++i) {
    theme_names.push_back(s_themes[i]);
    if (s_themes[i] == cur_theme) theme_i = int(i) + 1;
  }
  page->choice_row(_("Icon theme"), theme_names, theme_i, [](int v) {
    g_cfg.set("appearance", "icon_theme", v <= 0 ? "auto" : s_themes[size_t(v - 1)]);
    IconLoader::set_theme(g_cfg.get("appearance", "icon_theme"));
    IconLoader::clear_cache();
    apply_change();
  });
  page->end_card();

  page->begin_card(_("Wallpaper"));
  auto *grid =
      new WallpaperGrid(page->left() + S(20), page->cursor(), page->width() - S(40), S(80));
  page->advance(grid->h() + S(8));
  std::string cm = g_cfg.get("appearance", "wallpaper_mode", "zoom");
  static const char *modes[] = {"zoom", "fit", "stretch", "center", "tile"};
  int mode_i = 0;
  for (int i = 0; i < 5; ++i)
    if (cm == modes[i]) mode_i = i;
  page->choice_row(_("Placement"), {_("Zoom"), _("Fit"), _("Stretch"), _("Center"), _("Tile")},
                   mode_i, [](int v) {
                     if (v >= 0 && v < 5) g_cfg.set("appearance", "wallpaper_mode", modes[v]);
                     apply_change();
                   });
  page->row(_("Your own picture"), S(160), S(34), [grid](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Browse..."));
    b->callback(
        [](Fl_Widget *, void *d) {
          const char *f = fl_file_chooser(_("Choose a wallpaper"), "Images (*.{png,jpg,jpeg,svg})",
                                          (home_dir() + "/Pictures").c_str());
          if (!f) return;
          static_cast<WallpaperGrid *>(d)->add(f);
          g_cfg.set("appearance", "wallpaper", f);
          apply_change();
        },
        grid);
    return b;
  });
  page->end_card();
  page->finish();
}

// Saves, then regenerates the compositor configuration. Slider drags call
// this often, so the compositor reloads once the value settles.
void aero_changed() {
  apply_change();
  Fl::remove_timeout(reload_compositor_later);
  Fl::add_timeout(0.35, reload_compositor_later);
}

void set_bool(const char *section, const char *key, bool on) {
  g_cfg.set(section, key, on ? "true" : "false");
  apply_change();
}

void build_panel(Page *page) {
  page->heading(_("Panel"), _("Position, size, clock and pinned applications"));
  page->begin_card(_("Panel style"));
  int preview_w = (page->width() - S(56)) / 3;
  int preview_y = page->cursor();
  new PanelStyleCard(page->left() + S(16), preview_y, preview_w, S(104), "full", _("Full edge"));
  new PanelStyleCard(page->left() + S(28) + preview_w, preview_y, preview_w, S(104), "floating",
                     _("Floating"));
  new PanelStyleCard(page->left() + S(40) + 2 * preview_w, preview_y, preview_w, S(104), "short",
                     _("Short"));
  page->advance(S(112));
  page->end_card();
  page->begin_card(_("Layout"));
  page->choice_row(_("Position"), {_("Bottom"), _("Top")},
                   lower(g_cfg.get("panel", "position", "bottom")) == "top" ? 1 : 0, [](int v) {
                     g_cfg.set("panel", "position", v == 1 ? "top" : "bottom");
                     apply_change();
                   });
  int h = g_cfg.get_int("panel", "height", 40);
  page->choice_row(_("Size"), {_("Small"), _("Medium"), _("Large")},
                   h == 32   ? 0
                   : h == 48 ? 2
                             : 1,
                   [](int v) {
                     static const char *vals[] = {"32", "40", "48"};
                     if (v >= 0 && v < 3) g_cfg.set("panel", "height", vals[v]);
                     apply_change();
                   });
  auto *panel_width = page->slider_row(
      _("Panel width"), 65, 100, g_cfg.get_int("panel", "width_percent", 96),
      [](double v) {
        g_cfg.set("panel", "width_percent", std::to_string(int(std::lround(v))));
        apply_change();
      },
      _("Floating panels follow this percentage. Short panels use up to 72%."));
  if (g_cfg.get("panel", "style", "full") == "full") panel_width->deactivate();
  page->switch_row(
      _("Hide automatically"), g_cfg.get_bool("panel", "autohide", false),
      [](bool on) { set_bool("panel", "autohide", on); },
      _("The panel appears when the pointer touches the screen edge."));
  page->end_card();

  page->begin_card(_("Clock and status"));
  page->switch_row(_("24-hour clock"), g_cfg.get_bool("panel", "clock_24h", true),
                   [](bool on) { set_bool("panel", "clock_24h", on); });
  page->switch_row(_("Show the date"), g_cfg.get_bool("panel", "show_date", true),
                   [](bool on) { set_bool("panel", "show_date", on); });
  page->switch_row(
      _("Show battery status"), g_cfg.get_bool("panel", "show_battery", true),
      [](bool on) { set_bool("panel", "show_battery", on); }, _("On laptops and tablets."));
  for (const auto &item : std::vector<std::pair<std::string, std::string>>{
           {"show_keyboard", _("Keyboard indicator")},
           {"show_bluetooth", _("Bluetooth indicator")},
           {"show_power", _("Power button")},
           {"show_media", _("Media player button")},
           {"show_network", _("Network indicator")},
           {"show_sound", _("Sound indicator")},
           {"show_tray", _("System tray button")}})
    page->switch_row(item.second, g_cfg.get_bool("panel", item.first, true),
                     [key = item.first](bool on) { set_bool("panel", key.c_str(), on); });
  page->switch_row(
      _("Hide duplicate tray icons"), g_cfg.get_bool("panel", "hide_duplicate_tray", true),
      [](bool on) { set_bool("panel", "hide_duplicate_tray", on); },
      _("nm-applet, Blueman and power applets keep running; SDE's indicators show their state."));
  page->end_card();

  page->begin_card(_("Pinned applications"));
  std::vector<DesktopEntry> pins;
  for (auto &id : split(g_cfg.get("panel", "pinned"), ';')) {
    DesktopEntry e;
    if (find_application(trim(id), e)) pins.push_back(e);
  }
  if (pins.empty()) page->note(_("No applications are pinned."));
  for (size_t i = 0; i < pins.size(); ++i) {
    int y = page->cursor(), x = page->left() + S(20), w = page->width() - S(40), rh = S(44);
    new ImageBox(x, y + (rh - S(28)) / 2, S(28), {pins[i].icon, "application-x-executable"});
    new Label(x + S(40), y, w - S(40) - S(3 * 96), rh, pins[i].name);
    struct Move {
      size_t index;
      int delta;
    };
    const char *labels[] = {_("Move Up"), _("Move Down"), _("Unpin")};
    int deltas[] = {-1, 1, 0};
    for (int b = 0; b < 3; ++b) {
      auto *btn = new FlatButton(x + w - (3 - b) * S(96) + S(4), y + (rh - S(32)) / 2, S(92), S(32),
                                 labels[b]);
      if ((b == 0 && i == 0) || (b == 1 && i + 1 == pins.size())) btn->deactivate();
      btn->callback(
          [](Fl_Widget *, void *d) {
            auto *m = static_cast<Move *>(d);
            auto ids = split(g_cfg.get("panel", "pinned"), ';');
            std::vector<std::string> valid;
            for (auto &id : ids) {
              DesktopEntry e;
              if (find_application(trim(id), e)) valid.push_back(trim(id));
            }
            if (m->index >= valid.size()) return;
            if (m->delta == 0)
              valid.erase(valid.begin() + long(m->index));
            else {
              size_t other = m->index + size_t(m->delta);
              if (other < valid.size()) std::swap(valid[m->index], valid[other]);
            }
            std::string list;
            for (auto &id : valid) list += (list.empty() ? "" : ";") + id;
            g_cfg.set("panel", "pinned", list);
            apply_change();
            Fl::add_timeout(0, [](void *) { refresh_page(); });
          },
          keep(new Move{i, deltas[b]}));
    }
    page->advance(rh);
  }
  page->note(
      _("To pin an application, right-click it in the application menu or on the taskbar. "
        "Drag pinned icons on the panel to change their order."));
  page->end_card();
  page->finish();
}

void build_action_center(Page *page) {
  page->heading(_("Action Center"), _("Clock, greeting and quick controls opened from the panel"));
  page->begin_card(_("Clock card"));
  page->switch_row(_("Show the clock"), g_cfg.get_bool("action_center", "show_clock", true),
                   [](bool on) { set_bool("action_center", "show_clock", on); });
  page->choice_row(_("Clock style"), {_("Digital"), _("Analog")},
                   g_cfg.get("action_center", "clock_style", "digital") == "analog" ? 1 : 0,
                   [](int v) {
                     g_cfg.set("action_center", "clock_style", v == 1 ? "analog" : "digital");
                     apply_change();
                   });
  page->switch_row(_("Show a greeting"), g_cfg.get_bool("action_center", "show_greeting", true),
                   [](bool on) { set_bool("action_center", "show_greeting", on); });
  page->switch_row(_("24-hour clock"), g_cfg.get_bool("panel", "clock_24h", true),
                   [](bool on) { set_bool("panel", "clock_24h", on); });
  page->end_card();
  page->begin_card(_("Quick controls"));
  page->switch_row(
      _("Colorful tiles"), g_cfg.get_bool("action_center", "multicolor", false),
      [](bool on) { set_bool("action_center", "multicolor", on); },
      _("Give each quick control its own color, inspired by Eduka-Desktop."));
  page->choice_row(_("Tile columns"), {_("Two"), _("Three")},
                   g_cfg.get_int("action_center", "tile_columns", 2) == 3 ? 1 : 0, [](int v) {
                     g_cfg.set("action_center", "tile_columns", v == 1 ? "3" : "2");
                     apply_change();
                   });
  page->switch_row(
      _("Move status icons into Action Center"),
      g_cfg.get_bool("action_center", "move_status", true),
      [](bool on) { set_bool("action_center", "move_status", on); },
      _("System indicators and application tray return to the panel when it closes."));
  for (const auto &item : std::vector<std::pair<std::string, std::string>>{
           {"show_media", _("Media player cards")},
           {"show_app_volume", _("Per-application volume")},
           {"show_microphone", _("Microphone controls")},
           {"show_tray", _("System tray card")}})
    page->switch_row(item.second, g_cfg.get_bool("action_center", item.first, true),
                     [key = item.first](bool on) { set_bool("action_center", key.c_str(), on); });
  page->note(
      _("Bluetooth, Wi-Fi, LAN, sound and media have dedicated pages. The Action Center stays at "
        "the panel corner above the clock."));
  page->row(_("Preview"), S(190), S(34), [](int x, int y, int w, int h) {
    auto *button = new FlatButton(x, y, w, h, _("Open Action Center"), true);
    button->callback([](Fl_Widget *, void *) { x11::send_command("quick"); });
    return button;
  });
  page->end_card();
  page->note(
      _("Click the panel clock or press Super+A to open the Action Center. Super+C opens the "
        "calendar "
        "directly."));
  page->finish();
}

// Effects page: Start menu animations and the compositor. The effect set,
// the capability rule and the compositor modes/report follow
// Eduka-Desktop's Effects and Appearance pages, in SDE's own layout.
void build_effects(Page *page) {
  page->heading(_("Effects and Animations"), _("Menu animations, compositing, blur and shadows"));
  double gib = 0;
  int threads = 0;
  bool capable = effects_capable(&gib, &threads);
  page->begin_card(_("Animations"));
  page->note(capable ? format(_("This computer can use effects (%.1f GiB memory, %d CPU threads)."), gib,
                              threads)
                     : format(_("Effects need 4 GB of memory and 4 CPU threads. This computer has "
                                "%.1f GiB and %d threads, so Automatic keeps them off."),
                              gib, threads));
  std::string mode = g_cfg.get("effects", "enabled", "auto");
  page->choice_row(
      _("Effects"), {_("Automatic"), _("On"), _("Off")},
      mode == "true" ? 1 : mode == "false" ? 2 : 0,
      [](int v) {
        static const char *vals[] = {"auto", "true", "false"};
        g_cfg.set("effects", "enabled", vals[std::max(0, std::min(2, v))]);
        apply_change();
      },
      _("Used by the Start menu: categories, application tiles and starting applications."));
  auto effect_choice = [&](const char *label, const char *key, const char *fallback,
                           const std::vector<std::pair<std::string, std::string>> &list,
                           const char *hint) {
    std::vector<std::string> names;
    int current = 0;
    std::string value = g_cfg.get("effects", key, fallback);
    for (size_t i = 0; i < list.size(); ++i) {
      names.push_back(list[i].second);
      if (list[i].first == value) current = int(i);
    }
    page->choice_row(
        label, names, current,
        [key, items = &list](int v) {
          if (v >= 0 && v < int(items->size())) g_cfg.set("effects", key, (*items)[size_t(v)].first);
          apply_change();
        },
        hint);
  };
  effect_choice(_("Pointer over a tile"), "hover", "pop", hover_effects(),
                _("Wave lifts the icon, Glow pulses a ring, Slide moves the name, Pop grows the icon, "
                  "Bounce makes it jump."));
  effect_choice(_("Opening an application"), "launch", "zoom", launch_effects(),
                _("Zoom grows or shrinks the icon, Ripple spreads rings, Bubbles rise, Confetti "
                  "scatters colors, Fade dissolves the menu."));
  std::string speed = g_cfg.get("effects", "speed", "normal");
  page->choice_row(_("Speed"), {_("Slow"), _("Normal"), _("Fast")},
                   speed == "slow" ? 0 : speed == "fast" ? 2 : 1, [](int v) {
                     static const char *vals[] = {"slow", "normal", "fast"};
                     g_cfg.set("effects", "speed", vals[std::max(0, std::min(2, v))]);
                     apply_change();
                   });
  std::string size = g_cfg.get("effects", "size", "normal");
  page->choice_row(_("Size"), {_("Small"), _("Normal"), _("Large")},
                   size == "small" ? 0 : size == "large" ? 2 : 1, [](int v) {
                     static const char *vals[] = {"small", "normal", "large"};
                     g_cfg.set("effects", "size", vals[std::max(0, std::min(2, v))]);
                     apply_change();
                   });
  page->switch_row(_("Random effects"), g_cfg.get_bool("effects", "random", false),
                   [](bool on) { set_bool("effects", "random", on); },
                   _("A different opening effect each time."));
  page->switch_row(
      _("Low resource mode"), g_cfg.get_bool("appearance", "low_resource", false),
      [](bool on) {
        g_cfg.set("appearance", "low_resource", on ? "true" : "false");
        if (on) g_cfg.set("aero", "blur", "false");
        aero_changed();
      },
      _("For older computers: turns effects and blur off."));
  page->end_card();

  page->begin_card(_("Compositor"));
  page->note(_("The compositor draws transparency, blur, shadows and rounded corners. Without one "
               "SDE uses an opaque look."));
  std::string cm = g_cfg.get("compositor", "mode", "auto"), cb = g_cfg.get("compositor", "backend", "auto");
  int ci = cm == "off" ? 3 : cb == "glx" ? 1 : cb == "xrender" ? 2 : 0;
  page->choice_row(
      _("Compositor"), {_("Automatic (recommended)"), _("OpenGL (smoother, needs 3D graphics)"),
                        _("XRender (works everywhere, also VirtualBox)"), _("Off")},
      ci,
      [](int v) {
        g_cfg.set("compositor", "mode", v == 3 ? "off" : "auto");
        g_cfg.set("compositor", "backend", v == 1 ? "glx" : v == 2 ? "xrender" : "auto");
        apply_change();
        write_compositor_config(g_cfg);
        write_compositor_config(g_cfg, true);
        restart_compositor_now();
      },
      _("Changes apply at once."));
  static std::string report;
  report = compositor_report(g_cfg);
  page->note(report);
  page->row(_("Compositor report"), S(220), S(34), [](int x, int y, int w, int h) {
    auto *row = new Fl_Group(x, y, w, h);
    auto *restart = new FlatButton(x, y, (w - S(8)) / 2, h, _("Restart"));
    restart->callback([](Fl_Widget *, void *) {
      write_compositor_config(g_cfg);
      write_compositor_config(g_cfg, true);
      restart_compositor_now();
    });
    auto *copy = new FlatButton(x + (w + S(8)) / 2, y, (w - S(8)) / 2, h, _("Copy"));
    copy->callback([](Fl_Widget *, void *) {
      Fl::copy(report.c_str(), int(report.size()), 1);
      Fl::copy(report.c_str(), int(report.size()), 0);
    });
    row->end();
    return row;
  });
  page->end_card();
  page->note(_("Glass transparency, blur strength and rounded corners are in Appearance."));
  page->finish();
}

void build_desktop(Page *page) {
  page->heading(_("Desktop"), _("Icons shown on the desktop"));
  page->begin_card(_("Desktop icons"));
  page->switch_row(_("Files in the Desktop folder"), g_cfg.get_bool("desktop", "show_icons", true),
                   [](bool on) { set_bool("desktop", "show_icons", on); });
  page->switch_row(_("Home"), g_cfg.get_bool("desktop", "show_home", true),
                   [](bool on) { set_bool("desktop", "show_home", on); });
  page->switch_row(_("Trash"), g_cfg.get_bool("desktop", "show_trash", false),
                   [](bool on) { set_bool("desktop", "show_trash", on); });
  page->end_card();
  page->finish();
}

void send_test_notification() {
  dbus::Bus *bus = dbus::Bus::session();
  if (!bus) return;
  DBusMessage *m = dbus::method("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                "org.freedesktop.Notifications", "Notify");
  {
    dbus::Writer w(m);
    w.str("SDE Settings")
        .u32(0)
        .str("preferences-desktop-notification")
        .str(_("Notifications work"))
        .str(_("This is how notifications from your applications look."));
    w.strings({"default", _("Open")});
    { dbus::Writer hints(w.iter(), DBUS_TYPE_ARRAY, "{sv}"); }
    w.i32(-1);
  }
  bus->call_async(m, nullptr);
}

void build_notifications(Page *page) {
  page->heading(_("Notifications"), _("Messages from your applications"));
  page->begin_card();
  page->switch_row(
      _("Do Not Disturb"), g_cfg.get_bool("notifications", "do_not_disturb", false),
      [](bool on) { set_bool("notifications", "do_not_disturb", on); },
      _("Hide notifications, except critical ones such as a low battery."));
  int t = g_cfg.get_int("notifications", "timeout", 6);
  page->choice_row(_("Show notifications for"),
                   {_("4 seconds"), _("6 seconds"), _("10 seconds"), _("Until dismissed")},
                   t == 4    ? 0
                   : t == 10 ? 2
                   : t == 0  ? 3
                             : 1,
                   [](int v) {
                     static const char *vals[] = {"4", "6", "10", "0"};
                     if (v >= 0 && v < 4) g_cfg.set("notifications", "timeout", vals[v]);
                     apply_change();
                   });
  page->row(_("Try it"), S(200), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Send a Test Notification"));
    b->callback([](Fl_Widget *, void *) { send_test_notification(); });
    return b;
  });
  page->end_card();
  page->finish();
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
  if (!check) return _("none");
  std::string n = x11::get_utf8(check, "_NET_WM_NAME");
  return n.empty() ? _("unknown") : n;
}

// Source names are short clickable links. Licenses open the installed fork's
// complete copyright file, including all historical contributors.
FlatButton *action_button(int x, int y, int w, int h, const std::string &label,
                          std::vector<std::string> argv, bool primary = false) {
  auto *button = new FlatButton(x, y, w, h, label, primary);
  button->callback(
      [](Fl_Widget *, void *p) {
        auto *command = static_cast<std::vector<std::string> *>(p);
        spawn_detached(*command);
      },
      keep(new std::vector<std::string>(std::move(argv))));
  return button;
}

void build_suite(Page *page) {
  page->heading(_("Applications"), _("Applications for DebION GNU/Linux"));
  page->note(
      _("Development edition. Native SDE applications are included in one package. "
        "Original authors and licenses remain credited."));
  page->choice_row(
      _("Application theme"), {_("Follow SDE"), _("System GTK theme")},
      g_cfg.get("suite", "gtk_theme", "auto") == "system" ? 1 : 0,
      [](int choice) {
        g_cfg.set("suite", "gtk_theme", choice ? "system" : "auto");
        apply_change();
      },
      _("Applies when a suite application next starts."));
  for (const auto &app : suite_apps()) {
    page->begin_card(app.name + "  ·  " + app.version);
    page->row(_(app.role.c_str()), S(112), S(32), [&app](int x, int y, int w, int h) {
      auto *button = action_button(x, y, w, h, _("Open"), split_command(app.command), true);
      if (find_program(split_command(app.command)[0]).empty()) button->deactivate();
      return button;
    });
    int x = page->left() + S(20), y = page->cursor();
    int available = page->width() - S(40), gap = S(8);
    int source_w = std::min(S(206), (available - gap) * 2 / 3);
    action_button(x, y, source_w, S(32), app.upstream, {"xdg-open", app.url});
    action_button(x + source_w + gap, y, available - source_w - gap, S(32), _("Copyright"),
                  {"xdg-open", "/usr/share/doc/" + app.package + "/copyright"});
    page->advance(S(42));
    page->note(app.license);
    page->end_card();
  }
  page->note(
      _("Original application licenses and copyrights are retained. SDE Update credits mintupdate; "
        "PackageKit "
        "performs Debian transactions."));
  page->finish();
}

void build_suite_system(Page *page) {
  page->heading(_("Power & Updates"), _("System tools included with SDE"));
  const std::vector<std::pair<std::string, std::vector<std::string>>> tools = {
      {_("Power Preferences"), {"sde-power-preferences"}},
      {_("Battery History"), {"sde-power-statistics"}},
      {_("System Monitor"), {"sde-system-monitor"}},
      {_("System Updates"), {"sde-update"}},
      {_("Screen Lock Preferences"), {"sde-screensaver-preferences"}},
  };
  page->begin_card(_("System"));
  for (auto &tool : tools) {
    page->row(tool.first, S(112), S(34), [&tool](int x, int y, int w, int h) {
      return action_button(x, y, w, h, _("Open"), tool.second);
    });
  }
  page->end_card();
  page->note(
      _("SDE Power Manager handles battery, lid and idle policies. SDE Screen Lock starts with the "
        "session. "
        "No background updater is started."));
  page->begin_card(_("Windows and effects"));
  page->note(_("SDE WM manages windows. Compositing, animations and glass are on the Effects page."));
  page->row(_("Effects and Animations"), S(112), S(34), [](int x, int y, int w, int h) {
    return action_button(x, y, w, h, _("Open"), {"sde-settings", "--page", "effects"});
  });
  page->end_card();
  page->row(_("Applications and credits"), S(112), S(34), [](int x, int y, int w, int h) {
    return action_button(x, y, w, h, _("Applications"), {"sde-settings", "--page", "suite"});
  });
  page->finish();
}

void build_about(Page *page) {
  page->begin_card();
  int y = page->cursor();
  new ImageBox(page->left() + S(24), y + S(16), S(88), {"sde-logo", "start-here"});
  int tx = page->left() + S(132), tw = page->width() - S(152);
  new Label(tx, y + S(14), tw, S(34), "Selviana Desktop Environment", Label::TITLE);
  new Label(tx, y + S(50), tw, S(22), _("Small and fast, with a modern desktop experience."),
            Label::SUBTITLE);
  new Label(tx, y + S(76), tw, S(22), format(_("Version %s (%s)"), SDE_VERSION_LABEL, SDE_VERSION),
            Label::DIM);
  page->advance(S(112));
  page->end_card();

  struct utsname u;
  uname(&u);
  std::string mem = read_first("/proc/meminfo", "MemTotal");
  if (!mem.empty()) mem = format("%.1f GiB", atof(mem.c_str()) / 1024.0 / 1024.0);
  std::string cpu = read_first("/proc/cpuinfo", "model name");
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, 0);
  std::vector<std::pair<std::string, std::string>> rows = {
      {_("Operating system"), os_release("PRETTY_NAME")},
      {_("Kernel"), std::string(u.sysname) + " " + u.release},
      {_("Processor"), cpu.empty() ? u.machine : cpu},
      {_("Memory"), mem},
      {_("Display"), format("%d x %d", sw, sh)},
      {_("Interface size"), format("%d %%", int(std::lround(g_theme.scale * 100)))},
      {_("Window manager"), wm_name()},
      {_("Toolkit"), "FLTK " SDE_FLTK_VERSION},
  };
  page->begin_card(_("System"));
  for (auto &r : rows) {
    int ry = page->cursor(), x = page->left() + S(20), w = page->width() - S(40);
    int kw = std::min(S(190), w / 3);
    new Label(x, ry, kw, S(30), r.first, Label::DIM);
    new Label(x + kw + S(10), ry, w - kw - S(10), S(30), r.second, Label::NORMAL);
    page->advance(S(30));
  }
  page->end_card();
  page->note(_("Free software under the GNU General Public License, version 3 or later."));
  page->note(_("Copyright (C) 2026 Hugo Moniz do Rego and the SDE contributors."));
  page->finish();
}

const std::vector<PageInfo> &pages() {
  static const std::vector<PageInfo> list = {
      {"home", N_("Overview"), {"preferences-system", "computer"}, build_home},
      {"appearance",
       N_("Appearance"),
       {"preferences-desktop-theme", "preferences-desktop-wallpaper"},
       build_appearance},
      {"panel", N_("Panel"), {"preferences-system-windows", "preferences-desktop"}, build_panel},
      {"desktop", N_("Desktop"), {"user-desktop", "preferences-desktop-wallpaper"}, build_desktop},
      {"action-center",
       N_("Action Center"),
       {"preferences-system", "preferences-desktop"},
       build_action_center},
      {"notifications",
       N_("Notifications"),
       {"preferences-desktop-notification", "notification-active", "dialog-information"},
       build_notifications},
      {"effects",
       N_("Effects"),
       {"preferences-desktop-effects", "applications-graphics", "preferences-desktop"},
       build_effects},
      {"display",
       N_("Displays"),
       {"preferences-desktop-display", "video-display", "computer"},
       build_display_page},
      {"input",
       N_("Keyboard & Mouse"),
       {"preferences-desktop-keyboard", "input-keyboard", "input-mouse"},
       build_input_page},
      {"suite", N_("Applications"), {"applications-other", "sde-logo"}, build_suite},
      {"system", N_("Power & Updates"), {"utilities-system-monitor"}, build_suite_system},
      {"about", N_("About"), {"help-about", "dialog-information"}, build_about},
  };
  return list;
}

// --------------------------------------------------------------- window

class Sidebar;
Sidebar *g_sidebar = nullptr;
Fl_Scroll *g_sidebar_scroll = nullptr;
Page *g_page = nullptr;
int g_page_index = 0;
int g_content_x = 0, g_content_w = 0;

void show_page(int index);

class Sidebar : public Fl_Widget {
 public:
  Sidebar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int row_h() const {
    return std::max(S(18), std::min(S(44), (h() - S(144)) / int(pages().size())));
  }
  int row_y(int i) const {
    return y() + S(80) + i * row_h() + (i >= 1 ? S(24) : 0) + (i >= 5 ? S(24) : 0);
  }
  void draw() override {
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), g_theme.surface);
      p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), g_theme.border);
      p.image(IconLoader::get({"sde-logo", "start-here"}, S(36)), x() + S(20), y() + S(20), S(36));
      for (size_t i = 0; i < pages().size(); ++i) {
        int ry = row_y(int(i));
        if (int(i) == g_page_index) {
          p.fill_rounded(x() + S(10), ry, w() - S(20), row_h() - S(4), S(9),
                         g_theme.accent.alpha(0.15));
          p.fill_rounded(x() + S(10), ry + S(10), Sd(3), std::max(S(6), row_h() - S(20)), Sd(1.5),
                         g_theme.accent);
        } else if (int(i) == hover_) {
          p.fill_rounded(x() + S(10), ry, w() - S(20), row_h() - S(4), S(9), g_theme.hover);
        }
        if (Fl::focus() == this && int(i) == g_page_index)
          focus_ring(p, x() + S(10), ry, w() - S(20), row_h() - S(4), S(9));
        settings_glyph(p, pages()[i].id, x() + S(24), ry + (row_h() - S(20)) / 2, S(20),
                       g_theme.accent_text);
      }
    }
    text_font(g_theme.font_size + 2, true);
    draw_text(_("Settings"), x() + S(64), y() + S(22), w() - S(70), S(32), g_theme.text);
    text_font(g_theme.font_size - 2, true);
    draw_text(_("PERSONALIZATION"), x() + S(22), row_y(1) - S(23), w() - S(30), S(20),
              g_theme.text_dim);
    draw_text(_("SYSTEM"), x() + S(22), row_y(5) - S(23), w() - S(30), S(20), g_theme.text_dim);
    for (size_t i = 0; i < pages().size(); ++i) {
      bool cur = int(i) == g_page_index;
      text_font(g_theme.font_size, cur);
      if (text_width(_(pages()[i].title)) > w() - S(66))
        text_font(std::max(S(9), g_theme.font_size - S(2)), cur);
      draw_text(_(pages()[i].title), x() + S(56), row_y(int(i)), w() - S(66), row_h() - S(4),
                cur ? g_theme.accent_text : g_theme.text);
    }
    // The version line only when it does not collide with the page list.
    if (row_y(int(pages().size())) + S(8) < y() + h() - S(36)) {
      text_font(g_theme.font_size - 1);
      draw_text(_("Saved automatically"), x() + S(20), y() + h() - S(36), w() - S(30), S(20),
                g_theme.text_dim);
    }
  }
  int index_at(int ey) const {
    for (size_t i = 0; i < pages().size(); ++i)
      if (ey >= row_y(int(i)) && ey < row_y(int(i)) + row_h() - S(4)) return int(i);
    return -1;
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH: {
        int i = index_at(Fl::event_y());
        if (i >= 0) show_page(i);
        return 1;
      }
      case FL_MOVE:
      case FL_ENTER: {
        int h = index_at(Fl::event_y());
        if (h != hover_) {
          hover_ = h;
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
        int k = Fl::event_key(), n = int(pages().size());
        if (k == FL_Up && g_page_index > 0)
          show_page(g_page_index - 1);
        else if (k == FL_Down && g_page_index + 1 < n)
          show_page(g_page_index + 1);
        else
          return 0;
        take_focus();
        return 1;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  int hover_ = -1;
};

class SettingsWindow : public BufferedWindow {
 public:
  SettingsWindow(int w, int h) : BufferedWindow(w, h, _("Settings")) {}
  void draw() override {
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), g_theme.background);
    }
    draw_children();
  }
};

void show_page(int index) {
  if (index < 0 || index >= int(pages().size())) return;
  g_page_index = index;
  g_restart_page = pages()[size_t(index)].id;
  if (g_sidebar_scroll && g_sidebar) {
    int top = g_sidebar->row_y(index), bottom = top + g_sidebar->row_h();
    int scroll = g_sidebar_scroll->yposition();
    if (top < 0)
      scroll += top - S(8);
    else if (bottom > g_sidebar_scroll->h())
      scroll += bottom - g_sidebar_scroll->h() + S(8);
    scroll = std::max(0, std::min(scroll, g_sidebar->h() - g_sidebar_scroll->h()));
    g_sidebar_scroll->scroll_to(0, scroll);
  }
  if (g_page) {
    g_window->remove(g_page);
    Fl::delete_widget(g_page);
  }
  g_window->begin();
  g_page = new Page(g_content_x, 0, g_content_w, g_window->h());
  pages()[size_t(index)].build(g_page);
  g_window->end();
  g_window->redraw();
}

}  // namespace

void refresh_page() {
  int scroll = g_page ? g_page->yposition() : 0;
  show_page(g_page_index);
  if (g_page && scroll > 0) g_page->scroll_to(0, scroll);
}

}  // namespace sde

using namespace sde;

int main(int argc, char **argv) {
  init_i18n();
  g_argv0 = argv[0];
  std::string page = "home";
  bool apply = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--page" && i + 1 < argc) {
      page = argv[++i];
    } else if (a == "--apply") {
      apply = true;
    } else if (a == "--version") {
      printf("sde-settings %s (%s)\n", SDE_VERSION, SDE_VERSION_LABEL);
      return 0;
    } else {
      printf("%s",
             _("Usage: sde-settings [--page NAME] [--apply] [--version]\n\n"
               "  --page NAME  open a page: home, appearance, panel, desktop, action-center,\n"
               "               notifications, display, input, suite, system or about\n"
               "  --apply      apply the saved display, keyboard and mouse settings and exit\n"));
      return a == "--help" ? 0 : 2;
    }
  }
  g_cfg.load();
  if (apply) {
    sync_gtk_settings(g_cfg);
    migrate_legacy_suite_shortcuts();
    apply_all(g_cfg);
    return 0;
  }
  fl_open_display();
  g_theme = Theme::from_config(g_cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(g_cfg.get("appearance", "icon_theme", "auto"));
  Fl::visible_focus(1);

  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int W = std::min(S(940), sw - S(24)), H = std::min(S(660), sh - S(60));
  int side = W < S(760) ? S(176) : S(220);
  auto *win = new SettingsWindow(W, H);
  g_window = win;
  g_sidebar_scroll = new Fl_Scroll(0, 0, side, H);
  g_sidebar_scroll->type(Fl_Scroll::VERTICAL);
  g_sidebar_scroll->box(FL_FLAT_BOX);
  g_sidebar_scroll->color(g_theme.surface.fl());
  g_sidebar_scroll->scrollbar_size(S(10));
  g_sidebar_scroll->scrollbar.color(g_theme.surface.fl());
  g_sidebar_scroll->scrollbar.selection_color(g_theme.border.fl());
  int sidebar_height = std::max(H, S(144) + int(pages().size()) * S(44));
  g_sidebar = new Sidebar(0, 0, side - S(10), sidebar_height);
  g_sidebar_scroll->end();
  g_content_x = side;
  g_content_w = W - side;
  win->end();
  win->size_range(std::min(W, S(600)), std::min(H, S(420)));
  win->resizable(nullptr);
  win->xclass("sde-settings");
  int index = 0;
  for (size_t i = 0; i < pages().size(); ++i)
    if (page == pages()[i].id) index = int(i);
  show_page(index);
  win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  start_display_watch();
  win->show();
  return Fl::run();
}
