// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Welcome (sde-welcome): the first-login greeting of SDE, after the
// welcome screen concept of Linux Mint (no Mint code or artwork). The header
// keeps the operating system and the desktop apart: "Welcome to <NAME>"
// with the operating system's logo (both from os-release), and below it the
// desktop, the Selviana Desktop Environment with the SDE logo and version.
// Since 0.1.0~alpha51 one calm window: a small header, six first steps
// (Appearance, Software Updater, Drivers, Snapshots or System Reports, Help,
// Donate) and "Show this dialog at startup" with Close.
//
// /etc/xdg/autostart/sde-welcome.desktop runs "sde-welcome --autostart" at
// every login. It opens the window at the first login of each user after
// installation (stamp ~/.config/sde/welcome-done), later only while
// sde.conf [welcome] show=true, and never in the live session of the ISO
// (welcome::is_live_session). Started from the menu it always opens; in a
// live session it says that settings are not kept.
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "about.h"
#include "buffered_window.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "icons.h"
#include "legal.h"
#include "model.h"
#include "paint.h"
#include "shell.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

using namespace sde;

namespace {

bool g_live = false;
bool g_keyboard = false;  // the last input was a key: show focus rings

const Theme &theme() { return shell::theme(); }

// ------------------------------------------------------------- widgets

// A first step: icon, label and a one-line hint on a soft rounded tile.
class Tile : public Fl_Button {
 public:
  Tile(int x, int y, int w, int h, std::vector<std::string> icons, std::string label, std::string hint,
       std::function<void()> run)
      : Fl_Button(x, y, w, h), icons_(std::move(icons)), label_(std::move(label)), hint_(std::move(hint)),
        run_(std::move(run)) {
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
    callback([](Fl_Widget *w, void *) {
      auto *t = static_cast<Tile *>(w);
      if (t->run_) t->run_();
    });
  }

  void draw() override {
    const Theme &t = theme();
    bool hot = Fl::belowmouse() == this, down = value() != 0;
    int isz = std::min(S(32), h() - S(20));
    int box = isz + S(12);
    {
      Painter p(window()->w(), window()->h());
      Color bg = down ? t.hover.mix(t.text, 0.06) : hot ? t.hover : t.surface;
      p.fill_rounded(x(), y(), w(), h(), Sd(10), bg);
      p.stroke_rounded(x() + 0.5, y() + 0.5, w() - 1, h() - 1, Sd(10), t.border);
      p.fill_rounded(x() + S(14), y() + (h() - box) / 2.0, box, box, Sd(8), t.accent.alpha(t.dark ? 0.20 : 0.10));
      p.image(IconLoader::get(icons_, isz), x() + S(14) + S(6), y() + (h() - isz) / 2.0, isz);
      if (Fl::focus() == this && g_keyboard) focus_ring(p, x(), y(), w(), h(), Sd(10));
    }
    int tx = x() + S(14) + box + S(14), tw = w() - (tx - x()) - S(12);
    int mid = y() + h() / 2;
    text_font(t.font_size + 1, true);
    draw_text(label_, tx, mid - S(21), tw, S(22), t.text);
    text_font(t.font_size - 1);
    draw_text(hint_, tx, mid + S(1), tw, S(20), t.text_dim);
  }

  int handle(int e) override {
    if (e == FL_FOCUS) g_keyboard = Fl::event() == FL_KEYBOARD;
    if (e == FL_ENTER || e == FL_LEAVE || e == FL_FOCUS || e == FL_UNFOCUS) {
      redraw();
      return 1;
    }
    if (e == FL_KEYBOARD && Fl::focus() == this &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter || Fl::event_key() == ' ')) {
      do_callback();
      return 1;
    }
    return Fl_Button::handle(e);
  }

 private:
  std::vector<std::string> icons_;
  std::string label_, hint_;
  std::function<void()> run_;
};

// Themed check box with its label.
class CheckBox : public Fl_Button {
 public:
  CheckBox(int x, int y, int w, int h, std::string label) : Fl_Button(x, y, w, h), label_(std::move(label)) {
    type(FL_TOGGLE_BUTTON);
    box(FL_NO_BOX);
    down_box(FL_NO_BOX);
  }
  static int width_for(const std::string &label) {
    text_font(theme().font_size);
    return S(28) + text_width(label) + S(8);
  }
  void draw() override {
    const Theme &t = theme();
    int b = S(18);
    double by = y() + (h() - b) / 2.0;
    {
      Painter p(window()->w(), window()->h());
      if (value()) {
        p.fill_rounded(x() + S(2), by, b, b, Sd(4), t.accent);
        cairo_t *cr = p.cr();
        cairo_set_line_width(cr, Sd(2));
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        p.set(t.on_accent);
        cairo_move_to(cr, x() + S(2) + b * 0.26, by + b * 0.52);
        cairo_line_to(cr, x() + S(2) + b * 0.44, by + b * 0.70);
        cairo_line_to(cr, x() + S(2) + b * 0.76, by + b * 0.32);
        cairo_stroke(cr);
      } else {
        p.fill_rounded(x() + S(2), by, b, b, Sd(4), t.surface);
        p.stroke_rounded(x() + S(2) + 0.5, by + 0.5, b - 1, b - 1, Sd(4), t.text_dim.mix(t.background, 0.35),
                         Sd(1.2));
      }
      if (Fl::focus() == this && g_keyboard) focus_ring(p, x() + S(2), by, b, b, Sd(4));
    }
    text_font(t.font_size);
    draw_text(label_, x() + S(28), y(), w() - S(28), h(), t.text);
  }
  int handle(int e) override {
    if (e == FL_FOCUS) g_keyboard = Fl::event() == FL_KEYBOARD;
    if (e == FL_FOCUS || e == FL_UNFOCUS) {
      redraw();
      return 1;
    }
    if (e == FL_KEYBOARD && Fl::focus() == this && Fl::event_key() == ' ') {
      value(!value());
      do_callback();
      return 1;
    }
    return Fl_Button::handle(e);
  }

 private:
  std::string label_;
};

// ------------------------------------------------------------- window

struct Step {
  std::vector<std::string> icons;
  std::string label, hint;
  std::function<void()> run;
};

std::vector<Step> first_steps() {
  std::vector<Step> s;
  s.push_back({{"preferences-desktop-theme", "preferences-desktop-appearance", "preferences-desktop"},
               _("Appearance"),
               _("Theme, accent and wallpaper"),
               [] { shell::launch({"sde-settings", "--page", "appearance"}); }});
  s.push_back({{"system-software-update", "software-update-available", "update-manager"},
               _("Software Updater"),
               _("Install security fixes and updates"),
               [] { shell::launch({"sde-update"}); }});
  s.push_back({{"jockey", "preferences-system", "application-x-firmware", "computer"},
               _("Drivers"),
               _("Additional and proprietary drivers"),
               [] { shell::launch({"sde-drivers"}); }});
  welcome::Tool ts = welcome::snapshot_tool();
  if (!ts.argv.empty())
    s.push_back({{"timeshift", "document-revert", "backup", "drive-harddisk"},
                 _("Snapshots"),
                 _("Restore the system if an update breaks it"),
                 [ts] { shell::launch(ts.argv); }});
  else
    s.push_back({{"apport", "utilities-system-monitor", "dialog-information"},
                 _("System Reports"),
                 _("Crashes and system details"),
                 [] { shell::launch({"sde-system-reports"}); }});
  s.push_back({{"help-browser", "system-help", "help-contents"},
               _("Help"),
               _("Guides and troubleshooting"),
               [] { shell::open_url(welcome::kDocsUrl); }});
  s.push_back({{"emblem-favorite", "starred", "face-smile"},
               _("Donate"),
               _("Support the development of SDE"),
               [] { shell::open_url(welcome::kDonateUrl); }});
  return s;
}

int pad() { return S(28); }
int header_h() { return S(118); }

// The operating system's name and logo from os-release (NAME, LOGO); SDE
// never assumes which distribution it runs on.
const std::string &os_name() {
  static const std::string name = legal::os_name();
  return name;
}
std::vector<std::string> os_logo_icons() {
  std::vector<std::string> names;
  std::string logo = os_release("LOGO");
  if (!logo.empty()) names.push_back(logo);
  for (const char *n : {"distributor-logo", "start-here", "computer"}) names.push_back(n);
  return names;
}
int footer_h() { return S(64); }

class WelcomeWindow : public BufferedWindow {
 public:
  WelcomeWindow(int w, int h, const char *title) : BufferedWindow(w, h, title) {
    steps_ = first_steps();
    for (auto &s : steps_) tiles_.push_back(new Tile(0, 0, 10, 10, s.icons, s.label, s.hint, s.run));
    std::string cb = _("Show this dialog at startup");
    check_ = new CheckBox(0, 0, CheckBox::width_for(cb), S(32), cb);
    check_->value(welcome::show_at_startup(shell::config()));
    check_->callback([](Fl_Widget *w, void *) {
      welcome::set_show_at_startup(shell::config(), static_cast<CheckBox *>(w)->value() != 0);
      shell::config().save();
    });
    if (g_live) check_->hide();  // nothing is kept in a live session
    close_ = new FlatButton(0, 0, S(96), S(34), _("Close"), true);
    close_->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
    end();
    layout();
  }

  void resize(int X, int Y, int W, int H) override {
    BufferedWindow::resize(X, Y, W, H);
    layout();
  }

  void layout() {
    int top = header_h();
    int gap = S(12);
    int cols = w() < S(520) ? 1 : 2;
    int rows = int((tiles_.size() + size_t(cols) - 1) / size_t(cols));
    int gw = w() - 2 * pad();
    int gh = h() - top - footer_h() - S(14);
    int tw = (gw - gap * (cols - 1)) / cols;
    int th = std::max(S(40), std::min(S(84), (gh - gap * (rows - 1)) / std::max(1, rows)));
    for (size_t i = 0; i < tiles_.size(); ++i) {
      int c = int(i) % cols, r = int(i) / cols;
      tiles_[i]->resize(pad() + c * (tw + gap), top + r * (th + gap), tw, th);
    }
    int fy = h() - footer_h() + (footer_h() - S(34)) / 2;
    close_->resize(w() - pad() - S(96), fy, S(96), S(34));
    check_->resize(pad() - S(2), fy + 1, check_->w(), S(32));
    redraw();
  }

  void draw() override {
    const Theme &t = theme();
    int lsz = S(48), msz = S(20);
    int tx = pad() + lsz + S(16);
    int desk_y = S(84);
    {
      Painter p(w(), h());
      p.fill_rect(0, 0, w(), h(), t.background);
      // The operating system ...
      p.image(IconLoader::get(os_logo_icons(), lsz), pad(), S(26), lsz);
      // ... and the desktop, with the SDE logo.
      if (!sde_mark_ || mark_dark_ != t.dark) {
        if (sde_mark_) cairo_surface_destroy(sde_mark_);
        sde_mark_ = legal::load_branding({"sde-mark", legal::logo_name(t.dark)}, msz, msz, {"sde-logo"});
        mark_dark_ = t.dark;
      }
      if (sde_mark_)
        p.surface(sde_mark_, tx, desk_y + (S(24) - cairo_image_surface_get_height(sde_mark_)) / 2);
      if (g_live) {  // in the footer, where the check box would be
        int ly = h() - footer_h() + (footer_h() - S(32)) / 2;
        p.fill_rounded(pad(), ly, close_->x() - pad() - S(16), S(32), Sd(8), t.surface_alt);
        p.circle(pad() + S(16), ly + S(16), Sd(4), t.warning);
      }
      p.fill_rect(pad(), h() - footer_h(), w() - 2 * pad(), std::max(1, S(1)), t.border);
    }
    text_font(t.font_size + 7, true);
    std::string title = os_name().empty() ? std::string(_("Welcome")) : format(_("Welcome to %s"), os_name().c_str());
    draw_text(title, tx, S(24), w() - tx - pad(), S(30), t.text);
    text_font(t.font_size);
    draw_text(_("A few first steps to make this computer yours."), tx, S(54), w() - tx - pad(), S(22), t.text_dim);
    // "Desktop: Selviana Desktop Environment 0.1 Alpha 52"
    int dx = tx + (sde_mark_ ? msz + S(8) : 0);
    text_font(t.font_size - 1);
    std::string label = std::string(_("Desktop:")) + " ";
    draw_text(label, dx, desk_y, w() - dx - pad(), S(24), t.text_dim);
    int lw = text_width(label);
    text_font(t.font_size - 1, true);
    draw_text(format("%s %s", legal::kProjectName, SDE_VERSION_LABEL), dx + lw, desk_y, w() - dx - lw - pad(), S(24),
              t.text);
    if (g_live) {
      text_font(t.font_size - 1);
      std::string note = os_name().empty()
                             ? std::string(_("Live session \xe2\x80\x94 install the system to keep your settings"))
                             : format(_("Live session \xe2\x80\x94 install %s to keep your settings"),
                                      os_name().c_str());
      draw_text(note, pad() + S(30), h() - footer_h() + (footer_h() - S(32)) / 2, close_->x() - pad() - S(56), S(32),
                t.text);
    }
    draw_children();
  }

  int handle(int e) override {
    if (e == FL_PUSH) g_keyboard = false;
    if (e == FL_KEYBOARD && Fl::event_key() == FL_Escape) {
      hide();
      return 1;
    }
    int r = BufferedWindow::handle(e);
    if (!r && handle_about_event(e, about)) return 1;
    return r;
  }

  AppInfo about;

  ~WelcomeWindow() override {
    if (sde_mark_) cairo_surface_destroy(sde_mark_);
  }

 private:
  cairo_surface_t *sde_mark_ = nullptr;
  bool mark_dark_ = false;
  std::vector<Step> steps_;
  std::vector<Tile *> tiles_;
  CheckBox *check_ = nullptr;
  FlatButton *close_ = nullptr;
};

AppInfo app_info() {
  AppInfo info;
  info.name = _("SDE Welcome");
  info.program = "sde-welcome";
  info.version = SDE_VERSION;
  info.icon = "sde-welcome";
  info.icon_fallbacks = {"sde-logo"};
  info.description = _("First steps after installation: appearance, updates, drivers, snapshots and "
                       "help.");
  info.credits = {_("Welcome screen concept after Linux Mint's mintwelcome (no code or artwork "
                    "used)"),
                  toolkit_credit()};
  return info;
}

const char *kUsage =
    "Usage: sde-welcome [--autostart] [--live-status] [--version]\n\n"
    "  --autostart     login: open only at the first login after installation, or\n"
    "                  while \"Show this dialog at startup\" is on; never live\n"
    "  --live-status   print whether this is a live session and why, then exit\n";

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  bool autostart = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") return print_version(app_info());
    if (a == "--autostart") {
      autostart = true;
    } else if (a == "--live-status") {
      std::string why = welcome::live_reason(welcome::probe_live_session());
      printf("%s\n", why.empty() ? "installed system" : ("live session: " + why).c_str());
      printf("welcome shown before: %s\n", welcome::welcome_done() ? "yes" : "no");
      return why.empty() ? 1 : 0;
    } else if (a == "--page" && i + 1 < argc) {
      ++i;  // alpha50 and older had pages; the window has one view now
    } else if (starts_with(a, "--page=")) {
    } else {
      fputs(kUsage, a == "--help" ? stdout : stderr);
      return a == "--help" ? 0 : 2;
    }
  }
  g_live = welcome::is_live_session();
  if (autostart) {
    Config cfg;
    cfg.load();
    if (!welcome::should_autostart(g_live, welcome::welcome_done(), welcome::show_at_startup(cfg))) return 0;
  }
  shell::init();
  if (!g_live) welcome::mark_welcome_done();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, 0);
  int W = std::min(S(720), sw - S(16)), H = std::min(S(480), sh - S(40));
  auto *win = new WelcomeWindow(W, H, _("Welcome"));
  win->about = app_info();
  win->xclass("sde-welcome");
  win->position(sx + (sw - W) / 2, sy + (sh - H) / 2);
  win->size_range(std::min(W, S(420)), std::min(H, S(360)));
  win->resizable(win);
  win->show();
  return Fl::run();
}
