// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// calculator: the Calculator of the Selviana Desktop Environment.
// Standard, Scientific, Programmer and Converter modes with a History,
// written from scratch on SDE-FLTK (no GTK, no web view, works offline).
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xutil.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "about.h"
#include "config.h"
#include "converter.h"
#include "engine.h"
#include "i18n.h"
#include "icons.h"
#include "programmer.h"
#include "state.h"
#include "theme.h"
#include "ui.h"
#include "x11.h"
#include "wm.h"

#ifndef SDE_APP_VERSION
#define SDE_APP_VERSION "2.0.0"
#endif

using namespace calc;

namespace {

void usage(FILE *out) {
  fprintf(out, "%s",
          _("Usage: calculator [--mode standard|scientific|programmer|converter]\n"
            "       calculator --eval EXPRESSION [--angle deg|rad|grad]\n"
            "       calculator --eval EXPRESSION --base hex|dec|oct|bin [--bits 8|16|32|64]\n"
            "       calculator --convert VALUE FROM TO\n"
            "       calculator --version | --help\n"
            "\n"
            "Standard, scientific and programmer calculations, unit conversion and a\n"
            "history of your results.\n"
            "\n"
            "  --mode NAME     open in a mode\n"
            "  --eval EXPR     print the result of EXPR and exit (status 2 on errors)\n"
            "  --angle UNIT    angle unit of --eval (default: deg)\n"
            "  --base BASE     evaluate --eval as a programmer expression in BASE\n"
            "  --bits N        word size of --base (default: 64)\n"
            "  --convert V F T convert V from unit F to unit T (e.g. 1 km mi)\n"
            "  --version       print the version and exit\n"
            "  --help          show this help and exit\n"));
}

int eval_cli(const std::string &expr, const std::string &angle, const std::string &base, int bits) {
  if (!base.empty()) {
    int b = base == "hex" ? 16 : base == "oct" ? 8 : base == "bin" ? 2 : base == "dec" ? 10 : 0;
    if (!b) return 2;
    ProgResult r = evaluate_programmer(expr, b, bits);
    if (!r.ok) {
      fprintf(stderr, "%s\n", r.message.c_str());
      return 2;
    }
    printf("%s\n", format_base(r.value, b, bits).c_str());
    return 0;
  }
  Context ctx;
  if (angle == "rad") ctx.angle = Angle::Radians;
  else if (angle == "grad") ctx.angle = Angle::Gradians;
  else if (angle != "deg") return 2;
  Result r = evaluate(expr, ctx);
  if (!r.ok) {
    fprintf(stderr, "%s\n", r.message.c_str());
    return 2;
  }
  printf("%s\n", format_number(r.value).c_str());
  return 0;
}

int convert_cli(const std::string &value, const std::string &from, const std::string &to) {
  Result v = evaluate(value);
  if (!v.ok) {
    fprintf(stderr, "%s\n", v.message.c_str());
    return 2;
  }
  for (const auto &c : categories()) {
    int f = find_unit(c, from), t = find_unit(c, to);
    if (f < 0 || t < 0) continue;
    Number out;
    if (!convert(c, f, t, v.value, out)) {
      fprintf(stderr, "%s\n", _("Invalid input"));
      return 2;
    }
    printf("%s\n", format_number(out).c_str());
    return 0;
  }
  fprintf(stderr, "%s\n", _("Unknown units"));
  return 2;
}

// _NET_WM_ICON from the installed application icon, so the title bar and
// the taskbar show the Calculator icon instead of a generic one.
void set_window_icon(Window win) {
  std::vector<long> data;
  for (int size : {32, 64}) {
    std::string path;
    for (const char *name : {"sde-calculator", "org.debion.Calculator", "accessories-calculator"})
      if (!(path = sde::IconLoader::lookup(name, size)).empty()) break;
    if (path.empty()) return;
    cairo_surface_t *s = sde::load_image(path, size, size);
    if (!s) return;
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    if (w != size || h != size) {
      cairo_surface_destroy(s);
      continue;
    }
    cairo_surface_flush(s);
    const unsigned char *px = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    data.push_back(w);
    data.push_back(h);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        uint32_t p = *reinterpret_cast<const uint32_t *>(px + y * stride + x * 4);
        uint32_t a = p >> 24;
        uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        if (a && a < 255) {  // cairo is premultiplied, _NET_WM_ICON is not
          r = std::min(255u, r * 255 / a);
          g = std::min(255u, g * 255 / a);
          b = std::min(255u, b * 255 / a);
        }
        data.push_back(long((a << 24) | (r << 16) | (g << 8) | b));
      }
    cairo_surface_destroy(s);
  }
  if (!data.empty()) sde::x11::set_cardinals(win, "_NET_WM_ICON", data);
}

}  // namespace

int main(int argc, char **argv) {
  sde::init_i18n();
  std::string eval, angle = "deg", base, mode;
  bool have_eval = false;
  int bits = 64;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](std::string &out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    if (a == "--version" || a == "-V") return sde::print_version("calculator", SDE_APP_VERSION);
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    }
    if (a == "--eval") {
      if (!value(eval)) return 2;
      have_eval = true;
    } else if (a == "--angle") {
      if (!value(angle)) return 2;
    } else if (a == "--base") {
      if (!value(base)) return 2;
    } else if (a == "--bits") {
      std::string b;
      if (!value(b)) return 2;
      bits = std::atoi(b.c_str());
      if (bits != 8 && bits != 16 && bits != 32 && bits != 64) return 2;
    } else if (a == "--convert") {
      if (i + 3 >= argc) return 2;
      return convert_cli(argv[i + 1], argv[i + 2], argv[i + 3]);
    } else if (a == "--mode") {
      if (!value(mode)) return 2;
    } else if (a.compare(0, 7, "--mode=") == 0) {
      mode = a.substr(7);
    } else {
      fprintf(stderr, "calculator: unknown option %s (see --help)\n", a.c_str());
      return 2;
    }
  }
  if (have_eval) return eval_cli(eval, angle, base, bits);

  Settings settings;
  settings.load(settings_path());
  if (!mode.empty()) settings.mode = mode_from_id(mode);

  sde::Config config;
  config.load();
  fl_open_display();
  sde::Theme::from_config(config).apply_to_fltk();

  double scale = sde::ui_scale() > 0 ? sde::ui_scale() : 1;
  int w = settings.width > 0 ? int(settings.width * scale) : sde::S(360) + (settings.history_visible ? sde::S(270) : 0);
  int h = settings.height > 0 ? int(settings.height * scale)
                              : (settings.mode == Mode::Programmer ? sde::S(660) : sde::S(600));
  int wx = 0, wy = 0;
  sde::fit_to_work_area(wx, wy, w, h);
  w = std::max(sde::S(300), w);
  h = std::max(sde::S(470), h);
  auto *window = new CalcWindow(w, h, settings);
  window->position(wx, wy);
  Fl::add_handler(CalcWindow::x_event_handler);
  window->show(1, argv);
  XClassHint hint{const_cast<char *>("calculator"), const_cast<char *>("org.debion.Calculator")};
  XSetClassHint(fl_display, fl_xid(window), &hint);
  set_window_icon(fl_xid(window));
  Window root = RootWindow(fl_display, fl_screen);
  XWindowAttributes attr;
  XGetWindowAttributes(fl_display, root, &attr);
  XSelectInput(fl_display, root, attr.your_event_mask | PropertyChangeMask);
  window->set_mode(settings.mode);
  int status = Fl::run();
  delete window;
  return status;
}
