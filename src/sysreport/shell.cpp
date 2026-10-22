// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
#include "shell.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <map>

#include "buffered_window.h"
#include "i18n.h"
#include "icons.h"
#include "integration.h"
#include "paint.h"
#include "process.h"
#include "spawn.h"
#include "util.h"
#include "widgets.h"
#include "wm.h"
#include "x11.h"

namespace sde {
namespace shell {

namespace {

Config g_cfg;
Theme g_theme;
Options g_opts;
class ShellWindow;
ShellWindow *g_win = nullptr;
Page *g_page = nullptr;
std::string g_current;
std::string g_busy;  // what run_admin() is doing, "" when idle
std::map<std::string, bool> g_packages;

int sidebar_w() { return g_opts.pages.size() > 1 ? S(232) : 0; }
int item_h() { return S(40); }
int items_top() { return S(84); }

class ShellWindow : public BufferedWindow {
 public:
  ShellWindow(int w, int h, const char *title) : BufferedWindow(w, h, title) {}

  int item_at(int x, int y) const {
    if (x >= sidebar_w()) return -1;
    int i = (y - items_top()) / item_h();
    if (y < items_top() || i < 0 || i >= int(g_opts.pages.size())) return -1;
    return i;
  }

  void draw() override {
    const Theme &t = g_theme;
    int sw = sidebar_w();
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
      if (sw) {
        p.fill_rect(0, 0, sw, h(), t.surface_alt);
        p.line(sw - 0.5, 0, sw - 0.5, h(), t.border);
        p.image(IconLoader::get({g_opts.about.icon, "sde-logo", "computer"}, S(36)), S(20), S(22), S(36));
        for (size_t i = 0; i < g_opts.pages.size(); ++i) {
          const PageDef &d = g_opts.pages[i];
          int y = items_top() + int(i) * item_h();
          bool cur = d.id == g_current, hot = int(i) == hover_;
          if (cur || hot)
            p.fill_rounded(S(10), y + S(2), sw - S(20), item_h() - S(4), t.radius_small,
                           cur ? t.accent.alpha(t.dark ? 0.28 : 0.16) : t.hover);
          if (cur) p.fill_rounded(S(10), y + S(12), S(3), item_h() - S(24), S(1.5), t.accent);
          p.image(IconLoader::get(d.icons, S(20)), S(24), y + (item_h() - S(20)) / 2.0, S(20));
        }
      }
    }
    if (sw) {
      text_font(t.font_size + 2, true);
      draw_text(g_opts.title, S(64), S(22), sw - S(74), S(20), t.text);
      text_font(t.font_size - 1);
      draw_text(g_opts.about.version.empty() ? "" : format(_("Version %s"), g_opts.about.version.c_str()),
                S(64), S(42), sw - S(74), S(18), t.text_dim);
      for (size_t i = 0; i < g_opts.pages.size(); ++i) {
        const PageDef &d = g_opts.pages[i];
        int y = items_top() + int(i) * item_h();
        text_font(t.font_size + 1, d.id == g_current);
        draw_text(d.title, S(54), y, sw - S(64), item_h(), d.id == g_current ? t.accent_text : t.text);
      }
      if (!g_opts.sidebar_switch.empty()) {
        text_font(t.font_size);
        fl_color(t.text.fl());
        fl_draw(g_opts.sidebar_switch.c_str(), S(20), h() - S(64), sw - S(88), S(48),
                Fl_Align(FL_ALIGN_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE), nullptr, 0);
      }
    }
    draw_children();
  }

  int handle(int e) override {
    switch (e) {
      case FL_MOVE:
      case FL_ENTER:
      case FL_LEAVE: {
        int i = e == FL_LEAVE ? -1 : item_at(Fl::event_x(), Fl::event_y());
        if (i != hover_) {
          hover_ = i;
          damage_rect(0, 0, sidebar_w(), h());
        }
        break;
      }
      case FL_PUSH: {
        int i = item_at(Fl::event_x(), Fl::event_y());
        if (i >= 0 && Fl::event_button() == FL_LEFT_MOUSE) {
          show_page(g_opts.pages[size_t(i)].id);
          return 1;
        }
        break;
      }
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k == FL_Escape) {
          hide();
          return 1;
        }
        // Ctrl+Page Up / Page Down move through the sidebar.
        if ((Fl::event_state() & FL_CTRL) && (k == FL_Page_Up || k == FL_Page_Down) &&
            g_opts.pages.size() > 1) {
          int cur = 0;
          for (size_t i = 0; i < g_opts.pages.size(); ++i)
            if (g_opts.pages[i].id == g_current) cur = int(i);
          int n = int(g_opts.pages.size());
          cur = (cur + (k == FL_Page_Down ? 1 : n - 1)) % n;
          show_page(g_opts.pages[size_t(cur)].id);
          return 1;
        }
        break;
      }
      default:
        break;
    }
    int r = BufferedWindow::handle(e);
    if (!r && handle_about_event(e, g_opts.about)) return 1;
    return r;
  }

 private:
  int hover_ = -1;
};

Switch *g_switch = nullptr;

void make_switch() {
  if (g_opts.sidebar_switch.empty() || !sidebar_w() || !g_win) return;
  g_win->begin();
  g_switch = new Switch(sidebar_w() - S(64), g_win->h() - S(54), S(46), S(26));
  g_switch->value(g_opts.sidebar_switch_value);
  g_switch->tooltip(nullptr);
  g_switch->callback([](Fl_Widget *w, void *) {
    if (g_opts.sidebar_switch_changed) g_opts.sidebar_switch_changed(static_cast<Switch *>(w)->value() != 0);
  });
  g_win->end();
}

class Hero : public Fl_Widget {
 public:
  Hero(int x, int y, int w, int h, std::vector<std::string> icons, std::string title, std::string sub)
      : Fl_Widget(x, y, w, h), icons_(std::move(icons)), title_(std::move(title)), sub_(std::move(sub)) {}
  void draw() override {
    const Theme &t = g_theme;
    int isz = S(76);
    {
      Painter p(window()->w(), window()->h());
      Color a = t.accent, b = t.accent.mix(t.dark ? Color::hex(0x000000) : Color::hex(0x1b1f3a), 0.45);
      p.fill_gradient(x(), y(), w(), h(), t.radius_large, a, b, true);
      // Soft circles for depth (no artwork, only shapes).
      p.circle(x() + w() - S(70), y() + S(30), S(90), Color::hex(0xffffff, 0.07));
      p.circle(x() + w() - S(10), y() + h() - S(10), S(60), Color::hex(0xffffff, 0.06));
      p.fill_rounded(x() + S(26), y() + (h() - isz - S(16)) / 2.0, isz + S(16), isz + S(16), S(22),
                     Color::hex(0xffffff, 0.16));
      p.image(IconLoader::get(icons_, isz), x() + S(34), y() + (h() - isz) / 2.0, isz);
    }
    // The gradient ends dark: white text reads on every accent.
    Color fg = Color::hex(0xffffff);
    int tx = x() + S(26) + isz + S(36);
    text_font(t.font_size + 12, true);
    draw_text(title_, tx, y() + h() / 2 - S(40), w() - (tx - x()) - S(20), S(40), fg);
    text_font(t.font_size + 1);
    fl_color(fg.fl());
    fl_draw(sub_.c_str(), tx, y() + h() / 2 + S(4), w() - (tx - x()) - S(24), h() / 2 - S(12),
            Fl_Align(FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP), nullptr, 0);
  }

 private:
  std::vector<std::string> icons_;
  std::string title_, sub_;
};

class StepBox : public Fl_Widget {
 public:
  StepBox(int x, int y, int w, int h, std::vector<std::string> icons, std::string title,
          std::string status, Color status_color)
      : Fl_Widget(x, y, w, h),
        icons_(std::move(icons)),
        title_(std::move(title)),
        status_(std::move(status)),
        status_color_(status_color) {}
  void draw() override {
    const Theme &t = g_theme;
    {
      Painter p(window()->w(), window()->h());
      p.fill_rounded(x(), y(), w(), h(), Sd(12), t.surface);
      p.stroke_rounded(x(), y(), w(), h(), Sd(12), t.border);
      p.fill_rounded(x() + S(18), y() + S(18), S(48), S(48), S(12), t.accent.alpha(t.dark ? 0.22 : 0.10));
      p.image(IconLoader::get(icons_, S(32)), x() + S(26), y() + S(26), S(32));
      if (!status_.empty()) p.circle(x() + S(88), y() + S(51), S(4), status_color_);
    }
    text_font(t.font_size + 2, true);
    draw_text(title_, x() + S(82), y() + S(14), w() - S(100), S(26), t.text);
    if (!status_.empty()) {
      text_font(t.font_size, true);
      // Status colors (yellow especially) are darkened toward the text color to stay readable.
      draw_text(status_, x() + S(98), y() + S(40), w() - S(116), S(22), status_color_.mix(t.text, 0.45));
    }
  }

 private:
  std::vector<std::string> icons_;
  std::string title_, status_;
  Color status_color_;
};

void run_fn(Fl_Widget *, void *d) {
  auto *fn = static_cast<std::function<void()> *>(d);
  // A callback may rebuild the page (and delete this button): run it later.
  auto *copy = new std::function<void()>(*fn);
  Fl::add_timeout(0, [](void *p) {
    auto *f = static_cast<std::function<void()> *>(p);
    (*f)();
    delete f;
  }, copy);
}

int button_w(const std::string &label) {
  text_font(g_theme.font_size + 1);
  return std::max(S(96), text_width(label) + S(36));
}

void build_current() {
  if (!g_win) return;
  int scroll = 0;
  if (g_page) {
    scroll = g_page->yposition();
    g_win->remove(g_page);
    Fl::delete_widget(g_page);
  }
  g_win->begin();
  g_page = new Page(sidebar_w(), 0, g_win->w() - sidebar_w(), g_win->h());
  for (auto &d : g_opts.pages)
    if (d.id == g_current && d.build) {
      if (!g_busy.empty()) {
        g_page->begin_card();
        g_page->note(g_busy + "  " + _("(this can take a few minutes; the window stays usable)"));
        g_page->end_card();
      }
      d.build(g_page);
    }
  g_page->finish();
  g_win->end();
  if (scroll) g_page->scroll_to(0, std::min(scroll, std::max(0, g_page->h())));
  g_win->redraw();
}

}  // namespace

void init() {
  g_cfg.load();
  if (migrate_theme_config(g_cfg)) {
  }
  fl_open_display();
  g_theme = Theme::from_config(g_cfg);
  g_theme.apply_to_fltk();
  IconLoader::set_theme(g_cfg.get("appearance", "icon_theme", "auto"));
}

Config &config() { return g_cfg; }
const Theme &theme() { return g_theme; }

void refresh() { build_current(); }

void show_page(const std::string &id) {
  if (id == g_current) return;
  g_current = id;
  if (g_page) {
    // A new page starts at the top.
    g_win->remove(g_page);
    Fl::delete_widget(g_page);
    g_page = nullptr;
  }
  build_current();
}

void apply_config(bool retheme) {
  g_cfg.save();
  sync_gtk_settings(g_cfg);
  if (sync_openbox(g_cfg)) reconfigure_window_manager();
  x11::notify_config_changed();
  if (retheme) {
    write_compositor_config(g_cfg);
    write_compositor_config(g_cfg, true);
    reload_compositor();
    g_theme = Theme::from_config(g_cfg);
    g_theme.apply_to_fltk();
    IconLoader::set_theme(g_cfg.get("appearance", "icon_theme", "auto"));
    IconLoader::clear_cache();
    if (g_switch) g_switch->redraw();
    build_current();
  }
}

int run(Options opts) {
  g_opts = std::move(opts);
  g_current = g_opts.start.empty() && !g_opts.pages.empty() ? g_opts.pages[0].id : g_opts.start;
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int W = std::min(S(g_opts.width), sw - S(24)), H = std::min(S(g_opts.height), sh - S(48));
  g_win = new ShellWindow(W, H, g_opts.title.c_str());
  g_win->end();
  g_win->xclass(g_opts.xclass.c_str());
  g_win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  g_win->size_range(S(560), S(420));
  make_switch();
  build_current();
  g_win->show();
  return Fl::run();
}

void step(Page *page, const std::vector<std::string> &icons, const std::string &title,
          const std::string &text, const std::vector<Action> &actions, const std::string &status,
          const Color *status_color) {
  int x = page->left(), y = page->cursor(), w = page->width();
  int gap = S(8), bw_total = 0;
  for (auto &a : actions) bw_total += button_w(a.label) + gap;
  if (bw_total) bw_total -= gap;
  int text_x = x + S(82);
  int side_w = w - S(82) - S(20) - bw_total - S(20);
  bool side = side_w >= S(260);
  int tw = side ? side_w : w - S(82) - S(20);
  int top = status.empty() ? S(42) : S(64);
  int th = text.empty() ? 0 : Label::measure(text, tw, Label::DIM) + S(4);
  int h = top + th + S(16);
  if (!side && !actions.empty()) h += S(34) + S(8);
  h = std::max(h, S(84));
  Color sc = status_color ? *status_color : g_theme.text_dim;
  new StepBox(x, y, w, h, icons, title, status, sc);
  if (!text.empty()) new Label(text_x, y + top, tw, th, text, Label::DIM, true);
  int bx = side ? x + w - S(20) - bw_total : text_x;
  int by = side ? y + (h - S(34)) / 2 : y + h - S(16) - S(34);
  for (auto &a : actions) {
    int bw = button_w(a.label);
    auto *b = new FlatButton(bx, by, bw, S(34), a.label, a.primary);
    b->callback(run_fn, keep(new std::function<void()>(a.run)));
    if (!a.enabled || !g_busy.empty()) b->deactivate();
    bx += bw + gap;
  }
  page->advance(h + S(14));
}

FlatButton *button_row(Page *page, const std::string &label, const std::string &button,
                       std::function<void()> run, const std::string &hint, bool primary) {
  FlatButton *b = nullptr;
  int bw = button_w(button);
  auto *fn = keep(new std::function<void()>(std::move(run)));
  page->row(label, bw, S(34), [&](int x, int y, int w, int h) {
    b = new FlatButton(x, y, w, h, button, primary);
    b->callback(run_fn, fn);
    return b;
  }, hint);
  return b;
}

void hero(Page *page, const std::vector<std::string> &icons, const std::string &title,
          const std::string &subtitle) {
  int h = S(176);
  new Hero(page->left(), page->cursor(), page->width(), h, icons, title, subtitle);
  page->advance(h + S(20));
}

bool launch(const std::vector<std::string> &argv) {
  if (argv.empty()) return false;
  if (find_program(argv[0]).empty()) {
    fl_message(_("%s is not installed."), argv[0].c_str());
    return false;
  }
  return spawn_detached(argv);
}

void open_url(const std::string &url) {
  if (!open_uri(url)) fl_message(_("Cannot open %s"), url.c_str());
}

void copy_text(const std::string &text) {
  Fl::copy(text.c_str(), int(text.size()), 0);
  Fl::copy(text.c_str(), int(text.size()), 1);
}

void run_admin(const std::vector<std::string> &argv, const std::string &what,
               std::function<void(const std::string &, int)> done) {
  if (find_program("pkexec").empty()) {
    fl_alert("%s", _("pkexec (polkit) is not installed, so SDE cannot ask for the administrator "
                     "password. Run the command in a terminal with sudo instead."));
    return;
  }
  if (!g_busy.empty()) {
    fl_message(_("Please wait: %s"), g_busy.c_str());
    return;
  }
  std::vector<std::string> cmd = {"pkexec"};
  cmd.insert(cmd.end(), argv.begin(), argv.end());
  g_busy = what;
  build_current();
  auto *fn = keep(new std::function<void(const std::string &, int)>(std::move(done)));
  run_async(cmd, [fn](const std::string &out, int status) {
    g_busy.clear();
    refresh_package_cache();
    (*fn)(out, status);
    build_current();
  });
}

void install_packages(const std::vector<std::string> &packages, const std::string &what) {
  if (packages.empty()) return;
  std::vector<std::string> argv = {"env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y"};
  argv.insert(argv.end(), packages.begin(), packages.end());
  std::string list;
  for (auto &p : packages) list += (list.empty() ? "" : " ") + p;
  run_admin(argv, what.empty() ? format(_("Installing %s..."), list.c_str()) : what,
            [list](const std::string &out, int status) {
              if (status == 0) return;
              if (status == 126 || status == 127) {
                fl_message("%s", _("The installation was cancelled (administrator authorization "
                                   "was refused)."));
                return;
              }
              std::string tail = out.size() > 1500 ? out.substr(out.size() - 1500) : out;
              fl_alert(_("Installing %s failed (status %d).\n\n%s"), list.c_str(), status, tail.c_str());
            });
}

bool busy(std::string *what) {
  if (what) *what = g_busy;
  return !g_busy.empty();
}

bool package_installed(const std::string &package) {
  auto it = g_packages.find(package);
  if (it != g_packages.end()) return it->second;
  bool ok = false;
  if (!find_program("dpkg-query").empty()) {
    int st = -1;
    std::string out = run_sync({"dpkg-query", "-W", "-f=${Status}", package}, &st, 3000);
    ok = st == 0 && out.find("install ok installed") != std::string::npos;
  }
  g_packages[package] = ok;
  return ok;
}

void refresh_package_cache() { g_packages.clear(); }

}  // namespace shell
}  // namespace sde
