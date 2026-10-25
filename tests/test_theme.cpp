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
#include <string>

#include "build_history.h"
#include "config.h"
#include "icons.h"
#include "theme.h"
#include "util.h"
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
        CHECK(t.opacity < 0.9 && t.popup_opacity < 0.95 && t.gtk_menu_alpha < 0.95);
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
  // hicolor is last.
  std::vector<std::string> want = {"FixtureChild", "FixtureParent", "FixtureGrand", "FixtureOther", "hicolor"};
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
        // A clear glass, clearer with the slider; one tint for every surface.
        CHECK(t.opacity == t.popup_opacity && t.opacity >= 0.12 && t.opacity <= 0.5 && t.opacity < previous);
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

int main() {
  char tmpl[] = "/tmp/sde-theme-test-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  test_caption_layout();
  test_transparency_contrast();
  test_soft_palettes();
  test_liquid_glass();
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
