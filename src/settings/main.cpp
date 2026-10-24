// SPDX-License-Identifier: GPL-3.0-or-later
// sde-settings: the SDE Control Center (also started as sde-control-center).
//
// The overview is a hub of every setting by category (hub.cpp): the pages
// of this window (Appearance, Panel, Start Menu, Displays, Sound, Users &
// Avatar ...) and tiles that start the separate tools (Display Settings,
// Software Updater, Driver Manager, System Reports, System Monitor,
// printers). Every change is saved immediately to ~/.config/sde/sde.conf
// and announced to the running desktop. The program and its page ids keep
// their names: "sde-settings --page NAME" opens a page.
//
// "sde-settings --apply" applies the saved display, keyboard and pointer
// settings without opening a window (run by sde-session at login).
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../panel/timezone.h"
#include "about.h"
#include "avatar.h"
#include "avatar_view.h"
#include "crash.h"
#include "buffered_window.h"
#include "build_config.h"
#include "build_history.h"
#include "build_info.h"
#include "config.h"
#include "dbus.h"
#include "effects.h"
#include "desktop_entry.h"
#include "devices.h"
#include "gpu.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "integration.h"
#include "paint.h"
#include "process.h"
#include "settings.h"
#include "spawn.h"
#include "suite.h"
#include "system.h"
#include "theme.h"
#include "thumbnails.h"
#include "ui.h"
#include "util.h"
#include "wallpapers.h"
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
// The anchor ("wallpaper", "proxy", "mouse" ...) the next page shown
// scrolls to (see Page::anchor()), from --page or an overview tile.
std::string g_open_anchor;
// True while refresh_page() rebuilds the visible page (not a new visit).
bool g_refreshing = false;

}  // namespace

void refresh_if_shown(const char *page_id) {
  if (g_restart_page == page_id) Fl::add_timeout(0, [](void *) { refresh_page(); });
}

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

// Writes the chosen family, mode and accent and applies them everywhere:
// GTK (settings.ini, XSETTINGS, libadwaita colors), SDE WM frames and
// button layout, the compositor and every SDE program.
void set_theme(const std::string &family, const std::string &mode, const std::string &accent) {
  g_cfg.set("appearance", "style", family);
  g_cfg.set("appearance", "theme", mode);
  g_cfg.set("appearance", "accent_name", accent);
  g_cfg.set("appearance", "accent", theme_accent_color(family, accent, false).to_hex());
  g_theme = Theme::from_config(g_cfg);
  aero_changed();
  if (g_window) g_window->redraw();
}

// Generic card-like radio widget: draw_card() paints, pick() chooses.
class PickCard : public Fl_Widget {
 public:
  PickCard(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  virtual bool selected() const = 0;
  virtual void pick() = 0;
  int handle(int e) override {
    switch (e) {
      case FL_PUSH: return 1;
      case FL_RELEASE:
        if (Fl::event_inside(this)) pick();
        return 1;
      case FL_ENTER: case FL_LEAVE:
        hot_ = e == FL_ENTER;
        redraw();
        return 1;
      case FL_FOCUS: case FL_UNFOCUS:
        redraw();
        return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          pick();
          return 1;
        }
        return 0;
      default:
        return Fl_Widget::handle(e);
    }
  }

 protected:
  bool hot_ = false;
};

// A miniature desktop drawn with a family's palette and shapes (0.2.0
// Alpha 2): a window with that family's corners and caption buttons,
// sidebar, text lines and an accent button, a popup menu with the
// highlighted item and a slim panel. SDE Liquid Glass shows its glass over
// a livelier backdrop: translucent layers with light rims, pill shapes.
void draw_theme_preview(Painter &p, const Theme &t, double x, double y, double w, double h,
                        double outer_r) {
  bool glass = t.family == "glass";
  // Backdrop: a calm wallpaper; behind glass, soft light spots to show it.
  Color paper = t.dark ? t.panel_bg.mix(Color::hex(0x000000), 0.25) : t.surface_alt.mix(t.border, 0.6);
  if (glass) paper = t.dark ? Color::hex(0x1B2A3A) : Color::hex(0x9CC3DD);
  p.fill_gradient(x, y, w, h, outer_r, paper.mix(t.accent, glass ? 0.28 : 0.10), paper, true);
  if (glass) {
    p.circle(x + w * 0.80, y + h * 0.22, h * 0.22, Color::hex(t.dark ? 0x46607A : 0xF6E7C8, 0.85));
    p.circle(x + w * 0.18, y + h * 0.92, h * 0.30, Color::hex(t.dark ? 0x2E5B55 : 0x7FB7A7, 0.75));
  }
  double s = std::min(1.0, w / Sd(190));  // the miniature's scale
  double wx = x + w * 0.07, wy = y + h * 0.10, ww = w * 0.74, wh = h * 0.70;
  double r = std::max(Sd(3), t.radius * 0.6 * s);
  p.shadow(wx, wy, ww, wh, r, t.shadow.alpha(std::min(0.35, t.shadow.a)), int(Sd(6)));
  Color body = glass ? t.background.alpha(t.dark ? 0.82 : 0.78) : t.background;
  p.fill_rounded(wx, wy, ww, wh, r, body);
  double th = Sd(16);
  // Title bar.
  cairo_save(p.cr());
  p.rounded_rect(wx, wy, ww, wh, r);
  cairo_clip(p.cr());
  p.fill_rect(wx, wy, ww, th, glass ? t.titlebar_bg.alpha(0.7) : t.titlebar_bg);
  if (!glass) p.line(wx, wy + th - 0.5, wx + ww, wy + th - 0.5, t.border);
  // Sidebar and content.
  p.fill_rect(wx, wy + th, ww * 0.28, wh - th, glass ? t.surface_alt.alpha(0.55) : t.surface_alt);
  p.fill_rect(wx + ww * 0.28, wy + th, ww * 0.72, wh - th, glass ? t.surface.alpha(0.9) : t.surface);
  cairo_restore(p.cr());
  if (glass)
    p.surface_edge(wx, wy, ww, wh, r, t.border.alpha(0.7), 1.0, t.dark);
  else
    p.stroke_rounded(wx + 0.5, wy + 0.5, ww - 1, wh - 1, r, t.border);
  double cy = wy + th / 2;
  {
    // Caption buttons of every family since alpha51 (Mint-Y like): a thin
    // minimize dash and maximize square, and close as a soft gray disc with
    // the cross cut out, as SDE WM and GTK draw them.
    Color g = t.caption_glyph;
    double bw = Sd(12), c = wx + ww - Sd(4) - bw / 2;
    double dr = Sd(4.6), k = Sd(1.8);
    p.circle(c, cy, dr, t.caption_disc);
    p.line(c - k, cy - k, c + k, cy + k, t.titlebar_bg, Sd(1));
    p.line(c - k, cy + k, c + k, cy - k, t.titlebar_bg, Sd(1));
    c -= bw;
    p.stroke_rounded(c - Sd(2.2), cy - Sd(2.2), Sd(4.4), Sd(4.4), Sd(0.6), g, Sd(0.9));
    c -= bw;
    p.line(c - Sd(2.2), cy + Sd(2.2), c + Sd(2.2), cy + Sd(2.2), g, Sd(0.9));
  }
  // Sidebar rows, the selected one in the accent.
  for (int i = 0; i < 3; ++i) {
    double ry = wy + th + Sd(8) + i * Sd(11);
    if (i == 0) p.fill_rounded(wx + Sd(4), ry - Sd(3), ww * 0.28 - Sd(8), Sd(9), Sd(3), t.accent.alpha(0.22));
    p.fill_rounded(wx + Sd(8), ry, ww * 0.16, Sd(3), Sd(1.5), i == 0 ? t.accent_text : t.text_dim.alpha(0.6));
  }
  double cx = wx + ww * 0.28 + Sd(8);
  p.fill_rounded(cx, wy + th + Sd(9), ww * 0.34, Sd(4), Sd(2), t.text.alpha(0.75));
  p.fill_rounded(cx, wy + th + Sd(18), ww * 0.46, Sd(3), Sd(1.5), t.text_dim.alpha(0.6));
  p.fill_rounded(cx, wy + th + Sd(25), ww * 0.40, Sd(3), Sd(1.5), t.text_dim.alpha(0.6));
  double br = std::min(Sd(5), t.radius_small * 0.6);  // pill buttons in Liquid Glass
  p.fill_rounded(cx, wy + wh - Sd(16), Sd(30), Sd(10), glass ? Sd(5) : br, t.accent);
  // A popup menu overlapping the window, as Acrylic/vibrancy shows it; its
  // corners follow the family's radius like the real menus.
  double mx = wx + ww * 0.58, my = wy + wh * 0.42, mw = w * 0.36, mh = Sd(34);
  double mr = std::max(Sd(3), std::min(Sd(9), t.radius * 0.6));
  p.shadow(mx, my, mw, mh, mr, t.shadow.alpha(std::min(0.3, t.shadow.a)), int(Sd(5)));
  p.fill_rounded(mx, my, mw, mh, mr, t.menu_bg.alpha(glass ? 0.80 : t.translucent ? 0.94 : 1.0));
  if (glass)
    p.surface_edge(mx, my, mw, mh, mr, t.menu_border.alpha(0.6), 1.0, t.dark);
  else
    p.stroke_rounded(mx + 0.5, my + 0.5, mw - 1, mh - 1, mr, t.menu_border);
  p.fill_rounded(mx + Sd(3), my + Sd(12), mw - Sd(6), Sd(10), glass ? Sd(5) : Sd(4), t.menu_hover);
  p.fill_rounded(mx + Sd(7), my + Sd(5), mw * 0.5, Sd(3), Sd(1.5), t.menu_text.alpha(0.6));
  p.fill_rounded(mx + Sd(7), my + Sd(15.5), mw * 0.55, Sd(3), Sd(1.5), t.menu_hover_text.alpha(0.9));
  p.fill_rounded(mx + Sd(7), my + Sd(26), mw * 0.4, Sd(3), Sd(1.5), t.menu_text.alpha(0.6));
  // The panel: a strip along the bottom, a floating glass pill in Liquid Glass.
  double ph = Sd(11), py = y + h - ph - (glass ? Sd(4) : 0);
  double px = glass ? x + w * 0.16 : x, pw = glass ? w * 0.68 : w;
  cairo_save(p.cr());
  p.rounded_rect(x, y, w, h, outer_r);
  cairo_clip(p.cr());
  p.fill_rounded(px, py, pw, glass ? ph : ph + outer_r, glass ? ph / 2 : 0, t.surface.alpha(glass ? 0.72 : 0.97));
  if (glass)
    p.surface_edge(px, py, pw, ph, ph / 2, t.border.alpha(0.5), 1.0, t.dark);
  else
    p.line(px, py + 0.5, px + pw, py + 0.5, t.border);
  for (int i = 0; i < 4; ++i)
    p.fill_rounded(px + (glass ? Sd(6) : Sd(5)) + i * Sd(9), py + Sd(3), Sd(6), ph - Sd(6), Sd(1.5),
                   i == 0 ? t.accent : t.text_dim.alpha(0.45));
  cairo_restore(p.cr());
}

// Theme family card: preview, name and a one-line description.
class FamilyCard : public PickCard {
 public:
  FamilyCard(int x, int y, int w, int h, std::string id, std::string label, std::string desc)
      : PickCard(x, y, w, h), id_(std::move(id)), label_(std::move(label)), desc_(std::move(desc)) {
    tooltip(desc_.c_str());
  }
  bool selected() const override { return g_theme.family == id_; }
  void pick() override { set_theme(id_, g_theme.mode, g_theme.accent_name); }
  void draw() override {
    bool on = selected();
    int pv = h() - S(44);
    {
      Painter p(window()->w(), window()->h());
      Theme t = Theme::make(id_, g_theme.dark, g_theme.accent_name);
      t.translucent = g_theme.translucent;
      p.fill_rounded(x(), y(), w(), h(), S(12), hot_ ? g_theme.hover : g_theme.surface);
      double in = Sd(6);
      cairo_save(p.cr());
      p.rounded_rect(x() + in, y() + in, w() - 2 * in, pv - in, Sd(8));
      cairo_clip(p.cr());
      draw_theme_preview(p, t, x() + in, y() + in, w() - 2 * in, pv - in, Sd(8));
      cairo_restore(p.cr());
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(12), on ? g_theme.accent : g_theme.border,
                       on ? Sd(2.5) : Sd(1));
      if (on) {
        double bx = x() + w() - Sd(18), by = y() + Sd(18);
        p.circle(bx, by, Sd(9), g_theme.accent);
        p.line(bx - Sd(4), by, bx - Sd(1), by + Sd(3), g_theme.on_accent, Sd(2));
        p.line(bx - Sd(1), by + Sd(3), bx + Sd(4), by - Sd(3), g_theme.on_accent, Sd(2));
      }
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), Sd(12));
    }
    text_font(g_theme.font_size, true);
    draw_text(std::string("SDE ") + _(label_.c_str()), x() + S(12), y() + pv + S(3), w() - S(24), S(20),
              on ? g_theme.accent_text : g_theme.text);
    text_font(std::max(S(10), g_theme.font_size - S(2)));
    draw_text(_(short_desc().c_str()), x() + S(12), y() + pv + S(21), w() - S(24), S(18), g_theme.text_dim);
  }

 private:
  std::string short_desc() const {
    if (id_ == "graphite") return N_("Calm neutral grays · soft depth");
    if (id_ == "fluent") return N_("Fluent Design · Mica, Acrylic");
    return N_("Glass with light rims · pill shapes");
  }
  std::string id_, label_, desc_;
};

// Light / Dark / Auto: a half-and-half tile for Auto.
class ModeCard : public PickCard {
 public:
  ModeCard(int x, int y, int w, int h, std::string mode) : PickCard(x, y, w, h), mode_(std::move(mode)) {}
  bool selected() const override { return g_theme.mode == mode_; }
  void pick() override { set_theme(g_theme.family, mode_, g_theme.accent_name); }
  void draw() override {
    bool on = selected();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(10), hot_ || on ? g_theme.hover : g_theme.surface_alt);
      double sw = h() - Sd(14), sx = x() + Sd(8), sy = y() + Sd(7);
      Theme light = Theme::make(g_theme.family, false, g_theme.accent_name);
      Theme dark = Theme::make(g_theme.family, true, g_theme.accent_name);
      if (mode_ == "auto") {
        cairo_save(p.cr());
        p.rounded_rect(sx, sy, sw, sw, Sd(6));
        cairo_clip(p.cr());
        p.fill_rect(sx, sy, sw / 2, sw, light.background);
        p.fill_rect(sx + sw / 2, sy, sw / 2, sw, dark.background);
        cairo_restore(p.cr());
      } else {
        p.fill_rounded(sx, sy, sw, sw, Sd(6), mode_ == "dark" ? dark.background : light.background);
      }
      p.stroke_rounded(sx + 0.5, sy + 0.5, sw - 1, sw - 1, Sd(6), g_theme.border);
      p.fill_rounded(sx + sw * 0.2, sy + sw * 0.6, sw * 0.6, sw * 0.18, Sd(2), g_theme.accent);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(10), on ? g_theme.accent : g_theme.border,
                       on ? Sd(2) : Sd(1));
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), Sd(10));
    }
    const char *label = mode_ == "light" ? _("Light") : mode_ == "dark" ? _("Dark") : _("Auto");
    text_font(g_theme.font_size, on);
    int tx = x() + h() + S(2);
    draw_text(label, tx, y(), w() - (tx - x()) - S(6), h(), on ? g_theme.accent_text : g_theme.text);
  }

 private:
  std::string mode_;
};

// Accent swatches of the current family.
class AccentPicker : public Fl_Widget {
 public:
  AccentPicker(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int step() const { return std::min(S(36), w() / int(theme_accents().size())); }
  void draw() override {
    Painter p(window()->w(), window()->h());
    int st = step();
    auto accents = theme_accents();
    for (int i = 0; i < int(accents.size()); ++i) {
      Color c = theme_accent_color(g_theme.family, accents[size_t(i)], g_theme.dark);
      double cx = x() + st / 2.0 + i * st, cy = y() + h() / 2.0;
      bool active = accents[size_t(i)] == g_theme.accent_name;
      double r = st * 0.34;
      if (active) p.circle(cx, cy, r + Sd(4), c);
      p.circle(cx, cy, active ? r + Sd(2) : r + Sd(1), g_theme.surface);
      p.circle(cx, cy, r, c);
      p.stroke_rounded(cx - r, cy - r, 2 * r, 2 * r, r, g_theme.text.alpha(0.12));
      if (Fl::focus() == this && i == focus_) {
        p.set(g_theme.accent_text);
        cairo_set_line_width(p.cr(), Sd(2));
        cairo_new_sub_path(p.cr());
        cairo_arc(p.cr(), cx, cy, r + Sd(7), 0, 2 * M_PI);
        cairo_stroke(p.cr());
      }
    }
  }
  void pick(int i) {
    auto accents = theme_accents();
    if (i < 0 || i >= int(accents.size())) return;
    set_theme(g_theme.family, g_theme.mode, accents[size_t(i)]);
  }
  int handle(int e) override {
    int n = int(theme_accents().size());
    switch (e) {
      case FL_PUSH:
        return 1;
      case FL_MOVE: {
        int i = (Fl::event_x() - x()) / step();
        static std::string tip;
        tip = i >= 0 && i < n ? theme_accent_label(theme_accents()[size_t(i)]) : "";
        tooltip(tip.c_str());
        return 1;
      }
      case FL_ENTER:
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
  int focus_ = 0;
};

std::string default_wallpaper() {
  std::string base = std::string(SDE_DATADIR).substr(0, std::string(SDE_DATADIR).rfind('/'));
  std::string name = g_theme.dark ? "selviana-dark.svg" : "selviana-light.svg";
  std::string path = base + "/backgrounds/sde/" + name;
  // Builds installed elsewhere than /usr still show the packaged picture.
  if (!file_exists(path) && file_exists("/usr/share/backgrounds/sde/" + name))
    path = "/usr/share/backgrounds/sde/" + name;
  return path;
}

// ------------------------------------------------------------- wallpapers
//
// The list of wallpapers is scanned once per run on a worker thread, so the
// Appearance page opens at once. Thumbnails come from the Gallery
// thumbnail service: decoded on worker threads, shared with other
// applications through ~/.cache/thumbnails (freedesktop.org) and requested
// only for the tiles that are on screen.

struct WallpaperScan {
  std::mutex mu;
  bool started = false, done = false;
  std::vector<Wallpaper> list;
};
WallpaperScan &wallpaper_scan() {
  static auto *scan = new WallpaperScan;  // outlives the scanning thread
  return *scan;
}

void wallpaper_scan_done(void *) {
  if (g_restart_page == "appearance") refresh_page();
}

void start_wallpaper_scan() {
  WallpaperScan &s = wallpaper_scan();
  if (s.started) return;
  s.started = true;
  std::thread([&s] {
    auto list = scan_wallpapers(wallpaper_roots(SDE_DATADIR), gallery::extra_formats());
    {
      std::lock_guard<std::mutex> lock(s.mu);
      s.list = std::move(list);
      s.done = true;
    }
    Fl::awake(wallpaper_scan_done, nullptr);
  }).detach();
}

class WallpaperGrid;
WallpaperGrid *g_wallpaper_grid = nullptr;
std::unique_ptr<gallery::ThumbnailService> g_thumbnailer;
std::map<std::string, cairo_surface_t *> g_thumbs;  // nullptr: could not be decoded
void thumbnails_ready(void *);

gallery::ThumbnailService &thumbnailer() {
  if (!g_thumbnailer)
    g_thumbnailer.reset(
        new gallery::ThumbnailService([] { Fl::awake(thumbnails_ready, nullptr); }, 2));
  return *g_thumbnailer;
}

class WallpaperGrid : public Fl_Widget {
 public:
  struct Tile {
    std::string value;  // stored in [appearance] wallpaper ("auto" or a path)
    std::string path;   // picture shown
    std::string label;
    time_t mtime = 0;
  };

  WallpaperGrid(int x, int y, int w) : Fl_Widget(x, y, w, 10) {
    gap_ = S(12);
    cols_ = std::max(2, (w + gap_) / (S(150) + gap_));
    tw_ = (w - (cols_ - 1) * gap_) / cols_;
    th_ = tw_ * 10 / 16;
    label_h_ = S(24);
    build_tiles();
    int rows = std::max(1, (int(tiles_.size()) + cols_ - 1) / cols_);
    size(w, rows * row_h() - gap_);
    g_wallpaper_grid = this;
    for (size_t i = 0; i < tiles_.size(); ++i)
      if (selected(int(i))) focus_ = int(i);
  }
  ~WallpaperGrid() override {
    if (g_wallpaper_grid == this) g_wallpaper_grid = nullptr;
  }
  int count() const { return int(tiles_.size()); }
  int row_h() const { return th_ + label_h_ + gap_; }

  static std::string current_value() {
    std::string wp = g_cfg.get("appearance", "wallpaper", "auto");
    return wp.empty() ? "auto" : wp;
  }
  bool selected(int i) const {
    std::string cur = current_value();
    if (tiles_[size_t(i)].value == cur) return true;
    // The same picture through another path (symbolic link).
    if (cur != "auto" && tiles_[size_t(i)].value != "auto") {
      char a[PATH_MAX], b[PATH_MAX];
      return realpath(cur.c_str(), a) && realpath(tiles_[size_t(i)].value.c_str(), b) &&
             strcmp(a, b) == 0 && !has_value(cur);
    }
    return false;
  }
  bool has_value(const std::string &v) const {
    for (auto &t : tiles_)
      if (t.value == v) return true;
    return false;
  }

  void build_tiles() {
    tiles_.clear();
    Tile autot;
    autot.value = "auto";
    autot.path = default_wallpaper();
    autot.label = _("Automatic");
    tiles_.push_back(autot);
    std::vector<Wallpaper> list;
    {
      WallpaperScan &s = wallpaper_scan();
      std::lock_guard<std::mutex> lock(s.mu);
      list = s.list;
    }
    // A picture chosen with Browse... stays offered right after Automatic.
    std::string cur = current_value();
    bool listed = cur == "auto";
    char rc[PATH_MAX];
    std::string cur_real = cur != "auto" && realpath(cur.c_str(), rc) ? rc : cur;
    for (auto &w : list) {
      char rw[PATH_MAX];
      if (w.path == cur || (realpath(w.path.c_str(), rw) && cur_real == rw)) listed = true;
    }
    if (!listed && file_exists(cur)) {
      Tile t;
      t.value = t.path = cur;
      t.label = wallpaper_label(cur);
      struct stat st;
      if (stat(cur.c_str(), &st) == 0) t.mtime = st.st_mtime;
      tiles_.push_back(t);
    }
    for (auto &w : list) {
      Tile t;
      t.value = t.path = w.path;
      t.label = w.label;
      t.mtime = w.mtime;
      tiles_.push_back(t);
    }
  }

  void tile_box(int i, int &tx, int &ty) const {
    tx = x() + (i % cols_) * (tw_ + gap_);
    ty = y() + (i / cols_) * row_h();
  }

  // Asks for the thumbnails of the visible rows (and one row around them).
  void request_visible(int top, int bottom) {
    std::vector<gallery::ThumbnailService::Request> jobs;
    int first = std::max(0, (top - y()) / row_h() - 1) * cols_;
    int last = std::min(count(), ((bottom - y()) / row_h() + 2) * cols_);
    for (int i = first; i < last; ++i) {
      const Tile &t = tiles_[size_t(i)];
      if (g_thumbs.count(t.path)) continue;
      gallery::ThumbnailService::Request r;
      r.path = t.path;
      r.mtime = t.mtime;
      if (!r.mtime) {
        struct stat st;
        if (stat(t.path.c_str(), &st) == 0) r.mtime = st.st_mtime;
      }
      r.kind = gallery::JobKind::Large;
      jobs.push_back(r);
    }
    if (!jobs.empty()) thumbnailer().replace_queue(std::move(jobs));
  }

  void draw() override {
    int top = parent() ? parent()->y() : y(), bottom = parent() ? top + parent()->h() : y() + h();
    request_visible(top, bottom);
    int first = std::max(0, (top - y()) / row_h()) * cols_;
    int last = std::min(count(), ((bottom - y()) / row_h() + 1) * cols_);
    {
      Painter p(window()->w(), window()->h());
      for (int i = first; i < last; ++i) {
        int tx, ty;
        tile_box(i, tx, ty);
        const Tile &t = tiles_[size_t(i)];
        auto it = g_thumbs.find(t.path);
        cairo_surface_t *thumb = it == g_thumbs.end() ? nullptr : it->second;
        cairo_save(p.cr());
        p.rounded_rect(tx, ty, tw_, th_, Sd(8));
        cairo_clip(p.cr());
        if (thumb) {
          // Zoom: cover the tile, centered.
          double iw = cairo_image_surface_get_width(thumb), ih = cairo_image_surface_get_height(thumb);
          double sc = std::max(tw_ / iw, th_ / ih);
          cairo_translate(p.cr(), tx + (tw_ - iw * sc) / 2, ty + (th_ - ih * sc) / 2);
          cairo_scale(p.cr(), sc, sc);
          cairo_set_source_surface(p.cr(), thumb, 0, 0);
          cairo_pattern_set_filter(cairo_get_source(p.cr()), CAIRO_FILTER_GOOD);
          cairo_paint(p.cr());
        } else {
          p.fill_rect(tx, ty, tw_, th_, g_theme.hover);
          // Picture placeholder: a mountain and a sun.
          Color c = g_theme.text_dim.alpha(it == g_thumbs.end() ? 0.35 : 0.6);
          double cx = tx + tw_ / 2.0, cy = ty + th_ / 2.0, s = std::min(tw_, th_) * 0.32;
          p.stroke_rounded(cx - s, cy - s * .7, s * 2, s * 1.4, s * .15, c, Sd(1.5));
          p.circle(cx + s * .45, cy - s * .25, s * .16, c);
          cairo_move_to(p.cr(), cx - s * .8, cy + s * .55);
          cairo_line_to(p.cr(), cx - s * .2, cy - s * .1);
          cairo_line_to(p.cr(), cx + s * .2, cy + s * .3);
          cairo_line_to(p.cr(), cx + s * .45, cy + s * .1);
          cairo_line_to(p.cr(), cx + s * .8, cy + s * .55);
          cairo_close_path(p.cr());
          p.set(c);
          cairo_fill(p.cr());
        }
        cairo_restore(p.cr());
        bool sel = selected(i);
        if (sel) {
          p.stroke_rounded(tx - Sd(3), ty - Sd(3), tw_ + Sd(6), th_ + Sd(6), Sd(10), g_theme.accent,
                           Sd(3));
          // Check mark badge.
          double bx = tx + tw_ - Sd(14), by = ty + Sd(14);
          p.circle(bx, by, Sd(10), g_theme.accent);
          p.line(bx - Sd(4.5), by, bx - Sd(1), by + Sd(3.5), g_theme.on_accent, Sd(2));
          p.line(bx - Sd(1), by + Sd(3.5), bx + Sd(5), by - Sd(3.5), g_theme.on_accent, Sd(2));
        } else if (i == hover_) {
          p.stroke_rounded(tx - Sd(2), ty - Sd(2), tw_ + Sd(4), th_ + Sd(4), Sd(9),
                           g_theme.accent.alpha(.55), Sd(2));
        } else {
          p.stroke_rounded(tx, ty, tw_, th_, Sd(8), g_theme.border, Sd(1));
        }
        if (Fl::focus() == this && i == focus_) focus_ring(p, tx, ty, tw_, th_, Sd(8));
      }
    }
    for (int i = first; i < last; ++i) {
      int tx, ty;
      tile_box(i, tx, ty);
      bool sel = selected(i);
      text_font(g_theme.font_size - 1, sel);
      draw_text(tiles_[size_t(i)].label, tx, ty + th_ + S(2), tw_, label_h_ - S(4),
                sel ? g_theme.accent_text : g_theme.text_dim, 1);
    }
  }

  void pick(int i) {
    if (i < 0 || i >= count()) return;
    g_cfg.set("appearance", "wallpaper", tiles_[size_t(i)].value);
    apply_change();
    redraw();
  }
  int index_at(int ex, int ey) const {
    int col = (ex - x()) / (tw_ + gap_), row = (ey - y()) / row_h();
    if (ex < x() || ey < y() || col >= cols_) return -1;
    int tx = x() + col * (tw_ + gap_), ty = y() + row * row_h();
    if (ex >= tx + tw_ || ey >= ty + th_ + label_h_) return -1;
    int i = row * cols_ + col;
    return i < count() ? i : -1;
  }
  // Keeps the focused tile on screen while moving with the keyboard.
  void reveal(int i) {
    auto *scroll = dynamic_cast<Fl_Scroll *>(parent());
    if (!scroll) return;
    int tx, ty;
    tile_box(i, tx, ty);
    int top = scroll->y(), bottom = scroll->y() + scroll->h();
    int pos = scroll->yposition();
    if (ty - S(8) < top)
      scroll->scroll_to(scroll->xposition(), std::max(0, pos - (top - ty + S(8))));
    else if (ty + row_h() > bottom)
      scroll->scroll_to(scroll->xposition(), pos + (ty + row_h() - bottom));
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
        take_focus();
        return 1;
      case FL_RELEASE: {
        int i = index_at(Fl::event_x(), Fl::event_y());
        if (i >= 0) {
          focus_ = i;
          pick(i);
        }
        return 1;
      }
      case FL_ENTER:
      case FL_MOVE: {
        int i = index_at(Fl::event_x(), Fl::event_y());
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
        int k = Fl::event_key(), n = count(), f = focus_;
        if (k == FL_Left && f > 0)
          --f;
        else if (k == FL_Right && f + 1 < n)
          ++f;
        else if (k == FL_Up && f - cols_ >= 0)
          f -= cols_;
        else if (k == FL_Down && f + cols_ < n)
          f += cols_;
        else if (k == FL_Home)
          f = 0;
        else if (k == FL_End)
          f = n - 1;
        else if (k == ' ' || k == FL_Enter || k == FL_KP_Enter) {
          pick(focus_);
          return 1;
        } else
          return 0;
        focus_ = f;
        reveal(f);
        redraw();
        return 1;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  std::vector<Tile> tiles_;
  int tw_ = 0, th_ = 0, cols_ = 1, gap_ = 0, label_h_ = 0, focus_ = 0, hover_ = -1;
};

void thumbnails_ready(void *) {
  if (!g_thumbnailer) return;
  bool any = false;
  for (auto &r : g_thumbnailer->take_results()) {
    auto it = g_thumbs.find(r.path);
    if (it != g_thumbs.end()) {
      if (r.surface) cairo_surface_destroy(r.surface);
      continue;
    }
    g_thumbs[r.path] = r.surface;
    any = true;
  }
  if (any && g_wallpaper_grid) g_wallpaper_grid->redraw();
}

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
  // The sidebar caption above this page when it begins a group of pages
  // ("" for a small gap without a caption); nullptr inside a group.
  const char *section;
};

const std::vector<PageInfo> &pages();
void show_page(int index);
// Shows page |id|, scrolled to its |anchor| (see Page::anchor()) if given.
void open_page(const char *id, const char *anchor = nullptr) {
  for (size_t i = 0; i < pages().size(); ++i)
    if (std::string(pages()[i].id) == id) {
      g_open_anchor = anchor ? anchor : "";
      show_page(int(i));
      return;
    }
}

// Native category symbols remain distinct even without an optional icon theme.
void settings_glyph(Painter &p, const std::string &id, double x, double y, double size,
                    const Color &c) {
  // The overview's tools and the newer pages (users, start-menu, date-time).
  if (hub_glyph(p, id, x, y, size, c)) return;
  if (id == "appearance") {
    glyph::sun(p, x, y, size, c);
    return;
  }
  if (id == "action-center") {
    glyph::gear(p, x, y, size, c);
    return;
  }
  if (id == "clock") {
    // A clock face with its hands at ten past ten.
    double r = size * .46, cx = x + size / 2, cy = y + size / 2;
    p.stroke_rounded(cx - r, cy - r, 2 * r, 2 * r, r, c, size * .08);
    p.line(cx, cy, cx - r * .45, cy - r * .3, c, size * .08);
    p.line(cx, cy, cx + r * .1, cy - r * .62, c, size * .07);
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
  if (id == "power") {
    glyph::battery(p, x, y, size, 70, false, c, c);
    return;
  }
  if (id == "lock") {
    glyph::lock(p, x, y, size, c);
    return;
  }
  if (id == "sound") {
    glyph::speaker(p, x, y, size, 3, false, c);
    return;
  }
  if (id == "network") {
    glyph::wifi(p, x, y, size, 4, c);
    return;
  }
  if (id == "bluetooth") {
    glyph::bluetooth(p, x, y, size, c);
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
  if (id == "default-apps") {
    // A window with a check mark: "the one that opens it".
    p.stroke_rounded(x, y + size * .08, size, size * .84, size * .14, c, size * .075);
    glyph::check(p, x + size * .2, y + size * .22, size * .6, c);
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

class PanelStyleCard : public Fl_Widget {
 public:
  PanelStyleCard(int x, int y, int w, int h, std::string style, std::string title)
      : Fl_Widget(x, y, w, h), style_(std::move(style)), title_(std::move(title)) {
    tooltip(title_.c_str());
  }
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

void reload_compositor_later(void *) {
  write_compositor_config(g_cfg);
  write_compositor_config(g_cfg, true);
  reload_compositor();
}
void aero_changed();

// Transparency preset (Off, Subtle, Clear, Glass): a panel and a menu over a
// wallpaper, drawn with the opacity that preset gives the current family.
class TransparencyCard : public PickCard {
 public:
  TransparencyCard(int x, int y, int w, int h, const TransparencyPreset &p)
      : PickCard(x, y, w, h), id_(p.id), label_(p.label), value_(p.value) {}
  bool selected() const override {
    return g_cfg.get_bool("aero", "enabled", true) && g_theme.transparency == value_;
  }
  void pick() override {
    g_cfg.set("aero", "enabled", "true");
    g_cfg.set("aero", "transparency", std::to_string(value_));
    aero_changed();
    Fl::add_timeout(0, [](void *) { refresh_page(); });
  }
  void draw() override {
    bool on = selected();
    Config c = g_cfg;
    c.set("aero", "enabled", "true");
    c.set("aero", "transparency", std::to_string(value_));
    Theme t = Theme::from_config(c);
    int pv = h() - S(30);
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(10), hot_ ? g_theme.hover : g_theme.surface_alt);
      double in = Sd(6), px = x() + in, py = y() + in, pw = w() - 2 * in, ph = pv - in;
      cairo_save(p.cr());
      p.rounded_rect(px, py, pw, ph, Sd(7));
      cairo_clip(p.cr());
      // A busy backdrop shows how much comes through: stripes and a sun.
      // (Muted, natural colors since alpha51: a dusky sky over sand.)
      p.fill_gradient(px, py, pw, ph, 0, t.dark ? Color::hex(0x34475A) : Color::hex(0x8FA9BF),
                      t.dark ? Color::hex(0x5A4A3C) : Color::hex(0xCDB79A), true);
      for (int i = 0; i < 6; ++i)
        p.fill_rect(px + i * pw / 6, py, pw / 12, ph, Color::hex(0xFFFFFF).alpha(0.16));
      p.circle(px + pw * 0.72, py + ph * 0.35, ph * 0.22, Color::hex(0xE9D3A0));
      // A menu with text, at the popup opacity, and the panel at the bottom.
      double mx = px + pw * 0.12, my = py + ph * 0.14, mw = pw * 0.56, mh = ph * 0.52;
      p.fill_rounded(mx, my, mw, mh, Sd(4), t.menu_bg.alpha(t.popup_opacity));
      for (int i = 0; i < 3; ++i)
        p.fill_rounded(mx + Sd(6), my + Sd(6) + i * mh / 3.4, mw * (i == 1 ? 0.5 : 0.66), Sd(3), Sd(1.5),
                       t.menu_text.alpha(t.popup_opacity));
      double bh = ph * 0.2;
      p.fill_rect(px, py + ph - bh, pw, bh, t.panel_bg.alpha(t.opacity));
      for (int i = 0; i < 4; ++i)
        p.fill_rounded(px + Sd(6) + i * Sd(12), py + ph - bh + bh * 0.25, Sd(8), bh * 0.5, Sd(2),
                       (i == 0 ? g_theme.accent : t.text_dim).alpha(t.opacity));
      cairo_restore(p.cr());
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(10), on ? g_theme.accent : g_theme.border,
                       on ? Sd(2) : Sd(1));
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), Sd(10));
    }
    text_font(g_theme.font_size, on);
    draw_text(_(label_.c_str()), x() + S(10), y() + pv, w() - S(20), S(28), on ? g_theme.accent_text : g_theme.text);
  }

 private:
  std::string id_, label_;
  int value_;
};

// An installed icon theme: five typical icons (folder, home, settings,
// terminal, picture) drawn from that theme and its name. "auto" shows the
// theme Automatic resolves to in the current mode.
class IconThemeCard : public PickCard {
 public:
  IconThemeCard(int x, int y, int w, int h, std::string id, std::string label, std::string detail)
      : PickCard(x, y, w, h), id_(std::move(id)), label_(std::move(label)), detail_(std::move(detail)) {
    tooltip(detail_.empty() ? label_.c_str() : detail_.c_str());
  }
  ~IconThemeCard() override {
    for (auto *s : icons_)
      if (s) cairo_surface_destroy(s);
  }
  bool selected() const override {
    std::string cur = trim(g_cfg.get("appearance", "icon_theme", "auto"));
    if (cur.empty()) cur = "auto";
    if (id_ == "auto") return lower(cur) == "auto" || resolve_icon_theme(cur, g_theme.dark) != cur;
    return cur == id_;
  }
  void pick() override {
    g_cfg.set("appearance", "icon_theme", id_);
    IconLoader::set_theme(id_, g_theme.dark);
    IconLoader::clear_cache();
    apply_change();
    Fl::add_timeout(0, [](void *) { refresh_page(); });
  }
  void draw() override {
    static const char *const names[][3] = {{"folder", "inode-directory", nullptr},
                                           {"user-home", "folder-home", nullptr},
                                           {"preferences-system", "preferences-desktop", "applications-system"},
                                           {"utilities-terminal", "terminal", "org.gnome.Terminal"},
                                           {"image-x-generic", "image", "applications-graphics"}};
    int isz = std::min(S(28), (w() - S(24)) / 5 - S(4));
    if (icons_.empty() || size_ != isz) {
      for (auto *s : icons_)
        if (s) cairo_surface_destroy(s);
      icons_.clear();
      size_ = isz;
      std::string theme = id_ == "auto" ? resolve_icon_theme("auto", g_theme.dark) : id_;
      for (const auto &alts : names) {
        cairo_surface_t *s = nullptr;
        for (const char *n : alts) {
          if (!n) break;
          std::string path = IconLoader::lookup_in(theme, n, isz);
          if (!path.empty() && (s = load_image(path, isz, isz))) break;
        }
        icons_.push_back(s);
      }
    }
    bool on = selected();
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), S(10), hot_ ? g_theme.hover : g_theme.surface_alt);
      int step = (w() - S(16)) / 5;
      for (size_t i = 0; i < icons_.size(); ++i) {
        if (!icons_[i]) continue;
        double ix = x() + S(8) + int(i) * step + (step - size_) / 2.0, iy = y() + S(12);
        int iw = cairo_image_surface_get_width(icons_[i]), ih = cairo_image_surface_get_height(icons_[i]);
        cairo_save(p.cr());
        cairo_translate(p.cr(), ix, iy);
        if (iw > 0 && ih > 0) cairo_scale(p.cr(), double(size_) / iw, double(size_) / ih);
        cairo_set_source_surface(p.cr(), icons_[i], 0, 0);
        cairo_paint(p.cr());
        cairo_restore(p.cr());
      }
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, S(10), on ? g_theme.accent : g_theme.border,
                       on ? Sd(2) : Sd(1));
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), h(), Sd(10));
    }
    text_font(g_theme.font_size, on);
    draw_text(label_, x() + S(10), y() + S(16) + size_, w() - S(20), S(22), on ? g_theme.accent_text : g_theme.text);
    if (!detail_.empty()) {
      text_font(std::max(S(10), g_theme.font_size - S(2)));
      draw_text(detail_, x() + S(10), y() + S(36) + size_, w() - S(20), S(18), g_theme.text_dim);
    }
  }

 private:
  std::string id_, label_, detail_;
  std::vector<cairo_surface_t *> icons_;
  int size_ = 0;
};

Label *g_transparency_note = nullptr;

// "Panel 82 % · Menus 88 % · Window frames 94 % opaque" for the slider hint.
std::string transparency_summary(const Theme &t) {
  if (!t.translucent || t.transparency <= 0) return _("Every surface is opaque.");
  auto pc = [](double v) { return int(std::lround(v * 100)); };
  return format(_("Panel %d %%, menus %d %%, window frames %d %% opaque. At every level text keeps "
                  "its contrast (WCAG AA) over any background."),
                pc(t.opacity), pc(std::min(t.popup_opacity, t.gtk_menu_alpha)), pc(t.frame_opacity));
}

void build_appearance(Page *page) {
  g_transparency_note = nullptr;
  page->heading(_("Appearance"), _("Theme, colors, translucency, size and wallpaper"));
  page->begin_card(_("Theme"));
  auto fams = theme_families();
  int gap = S(12), n = int(fams.size());
  int cw = std::min(S(220), (page->width() - S(40) - (n - 1) * gap) / n);
  int y = page->cursor();
  for (int i = 0; i < n; ++i)
    new FamilyCard(page->left() + S(20) + i * (cw + gap), y, cw, S(164), fams[size_t(i)].id,
                   fams[size_t(i)].label, fams[size_t(i)].description);
  page->advance(S(176));
  page->row(_("Mode"), S(3 * 104 + 2 * 8), S(40), [](int x, int yy, int w, int h) {
    auto *g = new Fl_Group(x, yy, w, h);
    const char *modes[] = {"light", "dark", "auto"};
    for (int i = 0; i < 3; ++i) new ModeCard(x + i * S(112), yy, S(104), h, modes[i]);
    g->end();
    return g;
  }, _("Auto is light from 07:00 to 19:00 and dark in the evening."));
  page->row(_("Accent color"), S(36) * int(theme_accents().size()), S(38),
            [](int x, int yy, int w, int h) { return new AccentPicker(x, yy, w, h); });
  {
    std::string based = g_theme.family == "fluent"
                             ? _("Based on Flat Remix GTK and Flat Remix GNOME by Daniel Ruiz de Alegría.")
                             : _("Based on the Graphite GTK theme by Vince Liu.");
    page->note(format(_("Applications, menus, window frames and SDE use %s (GTK 3, GTK 4, libadwaita "
                        "and SDE WM). %s"),
                      g_theme.gtk_theme.c_str(), based.c_str()));
  }
  page->end_card();

  page->begin_card(_("Transparency and clarity"));
  {
    // Presets: Off, Subtle, Clear, Glass. One setting drives the panel,
    // menus, Start menu, Action Center, popups, window frames and GTK menus.
    const auto &presets = transparency_presets();
    int pn = int(presets.size()), pgap = S(10);
    int pw = std::min(S(150), (page->width() - S(40) - (pn - 1) * pgap) / pn);
    int py = page->cursor();
    for (int i = 0; i < pn; ++i)
      new TransparencyCard(page->left() + S(20) + i * (pw + pgap), py, pw, S(104), presets[size_t(i)]);
    page->advance(S(116));
  }
  page->slider_row(
      _("Transparency"), 0, 100, g_cfg.get_int("aero", "transparency", 25),
      [](double v) {
        g_cfg.set("aero", "transparency", std::to_string(int(std::lround(v))));
        if (!g_cfg.get_bool("aero", "enabled", true) && v > 0) g_cfg.set("aero", "enabled", "true");
        aero_changed();
        if (g_transparency_note) g_transparency_note->set(transparency_summary(g_theme));
      });
  {
    std::string text = transparency_summary(g_theme);
    int lw = page->width() - S(40), lh = Label::measure(text, lw, Label::DIM) + S(14);
    g_transparency_note = new Label(page->left() + S(20), page->cursor(), lw, lh, text, Label::DIM, true);
    page->advance(lh);
  }
  page->switch_row(
      _("Translucent surfaces"), g_cfg.get_bool("aero", "enabled", true),
      [](bool on) {
        g_cfg.set("aero", "enabled", on ? "true" : "false");
        aero_changed();
        Fl::add_timeout(0, [](void *) { refresh_page(); });
      },
      _("Acrylic panel and menus in Fluent, vibrancy in Graphite, frosted window frames in "
        "Glass."));
  page->switch_row(
      _("Blur behind glass"), g_cfg.get_bool("aero", "blur", true),
      [](bool on) {
        g_cfg.set("aero", "blur", on ? "true" : "false");
        aero_changed();
      },
      _("Blur keeps text calm and crisp on translucent surfaces. Uses OpenGL when available."));
  if (g_theme.lite)
    page->note(_("This computer uses the lite compositor profile (virtual machine or software "
                 "rendering): blur is off and surfaces stay more opaque so that text stays readable. "
                 "Choose OpenGL or XRender in Effects to use every effect."));
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
  page->end_card();

  // Icons: one choice for GTK applications, Liria, the desktop, the Start
  // menu, the panel and every SDE application.
  page->begin_card(_("Icons"));
  {
    auto infos = IconLoader::installed_theme_infos();
    std::string auto_theme = resolve_icon_theme("auto", g_theme.dark);
    struct Item {
      std::string id, label, detail;
    };
    std::vector<Item> items;
    items.push_back({"auto", _("Automatic"),
                     auto_theme == "Papirus" || auto_theme == "Papirus-Dark"
                         ? std::string(_("Papirus, light or dark"))
                         : format(_("%s, follows the mode"), auto_theme.c_str())});
    for (const auto &i : infos) {
      std::string detail = i.id == "Papirus" ? _("Recommended")
                           : i.id == "Papirus-Dark" ? _("For dark panels and themes")
                           : i.id == "Papirus-Light" ? _("For light panels")
                           : i.id == "Adwaita" ? _("GNOME")
                                               : i.comment;
      if (detail.size() > 40) detail = detail.substr(0, 37) + "...";
      items.push_back({i.id, i.name, detail});
    }
    int gap = S(10), cols = std::max(1, (page->width() - S(40) + gap) / (S(196) + gap));
    int cw = (page->width() - S(40) - (cols - 1) * gap) / cols, ch = S(86);
    for (size_t k = 0; k < items.size(); ++k) {
      int col = int(k) % cols, rowi = int(k) / cols;
      new IconThemeCard(page->left() + S(20) + col * (cw + gap), page->cursor() + rowi * (ch + gap), cw, ch,
                        items[k].id, items[k].label, items[k].detail);
    }
    int rows = (int(items.size()) + cols - 1) / cols;
    page->advance(rows * (ch + gap) + S(4));
    page->note(format(_("In use: %s. GTK applications, Liria, the desktop, the Start menu, the panel and "
                        "SDE applications follow this choice; missing icons come from the theme's parents, "
                        "Adwaita and hicolor."),
                      IconLoader::theme().c_str()));
    if (std::none_of(infos.begin(), infos.end(), [](const IconThemeInfo &i) { return i.id == "Papirus"; }))
      page->note(_("Install papirus-icon-theme for the recommended SDE icons."));
  }
  page->end_card();

  page->anchor("wallpaper");
  page->begin_card(_("Wallpaper"));
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
  page->row(_("Your own picture"), S(160), S(34), [](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Browse..."));
    b->callback([](Fl_Widget *, void *) {
      static std::string filter;
      filter = "Images (*.{png,jpg,jpeg,svg";
      for (auto &e : gallery::extra_formats()) filter += "," + e;
      filter += "})";
      const char *f = fl_file_chooser(_("Choose a wallpaper"), filter.c_str(),
                                      (home_dir() + "/Pictures").c_str());
      if (!f) return;
      g_cfg.set("appearance", "wallpaper", f);
      apply_change();
      Fl::add_timeout(0, [](void *) { refresh_page(); });
    });
    return b;
  });
  page->advance(S(6));
  start_wallpaper_scan();
  auto *grid = new WallpaperGrid(page->left() + S(20), page->cursor(), page->width() - S(40));
  page->advance(grid->h() + S(12));
  bool scanned;
  {
    std::lock_guard<std::mutex> lock(wallpaper_scan().mu);
    scanned = wallpaper_scan().done;
  }
  if (!scanned)
    page->note(_("Looking for wallpapers..."));
  else
    page->note(format(_("%d wallpapers from /usr/share/backgrounds, desktop-base themes and "
                        "~/Pictures/Wallpapers. Click one to use it."),
                      grid->count() - 1));
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
  {
    // alpha52: also in the search's right-click menu on the panel.
    std::string mode = g_cfg.get("panel", "search", "auto");
    int index = mode == "box" ? 1 : mode == "icon" ? 2 : mode == "hidden" ? 3 : 0;
    page->choice_row(
        _("Search"), {_("Automatic"), _("Search box"), _("Search icon only"), _("Hidden")}, index,
        [](int v) {
          static const char *vals[] = {"auto", "box", "icon", "hidden"};
          if (v >= 0 && v < 4) g_cfg.set("panel", "search", vals[v]);
          apply_change();
        },
        _("Hidden: Super still opens the Start menu, which searches as you type."));
  }
  page->end_card();

  page->begin_card(_("Clock and status"));
  page->switch_row(_("24-hour clock"), g_cfg.get_bool("panel", "clock_24h", true),
                   [](bool on) {
                     g_cfg.set("clock", "format", on ? "24h" : "12h");
                     set_bool("panel", "clock_24h", on);
                   });
  page->switch_row(_("Show the date"), g_cfg.get_bool("panel", "show_date", true),
                   [](bool on) { set_bool("panel", "show_date", on); });
  page->switch_row(_("Show Desktop button"), g_cfg.get_bool("panel", "show_desktop_button", false),
                   [](bool on) { set_bool("panel", "show_desktop_button", on); });
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
           {"show_tray", _("System tray button")},
           {"show_notifications", _("Notifications button")}})
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
  {
    // alpha52: one [clock] style for the panel and the Action Center; more
    // on the Clock & Weather page.
    std::string style = g_cfg.get("action_center", "clock_style", "");
    if (trim(style).empty()) style = g_cfg.get("clock", "style", "digital");
    style = lower(trim(style));
    page->choice_row(_("Clock style"), {_("Digital"), _("Blink"), _("LED"), _("Analog")},
                     style == "blink" ? 1 : style == "led" ? 2 : style == "analog" ? 3 : 0,
                     [](int v) {
                       static const char *values[] = {"digital", "blink", "led", "analog"};
                       g_cfg.set("action_center", "clock_style", values[v < 0 || v > 3 ? 0 : v]);
                       g_cfg.unset("clock", "style");
                       apply_change();
                     });
  }
  page->switch_row(_("Show a greeting"), g_cfg.get_bool("action_center", "show_greeting", true),
                   [](bool on) { set_bool("action_center", "show_greeting", on); });
  page->switch_row(_("24-hour clock"), g_cfg.get_bool("panel", "clock_24h", true),
                   [](bool on) {
                     g_cfg.set("clock", "format", on ? "24h" : "12h");
                     set_bool("panel", "clock_24h", on);
                   });
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
  page->switch_row(
      _("Computer"), g_cfg.get_bool("desktop", "show_computer", false),
      [](bool on) { set_bool("desktop", "show_computer", on); },
      _("Drives, devices and network places in the file manager."));
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
    w.str(_("Control Center"))
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
  page->heading(_("Applications"), _("Applications of the Selviana Desktop Environment"));
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
      _("Original application licenses and copyrights are retained. Software Updater credits mintupdate; "
        "PackageKit "
        "performs Debian transactions."));
  page->finish();
}

// ------------------------------------------- sound, network and bluetooth
//
// These pages read their state with pactl, nmcli and bluetoothctl in the
// background (never blocking the window) and rebuild once it arrives. The
// panel's status flyouts open them with "sde-settings --page NAME".

// Runs the |steps| one after another, then rebuilds |page_id| when shown.
struct Loader {
  bool loading = false, loaded = false;
};

void refresh_if(const char *page_id) {
  if (g_restart_page == page_id) Fl::add_timeout(0, [](void *) { refresh_page(); });
}

struct SoundState : Loader {
  std::vector<AudioDevice> sinks, sources;
  std::string sink, source;
  int volume = -1, mic_volume = -1;
  bool muted = false, mic_muted = false;
};
SoundState g_sound;

void load_sound() {
  if (g_sound.loading) return;
  g_sound.loading = true;
  auto *st = new SoundState;
  auto step = std::make_shared<int>(0);
  // A chain of pactl calls; each callback starts the next.
  struct Call {
    std::vector<std::string> argv;
    std::function<void(const std::string &)> take;
  };
  auto calls = std::make_shared<std::vector<Call>>(std::vector<Call>{
      {{"pactl", "list", "sinks"}, [st](const std::string &o) { st->sinks = parse_pactl_devices(o); }},
      {{"pactl", "list", "sources"},
       [st](const std::string &o) { st->sources = parse_pactl_devices(o); }},
      {{"pactl", "get-default-sink"}, [st](const std::string &o) { st->sink = trim(o); }},
      {{"pactl", "get-default-source"}, [st](const std::string &o) { st->source = trim(o); }},
      {{"pactl", "get-sink-volume", "@DEFAULT_SINK@"},
       [st](const std::string &o) { st->volume = first_percent(o); }},
      {{"pactl", "get-sink-mute", "@DEFAULT_SINK@"},
       [st](const std::string &o) { st->muted = parse_pactl_mute(o); }},
      {{"pactl", "get-source-volume", "@DEFAULT_SOURCE@"},
       [st](const std::string &o) { st->mic_volume = first_percent(o); }},
      {{"pactl", "get-source-mute", "@DEFAULT_SOURCE@"},
       [st](const std::string &o) { st->mic_muted = parse_pactl_mute(o); }},
  });
  auto next = std::make_shared<std::function<void()>>();
  *next = [st, step, calls, next] {
    if (*step >= int(calls->size())) {
      g_sound = *st;
      g_sound.loaded = true;
      g_sound.loading = false;
      delete st;
      *next = nullptr;  // breaks the reference cycle
      refresh_if("sound");
      return;
    }
    Call &c = (*calls)[size_t(*step)];
    ++*step;
    auto take = c.take;
    auto again = next;
    if (!run_async(with_timeout(c.argv), [take, again](const std::string &out, int status) {
          if (status == 0) take(out);
          if (*again) (*again)();
        }))
      (*next)();
  };
  (*next)();
}

void pactl_then_reload(std::vector<std::string> argv) {
  run_async(with_timeout(std::move(argv)), [](const std::string &, int) { load_sound(); });
}

void build_sound(Page *page) {
  page->heading(_("Sound"), _("Speakers, headphones and microphones"));
  if (find_program("pactl").empty()) {
    page->note(_("Sound settings need pactl (package pulseaudio-utils) and PipeWire or "
                 "PulseAudio."));
    page->finish();
    return;
  }
  if (!g_refreshing || !g_sound.loaded) load_sound();
  if (!g_sound.loaded) {
    page->note(_("Reading sound devices..."));
    page->finish();
    return;
  }
  static std::vector<std::string> sink_names, source_names;
  page->begin_card(_("Output"));
  if (g_sound.sinks.empty()) {
    page->note(_("No sound output was found. Is PipeWire or PulseAudio running?"));
  } else {
    std::vector<std::string> labels;
    int cur = 0;
    sink_names.clear();
    for (size_t i = 0; i < g_sound.sinks.size(); ++i) {
      labels.push_back(g_sound.sinks[i].description);
      sink_names.push_back(g_sound.sinks[i].name);
      if (g_sound.sinks[i].name == g_sound.sink) cur = int(i);
    }
    page->choice_row(_("Output device"), labels, cur, [](int v) {
      if (v >= 0 && v < int(sink_names.size()))
        pactl_then_reload({"pactl", "set-default-sink", sink_names[size_t(v)]});
    });
    page->slider_row(_("Volume"), 0, 150, std::max(0, g_sound.volume), [](double v) {
      pactl_then_reload({"pactl", "set-sink-volume", "@DEFAULT_SINK@",
                         std::to_string(int(std::lround(v))) + "%"});
    }, _("Above 100% the sound is amplified and may distort."));
    page->switch_row(_("Mute"), g_sound.muted, [](bool on) {
      pactl_then_reload({"pactl", "set-sink-mute", "@DEFAULT_SINK@", on ? "1" : "0"});
    });
  }
  page->end_card();
  page->begin_card(_("Input"));
  if (g_sound.sources.empty()) {
    page->note(_("No microphone was found."));
  } else {
    std::vector<std::string> labels;
    int cur = 0;
    source_names.clear();
    for (size_t i = 0; i < g_sound.sources.size(); ++i) {
      labels.push_back(g_sound.sources[i].description);
      source_names.push_back(g_sound.sources[i].name);
      if (g_sound.sources[i].name == g_sound.source) cur = int(i);
    }
    page->choice_row(_("Input device"), labels, cur, [](int v) {
      if (v >= 0 && v < int(source_names.size()))
        pactl_then_reload({"pactl", "set-default-source", source_names[size_t(v)]});
    });
    page->slider_row(_("Input volume"), 0, 100, std::max(0, g_sound.mic_volume), [](double v) {
      pactl_then_reload({"pactl", "set-source-volume", "@DEFAULT_SOURCE@",
                         std::to_string(int(std::lround(v))) + "%"});
    });
    page->switch_row(_("Mute microphone"), g_sound.mic_muted, [](bool on) {
      pactl_then_reload({"pactl", "set-source-mute", "@DEFAULT_SOURCE@", on ? "1" : "0"});
    });
  }
  page->end_card();
  if (!find_program("pavucontrol").empty())
    page->row(_("Per-application volume and profiles"), S(160), S(34),
              [](int x, int y, int w, int h) {
                return action_button(x, y, w, h, _("Volume Control"), {"pavucontrol"});
              });
  page->note(_("The sound icon on the panel also changes the volume and the output device."));
  page->finish();
}

struct NetworkState : Loader {
  std::vector<NetDevice> devices;
  bool wifi = true, airplane = false;
};
NetworkState g_net;

void load_network() {
  if (g_net.loading) return;
  g_net.loading = true;
  run_async(with_timeout({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device",
                          "status"}),
            [](const std::string &out, int) {
              auto devices = parse_nmcli_devices(out);
              run_async(with_timeout({"nmcli", "radio", "all"}),
                        [devices](const std::string &radio, int status) {
                          g_net.devices = devices;
                          // "WIFI-HW  WIFI     WWAN-HW  WWAN": second column.
                          std::istringstream in(radio);
                          std::string line, last;
                          while (std::getline(in, line))
                            if (!trim(line).empty()) last = line;
                          std::istringstream f(last);
                          std::string hw, wifi;
                          f >> hw >> wifi;
                          g_net.wifi = status != 0 || wifi != "disabled";
                          g_net.loaded = true;
                          g_net.loading = false;
                          refresh_if("network");
                        });
            });
}

std::string net_type_label(const std::string &type) {
  if (type == "wifi") return _("Wi-Fi");
  if (type == "ethernet") return _("Wired");
  if (type == "gsm" || type == "cdma") return _("Mobile broadband");
  if (type == "bt") return _("Bluetooth");
  if (type == "wireguard" || type == "vpn") return _("VPN");
  return type;
}

void build_network(Page *page) {
  page->heading(_("Network"), _("Wi-Fi, wired and mobile connections"));
  if (find_program("nmcli").empty()) {
    page->note(_("Network settings need NetworkManager (package network-manager)."));
    build_network_diagnostics(page);
    build_network_proxy(page);
    page->finish();
    return;
  }
  if (!g_refreshing || !g_net.loaded) load_network();
  if (!g_net.loaded) {
    page->note(_("Reading network devices..."));
    page->finish();
    return;
  }
  page->begin_card(_("Wireless"));
  page->switch_row(
      _("Wi-Fi"), g_net.wifi,
      [](bool on) {
        run_async(with_timeout({"nmcli", "radio", "wifi", on ? "on" : "off"}),
                  [](const std::string &, int) { load_network(); });
      },
      _("Choose a Wi-Fi network from the network icon on the panel."));
  page->row(_("Wi-Fi networks"), S(160), S(34), [](int x, int y, int w, int h) {
    return action_button(x, y, w, h, _("Show Networks"), {"sde-panel", "--section", "network"},
                         true);
  });
  page->end_card();
  page->begin_card(_("Devices"));
  if (g_net.devices.empty()) page->note(_("No network devices were found."));
  for (auto &d : g_net.devices) {
    bool connected = starts_with(d.state, "connected");
    std::string state = connected && !d.connection.empty()
                            ? format(_("Connected to %s"), d.connection.c_str())
                        : connected                     ? std::string(_("Connected"))
                        : d.state == "unavailable"      ? std::string(_("Cable unplugged or off"))
                        : d.state == "unmanaged"        ? std::string(_("Not managed"))
                                                        : std::string(_("Disconnected"));
    std::string device = d.device;
    page->row(net_type_label(d.type) + "  ·  " + d.device, S(120), S(34),
              [connected, device, &d](int x, int y, int w, int h) {
                auto *b = new FlatButton(x, y, w, h, connected ? _("Disconnect") : _("Connect"));
                b->callback(
                    [](Fl_Widget *, void *p) {
                      auto *a = static_cast<std::pair<std::string, bool> *>(p);
                      run_async(with_timeout({"nmcli", "device", a->second ? "disconnect" : "connect",
                                              a->first}, 30),
                                [](const std::string &, int) { load_network(); });
                    },
                    keep(new std::pair<std::string, bool>(device, connected)));
                if (d.state == "unavailable" || d.state == "unmanaged") b->deactivate();
                return b;
              },
              state);
  }
  page->end_card();
  build_network_diagnostics(page);
  build_network_proxy(page);
  if (!find_program("nm-connection-editor").empty())
    page->row(_("VPN and advanced connection settings"), S(160), S(34),
              [](int x, int y, int w, int h) {
                return action_button(x, y, w, h, _("Connections"), {"nm-connection-editor"});
              });
  page->finish();
}

struct BluetoothState : Loader {
  bool available = false, powered = false;
  std::string name;
  std::vector<BtDevice> devices;
};
BluetoothState g_bt;

void load_bluetooth() {
  if (g_bt.loading) return;
  g_bt.loading = true;
  run_async(with_timeout({"bluetoothctl", "show"}), [](const std::string &show, int status) {
    auto *st = new BluetoothState;
    st->available = status == 0 && !bluetoothctl_field(show, "Powered").empty();
    st->powered = bluetoothctl_field(show, "Powered") == "yes";
    st->name = bluetoothctl_field(show, "Alias");
    if (st->name.empty()) st->name = bluetoothctl_field(show, "Name");
    auto finish = [st] {
      g_bt = *st;
      g_bt.loaded = true;
      g_bt.loading = false;
      delete st;
      refresh_if("bluetooth");
    };
    if (!st->available) return finish();
    run_async(with_timeout({"bluetoothctl", "devices", "Paired"}),
              [st, finish](const std::string &paired, int) {
                st->devices = parse_bluetoothctl_devices(paired);
                run_async(with_timeout({"bluetoothctl", "devices", "Connected"}),
                          [st, finish](const std::string &conn, int) {
                            for (auto &c : parse_bluetoothctl_devices(conn))
                              for (auto &d : st->devices)
                                if (d.address == c.address) d.connected = true;
                            finish();
                          });
              });
  });
}

void build_bluetooth(Page *page) {
  page->heading(_("Bluetooth"), _("Headphones, speakers, mice, keyboards and phones"));
  if (find_program("bluetoothctl").empty()) {
    page->note(_("Bluetooth settings need BlueZ (package bluez)."));
    page->finish();
    return;
  }
  if (!g_refreshing || !g_bt.loaded) load_bluetooth();
  if (!g_bt.loaded) {
    page->note(_("Looking for the Bluetooth adapter..."));
    page->finish();
    return;
  }
  if (!g_bt.available) {
    page->begin_card(_("Adapter"));
    page->note(_("No Bluetooth adapter was found, or the Bluetooth service is not running."));
    page->end_card();
    page->finish();
    return;
  }
  page->begin_card(_("Adapter"));
  page->switch_row(
      _("Bluetooth"), g_bt.powered,
      [](bool on) {
        run_async(with_timeout({"bluetoothctl", "power", on ? "on" : "off"}),
                  [](const std::string &, int) { load_bluetooth(); });
      },
      g_bt.name.empty() ? std::string() : format(_("Visible to other devices as \"%s\" while "
                                                  "pairing."),
                                                g_bt.name.c_str()));
  page->row(_("Add a device"), S(160), S(34), [](int x, int y, int w, int h) {
    if (!find_program("blueman-manager").empty())
      return action_button(x, y, w, h, _("Add Device..."), {"blueman-manager"}, true);
    return action_button(x, y, w, h, _("Add Device..."), {"sde-panel", "--section", "bluetooth"},
                         true);
  });
  page->end_card();
  page->begin_card(_("Paired devices"));
  if (g_bt.devices.empty()) page->note(_("No devices are paired yet."));
  for (auto &d : g_bt.devices) {
    std::string addr = d.address;
    bool connected = d.connected;
    page->row(d.name, S(120), S(34),
              [addr, connected](int x, int y, int w, int h) {
                auto *b = new FlatButton(x, y, w, h, connected ? _("Disconnect") : _("Connect"));
                b->callback(
                    [](Fl_Widget *, void *p) {
                      auto *a = static_cast<std::pair<std::string, bool> *>(p);
                      run_async(with_timeout({"bluetoothctl", a->second ? "disconnect" : "connect",
                                              a->first}, 20),
                                [](const std::string &, int) { load_bluetooth(); });
                    },
                    keep(new std::pair<std::string, bool>(addr, connected)));
                if (!g_bt.powered) b->deactivate();
                return b;
              },
              connected ? std::string(_("Connected")) + "  ·  " + addr : addr);
  }
  page->end_card();
  page->finish();
}

struct PowerState : Loader {
  std::vector<std::string> profiles;
  std::string current;
};
PowerState g_power;

void load_power_profiles() {
  if (g_power.loading || find_program("powerprofilesctl").empty()) return;
  g_power.loading = true;
  run_async(with_timeout({"powerprofilesctl", "list"}), [](const std::string &out, int status) {
    g_power.profiles.clear();
    g_power.current.clear();
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
      // "* balanced:" marks the active profile.
      std::string t = trim(line);
      if (t.empty() || t.back() != ':' ||
          (t.find(' ') != std::string::npos && !starts_with(t, "* ")))
        continue;
      bool active = starts_with(t, "* ");
      std::string name = trim(t.substr(active ? 2 : 0, t.size() - (active ? 3 : 1)));
      if (name.empty()) continue;
      g_power.profiles.push_back(name);
      if (active) g_power.current = name;
    }
    if (status != 0) g_power.profiles.clear();
    g_power.loaded = true;
    g_power.loading = false;
    refresh_if("system");
  });
}

std::string profile_label(const std::string &p) {
  if (p == "power-saver") return _("Power saver");
  if (p == "balanced") return _("Balanced");
  if (p == "performance") return _("Performance");
  return p;
}

// Date and time: the time zone (with the panel clock's "Sync time zone")
// and network time. The panel's "Date and time settings" link opens this
// page.
void build_date_time(Page *page) {
  if (find_program("timedatectl").empty()) return;
  static bool listening = false;
  if (!listening) {
    listening = true;
    tzsync::Sync::get().set_listener([] {
      refresh_if_shown("system");
      refresh_if_shown("date-time");
    });
  }
  auto &tz = tzsync::Sync::get();
  page->begin_card(_("Date and time"));
  page->row(format(_("Time zone: %s"), tzsync::current_label().c_str()), S(112), S(34),
            [](int x, int y, int w, int h) {
              auto *b = new FlatButton(x, y, w, h, tzsync::Sync::get().busy() ? _("Syncing") : _("Sync"),
                                       true);
              b->callback([](Fl_Widget *, void *) { tzsync::Sync::get().start(true); });
              b->tooltip(_("Detect the time zone from the network, set it and synchronize the clock"));
              if (tzsync::Sync::get().busy()) b->deactivate();
              return b;
            },
            _("Detected from this network's location (ipapi.co or ip-api.com). Needs an "
              "administrator password."));
  if (!tz.message().empty()) page->note(tz.message());
  page->switch_row(
      _("Automatic time zone"), g_cfg.get_bool("clock", "auto_timezone", false),
      [](bool on) { set_bool("clock", "auto_timezone", on); },
      _("The panel sets the time zone from the network at login and when the network changes, at "
        "most once an hour, without asking for a password."));
  page->end_card();
}

// Control Center > Date & Time: the time zone card and the way to the clock.
void build_date_time_page(Page *page) {
  page->heading(_("Date & Time"), _("Time zone, network time and the panel clock"));
  if (find_program("timedatectl").empty())
    page->note(_("Date and time settings need timedatectl (package systemd)."));
  else
    build_date_time(page);
  page->begin_card(_("Clock"));
  page->row(_("Clock style, 12 or 24 hours and the weather"), S(170), S(34),
            [](int x, int y, int w, int h) {
              auto *b = new FlatButton(x, y, w, h, _("Clock & Weather"));
              b->callback([](Fl_Widget *, void *) { open_page("clock"); });
              return b;
            });
  page->end_card();
  page->finish();
}

// Control Center > Start Menu: [menu] in sde.conf, read by the panel each
// time the menu opens.
void build_start_menu_page(Page *page) {
  page->heading(_("Start Menu"), _("Size, application lists and your account"));
  page->begin_card(_("Layout"));
  std::string size = g_cfg.get("menu", "size", "medium");
  bool dragged = g_cfg.get_int("menu", "width", 0) > 0 && g_cfg.get_int("menu", "height", 0) > 0;
  page->choice_row(
      _("Size"), {_("Small"), _("Medium"), _("Large")},
      size == "small" ? 0
      : size == "large" ? 2
                        : 1,
      [](int v) {
        static const char *vals[] = {"small", "medium", "large"};
        if (v < 0 || v > 2) return;
        g_cfg.set("menu", "size", vals[v]);
        // A size dragged at the menu's edges wins over the presets.
        g_cfg.set("menu", "width", "0");
        g_cfg.set("menu", "height", "0");
        apply_change();
        refresh_if("start-menu");
      },
      dragged ? _("The menu keeps the size you dragged at its edges until you choose one here.")
              : _("You can also drag the menu's outer edges."));
  page->switch_row(_("Show recently added applications"),
                   g_cfg.get_bool("menu", "show_recently_added", true),
                   [](bool on) { set_bool("menu", "show_recently_added", on); });
  page->switch_row(_("Show most used applications"),
                   g_cfg.get_bool("menu", "show_most_used", g_cfg.get_bool("menu", "show_recent", true)),
                   [](bool on) { set_bool("menu", "show_most_used", on); });
  page->end_card();
  page->begin_card(_("Account"));
  page->row(_("Your picture and your name"), S(170), S(34),
            [](int x, int y, int w, int h) {
              auto *b = new FlatButton(x, y, w, h, _("Users & Avatar"));
              b->callback([](Fl_Widget *, void *) { open_page("users"); });
              return b;
            },
            _("The account button of the Start menu shows them; click it to pick another picture."));
  page->end_card();
  page->note(_("To pin an application, right-click it in the Start menu. Drag tiles to arrange "
               "them in groups."));
  page->finish();
}

void build_suite_system(Page *page) {
  page->heading(_("Power & Updates"), _("System tools included with SDE"));
  build_date_time(page);
  const std::vector<std::pair<std::string, std::vector<std::string>>> tools = {
      {_("Power"), {"sde-settings", "--page", "power"}},
      {_("Lock Screen"), {"sde-settings", "--page", "lock"}},
      {_("System Monitor"), {"sde-system-monitor"}},
      {_("Software Updater"), {"sde-update"}},
  };
  if (!g_refreshing || !g_power.loaded) load_power_profiles();
  if (g_power.loaded && !g_power.profiles.empty()) {
    page->begin_card(_("Power mode"));
    std::vector<std::string> labels;
    int cur = 0;
    for (size_t i = 0; i < g_power.profiles.size(); ++i) {
      labels.push_back(profile_label(g_power.profiles[i]));
      if (g_power.profiles[i] == g_power.current) cur = int(i);
    }
    page->choice_row(
        _("Power mode"), labels, cur,
        [](int v) {
          if (v < 0 || v >= int(g_power.profiles.size())) return;
          run_async(with_timeout({"powerprofilesctl", "set", g_power.profiles[size_t(v)]}),
                    [](const std::string &, int) { load_power_profiles(); });
        },
        _("Power saver makes the battery last longer; Performance is faster but uses more power."));
    page->end_card();
  }
  page->begin_card(_("System"));
  for (auto &tool : tools) {
    page->row(tool.first, S(112), S(34), [&tool](int x, int y, int w, int h) {
      return action_button(x, y, w, h, _("Open"), tool.second);
    });
  }
  page->end_card();
  page->note(
      _("SDE Power handles battery, lid and idle policies. SDE Screen Lock starts with the "
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

const std::vector<PageInfo> &pages() {
  // Grouped as the overview's categories; the ids never change.
  static const std::vector<PageInfo> list = {
      {"home", N_("Overview"), {"sde-control-center", "preferences-system"}, build_hub, nullptr},
      {"appearance",
       N_("Appearance"),
       {"preferences-desktop-theme", "preferences-desktop-wallpaper"},
       build_appearance,
       N_("PERSONALIZATION")},
      {"desktop", N_("Desktop"), {"user-desktop", "preferences-desktop-wallpaper"}, build_desktop, nullptr},
      {"panel", N_("Panel"), {"preferences-system-windows", "preferences-desktop"}, build_panel, nullptr},
      {"start-menu", N_("Start Menu"), {"start-here", "preferences-desktop"}, build_start_menu_page,
       nullptr},
      {"action-center",
       N_("Action Center"),
       {"preferences-system", "preferences-desktop"},
       build_action_center,
       nullptr},
      {"clock",
       N_("Clock & Weather"),
       {"preferences-system-time", "x-office-calendar", "preferences-system"},
       build_clock_page,
       nullptr},
      {"notifications",
       N_("Notifications"),
       {"preferences-desktop-notification", "notification-active", "dialog-information"},
       build_notifications,
       nullptr},
      {"effects",
       N_("Effects"),
       {"preferences-desktop-effects", "applications-graphics", "preferences-desktop"},
       build_effects,
       nullptr},
      {"lock", N_("Lock Screen"), {"system-lock-screen", "preferences-desktop-screensaver"},
       build_lock_page, nullptr},
      {"display",
       N_("Displays"),
       {"preferences-desktop-display", "video-display", "computer"},
       build_display_page,
       N_("SYSTEM")},
      {"sound", N_("Sound"), {"preferences-desktop-sound", "audio-volume-high"}, build_sound, nullptr},
      {"power", N_("Power"), {"preferences-system-power", "battery"}, build_power_page, nullptr},
      {"date-time", N_("Date & Time"), {"preferences-system-time"}, build_date_time_page, nullptr},
      {"users", N_("Users & Avatar"), {"system-users", "avatar-default"}, build_users_page, nullptr},
      {"default-apps",
       N_("Default Applications"),
       {"preferences-desktop-default-applications", "applications-other"},
       build_default_apps_page,
       nullptr},
      {"input",
       N_("Keyboard & Mouse"),
       {"preferences-desktop-keyboard", "input-keyboard", "input-mouse"},
       build_input_page,
       nullptr},
      {"touchpad", N_("Touchpad"), {"input-touchpad", "input-mouse"}, build_touchpad_page, nullptr},
      {"network",
       N_("Network"),
       {"preferences-system-network", "network-wireless"},
       build_network,
       N_("NETWORK & INTERNET")},
      {"bluetooth", N_("Bluetooth"), {"bluetooth", "preferences-bluetooth"}, build_bluetooth, nullptr},
      {"system", N_("Power & Updates"), {"utilities-system-monitor"}, build_suite_system,
       N_("ADMINISTRATION")},
      {"suite", N_("Applications"), {"applications-other", "sde-logo"}, build_suite, nullptr},
      {"about", N_("About"), {"help-about", "dialog-information"}, build_about, ""},
  };
  return list;
}

// --------------------------------------------------------------- window

class Sidebar;
Sidebar *g_sidebar = nullptr;
std::vector<std::string> g_qa_lines;  // extra QA lines of the page being shown
void qa_dump_window();
Fl_Scroll *g_sidebar_scroll = nullptr;
Page *g_page = nullptr;
int g_page_index = 0;
int g_content_x = 0, g_content_w = 0;

void show_page(int index);

class Sidebar : public Fl_Widget {
 public:
  Sidebar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  // Rows keep a comfortable height; the sidebar scrolls when the window is
  // too short for all of them.
  int row_h() const { return S(32); }
  // A caption before each group of pages (a small gap for an untitled one).
  int row_y(int i) const {
    int top = y() + S(80);
    for (int k = 0; k <= i && k < int(pages().size()); ++k)
      if (const char *section = pages()[size_t(k)].section) top += *section ? S(26) : S(10);
    return top + i * row_h();
  }
  // The height that shows every row and the footer.
  int content_h() const { return row_y(int(pages().size())) - y() + S(48); }
  void draw() override {
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), g_theme.surface);
      p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), g_theme.border);
      p.image(IconLoader::get({"sde-control-center", "sde-logo", "start-here"}, S(36)), x() + S(20), y() + S(20), S(36));
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
    {
      // "Control Center", on two lines when the sidebar is narrow.
      std::string title = _("Control Center");
      int tw = w() - S(70);
      text_font(g_theme.font_size + 2, true);
      if (text_width(title) > tw) text_font(g_theme.font_size + 1, true);
      size_t space = title.rfind(' ');
      if (text_width(title) > tw && space != std::string::npos) {
        draw_text(title.substr(0, space), x() + S(64), y() + S(19), tw, S(20), g_theme.text);
        draw_text(title.substr(space + 1), x() + S(64), y() + S(37), tw, S(20), g_theme.text);
      } else {
        draw_text(title, x() + S(64), y() + S(22), tw, S(32), g_theme.text);
      }
    }
    text_font(g_theme.font_size - 2, true);
    for (size_t i = 0; i < pages().size(); ++i)
      if (const char *section = pages()[i].section)
        if (*section)
          draw_text(_(section), x() + S(22), row_y(int(i)) - S(24), w() - S(30), S(20),
                    g_theme.text_dim);
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
  SettingsWindow(int w, int h) : BufferedWindow(w, h, _("Control Center")) {}
  void draw() override {
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), g_theme.background);
    }
    draw_children();
    qa_dump_window();
  }
};

// UI test hook, as the panel's (src/panel/popup.h): with SDE_PANEL_QA_DIR
// set, <dir>/Control-Center.txt lists the named controls (tooltip or label)
// and the sidebar rows ("nav:<page>"), one
// "name<TAB>x<TAB>y<TAB>w<TAB>h<TAB>active<TAB>scrolled" line each, in window
// pixels. "active" is 1 for a usable control that is fully in view.
void qa_collect(Fl_Widget *w, const Fl_Widget *view, std::string &out) {
  if (!w->visible()) return;
  const char *name = w->tooltip() ? w->tooltip() : w->label();
  if (name && *name && w != w->window()) {
    std::string n = name;
    for (char &c : n)
      if (c == '\t' || c == '\n') c = ' ';
    bool shown = !view || (w->y() >= view->y() && w->y() + w->h() <= view->y() + view->h() &&
                           w->x() >= view->x() && w->x() + w->w() <= view->x() + view->w());
    out += n + "\t" + std::to_string(w->x()) + "\t" + std::to_string(w->y()) + "\t" +
           std::to_string(w->w()) + "\t" + std::to_string(w->h()) + "\t" +
           std::to_string(w->active_r() && shown ? 1 : 0) + "\t" + (view ? "1" : "0") + "\n";
  }
  if (auto *g = w->as_group()) {
    const Fl_Widget *inner = dynamic_cast<Fl_Scroll *>(w) ? w : view;
    for (int i = 0; i < g->children(); ++i) qa_collect(g->child(i), inner, out);
  }
}

void qa_dump_window() {
  const char *dir = getenv("SDE_PANEL_QA_DIR");
  if (!dir || !*dir || !g_window) return;
  std::string text = "@page:" + g_restart_page + "\t0\t0\t0\t0\t1\t0\n";
  for (const std::string &line : g_qa_lines) text += line;
  if (g_sidebar && g_sidebar_scroll)
    for (size_t i = 0; i < pages().size(); ++i) {
      int ry = g_sidebar->row_y(int(i)), rh = g_sidebar->row_h() - S(4);
      bool shown = ry >= g_sidebar_scroll->y() && ry + rh <= g_sidebar_scroll->y() + g_sidebar_scroll->h();
      text += std::string("nav:") + pages()[i].id + "\t" + std::to_string(g_sidebar->x() + S(10)) +
              "\t" + std::to_string(ry) + "\t" + std::to_string(g_sidebar->w() - S(20)) + "\t" +
              std::to_string(rh) + "\t" + (shown ? "1" : "0") + "\t1\n";
    }
  for (int i = 0; i < g_window->children(); ++i)
    if (g_window->child(i) != g_sidebar_scroll) qa_collect(g_window->child(i), nullptr, text);
  std::string path = std::string(dir) + "/Control-Center.txt", tmp = path + ".tmp";
  if (FILE *f = fopen(tmp.c_str(), "w")) {
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    rename(tmp.c_str(), path.c_str());
  }
}

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
  g_qa_lines.clear();
  g_page = new Page(g_content_x, 0, g_content_w, g_window->h());
  pages()[size_t(index)].build(g_page);
  g_window->end();
  if (!g_open_anchor.empty()) {
    int offset = g_page->anchor_offset(g_open_anchor);
    if (offset >= 0) g_page->scroll_to(0, std::max(0, offset - S(8)));
    g_open_anchor.clear();
  }
  g_window->redraw();
}

}  // namespace

void refresh_page() {
  int scroll = g_page ? g_page->yposition() : 0;
  g_refreshing = true;
  show_page(g_page_index);
  g_refreshing = false;
  if (g_page && scroll > 0) g_page->scroll_to(0, scroll);
}

void open_settings_page(const std::string &id, const std::string &anchor) {
  open_page(id.c_str(), anchor.empty() ? nullptr : anchor.c_str());
}

const std::string &current_settings_page() { return g_restart_page; }

void draw_settings_glyph(Painter &p, const std::string &id, double x, double y, double size,
                         const Color &c) {
  settings_glyph(p, id, x, y, size, c);
}

void qa_line(const std::string &name, int x, int y, int w, int h) {
  if (!getenv("SDE_PANEL_QA_DIR")) return;
  g_qa_lines.push_back(name + "\t" + std::to_string(x) + "\t" + std::to_string(y) + "\t" +
                       std::to_string(w) + "\t" + std::to_string(h) + "\t1\t0\n");
}

}  // namespace sde

using namespace sde;

namespace {

// Other names accepted by --page (the panel's flyout names and common
// words), mapped to page ids and, for some, the card the page scrolls to.
struct PageAlias {
  const char *name, *page, *anchor;
};
const PageAlias kPageAliases[] = {
    {"overview", "home", nullptr},        {"control-center", "home", nullptr},
    {"wallpaper", "appearance", "wallpaper"}, {"wallpapers", "appearance", "wallpaper"},
    {"theme", "appearance", nullptr},     {"background", "appearance", "wallpaper"},
    {"icons", "desktop", nullptr},        {"start", "start-menu", nullptr},
    {"menu", "start-menu", nullptr},      {"startmenu", "start-menu", nullptr},
    {"displays", "display", nullptr},     {"screen", "display", nullptr},
    {"monitor", "display", nullptr},      {"keyboard", "input", nullptr},
    {"mouse", "input", "mouse"},          {"audio", "sound", nullptr},
    {"volume", "sound", nullptr},         {"media", "sound", nullptr},
    {"microphone", "sound", nullptr},     {"wifi", "network", nullptr},
    {"wi-fi", "network", nullptr},        {"ethernet", "network", nullptr},
    {"vpn", "network", nullptr},          {"internet", "network", nullptr},
    {"proxy", "network", "proxy"},        {"battery", "power", nullptr},
    {"updates", "system", nullptr},       {"applications", "suite", nullptr},
    {"apps", "suite", nullptr},           {"quick", "action-center", nullptr},
    {"animations", "effects", nullptr},   {"compositor", "effects", nullptr},
    {"info", "about", nullptr},           {"graphics", "about", nullptr},
    {"time", "date-time", nullptr},       {"date", "date-time", nullptr},
    {"timezone", "date-time", nullptr},   {"datetime", "date-time", nullptr},
    {"weather", "clock", nullptr},        {"clocks", "clock", nullptr},
    {"user", "users", nullptr},           {"account", "users", nullptr},
    {"accounts", "users", nullptr},       {"avatar", "users", "avatar"},
    {"picture", "users", "avatar"},       {"face", "users", "avatar"},
    {"screensaver", "lock", nullptr},     {"lock-screen", "lock", nullptr},
    {"defaults", "default-apps", nullptr}, {"default apps", "default-apps", nullptr},
    {"default-applications", "default-apps", nullptr},
    {"default applications", "default-apps", nullptr},
    {"mime", "default-apps", nullptr},    {"file-types", "default-apps", nullptr},
    {"open-with", "default-apps", nullptr}, {"browser", "default-apps", nullptr},
    {"mail", "default-apps", nullptr},    {"email", "default-apps", nullptr},
    {"terminal", "default-apps", nullptr},
};

// Page id for |name| (an id or an alias, any case), or "".
std::string resolve_page(std::string name, std::string *anchor = nullptr) {
  name = lower(trim(name));
  for (auto &p : pages())
    if (name == p.id) return p.id;
  for (auto &a : kPageAliases)
    if (name == a.name) {
      if (anchor && a.anchor) *anchor = a.anchor;
      return a.page;
    }
  return "";
}

std::string usage() {
  std::string ids, aliases;
  for (auto &p : pages()) ids += std::string(ids.empty() ? "" : ", ") + p.id;
  for (auto &a : kPageAliases) aliases += std::string(aliases.empty() ? "" : ", ") + a.name;
  auto wrap = [](const std::string &text) {
    std::string out, line;
    for (auto &word : split(text, ' ')) {
      if (line.size() + word.size() + 1 > 62) {
        out += "               " + line + "\n";
        line.clear();
      }
      line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) out += "               " + line + "\n";
    return out;
  };
  // The panel reads the page list after the last "--page NAME" (quick.cpp).
  return std::string(_("The Control Center of SDE (also started as sde-control-center).\n\n"
                       "Usage: sde-settings [--page NAME] [--list-pages] [--choose-avatar] [--apply]\n"
                       "                    [--version]\n\n"
                       "  --page NAME  open a page directly (also --page=NAME). Pages:\n")) +
         wrap(ids) + _("               Also accepted:\n") + wrap(aliases) +
         _("  --list-pages print the page names, one per line\n"
           "  --choose-avatar  open Users & Avatar and choose a picture for the account\n"
           "  --apply      apply the saved display, keyboard and mouse settings and exit\n"
           "  --version    print the version and exit\n");
}

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  sde::set_default_window_icon({"sde-control-center"});  // title bar and taskbar
  install_crash_handler("sde-settings");
  g_argv0 = argv[0];
  std::string page = "home", requested, anchor;
  bool apply = false, choose_avatar = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--page" && i + 1 < argc) {
      requested = argv[++i];
    } else if (starts_with(a, "--page=")) {
      requested = a.substr(7);
    } else if (a == "--list-pages") {
      for (auto &p : pages()) printf("%s\n", p.id);
      return 0;
    } else if (a == "--choose-avatar") {
      requested = "users";
      choose_avatar = true;
    } else if (a == "--apply") {
      apply = true;
    } else if (a == "--version") {
      return print_version("sde-settings", SDE_VERSION);
    } else {
      printf("%s", usage().c_str());
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }
  if (!requested.empty()) {
    std::string id = resolve_page(requested, &anchor);
    if (id.empty())
      fprintf(stderr, "sde-settings: unknown page \"%s\" (see --list-pages)\n", requested.c_str());
    else
      page = id;
  }
  g_cfg.load();
  if (migrate_theme_config(g_cfg)) g_cfg.save();
  if (apply) {
    sync_gtk_settings(g_cfg);
    sync_openbox(g_cfg);
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
  g_sidebar = new Sidebar(0, 0, side - S(10), H);
  g_sidebar->size(g_sidebar->w(), std::max(H, g_sidebar->content_h()));
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
  // Worker threads (thumbnails, wallpaper scan) wake the loop with Fl::awake().
  Fl::lock();
  g_open_anchor = anchor;
  show_page(index);
  win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  start_display_watch();
  // The account and its picture (AccountsService may take a moment), and
  // their changes made elsewhere (the Start menu, the lock screen).
  watch_account();
  win->show();
  if (choose_avatar) Fl::add_timeout(0.3, [](void *) { choose_avatar_picture(); });
  int status = Fl::run();
  g_thumbnailer.reset();  // joins the decoding threads
  return status;
}
