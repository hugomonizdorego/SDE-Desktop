// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for SDE helpers that do not need an X display.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

#include "config.h"
#include "integration.h"
#include "dbus.h"
#include "desktop_entry.h"
#include "rust_layout.h"
#include "theme.h"
#include "util.h"

using namespace sde;

static int g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

static double contrast(const Color &a, const Color &b) {
  double la = a.luminance(), lb = b.luminance();
  if (la < lb) std::swap(la, lb);
  return (la + 0.05) / (lb + 0.05);
}

static void test_split_command() {
  auto v = split_command("foo \"bar baz\" 'qux quux' a\\ b");
  CHECK(v.size() == 4);
  if (v.size() == 4) {
    CHECK(v[0] == "foo");
    CHECK(v[1] == "bar baz");
    CHECK(v[2] == "qux quux");
    CHECK(v[3] == "a b");
  }
  CHECK(split_command("").empty());
}

// Exercise Rust ownership and the real C ABI used by every C++ component.
static void test_rust_config() {
  // Check the geometry structure's C ABI as used by the native panel.
  auto geometry = sde_panel_geometry(-1280, 100, 1280, 720, 40, true, 1, 90, 8, 600, false);
  CHECK(geometry.x == -1216 && geometry.y == 108);
  CHECK(geometry.width == 1152 && geometry.height == 40 && geometry.reserve == 48);
  char temp[] = "/tmp/sde-config-ffi-XXXXXX";
  char *root = mkdtemp(temp);
  CHECK(root != nullptr);
  if (!root) return;
  std::string saved_home = env("XDG_CONFIG_HOME");
  std::string saved_dirs = env("XDG_CONFIG_DIRS");
  setenv("XDG_CONFIG_HOME", root, 1);
  std::string system = std::string(root) + "/system";
  setenv("XDG_CONFIG_DIRS", system.c_str(), 1);
  Config cfg;
  cfg.load();
  CHECK(cfg.get("appearance", "theme") == "light");
  cfg.set("appearance", "font", "Noto Sans Tetun");
  cfg.set("panel", "height", "999999999999999999999");
  CHECK(cfg.get_int("panel", "height", 40) == 40);
  Config copy = cfg;
  copy.set("appearance", "font", "Other");
  CHECK(cfg.get("appearance", "font") == "Noto Sans Tetun");
  copy = cfg;
  CHECK(copy.get("appearance", "font") == "Noto Sans Tetun");
  CHECK(cfg.save());
  copy.load();
  CHECK(copy.get("appearance", "font") == "Noto Sans Tetun");
  copy.unset("appearance", "font");
  CHECK(copy.get("appearance", "font") == "Sans");
  unlink(Config::user_path().c_str());
  rmdir((std::string(root) + "/sde").c_str());
  rmdir(root);
  if (saved_home.empty()) unsetenv("XDG_CONFIG_HOME");
  else setenv("XDG_CONFIG_HOME", saved_home.c_str(), 1);
  if (saved_dirs.empty()) unsetenv("XDG_CONFIG_DIRS");
  else setenv("XDG_CONFIG_DIRS", saved_dirs.c_str(), 1);
}

static void test_desktop_entry() {
  std::string dir = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/sde-test-de";
  make_dirs(dir);
  std::string file = dir + "/test.desktop";
  write_file_atomic(file,
                    "[Desktop Entry]\nType=Application\nName=Test App\nExec=testapp --open %U\n"
                    "Icon=test\nCategories=Utility;\nStartupWMClass=TestApp\n");
  DesktopEntry e;
  CHECK(e.load(file));
  CHECK(e.name == "Test App");
  CHECK(e.startup_wm_class == "TestApp");
  CHECK(e.menu_category() == "Accessories");
  auto cmd = e.command({"/tmp/a b.txt"});
  CHECK(cmd.size() == 3 && cmd[2] == "/tmp/a b.txt");
  auto none = e.command();
  CHECK(none.size() == 2);
}

static void test_application_categories_refresh() {
  char dir[] = "/tmp/sde-xdg-apps-XXXXXX";
  CHECK(mkdtemp(dir));
  std::string old_data = env("XDG_DATA_HOME"), old_dirs = env("XDG_DATA_DIRS");
  setenv("XDG_DATA_HOME", dir, 1);
  setenv("XDG_DATA_DIRS", dir, 1);
  std::string apps = std::string(dir) + "/applications";
  make_dirs(apps);
  CHECK(load_applications("SDE").empty());
  for (const auto &v :
       std::vector<std::pair<std::string, std::string>>{{"Utility;Calculator;", "Accessories"},
                                                        {"Utility;TextEditor;", "Accessories"},
                                                        {"Utility;Archiving;", "Accessories"},
                                                        {"Office;Viewer;", "Office"},
                                                        {"Graphics;Viewer;", "Graphics"},
                                                        {"Settings;HardwareSettings;", "Settings"},
                                                        {"System;Monitor;", "System"},
                                                        {"Network;WebBrowser;", "Internet"},
                                                        {"AudioVideo;Player;", "Multimedia"},
                                                        {"Development;IDE;", "Development"}}) {
    CHECK(write_file_atomic(
        apps + "/probe.desktop",
        "[Desktop Entry]\nType=Application\nName=Installed probe\nExec=true\nCategories=" +
            v.first + "\n"));
    auto loaded = load_applications("SDE");
    CHECK(loaded.size() == 1);
    if (!loaded.empty()) CHECK(loaded[0].menu_category() == v.second);
  }
  unlink((apps + "/probe.desktop").c_str());
  CHECK(load_applications("SDE").empty());
  if (old_data.empty())
    unsetenv("XDG_DATA_HOME");
  else
    setenv("XDG_DATA_HOME", old_data.c_str(), 1);
  if (old_dirs.empty())
    unsetenv("XDG_DATA_DIRS");
  else
    setenv("XDG_DATA_DIRS", old_dirs.c_str(), 1);
}

// WCAG 2.1: normal text needs 4.5:1, large/bold UI text and icons 3:1.
static void test_theme_contrast() {
  for (const auto &fam : theme_families())
    for (const char *mode : {"light", "dark"})
      for (const auto &accent : theme_accents()) {
        Config cfg;
        cfg.set("appearance", "style", fam.id);
        cfg.set("appearance", "theme", mode);
        cfg.set("appearance", "accent_name", accent);
        cfg.set("appearance", "scale", "100");
        Theme t = Theme::from_config(cfg);
        std::string what = fam.id + " " + mode + " " + accent;
        CHECK(t.family == fam.id && t.accent_name == accent);
        CHECK(contrast(t.text, t.background) >= 7.0);
        CHECK(contrast(t.text, t.surface) >= 7.0);
        CHECK(contrast(t.text, t.menu_bg) >= 7.0);
        CHECK(contrast(t.text_dim, t.surface) >= 4.5);
        CHECK(contrast(t.text_dim, t.background) >= 4.5);
        CHECK(contrast(t.menu_hover_text, t.menu_hover) >= 4.5);
        if (contrast(t.on_accent, t.accent) < 4.5)
          fprintf(stderr, "  note: on_accent on %s = %.2f\n", what.c_str(), contrast(t.on_accent, t.accent));
        CHECK(contrast(t.on_accent, t.accent) >= 3.0);
        // Selected text drawn in the accent color on the surface.
        double c = contrast(t.accent_text, t.surface);
        CHECK(c >= 4.5);
        if (c < 4.5) fprintf(stderr, "  accent text %s = %.2f\n", what.c_str(), c);
        CHECK(t.gtk_theme == theme_dir(fam.id, std::string(mode) == "dark", accent));
      }
}

// Saved [appearance] values of every release map to the new families.
static void test_theme_migration() {
  struct Case {
    const char *style, *theme, *accent, *name, *family, *mode, *want_accent;
  } cases[] = {
      {"sde", "light", "#2672BE", "", "graphite", "light", "blue"},
      {"sde", "dark", "#7A4FD1", "", "graphite", "dark", "purple"},
      {"aero", "light", "#167D9A", "", "fluent", "light", "teal"},
      {"aero", "dark", "#C2407A", "", "fluent", "dark", "pink"},
      {"liquid-glass", "light", "#D9682B", "", "glass", "light", "orange"},
      {"liquid-glass", "dark", "#277A4C", "", "glass", "dark", "green"},
      {"", "light", "#5A6472", "", "graphite", "light", "graphite"},
      {"Aero", "DARK", "#2F6FDB", "", "fluent", "dark", "blue"},
      {"bogus", "sepia", "", "", "graphite", "light", "blue"},
      {"graphite", "auto", "#3684DD", "", "graphite", "auto", "blue"},
      {"fluent", "dark", "#3684DD", "red", "fluent", "dark", "red"},
      {"glass", "light", "#CD0245", "", "glass", "light", "pink"},
      {"fluent", "light", "#FF0000", "", "fluent", "light", "red"},  // own color: nearest
  };
  for (const auto &c : cases) {
    ThemeChoice t = map_theme_choice(c.style, c.theme, c.accent, c.name);
    CHECK(t.family == c.family);
    CHECK(t.mode == c.mode);
    CHECK(t.accent == c.want_accent);
    if (t.family != c.family || t.mode != c.mode || t.accent != c.want_accent)
      fprintf(stderr, "  %s/%s/%s -> %s/%s/%s\n", c.style, c.theme, c.accent, t.family.c_str(),
              t.mode.c_str(), t.accent.c_str());
  }
  CHECK(map_theme_choice("fluent", "light", "#FF0000", "").custom_accent == "#FF0000");
  CHECK(map_theme_choice("fluent", "light", "#2777FF", "").custom_accent.empty());
  // A custom color stays SDE's accent; GTK gets the nearest named one.
  {
    Config cfg;
    cfg.set("appearance", "accent", "#FF0000");
    cfg.set("appearance", "scale", "100");
    Theme t = Theme::from_config(cfg);
    CHECK(t.accent.to_hex() == "#FF0000");
    CHECK(t.gtk_theme == "SDE-Graphite-Light-Red");
  }
  // migrate_theme_config() rewrites only legacy values.
  Config cfg;
  cfg.set("appearance", "style", "aero");
  cfg.set("appearance", "theme", "dark");
  cfg.set("appearance", "accent", "#7A4FD1");
  CHECK(migrate_theme_config(cfg));
  CHECK(cfg.get("appearance", "style") == "fluent");
  CHECK(cfg.get("appearance", "theme") == "dark");
  CHECK(cfg.get("appearance", "accent_name") == "purple");
  CHECK(cfg.get("appearance", "accent") == "#962AC3");
  CHECK(theme_dir(cfg) == "SDE-Fluent-Dark-Purple");
  CHECK(!migrate_theme_config(cfg));
  cfg.set("appearance", "style", "liquid-glass");
  cfg.set("appearance", "theme", "dim");
  CHECK(migrate_theme_config(cfg));
  CHECK(cfg.get("appearance", "style") == "glass");
  CHECK(cfg.get("appearance", "theme") == "light");
  // Directory names, the old one-argument form and the auto clock.
  CHECK(theme_dir("graphite") == "SDE-Graphite-Light-Blue");
  CHECK(theme_dir("aero", true) == "SDE-Fluent-Dark-Blue");
  CHECK(theme_dir("glass", false, "graphite") == "SDE-Glass-Light-Graphite");
  CHECK(theme_title_layout("graphite") == "CIML");
  CHECK(theme_title_layout("fluent") == "NLIMC");
  std::tm noon{};
  noon.tm_year = 126, noon.tm_mon = 0, noon.tm_mday = 10, noon.tm_hour = 12, noon.tm_isdst = -1;
  std::time_t t12 = mktime(&noon);
  CHECK(!theme_mode_dark("auto", t12));
  CHECK(theme_mode_dark("auto", t12 + 8 * 3600));
  CHECK(theme_auto_seconds_left(t12) == 7 * 3600);
  CHECK(theme_mode_dark("dark", t12) && !theme_mode_dark("light", t12 + 8 * 3600));
}

static void test_scale() {
  Config cfg;
  cfg.set("appearance", "scale", "150");
  Theme t = Theme::from_config(cfg);
  CHECK(t.scale == 1.5);
  CHECK(S(40) == 60);
  CHECK(t.panel_height == 60);
  cfg.set("appearance", "scale", "400");
  Theme::from_config(cfg);
  CHECK(ui_scale() == 2.0);
  cfg.set("appearance", "scale", "100");
  Theme::from_config(cfg);
  CHECK(S(40) == 40);
}

static void test_dbus_value() {
  dbus::Value dict;
  dict.type = DBUS_TYPE_ARRAY;
  dbus::Value entry;
  entry.type = DBUS_TYPE_DICT_ENTRY;
  dbus::Value key;
  key.type = 's';
  key.s = "urgency";
  dbus::Value var;
  var.type = DBUS_TYPE_VARIANT;
  dbus::Value byte;
  byte.type = 'y';
  byte.i = 2;
  var.items.push_back(byte);
  entry.items = {key, var};
  dict.items.push_back(entry);
  const dbus::Value *u = dict.get("urgency");
  CHECK(u && u->num() == 2);
  CHECK(dict.get("missing") == nullptr);
}

static void test_gtk_preferences() {
  char temp[] = "/tmp/sde-gtk-config-XXXXXX";
  char *root = mkdtemp(temp);
  CHECK(root != nullptr);
  if (!root) return;
  std::string saved = env("XDG_CONFIG_HOME");
  setenv("XDG_CONFIG_HOME", root, 1);
  setenv("GSETTINGS_BACKEND", "memory", 1);
  make_dirs(std::string(root) + "/gtk-3.0");
  CHECK(write_file_atomic(std::string(root) + "/gtk-3.0/settings.ini",
        "[Settings]\ngtk-double-click-time=450\n[Custom]\nkeep=yes\n"));
  Config cfg;
  cfg.set("appearance", "theme", "dark");
  cfg.set("appearance", "scale", "125");
  sync_gtk_settings(cfg);
  std::string text;
  CHECK(read_file(std::string(root) + "/gtk-3.0/settings.ini", text));
  CHECK(text.find("gtk-double-click-time=450") != std::string::npos);
  CHECK(text.find("keep=yes") != std::string::npos);
  CHECK(text.find("gtk-application-prefer-dark-theme=true") != std::string::npos);
  // SDE Graphite (the default) when it is installed; otherwise GTK's Adwaita.
  std::string name = gtk_theme_name(cfg);
  bool sde = name == "SDE-Graphite-Dark-Blue";
  CHECK(sde || name == "Adwaita-dark");
  CHECK(read_file(std::string(root) + "/gtk-4.0/settings.ini", text));
  CHECK(text.find("gtk-theme-name=" + name) != std::string::npos);
  CHECK(text.find("gtk-decoration-layout=close,minimize,maximize:menu") != std::string::npos);
  CHECK(read_file(std::string(root) + "/sde/xsettingsd.conf", text));
  CHECK(text.find("Net/ThemeName \"" + name + "\"") != std::string::npos);
  CHECK(text.find("Gtk/DecorationLayout") != std::string::npos);
  CHECK(text.find("Xft/DPI 122880") != std::string::npos);

  // Only SDE's block of the user's CSS changes; older blocks are replaced.
  CHECK(write_file_atomic(std::string(root) + "/gtk-3.0/gtk.css",
                          "/* SDE Aero: managed by SDE Settings - begin */\n@define-color "
                          "sde_accent #000000;\n/* SDE Aero: managed by SDE Settings - end */\n"
                          ".mine { color: red; }\n"));
  cfg.set("appearance", "style", "aero");  // alpha15 value
  cfg.set("appearance", "accent", "#167D9A");
  CHECK(theme_style(cfg) == "fluent");
  name = gtk_theme_name(cfg);
  CHECK(name == "SDE-Fluent-Dark-Teal" || name == "Adwaita-dark");
  sync_gtk_settings(cfg);
  CHECK(read_file(std::string(root) + "/gtk-3.0/gtk.css", text));
  CHECK(text.find(".mine { color: red; }") != std::string::npos);
  CHECK(text.find("SDE Aero: managed") == std::string::npos);
  CHECK(read_file(std::string(root) + "/gtk-3.0/settings.ini", text));
  CHECK(text.find("gtk-theme-name=" + name) != std::string::npos);
  CHECK(text.find("gtk-decoration-layout=menu:minimize,maximize,close") != std::string::npos);
  cfg.set("appearance", "style", "liquid-glass");
  CHECK(theme_style(cfg) == "glass");
  CHECK(theme_dir(cfg) == "SDE-Glass-Dark-Teal");
  cfg.unset("appearance", "style");
  cfg.unset("appearance", "accent");

  // The generated compositor configuration follows the family and [aero].
  cfg.set("aero", "enabled", "true");
  cfg.set("aero", "transparency", "30");
  cfg.set("compositor", "backend", "xrender");
  cfg.set("compositor", "effects", "full");
  cfg.set("appearance", "style", "glass");
  std::string conf = write_compositor_config(cfg);
  CHECK(read_file(conf, text));
  CHECK(text.find("backend = \"xrender\";") != std::string::npos);
  CHECK(text.find("frame-opacity = 0.64;") != std::string::npos);
  CHECK(text.find("blur-background = true;") != std::string::npos);
  CHECK(text.find("corner-radius = 16;") != std::string::npos);
  // Fluent: opaque frames, Acrylic (translucent, blurred) menus, 8 px corners.
  cfg.set("appearance", "style", "fluent");
  CHECK(read_file(write_compositor_config(cfg), text));
  CHECK(text.find("frame-opacity = 1.00;") != std::string::npos);
  CHECK(text.find("blur-background = true;") != std::string::npos);
  CHECK(text.find("corner-radius = 8;") != std::string::npos);
  CHECK(text.find("popup_menu = { opacity = 1.00") == std::string::npos);
  cfg.set("aero", "enabled", "false");
  CHECK(read_file(write_compositor_config(cfg), text));
  CHECK(text.find("frame-opacity = 1.00;") != std::string::npos);
  CHECK(text.find("blur-background") == std::string::npos);
  CHECK(text.find("popup_menu = { opacity = 1.00") != std::string::npos);
  if (saved.empty()) unsetenv("XDG_CONFIG_HOME"); else setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
}

int main() {
  test_rust_config();
  test_split_command();
  test_application_categories_refresh();
  test_desktop_entry();
  test_theme_contrast();
  test_theme_migration();
  test_scale();
  test_dbus_value();
  test_gtk_preferences();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
