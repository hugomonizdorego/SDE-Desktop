// SPDX-License-Identifier: GPL-3.0-or-later
// Theme tests of 0.1.0~alpha50: right-hand caption buttons and the migration
// of saved SDE WM layouts, the transparency model (contrast at the highest
// transparency of every family, mode and accent), icon theme lookup with
// inheritance (tests/fixtures/icon-themes) and the build history parser.
// Alpha51: calm palettes (low-chroma neutrals, muted accents, readable
// text), soft corner radius tokens that follow the UI scale, Mint-Y like
// caption colors and the SDE WM caption masks and hit area.
// 0.2.0 Alpha 2: per-family radii (Fluent after Windows 11, Liquid Glass the
// softest), Liquid Glass (glass shader of the OpenGL compositor) and the
// antialiased SDE WM caption pictures.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <cairo.h>

#include "build_history.h"
#include "config.h"
#include "icons.h"
#include "integration.h"
#include "theme.h"
#include "util.h"
#include "wallpaper_colors.h"
#include "wm.h"

using namespace sde;

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

static std::string g_tmp;

static void test_caption_layout() {
  for (const auto &fam : theme_families()) {
    CHECK(theme_title_layout(fam.id) == "NLIMC");
    CHECK(theme_decoration_layout(fam.id) == ":minimize,maximize,close");
  }
  // Saved SDE WM configurations: the old left layout of Graphite and Glass
  // moves to the right; a layout chosen by hand stays.
  setenv("XDG_CONFIG_HOME", (g_tmp + "/config").c_str(), 1);
  make_dirs(g_tmp + "/config/sde/openbox");
  auto rc = [](const std::string &name, const std::string &layout) {
    return "<?xml version=\"1.0\"?>\n<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
           "  <theme>\n    <name>" + name + "</name>\n    <titleLayout>" + layout +
           "</titleLayout>\n  </theme>\n  <mouse>\n  </mouse>\n</openbox_config>\n";
  };
  std::string path = openbox_config_path(), text;
  CHECK(path == g_tmp + "/config/sde/openbox/rc.xml");
  write_file_atomic(path, rc("SDE-Graphite-Light-Blue", "CIML"));
  CHECK(set_openbox_theme(std::string("SDE-Glass-Dark-Green"), theme_title_layout("glass")));
  CHECK(read_file(path, text));
  CHECK(text.find("<titleLayout>NLIMC</titleLayout>") != std::string::npos);
  CHECK(text.find("<name>SDE-Glass-Dark-Green</name>") != std::string::npos);
  CHECK(!set_openbox_theme(std::string("SDE-Glass-Dark-Green"), theme_title_layout("glass")));  // nothing left to do
  for (const char *old : {"CIMLN", "NLIMC"}) {
    write_file_atomic(path, rc("SDE-Aero", old));
    set_openbox_theme(std::string("SDE-Fluent-Light-Blue"), theme_title_layout("fluent"));
    CHECK(read_file(path, text) && text.find("<titleLayout>NLIMC</titleLayout>") != std::string::npos);
  }
  write_file_atomic(path, rc("Clearlooks", "LIMC"));
  set_openbox_theme(std::string("SDE-Graphite-Light-Blue"), theme_title_layout("graphite"));
  CHECK(read_file(path, text));
  CHECK(text.find("<titleLayout>LIMC</titleLayout>") != std::string::npos);
  CHECK(text.find("<name>Clearlooks</name>") != std::string::npos);
}

static Theme theme_for(const std::string &family, const char *mode, const std::string &accent, int transparency,
                       const char *effects = "full") {
  Config cfg;
  cfg.set("appearance", "style", family);
  cfg.set("appearance", "theme", mode);
  cfg.set("appearance", "accent_name", accent);
  cfg.set("appearance", "scale", "100");
  cfg.set("aero", "enabled", "true");
  cfg.set("aero", "transparency", std::to_string(transparency));
  cfg.set("compositor", "effects", effects);
  return Theme::from_config(cfg);
}

// WCAG 2.1 at the highest transparency: text 4.5:1 and secondary text 3:1
// over any backdrop, for every family, mode and accent.
static void test_transparency_contrast() {
  for (const auto &fam : theme_families())
    for (const char *mode : {"light", "dark"})
      for (const auto &accent : theme_accents()) {
        Theme t = theme_for(fam.id, mode, accent, 100);
        std::string what = fam.id + " " + mode + " " + accent;
        bool ok = true;
        auto need = [&](const char *name, double got, double want) {
          if (got + 1e-9 < want) {
            fprintf(stderr, "  %s: %s %.2f < %.1f\n", what.c_str(), name, got, want);
            ok = false;
          }
        };
        // Liquid Glass (auto picks OpenGL on a real GPU) keeps text readable
        // in the shader instead (test_liquid_glass()).
        if (!t.liquid) {
          need("panel text", translucent_contrast(t.text, t.panel_bg, t.opacity, true), 4.5);
          need("panel dim", translucent_contrast(t.text_dim, t.panel_bg, t.opacity, true), 3.0);
          need("menu text", translucent_contrast(t.menu_text, t.menu_bg, t.popup_opacity, true), 4.5);
          need("menu dim", translucent_contrast(t.text_dim, t.menu_bg, t.popup_opacity, true), 3.0);
          need("menu hover", translucent_contrast(t.menu_hover_text, t.menu_hover, t.popup_opacity, true), 3.0);
        }
        need("toolkit menu", translucent_contrast(t.menu_text, t.menu_bg, t.toolkit_menu_opacity, true), 4.5);
        need("frame title", translucent_contrast(t.titlebar_text, t.titlebar_bg, t.frame_opacity, true), 4.5);
        need("gtk menu", translucent_contrast(t.menu_text, t.menu_bg, t.gtk_menu_alpha, false), 4.5);
        need("gtk menu dim", translucent_contrast(t.text_dim, t.menu_bg, t.gtk_menu_alpha, false), 3.0);
        CHECK(ok);
        // 100 really is translucent: every surface lets something through.
        // GTK menus are opaque in CSS since 0.2.0 Alpha 3.1 (the compositor
        // makes GTK 3 menus glass or translucent).
        CHECK(t.opacity < 0.9 && t.popup_opacity < 0.95 && t.gtk_menu_alpha == 1.0);
        CHECK(t.blur && !t.lite);
        // Lite profile (VMs, software rendering): no blur, more opaque.
        Theme lite = theme_for(fam.id, mode, accent, 100, "lite");
        CHECK(lite.lite && !lite.blur);
        CHECK(lite.opacity > t.opacity && lite.popup_opacity > t.popup_opacity);
        CHECK(lite.frame_opacity == 1.0);
        // Off is opaque everywhere.
        Theme off = theme_for(fam.id, mode, accent, 0);
        CHECK(off.opacity == 1.0 && off.popup_opacity == 1.0 && off.frame_opacity == 1.0 &&
              off.gtk_menu_alpha == 1.0);
        // Opacity falls steadily with the slider.
        Theme mid = theme_for(fam.id, mode, accent, 50);
        CHECK(mid.opacity < 1.0 && mid.opacity > t.opacity);
      }
  // [aero] enabled=false and low resource mode turn every surface opaque.
  Config cfg;
  cfg.set("aero", "transparency", "80");
  cfg.set("aero", "enabled", "false");
  CHECK(Theme::from_config(cfg).opacity == 1.0);
  cfg.set("aero", "enabled", "true");
  cfg.set("appearance", "low_resource", "true");
  CHECK(Theme::from_config(cfg).opacity == 1.0);
  // Presets.
  CHECK(transparency_presets().size() == 4);
  CHECK(transparency_preset_id(0) == "off" && transparency_preset_id(25) == "subtle");
  CHECK(transparency_preset_id(50) == "clear" && transparency_preset_id(80) == "glass");
  CHECK(transparency_preset_id(33).empty());
  // The readable limit itself: black on white needs no help, gray on gray
  // cannot go far.
  CHECK(readable_opacity(Color::hex(0x000000), Color::hex(0xFFFFFF), 4.5, false) < 0.75);
  CHECK(readable_opacity(Color::hex(0x777777), Color::hex(0xFFFFFF), 4.5, false) == 1.0);
}

static void test_icon_lookup() {
  std::string fixtures = std::string(SDE_TEST_SOURCE_DIR) + "/tests/fixtures/icon-themes";
  setenv("XDG_DATA_DIRS", fixtures.c_str(), 1);
  setenv("XDG_DATA_HOME", (g_tmp + "/data").c_str(), 1);
  setenv("HOME", g_tmp.c_str(), 1);
  auto themes = IconLoader::installed_themes();
  auto has = [&](const char *n) { return std::find(themes.begin(), themes.end(), n) != themes.end(); };
  CHECK(has("FixtureChild") && has("FixtureParent") && has("Papirus") && has("Papirus-Dark"));
  CHECK(!has("hicolor") && !has("CursorOnly") && !has("EmblemsOnly"));
  // Auto: Papirus, paired with the mode; unknown themes fall back to auto.
  CHECK(resolve_icon_theme("auto", false) == "Papirus");
  CHECK(resolve_icon_theme("auto", true) == "Papirus-Dark");
  CHECK(resolve_icon_theme("", true) == "Papirus-Dark");
  CHECK(resolve_icon_theme("NoSuchTheme", false) == "Papirus");
  CHECK(resolve_icon_theme("FixtureChild", true) == "FixtureChild");

  IconLoader::set_theme("FixtureChild", false);
  CHECK(IconLoader::theme() == "FixtureChild");
  auto chain = IconLoader::theme_chain();
  // Depth first: the parent's own parent comes before the second parent;
  // then hicolor, and Papirus for what none of them has (0.2 Alpha 2).
  std::vector<std::string> want = {"FixtureChild", "FixtureParent", "FixtureGrand", "FixtureOther", "hicolor",
                                   "Papirus"};
  CHECK(chain == want);
  auto in = [&](const std::string &path, const std::string &theme) {
    return path.find("/icons/" + theme + "/") != std::string::npos;
  };
  CHECK(in(IconLoader::lookup("child-app", 48), "FixtureChild"));
  CHECK(in(IconLoader::lookup("child-app", 16), "FixtureChild"));  // the theme's own icon wins
  CHECK(in(IconLoader::lookup("parent-app", 48), "FixtureParent"));
  CHECK(in(IconLoader::lookup("order-test", 48), "FixtureGrand"));
  CHECK(in(IconLoader::lookup("other-only", 48), "FixtureOther"));
  CHECK(in(IconLoader::lookup("hicolor-only", 48), "hicolor"));
  CHECK(in(IconLoader::lookup("hicolor-only.png", 48), "hicolor"));  // Icon=name.png in desktop files
  CHECK(IconLoader::lookup("no-such-icon", 48).empty());
  // Scale=2 directories hold icons of twice their size.
  CHECK(in(IconLoader::lookup("scaled-folder", 48), "FixtureChild"));
  // Switching themes refreshes the lookup (live change from Settings).
  IconLoader::set_theme("auto", true);
  CHECK(IconLoader::theme() == "Papirus-Dark");
  CHECK(in(IconLoader::lookup("folder", 48), "Papirus-Dark"));
  CHECK(in(IconLoader::lookup("parent-app", 48), "FixtureParent"));  // Papirus-Dark > Papirus > FixtureParent
  IconLoader::set_theme("auto", false);
  CHECK(in(IconLoader::lookup("folder", 48), "Papirus") && !in(IconLoader::lookup("folder", 48), "Papirus-Dark"));
  // Previews of another theme leave the current one alone.
  CHECK(in(IconLoader::lookup_in("Papirus-Dark", "folder", 48), "Papirus-Dark"));
  CHECK(IconLoader::theme() == "Papirus");
}

// HSV saturation and chroma of a color (0-1).
static double chroma(const Color &c) {
  return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
}
static double saturation(const Color &c) {
  double mx = std::max({c.r, c.g, c.b});
  return mx > 0 ? chroma(c) / mx : 0;
}

static Theme theme_at(const std::string &family, const char *mode, const std::string &accent, int scale) {
  Config cfg;
  cfg.set("appearance", "style", family);
  cfg.set("appearance", "theme", mode);
  cfg.set("appearance", "accent_name", accent);
  cfg.set("appearance", "scale", std::to_string(scale));
  return Theme::from_config(cfg);
}

// Alpha51: calm, natural palettes with soft corners in every family.
static void test_soft_palettes() {
  for (const auto &fam : theme_families())
    for (const char *mode : {"light", "dark"})
      for (const auto &accent : theme_accents()) {
        Theme t = theme_at(fam.id, mode, accent, 100);
        std::string what = fam.id + " " + mode + " " + accent;
        bool ok = true;
        auto need = [&](const char *name, bool cond) {
          if (!cond) fprintf(stderr, "  %s: %s\n", what.c_str(), name);
          ok = ok && cond;
        };
        // Neutral surfaces: grays with at most a faint warm or cool tint.
        for (const Color *c : {&t.background, &t.surface, &t.surface_alt, &t.hover, &t.border, &t.menu_bg,
                               &t.panel_bg, &t.titlebar_bg, &t.text, &t.text_dim})
          need("neutral surface", chroma(*c) <= 0.05);
        // Accents: muted, no neon (a gray accent is fine).
        need("muted accent", chroma(t.accent) <= 0.55 && saturation(t.accent) <= 0.72);
        // Readable text everywhere (WCAG AA), including dimmed text on sidebars.
        need("text", t.text.contrast(t.background) >= 7 && t.text.contrast(t.surface) >= 7);
        need("dim text", t.text_dim.contrast(t.surface_alt) >= 4.5 && t.text_dim.contrast(t.menu_bg) >= 4.5 &&
                             t.text_dim.contrast(t.panel_bg) >= 4.5);
        need("title", t.titlebar_text.contrast(t.titlebar_bg) >= 7);
        need("menu highlight", t.menu_hover_text.contrast(t.menu_hover) >= 4.5);
        need("status", t.error.contrast(t.surface) >= 3 && t.success.contrast(t.surface) >= 3 &&
                           t.warning.contrast(t.surface) >= 3);
        // Soft corners: small for controls, medium for windows and menus,
        // large for big surfaces; never square. 0.2.0 Alpha 2: Fluent after
        // Windows 11 (4 px controls, 8 px windows and flyouts), Liquid Glass
        // the softest (pill-like controls, 24 px popups).
        need("radii", t.radius_small >= 4 && t.radius_small < t.radius && t.radius >= 8 && t.radius <= 16 &&
                          t.radius_large >= 8 && t.radius_large <= 24 && t.radius <= t.radius_large);
        if (fam.id == "graphite") need("graphite radii", t.radius_small == 6 && t.radius == 10 && t.radius_large == 14);
        if (fam.id == "fluent") need("fluent radii", t.radius_small == 4 && t.radius == 8 && t.radius_large == 8);
        if (fam.id == "glass") need("glass radii", t.radius_small == 10 && t.radius == 16 && t.radius_large == 24);
        need("radius_for", t.radius_for(8) == 4 && t.radius_for(40) == t.radius_small);
        // Caption buttons: a clear glyph, a soft but visible disc, a close
        // red that keeps its white cross readable; no traffic lights.
        need("caption glyph", t.caption_glyph.contrast(t.titlebar_bg) >= 4.5);
        double disc = t.caption_disc.contrast(t.titlebar_bg);
        need("caption disc", disc >= 1.6 && disc <= 3.6);
        need("caption close", t.caption_close.contrast(Color::hex(0xFFFFFF)) >= 3.0 &&
                                  chroma(t.caption_close) <= 0.55);
        need("caption press", t.caption_press.contrast(t.titlebar_bg) > disc);
        CHECK(ok);
        // Radii follow the interface scale.
        Theme big = theme_at(fam.id, mode, accent, 150);
        CHECK(big.radius == int(std::lround(t.radius * 1.5)) &&
              big.radius_small == int(std::lround(t.radius_small * 1.5)) &&
              big.radius_large == int(std::lround(t.radius_large * 1.5)));
        set_ui_scale(1.0);
      }
  // Accents of earlier palettes (alpha16 to alpha50) move to the new ones.
  Config cfg;
  cfg.set("appearance", "style", "graphite");
  cfg.set("appearance", "accent", "#3684DD");
  cfg.set("appearance", "accent_name", "blue");
  CHECK(migrate_theme_config(cfg));
  CHECK(cfg.get("appearance", "accent", "") == theme_accent_color("graphite", "blue", false).to_hex());
  ThemeChoice c = map_theme_choice("fluent", "dark", "#2777FF", "");
  CHECK(c.accent == "blue" && c.custom_accent.empty());
}

// Liquid Glass (0.2.0 Alpha 2): on the OpenGL compositor profile the glass
// shader draws SDE's surfaces; it tints the blurred backdrop with the surface
// color by the surface's opacity, keeps text and icons opaque and lifts (or
// deepens) the glass to liquid_legible_luminance() wherever the backdrop
// would make text hard to read, so text keeps WCAG 2.1 contrast over any
// backdrop while the glass stays clear.
static Color gray_of_luminance(double l) {
  double v = l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
  Color c;
  c.r = c.g = c.b = std::max(0.0, std::min(1.0, v));
  return c;
}

static void test_liquid_glass() {
  CHECK(theme_liquid_material("glass", false).on && theme_liquid_material("glass", true).on);
  CHECK(!theme_liquid_material("graphite", false).on && !theme_liquid_material("fluent", true).on);
  for (const char *mode : {"light", "dark"}) {
    LiquidMaterial m = theme_liquid_material("glass", std::string(mode) == "dark");
    CHECK(m.blur >= 1 && m.blur <= 12 && m.saturation >= 1 && m.saturation <= 2);
    CHECK(m.brightness > 0.5 && m.brightness < 1.5 && m.rim > 0 && m.rim <= 1 && m.rim_width > 0);
    CHECK(m.lens_band > m.lens_shift && m.lens_shift > 0 && m.key_range > 0 && m.key_range < 1);
    // 0.2.0 Alpha 3.1: a frosted glass (tests/test_glass.cpp measures it).
    CHECK(m.frost > 0 && m.tint >= 0.25 && m.tint <= 0.6);
    for (const auto &accent : theme_accents()) {
      double previous = 1.0;
      for (int transparency : {25, 50, 100}) {
        Config cfg;
        cfg.set("appearance", "style", "glass");
        cfg.set("appearance", "theme", mode);
        cfg.set("appearance", "accent_name", accent);
        cfg.set("appearance", "scale", "100");
        cfg.set("aero", "enabled", "true");
        cfg.set("aero", "transparency", std::to_string(transparency));
        cfg.set("compositor", "effects", "full");
        cfg.set("compositor", "backend", "glx");
        Theme t = Theme::from_config(cfg);
        CHECK(t.liquid);
        // Glass at the legible luminance: text 4.5:1, dimmed text 3:1; the
        // surface color itself is on the right side of it (the shader can
        // always reach it).
        double l = liquid_legible_luminance(t);
        Color glass = gray_of_luminance(l);
        CHECK(t.text.contrast(glass) >= 4.5 && t.menu_text.contrast(glass) >= 4.5);
        CHECK(t.text_dim.contrast(glass) >= 3.0);
        // Accent text (headings, links) where the accent itself is readable
        // on the surface (not yellow and orange in a light theme).
        if (t.accent.contrast(t.surface) >= 4.5) CHECK(t.accent.contrast(glass) >= 2.6);
        CHECK(t.dark ? t.surface.luminance() <= l && t.menu_bg.luminance() <= l
                     : t.surface.luminance() >= l && t.menu_bg.luminance() >= l);
        // A frosted glass, clearer with the slider in even steps (from the
        // surface color at 0 to the material's tint at 100); one tint for
        // every surface.
        CHECK(t.opacity == t.popup_opacity && t.opacity < previous);
        CHECK(std::fabs(t.opacity - (1.0 - transparency / 100.0 * (1.0 - m.tint))) < 1e-9);
        previous = t.opacity;
        // Other toolkits' menus still fade as a whole and stay readable.
        CHECK(translucent_contrast(t.menu_text, t.menu_bg, t.toolkit_menu_opacity, true) >= 4.5);
        CHECK(translucent_contrast(t.text_dim, t.menu_bg, t.toolkit_menu_opacity, true) >= 3.0);
        // The drawn glass elsewhere: XRender, the lite profile, no blur,
        // opaque surfaces and the other families.
        cfg.set("compositor", "backend", "xrender");
        CHECK(!Theme::from_config(cfg).liquid);
        cfg.set("compositor", "backend", "glx");
        cfg.set("compositor", "effects", "lite");
        CHECK(!Theme::from_config(cfg).liquid);
        cfg.set("compositor", "effects", "full");
        cfg.set("aero", "blur", "false");
        CHECK(!Theme::from_config(cfg).liquid);
        cfg.set("aero", "blur", "true");
        cfg.set("aero", "transparency", "0");
        CHECK(!Theme::from_config(cfg).liquid);
        cfg.set("aero", "transparency", "50");
        cfg.set("appearance", "style", "graphite");
        CHECK(!Theme::from_config(cfg).liquid);
      }
    }
  }
  set_ui_scale(1.0);
}

// The SDE WM themes built next to the tests (build/themes): Mint-Y like
// caption buttons with a hit area of at least 28 px at 100 %.
static void test_caption_frames() {
  std::string dir = std::string(SDE_TEST_BINARY_DIR) + "/themes/SDE-Graphite-Light-Blue/openbox-3/";
  std::string rc, xbm;
  if (!read_file(dir + "themerc", rc)) {
    fprintf(stderr, "  note: %s not built, skipping the frame checks\n", dir.c_str());
    return;
  }
  auto value = [&](const std::string &key) {
    size_t at = rc.find("\n" + key + ": ");
    return at == std::string::npos ? std::string() : rc.substr(at + key.size() + 3, rc.find('\n', at + 1) - at - key.size() - 3);
  };
  int width = atoi(value("window.button.width.percent").c_str());
  int height = atoi(value("window.button.height.percent").c_str());
  int pad = atoi(value("padding.height").c_str());
  // The title is at least a 15 px label (10 pt bold) plus the padding.
  int title = 15 + 2 * pad;
  CHECK(title * width / 100 >= 28 && title * height / 100 >= 28);
  CHECK(value("window.active.button.hover.bg") == "parentrelative");  // no square hover layer
  CHECK(value("window.active.button.close.hover.bg") == "parentrelative");
  CHECK(!value("window.active.button.close.unpressed.image.color").empty());
  CHECK(rc.find("gradient") == std::string::npos);
  for (const char *mask : {"close", "close_hover", "iconify", "iconify_hover", "max", "max_hover", "max_toggled",
                           "max_hover_toggled"}) {
    CHECK(read_file(dir + mask + ".xbm", xbm));
    CHECK(xbm.find("_width 14") != std::string::npos && xbm.find("_height 14") != std::string::npos);
  }
  // 0.2.0 Alpha 2 (SDE WM patch 0004): antialiased 8-bit pictures next to the
  // masks, and paddings that follow the title font.
  std::string png;
  for (const char *picture : {"close", "close_hover", "close_pressed", "iconify", "iconify_hover", "max",
                              "max_hover", "max_toggled", "max_hover_toggled", "max_pressed_toggled"}) {
    CHECK(read_file(dir + picture + ".png", png));
    CHECK(png.size() > 24 && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
  }
  CHECK(atoi(value("padding.width.percent").c_str()) > 0 && atoi(value("padding.height.percent").c_str()) > 0);
}

static void test_build_history() {
  BuildHistory h;
  std::string json =
      "{\"format\": 1, \"version\": \"0.1.0~alpha50\", \"build\": {\"commit\": \"abc1234\", \"number\": \"212\","
      " \"date\": \"2026-10-22\"}, \"entries\": [{\"title\": \"Alpha50 patch\", \"date\": \"2026-10-22\","
      " \"summary\": \"caption buttons \\u2014 icons\", \"items\": [\"Right-hand \\\"buttons\\\"\", \"Icons\\n\"]},"
      " {\"title\": \"Alpha16 patch\", \"date\": \"\", \"summary\": \"\", \"items\": []}, 42]}";
  CHECK(parse_build_history(json, h));
  CHECK(h.version == "0.1.0~alpha50" && h.commit == "abc1234" && h.number == "212" && h.date == "2026-10-22");
  CHECK(h.entries.size() == 2);
  if (h.entries.size() == 2) {
    CHECK(h.entries[0].summary == "caption buttons \xE2\x80\x94 icons");
    CHECK(h.entries[0].items.size() == 2 && h.entries[0].items[0] == "Right-hand \"buttons\"");
    CHECK(h.entries[1].title == "Alpha16 patch" && h.entries[1].items.empty());
  }
  CHECK(!parse_build_history("{\"entries\": [", h));
  CHECK(!parse_build_history("[]", h));
  CHECK(!parse_build_history("{\"version\": \"x\"}", h));
  // The file the build generated from CHANGELOG.md.
  std::string text;
  if (read_file(std::string(SDE_TEST_BINARY_DIR) + "/build-history.json", text)) {
    CHECK(parse_build_history(text, h));
    CHECK(!h.entries.empty() && !h.entries[0].title.empty() && !h.date.empty());
  }
}

// ------------------------------------- wallpaper colours (0.2.0 Alpha 4)
//
// Sample wallpapers drawn here (a sky, a forest, a sunset over the sea, a
// red, a rainbow, grays, black and white) and SDE's own Selviana pictures:
// the colour extraction (Oklab, the dominant hue, how strongly it may tint),
// the cache, the calm tint (same luminance, a tinted gray) and the floating
// layer of every family, mode and accent with it (WCAG AA kept).

namespace {

using Paint = void (*)(cairo_t *cr, double w, double h);

void stops(cairo_t *cr, double h, std::initializer_list<std::pair<double, unsigned>> list) {
  cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, h);
  for (const auto &s : list)
    cairo_pattern_add_color_stop_rgb(g, s.first, ((s.second >> 16) & 0xff) / 255.0, ((s.second >> 8) & 0xff) / 255.0,
                                     (s.second & 0xff) / 255.0);
  cairo_set_source(cr, g);
  cairo_paint(cr);
  cairo_pattern_destroy(g);
}

struct Sample {
  const char *name;
  Paint paint;
};

const Sample kSamples[] = {
    {"sky", [](cairo_t *cr, double, double h) { stops(cr, h, {{0, 0x8FD3E3}, {0.55, 0x3FA7C2}, {1, 0x167D9A}}); }},
    {"forest",
     [](cairo_t *cr, double, double h) {
       stops(cr, h, {{0, 0xA7C7D9}, {0.25, 0x7FA35B}, {0.6, 0x3F6B3A}, {1, 0x1E3B2A}});
     }},
    {"sunset",
     [](cairo_t *cr, double, double h) {
       stops(cr, h, {{0, 0x2B1B4A}, {0.35, 0xB4486B}, {0.55, 0xE86A3A}, {0.62, 0xF7C873}, {0.63, 0x1D3557},
                     {1, 0x0F1C30}});
     }},
    {"red", [](cairo_t *cr, double, double) { cairo_set_source_rgb(cr, 0.75, 0.22, 0.17), cairo_paint(cr); }},
    {"rainbow",
     [](cairo_t *cr, double w, double h) {
       for (int i = 0; i < 12; ++i) {
         double hue = i / 12.0 * 6, x = hue - std::floor(hue / 2) * 2 - 1, c = 0.8, v = c * (1 - std::fabs(x));
         double rgb[6][3] = {{c, v, 0}, {v, c, 0}, {0, c, v}, {0, v, c}, {v, 0, c}, {c, 0, v}};
         auto &k = rgb[int(hue) % 6];
         cairo_set_source_rgb(cr, k[0] + 0.1, k[1] + 0.1, k[2] + 0.1);
         cairo_rectangle(cr, w * i / 12, 0, w / 12 + 1, h);
         cairo_fill(cr);
       }
     }},
    {"gray",
     [](cairo_t *cr, double w, double h) {
       for (int y = 0; y < h; y += 4)
         for (int x = 0; x < w; x += 4) {
           double v = 0.42 + 0.16 * (((x * 7 + y * 13) % 17) / 17.0);
           cairo_set_source_rgb(cr, v, v, v);
           cairo_rectangle(cr, x, y, 4, 4);
           cairo_fill(cr);
         }
     }},
    {"black", [](cairo_t *cr, double, double) { cairo_set_source_rgb(cr, 0, 0, 0), cairo_paint(cr); }},
    {"white", [](cairo_t *cr, double, double) { cairo_set_source_rgb(cr, 1, 1, 1), cairo_paint(cr); }},
};

cairo_surface_t *sample_surface(const Sample &s) {
  cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 192, 108);
  cairo_t *cr = cairo_create(img);
  s.paint(cr, 192, 108);
  cairo_destroy(cr);
  cairo_surface_flush(img);
  return img;
}

// The colours of a sample measured from its pixels (as RGBA bytes).
WallpaperColors sample_colors(const Sample &s) {
  cairo_surface_t *img = sample_surface(s);
  int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
  int stride = cairo_image_surface_get_stride(img);
  const unsigned char *d = cairo_image_surface_get_data(img);
  std::vector<unsigned char> rgba(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint32_t v = reinterpret_cast<const uint32_t *>(d + y * stride)[x];
      unsigned char *o = &rgba[(size_t(y) * w + x) * 4];
      o[0] = (v >> 16) & 0xff, o[1] = (v >> 8) & 0xff, o[2] = v & 0xff, o[3] = 255;
    }
  cairo_surface_destroy(img);
  return wallpaper_colors_of_pixels(rgba.data(), w, h, w * 4, 4);
}

std::string write_sample(const Sample &s, const std::string &path) {
  cairo_surface_t *img = sample_surface(s);
  cairo_surface_write_to_png(img, path.c_str());
  cairo_surface_destroy(img);
  return path;
}

double degrees(double radians) {
  double d = std::fmod(radians * 180 / M_PI + 360, 360);
  return d;
}

// The distance between two hues in degrees.
double hue_gap(double a, double b) { return std::fabs(std::remainder(a - b, 2 * M_PI)) * 180 / M_PI; }

Config tinted_config(const std::string &family, const char *mode, const std::string &accent,
                     const std::string &wallpaper, const char *tint, int transparency = 25) {
  Config cfg;
  cfg.set("appearance", "style", family);
  cfg.set("appearance", "theme", mode);
  cfg.set("appearance", "accent_name", accent);
  cfg.set("appearance", "scale", "100");
  cfg.set("appearance", "wallpaper", wallpaper);
  if (tint) cfg.set("appearance", "wallpaper_tint", tint);
  cfg.set("aero", "enabled", "true");
  cfg.set("aero", "transparency", std::to_string(transparency));
  cfg.set("compositor", "effects", "full");
  return cfg;
}

}  // namespace

static void test_wallpaper_colors() {
  setenv("XDG_CACHE_HOME", (g_tmp + "/cache").c_str(), 1);
  // Oklab there and back.
  for (unsigned hex : {0x000000u, 0xFFFFFFu, 0x3672B9u, 0xE8975Fu, 0x2B2D31u, 0xF8F9FBu}) {
    Color c = Color::hex(hex);
    bool ok = false;
    Color back = from_oklab(to_oklab(c), &ok);
    CHECK(ok && std::fabs(back.r - c.r) < 1e-6 && std::fabs(back.g - c.g) < 1e-6 && std::fabs(back.b - c.b) < 1e-6);
  }
  CHECK(std::fabs(to_oklab(Color::hex(0xFFFFFF)).L - 1.0) < 1e-4 && std::fabs(to_oklab(Color::hex(0x777777)).a) < 1e-4);

  std::map<std::string, WallpaperColors> m;
  for (const auto &s : kSamples) m[s.name] = sample_colors(s);
  for (const auto &kv : m) {
    printf("wallpaper %-8s chroma %.3f hue %5.1f share %.2f strength %.2f dominant %s accent %s\n",
           kv.first.c_str(), kv.second.chroma, degrees(kv.second.hue), kv.second.share, kv.second.strength,
           kv.second.dominant.to_hex().c_str(), wallpaper_accent(kv.second).c_str());
    CHECK(kv.second.known);
  }
  // A clear colour tints strongly, in its own hue: the sky blue-cyan, the
  // forest green, the red red.
  CHECK(m["sky"].strength > 0.5 && hue_gap(m["sky"].hue, 225 * M_PI / 180) < 30);
  CHECK(m["forest"].strength > 0.5 && hue_gap(m["forest"].hue, 135 * M_PI / 180) < 25);
  CHECK(m["red"].strength > 0.9 && hue_gap(m["red"].hue, 32 * M_PI / 180) < 15);
  // Many hues at once tint less, gray, black and white not at all.
  CHECK(m["sunset"].strength < m["sky"].strength * 0.7 && m["rainbow"].strength < 0.6);
  for (const char *none : {"gray", "black", "white"}) CHECK(m[none].strength == 0 && m[none].chroma < 0.01);
  CHECK(wallpaper_accent(m["forest"]) == "green" && wallpaper_accent(m["red"]) == "red");
  CHECK(wallpaper_accent(m["sky"]) == "teal" || wallpaper_accent(m["sky"]) == "blue");
  for (const char *none : {"gray", "black", "white"}) CHECK(wallpaper_accent(m[none]) == "graphite");
  CHECK(!wallpaper_colors_of_pixels(nullptr, 10, 10, 40, 4).known);
  CHECK(!wallpaper_colors(g_tmp + "/no-such-picture.png").known);

  // From a file, cached by path, size and modification time.
  std::string pic = write_sample(kSamples[0], g_tmp + "/wallpaper.png");
  WallpaperColors f = wallpaper_colors(pic);
  CHECK(f.known && hue_gap(f.hue, m["sky"].hue) < 3 && std::fabs(f.strength - m["sky"].strength) < 0.05);
  std::string cache;
  CHECK(read_file(g_tmp + "/cache/sde/wallpaper-colors", cache) && cache.find(pic) != std::string::npos);
  CHECK(std::fabs(wallpaper_colors(pic).hue - f.hue) < 1e-9);
  write_sample(kSamples[3], pic);  // another picture under the same name
  CHECK(hue_gap(wallpaper_colors(pic).hue, m["red"].hue) < 3);
  // SDE's own wallpapers (SVG): calm blue-green, light and dark.
  for (const char *name : {"selviana-light.svg", "selviana-dark.svg"}) {
    WallpaperColors s = wallpaper_colors(std::string(SDE_TEST_SOURCE_DIR) + "/data/backgrounds/" + name);
    printf("wallpaper %s: hue %.1f strength %.2f accent %s\n", name, degrees(s.hue), s.strength,
           wallpaper_accent(s).c_str());
    CHECK(s.known && s.strength > 0.3 && hue_gap(s.hue, 225 * M_PI / 180) < 35);
  }
  // "auto" is the Selviana wallpaper of the mode (when it is installed).
  Config cfg;
  std::string light = wallpaper_in_use(cfg, false), dark = wallpaper_in_use(cfg, true);
  CHECK(light.empty() || ends_with(light, "selviana-light.svg"));
  CHECK(dark.empty() || ends_with(dark, "selviana-dark.svg"));
  cfg.set("appearance", "wallpaper", pic);
  CHECK(wallpaper_in_use(cfg, true) == pic);

  // The tint: the same luminance (white may lose up to 4.5 %), still a gray
  // (at most 0.045 between its channels), leaning towards the wallpaper.
  for (const auto &fam : theme_families())
    for (bool dk : {false, true}) {
      Theme base = Theme::make(fam.id, dk, "blue");
      for (const auto &kv : m)
        for (const Color *c : {&base.surface, &base.surface_alt, &base.hover, &base.border, &base.menu_bg,
                               &base.panel_bg, &base.background}) {
          Color t2 = tint_surface(*c, kv.second, 0.022);
          double y0 = c->luminance(), y1 = t2.luminance();
          CHECK(y1 <= y0 * 1.003 + 1e-4 && y1 >= y0 * 0.95 - 1e-4);
          CHECK(chroma(t2) <= 0.046 || chroma(t2) <= chroma(*c) + 1e-9);
          Oklab a = to_oklab(*c), b = to_oklab(t2);
          double moved = std::hypot(b.a - a.a, b.b - a.b);
          CHECK(moved <= 0.022 * kv.second.strength + 1e-3);
          if (kv.second.strength == 0) CHECK(moved < 1e-9);
          if (moved > 0.004) CHECK(hue_gap(std::atan2(b.b - a.b, b.a - a.a), kv.second.hue) < 5);
        }
    }

  // Themes with the tint: the floating layer of every family, mode and
  // accent leans towards the wallpaper and keeps WCAG AA.
  std::string sky = write_sample(kSamples[0], g_tmp + "/sky.png");
  std::string forest = write_sample(kSamples[1], g_tmp + "/forest.png");
  std::string gray = write_sample(kSamples[5], g_tmp + "/gray.png");
  std::string red = write_sample(kSamples[3], g_tmp + "/red.png");
  for (const auto &fam : theme_families())
    for (const char *mode : {"light", "dark"})
      for (const auto &accent : theme_accents())
        for (const std::string &wp : {sky, red}) {
          Theme plain = Theme::from_config(tinted_config(fam.id, mode, accent, wp, "false", 100));
          Theme t = Theme::from_config(tinted_config(fam.id, mode, accent, wp, "true", 100));
          std::string what = fam.id + " " + mode + " " + accent + " " + wp;
          bool ok = true;
          auto need = [&](const char *name, bool cond) {
            if (!cond) fprintf(stderr, "  %s: %s\n", what.c_str(), name);
            ok = ok && cond;
          };
          need("tinted", t.wallpaper_tint && !plain.wallpaper_tint);
          need("layer", t.layer_surface.to_hex() != plain.surface.to_hex() &&
                            t.menu_bg.to_hex() != plain.menu_bg.to_hex());
          need("windows keep their colours", t.surface.to_hex() == plain.surface.to_hex() &&
                                                 t.background.to_hex() == plain.background.to_hex());
          need("gtk menus", t.gtk_menu_bg.to_hex() == plain.menu_bg.to_hex());
          for (const Color *c : {&t.layer_surface, &t.layer_surface_alt, &t.layer_hover, &t.layer_border, &t.menu_bg,
                                 &t.panel_bg, &t.menu_border})
            need("a tinted gray", chroma(*c) <= 0.05);
          need("text", t.text.contrast(t.layer_surface) >= 7 && t.menu_text.contrast(t.menu_bg) >= 7);
          need("dim text", t.text_dim.contrast(t.layer_surface_alt) >= 4.5 && t.text_dim.contrast(t.menu_bg) >= 4.5 &&
                               t.text_dim.contrast(t.panel_bg) >= 4.5 && t.text_dim.contrast(t.layer_hover) >= 4.5);
          need("menu highlight", t.menu_hover_text.contrast(t.menu_hover) >= 4.5);
          // The same contrast as without the tint (within 5 %).
          need("contrast kept", std::fabs(t.text.contrast(t.layer_surface) / plain.text.contrast(plain.surface) - 1) <
                                    0.05 &&
                                    std::fabs(t.text_dim.contrast(t.menu_bg) / plain.text_dim.contrast(plain.menu_bg) -
                                              1) < 0.05);
          // At the highest transparency over any backdrop (the faded families).
          if (!t.liquid) {
            need("panel", translucent_contrast(t.text, t.layer_surface, t.opacity, true) >= 4.5 &&
                              translucent_contrast(t.text_dim, t.layer_surface, t.opacity, true) >= 3.0);
            need("menus", translucent_contrast(t.menu_text, t.menu_bg, t.popup_opacity, true) >= 4.5 &&
                              translucent_contrast(t.text_dim, t.menu_bg, t.popup_opacity, true) >= 3.0);
          }
          CHECK(ok);
        }
  // auto: on in Fluent and Liquid Glass, off in Graphite; a gray wallpaper
  // has nothing to give.
  CHECK(!Theme::from_config(tinted_config("graphite", "light", "blue", sky, nullptr)).wallpaper_tint);
  CHECK(Theme::from_config(tinted_config("fluent", "light", "blue", sky, nullptr)).wallpaper_tint);
  CHECK(Theme::from_config(tinted_config("glass", "dark", "blue", sky, "auto")).wallpaper_tint);
  CHECK(Theme::from_config(tinted_config("graphite", "dark", "blue", sky, "true")).wallpaper_tint);
  CHECK(!Theme::from_config(tinted_config("fluent", "dark", "blue", sky, "false")).wallpaper_tint);
  CHECK(!Theme::from_config(tinted_config("fluent", "light", "blue", gray, "true")).wallpaper_tint);
  CHECK(theme_wallpaper_tint_default("fluent") && theme_wallpaper_tint_default("glass") &&
        !theme_wallpaper_tint_default("graphite"));
  // A new wallpaper is a change of the look when the look takes its colours
  // (announce_wallpaper_change() then asks the session for its calm switch),
  // and the program that sets it measures it first.
  CHECK(!wallpaper_colors_in_use(tinted_config("graphite", "light", "blue", sky, nullptr)));
  CHECK(wallpaper_colors_in_use(tinted_config("fluent", "light", "blue", sky, nullptr)));
  CHECK(!wallpaper_colors_in_use(tinted_config("glass", "light", "blue", sky, "false")));
  Config with_accent = tinted_config("graphite", "light", "blue", sky, nullptr);
  with_accent.set("appearance", "accent_from_wallpaper", "true");
  CHECK(wallpaper_colors_in_use(with_accent));
  std::string fresh = write_sample(kSamples[2], g_tmp + "/fresh.png"), entries;
  prepare_wallpaper_colors(tinted_config("graphite", "dark", "blue", fresh, nullptr));
  CHECK(read_file(g_tmp + "/cache/sde/wallpaper-colors", entries) && entries.find(fresh) == std::string::npos);
  prepare_wallpaper_colors(tinted_config("fluent", "dark", "blue", fresh, nullptr));
  CHECK(read_file(g_tmp + "/cache/sde/wallpaper-colors", entries) && entries.find(fresh) != std::string::npos);
  // use_floating_layer(): what the panel, notifications and OSDs draw.
  Theme layer = Theme::from_config(tinted_config("fluent", "light", "blue", forest, "true"));
  Color window_surface = layer.surface;
  layer.use_floating_layer();
  CHECK(layer.surface.to_hex() == layer.layer_surface.to_hex() && layer.surface.to_hex() != window_surface.to_hex());

  // The accent from the wallpaper: a named accent (GTK and the frames follow).
  Config acc = tinted_config("fluent", "light", "purple", forest, nullptr);
  acc.set("appearance", "accent_from_wallpaper", "true");
  CHECK(theme_choice(acc).accent == "green" && theme_dir(acc) == "SDE-Fluent-Light-Green");
  acc.set("appearance", "wallpaper", red);
  CHECK(theme_choice(acc).accent == "red");
  acc.set("appearance", "wallpaper", gray);
  CHECK(theme_choice(acc).accent == "graphite");
  acc.set("appearance", "accent_from_wallpaper", "false");
  CHECK(theme_choice(acc).accent == "purple");

  // The Liquid Glass shader keys the floating layer's tinted colours, and
  // GTK 3 menus (the theme's own menu colour) onto the same glass.
  Config glass = tinted_config("glass", "light", "blue", sky, "true", 80);
  glass.set("compositor", "backend", "glx");
  Theme gt = Theme::from_config(glass);
  CHECK(gt.liquid && gt.wallpaper_tint);
  write_compositor_config(glass);
  std::string glsl;
  CHECK(read_file(config_home() + "/sde/liquid-glass.glsl", glsl));
  auto key = [&](const char *name) {
    size_t at = glsl.find(std::string("const vec3 ") + name + " = vec3(");
    Color c;
    if (at == std::string::npos) return c;
    sscanf(glsl.c_str() + at + strlen("const vec3 ") + strlen(name) + strlen(" = vec3("), "%lf, %lf, %lf", &c.r, &c.g,
           &c.b);
    return c;
  };
  auto same = [](const Color &a, const Color &b) {
    return std::fabs(a.r - b.r) < 1e-3 && std::fabs(a.g - b.g) < 1e-3 && std::fabs(a.b - b.b) < 1e-3;
  };
  CHECK(same(key("KEY_A"), gt.layer_surface) && same(key("KEY_B"), gt.menu_bg) && same(key("KEY_C"), gt.gtk_menu_bg));
  CHECK(!same(key("KEY_B"), key("KEY_C")));
  set_ui_scale(1.0);
}

int main() {
  char tmpl[] = "/tmp/sde-theme-test-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  // The wallpaper colours' cache (0.2.0 Alpha 4) of the test's own.
  setenv("XDG_CACHE_HOME", (g_tmp + "/cache").c_str(), 1);
  test_caption_layout();
  test_transparency_contrast();
  test_soft_palettes();
  test_liquid_glass();
  test_wallpaper_colors();
  test_caption_frames();
  test_icon_lookup();
  test_build_history();
  std::string cmd = "rm -rf '" + g_tmp + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("theme tests passed\n");
  return 0;
}
