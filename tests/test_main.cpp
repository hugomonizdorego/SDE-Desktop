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
  CHECK(cfg.get("appearance", "icon_theme") == "Papirus");
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
  for (const char *style : {"light", "dark"}) {
    for (int a = 0; a < kAccentChoiceCount; ++a) {
      Config cfg;
      cfg.set("appearance", "theme", style);
      cfg.set("appearance", "accent", kAccentChoices[a].hex);
      cfg.set("appearance", "scale", "100");
      Theme t = Theme::from_config(cfg);
      CHECK(contrast(t.text, t.background) >= 7.0);
      CHECK(contrast(t.text, t.surface) >= 7.0);
      CHECK(contrast(t.text_dim, t.surface) >= 4.5);
      CHECK(contrast(t.text_dim, t.background) >= 4.5);
      CHECK(contrast(t.on_accent, t.accent) >= 4.5);
      if (contrast(t.on_accent, t.accent) < 4.5)
        fprintf(stderr, "  on_accent on %s (%s) = %.2f\n", kAccentChoices[a].name, style,
                contrast(t.on_accent, t.accent));
      // Selected text drawn in the accent color on the surface.
      double c = contrast(t.accent_text, t.surface);
      CHECK(c >= 4.5);
      if (c < 4.5)
        fprintf(stderr, "  accent text %s on %s surface = %.2f\n", kAccentChoices[a].name, style, c);
    }
  }
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
  cfg.set("appearance", "icon_theme", "Papirus");
  sync_gtk_settings(cfg);
  std::string text;
  CHECK(read_file(std::string(root) + "/gtk-3.0/settings.ini", text));
  CHECK(text.find("gtk-double-click-time=450") != std::string::npos);
  CHECK(text.find("keep=yes") != std::string::npos);
  CHECK(text.find("gtk-application-prefer-dark-theme=true") != std::string::npos);
  CHECK(read_file(std::string(root) + "/gtk-4.0/settings.ini", text));
  CHECK(text.find("gtk-theme-name=Adwaita") != std::string::npos);
  CHECK(text.find("gtk-icon-theme-name=Papirus") != std::string::npos);
  CHECK(read_file(std::string(root) + "/sde/xsettingsd.conf", text));
  CHECK(text.find("Adwaita-dark") != std::string::npos);
  CHECK(text.find("Xft/DPI 122880") != std::string::npos);
  CHECK(text.find(R"(Net/IconThemeName "Papirus")") != std::string::npos);
  if (saved.empty()) unsetenv("XDG_CONFIG_HOME"); else setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
}

int main() {
  test_rust_config();
  test_split_command();
  test_application_categories_refresh();
  test_desktop_entry();
  test_theme_contrast();
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
