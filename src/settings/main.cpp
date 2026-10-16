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
#include "desktop_entry.h"
#include "glyphs.h"
#include "i18n.h"
#include "icons.h"
#include "paint.h"
#include "settings.h"
#include "spawn.h"
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

}  // namespace

Config &settings_config() { return g_cfg; }
const Theme &settings_theme() { return g_theme; }

void apply_change() {
  double old_scale = g_theme.scale;
  g_cfg.save();
  g_theme = Theme::from_config(g_cfg);
  g_theme.apply_to_fltk();
  if (sync_openbox(g_cfg)) reconfigure_window_manager();
  x11::notify_config_changed();
  if (std::fabs(g_theme.scale - old_scale) > 0.01) {
    // Every size depends on the scale: start again with the new one.
    Fl::add_timeout(0.2, [](void *) {
      execl("/proc/self/exe", g_argv0.c_str(), "--page", "appearance", static_cast<char *>(nullptr));
    });
  }
  if (g_window) g_window->redraw();
}

namespace {

// ------------------------------------------------------- custom widgets

// Light/Dark style preview that acts as a radio button.
class StyleCard : public Fl_Widget {
 public:
  StyleCard(int x, int y, int w, int h, bool dark) : Fl_Widget(x, y, w, h), dark_(dark) {}
  void draw() override {
    bool active = g_theme.dark == dark_;
    int pv = h() - S(26);
    {
      Painter p(window()->w(), window()->h());
      Color bg = dark_ ? Color::hex(0x161B22) : Color::hex(0xF5F7FA);
      Color surf = dark_ ? Color::hex(0x1F2630) : Color::hex(0xFFFFFF);
      Color line = dark_ ? Color::hex(0x384252) : Color::hex(0xD5DBE3);
      p.fill_rounded(x(), y(), w(), pv, Sd(12), bg);
      p.stroke_rounded(x(), y(), w(), pv, Sd(12), active ? g_theme.accent : g_theme.border,
                       active ? Sd(2.5) : Sd(1));
      p.fill_rounded(x() + Sd(14), y() + Sd(14), w() - Sd(28), pv - Sd(52), Sd(8), surf);
      p.fill_rounded(x() + Sd(24), y() + Sd(26), w() * 0.4, Sd(8), Sd(4), line);
      p.fill_rounded(x() + Sd(24), y() + Sd(42), w() * 0.55, Sd(8), Sd(4), line);
      p.fill_rounded(x() + Sd(24), y() + Sd(58), Sd(36), Sd(12), Sd(6), g_theme.accent);
      p.fill_rect(x() + 1, y() + pv - Sd(18), w() - 2, Sd(17), surf);
      p.circle(x() + Sd(16), y() + pv - Sd(9), Sd(4), g_theme.accent);
      if (Fl::focus() == this) focus_ring(p, x(), y(), w(), pv, Sd(12));
    }
    text_font(g_theme.font_size, active);
    draw_text(dark_ ? _("Dark") : _("Light"), x(), y() + pv + S(2), w(), S(22),
              active ? g_theme.accent_text : g_theme.text, 1);
  }
  void choose() {
    g_cfg.set("appearance", "theme", dark_ ? "dark" : "light");
    set_openbox_theme(dark_);
    reconfigure_window_manager();
    apply_change();
  }
  int handle(int e) override {
    switch (e) {
      case FL_PUSH: return 1;
      case FL_RELEASE:
        if (Fl::event_inside(this)) choose();
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS: redraw(); return 1;
      case FL_KEYBOARD:
        if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
          choose();
          return 1;
        }
        return 0;
      default: return Fl_Widget::handle(e);
    }
  }

 private:
  bool dark_;
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
      case FL_PUSH: return 1;
      case FL_RELEASE: pick((Fl::event_x() - x()) / step()); return 1;
      case FL_FOCUS:
      case FL_UNFOCUS: redraw(); return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k == FL_Left && focus_ > 0) --focus_;
        else if (k == FL_Right && focus_ + 1 < kAccentChoiceCount) ++focus_;
        else if (k == ' ' || k == FL_Enter) pick(focus_);
        else return 0;
        redraw();
        return 1;
      }
      default: return Fl_Widget::handle(e);
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
        if (!ends_with(l, ".svg") && !ends_with(l, ".png") && !ends_with(l, ".jpg") && !ends_with(l, ".jpeg"))
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
      int tx = x() + int(i % size_t(cols_)) * (tw_ + S(12)), ty = y() + int(i / size_t(cols_)) * (th_ + S(12));
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
      if (files_[i] == cur) p.stroke_rounded(tx - Sd(3), ty - Sd(3), tw_ + Sd(6), th_ + Sd(6), Sd(10), g_theme.accent, Sd(3));
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
      case FL_PUSH: return 1;
      case FL_RELEASE: {
        int col = (Fl::event_x() - x()) / (tw_ + S(12)), row = (Fl::event_y() - y()) / (th_ + S(12));
        if (col >= 0 && col < cols_ && row >= 0) pick(row * cols_ + col);
        return 1;
      }
      case FL_FOCUS:
      case FL_UNFOCUS: redraw(); return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key(), n = int(files_.size());
        if (k == FL_Left && focus_ > 0) --focus_;
        else if (k == FL_Right && focus_ + 1 < n) ++focus_;
        else if (k == ' ' || k == FL_Enter) pick(focus_);
        else return 0;
        redraw();
        return 1;
      }
      default: return Fl_Widget::handle(e);
    }
  }

 private:
  std::vector<std::string> files_;
  std::map<std::string, cairo_surface_t *> thumbs_;
  int tw_, th_, cols_, focus_ = 0;
};

class ImageBox : public Fl_Widget {
 public:
  ImageBox(int x, int y, int s, std::vector<std::string> names) : Fl_Widget(x, y, s, s), names_(std::move(names)) {}
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

void build_appearance(Page *page) {
  page->heading(_("Appearance"), _("Style, colors, size and wallpaper"));
  page->begin_card(_("Style"));
  int cw = std::min(S(150), (page->width() - S(56)) / 2);
  int y = page->cursor();
  new StyleCard(page->left() + S(20), y, cw, S(124), false);
  new StyleCard(page->left() + S(36) + cw, y, cw, S(124), true);
  page->advance(S(132));
  page->row(_("Accent color"), S(34) * kAccentChoiceCount, S(36), [](int x, int yy, int w, int h) {
    return new AccentPicker(x, yy, w, h);
  });
  page->end_card();

  page->begin_card(_("Size and text"));
  std::string sc = g_cfg.get("appearance", "scale", "auto");
  std::vector<std::string> scales = {_("Automatic"), "100 %", "125 %", "150 %", "175 %", "200 %"};
  int sc_i = sc == "auto" ? 0 : std::max(0, std::min(5, (atoi(sc.c_str()) - 100) / 25 + 1));
  page->choice_row(_("Interface size"), scales, sc_i, [](int v) {
    static const char *vals[] = {"auto", "100", "125", "150", "175", "200"};
    if (v >= 0 && v < 6) g_cfg.set("appearance", "scale", vals[v]);
    apply_change();
  }, _("Larger sizes help on high-resolution screens. Automatic follows the screen's DPI."));
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
  auto *grid = new WallpaperGrid(page->left() + S(20), page->cursor(), page->width() - S(40), S(80));
  page->advance(grid->h() + S(8));
  std::string cm = g_cfg.get("appearance", "wallpaper_mode", "zoom");
  static const char *modes[] = {"zoom", "fit", "stretch", "center", "tile"};
  int mode_i = 0;
  for (int i = 0; i < 5; ++i)
    if (cm == modes[i]) mode_i = i;
  page->choice_row(_("Placement"), {_("Zoom"), _("Fit"), _("Stretch"), _("Center"), _("Tile")}, mode_i, [](int v) {
    if (v >= 0 && v < 5) g_cfg.set("appearance", "wallpaper_mode", modes[v]);
    apply_change();
  });
  page->row(_("Your own picture"), S(160), S(34), [grid](int x, int y, int w, int h) {
    auto *b = new FlatButton(x, y, w, h, _("Browse..."));
    b->callback([](Fl_Widget *, void *d) {
      const char *f = fl_file_chooser(_("Choose a wallpaper"), "Images (*.{png,jpg,jpeg,svg})",
                                      (home_dir() + "/Pictures").c_str());
      if (!f) return;
      static_cast<WallpaperGrid *>(d)->add(f);
      g_cfg.set("appearance", "wallpaper", f);
      apply_change();
    }, grid);
    return b;
  });
  page->end_card();
  page->finish();
}

void set_bool(const char *section, const char *key, bool on) {
  g_cfg.set(section, key, on ? "true" : "false");
  apply_change();
}

void build_panel(Page *page) {
  page->heading(_("Panel"), _("Position, size, clock and pinned applications"));
  page->begin_card(_("Layout"));
  page->choice_row(_("Position"), {_("Bottom"), _("Top")},
                   lower(g_cfg.get("panel", "position", "bottom")) == "top" ? 1 : 0, [](int v) {
                     g_cfg.set("panel", "position", v == 1 ? "top" : "bottom");
                     apply_change();
                   });
  int h = g_cfg.get_int("panel", "height", 40);
  page->choice_row(_("Size"), {_("Small"), _("Medium"), _("Large")}, h == 32 ? 0 : h == 48 ? 2 : 1, [](int v) {
    static const char *vals[] = {"32", "40", "48"};
    if (v >= 0 && v < 3) g_cfg.set("panel", "height", vals[v]);
    apply_change();
  });
  page->switch_row(_("Hide automatically"), g_cfg.get_bool("panel", "autohide", false),
                   [](bool on) { set_bool("panel", "autohide", on); },
                   _("The panel appears when the pointer touches the screen edge."));
  page->end_card();

  page->begin_card(_("Clock and status"));
  page->switch_row(_("24-hour clock"), g_cfg.get_bool("panel", "clock_24h", true),
                   [](bool on) { set_bool("panel", "clock_24h", on); });
  page->switch_row(_("Show the date"), g_cfg.get_bool("panel", "show_date", true),
                   [](bool on) { set_bool("panel", "show_date", on); });
  page->switch_row(_("Show battery status"), g_cfg.get_bool("panel", "show_battery", true),
                   [](bool on) { set_bool("panel", "show_battery", on); }, _("On laptops and tablets."));
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
      auto *btn = new FlatButton(x + w - (3 - b) * S(96) + S(4), y + (rh - S(32)) / 2, S(92), S(32), labels[b]);
      if ((b == 0 && i == 0) || (b == 1 && i + 1 == pins.size())) btn->deactivate();
      btn->callback([](Fl_Widget *, void *d) {
        auto *m = static_cast<Move *>(d);
        auto ids = split(g_cfg.get("panel", "pinned"), ';');
        std::vector<std::string> valid;
        for (auto &id : ids) {
          DesktopEntry e;
          if (find_application(trim(id), e)) valid.push_back(trim(id));
        }
        if (m->index >= valid.size()) return;
        if (m->delta == 0) valid.erase(valid.begin() + long(m->index));
        else {
          size_t other = m->index + size_t(m->delta);
          if (other < valid.size()) std::swap(valid[m->index], valid[other]);
        }
        std::string list;
        for (auto &id : valid) list += (list.empty() ? "" : ";") + id;
        g_cfg.set("panel", "pinned", list);
        apply_change();
        Fl::add_timeout(0, [](void *) { refresh_page(); });
      }, keep(new Move{i, deltas[b]}));
    }
    page->advance(rh);
  }
  page->note(_("To pin an application, right-click it in the application menu or on the taskbar. "
               "Drag pinned icons on the panel to change their order."));
  page->end_card();
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
    w.str("SDE Settings").u32(0).str("preferences-desktop-notification").str(_("Notifications work"))
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
  page->switch_row(_("Do Not Disturb"), g_cfg.get_bool("notifications", "do_not_disturb", false),
                   [](bool on) { set_bool("notifications", "do_not_disturb", on); },
                   _("Hide notifications, except critical ones such as a low battery."));
  int t = g_cfg.get_int("notifications", "timeout", 6);
  page->choice_row(_("Show notifications for"),
                   {_("4 seconds"), _("6 seconds"), _("10 seconds"), _("Until dismissed")},
                   t == 4 ? 0 : t == 10 ? 2 : t == 0 ? 3 : 1, [](int v) {
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

void build_about(Page *page) {
  page->begin_card();
  int y = page->cursor();
  new ImageBox(page->left() + S(24), y + S(16), S(88), {"sde-logo", "start-here"});
  int tx = page->left() + S(132), tw = page->width() - S(152);
  new Label(tx, y + S(14), tw, S(34), "Selviana Desktop Environment", Label::TITLE);
  new Label(tx, y + S(50), tw, S(22), _("Small and fast, with a modern desktop experience."), Label::SUBTITLE);
  new Label(tx, y + S(76), tw, S(22), format(_("Version %s (%s)"), SDE_VERSION_LABEL, SDE_VERSION), Label::DIM);
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
      {"appearance", N_("Appearance"), {"preferences-desktop-theme", "preferences-desktop-wallpaper"}, build_appearance},
      {"panel", N_("Panel"), {"preferences-system-windows", "preferences-desktop"}, build_panel},
      {"desktop", N_("Desktop"), {"user-desktop", "preferences-desktop-wallpaper"}, build_desktop},
      {"notifications", N_("Notifications"),
       {"preferences-desktop-notification", "notification-active", "dialog-information"}, build_notifications},
      {"display", N_("Displays"), {"preferences-desktop-display", "video-display", "computer"}, build_display_page},
      {"input", N_("Keyboard & Mouse"), {"preferences-desktop-keyboard", "input-keyboard", "input-mouse"},
       build_input_page},
      {"about", N_("About"), {"help-about", "dialog-information"}, build_about},
  };
  return list;
}

// --------------------------------------------------------------- window

class Sidebar;
Sidebar *g_sidebar = nullptr;
Page *g_page = nullptr;
int g_page_index = 0;
int g_content_x = 0, g_content_w = 0;

void show_page(int index);

class Sidebar : public Fl_Widget {
 public:
  Sidebar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int row_y(int i) const { return y() + S(84) + i * S(44); }
  void draw() override {
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), g_theme.surface);
      p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), g_theme.border);
      p.image(IconLoader::get({"sde-logo", "start-here"}, S(36)), x() + S(20), y() + S(20), S(36));
      for (size_t i = 0; i < pages().size(); ++i) {
        int ry = row_y(int(i));
        if (int(i) == g_page_index) {
          p.fill_rounded(x() + S(10), ry, w() - S(20), S(38), S(9), g_theme.accent.alpha(0.15));
          p.fill_rounded(x() + S(10), ry + S(10), Sd(3), S(18), Sd(1.5), g_theme.accent);
        } else if (int(i) == hover_) {
          p.fill_rounded(x() + S(10), ry, w() - S(20), S(38), S(9), g_theme.hover);
        }
        if (Fl::focus() == this && int(i) == g_page_index)
          focus_ring(p, x() + S(10), ry, w() - S(20), S(38), S(9));
        p.image(IconLoader::get(pages()[i].icons, S(20)), x() + S(24), ry + S(9), S(20));
      }
    }
    text_font(g_theme.font_size + 2, true);
    draw_text(_("Settings"), x() + S(64), y() + S(22), w() - S(70), S(32), g_theme.text);
    for (size_t i = 0; i < pages().size(); ++i) {
      bool cur = int(i) == g_page_index;
      text_font(g_theme.font_size + 1, cur);
      draw_text(_(pages()[i].title), x() + S(56), row_y(int(i)), w() - S(66), S(38),
                cur ? g_theme.accent_text : g_theme.text);
    }
    // The version line only when it does not collide with the page list.
    if (row_y(int(pages().size())) + S(8) < y() + h() - S(36)) {
      text_font(g_theme.font_size - 1);
      draw_text("SDE " SDE_VERSION_LABEL, x() + S(20), y() + h() - S(36), w() - S(30), S(20), g_theme.text_dim);
    }
  }
  int index_at(int ey) const {
    for (size_t i = 0; i < pages().size(); ++i)
      if (ey >= row_y(int(i)) && ey < row_y(int(i)) + S(38)) return int(i);
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
      case FL_UNFOCUS: redraw(); return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key(), n = int(pages().size());
        if (k == FL_Up && g_page_index > 0) show_page(g_page_index - 1);
        else if (k == FL_Down && g_page_index + 1 < n) show_page(g_page_index + 1);
        else return 0;
        take_focus();
        return 1;
      }
      default: return Fl_Widget::handle(e);
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
  std::string page = "appearance";
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
      printf("%s", _("Usage: sde-settings [--page NAME] [--apply] [--version]\n\n"
                     "  --page NAME  open a page: appearance, panel, desktop, notifications,\n"
                     "               display, input or about\n"
                     "  --apply      apply the saved display, keyboard and mouse settings and exit\n"));
      return a == "--help" ? 0 : 2;
    }
  }
  g_cfg.load();
  if (apply) {
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
  g_sidebar = new Sidebar(0, 0, side, H);
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
  win->show();
  return Fl::run();
}
