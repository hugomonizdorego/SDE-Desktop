// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Unit tests of SDE's window management without a display (0.2.0 Alpha 4,
// docs/WINDOWS.md, docs/SHORTCUTS.md): the shortcut list and the SDE WM
// keybinds made from it (also from an older rc.xml and with the user's own
// keybinds), the workspaces in sde.conf and rc.xml and the plan of closing
// one, snapping with Win+arrows and Task View's layout.
//
//   sde-windows-tests SOURCE_DIR
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "screenshot_model.h"
#include "shortcuts.h"
#include "snap.h"
#include "taskview_model.h"
#include "util.h"
#include "workspaces.h"

using namespace sde;

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

static std::string g_source;

static int count(const std::string &text, const std::string &what) {
  int n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
  return n;
}

static void test_key_names() {
  using shortcuts::display_key;
  using shortcuts::normalize_key;
  CHECK(display_key("W-d") == "Win+D");
  CHECK(display_key("C-W-Left") == "Ctrl+Win+Left");
  CHECK(display_key("W-C-Left") == "Ctrl+Win+Left");
  CHECK(display_key("C-A-Delete") == "Ctrl+Alt+Delete");
  CHECK(display_key("A-S-Tab") == "Alt+Shift+Tab");
  CHECK(display_key("C-S-Escape") == "Ctrl+Shift+Esc");
  CHECK(display_key("Super_L") == "Win");
  CHECK(display_key("Print") == "PrtScn");
  CHECK(display_key("A-Print") == "Alt+PrtScn");
  CHECK(display_key("W-period") == "Win+.");
  CHECK(display_key("W-S-s") == "Win+Shift+S");
  CHECK(display_key("A-space") == "Alt+Space");
  CHECK(display_key("C-W-S-Right") == "Ctrl+Win+Shift+Right");
  CHECK(display_key("C-minus") == "Ctrl+Minus");
  CHECK(normalize_key("S-W-Up") == normalize_key("W-S-Up"));
  CHECK(normalize_key("W-D") == normalize_key("W-d"));
  CHECK(normalize_key("Mod4-d") == normalize_key("W-d"));
  CHECK(normalize_key("Control-Alt-t") == normalize_key("C-A-t"));
  CHECK(normalize_key("C-minus") != normalize_key("C-plus"));
  const auto *s = shortcuts::find("taskbar-app");
  CHECK(s && shortcuts::display_keys(*s) == "Win+1 … Win+9");
  s = shortcuts::find("start-menu");
  CHECK(s && shortcuts::display_keys(*s) == "Win or Ctrl+Esc or Alt+F1");
  s = shortcuts::find("workspace-left");
  CHECK(s && shortcuts::display_keys(*s) == "Ctrl+Win+Left or Ctrl+Alt+Left");
}

static void test_table() {
  std::set<std::string> ids, keys;
  std::set<std::string> groups;
  for (const auto &g : shortcuts::groups()) groups.insert(g.id);
  for (const auto &s : shortcuts::table()) {
    CHECK(ids.insert(s.id).second);  // unique ids
    CHECK(groups.count(s.group));
    CHECK(s.label && *s.label && s.action && *s.action);
    for (const auto &k : split(s.keys, ' ')) {
      std::string key = k;
      if (s.range) key.replace(key.find("%d"), 2, "1");
      // No key twice, system or application.
      bool fresh = keys.insert(shortcuts::normalize_key(key)).second;
      if (!fresh) fprintf(stderr, "key %s twice\n", key.c_str());
      CHECK(fresh);
    }
  }
  // The shortcuts the owner asked for are there.
  for (const char *id : {"show-desktop", "minimize-all", "restore-minimized", "minimize-others", "maximize",
                         "restore-minimize", "snap-left", "snap-right", "maximize-vertically", "switch-windows",
                         "switch-windows-back", "task-view", "close-window", "window-menu", "start-menu", "files",
                         "control-center", "lock", "run", "search", "action-center", "notifications",
                         "taskbar-app", "taskbar-focus", "tray", "display", "about", "system-monitor",
                         "quick-link-menu", "security", "emoji", "screenshot-snip", "screenshot", "workspace-new",
                         "workspace-close", "workspace-left", "workspace-right", "terminal"})
    CHECK(shortcuts::find(id) != nullptr);
  // Application shortcuts never become window manager keybinds: they would
  // take Ctrl+C from every program.
  Config cfg;
  for (const auto &b : shortcuts::bindings(cfg)) {
    const auto *s = shortcuts::find(b.id);
    CHECK(s && s->scope == shortcuts::Scope::System);
  }
}

static void test_bindings() {
  Config cfg;
  auto all = shortcuts::bindings(cfg);
  std::string xml;
  for (const auto &b : all) xml += b.xml + "\n";
  CHECK(count(xml, "<keybind key=\"W-d\">") == 1);
  CHECK(xml.find("<keybind key=\"Super_L\" onRelease=\"yes\">") != std::string::npos);
  CHECK(xml.find("<keybind key=\"W-9\"><action name=\"Execute\"><command>sde-panel --taskbar 9</command>") !=
        std::string::npos);
  CHECK(xml.find("<keybind key=\"A-space\"><action name=\"ShowMenu\"><menu>client-menu</menu>") != std::string::npos);
  CHECK(xml.find("<wrap>no</wrap>") != std::string::npos);
  CHECK(xml.find("&amp;&amp;") != std::string::npos);  // the shell command of Win+E, escaped
  CHECK(xml.find("{wrap}") == std::string::npos && xml.find("{all}") == std::string::npos);
  // [workspaces] wrap and alt_tab_all reach the actions.
  cfg.set("workspaces", "wrap", "true");
  cfg.set("workspaces", "alt_tab_all", "true");
  xml.clear();
  for (const auto &b : shortcuts::bindings(cfg)) xml += b.xml + "\n";
  CHECK(xml.find("<wrap>yes</wrap>") != std::string::npos && xml.find("<wrap>no</wrap>") == std::string::npos);
  CHECK(xml.find("<allDesktops>yes</allDesktops>") != std::string::npos);
  // [shortcuts] overrides: other keys, or none.
  cfg.set("shortcuts", "terminal", "W-Return C-A-t");
  cfg.set("shortcuts", "emoji", "none");
  xml.clear();
  for (const auto &b : shortcuts::bindings(cfg)) xml += b.xml + "\n";
  CHECK(xml.find("<keybind key=\"W-Return\"><action name=\"Execute\"><command>x-terminal-emulator") !=
        std::string::npos);
  CHECK(xml.find("sde-windows emoji") == std::string::npos);
  // Keys the user bound are left to the user.
  Config plain;
  auto fewer = shortcuts::bindings(plain, {shortcuts::normalize_key("W-e")});
  bool has_we = false;
  for (const auto &b : fewer) has_we = has_we || b.key == "W-e";
  CHECK(!has_we && fewer.size() + 1 == all.size());
}

static std::string read(const std::string &path) {
  std::string text;
  read_file(path, text);
  return text;
}

static void test_sync_keyboard() {
  Config cfg;
  // The template is what SDE writes for the defaults (sde-windows
  // openbox-config regenerates it).
  std::string tmpl = read(g_source + "/data/openbox/rc.xml");
  CHECK(!tmpl.empty());
  std::string again = tmpl;
  CHECK(!shortcuts::sync_openbox_keyboard(again, cfg));
  CHECK(again == tmpl);
  CHECK(!workspaces::sync_openbox_desktops(again, workspaces::Settings::from_config(cfg)));
  // An rc.xml of SDE 0.2.0 Alpha 3.1 with one keybind of the user's own and
  // one SDE keybind the user changed.
  std::string old =
      "<openbox_config>\n  <keyboard>\n    <chainQuitKey>C-g</chainQuitKey>\n"
      "    <!-- Application menu (Super key) -->\n"
      "    <keybind key=\"Super_L\" onRelease=\"yes\"><action name=\"Execute\"><command>sde-panel --menu</command>"
      "</action></keybind>\n"
      "    <keybind key=\"W-e\"><action name=\"Execute\"><command>xdg-open .</command></action></keybind>\n"
      "    <keybind key=\"Print\"><action name=\"Execute\"><command>flameshot gui</command></action></keybind>\n"
      "    <!-- Windows -->\n"
      "    <keybind key=\"W-Left\">\n      <action name=\"UnmaximizeFull\"/>\n"
      "      <action name=\"MoveResizeTo\"><x>0</x><y>0</y><width>50%</width><height>100%</height></action>\n"
      "    </keybind>\n"
      "    <keybind key=\"W-F12\"><keybind key=\"x\"><action name=\"Execute\"><command>xterm</command></action>"
      "</keybind></keybind>\n"
      "  </keyboard>\n  <mouse>\n  </mouse>\n</openbox_config>\n";
  std::string rc = old;
  CHECK(shortcuts::sync_openbox_keyboard(rc, cfg));
  CHECK(rc.find(shortcuts::kEndMark) != std::string::npos);
  CHECK(rc.find("xdg-open .") == std::string::npos);           // SDE's old Win+E went
  CHECK(rc.find("UnmaximizeFull") == std::string::npos);       // and its old Win+Left
  CHECK(rc.find("<!-- Windows -->") == std::string::npos);
  CHECK(count(rc, "flameshot gui") == 1);                       // the user's PrtScn stays
  CHECK(rc.find("<keybind key=\"Print\"><action name=\"Execute\"><command>sde-screenshot") == std::string::npos);
  CHECK(rc.find("<keybind key=\"W-Print\">") != std::string::npos);  // the other key of the same shortcut
  CHECK(count(rc, "<keybind key=\"W-F12\">") == 1);              // a chain of the user's stays whole
  CHECK(count(rc, "<keybind key=\"W-d\">") == 1);
  CHECK(count(rc, "<keybind key=\"Super_L\"") == 1);
  // Written again, nothing changes; the user's keybinds stay after the block.
  std::string twice = rc;
  CHECK(!shortcuts::sync_openbox_keyboard(twice, cfg));
  CHECK(twice == rc);
  CHECK(rc.find("flameshot") > rc.find(shortcuts::kEndMark));
  // A configuration without <keyboard> gets one.
  std::string bare = "<openbox_config>\n  <mouse>\n  </mouse>\n</openbox_config>\n";
  CHECK(shortcuts::sync_openbox_keyboard(bare, cfg));
  CHECK(bare.find("<keyboard>") < bare.find("<mouse>") && bare.find("W-Tab") != std::string::npos);
}

static void test_workspace_settings() {
  Config cfg;
  auto s = workspaces::Settings::from_config(cfg);
  CHECK(s.count == 2 && !s.wrap && !s.taskbar_all && s.names.size() == 5);
  CHECK(s.name(0) == "Workspace 1" && s.name(4) == "Workspace 5");
  cfg.set("workspaces", "count", "9");
  CHECK(workspaces::Settings::from_config(cfg).count == 5);
  cfg.set("workspaces", "count", "0");
  CHECK(workspaces::Settings::from_config(cfg).count == 1);
  s.count = 3;
  s.names[0] = "Mail";
  s.names[1] = "Workspace 2";  // the default name is not stored
  s.names[2] = "  Music  ";
  s.wrap = true;
  s.save(cfg);
  CHECK(cfg.get("workspaces", "count") == "3" && cfg.get("workspaces", "name1") == "Mail");
  CHECK(cfg.get("workspaces", "name2").empty() && cfg.get("workspaces", "name3") == "Music");
  CHECK(cfg.get_bool("workspaces", "wrap", false));
  auto back = workspaces::Settings::from_config(cfg);
  CHECK(back.shown_names() == (std::vector<std::string>{"Mail", "Workspace 2", "Music"}));
  // rc.xml's <desktops>.
  std::string rc = "<openbox_config>\n  <desktops>\n    <number>2</number>\n    <firstdesk>1</firstdesk>\n"
                   "    <names><name>Workspace 1</name><name>Workspace 2</name></names>\n  </desktops>\n"
                   "  <resize>\n  </resize>\n</openbox_config>\n";
  back.names[0] = "Mail & <News>";
  CHECK(workspaces::sync_openbox_desktops(rc, back));
  CHECK(rc.find("<number>3</number>") != std::string::npos);
  CHECK(rc.find("<names><name>Mail &amp; &lt;News&gt;</name><name>Workspace 2</name><name>Music</name></names>") !=
        std::string::npos);
  CHECK(!workspaces::sync_openbox_desktops(rc, back));
  std::string none = "<openbox_config>\n  <resize>\n  </resize>\n</openbox_config>\n";
  CHECK(workspaces::sync_openbox_desktops(none, back) && none.find("<number>3</number>") < none.find("<resize>"));
}

static void test_close_plan() {
  using workspaces::moved_desktop;
  // Closing workspace 2 (index 1) of 4: its windows to 1, 3 and 4 down.
  CHECK(moved_desktop(0, 1, 4) == 0);
  CHECK(moved_desktop(1, 1, 4) == 0);
  CHECK(moved_desktop(2, 1, 4) == 1);
  CHECK(moved_desktop(3, 1, 4) == 2);
  CHECK(moved_desktop(-1, 1, 4) == -1);  // on every workspace
  // The first one: its windows join the next one, which becomes the first.
  CHECK(moved_desktop(0, 0, 3) == 0 && moved_desktop(1, 0, 3) == 0 && moved_desktop(2, 0, 3) == 1);
  CHECK(workspaces::shown_after_close(1, 1, 4) == 0);
  CHECK(workspaces::shown_after_close(3, 1, 4) == 2);
  CHECK(workspaces::shown_after_close(0, 2, 4) == 0);
  CHECK(workspaces::shown_after_close(0, 0, 2) == 0);
  workspaces::Settings s;
  s.names = {"A", "B", "C", "", ""};
  auto n = workspaces::names_after_close(s, 1);
  CHECK(n == (std::vector<std::string>{"A", "C", "", "", ""}));
}

static void test_snap() {
  using namespace snap;
  CHECK(plan(Zone::Free, false, Key::Left).step == Step::SnapTo && plan(Zone::Free, false, Key::Left).zone == Zone::Left);
  CHECK(plan(Zone::Free, false, Key::Up).step == Step::Maximize);
  CHECK(plan(Zone::Free, false, Key::Down).step == Step::Minimize);
  CHECK(plan(Zone::Free, true, Key::Down).step == Step::Unmaximize);
  CHECK(plan(Zone::Free, true, Key::Right).zone == Zone::Right);
  CHECK(plan(Zone::Left, false, Key::Right).step == Step::Restore);
  CHECK(plan(Zone::Right, false, Key::Left).step == Step::Restore);
  CHECK(plan(Zone::Left, false, Key::Up).zone == Zone::TopLeft);
  CHECK(plan(Zone::TopLeft, false, Key::Up).step == Step::Maximize);
  CHECK(plan(Zone::TopLeft, false, Key::Down).zone == Zone::Left);
  CHECK(plan(Zone::Left, false, Key::Down).zone == Zone::BottomLeft);
  CHECK(plan(Zone::BottomLeft, false, Key::Down).step == Step::Minimize);
  CHECK(plan(Zone::TopLeft, false, Key::Right).zone == Zone::TopRight);
  CHECK(plan(Zone::BottomRight, false, Key::Up).zone == Zone::Right);
  Rect area{0, 0, 1366, 728};
  Rect l = zone_rect(Zone::Left, area), r = zone_rect(Zone::Right, area);
  CHECK(l.x == 0 && l.w == 683 && r.x == 683 && r.w == 683 && l.h == 728);
  Rect br = zone_rect(Zone::BottomRight, area);
  CHECK(br.x == 683 && br.y == 364 && br.h == 364);
  CHECK(fills({2, 1, 680, 720}, Zone::Left, area));       // a terminal's steps
  CHECK(!fills({200, 100, 600, 400}, Zone::Left, area));  // moved by hand
  CHECK(!fills(l, Zone::Free, area));
}

static void test_taskview_layout() {
  using namespace taskview;
  Layout empty = layout(1366, 768, 2, true, {}, 1.0);
  CHECK(empty.windows.empty() && empty.desks.size() == 2 && empty.add.w > 0);
  Layout five = layout(1366, 768, 5, false, {1.6, 1.6, 1.0, 0.7, 2.0}, 1.0);
  CHECK(five.desks.size() == 5 && five.add.w == 0 && five.windows.size() == 5);
  for (const auto &b : five.windows) {
    CHECK(b.x >= 0 && b.y >= 0 && b.x + b.w <= 1366 && b.w > 100 && b.h > 80);
    // Above the row of workspaces.
    CHECK(b.y + b.h <= five.desks[0].y);
  }
  for (size_t i = 0; i < five.windows.size(); ++i)
    for (size_t j = i + 1; j < five.windows.size(); ++j) {
      const Box &a = five.windows[i], &b = five.windows[j];
      CHECK(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
    }
  Layout one = layout(1366, 768, 1, true, {1.6}, 1.0);
  CHECK(one.windows.size() == 1 && one.windows[0].w <= 1366 * 2 / 5 + 1);
  Layout big = layout(2560, 1440, 3, true, {1.6, 1.6}, 2.0);
  CHECK(big.windows.size() == 2 && big.desks[0].w >= 300);
  Box f = fit({0, 0, 400, 400}, 2.0);
  CHECK(f.w == 400 && f.h == 200 && f.y == 100);
  CHECK(best_columns(4, {0, 0, 1200, 600}, 20, 30, {1.6, 1.6, 1.6, 1.6}) >= 2);
  CHECK(move_selection(0, 5, 3, 1, 0) == 1);
  CHECK(move_selection(4, 5, 3, 1, 0) == 0);   // round
  CHECK(move_selection(0, 5, 3, -1, 0) == 4);
  CHECK(move_selection(1, 5, 3, 0, 1) == 4);
  CHECK(move_selection(4, 5, 3, 0, 1) == 4);   // nothing below
  CHECK(move_selection(-1, 5, 3, 1, 0) == 0);
  CHECK(move_selection(0, 0, 3, 1, 0) == -1);
}

static std::set<std::string> g_files;
static bool fake_exists(const std::string &path) { return g_files.count(path) > 0; }

static void test_screenshot_names() {
  using namespace screenshot;
  CHECK(folder("/home/alex/Pictures/") == "/home/alex/Pictures/Screenshots");
  setenv("TZ", "UTC", 1);
  tzset();
  std::time_t when = 1793869812;  // 2026-11-05 09:10:12 UTC
  std::string name = file_name(when, fake_exists, "/p");
  CHECK(name == "Screenshot 2026-11-05 09-10-12.png");
  g_files.insert("/p/" + name);
  CHECK(file_name(when, fake_exists, "/p") == "Screenshot 2026-11-05 09-10-12 (2).png");
  g_files.insert("/p/Screenshot 2026-11-05 09-10-12 (2).png");
  CHECK(file_name(when, fake_exists, "/p") == "Screenshot 2026-11-05 09-10-12 (3).png");
  Rect r = from_corners(400, 300, 100, 120, {0, 0, 1280, 800});
  CHECK(r.x == 100 && r.y == 120 && r.w == 301 && r.h == 181);
  r = from_corners(-50, -20, 30, 40, {0, 0, 1280, 800});
  CHECK(r.x == 0 && r.y == 0 && r.w == 31 && r.h == 41);
  CHECK(intersect({0, 0, 10, 10}, {20, 20, 5, 5}).empty());
}

int main(int argc, char **argv) {
  g_source = argc > 1 ? argv[1] : ".";
  test_screenshot_names();
  // No configuration of this machine: only the built-in values.
  setenv("XDG_CONFIG_HOME", "/nonexistent-sde-windows-tests", 1);
  setenv("XDG_CONFIG_DIRS", "/nonexistent-sde-windows-tests", 1);
  setenv("LANGUAGE", "C", 1);
  test_key_names();
  test_table();
  test_bindings();
  test_sync_keyboard();
  test_workspace_settings();
  test_close_plan();
  test_snap();
  test_taskview_layout();
  if (g_failures) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("all window management tests passed\n");
  return 0;
}
