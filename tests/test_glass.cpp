// SPDX-License-Identifier: GPL-3.0-or-later
// Translucent surfaces over a busy web page (0.2.0 Alpha 3.1).
//
// SDE's translucent surfaces must stay readable over anything, the worst
// being a busy, high-contrast page (black text on white, a black video with
// bright shapes, colourful thumbnails). This test draws such a page (and a
// dark one), puts every kind of surface on it the way the compositor draws
// them with the configuration SDE generates, and measures:
//
//  * Liquid Glass (the compositor's glass shader, src/common/integration.cpp):
//    the compositor's blur (a Gaussian a little weaker than its dual_kawase
//    strength), then the shader's frost, saturation, brightness, the tint
//    with the surface color and the lift that keeps text readable, with the
//    constants of the shader SDE writes for that theme;
//  * whole-window translucency (Graphite and Fluent, other toolkits' menus,
//    window frames, the XRender and lite profiles): the surface and its text
//    blended over the blurred (or, without blur, sharp) page;
//
// for every family, mode, accent, transparency (25, 50, 80, 100) and
// compositor profile (OpenGL, XRender, lite):
//
//  * text 4.5:1 and dimmed text 3:1 (WCAG 2.1) against the surface around
//    it, wherever it is on the page;
//  * a calm surface: what the page leaves in it is soft colour, not the
//    page's text or edges (the luminance of a surface may differ from its
//    own 4 px blur by at most a few percent);
//  * the steps of the slider are even (Off, Subtle, Clear, Glass) and
//    without blur the surfaces stay almost opaque.
#include <cairo.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "config.h"
#include "integration.h"
#include "theme.h"
#include "util.h"

using namespace sde;

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

namespace {

constexpr int W = 360, H = 240;

struct Rgb {
  double r = 0, g = 0, b = 0;
};
Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Rgb operator*(Rgb a, double k) { return {a.r * k, a.g * k, a.b * k}; }
Rgb mix(Rgb a, Rgb b, double t) { return a * (1 - t) + b * t; }
Rgb clamp01(Rgb a) {
  auto c = [](double v) { return std::min(1.0, std::max(0.0, v)); };
  return {c(a.r), c(a.g), c(a.b)};
}
Rgb of(const Color &c) { return {c.r, c.g, c.b}; }
double lin(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double lum(Rgb c) { return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b); }
double contrast(double la, double lb) {
  if (la < lb) std::swap(la, lb);
  return (la + 0.05) / (lb + 0.05);
}

using Image = std::vector<Rgb>;

// The busy page: a video site in the light or dark theme of a browser.
Image busy_page(bool dark) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  cairo_t *cr = cairo_create(s);
  double bg = dark ? 0.06 : 1.0, fg = dark ? 0.95 : 0.06;
  cairo_set_source_rgb(cr, bg, bg, bg);
  cairo_paint(cr);
  // The player: black with bright shapes and big letters.
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_rectangle(cr, 10, 10, 200, 120);
  cairo_fill(cr);
  unsigned seed = 7;
  auto rnd = [&seed](int n) { return int(rand_r(&seed) % unsigned(n)); };
  for (int i = 0; i < 18; ++i) {
    double c[][3] = {{1, 0.85, 0}, {1, 1, 1}, {0.9, 0.45, 0}, {0.16, 0.16, 0.16}};
    auto &k = c[rnd(4)];
    cairo_set_source_rgb(cr, k[0], k[1], k[2]);
    cairo_save(cr);
    cairo_translate(cr, 20 + rnd(170), 20 + rnd(100));
    cairo_scale(cr, 6 + rnd(18), 5 + rnd(14));
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_fill(cr);
  }
  cairo_set_source_rgb(cr, 1, 0.9, 0.25);
  for (int i = 0; i < 6; ++i) cairo_rectangle(cr, 24 + i * 26, 40, 18, 34);
  cairo_fill(cr);
  // Text: rows of glyph-like strokes, 1 to 2 px wide.
  cairo_set_source_rgb(cr, fg, fg, fg);
  for (int row = 0; row < 6; ++row)
    for (int x = 10; x < 210;) {
      int w = 1 + rnd(2), h = 7 + rnd(4), y = 140 + row * 16;
      cairo_rectangle(cr, x, y + (11 - h), w, h);
      if (rnd(3) == 0) cairo_rectangle(cr, x, y + 4, 4 + rnd(4), 1 + rnd(2));
      x += w + 2 + rnd(4) + (rnd(7) == 0 ? 5 : 0);
    }
  cairo_fill(cr);
  // Thumbnails in strong colours, with a title each.
  for (int i = 0; i < 4; ++i) {
    double y = 10 + i * 58;
    cairo_set_source_rgb(cr, rnd(256) / 255.0, rnd(256) / 255.0, rnd(256) / 255.0);
    cairo_rectangle(cr, 222, y, 64, 48);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, rnd(256) / 255.0, rnd(256) / 255.0, rnd(256) / 255.0);
    cairo_move_to(cr, 222, y + 48);
    cairo_line_to(cr, 286, y + 14);
    cairo_line_to(cr, 286, y + 48);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, fg, fg, fg);
    for (int x = 292; x < 352; x += 3 + rnd(3)) cairo_rectangle(cr, x, y + 4, 1 + rnd(2), 9);
    cairo_fill(cr);
  }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  Image img(W * H);
  const unsigned char *d = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const unsigned char *p = d + y * stride + x * 4;
      img[y * W + x] = {p[2] / 255.0, p[1] / 255.0, p[0] / 255.0};
    }
  cairo_surface_destroy(s);
  return img;
}

// A Gaussian blur (separable, edges clamped), in the 8-bit sRGB values the
// compositor blends.
Image blur(const Image &in, double sigma) {
  if (sigma < 0.3) return in;
  int r = int(std::ceil(sigma * 3));
  std::vector<double> k(2 * r + 1);
  double sum = 0;
  for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-0.5 * i * i / (sigma * sigma));
  for (double &v : k) v /= sum;
  Image tmp(in.size()), out(in.size());
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      Rgb a;
      for (int i = -r; i <= r; ++i) a = a + in[y * W + std::min(W - 1, std::max(0, x + i))] * k[i + r];
      tmp[y * W + x] = a;
    }
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      Rgb a;
      for (int i = -r; i <= r; ++i) a = a + tmp[std::min(H - 1, std::max(0, y + i)) * W + x] * k[i + r];
      out[y * W + x] = a;
    }
  return out;
}

Rgb sample(const Image &img, double x, double y) {
  x = std::min(W - 1.0, std::max(0.0, x));
  y = std::min(H - 1.0, std::max(0.0, y));
  int x0 = int(x), y0 = int(y), x1 = std::min(W - 1, x0 + 1), y1 = std::min(H - 1, y0 + 1);
  double fx = x - x0, fy = y - y0;
  Rgb a = mix(img[y0 * W + x0], img[y0 * W + x1], fx), b = mix(img[y1 * W + x0], img[y1 * W + x1], fx);
  return mix(a, b, fy);
}

// The equivalent Gaussian of the compositor's blur: dual_kawase "strength"
// reaches about min_radius pixels (picom's table); a third of that, less a
// little, is a fair sigma. XRender's is a Gaussian of its own deviation.
double blur_sigma(const std::string &conf) {
  std::smatch m;
  if (std::regex_search(conf, m, std::regex("method = \"dual_kawase\"; strength = (\\d+)"))) {
    static const int reach[] = {1, 6, 11, 17, 24, 32, 40, 51, 67, 83, 101, 123};
    int s = std::max(1, std::min(12, atoi(m[1].str().c_str())));
    return reach[s - 1] / 3.5;
  }
  if (std::regex_search(conf, m, std::regex("method = \"gaussian\"; size = \\d+; deviation = ([0-9.]+)")))
    return atof(m[1].str().c_str());
  return 0;
}

double constant(const std::string &glsl, const std::string &name) {
  std::smatch m;
  if (!std::regex_search(glsl, m, std::regex("const float " + name + " = ([-0-9.]+);"))) {
    fprintf(stderr, "  no %s in the shader\n", name.c_str());
    ++g_failures;
    return 0;
  }
  return atof(m[1].str().c_str());
}

// "const vec3 NAME = vec3(r, g, b);" of the shader.
Rgb vec3_constant(const std::string &glsl, const std::string &name) {
  std::smatch m;
  if (!std::regex_search(glsl, m, std::regex("const vec3 " + name + " = vec3\\(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\\);"))) {
    fprintf(stderr, "  no %s in the shader\n", name.c_str());
    ++g_failures;
    return {};
  }
  return {atof(m[1].str().c_str()), atof(m[2].str().c_str()), atof(m[3].str().c_str())};
}

// CIE lightness L* (0-100) of a relative luminance: equal steps look equal.
double lightness(double y) { return y > 0.008856 ? 116 * std::cbrt(y) - 16 : 903.3 * y; }

// The calm-surface measure: how far the lightness of a surface strays from
// its mean over 9 x 9 pixels (sharp structure the page leaves in it: text,
// edges), at most, in L* units. About 3 is a faint trace; the page's own
// text is about 90.
double structure(const std::vector<double> &l) {
  std::vector<double> sum((W + 1) * (H + 1), 0.0);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      sum[(y + 1) * (W + 1) + x + 1] = lightness(l[y * W + x]) + sum[y * (W + 1) + x + 1] +
                                       sum[(y + 1) * (W + 1) + x] - sum[y * (W + 1) + x];
  double worst = 0;
  for (int y = 8; y < H - 8; ++y)
    for (int x = 8; x < W - 8; ++x) {
      int x0 = x - 4, y0 = y - 4, x1 = x + 5, y1 = y + 5;
      double mean = (sum[y1 * (W + 1) + x1] - sum[y0 * (W + 1) + x1] - sum[y1 * (W + 1) + x0] +
                     sum[y0 * (W + 1) + x0]) / 81;
      worst = std::max(worst, std::fabs(lightness(l[y * W + x]) - mean));
    }
  return worst;
}

struct Result {
  double text = 99, dim = 99, structure = 0;
};

// Liquid Glass: every pixel of a surface over the page, as the shader draws
// it where there is no text (the frost and the legibility lift).
Result liquid(const Image &page, const std::string &glsl, double tint, const Theme &t) {
  // The panel's and popups' glass colour as the shader has it (0.2.0 Alpha
  // 4: the floating layer's, with the wallpaper's tint).
  Rgb key = vec3_constant(glsl, "KEY_A");
  double frost = constant(glsl, "FROST"), saturation = constant(glsl, "SATURATION");
  double brightness = constant(glsl, "BRIGHTNESS"), legible = constant(glsl, "LEGIBLE");
  bool dark = constant(glsl, "DARK") > 0.5;
  std::vector<double> l(page.size());
  Result r;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      Rgb sum = sample(page, x, y);
      for (int i = 0; i < 12; ++i) {
        double a = i * 2.39996, rr = frost * std::sqrt((i + 0.5) / 12.0);
        sum = sum + sample(page, x + rr * std::cos(a), y + rr * std::sin(a));
      }
      Rgb back = sum * (1.0 / 13);
      double luma = 0.2126 * back.r + 0.7152 * back.g + 0.0722 * back.b;
      back = clamp01(mix(Rgb{luma, luma, luma}, back, saturation) * brightness);
      auto ok = [&](double tt) { return (lum(clamp01(mix(back, key, tt))) - legible) * (dark ? -1 : 1) >= 0; };
      double use = tint;
      if (!ok(tint) && ok(1.0)) {
        double lo = tint, hi = 1.0;
        for (int i = 0; i < 7; ++i) {
          double mid = (lo + hi) / 2;
          (ok(mid) ? hi : lo) = mid;
        }
        use = hi;
      }
      double g = lum(clamp01(mix(back, key, use)));
      l[y * W + x] = g;
      r.text = std::min({r.text, contrast(t.text.luminance(), g), contrast(t.menu_text.luminance(), g)});
      r.dim = std::min(r.dim, contrast(t.text_dim.luminance(), g));
    }
  r.structure = structure(l);
  return r;
}

// A surface faded as a whole (its text too) over the page: text against
// the surface around it within a glyph's box (9 x 13 px).
Result faded(const Image &page, Rgb surface, Rgb text, Rgb dim, double alpha) {
  std::vector<double> ls(page.size()), lt(page.size()), ld(page.size());
  for (size_t i = 0; i < page.size(); ++i) {
    ls[i] = lum(mix(page[i], surface, alpha));
    lt[i] = lum(mix(page[i], text, alpha));
    ld[i] = lum(mix(page[i], dim, alpha));
  }
  Result r;
  bool dark_text = lum(text) < lum(surface);
  for (int y = 0; y + 13 <= H; y += 3)
    for (int x = 0; x + 9 <= W; x += 3) {
      double s_lo = 1, s_hi = 0, t_lo = 1, t_hi = 0, d_lo = 1, d_hi = 0;
      for (int j = 0; j < 13; ++j)
        for (int i = 0; i < 9; ++i) {
          size_t k = (y + j) * W + x + i;
          s_lo = std::min(s_lo, ls[k]), s_hi = std::max(s_hi, ls[k]);
          t_lo = std::min(t_lo, lt[k]), t_hi = std::max(t_hi, lt[k]);
          d_lo = std::min(d_lo, ld[k]), d_hi = std::max(d_hi, ld[k]);
        }
      // Dark text: its lightest pixel against the darkest surface around.
      r.text = std::min(r.text, dark_text ? contrast(s_lo, t_hi) : contrast(s_hi, t_lo));
      r.dim = std::min(r.dim, dark_text ? contrast(s_lo, d_hi) : contrast(s_hi, d_lo));
    }
  r.structure = structure(ls);
  return r;
}

struct Profile {
  const char *name, *backend, *effects;
};

std::string g_tmp;

}  // namespace

// A wallpaper for the tinted pass, written as PNG: a blue-green sky over the
// sea, or a strong red.
static std::string write_wallpaper(const std::string &path, bool red) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 160, 90);
  cairo_t *cr = cairo_create(s);
  if (red) {
    cairo_set_source_rgb(cr, 0.75, 0.13, 0.12);
  } else {
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, 90);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.56, 0.83, 0.89);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.09, 0.49, 0.60);
    cairo_set_source(cr, g);
    cairo_pattern_destroy(g);
  }
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_write_to_png(s, path.c_str());
  cairo_surface_destroy(s);
  return path;
}

// |wallpaper| empty: the theme's own colours (no tint); otherwise the
// floating layer takes that wallpaper's tint (0.2.0 Alpha 4), measured for
// one accent.
static void test_busy_pages(const std::string &label, const std::string &wallpaper) {
  Image pages[2] = {busy_page(false), busy_page(true)};
  std::map<std::pair<int, int>, Image> blurred;  // (dark, sigma x 10)
  auto backdrop = [&](bool dark, double sigma) -> const Image & {
    auto key = std::make_pair(int(dark), int(std::lround(sigma * 10)));
    auto it = blurred.find(key);
    if (it == blurred.end()) it = blurred.emplace(key, blur(pages[dark], sigma)).first;
    return it->second;
  };
  const Profile profiles[] = {{"OpenGL", "auto", "auto"}, {"XRender", "xrender", "full"}, {"lite", "auto", "lite"}};
  std::map<std::string, Result> summary;
  int shown = 0;
  for (const auto &fam : theme_families())
    for (bool dark : {false, true})
      for (const auto &accent : theme_accents())
        for (const auto &prof : profiles)
          for (int level : {25, 50, 80, 100}) {
            // Contrast falls and the page shows more with the slider: the
            // XRender and lite profiles are measured at their worst, 100.
            if (std::string(prof.name) != "OpenGL" && level != 100) continue;
            if (!wallpaper.empty() && accent != "blue") continue;
            Config cfg;
            cfg.set("appearance", "style", fam.id);
            cfg.set("appearance", "theme", dark ? "dark" : "light");
            cfg.set("appearance", "accent_name", accent);
            cfg.set("appearance", "scale", "100");
            cfg.set("appearance", "wallpaper_tint", wallpaper.empty() ? "false" : "true");
            if (!wallpaper.empty()) cfg.set("appearance", "wallpaper", wallpaper);
            cfg.set("aero", "transparency", std::to_string(level));
            cfg.set("compositor", "backend", prof.backend);
            cfg.set("compositor", "effects", prof.effects);
            Theme t = Theme::from_config(cfg);
            std::string conf, glsl;
            CHECK(read_file(write_compositor_config(cfg), conf));
            double sigma = blur_sigma(conf);
            const Image &page = backdrop(dark, sigma);
            char what[200];
            snprintf(what, sizeof what, "%s%s %s %s, %s, transparency %d", label.c_str(), fam.id.c_str(),
                     dark ? "dark" : "light", accent.c_str(), prof.name, level);
            if (!wallpaper.empty()) CHECK(t.wallpaper_tint == true);
            std::vector<std::pair<std::string, Result>> results;
            if (t.liquid) {
              CHECK(read_file(config_home() + "/sde/liquid-glass.glsl", glsl));
              results.push_back({"glass", liquid(page, glsl, t.popup_opacity, t)});
            } else {
              // What the panel paints (the floating layer's surface) and what
              // GTK and the previews call the panel.
              results.push_back({"panel", faded(page, of(t.layer_surface), of(t.text), of(t.text_dim), t.opacity)});
              results.push_back({"panel_bg", faded(page, of(t.panel_bg), of(t.text), of(t.text_dim), t.opacity)});
              results.push_back(
                  {"menu", faded(page, of(t.menu_bg), of(t.menu_text), of(t.text_dim), t.popup_opacity)});
            }
            results.push_back({"other menus", faded(page, of(t.menu_bg), of(t.menu_text), of(t.text_dim),
                                                    t.toolkit_menu_opacity)});
            results.push_back({"frame", faded(page, of(t.titlebar_bg), of(t.titlebar_text), of(t.titlebar_text),
                                              t.frame_opacity)});
            for (const auto &[name, r] : results) {
              // Calm: what the page leaves in a surface is soft colour (a
              // blur) or at most a faint trace (no blur, almost opaque).
              bool ok = r.text >= 4.5 && r.dim >= 3.0 && r.structure <= 3.0;
              if (!ok && ++shown <= 40)
                fprintf(stderr, "  %s, %s: text %.2f, dimmed %.2f, structure %.1f\n", what, name.c_str(), r.text,
                        r.dim, r.structure);
              if (!ok) ++g_failures;
              Result &s = summary[label + prof.name + " " + name];
              s.text = std::min(s.text, r.text);
              s.dim = std::min(s.dim, r.dim);
              s.structure = std::max(s.structure, r.structure);
            }
          }
  for (const auto &[what, s] : summary)
    printf("busy pages, %s: text at least %.2f:1, dimmed text %.2f:1, structure at most %.1f L*\n", what.c_str(),
           s.text, s.dim, s.structure);
}

// The slider is linear: Off, Subtle, Clear and Glass are even steps; without
// blur the surfaces stay almost opaque.
static void test_even_steps() {
  for (const auto &fam : theme_families())
    for (const char *mode : {"light", "dark"})
      for (const char *effects : {"auto", "lite"}) {
        std::vector<double> panel, popup, frame;
        for (int level : {0, 25, 50, 80, 100}) {
          Config cfg;
          cfg.set("appearance", "style", fam.id);
          cfg.set("appearance", "theme", mode);
          cfg.set("aero", "transparency", std::to_string(level));
          cfg.set("compositor", "effects", effects);
          Theme t = Theme::from_config(cfg);
          panel.push_back(t.opacity);
          popup.push_back(t.popup_opacity);
          frame.push_back(t.frame_opacity);
          CHECK(t.gtk_menu_alpha == 1.0);
          if (std::string(effects) == "lite")
            CHECK(t.opacity >= 0.97 && t.popup_opacity >= 0.97 && t.frame_opacity == 1.0);
        }
        for (const auto *v : {&panel, &popup, &frame}) {
          const auto &o = *v;
          double step = (o[0] - o[4]) / 100;  // per unit of the slider
          for (int i = 1; i < 5; ++i) CHECK(o[i] <= o[i - 1] + 1e-12);
          CHECK(std::fabs((o[0] - o[1]) - 25 * step) < 1e-9);
          CHECK(std::fabs((o[1] - o[2]) - 25 * step) < 1e-9);
          CHECK(std::fabs((o[2] - o[3]) - 30 * step) < 1e-9);
        }
      }
}

// The shader SDE writes: dithered against banding, the frost and the
// materials of palettes.json.
static void test_shader_text() {
  Config cfg;
  cfg.set("appearance", "style", "glass");
  cfg.set("aero", "transparency", "80");
  cfg.set("compositor", "backend", "glx");
  cfg.set("compositor", "effects", "full");
  Theme t = Theme::from_config(cfg);
  CHECK(t.liquid);
  std::string conf, glsl;
  CHECK(read_file(write_compositor_config(cfg), conf));
  CHECK(read_file(config_home() + "/sde/liquid-glass.glsl", glsl));
  CHECK(glsl.find("dither(gl_FragCoord.xy)") != std::string::npos);
  CHECK(glsl.find("frosted(") != std::string::npos && constant(glsl, "FROST") > 4);
  // GTK 3 menus are glass too; popups with their own shadow get no second one.
  CHECK(conf.find("window-shader-fg-rule") != std::string::npos);
  CHECK(conf.find("(window_type = 'popup_menu' || window_type = 'dropdown_menu') && !argb && !_SDE_SURFACE") !=
        std::string::npos);
  CHECK(conf.find("\"override_redirect && argb && !_SDE_SURFACE\"];\nfading") != std::string::npos ||
        conf.find("\"override_redirect && argb && !_SDE_SURFACE\"];") != std::string::npos);
  CHECK(conf.find("popup_menu = { opacity") == std::string::npos);
  CHECK(blur_sigma(conf) >= 15);
}

int main() {
  char tmpl[] = "/tmp/sde-glass-test-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  // A computer with a GPU and accelerated OpenGL (OpenGL unless a profile
  // says otherwise, also inside an SDE session on software OpenGL), and a
  // configuration directory of the test's own for the generated files.
  setenv("SDE_GRAPHICS", "hardware", 1);
  setenv("SDE_OPENGL", "accelerated:test GPU", 1);
  setenv("XDG_CONFIG_HOME", (g_tmp + "/config").c_str(), 1);
  setenv("XDG_CACHE_HOME", (g_tmp + "/cache").c_str(), 1);  // the wallpaper colours' cache
  make_dirs(g_tmp + "/config/sde");
  test_even_steps();
  test_shader_text();
  test_busy_pages("", "");
  // 0.2.0 Alpha 4: with the wallpaper's tint on the floating layer.
  test_busy_pages("tinted (sky) ", write_wallpaper(g_tmp + "/sky.png", false));
  test_busy_pages("tinted (red) ", write_wallpaper(g_tmp + "/red.png", true));
  std::string cmd = "rm -rf '" + g_tmp + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("glass tests passed\n");
  return 0;
}
