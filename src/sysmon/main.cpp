// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE System Monitor: processes, live performance graphs, file systems and
// startup apps for the Selviana Desktop Environment.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "about.h"
#include "config.h"
#include "i18n.h"
#include "menu.h"
#include "theme.h"
#include "ui.h"
#include "widgets.h"

#ifndef SDE_SYSMON_VERSION
#define SDE_SYSMON_VERSION "1.0.0"
#endif

namespace sysmon {
namespace {

struct NavItem {
  const char *id;
  const char *label;
  Icon icon;
};
const NavItem kNav[] = {
    {"processes", N_("Processes"), Icon::Processes},
    {"performance", N_("Performance"), Icon::Performance},
    {"disks", N_("File Systems"), Icon::Disks},
    {"startup", N_("Startup Apps"), Icon::Startup},
};
const int kNavCount = 4;

class NavRail : public Fl_Widget {
 public:
  std::function<void(int)> on_pick;
  std::function<void()> on_menu;  // header click or right click
  NavRail(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_NO_BOX); }
  int current = 0;

  void draw() override {
    const sde::Theme &t = sde::active_theme();
    Color rail = t.surface_alt;
    fl_push_clip(x(), y(), w(), h());
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), rail);
      p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), t.border);
      draw_icon(p, Icon::App, x() + S(20), y() + S(20), S(30), t.accent);
      for (int i = 0; i < kNavCount; ++i) {
        int iy = item_y(i);
        bool sel = i == current, hot = i == hover_;
        if (sel) {
          p.fill_rounded(x() + S(12), iy, w() - S(24), S(40), Sd(10), t.accent.alpha(t.dark ? 0.26 : 0.14));
          p.fill_rounded(x() + S(12), iy + S(11), S(4), S(18), Sd(2), t.accent);
        } else if (hot) {
          p.fill_rounded(x() + S(12), iy, w() - S(24), S(40), Sd(10), t.hover);
        }
        draw_icon(p, kNav[i].icon, x() + S(30), iy + S(11), S(18), sel ? t.accent_text : t.text);
        if (Fl::focus() == this && keyboard_ && sel) sde::focus_ring(p, x() + S(12), iy, w() - S(24), S(40), Sd(10));
      }
    }
    sde::text_font(t.font_size + S(1), true);
    sde::draw_text(_("System Monitor"), x() + S(60), y() + S(18), w() - S(70), S(34), t.text);
    for (int i = 0; i < kNavCount; ++i) {
      bool sel = i == current;
      sde::text_font(t.font_size, sel);
      sde::draw_text(_(kNav[i].label), x() + S(60), item_y(i), w() - S(76), S(40), sel ? t.accent_text : t.text);
    }
    fl_pop_clip();
  }

  int handle(int e) override {
    switch (e) {
      case FL_ENTER: return 1;
      case FL_FOCUS:
        keyboard_ = Fl::event() == FL_KEYBOARD;
        redraw();
        return 1;
      case FL_UNFOCUS: redraw(); return 1;
      case FL_LEAVE: set_hover(-1); return 1;
      case FL_MOVE: set_hover(hit(Fl::event_y())); return 1;
      case FL_PUSH: {
        keyboard_ = false;
        if ((Fl::event_button() == FL_RIGHT_MOUSE || Fl::event_y() < item_y(0)) && on_menu) {
          on_menu();
          return 1;
        }
        int i = hit(Fl::event_y());
        if (i >= 0 && on_pick) on_pick(i);
        return 1;
      }
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if ((k == FL_Up || k == FL_Down) && on_pick) {
          keyboard_ = true;
          on_pick((current + (k == FL_Up ? kNavCount - 1 : 1)) % kNavCount);
          return 1;
        }
        return 0;
      }
    }
    return Fl_Widget::handle(e);
  }

 private:
  int item_y(int i) const { return y() + S(76) + i * S(46); }
  int hit(int ey) const {
    for (int i = 0; i < kNavCount; ++i)
      if (ey >= item_y(i) && ey < item_y(i) + S(40)) return i;
    return -1;
  }
  void set_hover(int i) {
    if (i == hover_) return;
    hover_ = i;
    redraw();
  }
  int hover_ = -1;
  bool keyboard_ = false;  // focus ring only after keyboard use
};

// Live CPU and memory meters at the bottom of the rail; repainted alone.
class RailMeters : public Fl_Widget {
 public:
  RailMeters(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_NO_BOX); }
  void set(double cpu, double mem, const std::string &mem_text) {
    if (std::abs(cpu - cpu_) < 0.05 && std::abs(mem - mem_) < 0.05 && mem_text == mem_text_) return;
    cpu_ = cpu, mem_ = mem, mem_text_ = mem_text;
    redraw();
  }
  void draw() override {
    const sde::Theme &t = sde::active_theme();
    fl_push_clip(x(), y(), w(), h());
    int bx = x() + S(20), bw = w() - S(40);
    {
      Painter p(window()->w(), window()->h());
      p.fill_rect(x(), y(), w(), h(), t.surface_alt);
      p.line(x() + w() - 0.5, y(), x() + w() - 0.5, y() + h(), t.border);
      p.line(x() + S(16), y() + 0.5, x() + w() - S(16), y() + 0.5, t.border);
      for (int i = 0; i < 2; ++i) {
        double v = i ? mem_ : cpu_;
        int by = y() + S(40) + i * S(52);
        p.fill_rounded(bx, by, bw, S(6), Sd(3), t.border);
        p.fill_rounded(bx, by, std::max(Sd(6), bw * std::min(1.0, v / 100)), S(6), Sd(3),
                       i ? palette().memory : palette().cpu);
      }
    }
    for (int i = 0; i < 2; ++i) {
      int ty = y() + S(16) + i * S(52);
      sde::text_font(t.font_size - S(1));
      sde::draw_text(i ? _("Memory") : _("CPU"), bx, ty, bw, S(20), t.text_dim);
      sde::text_font(t.font_size - S(1), true);
      sde::draw_text(i ? mem_text_ : format_percent(cpu_), bx, ty, bw, S(20), t.text, 2);
    }
    fl_pop_clip();
  }

 private:
  double cpu_ = -1, mem_ = -1;
  std::string mem_text_;
};

class MainWindow : public MonitorWindow {
 public:
  MainWindow(int w, int h, int page) : MonitorWindow(w, h, _("System Monitor")) {
    int rail = S(220);
    nav_ = new NavRail(0, 0, rail, h - S(120));
    meters_ = new RailMeters(0, h - S(120), rail, S(120));
    int px = rail, pw = w - rail;
    pages_.push_back(make_processes_page(px, 0, pw, h, sampler_));
    pages_.push_back(make_performance_page(px, 0, pw, h));
    pages_.push_back(make_filesystems_page(px, 0, pw, h));
    pages_.push_back(make_startup_page(px, 0, pw, h));
    for (auto *p : pages_) p->hide();
    // Only the pages grow; the rail keeps its width.
    auto *grow = new Fl_Box(rail, 0, pw, h);
    grow->hide();
    resizable(grow);
    end();
    nav_->on_pick = [this](int i) { show_page(i); };
    nav_->on_menu = [this] { app_menu(); };
    size_range(S(760), S(520));
    xclass("sde-system-monitor");
    callback([](Fl_Widget *w, void *) {
      if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
      w->hide();
    });
    current_ = -1;
    show_page(page);
    sampler_.start([this] { Fl::awake(&MainWindow::on_sample, this); });
  }
  ~MainWindow() override { sampler_.stop(); }

  void resize(int X, int Y, int W, int H) override {
    MonitorWindow::resize(X, Y, W, H);
    int rail = S(220);
    nav_->resize(0, 0, rail, H - S(120));
    meters_->resize(0, H - S(120), rail, S(120));
    for (auto *p : pages_) p->resize(rail, 0, W - rail, H);
  }

  int handle(int e) override {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_F + 1) {
      show_about();
      return 1;
    }
    if (e == FL_SHORTCUT && (Fl::event_state() & FL_CTRL)) {
      int k = Fl::event_key();
      if (k == 'f') {
        pages_[current_]->focus_search();
        return 1;
      }
      if (k >= '1' && k < '1' + kNavCount) {
        show_page(k - '1');
        return 1;
      }
      if (k == 'q' || k == 'w') {
        hide();
        return 1;
      }
    }
    return MonitorWindow::handle(e);
  }

  static void show_about() {
    sde::AppInfo info;
    info.name = _("System Monitor");
    info.program = "sde-system-monitor";
    info.version = SDE_SYSMON_VERSION;
    info.icon = "sde-system-monitor";
    info.description = _("Shows running processes, live CPU, memory, disk and network graphs, "
                         "mounted file systems and the apps that start when you log in.");
    info.credits = {sde::toolkit_credit()};
    sde::show_about(info);
  }

  void app_menu() {
    sde::ContextMenu menu;
    for (int i = 0; i < kNavCount; ++i)
      menu.radio(_(kNav[i].label), i == current_, [this, i] { show_page(i); });
    menu.separator();
    sde::MenuItemOptions about;
    about.icon = "info";
    about.shortcut = FL_F + 1;
    menu.item(_("About System Monitor"), [] { show_about(); }, about);
    menu.popup();
  }

  void show_page(int i) {
    if (i < 0 || i >= int(pages_.size()) || i == current_) return;
    if (current_ >= 0) pages_[current_]->hide();
    current_ = i;
    nav_->current = i;
    nav_->redraw();
    pages_[i]->show();
    pages_[i]->activated();
    sampler_.want_processes(i == 0, i == 0);
    sampler_.want_filesystems(i == 2);
    if (auto snap = sampler_.latest()) pages_[i]->update(snap, true);
    redraw();
  }

 private:
  static void on_sample(void *data) {
    auto *self = static_cast<MainWindow *>(data);
    auto snap = self->sampler_.latest();
    if (!snap || snap->serial == self->last_serial_) return;
    self->last_serial_ = snap->serial;
    // Iconified or closed: keep the history, skip painting.
    bool shown = self->shown() && self->visible();
    for (int i = 0; i < int(self->pages_.size()); ++i)
      self->pages_[i]->update(snap, shown && i == self->current_);
    if (shown) {
      double mem = snap->mem.total ? 100.0 * double(snap->mem.used()) / double(snap->mem.total) : 0;
      self->meters_->set(snap->cpu_total, mem, format_percent(mem));
    }
  }

  Sampler sampler_;
  NavRail *nav_;
  RailMeters *meters_;
  std::vector<Page *> pages_;
  int current_ = 0;
  uint64_t last_serial_ = 0;
};

void usage(FILE *out) {
  fprintf(out,
          "Usage: sde-system-monitor [--page processes|performance|disks|startup]\n"
          "       sde-system-monitor --version | --help\n\n"
          "Shows running processes, live CPU, memory, disk and network graphs,\n"
          "mounted file systems and the apps that start when you log in.\n\n"
          "  --page NAME   open on a page (processes, performance, disks or\n"
          "                filesystems, startup)\n"
          "  --version     print the version and exit\n"
          "  --help        show this help and exit\n");
}

}  // namespace
}  // namespace sysmon

int main(int argc, char **argv) {
  using namespace sysmon;
  sde::init_i18n();
  int page = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version" || a == "-V") return sde::print_version("sde-system-monitor", SDE_SYSMON_VERSION);
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    }
    std::string name;
    if (a == "--page" && i + 1 < argc) name = argv[++i];
    else if (a.compare(0, 7, "--page=") == 0) name = a.substr(7);
    else {
      fprintf(stderr, "sde-system-monitor: unknown option '%s'\n", a.c_str());
      usage(stderr);
      return 2;
    }
    if (name == "processes") page = 0;
    else if (name == "performance" || name == "resources") page = 1;
    else if (name == "disks" || name == "filesystems" || name == "file-systems") page = 2;
    else if (name == "startup") page = 3;
    else {
      fprintf(stderr, "sde-system-monitor: unknown page '%s'\n", name.c_str());
      usage(stderr);
      return 2;
    }
  }
  sde::Config config;
  config.load();
  fl_open_display();
  auto theme = sde::Theme::from_config(config);
  theme.apply_to_fltk();
  Fl::lock();  // enables Fl::awake() from the sampler thread
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int w = std::min(S(1180), sw - S(24)), h = std::min(S(760), sh - S(40));
  auto *window = new MainWindow(w, h, page);
  window->position(sx + (sw - w) / 2, sy + (sh - h) / 2);
  window->show(1, argv);
  int rc = Fl::run();
  delete window;
  return rc;
}
