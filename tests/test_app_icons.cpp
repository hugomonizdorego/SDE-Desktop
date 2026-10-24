// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Application icons of 0.2 Alpha 2 (IconLoader::app_icon(), icons.h): the
// near matches when the icon theme lacks Icon= (file extension, -symbolic,
// case, reverse-DNS names, the desktop file ID, the program, the window
// class), SDE's own drawings before a generic Icon=, the category icons,
// Papirus at the end of the theme chain and the cache. Uses the fixture
// themes of tests/fixtures/icon-themes; no display is needed.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "desktop_entry.h"
#include "icons.h"
#include "util.h"

using namespace sde;

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

static DesktopEntry app(const std::string &id, const std::string &icon, const std::string &exec,
                        std::vector<std::string> categories = {}) {
  DesktopEntry e;
  e.id = id;
  e.path = "/usr/share/applications/" + id;
  e.type = "Application";
  e.name = id;
  e.icon = icon;
  e.exec = exec;
  e.categories = categories;
  return e;
}

static bool has(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

static size_t pos(const std::vector<std::string> &v, const std::string &s) {
  return size_t(std::find(v.begin(), v.end(), s) - v.begin());
}

int main() {
  char tmpl[] = "/tmp/sde-app-icons-test-XXXXXX";
  std::string tmp = mkdtemp(tmpl);
  std::string fixtures = std::string(SDE_TEST_SOURCE_DIR) + "/tests/fixtures/icon-themes";
  // A user icon folder that applications installed later fill.
  std::string late = tmp + "/data/icons/hicolor/48x48/apps";
  make_dirs(late);
  setenv("XDG_DATA_DIRS", fixtures.c_str(), 1);
  setenv("XDG_DATA_HOME", (tmp + "/data").c_str(), 1);
  setenv("HOME", tmp.c_str(), 1);
  IconLoader::set_theme("FixtureChild", false);
  auto in = [](const std::string &path, const std::string &theme) {
    return path.find("/icons/" + theme + "/") != std::string::npos;
  };

  // The theme chain ends with hicolor, then Papirus for what nothing else
  // has (the Papirus fixture has "folder").
  auto chain = IconLoader::theme_chain();
  CHECK(!chain.empty() && chain.back() == "Papirus" && pos(chain, "hicolor") < pos(chain, "Papirus"));
  CHECK(in(IconLoader::lookup("folder", 48), "Papirus"));

  // Icon= that the theme has.
  CHECK(IconLoader::app_icon(app("a.desktop", "child-app", "a"), 48) == "child-app");
  // Near forms of Icon=: without "-symbolic", in lower case, a file
  // extension, the last part of a reverse-DNS name.
  CHECK(IconLoader::app_icon(app("b.desktop", "fixture-tool-symbolic", "b"), 48) == "fixture-tool");
  CHECK(IconLoader::app_icon(app("c.desktop", "Fixture-Viewer", "c"), 48) == "fixture-viewer");
  CHECK(IconLoader::app_icon(app("d.desktop", "fixture-viewer.png", "d"), 48) == "fixture-viewer");
  CHECK(IconLoader::app_icon(app("e.desktop", "org.example.Fixture-Viewer", "e"), 48) == "fixture-viewer");
  // An absolute path that exists is the icon; one that is gone gives its name.
  std::string file = fixtures + "/icons/hicolor/48x48/apps/fixture-tool.svg";
  CHECK(IconLoader::app_icon(app("f.desktop", file, "f"), 48) == file);
  CHECK(IconLoader::app_icon(app("g.desktop", "/opt/gone/fixture-viewer.svg", "g"), 48) == "fixture-viewer");
  // The desktop file ID, the program and the window class.
  CHECK(IconLoader::app_icon(app("fixture-viewer.desktop", "gone", "x"), 48) == "fixture-viewer");
  CHECK(IconLoader::app_icon(app("org.example.Fixture-Viewer.desktop", "", "x"), 48) == "fixture-viewer");
  CHECK(IconLoader::app_icon(app("h.desktop", "gone", "env LANG=C /opt/bin/fixture-viewer %U"), 48) ==
        "fixture-viewer");
  CHECK(IconLoader::app_icon(app("i.desktop", "gone", "python3 /usr/share/fv/fixture-viewer.py %f"), 48) ==
        "fixture-viewer");
  CHECK(IconLoader::app_icon(app("j.desktop", "gone",
                                 "flatpak run --branch=stable --arch=x86_64 org.example.Fixture-Viewer"),
                             48) == "fixture-viewer");
  CHECK(IconLoader::app_icon(app("k.desktop", "gone", "pkexec sudo fixture-viewer"), 48) == "fixture-viewer");
  DesktopEntry wm = app("l.desktop", "gone", "l");
  wm.startup_wm_class = "Fixture-Viewer";
  CHECK(IconLoader::app_icon(wm, 48) == "fixture-viewer");

  // A generic Icon= gives way to an icon of the application itself (here
  // SDE's drawing, "sde-app-" + ID), and stays when there is none.
  CHECK(IconLoader::app_icon(app("fixture-net.desktop", "preferences-system-network", "fixture-net"), 48) ==
        "sde-app-fixture-net");
  CHECK(IconLoader::app_icon(app("other-net.desktop", "preferences-system-network", "other-net"), 48) ==
        "preferences-system-network");
  // Not generic: Icon= first even when an icon of the ID exists.
  CHECK(IconLoader::app_icon(app("fixture-viewer.desktop", "fixture-tool", "x"), 48) == "fixture-tool");

  // Nothing near: an icon of the categories (network settings first), or
  // nothing without them.
  CHECK(IconLoader::app_icon(app("m.desktop", "gone", "m", {"GTK", "Settings", "X-GNOME-NetworkSettings"}),
                             48) == "preferences-system-network");
  CHECK(IconLoader::app_icon(app("n.desktop", "gone", "n", {"Game", "ArcadeGame"}), 48) == "applications-games");
  CHECK(IconLoader::app_icon(app("n.desktop", "gone", "n", {"Game", "ArcadeGame"}), 48, false).empty());
  CHECK(IconLoader::app_icon(app("o.desktop", "gone", "o"), 48).empty());

  // The candidate lists.
  auto names = app_icon_names(app("org.gnome.Maps.desktop", "org.gnome.Maps", "gnome-maps --x"));
  CHECK(!names.empty() && names[0] == "org.gnome.Maps");
  CHECK(has(names, "Maps") && has(names, "maps") && has(names, "gnome-maps") &&
        has(names, "sde-app-org.gnome.Maps"));
  CHECK(pos(names, "maps") < pos(names, "gnome-maps"));
  names = app_icon_names(app("nm-connection-editor.desktop", "preferences-system-network",
                             "nm-connection-editor"));
  CHECK(!names.empty() && names[0] == "nm-connection-editor" &&
        pos(names, "sde-app-nm-connection-editor") < pos(names, "preferences-system-network"));
  // Search entries that are no applications have no desktop file ID.
  names = app_icon_names(app("sde-search:appearance", "preferences-desktop-theme", ""));
  CHECK(names.size() == 1 && names[0] == "preferences-desktop-theme");
  auto cats = category_icon_names(app("p.desktop", "", "p", {"System", "TerminalEmulator"}));
  CHECK(!cats.empty() && cats[0] == "utilities-terminal" && has(cats, "applications-system"));
  cats = category_icon_names(app("q.desktop", "", "q", {"Network", "WebBrowser"}));
  CHECK(!cats.empty() && cats[0] == "web-browser" && has(cats, "applications-internet"));
  CHECK(category_icon_names(app("r.desktop", "", "r")).back() == "applications-other");
  auto list = app_icon_list(app("s.desktop", file, "s", {"Game"}));
  CHECK(!list.empty() && list[0] == file && list.back() == "input-gaming");

  // Cached until applications are installed: then their icons count.
  DesktopEntry later = app("late-app.desktop", "late-app", "late-app", {"Game"});
  CHECK(IconLoader::app_icon(later, 48) == "applications-games");
  write_file_atomic(late + "/late-app.svg",
                    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\"><rect width=\"16\" "
                    "height=\"16\" fill=\"#E39A2B\"/></svg>\n");
  CHECK(IconLoader::app_icon(later, 48) == "applications-games");
  IconLoader::forget_missing();
  CHECK(IconLoader::app_icon(later, 48) == "late-app");
  CHECK(IconLoader::get({IconLoader::app_icon(later, 32)}, 32) != nullptr);

  std::string cmd = "rm -rf '" + tmp + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", tmp.c_str());
  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("application icon tests passed\n");
  return 0;
}
