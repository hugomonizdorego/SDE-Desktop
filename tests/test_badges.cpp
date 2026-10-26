// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the live badges' rules (src/panel/badge_model.h, 0.2 Alpha
// 3): LauncherEntry properties, which application a notification or an
// entry belongs to, the count shown, the badge's colours, and the program of
// a D-Bus peer (dbus::peer_program). No display or bus is needed.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../src/panel/badge_model.h"
#include "dbus.h"
#include "util.h"

using namespace sde;

static int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

namespace {

dbus::Value value(int type) {
  dbus::Value v;
  v.type = type;
  return v;
}
dbus::Value text(const std::string &s) {
  dbus::Value v = value('s');
  v.s = s;
  return v;
}
dbus::Value number(int type, long long i) {
  dbus::Value v = value(type);
  v.i = i;
  return v;
}
dbus::Value flag(bool b) {
  dbus::Value v = value('b');
  v.b = b;
  return v;
}
dbus::Value real(double d) {
  dbus::Value v = value('d');
  v.d = d;
  return v;
}
// a{sv}
dbus::Value dict(const std::vector<std::pair<std::string, dbus::Value>> &entries) {
  dbus::Value a = value('a');
  a.element_signature = "{sv}";
  for (auto &kv : entries) {
    dbus::Value e = value('e'), var = value('v');
    var.items.push_back(kv.second);
    e.items = {text(kv.first), var};
    a.items.push_back(e);
  }
  return a;
}

DesktopEntry app(const std::string &id, const std::string &name, const std::string &exec,
                 const std::string &icon = "", const std::string &wm_class = "") {
  DesktopEntry e;
  e.id = id;
  e.type = "Application";
  e.name = name;
  e.exec = exec;
  e.icon = icon;
  e.startup_wm_class = wm_class;
  return e;
}

// A w x h ARGB32 picture (premultiplied) of |argb| with |alpha|.
std::vector<unsigned char> picture(int w, int h, uint32_t rgb, unsigned alpha = 255) {
  std::vector<unsigned char> px(size_t(w * h * 4));
  for (int i = 0; i < w * h; ++i) {
    uint32_t r = ((rgb >> 16) & 255) * alpha / 255, g = ((rgb >> 8) & 255) * alpha / 255, b = (rgb & 255) * alpha / 255;
    uint32_t p = (alpha << 24) | (r << 16) | (g << 8) | b;
    std::memcpy(&px[size_t(i) * 4], &p, 4);
  }
  return px;
}

}  // namespace

int main() {
  using badges::Badge;
  using badges::LauncherState;

  // LauncherEntry: Update signals carry what changed.
  {
    LauncherState s;
    badges::apply(s, dict({{"count", number('x', 7)}, {"count-visible", flag(true)}}));
    CHECK(s.count == 7 && s.count_visible && !s.urgent && !s.progress_visible);
    badges::apply(s, dict({{"progress", real(0.4)}, {"progress-visible", flag(true)}, {"urgent", flag(true)}}));
    CHECK(s.count == 7 && s.count_visible && s.urgent && s.progress_visible && s.progress > 0.39 && s.progress < 0.41);
    badges::apply(s, dict({{"count", number('i', 3)}, {"progress", real(7.0)}, {"quicklist", text("/x")}}));
    CHECK(s.count == 3 && s.progress == 1.0);
    // Wrong types are ignored.
    badges::apply(s, dict({{"count", text("9")}, {"urgent", number('u', 0)}}));
    CHECK(s.count == 3 && s.urgent);
    badges::apply(s, text("not a dict"));
    CHECK(s.count == 3);
  }
  CHECK(badges::app_id_from_uri("application://firefox.desktop") == "firefox.desktop");
  CHECK(badges::app_id_from_uri("application://org.telegram.desktop") == "org.telegram.desktop.desktop");
  CHECK(badges::app_id_from_uri("file:///x.desktop").empty() && badges::app_id_from_uri("application://").empty());
  CHECK(badges::app_id_from_uri("application://a/b.desktop").empty());

  // The program an Exec= line starts.
  CHECK(badges::exec_program("/usr/lib/thunderbird/thunderbird %u") == "thunderbird");
  CHECK(badges::exec_program("env GDK_BACKEND=x11 python3 /opt/foo/foo.py %U") == "foo");
  CHECK(badges::exec_program("/usr/bin/flatpak run --branch=stable --arch=x86_64 org.telegram.desktop -- %u") ==
        "flatpak:org.telegram.desktop");
  CHECK(badges::exec_program("").empty());

  // Which application: the desktop file ID, the program, the name, the icon.
  std::vector<DesktopEntry> apps = {
      app("firefox-esr.desktop", "Firefox ESR", "/usr/lib/firefox-esr/firefox-esr %u", "firefox-esr"),
      app("thunderbird.desktop", "Thunderbird", "/usr/bin/thunderbird %u", "thunderbird"),
      app("org.telegram.desktop.desktop", "Telegram", "telegram-desktop -- %u", "org.telegram.desktop"),
      app("chromium.desktop", "Chromium", "/usr/bin/chromium %U", "chromium"),
      app("chrome-whatsapp-Default.desktop", "WhatsApp", "/usr/bin/chromium --profile-directory=Default --app-id=wa",
          "chrome-wa-Default", "crx_wa"),
      app("slack.desktop", "Slack", "/usr/bin/slack %U", "slack"),
      app("com.discordapp.Discord.desktop", "Discord", "/usr/bin/flatpak run com.discordapp.Discord", "discord"),
      app("org.gnome.Evolution.desktop", "Evolution", "evolution %U", "org.gnome.Evolution"),
  };
  using Src = badges::Source;
  CHECK(badges::match_app(Src{"org.telegram.desktop", "", "Telegram Desktop", ""}, apps) ==
        "org.telegram.desktop.desktop");
  CHECK(badges::match_app(Src{"THUNDERBIRD", "", "", ""}, apps) == "thunderbird.desktop");
  CHECK(badges::match_app(Src{"", "thunderbird", "", ""}, apps) == "thunderbird.desktop");
  // A browser's notification: the browser, not its web app.
  CHECK(badges::match_app(Src{"", "chromium", "", ""}, apps) == "chromium.desktop");
  CHECK(badges::match_app(Src{"", "crx_wa", "", ""}, apps) == "chrome-whatsapp-Default.desktop");
  CHECK(badges::match_app(Src{"", "flatpak:com.discordapp.Discord", "", ""}, apps) == "com.discordapp.Discord.desktop");
  // By name: Name=, a program, one name beginning with the other.
  CHECK(badges::match_app(Src{"", "", "Slack", ""}, apps) == "slack.desktop");
  CHECK(badges::match_app(Src{"", "", "evolution", ""}, apps) == "org.gnome.Evolution.desktop");
  CHECK(badges::match_app(Src{"", "python3", "Telegram Desktop", ""}, apps) == "org.telegram.desktop.desktop");
  CHECK(badges::match_app(Src{"", "", "Firefox", ""}, apps) == "firefox-esr.desktop");
  // By icon, when only one application has it.
  CHECK(badges::match_app(Src{"", "", "", "org.gnome.Evolution"}, apps) == "org.gnome.Evolution.desktop");
  CHECK(badges::match_app(Src{"", "", "", "/tmp/picture.png"}, apps).empty());
  // Nothing fits: notify-send from a script.
  CHECK(badges::match_app(Src{"no-such-app", "notify-send", "notify-send", "dialog-information"}, apps).empty());

  // The count: the application's own LauncherEntry count when shown, else
  // its unread notifications.
  LauncherState own;
  own.count = 12;
  own.count_visible = true;
  CHECK(badges::combine(&own, 3).count == 12);
  own.count_visible = false;
  own.urgent = true;
  Badge b = badges::combine(&own, 3);
  CHECK(b.count == 3 && b.urgent && b.progress < 0 && b.shown());
  own.progress = 0.25;
  own.progress_visible = true;
  CHECK(badges::combine(&own, 0).progress == 0.25);
  CHECK(!badges::combine(nullptr, 0).shown() && badges::combine(nullptr, 2).count == 2);
  CHECK(badges::count_text(7) == "7" && badges::count_text(99) == "99" && badges::count_text(100) == "99+");

  // Colours: the icon's own hue, readable; grey icons take the fallback.
  {
    Color light = Color::hex(0xF3F3F3), dark = Color::hex(0x202020), accent = Color::hex(0x2672BE);
    auto blue = picture(16, 16, 0x0A84FF);
    auto c = badges::colors_from_pixels(blue.data(), 16, 16, 64, light, accent);
    CHECK(c.fill.b > c.fill.r && c.fill.b > c.fill.g);  // still blue, with white text
    CHECK(c.fill.contrast(c.text) >= 4.5 && c.text.luminance() > 0.9);
    // Yellow: dark text on it.
    auto yellow = picture(16, 16, 0xF5C211);
    c = badges::colors_from_pixels(yellow.data(), 16, 16, 64, light, accent);
    CHECK(c.text.luminance() < 0.1 && c.fill.contrast(c.text) >= 4.5 && c.fill.r > c.fill.b);
    // A grey icon: the accent; a transparent one too.
    auto grey = picture(16, 16, 0x808080);
    c = badges::colors_from_pixels(grey.data(), 16, 16, 64, light, accent);
    CHECK(std::abs(c.fill.b - accent.b) < 0.2 && c.fill.contrast(c.text) >= 4.5);
    auto clear = picture(16, 16, 0xFF0000, 0);
    c = badges::colors_from_pixels(clear.data(), 16, 16, 64, light, accent);
    CHECK(c.fill.b > c.fill.r);
    // Mostly grey with a red mark: the red.
    auto mixed = picture(16, 16, 0x777777);
    auto red = picture(16, 4, 0xE01B24);
    std::copy(red.begin(), red.end(), mixed.begin());
    c = badges::colors_from_pixels(mixed.data(), 16, 16, 64, light, accent);
    CHECK(c.fill.r > c.fill.g && c.fill.r > c.fill.b);
    // Apart from the surface: almost the colour of a dark surface gets
    // lighter, of a light surface darker.
    c = badges::readable(Color::hex(0x252525), dark);
    CHECK(c.fill.contrast(dark) >= 1.25 && c.fill.contrast(c.text) >= 4.5);
    c = badges::readable(Color::hex(0xEEEEEE), light);
    CHECK(c.fill.contrast(light) >= 1.25 && c.fill.contrast(c.text) >= 4.5);
  }

  // The program of a D-Bus peer, from a fake /proc.
  {
    char tmpl[] = "/tmp/sde-badges-XXXXXX";
    std::string proc = mkdtemp(tmpl);
    make_dirs(proc + "/10/root");
    make_dirs(proc + "/11/root");
    make_dirs(proc + "/12/root");
    // A program, a script run by an interpreter, a Flatpak.
    if (symlink("/usr/lib/thunderbird/thunderbird (deleted)", (proc + "/10/exe").c_str()) != 0) ++failures;
    std::ofstream(proc + "/10/cmdline") << std::string("thunderbird\0", 12);
    if (symlink("/usr/bin/python3.13", (proc + "/11/exe").c_str()) != 0) ++failures;
    std::ofstream(proc + "/11/cmdline") << std::string("python3\0-u\0/usr/bin/qa-mailer.py\0", 33);
    std::ofstream(proc + "/12/root/.flatpak-info") << "[Application]\nname=org.telegram.desktop\nruntime=x\n";
    CHECK(dbus::peer_program(10, proc) == "thunderbird");
    CHECK(dbus::peer_program(11, proc) == "qa-mailer");
    CHECK(dbus::peer_program(12, proc) == "flatpak:org.telegram.desktop");
    CHECK(dbus::peer_program(13, proc).empty() && dbus::peer_program(0, proc).empty());
    std::string cmd = "rm -rf '" + proc + "'";
    if (std::system(cmd.c_str()) != 0) ++failures;
  }

  if (failures) {
    std::fprintf(stderr, "%d badge check(s) failed\n", failures);
    return 1;
  }
  std::printf("badge tests passed\n");
  return 0;
}
