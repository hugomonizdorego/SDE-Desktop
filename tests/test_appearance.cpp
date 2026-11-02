// SPDX-License-Identifier: GPL-3.0-or-later
// Control Center > Appearance (0.2.0 Alpha 3.1), without a display:
//  * the look in use and the chosen one (src/settings/appearance_draft.h):
//    reading and writing sde.conf, the text the preview window gets, what
//    differs, Apply, Revert and following a change made elsewhere;
//  * the files a theme switch writes before anyone is told
//    (write_appearance_files(), src/common/integration.h): unchanged
//    settings change nothing, an accent change does not touch the
//    compositor, a family change does (reloaded in place, never restarted),
//    the GTK block names the theme's colors so running GTK 3 applications
//    follow a switch, and the session's picture of the screen is excluded
//    from fading.
// Usage: sde-appearance-tests
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "appearance_draft.h"
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

#define CHECK_EQ(a, b)                                                                        \
  do {                                                                                        \
    auto va = (a);                                                                            \
    auto vb = (b);                                                                            \
    if (!(va == vb)) {                                                                        \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got \"%s\"\n", __FILE__, __LINE__, \
              #a, #b, std::string(va).c_str());                                               \
      ++g_failures;                                                                           \
    }                                                                                         \
  } while (0)

static bool contains(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}

static std::string read_all(const std::string &path) {
  std::string text;
  read_file(path, text);
  return text;
}

// A configuration with only |user| as the user's file.
static Config config_with(const std::string &user) {
  write_file_atomic(config_home() + "/sde/sde.conf", user);
  Config c;
  c.load();
  return c;
}

static void test_look() {
  Config cfg = config_with("");
  AppearanceLook l = AppearanceLook::from_config(cfg);
  CHECK_EQ(l.family, "graphite");
  CHECK_EQ(l.mode, "light");
  CHECK_EQ(l.accent, "blue");
  CHECK(l.translucent);
  CHECK(l.transparency == 25);
  CHECK(l.blur && l.blur_strength == 6 && l.rounded_corners && l.animations);

  // Written into a configuration and read back.
  AppearanceLook m = l;
  m.family = "fluent";
  m.mode = "dark";
  m.accent = "teal";
  m.transparency = 50;
  m.blur = false;
  m.blur_strength = 9;
  m.rounded_corners = false;
  m.animations = false;
  Config c2 = cfg;
  m.write(c2);
  CHECK(AppearanceLook::from_config(c2) == m);
  CHECK_EQ(c2.get("appearance", "style"), "fluent");
  CHECK_EQ(c2.get("appearance", "accent_name"), "teal");
  CHECK_EQ(c2.get("aero", "transparency"), "50");
  CHECK(!AppearanceLook::from_config(cfg).operator==(m));  // |cfg| itself is untouched

  // The text the preview window gets, and back.
  AppearanceLook back;
  CHECK(AppearanceLook::decode(m.encode(), back));
  CHECK(back == m);
  CHECK(!AppearanceLook::decode("mode=dark;transparency=40", back));  // no family: refused
  AppearanceLook odd;
  CHECK(AppearanceLook::decode("family=glass;mode=sepia;accent=puce;transparency=250;strength=40;unknown=1", odd));
  CHECK_EQ(odd.family, "glass");
  CHECK_EQ(odd.mode, "light");
  CHECK_EQ(odd.accent, "blue");
  CHECK(odd.transparency == 100);
  CHECK(odd.blur_strength == 12);

  // A color of the user's own stays until a swatch is chosen.
  Config custom = config_with("[appearance]\naccent=#12A4B0\n");
  AppearanceLook c = AppearanceLook::from_config(custom);
  CHECK_EQ(c.custom_accent, "#12A4B0");
  Config c3 = custom;
  c.write(c3);
  CHECK_EQ(c3.get("appearance", "accent"), "#12A4B0");

  // What differs, in the order of the page.
  auto changes = look_changes(l, m);
  CHECK(changes.size() == 7);
  CHECK_EQ(changes.front(), "family");
  CHECK_EQ(changes.back(), "animations");
  CHECK(look_changes(l, l).empty());
}

static void test_draft() {
  Config cfg = config_with("[appearance]\nstyle=graphite\ntheme=light\naccent_name=blue\n");
  AppearanceDraft d;
  d.load(cfg);
  CHECK(!d.dirty());
  d.set_family("fluent");
  CHECK(d.dirty());
  CHECK(d.changes().size() == 1 && d.changes()[0] == "family");
  CHECK_EQ(d.applied().family, "graphite");  // nothing is applied by choosing
  d.set_family("no-such-family");
  CHECK_EQ(d.pending().family, "fluent");
  d.revert();
  CHECK(!d.dirty());
  CHECK_EQ(d.pending().family, "graphite");

  // Transparency: a preset turns translucent surfaces on at its level; the
  // slider does above 0; turning them off keeps the level.
  d.set_translucent(false);
  d.set_preset(80);
  CHECK(d.pending().translucent && d.pending().transparency == 80);
  d.set_translucent(false);
  d.set_transparency(0);
  CHECK(!d.pending().translucent);
  d.set_transparency(37);
  CHECK(d.pending().translucent && d.pending().transparency == 37);
  d.set_transparency(400);
  CHECK(d.pending().transparency == 100);
  d.set_blur_strength(0);
  CHECK(d.pending().blur_strength == 1);
  d.revert();

  // The previews get the chosen look; the configuration in use is not
  // written until Apply.
  d.set_mode("dark");
  d.set_accent("purple");
  Config preview = d.preview_config(cfg);
  CHECK_EQ(preview.get("appearance", "theme"), "dark");
  CHECK_EQ(preview.get("appearance", "accent_name"), "purple");
  CHECK_EQ(cfg.get("appearance", "theme"), "light");
  d.apply(cfg);
  CHECK(!d.dirty());
  CHECK_EQ(cfg.get("appearance", "theme"), "dark");
  CHECK_EQ(cfg.get("appearance", "accent_name"), "purple");
  CHECK_EQ(d.applied().mode, "dark");

  // A change made elsewhere (the panel's dark mode, the automatic mode):
  // the look in use follows it, a choice that was made stays, cards that
  // were not touched follow.
  Config now = config_with("[appearance]\nstyle=graphite\ntheme=dark\naccent_name=purple\n");
  AppearanceDraft e;
  e.load(now);
  e.set_family("glass");
  Config elsewhere = config_with("[appearance]\nstyle=graphite\ntheme=light\naccent_name=purple\n");
  e.rebase(elsewhere);
  CHECK_EQ(e.applied().mode, "light");
  CHECK_EQ(e.pending().mode, "light");     // untouched: follows
  CHECK_EQ(e.pending().family, "glass");   // chosen: stays
  CHECK(e.dirty());
  // Applied elsewhere exactly as chosen: nothing is pending any more.
  Config same = config_with("[appearance]\nstyle=glass\ntheme=light\naccent_name=purple\n");
  e.rebase(same);
  CHECK(!e.dirty());
}

static void test_files(const std::string &source_dir, const std::string &binary_dir) {
  // The SDE themes of this build (GTK when they were built), SDE WM's
  // configuration from the template, a compositor on a real GPU.
  bool gtk_themes = file_exists(binary_dir + "/themes/SDE-Graphite-Light-Teal/gtk-3.0/gtk.css");
  setenv("XDG_DATA_DIRS", (binary_dir + ":/usr/share").c_str(), 1);
  setenv("SDE_GRAPHICS", "hardware", 1);
  setenv("GSETTINGS_BACKEND", "memory", 1);
  make_dirs(config_home() + "/sde/openbox");
  write_file_atomic(config_home() + "/sde/openbox/rc.xml", read_all(source_dir + "/data/openbox/rc.xml"));

  Config cfg = config_with("[appearance]\nstyle=graphite\ntheme=light\naccent_name=blue\n[compositor]\nmode=auto\n");
  AppearanceFiles first = write_appearance_files(cfg);
  CHECK(first.gtk);         // the first time every file is new
  CHECK(first.compositor);
  CHECK(file_exists(config_home() + "/sde/aero-compositor.conf"));

  // The same settings again: nobody has anything to re-read.
  AppearanceFiles again = write_appearance_files(cfg);
  CHECK(!again.gtk && !again.wm && !again.compositor);
  CHECK(!again.any());

  // An accent: GTK and the window frames change (another theme
  // directory), the compositor's configuration does not, so the session
  // does not even reload it.
  AppearanceDraft d;
  d.load(cfg);
  d.set_accent("teal");
  d.apply(cfg);
  std::string compositor_before = compositor_inputs(cfg);
  AppearanceFiles accent = write_appearance_files(cfg);
  CHECK(!accent.compositor);
  CHECK(compositor_inputs(cfg) == compositor_before);
  if (gtk_themes) {
    CHECK(accent.gtk);
    CHECK(contains(read_all(config_home() + "/sde/xsettingsd.conf"), "SDE-Graphite-Light-Teal"));
  }
  CHECK(accent.wm);
  CHECK(contains(read_all(config_home() + "/sde/openbox/rc.xml"), "SDE-Graphite-Light-Teal"));

  // A family: corners and shadows differ, so the compositor reloads (in
  // place; nothing here restarts it).
  d.set_family("fluent");
  d.apply(cfg);
  AppearanceFiles family = write_appearance_files(cfg);
  CHECK(family.compositor);
  CHECK(family.wm);

  // Transparency only: the compositor (frame opacity), not XSETTINGS.
  d.set_transparency(80);
  d.apply(cfg);
  AppearanceFiles glass = write_appearance_files(cfg);
  CHECK(glass.compositor);
  CHECK(!glass.gtk);

  // The session's picture of the screen is not faded by the compositor.
  {
    std::string inputs = compositor_inputs(cfg);
    size_t at = inputs.find("fade-exclude = [");
    CHECK(at != std::string::npos &&
          inputs.substr(at, inputs.find('\n', at) - at).find("\"_SDE_CURTAIN@\"") != std::string::npos);
  }

  // The GTK block: the theme's own colors, so that running GTK 3
  // applications (which read it only at start) follow a switch.
  if (gtk_themes) {
    std::string css = gtk_user_css(cfg, 3);
    CHECK(!contains(css, "@define-color sde_accent"));  // the theme defines it
    // No color of its own: nothing a running GTK 3 application could keep
    // from the old look. Translucent GTK menus (when the theme asks for
    // them; since 0.2.0 Alpha 3.1 the compositor makes them translucent
    // instead) name the theme's menu color.
    CHECK(!contains(css, "rgba(") && !contains(css, "#"));
    if (Theme::from_config(cfg).gtk_menu_alpha < 0.999) CHECK(contains(css, "alpha(@sde_menu, "));
    Config custom = cfg;
    custom.unset("appearance", "accent_name");
    custom.set("appearance", "accent", "#12A4B0");
    CHECK(contains(gtk_user_css(custom, 3), "@define-color sde_accent #12A4B0"));
  }

  // XRender chosen by hand: its configuration keeps every effect (the
  // fallback for a failing OpenGL, lite, is the same file and must not
  // overwrite it).
  Config xrender = cfg;
  xrender.set("compositor", "backend", "xrender");
  write_appearance_files(xrender);
  CHECK(contains(read_all(config_home() + "/sde/aero-compositor-xrender.conf"), "effects: full"));
  CHECK(!write_appearance_files(xrender).compositor);

  // Without a compositor there is nothing to reload.
  Config off = cfg;
  off.set("compositor", "mode", "off");
  CHECK(compositor_inputs(off).empty());
}

int main() {
  char tmpl[] = "/tmp/sde-appearance-test-XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (!dir) return 2;
  std::string tmp = dir;
  setenv("HOME", tmp.c_str(), 1);
  setenv("XDG_CONFIG_HOME", (tmp + "/config").c_str(), 1);
  setenv("XDG_CONFIG_DIRS", (tmp + "/xdg").c_str(), 1);
  setenv("XDG_DATA_HOME", (tmp + "/data").c_str(), 1);
  setenv("XDG_RUNTIME_DIR", (tmp + "/run").c_str(), 1);
  unsetenv("DISPLAY");
  make_dirs(tmp + "/config/sde");
  test_look();
  test_draft();
  test_files(SDE_TEST_SOURCE_DIR, SDE_TEST_BINARY_DIR);
  std::string cmd = "rm -rf '" + tmp + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("sde-appearance-tests: all passed\n");
  return 0;
}
