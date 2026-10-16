// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for SDE helpers that do not need an X display.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "config.h"
#include "dbus.h"
#include "desktop_entry.h"
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

int main() {
  test_split_command();
  test_desktop_entry();
  test_theme_contrast();
  test_scale();
  test_dbus_value();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
