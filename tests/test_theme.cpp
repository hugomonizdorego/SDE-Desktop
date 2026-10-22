// SPDX-License-Identifier: GPL-3.0-or-later
// Theme tests of 0.1.0~alpha50: right-hand caption buttons and the migration
// of saved SDE WM layouts, the transparency model (contrast at the highest
// transparency of every family, mode and accent), icon theme lookup with
// inheritance (tests/fixtures/icon-themes) and the build history parser.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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
        need("panel text", translucent_contrast(t.text, t.panel_bg, t.opacity, true), 4.5);
        need("panel dim", translucent_contrast(t.text_dim, t.panel_bg, t.opacity, true), 3.0);
        need("menu text", translucent_contrast(t.menu_text, t.menu_bg, t.popup_opacity, true), 4.5);
        need("menu dim", translucent_contrast(t.text_dim, t.menu_bg, t.popup_opacity, true), 3.0);
        need("menu hover", translucent_contrast(t.menu_hover_text, t.menu_hover, t.popup_opacity, true), 3.0);
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
