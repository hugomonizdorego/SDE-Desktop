// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DebION Dev-Team
//
// SDE Welcome (sde-welcome): the first-login guide of SDE on DebION. A
// sidebar of pages, after the welcome screen concept of Linux Mint (no Mint
// code or artwork): Welcome, First Steps (colors, panel layout, updates,
// drivers, snapshots, firewall, codecs, software), Driver Manager, System
// Reports, Help and Contribute. Started at login by
// /etc/xdg/autostart/sde-welcome.desktop ("sde-welcome --autostart") while
// sde.conf [welcome] show=true; the sidebar switch turns that off.
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "config.h"
#include "i18n.h"
#include "model.h"
#include "pages.h"
#include "paint.h"
#include "process.h"
#include "shell.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"

using namespace sde;

namespace {

// ------------------------------------------------------------- pickers

// A row of preview tiles with a label under each; one is selected.
class TilePicker : public Fl_Widget {
 public:
  using DrawFn = std::function<void(Painter &, int, int, int, int, int)>;
  TilePicker(int x, int y, int w, int h, std::vector<std::string> labels, int selected, DrawFn draw_fn,
             std::function<void(int)> pick)
      : Fl_Widget(x, y, w, h),
        labels_(std::move(labels)),
        selected_(selected),
        draw_fn_(std::move(draw_fn)),
        pick_(std::move(pick)) {}

  int count() const { return int(labels_.size()); }
  int gap() const { return S(14); }
  int tile_w() const { return (w() - gap() * (count() - 1)) / std::max(1, count()); }
  int tile_at(int ex) const {
    int i = (ex - x()) / (tile_w() + gap());
    return (i >= 0 && i < count()) ? i : -1;
  }

  void draw() override {
    const Theme &t = shell::theme();
    int tw = tile_w(), ph = h() - S(28);
    {
      Painter p(window()->w(), window()->h());
      for (int i = 0; i < count(); ++i) {
        int tx = x() + i * (tw + gap());
        bool sel = i == selected_;
        p.fill_rounded(tx, y(), tw, ph, S(12), t.surface_alt);
        draw_fn_(p, i, tx + S(10), y() + S(10), tw - S(20), ph - S(20));
        p.stroke_rounded(tx, y(), tw, ph, S(12), sel ? t.accent : t.border, sel ? Sd(2.5) : 1.0);
        if (Fl::focus() == this && i == focus_) focus_ring(p, tx, y(), tw, ph, S(12));
      }
    }
    for (int i = 0; i < count(); ++i) {
      int tx = x() + i * (tw + gap());
      text_font(t.font_size + 1, i == selected_);
      draw_text(labels_[size_t(i)], tx, y() + ph + S(4), tw, S(22),
                i == selected_ ? t.accent_text : t.text, 1);
    }
  }

  int handle(int e) override {
    switch (e) {
      case FL_PUSH:
        if (Fl::visible_focus()) take_focus();
        return 1;
      case FL_RELEASE:
        if (Fl::event_inside(this)) choose(tile_at(Fl::event_x()));
        return 1;
      case FL_FOCUS:
      case FL_UNFOCUS:
        focus_ = selected_ < 0 ? 0 : selected_;
        redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k == FL_Left || k == FL_Right) {
          focus_ = (focus_ + (k == FL_Right ? 1 : count() - 1)) % count();
          redraw();
          return 1;
        }
        if (k == ' ' || k == FL_Enter || k == FL_KP_Enter) {
          choose(focus_);
          return 1;
        }
        return 0;
      }
      default:
        return Fl_Widget::handle(e);
    }
  }

 private:
  void choose(int i) {
    if (i < 0 || i == selected_) return;
    selected_ = i;
    redraw();
    auto fn = pick_;
    // The callback may rebuild the page, which deletes this widget.
    Fl::add_timeout(0, [](void *d) {
      auto *f = static_cast<std::pair<std::function<void(int)>, int> *>(d);
      f->first(f->second);
      delete f;
    }, new std::pair<std::function<void(int)>, int>(fn, i));
  }

  std::vector<std::string> labels_;
  int selected_, focus_ = 0;
  DrawFn draw_fn_;
  std::function<void(int)> pick_;
};

// Accent color swatches.
class AccentPicker : public Fl_Widget {
 public:
  AccentPicker(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  static int cell() { return S(36); }
  void draw() override {
    const Theme &t = shell::theme();
    ThemeChoice c = theme_choice(shell::config());
    Painter p(window()->w(), window()->h());
    auto acc = theme_accents();
    for (size_t i = 0; i < acc.size(); ++i) {
      double cx = x() + cell() * (i + 0.5), cy = y() + h() / 2.0;
      Color col = theme_accent_color(c.family, acc[i], t.dark);
      if (acc[i] == c.accent) p.circle(cx, cy, S(15), t.text);
      if (acc[i] == c.accent) p.circle(cx, cy, S(13), t.surface);
      p.circle(cx, cy, S(11), col);
    }
  }
  int handle(int e) override {
    if (e == FL_PUSH) return 1;
    if (e == FL_RELEASE && Fl::event_inside(this)) {
      int i = (Fl::event_x() - x()) / cell();
      auto acc = theme_accents();
      if (i >= 0 && i < int(acc.size())) {
        std::string name = acc[size_t(i)];
        Fl::add_timeout(0, [](void *d) {
          auto *n = static_cast<std::string *>(d);
          Config &cfg = shell::config();
          ThemeChoice c = theme_choice(cfg);
          cfg.set("appearance", "accent_name", *n);
          cfg.set("appearance", "accent", theme_accent_color(c.family, *n, false).to_hex());
          delete n;
          shell::apply_config(true);
        }, new std::string(name));
      }
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

// A tiny window in a palette, for the mode tiles.
void mini_window(Painter &p, const Theme &pal, int x, int y, int w, int h) {
  p.fill_rounded(x, y, w, h, S(8), pal.background);
  p.fill_rounded(x, y, w, S(14), S(8), pal.titlebar_bg);
  p.fill_rect(x, y + S(8), w, S(6), pal.titlebar_bg);
  p.fill_rounded(x + S(8), y + S(22), w * 0.3, h - S(30), S(5), pal.surface_alt);
  p.fill_rounded(x + w * 0.3 + S(16), y + S(22), w * 0.7 - S(24), S(10), S(4), pal.surface);
  p.fill_rounded(x + w * 0.3 + S(16), y + S(38), w * 0.45, S(8), S(4), pal.surface);
  p.fill_rounded(x + w - S(48), y + h - S(22), S(40), S(14), S(5), pal.accent);
  p.stroke_rounded(x, y, w, h, S(8), pal.border);
}

void build_colors(Page *page) {
  Config &cfg = shell::config();
  ThemeChoice c = theme_choice(cfg);
  page->begin_card(_("Desktop colors"));
  page->note(_("Choose a light or dark desktop and an accent color. SDE programs, GTK applications "
               "and window frames change at once."));
  const char *modes[] = {"light", "dark", "auto"};
  int sel = c.mode == "dark" ? 1 : c.mode == "auto" ? 2 : 0;
  int y = page->cursor() + S(6);
  int w = std::min(page->width() - S(40), S(560));
  std::string family = c.family, accent = c.accent;
  new TilePicker(page->left() + S(20), y, w, S(128), {_("Light"), _("Dark"), _("Auto")}, sel,
                 [family, accent](Painter &p, int i, int x, int yy, int ww, int hh) {
                   if (i < 2) {
                     mini_window(p, Theme::make(family, i == 1, accent), x, yy, ww, hh);
                   } else {
                     // Light in the day, dark in the evening: half and half.
                     cairo_save(p.cr());
                     cairo_rectangle(p.cr(), x, yy, ww / 2.0, hh);
                     cairo_clip(p.cr());
                     mini_window(p, Theme::make(family, false, accent), x, yy, ww, hh);
                     cairo_restore(p.cr());
                     cairo_save(p.cr());
                     cairo_rectangle(p.cr(), x + ww / 2.0, yy, ww / 2.0, hh);
                     cairo_clip(p.cr());
                     mini_window(p, Theme::make(family, true, accent), x, yy, ww, hh);
                     cairo_restore(p.cr());
                   }
                 },
                 [modes](int i) {
                   shell::config().set("appearance", "theme", modes[i]);
                   shell::apply_config(true);
                 });
  page->advance(S(140));
  page->row(_("Accent color"), AccentPicker::cell() * int(theme_accents().size()), S(38),
            [](int x, int yy, int, int h) {
              return new AccentPicker(x, yy, AccentPicker::cell() * int(theme_accents().size()), h);
            });
  shell::button_row(page, _("Theme style, transparency, fonts, wallpaper"), _("Appearance Settings"),
                    [] { shell::launch({"sde-settings", "--page", "appearance"}); });
  page->end_card();
}

void build_panel_layout(Page *page) {
  Config &cfg = shell::config();
  page->begin_card(_("Panel layout"));
  page->note(_("Pick how the panel looks. You can change it later in Settings > Panel."));
  const auto &layouts = welcome::panel_layouts();
  std::string cur = cfg.get("panel", "style", "full");
  int sel = 0;
  std::vector<std::string> labels;
  for (size_t i = 0; i < layouts.size(); ++i) {
    labels.push_back(_(layouts[i].label));
    if (cur == layouts[i].id) sel = int(i);
  }
  bool top = lower(cfg.get("panel", "position", "bottom")) == "top";
  int w = std::min(page->width() - S(40), S(560));
  new TilePicker(page->left() + S(20), page->cursor() + S(6), w, S(118), labels, sel,
                 [top](Painter &p, int i, int x, int y, int ww, int hh) {
                   const Theme &t = shell::theme();
                   p.fill_rounded(x, y, ww, hh, S(6), t.background);
                   p.fill_rounded(x + S(10), y + (top ? S(26) : S(10)), ww * 0.45, S(28), S(4), t.surface);
                   std::string id = welcome::panel_layouts()[size_t(i)].id;
                   int len = id == "full" ? ww : id == "short" ? ww * 2 / 3 : ww - S(16);
                   int bx = x + (ww - len) / 2;
                   int by = top ? (id == "floating" ? y + S(4) : y) : (id == "floating" ? y + hh - S(18) : y + hh - S(14));
                   p.fill_rounded(bx, by, len, S(14), id == "full" ? 0 : S(5), t.surface);
                   p.stroke_rounded(bx, by, len, S(14), id == "full" ? 0 : S(5), t.border);
                   p.fill_rounded(bx + S(4), by + S(3), S(16), S(8), S(3), t.accent);
                   for (int k = 0; k < 3; ++k) p.circle(bx + S(28 + k * 10), by + S(7), S(2), t.text_dim);
                   // The Show desktop strip at the far end.
                   p.line(bx + len - S(6), by + S(3), bx + len - S(6), by + S(11), t.text_dim);
                 },
                 [](int i) {
                   shell::config().set("panel", "style", welcome::panel_layouts()[size_t(i)].id);
                   shell::apply_config();
                 });
  page->advance(S(130));
  for (size_t i = 0; i < layouts.size(); ++i)
    if (int(i) == sel) page->note(_(layouts[i].description));
  page->choice_row(_("Position"), {_("Bottom"), _("Top")}, top ? 1 : 0, [](int v) {
    shell::config().set("panel", "position", v == 1 ? "top" : "bottom");
    shell::apply_config();
    Fl::add_timeout(0, [](void *) { shell::refresh(); });
  });
  page->switch_row(_("Show desktop button"), cfg.get_bool("panel", "show_desktop_button", false),
                   [](bool on) {
                     shell::config().set("panel", "show_desktop_button", on ? "true" : "false");
                     shell::apply_config();
                   },
                   _("A thin strip at the far end of the panel: click it to minimize every window "
                     "and click again to bring them back. Super+D does the same."));
  shell::button_row(page, _("Pinned applications, clock, tray icons"), _("Panel Settings"),
                    [] { shell::launch({"sde-settings", "--page", "panel"}); });
  page->end_card();
}

// ------------------------------------------------------------- pages

void build_welcome(Page *page) {
  std::string os = os_release("PRETTY_NAME");
  shell::hero(page, {"sde-logo", "start-here", "computer"},
              _("Welcome to SDE"),
              format(_("The Selviana Desktop Environment %s on %s, by the DebION Dev-Team. Thank you "
                       "for trying it!"),
                     SDE_VERSION, os.empty() ? "DebION GNU/Linux" : os.c_str()));
  page->begin_card(_("Getting started"));
  page->note(_("This guide helps you set up the computer in a few minutes: pick your colors and "
               "panel, install updates and drivers, protect your files with snapshots and turn on "
               "the firewall. Everything here can also be found later in the Start menu and in "
               "Settings."));
  shell::button_row(page, _("Colors, panel, updates, drivers, snapshots and more"), _("First Steps"),
                    [] { shell::show_page("first-steps"); }, "", true);
  shell::button_row(page, _("Guides, troubleshooting and where to ask for help"), _("Documentation"),
                    [] { shell::show_page("help"); });
  page->end_card();
  page->begin_card(_("About DebION and SDE"));
  page->note(_("DebION GNU/Linux is based on Debian testing. SDE is its own desktop, written from "
               "scratch in C++ on SDE-FLTK, cairo and Xlib: the panel with the Start menu and "
               "Action Center, the desktop, Settings, the Liria file manager and native "
               "applications such as Niny, Xanara, Taca, Gallery and Photo."));
  page->note(_("This is an alpha release: things change quickly, and some may break. Crash reports "
               "under System Reports help fix them."));
  page->end_card();
}

void build_first_steps(Page *page) {
  page->heading(_("First Steps"), _("Set up your desktop and keep the system safe"));
  const Theme &t = shell::theme();
  build_colors(page);
  build_panel_layout(page);

  shell::step(page, {"system-software-update", "software-update-available"}, _("Software Updater"),
              _("Updates fix security problems and bugs. Software Updater checks for them and shows a "
                "notification; install them regularly. Critical, security, main and Flatpak updates are grouped."),
              {{_("Launch"), [] { shell::launch({"sde-update"}); }, true}});

  shell::step(page, {"jockey", "preferences-system", "application-x-firmware", "computer"},
              _("Driver Manager"),
              _("Some graphics cards, Wi-Fi and Bluetooth adapters need drivers or firmware that are "
                "not free software. The Driver Manager finds them and installs Debian's packages."),
              {{_("Launch"), [] { shell::show_page("drivers"); }}});

  {
    welcome::Tool ts = welcome::snapshot_tool();
    std::vector<shell::Action> actions;
    std::string status;
    Color sc = t.text_dim;
    if (!ts.argv.empty()) {
      status = _("Timeshift is installed");
      sc = t.success;
      actions.push_back({_("Launch"), [ts] { shell::launch(ts.argv); }, true});
    } else {
      status = _("Timeshift is not installed");
      sc = t.warning;
      actions.push_back({_("Install Timeshift"), [] {
                           shell::install_packages({"timeshift"}, _("Installing Timeshift..."));
                         }, true});
    }
    shell::step(page, {"timeshift", "document-revert", "backup", "drive-harddisk"}, _("System Snapshots"),
                _("A snapshot saves the state of the system files. If an update or a change breaks "
                  "something, Timeshift restores an earlier snapshot in a few minutes. Your personal "
                  "files are not included: back them up separately. Schedule daily snapshots on a "
                  "disk with enough free space."),
                actions, status, &sc);
  }

  {
    bool ufw = !find_program("ufw").empty() || file_exists("/usr/sbin/ufw") || file_exists("/etc/ufw/ufw.conf");
    std::string conf;
    read_file("/etc/ufw/ufw.conf", conf);
    bool on = ufw && welcome::ufw_conf_enabled(conf);
    welcome::Tool gui = welcome::firewall_tool();
    std::vector<shell::Action> actions;
    std::string status;
    Color sc = t.warning;
    if (!ufw) {
      status = _("No firewall is installed");
      actions.push_back({_("Install Firewall"), [] {
                           shell::install_packages({"ufw", "gufw"}, _("Installing the firewall..."));
                         }, true});
    } else if (!on) {
      status = _("The firewall is off");
      actions.push_back({_("Turn On"), [] {
                           shell::run_admin({"ufw", "enable"}, _("Turning the firewall on..."),
                                            [](const std::string &out, int st) {
                                              if (st != 0 && st != 126 && st != 127)
                                                fl_alert("%s", out.c_str());
                                            });
                         }, true});
    } else {
      status = _("The firewall is on");
      sc = t.success;
    }
    if (ufw && !gui.argv.empty())
      actions.push_back({_("Configure"), [gui] { shell::launch(gui.argv); }});
    else if (ufw && gui.argv.empty())
      actions.push_back({_("Install gufw"), [] { shell::install_packages({"gufw"}, ""); }});
    shell::step(page, {"security-high", "gufw", "network-firewall", "channel-secure"}, _("Firewall"),
                _("The firewall blocks incoming connections that you did not ask for. On a laptop "
                  "that joins public Wi-Fi it is a good idea to turn it on; the default rules still "
                  "let you browse, update and share nothing by accident."),
                actions, status, &sc);
  }

  {
    std::vector<std::string> missing;
    for (auto &p : welcome::codec_packages())
      if (!shell::package_installed(p)) missing.push_back(p);
    std::vector<shell::Action> actions;
    std::string status;
    Color sc = t.success;
    if (missing.empty()) {
      status = _("Multimedia codecs are installed");
    } else {
      sc = t.warning;
      status = format(ngettext("%d codec package is missing", "%d codec packages are missing",
                               missing.size()),
                      int(missing.size()));
      actions.push_back({_("Install Codecs"), [missing] {
                           shell::install_packages(missing, _("Installing multimedia codecs..."));
                         }, true});
    }
    shell::step(page, {"applications-multimedia", "multimedia-player", "video-x-generic"},
                _("Multimedia Codecs"),
                _("Plays and records more audio and video formats (MP3, H.264, AAC and others) in "
                  "GStreamer applications. Debian ships these in its main archive "
                  "(gstreamer1.0-plugins-good, -bad, -ugly and gstreamer1.0-libav), so no extra "
                  "repository is needed. Some formats may be patent-encumbered in your country."),
                actions, status, &sc);
  }

  {
    auto centers = welcome::software_centers();
    std::vector<shell::Action> actions;
    std::string status;
    Color sc = t.text_dim;
    if (!centers.empty()) {
      welcome::Tool c = centers.front();
      status = c.name;
      sc = t.success;
      actions.push_back({_("Launch"), [c] { shell::launch(c.argv); }, true});
    } else {
      status = _("No software center is installed");
      actions.push_back({_("Install GNOME Software"), [] {
                           shell::install_packages({"gnome-software"}, _("Installing GNOME Software..."));
                         }, true});
    }
    shell::step(page, {"system-software-install", "softwarecenter", "applications-other"},
                _("Software"),
                _("Thousands of applications are available from Debian. A software center lets you "
                  "browse, install and remove them; in a terminal, \"apt install NAME\" does the "
                  "same."),
                actions, status, &sc);
  }

  shell::step(page, {"apport", "utilities-system-monitor", "dialog-warning"}, _("System Reports"),
              _("If an SDE program crashes, its report with a backtrace is listed here, ready to "
                "copy or send to the developers on GitHub."),
              {{_("Launch"), [] { shell::show_page("reports"); }}});
}

void build_help(Page *page) {
  page->heading(_("Documentation & Help"), _("Learn how SDE works and where to get help"));
  std::string local = std::string(SDE_DATADIR) + "/doc/sde";
  shell::step(page, {"help-browser", "system-help", "help-contents"}, _("Documentation"),
              _("Guides for the desktop, the suite and building SDE, including the release notes of "
                "each alpha."),
              {{_("Open"), [] { shell::open_url(welcome::kDocsUrl); }, true},
               {_("Local Copy"), [local] { shell::open_url(dir_exists(local) ? local : "/usr/share/doc/sde"); }}});
  shell::step(page, {"system-help", "dialog-question", "help-browser"}, _("Troubleshooting"),
              _("What to do when the panel restarts in safe mode, the compositor misbehaves or a "
                "program will not start. \"sde-session --check\" in a terminal tests the "
                "installation."),
              {{_("Open"), [] {
                  shell::open_url(std::string(welcome::kProjectUrl) + "/blob/main/docs/TROUBLESHOOTING.md");
                }}});
  shell::step(page, {"bug-buddy", "apport", "dialog-warning"}, _("Report a problem"),
              _("Found a bug or have an idea? Open an issue on GitHub. System Reports fills in the "
                "details for crashes."),
              {{_("Open Issues"), [] { shell::open_url(welcome::kIssuesUrl); }},
               {_("System Reports"), [] { shell::show_page("reports"); }}});
  shell::step(page, {"internet-chat", "system-users"}, _("Debian community"),
              _("DebION follows Debian testing, so Debian's documentation, wiki and forums answer "
                "most questions about packages and hardware."),
              {{_("Debian Wiki"), [] { shell::open_url("https://wiki.debian.org/"); }},
               {_("Forums"), [] { shell::open_url("https://forums.debian.net/"); }}});
}

void build_contribute(Page *page) {
  page->heading(_("Contribute"), _("SDE is free software made by volunteers"));
  shell::step(page, {"emblem-favorite", "starred", "face-smile"}, _("Donate"),
              _("SDE and DebION are developed in our free time. A donation through PayPal helps pay "
                "for hardware and testing. Thank you!"),
              {{_("Donate with PayPal"), [] { shell::open_url(welcome::kDonateUrl); }, true}});
  shell::step(page, {"applications-development", "text-x-script", "utilities-terminal"}, _("Code"),
              _("The source code, the build guide and the roadmap are on GitHub. Pull requests are "
                "welcome."),
              {{_("GitHub"), [] { shell::open_url(welcome::kProjectUrl); }}});
  shell::step(page, {"preferences-desktop-locale", "config-language", "accessories-dictionary"},
              _("Translations"),
              _("SDE uses gettext: translate po/sde.pot into your language and send the .po file."),
              {{_("How to Translate"), [] {
                  shell::open_url(std::string(welcome::kProjectUrl) + "/blob/main/po/README.md");
                }}});
  shell::step(page, {"applications-science", "bug-buddy", "applications-utilities"}, _("Testing"),
              _("Every alpha needs testers. Report what works and what does not, with the system "
                "information from System Reports."),
              {{_("Report a Problem"), [] { shell::open_url(welcome::kIssuesUrl); }}});
}

AppInfo app_info() {
  AppInfo info;
  info.name = _("SDE Welcome");
  info.program = "sde-welcome";
  info.version = SDE_VERSION;
  info.icon = "sde-logo";
  info.description = _("First steps with SDE on DebION: colors, panel layout, updates, drivers, "
                       "snapshots, firewall, codecs and help.");
  info.credits = {_("Welcome screen concept after Linux Mint's mintwelcome (no code or artwork "
                    "used)"),
                  toolkit_credit()};
  return info;
}

const char *kUsage =
    "Usage: sde-welcome [--page NAME] [--autostart] [--version]\n\n"
    "  --page NAME   open a page: welcome, first-steps, drivers, reports, help, contribute\n"
    "  --autostart   do nothing unless sde.conf [welcome] show is true (login)\n";

}  // namespace

int main(int argc, char **argv) {
  init_i18n();
  std::string start = "welcome";
  bool autostart = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version") {
      printf("sde-welcome %s (%s) - %s\n", SDE_VERSION, SDE_VERSION_LABEL, kDeveloper);
      return 0;
    }
    if (a == "--autostart") {
      autostart = true;
    } else if (a == "--page" && i + 1 < argc) {
      start = argv[++i];
    } else if (starts_with(a, "--page=")) {
      start = a.substr(7);
    } else {
      fputs(kUsage, a == "--help" ? stdout : stderr);
      return a == "--help" ? 0 : 2;
    }
  }
  if (autostart) {
    Config cfg;
    cfg.load();
    if (!welcome::show_at_startup(cfg)) return 0;
  }
  shell::init();
  shell::Options o;
  o.title = _("SDE Welcome");
  o.xclass = "sde-welcome";
  o.about = app_info();
  o.width = 980;
  o.height = 700;
  o.pages = {
      {"welcome", _("Welcome"), {"user-home", "go-home"}, build_welcome},
      {"first-steps", _("First Steps"), {"preferences-desktop", "emblem-default"}, build_first_steps},
      {"drivers", _("Driver Manager"), {"jockey", "application-x-firmware", "preferences-system", "computer"},
       sysreport::build_drivers_page},
      {"reports", _("System Reports"), {"apport", "utilities-system-monitor", "dialog-warning"}, sysreport::build_reports_page},
      {"help", _("Documentation"), {"help-browser", "help-contents"}, build_help},
      {"contribute", _("Contribute"), {"emblem-favorite", "starred"}, build_contribute},
  };
  bool known = false;
  for (auto &p : o.pages) known = known || p.id == start;
  if (!known) {
    fprintf(stderr, "sde-welcome: unknown page %s\n%s", start.c_str(), kUsage);
    return 2;
  }
  o.start = start;
  o.sidebar_switch = _("Show this dialog at startup");
  o.sidebar_switch_value = welcome::show_at_startup(shell::config());
  o.sidebar_switch_changed = [](bool on) {
    welcome::set_show_at_startup(shell::config(), on);
    shell::config().save();
  };
  return shell::run(std::move(o));
}
