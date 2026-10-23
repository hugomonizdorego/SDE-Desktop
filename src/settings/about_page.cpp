// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Control Center > About, in two clearly separate parts:
//
//  * Selviana Desktop Environment: the official SDE logo (the variant for
//    light or dark backgrounds), the SDE version, build information, the
//    desktop session, the development log, copyright, license, trademarks,
//    the open-source licenses of every program and component, and the
//    donation button with the PayPal trademark note.
//  * System: this computer and its operating system. The operating system
//    is whatever /etc/os-release names (PRETTY_NAME): SDE runs on Debian
//    and on distributions based on it, such as DebION GNU/Linux, which are
//    separate projects. Then the kernel, hardware and graphics.
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/x.H>
#include <sys/utsname.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "about.h"
#include "build_config.h"
#include "build_history.h"
#include "build_info.h"
#include "devices.h"
#include "gpu.h"
#include "i18n.h"
#include "icons.h"
#include "legal.h"
#include "paint.h"
#include "process.h"
#include "settings.h"
#include "spawn.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "widgets.h"
#include "x11.h"

namespace sde {

namespace {

const char *const kDonateUrl = "https://paypal.me/hugocenturion0311";

// The official SDE logo, re-rendered when the theme turns light or dark:
// sde-logo on light backgrounds, sde-logo-dark on dark ones
// (/usr/share/sde/branding), else the sde-logo icon.
class LogoBox : public Fl_Widget {
 public:
  LogoBox(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  ~LogoBox() override { drop(); }
  void draw() override {
    const Theme &t = settings_theme();
    if (!logo_ || dark_ != t.dark || lw_ != w() || lh_ != h()) {
      drop();
      logo_ = legal::load_branding({legal::logo_name(t.dark), "sde-logo", "sde-mark"}, w(), h(),
                                   {"sde-logo", "start-here"});
      dark_ = t.dark;
      lw_ = w();
      lh_ = h();
    }
    if (!logo_) return;
    Painter p(window()->w(), window()->h());
    int iw = cairo_image_surface_get_width(logo_), ih = cairo_image_surface_get_height(logo_);
    p.surface(logo_, x(), y() + (h() - ih) / 2);
    (void)iw;
  }
  // Width / height of the installed logo (1 when there is none).
  static double aspect(bool dark) {
    cairo_surface_t *s = legal::load_branding({legal::logo_name(dark), "sde-logo", "sde-mark"}, 1024, 1024,
                                              {"sde-logo", "start-here"});
    if (!s) return 1.0;
    double a = double(cairo_image_surface_get_width(s)) / std::max(1, cairo_image_surface_get_height(s));
    cairo_surface_destroy(s);
    return a;
  }

 private:
  void drop() {
    if (logo_) cairo_surface_destroy(logo_);
    logo_ = nullptr;
  }
  cairo_surface_t *logo_ = nullptr;
  bool dark_ = false;
  int lw_ = 0, lh_ = 0;
};

// A "key: value" line; |detail| is a dim second line.
void info_row(Page *page, const std::string &key, const std::string &value, const std::string &detail = "") {
  int ry = page->cursor(), x = page->left() + S(20), w = page->width() - S(40);
  int kw = std::min(S(190), w / 3), vw = w - kw - S(10);
  int vh = std::max(S(30), Label::measure(value, vw, Label::NORMAL) + S(6));
  new Label(x, ry, kw, S(30), key, Label::DIM);
  new Label(x + kw + S(10), ry, vw, vh, value, Label::NORMAL, true);
  page->advance(vh);
  if (!detail.empty()) {
    int dh = Label::measure(detail, vw, Label::DIM) + S(4);
    new Label(x + kw + S(10), page->cursor() - S(4), vw, dh, detail, Label::DIM, true);
    page->advance(dh);
  }
}

FlatButton *button(int x, int y, int w, int h, const std::string &label, std::function<void()> action) {
  auto *b = new FlatButton(x, y, w, h, label);
  b->callback([](Fl_Widget *, void *fn) { (*static_cast<std::function<void()> *>(fn))(); },
              keep(new std::function<void()>(std::move(action))));
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
  if (!check) return _("none");
  std::string n = x11::get_utf8(check, "_NET_WM_NAME");
  return n.empty() ? _("unknown") : n;
}

// OpenGL renderer from "glxinfo -B" (or "eglinfo -B"), asked once per run
// in the background with a time limit. 0 unknown, 1 running, 2 known.
int g_gl_state = 0;
std::string g_gl_renderer;
void start_gl_query() {
  static bool started = false;
  if (started) return;
  started = true;
  std::vector<std::string> argv;
  if (!find_program("glxinfo").empty())
    argv = {"glxinfo", "-B"};
  else if (!find_program("eglinfo").empty())
    argv = {"eglinfo", "-B"};
  if (argv.empty()) return;
  g_gl_state = 1;
  bool ok = run_async(with_timeout(argv, 5), [](const std::string &out, int) {
    g_gl_renderer = parse_gl_renderer(out);
    g_gl_state = g_gl_renderer.empty() ? 0 : 2;
    refresh_if_shown("about");
  });
  if (!ok) g_gl_state = 0;
}

bool g_history_all = false;

// "What's new in each build": the newest entries of build-history.json
// (from CHANGELOG.md); the three newest are open, the others behind a button.
void build_history_card(Page *page) {
  static bool loaded = false;
  static BuildHistory history;
  if (!loaded) {
    loaded = true;
    std::string source = build_history_path(), text;
    if (source.empty() || !read_file(source, text) || !parse_build_history(text, history)) history = BuildHistory();
  }
  page->begin_card(_("What's new in each build"));
  if (history.entries.empty()) {
    page->note(_("The development log (build-history.json) is not installed."));
    page->end_card();
    return;
  }
  int x = page->left() + S(20), w = page->width() - S(40);
  size_t shown = g_history_all ? history.entries.size() : std::min<size_t>(3, history.entries.size());
  for (size_t k = 0; k < shown; ++k) {
    const BuildEntry &e = history.entries[k];
    std::string head = e.title + (e.date.empty() ? "" : "  ·  " + e.date);
    new Label(x, page->cursor(), w, S(26), head, Label::BOLD);
    page->advance(S(26));
    if (!e.summary.empty()) {
      int sh = Label::measure(e.summary, w, Label::DIM) + S(2);
      new Label(x, page->cursor(), w, sh, e.summary, Label::DIM, true);
      page->advance(sh);
    }
    for (const auto &item : e.items) {
      // A hanging bullet: wrapped lines start under the text.
      int tx = x + S(28), tw = w - S(28);
      int ih = Label::measure(item, tw, Label::NORMAL) + S(2);
      new Label(x + S(12), page->cursor(), S(16), S(22), "•", Label::NORMAL);
      new Label(tx, page->cursor(), tw, ih, item, Label::NORMAL, true);
      page->advance(ih);
    }
    page->advance(S(10));
  }
  if (history.entries.size() > 3) {
    std::string label = g_history_all ? std::string(_("Show fewer builds"))
                                      : format(_("Show all %d builds"), int(history.entries.size()));
    page->row(_("Development log"), S(200), S(34), [label](int bx, int by, int bw, int bh) {
      auto *b = new FlatButton(bx, by, bw, bh, label);
      b->callback([](Fl_Widget *, void *) {
        g_history_all = !g_history_all;
        Fl::add_timeout(0, [](void *) { refresh_page(); });
      });
      return b;
    });
  }
  page->end_card();
}

// The desktop itself, for the license viewer.
AppInfo desktop_info() {
  AppInfo info;
  info.name = legal::kProjectName;
  info.program = "sde";
  info.version = SDE_VERSION;
  info.icon = "sde-logo";
  return info;
}

void sde_part(Page *page) {
  const Theme &t = settings_theme();
  page->heading(legal::kProjectName, _("The desktop environment: version, build, licenses and credits"));

  // Logo and version. A wide logo (with its name) goes on top; a square
  // one stands left of the text, or above it in narrow windows. The name
  // is the part's heading, so the card shows the release.
  page->begin_card();
  int y = page->cursor() + S(14);
  int x = page->left() + S(24), w = page->width() - S(48);
  bool wide = LogoBox::aspect(t.dark) >= 2.0;
  int tx = x, tw = w, logo_bottom = y;
  if (wide) {
    int lh = S(64);
    new LogoBox(x, y, std::min(w, S(380)), lh);
    y += lh + S(14);
  } else if (w < S(420)) {
    new LogoBox(x, y, S(72), S(72));
    y += S(72) + S(12);
  } else {
    new LogoBox(x, y, S(88), S(88));
    tx = x + S(108);
    tw = w - S(108);
    logo_bottom = y + S(88);
  }
  auto line = [&](const std::string &text, Label::Style style) {
    int lh = std::max(S(22), Label::measure(text, tw, style) + S(4));
    new Label(tx, y, tw, lh, text, style, true);
    y += lh;
  };
  line(format(_("Version %s"), SDE_VERSION_LABEL), Label::TITLE);
  line(_("Small and fast, with a modern desktop experience."), Label::SUBTITLE);
  line(format("%s  \xC2\xB7  %s", developer_line().c_str(), SDE_VERSION), Label::DIM);
  page->advance(std::max(y, logo_bottom) + S(14) - page->cursor());
  page->end_card();

  // Build information, embedded at build time (tools/build-info.py).
  page->begin_card(_("Build"));
  info_row(page, _("Version"), format("%s (%s)", SDE_VERSION_LABEL, SDE_VERSION));
  info_row(page, _("Build"), std::string(SDE_BUILD_NUMBER).empty() ? std::string(_("Unknown"))
                                                                   : std::string(SDE_BUILD_NUMBER));
  info_row(page, _("Build date"), SDE_BUILD_DATE);
  info_row(page, _("Commit"), std::string(SDE_BUILD_COMMIT).empty()
                                  ? std::string(_("Unknown (not built from a source repository)"))
                                  : std::string(SDE_BUILD_COMMIT));
  info_row(page, _("Developer"), legal::kDeveloper);
  info_row(page, _("Website"), legal::kWebsite);
  page->end_card();

  // Copyright, license, trademarks and the licenses of every component.
  page->begin_card(_("Copyright and licenses"));
  info_row(page, _("Copyright"), legal::kCopyright);
  page->row(legal::license_name(legal::kLicense), S(170), S(34),
            [](int bx, int by, int bw, int bh) {
              return button(bx, by, bw, bh, _("View License"), [] { show_license(desktop_info()); });
            },
            _("SDE is free software: you can redistribute it and change it under the terms of this "
              "license. It comes with ABSOLUTELY NO WARRANTY."));
  page->row(_("Open-source licenses"), S(170), S(34),
            [](int bx, int by, int bw, int bh) {
              return button(bx, by, bw, bh, _("Show Licenses"), [] { show_open_source_licenses(); });
            },
            _("Every SDE program and bundled component with its license and copyright holders."));
  page->note(legal::trademark("sde"));
  page->note(_("The Selviana Desktop Environment is inspired by the Equinox Desktop Environment (EDE) "
               "and contains no EDE code."));
  page->end_card();

  // Support development: an honest note and a PayPal link.
  page->begin_card(_("Support development"));
  {
    std::string text = _("SDE is in alpha. The developer keeps monitoring it and improving it with every "
                         "build. Donations are optional and help keep that work going; SDE stays free "
                         "software either way.");
    int lw = page->width() - S(40), lh = Label::measure(text, lw, Label::NORMAL) + S(8);
    new Label(page->left() + S(20), page->cursor(), lw, lh, text, Label::NORMAL, true);
    page->advance(lh + S(6));
  }
  page->row(_("PayPal"), S(200), S(36), [](int bx, int by, int bw, int bh) {
    auto *b = new FlatButton(bx, by, bw, bh, _("Donate via PayPal"), true);
    b->callback([](Fl_Widget *, void *) { spawn_detached({"xdg-open", kDonateUrl}); });
    b->tooltip(kDonateUrl);
    return b;
  }, legal::trademark("paypal"));
  page->end_card();

  page->begin_card(_("Desktop session"));
  info_row(page, _("Window manager"), wm_name());
  info_row(page, _("Toolkit"), "SDE-FLTK " SDE_FLTK_VERSION);
  info_row(page, _("Theme"), t.gtk_theme);
  info_row(page, _("Interface size"), format("%d %%", int(std::lround(t.scale * 100))));
  page->end_card();

  build_history_card(page);
}

void system_part(Page *page) {
  page->advance(S(10));
  page->heading(_("System"), _("This computer and its operating system"));

  struct utsname u;
  uname(&u);
  std::string os = legal::os_pretty_name();
  std::string mem = read_first("/proc/meminfo", "MemTotal");
  if (!mem.empty()) mem = format("%.1f GiB", atof(mem.c_str()) / 1024.0 / 1024.0);
  std::string cpu = read_first("/proc/cpuinfo", "model name");
  int sx, sy, sw, sh;
  Fl::screen_xywh(sx, sy, sw, sh, 0);
  page->begin_card(_("Operating system"));
  info_row(page, _("Operating system"), os.empty() ? std::string(_("Unknown (no /etc/os-release)")) : os);
  info_row(page, _("Kernel"), std::string(u.sysname) + " " + u.release);
  info_row(page, _("Architecture"), u.machine);
  page->end_card();

  page->begin_card(_("Hardware"));
  info_row(page, _("Processor"), cpu.empty() ? u.machine : cpu);
  info_row(page, _("Memory"), mem);
  info_row(page, _("Display"), format("%d x %d", sw, sh));
  page->end_card();

  // Graphics: every display controller (hybrid laptops have two), its
  // kernel driver and the OpenGL renderer actually used for drawing.
  static bool gpus_read = false;
  static std::vector<Gpu> gpus;
  if (!gpus_read) {
    gpus = detect_gpus();
    gpus_read = true;
  }
  start_gl_query();
  page->begin_card(_("Graphics"));
  int in_use = g_gl_state == 2 ? renderer_gpu(gpus, g_gl_renderer) : -1;
  if (gpus.empty()) info_row(page, _("Graphics card"), _("No PCI graphics card found"));
  for (size_t i = 0; i < gpus.size(); ++i) {
    const Gpu &g = gpus[i];
    std::string key = gpus.size() > 1 ? format(_("Graphics card %d"), int(i) + 1)
                                      : std::string(_("Graphics card"));
    std::string detail = g.driver.empty() ? std::string(_("No driver in use"))
                                          : format(_("Driver: %s"), g.driver.c_str());
    if (g.boot_vga && gpus.size() > 1) detail += std::string("  ·  ") + _("Primary");
    if (int(i) == in_use) detail += std::string("  ·  ") + _("Drawing the desktop");
    detail += "  ·  " + format("PCI %s [%s:%s]", g.slot.c_str(), g.vendor_id.c_str(),
                               g.device_id.c_str());
    info_row(page, key, gpu_label(g), detail);
  }
  std::string gl = g_gl_state == 2   ? g_gl_renderer
                   : g_gl_state == 1 ? std::string(_("Checking..."))
                                     : std::string(_("Unknown (install mesa-utils for glxinfo)"));
  info_row(page, _("OpenGL renderer"), gl);
  page->end_card();

  // Trademark notices of the names shown in this part (the kernel is
  // Linux; the operating system may name Debian).
  std::vector<std::string> keys = legal::trademark_keys_for_text(os + " " + u.sysname);
  page->note(_("SDE is developed separately from the operating system it runs on; the name of the "
               "operating system comes from /etc/os-release."));
  for (const auto &k : keys) page->note(legal::trademark(k));
}

}  // namespace

void build_about(Page *page) {
  sde_part(page);
  system_part(page);
  page->finish();
}

}  // namespace sde
